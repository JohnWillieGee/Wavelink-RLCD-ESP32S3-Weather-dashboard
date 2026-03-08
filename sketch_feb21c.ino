#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans24pt7b.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <math.h>
#include <Wire.h>
#include "display_bsp.h"
#include "font.h"
#include "secfont.h"
#include "PCF85063A-SOLDERED.h"



// ===== CONFIGURATION =====
#include "secrets.h"   // WiFi SSID/password, API key, location — keep out of version control

const char* ntpServer          = "pool.ntp.org";

// POSIX TZ string — handles DST transitions automatically.
// AEST-10AEDT,M10.1.0,M4.1.0/3 means:
//   Standard: AEST UTC+10, DST: AEDT UTC+11
//   DST starts 1st Sunday October 02:00, ends 1st Sunday April 03:00
// Change if your timezone differs — other options:
//   "AWST-8"                           Perth       (no DST)
//   "ACST-9:30"                        Darwin      (no DST)
//   "ACST-9:30ACDT,M10.1.0,M4.1.0/3"  Adelaide    (DST)
//   "AEST-10"                          Brisbane    (no DST)
//   "AEST-10AEDT,M10.1.0,M4.1.0/3"    Sydney/Melb (DST)
const char* posixTZ = "AEST-10AEDT,M10.1.0,M4.1.0/3";

// gmtOffset_sec is now computed at runtime from the system clock after NTP sync.
// It reflects the current UTC offset including DST. Do NOT hardcode this.
long gmtOffset_sec = 36000;   // default AEST — overwritten after NTP sync

const int BTN_LEFT    = 0;
const int BTN_MIDDLE  = 18;
const int BAT_ADC_PIN = 4;
static const uint8_t SHTC3_ADDR = 0x70;

#define FONT_SMALL   FreeSans9pt7b
#define FONT_MEDIUM  FreeSans12pt7b
#define FONT_LARGE   FreeSans18pt7b
#define FONT_XLARGE  FreeSans24pt7b



static const int W = 400;
static const int H = 300;
DisplayPort RlcdPort(12, 11, 5, 40, 41, W, H);
GFXcanvas1  canvas(W, H);
PCF85063A   rtc;

float temperature    = 0.0f;
float humidity       = 0.0f;
float batteryVoltage = 0.0f;
int   wifiRSSI       = 0;
int   hour24         = 0;
int   minuteVal      = 0;
int   secondVal      = 0;
bool  wifiConnected  = false;
int   sensorUpdateCounter = 0;
unsigned long ntpLastSync = 0;
int   sensorReadCount     = 0;
int   sensorFailCount     = 0;

int  currentPage  = 0;
const int totalPages = 14;

// Flash state for Earth dot on seasons orbit page
bool     earthFlashOn    = true;
unsigned long earthFlashLast = 0;
unsigned long lastButtonPress = 0;
const unsigned long debounceDelay = 200;
bool btn_left_pressed   = false;
bool btn_middle_pressed = false;

const int HISTORY_SIZE = 24;

struct WeatherData {
  float  currentTemp;
  float  feelsLike;
  String condition;
  int    humidity;
  float  windSpeed;
  String windDir;
  float  uvIndex;
  float  precipMM;
  int    airQualityIndex;
  String airQualityText;
  float  pm25;
  struct Forecast {
    String day;
    float  maxTemp;
    float  minTemp;
    String condition;
    float  precipMM;
    int    rainChance;
  } forecast[3];
  unsigned long lastUpdate;
  bool valid;
} weatherData;

struct AstronomyData {
  String sunrise, sunset, moonrise, moonset, moonPhase;
  int    moonIllumination;
  bool   valid;
} astroData;

struct HourlyData {
  float  temp[6];
  int    rainChance[6];
  float  rainMM[6];
  float  uvIndex[6];
  float  windSpeed[6];
  String time[6];
  bool   valid;
} hourlyData;

struct HistoricalData {
  float tempHistory[HISTORY_SIZE];
  float humidityHistory[HISTORY_SIZE];
  int   currentIndex;
  unsigned long lastLogTime;
  bool  initialized;
  int   sampleCount;
} history;

struct GraphBounds { float mn, mx, rng; };

struct SeasonEvents {
  int marchEq;
  int juneSol;
  int septEq;
  int decSol;
};

struct SeasonInfo {
  const char* name;
  int daysSince;
  int daysUntil;
  const char* nextEvent;
  int nextEventDoy;
};

// ===== TZ LABEL HELPER =====
// Returns the current local timezone abbreviation (e.g. "AEDT" or "AEST")
// by reading tm_zone from the system clock — set correctly by the POSIX TZ string.
// Falls back to a UTC-offset string if tm_zone is unavailable.
const char* getTZLabel() {
  // tm_zone and tm_gmtoff are not available in ESP32 newlib's struct tm.
  // Derive the label from gmtOffset_sec (updated after every NTP sync)
  // combined with tm_isdst to distinguish DST from standard time.
  time_t now = time(nullptr);
  struct tm* ti = localtime(&now);
  bool dst = (ti && ti->tm_isdst > 0);
  // Map offset + DST flag to known AUS timezone labels
  if      (gmtOffset_sec == 39600)               return "AEDT";  // UTC+11 DST
  else if (gmtOffset_sec == 36000 && dst)         return "AEDT";  // should not occur but safe
  else if (gmtOffset_sec == 36000)               return "AEST";  // UTC+10 standard
  else if (gmtOffset_sec == 37800)               return "ACDT";  // UTC+10:30 DST
  else if (gmtOffset_sec == 34200)               return "ACST";  // UTC+9:30 standard
  else if (gmtOffset_sec == 28800)               return "AWST";  // UTC+8
  else if (gmtOffset_sec == 32400)               return "JST";
  else if (gmtOffset_sec == 0)                   return "UTC";
  else                                           return "LOC";
}

// ===== CENTERED TEXT HELPER =====
void printCentered(const GFXfont* font, int y, const char* text) {
  canvas.setFont(font);
  int16_t x1, y1; uint16_t tw, th;
  canvas.getTextBounds(text, 0, y, &x1, &y1, &tw, &th);
  canvas.setCursor((W - tw) / 2 - x1, y);
  canvas.print(text);
}

// ===== CRC8 / SHTC3 =====
uint8_t crc8(const uint8_t *data, int len) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

bool shtc3_cmd(uint16_t cmd) {
  Wire.beginTransmission(SHTC3_ADDR);
  Wire.write((uint8_t)(cmd >> 8));
  Wire.write((uint8_t)(cmd & 0xFF));
  return Wire.endTransmission() == 0;
}

bool shtc3_read(float &tempC, float &rh) {
  if (!shtc3_cmd(0x3517)) return false;
  delay(50);
  if (!shtc3_cmd(0x7866)) return false;
  delay(20);
  Wire.requestFrom((int)SHTC3_ADDR, 6);
  if (Wire.available() != 6) return false;
  uint8_t d[6];
  for (int i = 0; i < 6; i++) d[i] = Wire.read();
  if (crc8(&d[0], 2) != d[2] || crc8(&d[3], 2) != d[5]) return false;
  uint16_t tRaw  = (uint16_t)d[0] << 8 | d[1];
  uint16_t rhRaw = (uint16_t)d[3] << 8 | d[4];
  tempC = -45.0f + 175.0f * (float)tRaw  / 65536.0f - 4.0f;
  rh    = 100.0f * (float)rhRaw / 65536.0f;
  shtc3_cmd(0xB098);
  return true;
}

// ===== BATTERY =====
float readBatteryVoltage() {
  int raw = analogRead(BAT_ADC_PIN);
  return (raw / 4095.0f) * 3.3f * 3.0f * 1.079f;
}

int batteryToSegments(float vbat) {
  if (vbat >= 4.0f)  return 5;
  if (vbat >= 3.90f) return 4;
  if (vbat >= 3.80f) return 3;
  if (vbat >= 3.65f) return 2;
  if (vbat >= 3.50f) return 1;
  return 0;
}



// ===== WEATHER API =====
bool fetchWeatherData() {
  if (!wifiConnected) return false;
  HTTPClient http;
  String url = "https://api.weatherapi.com/v1/forecast.json?key=" + String(weatherApiKey) +
               "&q=" + String(weatherLocation) + "&days=3&aqi=yes&alerts=no";
  http.begin(url); http.setTimeout(15000);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String payload = http.getString(); http.end();
  if (payload.length() < 5000) return false;

  auto pfloat = [&](const char* key, int from, int to) -> float {
    int p = payload.indexOf(key, from);
    if (p < 0 || (to > 0 && p > to)) return 0.0f;
    p += strlen(key);
    int e1 = payload.indexOf(",", p), e2 = payload.indexOf("}", p);
    int e = (e1 > 0 && (e2 < 0 || e1 < e2)) ? e1 : e2;
    String s = payload.substring(p, e); s.trim(); return s.toFloat();
  };
  auto pint = [&](const char* key, int from, int to) -> int {
    int p = payload.indexOf(key, from);
    if (p < 0 || (to > 0 && p > to)) return 0;
    p += strlen(key);
    int e1 = payload.indexOf(",", p), e2 = payload.indexOf("}", p);
    int e = (e1 > 0 && (e2 < 0 || e1 < e2)) ? e1 : e2;
    String s = payload.substring(p, e); s.trim(); return s.toInt();
  };
  auto pstr = [&](const char* key, int from) -> String {
    int p = payload.indexOf(key, from);
    if (p < 0) return "";
    p += strlen(key);
    return payload.substring(p, payload.indexOf("\"", p));
  };

  int curPos   = payload.indexOf("\"current\":");
  int fcastPos = payload.indexOf("\"forecast\":");

  weatherData.currentTemp = pfloat("\"temp_c\":",      curPos, fcastPos);
  weatherData.feelsLike   = pfloat("\"feelslike_c\":", curPos, fcastPos);
  weatherData.humidity    = pint  ("\"humidity\":",    curPos, fcastPos);
  weatherData.windSpeed   = pfloat("\"wind_kph\":",    curPos, fcastPos);
  weatherData.windDir     = pstr  ("\"wind_dir\":\"",  curPos);
  weatherData.condition   = pstr  ("\"text\":\"",      curPos);
  weatherData.precipMM    = pfloat("\"precip_mm\":",   curPos, fcastPos);

  {
    int uvPos = payload.indexOf("\"uv\":", curPos);
    if (uvPos > 0 && uvPos < fcastPos) {
      uvPos += 5;
      int e1 = payload.indexOf(",", uvPos), e2 = payload.indexOf("}", uvPos);
      int e = (e1 > 0 && (e2 < 0 || e1 < e2)) ? e1 : e2;
      String s = payload.substring(uvPos, e); s.trim();
      weatherData.uvIndex = s.toFloat();
    } else weatherData.uvIndex = 0.0f;
  }

  weatherData.airQualityIndex = pint("\"us-epa-index\":", curPos, fcastPos);
  weatherData.pm25            = pfloat("\"pm2_5\":",       curPos, fcastPos);
  switch (weatherData.airQualityIndex) {
    case 1: weatherData.airQualityText = "Good";           break;
    case 2: weatherData.airQualityText = "Moderate";       break;
    case 3: weatherData.airQualityText = "Unhealthy+";     break;
    case 4: weatherData.airQualityText = "Unhealthy";      break;
    case 5: weatherData.airQualityText = "Very Unhealthy"; break;
    case 6: weatherData.airQualityText = "Hazardous";      break;
    default:weatherData.airQualityText = "Unknown";        break;
  }

  int arrPos = payload.indexOf("\"forecastday\":[");
  if (arrPos > 0) {
    int sPos = arrPos;
    for (int i = 0; i < 3; i++) {
      int p = payload.indexOf("\"date\":\"", sPos); if (p < 0) break;
      p += 8; int e = payload.indexOf("\"", p);
      weatherData.forecast[i].day = payload.substring(p, e); sPos = e;
      int dObj = payload.indexOf("\"day\":{", sPos); if (dObj < 0) break;
      weatherData.forecast[i].maxTemp    = pfloat("\"maxtemp_c\":",        dObj, dObj+2000);
      weatherData.forecast[i].minTemp    = pfloat("\"mintemp_c\":",        dObj, dObj+2000);
      weatherData.forecast[i].precipMM   = pfloat("\"totalprecip_mm\":",   dObj, dObj+2000);
      weatherData.forecast[i].rainChance = pint  ("\"daily_chance_of_rain\":", dObj, dObj+2000);
      int cPos = payload.indexOf("\"condition\":", dObj);
      if (cPos > 0) { weatherData.forecast[i].condition = pstr("\"text\":\"", cPos); sPos = cPos+100; }
      else sPos = dObj+100;
    }
  }

  int astroPos = payload.indexOf("\"astronomy\":");
  if (astroPos < 0) astroPos = payload.indexOf("\"astro\":");
  if (astroPos > 0) {
    astroData.sunrise          = pstr("\"sunrise\":\"",    astroPos);
    astroData.sunset           = pstr("\"sunset\":\"",     astroPos);
    astroData.moonrise         = pstr("\"moonrise\":\"",   astroPos);
    astroData.moonset          = pstr("\"moonset\":\"",    astroPos);
    astroData.moonPhase        = pstr("\"moon_phase\":\"", astroPos);
    astroData.moonIllumination = pint("\"moon_illumination\":", astroPos, astroPos+500);
    astroData.valid = true;
  }

  int hourPos = payload.indexOf("\"hour\":[");
  if (hourPos > 0) {
    int sPos = hourPos, found = 0;
    for (int h = 0; h < 24 && found < 6; h++) {
      int tp = payload.indexOf("\"time\":\"", sPos); if (tp < 0) break;
      tp += 8; int te = payload.indexOf("\"", tp);
      String ts = payload.substring(tp, te);
      int hv = ts.substring(11, 13).toInt(); sPos = te;
      if (hv < hour24) continue;
      hourlyData.time[found]       = ts.substring(11, 16);
      hourlyData.temp[found]       = pfloat("\"temp_c\":",         sPos, sPos+2000);
      hourlyData.rainChance[found] = pint  ("\"chance_of_rain\":", sPos, sPos+2000);
      hourlyData.rainMM[found]     = pfloat("\"precip_mm\":",      sPos, sPos+2000);
      hourlyData.uvIndex[found]    = pfloat("\"uv\":",             sPos, sPos+2000);
      hourlyData.windSpeed[found]  = pfloat("\"wind_kph\":",       sPos, sPos+2000);
      found++;
    }
    hourlyData.valid = (found > 0);
  }

  weatherData.lastUpdate = millis();
  weatherData.valid = true;
  return true;
}

// ===== DISPLAY HELPERS =====
void pushCanvasToRLCD(bool invert = false) {
  uint8_t *buf = canvas.getBuffer();
  const int bpr = (W + 7) / 8;
  RlcdPort.RLCD_ColorClear(ColorWhite);
  for (int y = 0; y < H; y++) {
    uint8_t *row = buf + y * bpr;
    for (int bx = 0; bx < bpr; bx++) {
      uint8_t v = invert ? row[bx] ^ 0xFF : row[bx];
      int x0 = bx * 8;
      for (int bit = 0; bit < 8; bit++) {
        int x = x0 + bit; if (x >= W) break;
        if (v & (0x80 >> bit)) RlcdPort.RLCD_SetPixel((uint16_t)x, (uint16_t)y, ColorBlack);
      }
    }
  }
  RlcdPort.RLCD_Display();
}

void drawThermometerIcon(int x, int y) {
  canvas.drawCircle(x+3, y+18, 4, 1); canvas.fillCircle(x+3, y+18, 2, 1);
  canvas.fillRect(x+1, y, 4, 16, 1);  canvas.fillRect(x+2, y, 2, 16, 0);
}

void drawDropletIcon(int x, int y) {
  canvas.fillCircle(x+4, y+10, 4, 1);
  canvas.fillTriangle(x+4, y, x, y+8, x+8, y+8, 1);
  canvas.fillCircle(x+4, y+10, 2, 0);
}

void drawWiFiIcon(int x, int y, int rssi) {
  int bars = 0;
  if      (rssi > -50) bars = 4;
  else if (rssi > -60) bars = 3;
  else if (rssi > -70) bars = 2;
  else if (rssi > -80) bars = 1;
  for (int i = 0; i < 4; i++) {
    int h = (i+1) * 4;
    if (i < bars && wifiConnected) canvas.fillRect(x+i*6, y+16-h, 4, h, 1);
    else                           canvas.drawRect(x+i*6, y+16-h, 4, h, 1);
  }
}


// ===== PAGE 0: DASHBOARD =====
void drawDashboardPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1);
  canvas.drawRect(1, 1, W-2, H-2, 1);

  int dow = rtc.getWeekday();
  canvas.drawRect(8, 8, 384, 38, 1);
  drawWiFiIcon(15, 18, wifiRSSI);
  canvas.setFont(&FONT_MEDIUM); canvas.setTextColor(1);
  canvas.setCursor(48, 33);
  if (wifiConnected) canvas.print(wifiRSSI); else canvas.print("--");
  canvas.fillRect(130, 12, 2, 30, 1);

  canvas.setFont(&FONT_MEDIUM); canvas.setCursor(150, 35);
  canvas.print(getTZLabel());

  canvas.fillRect(240, 12, 2, 30, 1);
  int batX = 310;
  canvas.drawRect(batX, 18, 60, 20, 1);
  canvas.fillRect(batX+60, 24, 3, 8, 1);
  int segs = batteryToSegments(batteryVoltage);
  for (int i = 0; i < segs; i++) canvas.fillRect(batX+2+(i*11), 20, 10, 16, 1);
  canvas.setFont(&FONT_MEDIUM); canvas.setCursor(250, 33);
  canvas.print(batteryVoltage, 1); canvas.print("V");

  // Temperature box
  canvas.drawRect(12, 55, 185, 78, 1); canvas.drawRect(13, 56, 183, 76, 1);
  drawThermometerIcon(20, 67);
  canvas.setFont(&FONT_SMALL); canvas.setCursor(45, 76); canvas.print("TEMPERATURE");
  canvas.setFont(&FONT_XLARGE); canvas.setCursor(30, 122); canvas.print(temperature, 1);
  canvas.setFont(&FONT_MEDIUM); canvas.setCursor(155, 122); canvas.print("C");

  // Humidity box
  canvas.drawRect(203, 55, 185, 78, 1); canvas.drawRect(204, 56, 183, 76, 1);
  drawDropletIcon(211, 67);
  canvas.setFont(&FONT_SMALL); canvas.setCursor(237, 76); canvas.print("HUMIDITY");
  canvas.setFont(&FONT_XLARGE); canvas.setCursor(222, 122); canvas.print((int)humidity);
  canvas.setFont(&FONT_MEDIUM); canvas.setCursor(338, 122); canvas.print("%");

  canvas.fillRect(8, 145, 384, 3, 1);

  const char* fullDays[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
  canvas.setFont(&FONT_MEDIUM); canvas.setCursor(15, 172);
  if (dow >= 0 && dow <= 6) canvas.print(fullDays[dow]);
  canvas.print(" ");
  canvas.print(rtc.getDay()); canvas.print("/");
  canvas.print(rtc.getMonth()); canvas.print("/");
  int yr = rtc.getYear() % 100;
  if (yr < 10) canvas.print("0");
  canvas.print(yr);
  canvas.setCursor(255, 172); canvas.print(weatherLocation);

  char timeStr[6]; sprintf(timeStr, "%02d:%02d", hour24, minuteVal);
  canvas.setFont(&DSEG7_Classic_Bold_84); canvas.setCursor(18, 268); canvas.print(timeStr);
  char secStr[3]; sprintf(secStr, "%02d", secondVal);
  canvas.setFont(&DSEG7_Classic_Bold_36); canvas.setCursor(323, 230); canvas.print(secStr);

  pushCanvasToRLCD(false);
}

// ===== PAGE 1: ANALOGUE CLOCK =====
void drawAnalogClockPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1);
  canvas.drawRect(1, 1, W-2, H-2, 1);

  // Inverted header bar — location + timezone
  canvas.fillRect(8, 8, 384, 22, 1);
  canvas.setTextColor(0);
  canvas.setFont(&FONT_SMALL);
  char hdrBuf[32];
  snprintf(hdrBuf, sizeof(hdrBuf), "%s  %s", weatherLocation, getTZLabel());
  int16_t hx1, hy1; uint16_t htw, hth;
  canvas.getTextBounds(hdrBuf, 0, 24, &hx1, &hy1, &htw, &hth);
  canvas.setCursor((W - htw) / 2 - hx1, 24);
  canvas.print(hdrBuf);
  canvas.setTextColor(1);

  // Clock geometry — filled black face, R=110
  const int cx = 200, cy = 152, R = 110;

  // Filled black face
  canvas.fillCircle(cx, cy, R, 1);

  // Hour markers — white, thick at 12/3/6/9, medium elsewhere
  for (int i = 0; i < 12; i++) {
    float a = (i * 30 - 90) * 3.14159f / 180.0f;
    bool isCard = (i % 3 == 0);
    int r1 = R - 2, r2 = isCard ? R - 20 : R - 12;
    int x1 = cx + (int)(r1 * cosf(a)), y1 = cy + (int)(r1 * sinf(a));
    int x2 = cx + (int)(r2 * cosf(a)), y2 = cy + (int)(r2 * sinf(a));
    // Draw thick white marker as 2-3 adjacent lines
    canvas.drawLine(x1, y1, x2, y2, 0);
    if (isCard) {
      // Extra adjacent pixels for thick cardinal markers
      int px1 = cx + (int)(r1 * cosf(a + 0.04f)), py1 = cy + (int)(r1 * sinf(a + 0.04f));
      int px2 = cx + (int)(r2 * cosf(a + 0.04f)), py2 = cy + (int)(r2 * sinf(a + 0.04f));
      canvas.drawLine(px1, py1, px2, py2, 0);
      int qx1 = cx + (int)(r1 * cosf(a - 0.04f)), qy1 = cy + (int)(r1 * sinf(a - 0.04f));
      int qx2 = cx + (int)(r2 * cosf(a - 0.04f)), qy2 = cy + (int)(r2 * sinf(a - 0.04f));
      canvas.drawLine(qx1, qy1, qx2, qy2, 0);
    }
  }

  // Minute ticks — white, thin
  for (int i = 0; i < 60; i++) {
    if (i % 5 == 0) continue;
    float a = (i * 6 - 90) * 3.14159f / 180.0f;
    int x1 = cx + (int)((R-2)  * cosf(a)), y1 = cy + (int)((R-2)  * sinf(a));
    int x2 = cx + (int)((R-7) * cosf(a)), y2 = cy + (int)((R-7) * sinf(a));
    canvas.drawLine(x1, y1, x2, y2, 0);
  }

  // Hour numerals 12, 3, 6, 9 in white (setTextColor 0 = white on filled face)
  canvas.setTextColor(0);
  canvas.setFont(&FONT_MEDIUM);
  struct { const char* n; int a; } hnums[] = {{"12",0},{"3",90},{"6",180},{"9",270}};
  for (int i = 0; i < 4; i++) {
    float rad = (hnums[i].a - 90) * 3.14159f / 180.0f;
    int nr = R - 30;
    int16_t nx1, ny1; uint16_t ntw, nth;
    canvas.getTextBounds(hnums[i].n, 0, 0, &nx1, &ny1, &ntw, &nth);
    int tx = cx + (int)(nr * cosf(rad)) - ntw/2 - nx1;
    int ty = cy + (int)(nr * sinf(rad)) + nth/2;
    canvas.setCursor(tx, ty);
    canvas.print(hnums[i].n);
  }
  canvas.setTextColor(1);

  // Compute hand angles
  int h12    = hour24 % 12;
  float secF  = (float)secondVal;
  float minF  = (float)minuteVal + secF / 60.0f;
  float hourF = (float)h12 + minF / 60.0f;
  float hourAngle = hourF * 30.0f;   // degrees
  float minAngle  = minF  * 6.0f;
  float secAngle  = secF  * 6.0f;

  // Hour hand — white, thick (3 adjacent lines)
  float hRad = (hourAngle - 90.0f) * 3.14159f / 180.0f;
  int hx = cx + (int)(63 * cosf(hRad)), hy = cy + (int)(63 * sinf(hRad));
  canvas.drawLine(cx, cy, hx, hy, 0);
  canvas.drawLine(cx+1, cy,   hx+1, hy,   0);
  canvas.drawLine(cx,   cy+1, hx,   hy+1, 0);
  canvas.drawLine(cx-1, cy,   hx-1, hy,   0);
  canvas.drawLine(cx,   cy-1, hx,   hy-1, 0);

  // Minute hand — white, medium (2 adjacent lines)
  float mRad = (minAngle - 90.0f) * 3.14159f / 180.0f;
  int mhx = cx + (int)(90 * cosf(mRad)), mhy = cy + (int)(90 * sinf(mRad));
  canvas.drawLine(cx, cy, mhx, mhy, 0);
  canvas.drawLine(cx+1, cy,   mhx+1, mhy,   0);
  canvas.drawLine(cx,   cy+1, mhx,   mhy+1, 0);

  // Second hand — white, single pixel with tail
  float sRad = (secAngle - 90.0f) * 3.14159f / 180.0f;
  int shx = cx + (int)(98  * cosf(sRad)), shy  = cy + (int)(98  * sinf(sRad));
  int stx = cx + (int)(19  * cosf(sRad + 3.14159f)), sty = cy + (int)(19 * sinf(sRad + 3.14159f));
  canvas.drawLine(stx, sty, shx, shy, 0);

  // Centre cap — white filled, black dot
  canvas.fillCircle(cx, cy, 5, 0);
  canvas.fillCircle(cx, cy, 2, 1);

  // Bottom strip — divider line then date and indoor data
  canvas.fillRect(8, 270, 384, 2, 1);
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  // Date left
  char dateBuf[16];
  const char* days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  int dow = rtc.getWeekday();
  snprintf(dateBuf, sizeof(dateBuf), "%s %d/%d/%02d",
    (dow>=0&&dow<=6)?days[dow]:"---",
    rtc.getDay(), rtc.getMonth(), rtc.getYear()%100);
  canvas.setCursor(14, 290); canvas.print(dateBuf);
  // Temp/humidity/battery right-aligned
  char infoBuf[24];
  snprintf(infoBuf, sizeof(infoBuf), "%.1fC  %d%%  %.1fV",
    temperature, (int)humidity, batteryVoltage);
  int16_t ix1, iy1; uint16_t itw, ith;
  canvas.getTextBounds(infoBuf, 0, 290, &ix1, &iy1, &itw, &ith);
  canvas.setCursor(W - 14 - itw - ix1, 290); canvas.print(infoBuf);

  pushCanvasToRLCD(false);
}

// ===== PAGE 2: AUS TIMEZONE CLOCK =====
void drawTimeZonePage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1);
  canvas.drawRect(1, 1, W-2, H-2, 1);

  // Inverted header bar — centred text
  canvas.fillRect(8, 8, 384, 22, 1);
  canvas.setTextColor(0); canvas.setFont(&FONT_SMALL);
  int16_t tzHx1, tzHy1; uint16_t tzHtw, tzHth;
  canvas.getTextBounds("AUS TIME ZONES", 0, 24, &tzHx1, &tzHy1, &tzHtw, &tzHth);
  canvas.setCursor((W - tzHtw) / 2 - tzHx1, 24);
  canvas.print("AUS TIME ZONES");
  canvas.setTextColor(1);

  // Vertical centre divider
  canvas.fillRect(199, 30, 2, 258, 1);

  // Get current UTC time directly from the system epoch — no offset arithmetic needed.
  // gmtime() always returns UTC regardless of timezone setting.
  time_t nowEpoch = time(nullptr);
  struct tm utcTm;
  gmtime_r(&nowEpoch, &utcTm);
  int utcMins = utcTm.tm_hour * 60 + utcTm.tm_min;

  // DST state: localtime tm_isdst tells us accurately (set by POSIX TZ rule)
  struct tm* nowTm = localtime(&nowEpoch);
  bool dstActive = (nowTm && nowTm->tm_isdst > 0);

  // Zone offsets in minutes from UTC — DST affects Adelaide & Sydney/Melbourne
  // Perth   AWST  UTC+8     always
  // Darwin  ACST  UTC+9:30  always
  // Adelaide ACST/ACDT UTC+9:30 / UTC+10:30
  // Brisbane AEST  UTC+10   always
  // Sydney  AEST/AEDT UTC+10 / UTC+11
  struct TZEntry { const char* city; const char* tz; int offMins; } left[3], right[3];

  left[0] = { "Perth",    "AWST",                8*60       };
  left[1] = { "Darwin",   "ACST",                9*60+30    };
  left[2] = { "Adelaide", dstActive?"ACDT":"ACST", dstActive?10*60+30:9*60+30 };
  right[0]= { "Brisbane", "AEST",                10*60      };
  right[1]= { "Sydney",   dstActive?"AEDT":"AEST", dstActive?11*60:10*60 };
  right[2]= { "UTC",      "UTC",                 0          };

  // Grid: 3 rows each side, row height = 258/3 = 86px
  const int GT = 30, RH = 86, COLW = 191;
  const int LCOL = 8, RCOL = 201;

  for (int col = 0; col < 2; col++) {
    int lx = (col == 0) ? LCOL : RCOL;
    for (int i = 0; i < 3; i++) {
      TZEntry& z = (col == 0) ? left[i] : right[i];
      int ry = GT + i * RH;

      // Row divider
      if (i > 0) canvas.fillRect(lx, ry, COLW, 1, 1);

      // City name — plain, centred
      canvas.setFont(&FONT_SMALL);
      int16_t cx1, cy1; uint16_t ctw, cth;
      canvas.getTextBounds(z.city, 0, 0, &cx1, &cy1, &ctw, &cth);
      canvas.setCursor(lx + (COLW - ctw) / 2 - cx1, ry + 16);
      canvas.print(z.city);

      // TZ label centred
      int16_t tx1, ty1; uint16_t ttw, tth;
      canvas.getTextBounds(z.tz, 0, 0, &tx1, &ty1, &ttw, &tth);
      canvas.setCursor(lx + (COLW - ttw) / 2 - tx1, ry + 32);
      canvas.print(z.tz);

      // Time — DSEG7 36pt, centred
      int tzMins = ((utcMins + z.offMins) % 1440 + 1440) % 1440;
      int tzH = tzMins / 60, tzM = tzMins % 60;
      char tsBuf[6]; sprintf(tsBuf, "%02d:%02d", tzH, tzM);
      canvas.setFont(&DSEG7_Classic_Bold_36);
      int16_t dx1, dy1; uint16_t dtw, dth;
      canvas.getTextBounds(tsBuf, 0, 0, &dx1, &dy1, &dtw, &dth);
      canvas.setCursor(lx + (COLW - dtw) / 2 - dx1, ry + RH - 8);
      canvas.print(tsBuf);
    }
  }

  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  pushCanvasToRLCD(false);
}

// ===== PAGE 3: CURRENT CONDITIONS =====
void drawCurrentWeatherPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1);
  canvas.drawRect(1, 1, W-2, H-2, 1);

  if (!weatherData.valid) {
    canvas.setFont(&FONT_LARGE); canvas.setTextColor(1);
    canvas.setCursor(60, 130); canvas.print("NO WEATHER DATA");
    canvas.setFont(&FONT_SMALL); canvas.setCursor(110, 165); canvas.print("Press KEY button");
    pushCanvasToRLCD(false); return;
  }

  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(12, 25); canvas.print("CURRENT CONDITIONS  "); canvas.print(weatherLocation);

  canvas.setFont(&FONT_XLARGE); canvas.setCursor(12, 75); canvas.print(weatherData.currentTemp, 1);
  canvas.setFont(&FONT_LARGE);  canvas.print(" C");

  // Condition text — word-wrap into two lines if longer than ~20 chars
  canvas.setFont(&FONT_SMALL);
  String cond = weatherData.condition;
  const int condMaxChars = 20;
  if (cond.length() <= condMaxChars) {
    canvas.setCursor(12, 95); canvas.print(cond);
  } else {
    int sp = cond.lastIndexOf(' ', condMaxChars);
    if (sp < 1) sp = condMaxChars;
    canvas.setCursor(12, 95);  canvas.print(cond.substring(0, sp));
    canvas.setCursor(12, 109); canvas.print(cond.substring(sp + 1, min((int)cond.length(), sp + condMaxChars + 1)));
  }

  // Feels like — always on line 3
  canvas.setFont(&FONT_SMALL); canvas.setCursor(12, 123);
  canvas.print("Feels like "); canvas.print(weatherData.feelsLike, 1); canvas.print(" C");

  canvas.fillRect(200, 34, 2, 102, 1);
  canvas.fillRect(302, 34, 2, 102, 1);

  const int boxTop = 34, boxBot = 138, slotH = (boxBot - boxTop) / 2;
  canvas.setFont(&FONT_SMALL);  canvas.setCursor(208, boxTop + 16);         canvas.print("HIGH");
  canvas.setFont(&FONT_MEDIUM); canvas.setCursor(208, boxTop + 38);         canvas.print(weatherData.forecast[0].maxTemp, 1); canvas.setFont(&FONT_SMALL); canvas.print(" C");
  canvas.setFont(&FONT_SMALL);  canvas.setCursor(208, boxTop + slotH + 16); canvas.print("LOW");
  canvas.setFont(&FONT_MEDIUM); canvas.setCursor(208, boxTop + slotH + 38); canvas.print(weatherData.forecast[0].minTemp, 1); canvas.setFont(&FONT_SMALL); canvas.print(" C");
  canvas.setFont(&FONT_SMALL);  canvas.setCursor(310, boxTop + 16);         canvas.print("UV INDEX");
  canvas.setFont(&FONT_MEDIUM); canvas.setCursor(310, boxTop + 38);         canvas.print(weatherData.uvIndex, 1);
  canvas.setFont(&FONT_SMALL);  canvas.setCursor(310, boxTop + slotH + 16); canvas.print("HUMIDITY");
  canvas.setFont(&FONT_MEDIUM); canvas.setCursor(310, boxTop + slotH + 38); canvas.print(weatherData.humidity); canvas.print(" %");

  canvas.fillRect(8, 134, 384, 2, 1);

  int lx = 12, rx = 210, gy = 152, rowH = 42;
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(lx, gy); canvas.print("WIND");
  canvas.setCursor(rx, gy); canvas.print("OUTDOOR TEMP");
  canvas.setFont(&FONT_MEDIUM);
  canvas.setCursor(lx, gy+20); canvas.print(weatherData.windSpeed, 1); canvas.print(" kph "); canvas.print(weatherData.windDir);
  canvas.setCursor(rx, gy+20); canvas.print(weatherData.currentTemp, 1); canvas.print(" C");
  gy += rowH;
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(lx, gy); canvas.print("AIR QUALITY");
  canvas.setCursor(rx, gy); canvas.print("PM2.5");
  canvas.setFont(&FONT_MEDIUM);
  canvas.setCursor(lx, gy+20); canvas.print(weatherData.airQualityText);
  canvas.setCursor(rx, gy+20); canvas.print(weatherData.pm25, 1); canvas.print(" ug/m3");
  gy += rowH;
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(lx, gy); canvas.print("LAST UPDATED");
  canvas.setCursor(rx, gy); canvas.print("RAINFALL");
  canvas.setFont(&FONT_MEDIUM);
  canvas.setCursor(lx, gy+20); canvas.print((millis() - weatherData.lastUpdate) / 60000); canvas.print(" min ago");
  canvas.setCursor(rx, gy+20); canvas.print(weatherData.precipMM, 1); canvas.print(" mm");

  canvas.fillRect(200, 136, 2, 132, 1);
  canvas.fillRect(8, 268, 384, 2, 1);
  pushCanvasToRLCD(false);
}

// ===== PAGE 5: 3-DAY FORECAST =====
void drawForecastPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1);
  canvas.drawRect(1, 1, W-2, H-2, 1);

  if (!weatherData.valid) {
    canvas.setFont(&FONT_LARGE); canvas.setTextColor(1);
    canvas.setCursor(60, 130); canvas.print("NO WEATHER DATA");
    canvas.setFont(&FONT_SMALL); canvas.setCursor(110, 165); canvas.print("Press KEY button");
    pushCanvasToRLCD(false); return;
  }

  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(12, 25); canvas.print("3-DAY FORECAST  "); canvas.print(weatherLocation);

  const int cardW = 122, cardY = 36, hdrH = 24;
  const int rowH = 40, condH = 52;
  const int cardH = hdrH + rowH + rowH + condH + rowH + rowH;

  for (int i = 0; i < 3; i++) {
    int cx = 10 + i * (cardW + 4);
    canvas.drawRect(cx, cardY, cardW, cardH, 1);
    canvas.fillRect(cx+1, cardY+1, cardW-2, hdrH-1, 1);
    String dayStr = weatherData.forecast[i].day;
    if (dayStr.length() >= 10) dayStr = dayStr.substring(5);
    canvas.setTextColor(0); canvas.setFont(&FONT_SMALL);
    canvas.setCursor(cx+6, cardY+17); canvas.print(dayStr);
    canvas.setTextColor(1);

    int y = cardY + hdrH + 2;
    canvas.setFont(&FONT_SMALL);  canvas.setCursor(cx+6, y+11); canvas.print("HIGH");
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(cx+6, y+30); canvas.print(weatherData.forecast[i].maxTemp, 0); canvas.print(" C");
    y += rowH;
    canvas.setFont(&FONT_SMALL);  canvas.setCursor(cx+6, y+11); canvas.print("LOW");
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(cx+6, y+30); canvas.print(weatherData.forecast[i].minTemp, 0); canvas.print(" C");
    y += rowH;
    canvas.setFont(&FONT_SMALL);  canvas.setCursor(cx+6, y+11); canvas.print("COND");
    String fcond = weatherData.forecast[i].condition;
    if (fcond.length() > 13) {
      int sp = fcond.lastIndexOf(' ', 13);
      if (sp > 0) {
        canvas.setCursor(cx+6, y+28); canvas.print(fcond.substring(0, sp));
        canvas.setCursor(cx+6, y+42); canvas.print(fcond.substring(sp+1, min((int)fcond.length(), sp+14)));
      } else {
        canvas.setCursor(cx+6, y+28); canvas.print(fcond.substring(0, 13));
      }
    } else {
      canvas.setCursor(cx+6, y+28); canvas.print(fcond);
    }
    y += condH;
    canvas.setFont(&FONT_SMALL);  canvas.setCursor(cx+6, y+11); canvas.print("RAIN");
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(cx+6, y+30); canvas.print(weatherData.forecast[i].precipMM, 1); canvas.print("mm");
    y += rowH;
    canvas.setFont(&FONT_SMALL);  canvas.setCursor(cx+6, y+11); canvas.print("CHANCE");
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(cx+6, y+30);
    if (i == 0 && hourlyData.valid) {
      int maxChance = 0;
      for (int h = 0; h < 6; h++) if (hourlyData.rainChance[h] > maxChance) maxChance = hourlyData.rainChance[h];
      canvas.print(maxChance); canvas.print(" %");
    } else {
      canvas.print(weatherData.forecast[i].rainChance); canvas.print(" %");
    }
  }

  // No bottom divider line — cards contain all content
  canvas.setFont(&FONT_SMALL); canvas.setCursor(12, 285);
  canvas.print("Updated "); canvas.print((millis() - weatherData.lastUpdate) / 60000); canvas.print("m ago");
  pushCanvasToRLCD(false);
}

// ===== ASTRONOMY HELPERS =====
int parseTimeToMinutes(const String& t) {
  if (t.length() < 4) return 0;
  int colon = t.indexOf(':'); if (colon < 0) return 0;
  int h = t.substring(0, colon).toInt();
  int m = t.substring(colon+1, colon+3).toInt();
  bool pm = (t.indexOf("PM") >= 0 || t.indexOf("pm") >= 0);
  bool am = (t.indexOf("AM") >= 0 || t.indexOf("am") >= 0);
  if (pm && h != 12) h += 12;
  if (am && h == 12) h  = 0;
  return h * 60 + m;
}

String minutesToTimeStr(int mins) {
  mins = ((mins % 1440) + 1440) % 1440;
  char buf[6]; sprintf(buf, "%02d:%02d", mins / 60, mins % 60);
  return String(buf);
}

void drawMoonIcon(int cx, int cy, int r, const String& phase) {
  if (phase == "New Moon")  { canvas.drawCircle(cx, cy, r, 1); return; }
  if (phase == "Full Moon") { canvas.fillCircle(cx, cy, r, 1); return; }
  bool waxing = (phase == "Waxing Crescent" || phase == "First Quarter" || phase == "Waxing Gibbous");
  canvas.fillCircle(cx, cy, r, 1);
  int ox = 0;
  if      (phase == "Waxing Crescent" || phase == "Waning Crescent") ox = r - 2;
  else if (phase == "First Quarter"   || phase == "Last Quarter")     ox = 0;
  else if (phase == "Waxing Gibbous"  || phase == "Waning Gibbous")   ox = -(r - 2);
  int darkX = waxing ? cx - ox : cx + ox;
  canvas.fillCircle(darkX, cy, r, 0);
  canvas.drawCircle(cx, cy, r, 1);
}

// ===== PAGE 6: ASTRONOMY =====
void drawAstronomyPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1);
  canvas.drawRect(1, 1, W-2, H-2, 1);
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  const char* fullDays[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
  int dow = rtc.getWeekday();
  canvas.setCursor(12, 24);
  if (dow >= 0 && dow <= 6) canvas.print(fullDays[dow]);
  canvas.print("  ");
  canvas.print(rtc.getDay()); canvas.print("/"); canvas.print(rtc.getMonth()); canvas.print("/");
  int yr = rtc.getYear() % 100; if (yr < 10) canvas.print("0"); canvas.print(yr);
  canvas.print("  ASTRONOMY  "); canvas.print(weatherLocation);

  if (!astroData.valid) {
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(80, 160); canvas.print("NO ASTRONOMY DATA");
    pushCanvasToRLCD(false); return;
  }

  canvas.drawRect(8, 36, 384, 72, 1); canvas.drawRect(9, 37, 382, 70, 1);
  int lx = 20, rx = 210;
  int sunriseMins = parseTimeToMinutes(astroData.sunrise);
  int sunsetMins  = parseTimeToMinutes(astroData.sunset);
  int dayLenMins  = sunsetMins - sunriseMins;
  int noonMins    = sunriseMins + dayLenMins / 2;
  int dlH = dayLenMins / 60, dlM = dayLenMins % 60;

  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(lx, 56); canvas.print("SUNRISE");
  canvas.setCursor(rx, 56); canvas.print("SUNSET");
  canvas.setFont(&FONT_LARGE);
  canvas.setCursor(lx, 84); canvas.print(astroData.sunrise);
  canvas.setCursor(rx, 84); canvas.print(astroData.sunset);
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(lx, 100); canvas.print("Day: "); canvas.print(dlH); canvas.print("h "); canvas.print(dlM); canvas.print("m");
  canvas.setCursor(rx, 100); canvas.print("Solar noon: "); canvas.print(minutesToTimeStr(noonMins));

  const int moonTop = 112, moonBot = 266, moonRows = 3;
  const int moonSlotH = (moonBot - moonTop - 4) / moonRows;
  canvas.drawRect(8, moonTop, 384, moonBot - moonTop, 1);
  canvas.drawRect(9, moonTop+1, 382, moonBot - moonTop - 2, 1);
  drawMoonIcon(340, moonTop + (moonBot - moonTop)/2, 28, astroData.moonPhase);

  String phase = astroData.moonPhase;
  int illum = astroData.moonIllumination;
  String nextPhase = ""; float daysUntil = 0.0f;
  if      (phase == "New Moon")        { nextPhase = "Waxing Crescent"; daysUntil = 3.7f - (illum / 100.0f * 3.7f); }
  else if (phase == "Waxing Crescent") { nextPhase = "First Quarter";   daysUntil = 3.7f - (illum / 50.0f * 3.7f); }
  else if (phase == "First Quarter")   { nextPhase = "Waxing Gibbous";  daysUntil = 3.7f - ((illum-50) / 50.0f * 3.7f); }
  else if (phase == "Waxing Gibbous")  { nextPhase = "Full Moon";       daysUntil = 3.7f - (illum / 100.0f * 3.7f); }
  else if (phase == "Full Moon")       { nextPhase = "Waning Gibbous";  daysUntil = 3.7f - ((100-illum) / 100.0f * 3.7f); }
  else if (phase == "Waning Gibbous")  { nextPhase = "Last Quarter";    daysUntil = 3.7f - ((100-illum) / 50.0f * 3.7f); }
  else if (phase == "Last Quarter")    { nextPhase = "Waning Crescent"; daysUntil = 3.7f - ((50-illum) / 50.0f * 3.7f); }
  else if (phase == "Waning Crescent") { nextPhase = "New Moon";        daysUntil = 3.7f - (illum / 100.0f * 3.7f); }
  if (daysUntil < 0.5f) daysUntil = 0.5f;

  for (int r = 0; r < moonRows; r++) {
    int sy = moonTop + 8 + r * moonSlotH;
    canvas.setFont(&FONT_SMALL);
    switch (r) {
      case 0:
        canvas.setCursor(lx, sy+12); canvas.print("MOONRISE"); canvas.setCursor(rx, sy+12); canvas.print("MOONSET");
        canvas.setFont(&FONT_MEDIUM);
        canvas.setCursor(lx, sy+32); canvas.print(astroData.moonrise); canvas.setCursor(rx, sy+32); canvas.print(astroData.moonset);
        break;
      case 1:
        canvas.setCursor(lx, sy+12); canvas.print("PHASE"); canvas.setCursor(rx, sy+12); canvas.print("ILLUM");
        canvas.setFont(&FONT_MEDIUM);
        canvas.setCursor(lx, sy+32); canvas.print(phase); canvas.setCursor(rx, sy+32); canvas.print(illum); canvas.print(" %");
        break;
      case 2:
        canvas.setCursor(lx, sy+12); canvas.print("NEXT PHASE"); canvas.setCursor(rx, sy+12); canvas.print("DAYS UNTIL");
        canvas.setFont(&FONT_MEDIUM);
        canvas.setCursor(lx, sy+32); canvas.print(nextPhase); canvas.setCursor(rx, sy+32); canvas.print((int)roundf(daysUntil)); canvas.print(" days");
        break;
    }
  }
  pushCanvasToRLCD(false);
}

// ===== PAGE 7: LUNAR ORBIT =====
void drawLunarOrbitPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1);
  canvas.drawRect(1, 1, W-2, H-2, 1);
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(12, 24); canvas.print("LUNAR ORBIT  "); canvas.print(weatherLocation);

  if (!astroData.valid) {
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(80, 160); canvas.print("NO ASTRONOMY DATA");
    pushCanvasToRLCD(false); return;
  }

  String phase = astroData.moonPhase;
  int illum    = astroData.moonIllumination;

  // Moon age approximation from phase
  int age = 0;
  if      (phase == "New Moon")        age = 0;
  else if (phase == "Waxing Crescent") age = 4;
  else if (phase == "First Quarter")   age = 7;
  else if (phase == "Waxing Gibbous")  age = 11;
  else if (phase == "Full Moon")       age = 15;
  else if (phase == "Waning Gibbous")  age = 18;
  else if (phase == "Last Quarter")    age = 22;
  else if (phase == "Waning Crescent") age = 26;
  // (cycleProgress not used — age drives the progress arc directly)

  // Next phase
  int daysUntilNext = 0;
  if      (phase == "New Moon")        daysUntilNext = 4;
  else if (phase == "Waxing Crescent") daysUntilNext = 3;
  else if (phase == "First Quarter")   daysUntilNext = 4;
  else if (phase == "Waxing Gibbous")  daysUntilNext = 4;
  else if (phase == "Full Moon")       daysUntilNext = 4;
  else if (phase == "Waning Gibbous")  daysUntilNext = 3;
  else if (phase == "Last Quarter")    daysUntilNext = 4;
  else if (phase == "Waning Crescent") daysUntilNext = 4;

  // Phase angle on orbit + next phase angle for midpoint calc
  int moonAngleDeg = 0, nextAngleDeg = 45;
  if      (phase == "New Moon")        { moonAngleDeg =   0; nextAngleDeg =  45; }
  else if (phase == "Waxing Crescent") { moonAngleDeg =  45; nextAngleDeg =  90; }
  else if (phase == "First Quarter")   { moonAngleDeg =  90; nextAngleDeg = 135; }
  else if (phase == "Waxing Gibbous")  { moonAngleDeg = 135; nextAngleDeg = 180; }
  else if (phase == "Full Moon")       { moonAngleDeg = 180; nextAngleDeg = 225; }
  else if (phase == "Waning Gibbous")  { moonAngleDeg = 225; nextAngleDeg = 270; }
  else if (phase == "Last Quarter")    { moonAngleDeg = 270; nextAngleDeg = 315; }
  else if (phase == "Waning Crescent") { moonAngleDeg = 315; nextAngleDeg = 360; }

  // ===== ORBIT DIAGRAM — full height, no bottom panel =====
  const int ox = 200, oy = 158, orb = 100;
  for (int a = 0; a < moonAngleDeg; a++) {
    float r1 = a * 3.14159f / 180.0f;
    float r2 = (a+1) * 3.14159f / 180.0f;
    int x1 = ox + (int)((orb-5) * cosf(r1)), y1 = oy + (int)((orb-5) * sinf(r1));
    int x2 = ox + (int)((orb-5) * cosf(r2)), y2 = oy + (int)((orb-5) * sinf(r2));
    canvas.drawLine(x1, y1, x2, y2, 1);
    x1 = ox + (int)((orb-6) * cosf(r1)); y1 = oy + (int)((orb-6) * sinf(r1));
    x2 = ox + (int)((orb-6) * cosf(r2)); y2 = oy + (int)((orb-6) * sinf(r2));
    canvas.drawLine(x1, y1, x2, y2, 1);
    x1 = ox + (int)((orb-7) * cosf(r1)); y1 = oy + (int)((orb-7) * sinf(r1));
    x2 = ox + (int)((orb-7) * cosf(r2)); y2 = oy + (int)((orb-7) * sinf(r2));
    canvas.drawLine(x1, y1, x2, y2, 1);
  }

  // Orbit circle
  canvas.drawCircle(ox, oy, orb, 1);

  // Earth at centre
  canvas.fillCircle(ox, oy, 8, 1);
  canvas.fillCircle(ox, oy, 4, 0);
  canvas.setTextColor(0); canvas.setFont(&FONT_SMALL);
  canvas.setCursor(ox-4, oy+4); canvas.print("E");
  canvas.setTextColor(1);

  // Phase markers and labels — all outside the orbit
  struct { int angle; const char* lbl; const char* sub; } markers[] = {
    {   0, "New",  "Moon"  },
    {  45, "Wax",  "Cresc" },
    {  90, "1st",  "Qtr"   },
    { 135, "Wax",  "Gibb"  },
    { 180, "Full", "Moon"  },
    { 225, "Wan",  "Gibb"  },
    { 270, "Last", "Qtr"   },
    { 315, "Wan",  "Cresc" },
  };

  for (int i = 0; i < 8; i++) {
    float rad = markers[i].angle * 3.14159f / 180.0f;
    int ir = orb - 5, or2 = orb + 5;
    canvas.drawLine(ox+(int)(ir*cosf(rad)), oy+(int)(ir*sinf(rad)),
                    ox+(int)(or2*cosf(rad)), oy+(int)(or2*sinf(rad)), 1);

    int lx, ly;

    if (markers[i].angle == 270) {
      // Last Qtr — top of orbit, place below dot to avoid header
      lx = ox - 17;
      ly = oy - orb + 22;  // cap top=71, dot edge=62, gap=9px
    } else if (markers[i].angle == 90) {
      // 1st Qtr — bottom of orbit, place above dot to avoid page dots
      lx = ox - 8;
      ly = oy + orb - 42;  // row2 bot=233, dot edge=262, gap=29px
    } else {
      float lr = orb + 28;  // push labels well clear of moon icon (r=9)
      lx = ox + (int)(lr * cosf(rad));
      ly = oy + (int)(lr * sinf(rad));
      bool isLeft = markers[i].angle > 90 && markers[i].angle < 270;
      bool isTop  = markers[i].angle > 180 && markers[i].angle < 360;
      if (isLeft) lx -= 38; else lx -= 8;
      if (isTop)  ly -= 18; else ly -= 2;
    }

    canvas.setFont(&FONT_SMALL);
    canvas.setCursor(lx, ly);    canvas.print(markers[i].lbl);
    canvas.setCursor(lx, ly+16); canvas.print(markers[i].sub);
    // Days until next phase on the upcoming phase marker
    // nextAngleDeg can be 360 (Waning Crescent→New Moon) — clamp to 360%360=0 correctly matches angle 0
    if (markers[i].angle == (nextAngleDeg % 360)) {
      canvas.setCursor(lx, ly+32); canvas.print("in "); canvas.print(daysUntilNext); canvas.print("d");
    }
  }

  // Moon position on orbit
  float moonRad = moonAngleDeg * 3.14159f / 180.0f;
  int mx = ox + (int)(orb * cosf(moonRad));
  int my = oy + (int)(orb * sinf(moonRad));

  // Draw moon phase icon
  drawMoonIcon(mx, my, 9, phase);

  // Fixed labels flanking Earth icon — ILLUM left, AGE right, stable at all moon positions
  // Left side: shifted further left to give clear gap from Earth edge (ox-8)
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(ox - 72, oy - 6);  canvas.print("ILLUM");
  canvas.setFont(&FONT_MEDIUM);
  canvas.setCursor(ox - 72, oy + 14); canvas.print(illum); canvas.print("%");

  // Right side: left-edge of text starts ~10px right of Earth edge (ox+8), so anchor at ox+18
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(ox + 18, oy - 6);  canvas.print("AGE");
  canvas.setFont(&FONT_MEDIUM);
  canvas.setCursor(ox + 18, oy + 14); canvas.print(age); canvas.print("d");

  pushCanvasToRLCD(false);
}

// ===== PAGE 8: SUN POSITION =====
void drawSunArcPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1); canvas.drawRect(1, 1, W-2, H-2, 1);
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(12, 25); canvas.print("SUN POSITION  "); canvas.print(weatherLocation);

  if (!astroData.valid) {
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(80, 160); canvas.print("NO ASTRONOMY DATA");
    pushCanvasToRLCD(false); return;
  }

  const int cx = 200, cy = 200, r = 145, arcMarginX = 28;
  canvas.fillRect(arcMarginX, cy, W - arcMarginX*2, 2, 1);
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(arcMarginX, cy - 6); canvas.print("E");
  canvas.setCursor(W - arcMarginX - 8, cy - 6); canvas.print("W");

  for (int deg = 0; deg <= 180; deg++) {
    float rad = deg * 3.14159f / 180.0f;
    int x = cx - (int)(r * cosf(rad)), y = cy - (int)(r * sinf(rad));
    if (x >= 0 && x < W && y >= 0 && y < H) canvas.drawPixel(x, y, 1);
  }

  int noonX = cx;
  for (int y = cy - r; y < cy; y += 5) canvas.drawFastVLine(noonX, y, 3, 1);
  canvas.setFont(&FONT_SMALL); canvas.setCursor(noonX - 18, cy - r - 6); canvas.print("NOON");

  int sunriseMins = parseTimeToMinutes(astroData.sunrise);
  int sunsetMins  = parseTimeToMinutes(astroData.sunset);
  int nowMins     = hour24 * 60 + minuteVal;
  int dayLen      = sunsetMins - sunriseMins;
  if (dayLen <= 0) dayLen = 1;  // guard against bad API data

  // Sunrise/sunset times — 10px below horizon line so they don't touch it
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(arcMarginX + 2, cy + 20); canvas.print(astroData.sunrise);
  canvas.setCursor(W - arcMarginX - 52, cy + 20); canvas.print(astroData.sunset);

  bool isDaytime = (nowMins >= sunriseMins && nowMins <= sunsetMins);

  if (isDaytime) {
    float sunAngleDeg = (float)(nowMins - sunriseMins) / (float)dayLen * 180.0f;
    float sunRad = sunAngleDeg * 3.14159f / 180.0f;
    int sunX = cx - (int)(r * cosf(sunRad)), sunY = cy - (int)(r * sinf(sunRad));
    canvas.fillCircle(sunX, sunY, 9, 1); canvas.fillCircle(sunX, sunY, 5, 0);
    for (int i = 0; i < 8; i++) {
      float a = i * 3.14159f / 4.0f;
      canvas.drawLine(sunX+(int)(11*cosf(a)), sunY+(int)(11*sinf(a)), sunX+(int)(15*cosf(a)), sunY+(int)(15*sinf(a)), 1);
    }
  } else {
    int nightX = (nowMins < sunriseMins) ? cx - r : cx + r;
    canvas.drawCircle(nightX, cy + 12, 8, 1);
    canvas.setFont(&FONT_SMALL); canvas.setCursor(cx - 20, cy + 26); canvas.print("NIGHT");
  }

  // Divider and bottom info panel — pushed down to give clear space from arc times
  canvas.fillRect(8, 230, 384, 2, 1);
  int dlH = dayLen / 60, dlM = dayLen % 60, noonMins = sunriseMins + dayLen / 2;
  String countdownLabel, countdownVal;
  if (isDaytime) {
    int ml = sunsetMins - nowMins; countdownLabel = "SUNSET IN"; countdownVal = String(ml/60) + "h " + String(ml%60) + "m";
  } else if (nowMins < sunriseMins) {
    int ml = sunriseMins - nowMins; countdownLabel = "SUNRISE IN"; countdownVal = String(ml/60) + "h " + String(ml%60) + "m";
  } else {
    int ml = (sunriseMins + 1440) - nowMins; countdownLabel = "SUNRISE IN"; countdownVal = String(ml/60) + "h " + String(ml%60) + "m";
  }

  int col1 = 14, col2 = 148, col3 = 282;
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(col1, 250); canvas.print("DAY LENGTH");
  canvas.setCursor(col2, 250); canvas.print("SOLAR NOON");
  canvas.setCursor(col3, 250); canvas.print(countdownLabel);
  canvas.setFont(&FONT_MEDIUM);
  canvas.setCursor(col1, 270); canvas.print(dlH); canvas.print("h "); canvas.print(dlM); canvas.print("m");
  canvas.setCursor(col2, 270); canvas.print(minutesToTimeStr(noonMins));
  canvas.setCursor(col3, 270); canvas.print(countdownVal);
  canvas.fillRect(140, 230, 1, 42, 1); canvas.fillRect(274, 230, 1, 42, 1);
  pushCanvasToRLCD(false);
}

// ===== PAGE 13: SYSTEM INFO =====
void drawSystemPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1); canvas.drawRect(1, 1, W-2, H-2, 1);

  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(12, 24); canvas.print("SYSTEM INFO");

  // 5 rows at 44px each — label at y+12, value at y+32, 18px gap between them
  const int lx = 14, rx = 204, rowH = 44, gy0 = 36;
  canvas.fillRect(198, gy0, 2, rowH * 5, 1);

  auto drawDetail = [&](int x, int y, const char* lbl, String val, bool useSmall = false) {
    canvas.setFont(&FONT_SMALL);  canvas.setCursor(x, y+12); canvas.print(lbl);
    canvas.setFont(useSmall ? &FONT_SMALL : &FONT_MEDIUM); canvas.setCursor(x, y+32); canvas.print(val);
  };

  // Build value strings
  String wifiVal = wifiConnected ? String(wifiRSSI) + " dBm" : "Offline";
  String ipStr   = wifiConnected ? WiFi.localIP().toString() : "Not connected";
  String wxVal   = "No data";
  if (weatherData.valid) {
    unsigned long nf = 1800000UL - min((unsigned long)1800000UL, millis() - weatherData.lastUpdate);
    wxVal = String(nf/60000) + "m " + String((nf%60000)/1000) + "s";
  }
  unsigned long upSec = millis() / 1000;
  char uptimeStr[12];
  sprintf(uptimeStr, "%02d:%02d:%02d", (int)(upSec/3600), (int)((upSec%3600)/60), (int)(upSec%60));
  String ntpStr = "Never";
  if (ntpLastSync > 0) {
    unsigned long syncAgo = (millis() - ntpLastSync) / 60000;
    ntpStr = syncAgo < 60 ? String(syncAgo) + " min ago" : String(syncAgo/60) + " hr ago";
  }
  String ssidStr = wifiConnected ? String(ssid) : "--";
  if (ssidStr.length() > 20) ssidStr = ssidStr.substring(0, 20); // FONT_SMALL safe up to ~26 chars
  String chanStr  = wifiConnected ? String(WiFi.channel()) : "--";
  int total = sensorReadCount + sensorFailCount;
  String readsStr = total > 0 ? String(sensorReadCount) + "/" + String(total) + " ok" : "No reads";

  int gy = gy0;
  drawDetail(lx, gy, "WIFI",         wifiVal);
  drawDetail(rx, gy, "IP ADDRESS",   ipStr);       gy += rowH;
  drawDetail(lx, gy, "WEATHER",      weatherData.valid ? "Fresh" : "No Data");
  drawDetail(rx, gy, "WX REFRESH",   wxVal);        gy += rowH;
  drawDetail(lx, gy, "UPTIME",       String(uptimeStr));
  drawDetail(rx, gy, "NTP SYNC",     ntpStr);       gy += rowH;
  drawDetail(lx, gy, "NETWORK",      ssidStr, true);
  drawDetail(rx, gy, "CHANNEL",      chanStr);      gy += rowH;
  drawDetail(lx, gy, "SENSOR",       (sensorFailCount == 0) ? "OK" : "Errors");
  drawDetail(rx, gy, "SENSOR READS", readsStr);

  canvas.fillRect(8, 264, 384, 2, 1);
  pushCanvasToRLCD(false);
}

// ===== PAGE 4: HOURLY FORECAST ===== (note: function defined here, called from draw())
void drawHourlyPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1); canvas.drawRect(1, 1, W-2, H-2, 1);
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(12, 24); canvas.print("HOURLY FORECAST  "); canvas.print(weatherLocation);

  if (!hourlyData.valid) {
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(80, 150); canvas.print("NO HOURLY DATA");
    pushCanvasToRLCD(false); return;
  }

  const int cols = 6, colW = 62, colGap = 2, startX = 11;
  const int topY = 36, hdrH = 24, rowH = 38, totalH = hdrH + rowH * 5;
  const int yTime = topY, yTemp = yTime+hdrH, yRainPc = yTemp+rowH, yRainMM = yRainPc+rowH, yUV = yRainMM+rowH, yWind = yUV+rowH;

  for (int i = 0; i < cols; i++) {
    int cx = startX + i * (colW + colGap);
    canvas.drawRect(cx, topY, colW, totalH, 1);
    canvas.fillRect(cx+1, yTime+1, colW-2, hdrH-2, 1);
    canvas.setTextColor(0); canvas.setFont(&FONT_SMALL);
    canvas.setCursor(cx+5, yTime+17); canvas.print(hourlyData.time[i]);
    canvas.setTextColor(1); canvas.setFont(&FONT_SMALL);
    canvas.setCursor(cx+4, yTemp+13);   canvas.print("TEMP");
    canvas.setCursor(cx+4, yTemp+30);   canvas.print((int)hourlyData.temp[i]); canvas.print("c");
    canvas.setCursor(cx+4, yRainPc+13); canvas.print("RAIN");
    canvas.setCursor(cx+4, yRainPc+30); canvas.print(hourlyData.rainChance[i]); canvas.print("%");
    canvas.setCursor(cx+4, yRainMM+13); canvas.print("MM");
    canvas.setCursor(cx+4, yRainMM+30); canvas.print(hourlyData.rainMM[i], 1);
    canvas.setCursor(cx+4, yUV+13);     canvas.print("UV");
    canvas.setCursor(cx+4, yUV+30);     canvas.print(hourlyData.uvIndex[i], 1);
    canvas.setCursor(cx+4, yWind+13);   canvas.print("WIND");
    canvas.setCursor(cx+4, yWind+30);   canvas.print((int)hourlyData.windSpeed[i]); canvas.print("k");
  }

  // No divider line — footer sits cleanly below the grid
  canvas.setFont(&FONT_SMALL); canvas.setCursor(12, 285);
  canvas.print("Upd ");
  if (weatherData.valid) { canvas.print((millis() - weatherData.lastUpdate) / 60000); canvas.print("m ago"); }
  else { canvas.print("--"); }
  pushCanvasToRLCD(false);
}

// ===== GRAPH HELPERS =====
GraphBounds calcGraphBounds(float* data, int count, float minRange, float pad) {
  GraphBounds b = { 999.0f, -999.0f, 0.0f };
  for (int i = 0; i < count; i++) { if (data[i] < b.mn) b.mn = data[i]; if (data[i] > b.mx) b.mx = data[i]; }
  b.rng = b.mx - b.mn;
  if (b.rng < minRange) b.rng = minRange;
  b.mn -= pad; b.mx += pad; b.rng = b.mx - b.mn;
  return b;
}

int calcTrend(float* data, int si, int pts) {
  if (pts < 3) return 0;
  float delta = data[(si + pts - 1) % HISTORY_SIZE] - data[(si + pts - 3) % HISTORY_SIZE];
  if (delta > 0.5f) return 1; if (delta < -0.5f) return -1; return 0;
}

void drawTrendArrow(int x, int y, int trend) {
  if (trend == 1)       { canvas.fillTriangle(x, y-8, x-5, y, x+5, y, 1); canvas.fillRect(x-2, y, 4, 5, 1); }
  else if (trend == -1) { canvas.fillTriangle(x, y+8, x-5, y, x+5, y, 1); canvas.fillRect(x-2, y-5, 4, 5, 1); }
  else                  { canvas.fillRect(x-6, y-3, 12, 2, 1); canvas.fillRect(x-6, y+1, 12, 2, 1); }
}

void drawEnhancedGraph(float* data, int si, int pts, int gX, int gY, int gW, int gH, GraphBounds b, const char* unit) {
  canvas.drawRect(gX-1, gY-1, gW+2, gH+2, 1);
  for (int i = 1; i <= 3; i++) canvas.drawLine(gX, gY+(i*gH/4), gX+gW, gY+(i*gH/4), 1);
  for (int i = 0; i <= 4; i++) canvas.drawLine(gX+(i*gW/4), gY, gX+(i*gW/4), gY+gH, 1);

  int minIdx = -1, maxIdx = -1; float minVal = 9999.0f, maxVal = -9999.0f;
  for (int i = 0; i < pts; i++) {
    float v = data[(si+i) % HISTORY_SIZE];
    if (v < minVal) { minVal = v; minIdx = i; }
    if (v > maxVal) { maxVal = v; maxIdx = i; }
  }
  for (int i = 0; i < pts-1; i++) {
    int i1 = (si+i) % HISTORY_SIZE, i2 = (si+i+1) % HISTORY_SIZE;
    int x1 = gX+(i*gW/(HISTORY_SIZE-1)), x2 = gX+((i+1)*gW/(HISTORY_SIZE-1));
    int y1 = gY+gH-(int)((data[i1]-b.mn)/b.rng*gH), y2 = gY+gH-(int)((data[i2]-b.mn)/b.rng*gH);
    canvas.drawLine(x1, y1, x2, y2, 1); canvas.drawLine(x1, y1-1, x2, y2-1, 1);
  }
  if (minIdx >= 0) {
    int mx = gX+(minIdx*gW/(HISTORY_SIZE-1)), my = gY+gH-(int)((minVal-b.mn)/b.rng*gH);
    canvas.fillTriangle(mx, my+2, mx-4, my-5, mx+4, my-5, 1);
    canvas.setFont(&FONT_SMALL); canvas.setCursor(constrain(mx-8,gX,gX+gW-20), my+14); canvas.print(minVal, 1);
  }
  if (maxIdx >= 0) {
    int mx = gX+(maxIdx*gW/(HISTORY_SIZE-1)), my = gY+gH-(int)((maxVal-b.mn)/b.rng*gH);
    canvas.fillTriangle(mx, my-2, mx-4, my+5, mx+4, my+5, 1);
    canvas.setFont(&FONT_SMALL); canvas.setCursor(constrain(mx-8,gX,gX+gW-20), my-5); canvas.print(maxVal, 1);
  }
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(gX-38, gY+5);      canvas.print(b.mx, 0); canvas.print(unit);
  canvas.setCursor(gX-38, gY+gH/2+4); canvas.print(((b.mx+b.mn)/2.0f), 0); canvas.print(unit);
  canvas.setCursor(gX-38, gY+gH-2);   canvas.print(b.mn, 0); canvas.print(unit);
  const char* xLabels[] = { "6h", "4.5h", "3h", "1.5h", "0h" };
  for (int i = 0; i <= 4; i++) { int lx = gX+(i*gW/4)-(i==4?12:6); canvas.setCursor(lx, gY+gH+14); canvas.print(xLabels[i]); }
}

// ===== PAGE 11: TEMP GRAPH =====
void drawTempGraphPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1); canvas.drawRect(1, 1, W-2, H-2, 1);
  drawThermometerIcon(16, 14);
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(38, 22); canvas.print("INDOOR TEMP");
  canvas.setCursor(38, 38); canvas.print("6 HOUR HISTORY");
  // Current value — FONT_LARGE fits header box without clipping
  canvas.setFont(&FONT_LARGE); canvas.setCursor(220, 38); canvas.print(temperature, 1);
  canvas.setFont(&FONT_SMALL); canvas.print(" C");
  int tTrend = calcTrend(history.tempHistory, history.currentIndex, history.sampleCount);
  drawTrendArrow(375, 22, tTrend);

  if (history.sampleCount < 2) {
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(85, 155); canvas.print("COLLECTING DATA");
    canvas.setFont(&FONT_SMALL);  canvas.setCursor(95, 178); canvas.print("Graph available in 15 mins");
    pushCanvasToRLCD(false); return;
  }

  int gX = 44, gY = 52, gW = 336, gH = 168;
  GraphBounds b = calcGraphBounds(history.tempHistory, HISTORY_SIZE, 5.0f, 1.0f);
  drawEnhancedGraph(history.tempHistory, history.currentIndex, min((int)history.sampleCount,(int)HISTORY_SIZE), gX, gY, gW, gH, b, "c");

  // Bottom stats bar — verified column positions, min 7px between all items
  canvas.drawRect(8, 244, 384, 24, 1);
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(8,   261); canvas.print("0h:");
  canvas.setCursor(39,  261); canvas.print(temperature, 1); canvas.print("c");
  canvas.setCursor(90,  261); canvas.print("MIN:");
  canvas.setCursor(134, 261); canvas.print(b.mn+1.0f, 1); canvas.print("c");
  canvas.setCursor(200, 261); canvas.print("MAX:");
  canvas.setCursor(250, 261); canvas.print(b.mx-1.0f, 1); canvas.print("c");
  canvas.setCursor(308, 261);
  if (tTrend == 1) canvas.print("RISING"); else if (tTrend == -1) canvas.print("FALLING"); else canvas.print("STABLE");
  pushCanvasToRLCD(false);
}

// ===== PAGE 12: HUMIDITY GRAPH =====
void drawHumidityGraphPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1); canvas.drawRect(1, 1, W-2, H-2, 1);
  drawDropletIcon(16, 14);
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(38, 22); canvas.print("INDOOR HUMIDITY");
  canvas.setCursor(38, 38); canvas.print("6 HOUR HISTORY");
  // Current value — FONT_LARGE fits header box without clipping
  canvas.setFont(&FONT_LARGE); canvas.setCursor(220, 38); canvas.print((int)humidity);
  canvas.setFont(&FONT_SMALL); canvas.print(" %");
  int hTrend = calcTrend(history.humidityHistory, history.currentIndex, history.sampleCount);
  drawTrendArrow(375, 22, hTrend);

  if (history.sampleCount < 2) {
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(85, 155); canvas.print("COLLECTING DATA");
    canvas.setFont(&FONT_SMALL);  canvas.setCursor(95, 178); canvas.print("Graph available in 15 mins");
    pushCanvasToRLCD(false); return;
  }

  int gX = 44, gY = 52, gW = 336, gH = 168;
  GraphBounds b = calcGraphBounds(history.humidityHistory, HISTORY_SIZE, 10.0f, 2.0f);
  if (b.mn < 0.0f) b.mn = 0.0f; if (b.mx > 100.0f) b.mx = 100.0f;
  b.rng = b.mx - b.mn; if (b.rng < 1.0f) b.rng = 1.0f;
  drawEnhancedGraph(history.humidityHistory, history.currentIndex, min((int)history.sampleCount,(int)HISTORY_SIZE), gX, gY, gW, gH, b, "%");

  // Bottom stats bar — verified column positions, min 7px between all items
  canvas.drawRect(8, 244, 384, 24, 1);
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(8,   261); canvas.print("0h:");
  canvas.setCursor(39,  261); canvas.print((int)humidity); canvas.print("%");
  canvas.setCursor(90,  261); canvas.print("MIN:");
  canvas.setCursor(134, 261); canvas.print(b.mn+2.0f, 1); canvas.print("%");
  canvas.setCursor(200, 261); canvas.print("MAX:");
  canvas.setCursor(250, 261); canvas.print(b.mx-2.0f, 1); canvas.print("%");
  canvas.setCursor(308, 261);
  if (hTrend == 1) canvas.print("RISING"); else if (hTrend == -1) canvas.print("FALLING"); else canvas.print("STABLE");
  pushCanvasToRLCD(false);
}

// ===== SEASONS HELPER =====
// Returns day of year (1-365)
int dayOfYear(int day, int month, int year) {
  const int dpm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
  int doy = 0;
  for (int m = 0; m < month - 1; m++) doy += dpm[m];
  if (leap && month > 2) doy++;
  return doy + day;
}

// Approximate solstice/equinox day-of-year for a given year
// Returns {marchEq, juneSol, septEq, decSol}
SeasonEvents calcSeasonEvents(int year) {
  // Simplified approximation — accurate to within 1-2 days
  SeasonEvents e;
  float y = year + 0.5f;
  e.marchEq = (int)(79.3125f + 0.2422f * (y - 2000) - (int)((y - 2000) / 4.0f));
  e.juneSol = (int)(171.3125f + 0.2422f * (y - 2000) - (int)((y - 2000) / 4.0f));
  e.septEq  = (int)(264.3125f + 0.2422f * (y - 2000) - (int)((y - 2000) / 4.0f));
  e.decSol  = (int)(354.3125f + 0.2422f * (y - 2000) - (int)((y - 2000) / 4.0f));
  return e;
}

// Southern Hemisphere season from day of year
SeasonInfo getSeasonInfo(int doy, int year) {
  SeasonEvents e = calcSeasonEvents(year);
  SeasonInfo s;
  if (doy >= e.decSol || doy < e.marchEq) {
    int since = doy >= e.decSol ? doy - e.decSol : doy + (365 - e.decSol);
    int until = doy >= e.decSol ? (e.marchEq + 365 - doy) : (e.marchEq - doy);
    s.name = "SUMMER"; s.daysSince = since; s.daysUntil = until;
    s.nextEvent = "Autumn Equinox"; s.nextEventDoy = e.marchEq;
  } else if (doy >= e.marchEq && doy < e.juneSol) {
    s.name = "AUTUMN"; s.daysSince = doy - e.marchEq; s.daysUntil = e.juneSol - doy;
    s.nextEvent = "Winter Solstice"; s.nextEventDoy = e.juneSol;
  } else if (doy >= e.juneSol && doy < e.septEq) {
    s.name = "WINTER"; s.daysSince = doy - e.juneSol; s.daysUntil = e.septEq - doy;
    s.nextEvent = "Spring Equinox"; s.nextEventDoy = e.septEq;
  } else {
    s.name = "SPRING"; s.daysSince = doy - e.septEq; s.daysUntil = e.decSol - doy;
    s.nextEvent = "Summer Solstice"; s.nextEventDoy = e.decSol;
  }
  return s;
}

// Convert day-of-year to date string "DD Mon"
String doyToDateStr(int doy, int year) {
  int dpm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
  if (leap) dpm[1] = 29;
  int m = 0;
  int days = doy;
  while (m < 12 && days > dpm[m]) { days -= dpm[m]; m++; }
  char buf[10];
  snprintf(buf, sizeof(buf), "%d %s", days, months[m]);
  return String(buf);
}

// ===== PAGE 9: EARTH & SEASONS =====
void drawSeasonsPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1);
  canvas.drawRect(1, 1, W-2, H-2, 1);
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(12, 24); canvas.print("EARTH & SEASONS  "); canvas.print(weatherLocation);

  int day   = rtc.getDay();
  int month = rtc.getMonth();
  int year  = rtc.getYear();
  int doy   = dayOfYear(day, month, year);
  SeasonEvents e = calcSeasonEvents(year);
  SeasonInfo   s = getSeasonInfo(doy, year);

  // ===== ORBIT DIAGRAM — full height, no bottom panel =====
  // Centre at y=158, orx=130, ory=100 — fits within border with clearance for outside labels
  const int ox = 200, oy = 158, orx = 130, ory = 100;

  // Orbit ellipse
  for (int deg = 0; deg < 360; deg += 1) {
    float rad = deg * 3.14159f / 180.0f;
    int x = ox + (int)(orx * cosf(rad));
    int y = oy + (int)(ory * sinf(rad));
    if (x >= 0 && x < W && y >= 0 && y < H) canvas.drawPixel(x, y, 1);
  }

  // Sun at centre
  canvas.fillCircle(ox, oy, 7, 1);
  canvas.fillCircle(ox, oy, 3, 0);
  for (int i = 0; i < 8; i++) {
    float a = i * 3.14159f / 4.0f;
    canvas.drawLine(ox+(int)(9*cosf(a)), oy+(int)(9*sinf(a)),
                    ox+(int)(13*cosf(a)), oy+(int)(13*sinf(a)), 1);
  }
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(0);
  canvas.setCursor(ox-4, oy+4); canvas.print("S");
  canvas.setTextColor(1);

  // Season markers — EQU/SOL + date, mathematically positioned clear of orbit and borders
  struct { int mdoy; const char* lbl; float angle; } markers[] = {
    { e.marchEq, "EQU", 0.0f   },
    { e.juneSol, "SOL", 270.0f },
    { e.septEq,  "EQU", 180.0f },
    { e.decSol,  "SOL", 90.0f  },
  };

  for (int i = 0; i < 4; i++) {
    float rad = markers[i].angle * 3.14159f / 180.0f;
    int mx = ox + (int)(orx * cosf(rad));
    int my = oy + (int)(ory * sinf(rad));
    canvas.fillCircle(mx, my, 4, 1);
    int tx, ty;
    // Right (MAR EQ) — both rows above dot, 8px clear of orbit
    if (markers[i].angle == 0.0f)   { tx = mx + 10; ty = my - 25; }
    // Left (SEP EQ) — pushed fully left of orbit; '22 Sep'=57px wide, mx=70, need tx<5
    if (markers[i].angle == 180.0f) { tx = mx - 68; ty = my - 25; }
    // Top (JUN SOL) — below dot inside ellipse, pushed down clear of orbit
    if (markers[i].angle == 270.0f) { tx = mx - 16; ty = my + 22; }
    // Bottom (DEC SOL) — both rows above dot, 9px clear of orbit
    if (markers[i].angle == 90.0f)  { tx = mx - 16; ty = my - 26; }
    canvas.setFont(&FONT_SMALL);
    canvas.setCursor(tx, ty);    canvas.print(markers[i].lbl);
    canvas.setCursor(tx, ty+14); canvas.print(doyToDateStr(markers[i].mdoy, year));
  }

  // Season names in the 4 corners — fixed positions outside the ellipse
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(14,  52);  canvas.print("WINTER");  // top-left  (JUN SOL side)
  canvas.setCursor(316, 52);  canvas.print("AUTUMN");  // top-right (MAR EQ side)
  canvas.setCursor(14,  272); canvas.print("SPRING");  // bot-left  (SEP EQ side)
  canvas.setCursor(316, 272); canvas.print("SUMMER");  // bot-right (DEC SOL side)

  // Earth position
  float earthAngle;
  if      (doy >= e.decSol)                  earthAngle = 90.0f  - (float)(doy - e.decSol)    / (float)(e.marchEq + 365 - e.decSol) * 90.0f;
  else if (doy < e.marchEq)                  earthAngle = 90.0f  - (float)(doy + 365 - e.decSol) / (float)(e.marchEq + 365 - e.decSol) * 90.0f;
  else if (doy >= e.marchEq && doy < e.juneSol) earthAngle = 360.0f - (float)(doy - e.marchEq) / (float)(e.juneSol - e.marchEq) * 90.0f;
  else if (doy >= e.juneSol && doy < e.septEq)  earthAngle = 270.0f - (float)(doy - e.juneSol) / (float)(e.septEq  - e.juneSol) * 90.0f;
  else                                           earthAngle = 180.0f - (float)(doy - e.septEq)  / (float)(e.decSol  - e.septEq)  * 90.0f;

  float erad = earthAngle * 3.14159f / 180.0f;
  int ex = ox + (int)(orx * cosf(erad));
  int ey = oy + (int)(ory * sinf(erad));
  canvas.fillCircle(ex, ey, 6, 1);
  canvas.fillCircle(ex, ey, 3, 0);
  int elx = ex + (ex > ox ? 9 : -28);
  int ely = ey + (ey > oy ? 14 : -6);
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(elx, ely);    canvas.print("NOW");
  canvas.setCursor(elx, ely+12); canvas.print(s.daysUntil); canvas.print("d");

  pushCanvasToRLCD(false);
}
// ===== PAGE 10: SEASONS ORBIT DIAGRAM =====
void drawSeasonsOrbitPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1);
  canvas.drawRect(1, 1, W-2, H-2, 1);

  int day_  = rtc.getDay();
  int mon_  = rtc.getMonth();
  int year_ = rtc.getYear();
  int doy   = dayOfYear(day_, mon_, year_);
  SeasonEvents e = calcSeasonEvents(year_);
  SeasonInfo   s = getSeasonInfo(doy, year_);

  // Days until each event
  int dJun = e.juneSol - doy; if (dJun <= 0) dJun += 365;
  int dSep = e.septEq  - doy; if (dSep <= 0) dSep += 365;
  int dDec = e.decSol  - doy; if (dDec <= 0) dDec += 365;
  int dMar = e.marchEq - doy; if (dMar <= 0) dMar += 365;
  int minDays = dJun;
  if (dSep < minDays) minDays = dSep;
  if (dDec < minDays) minDays = dDec;
  if (dMar < minDays) minDays = dMar;

  // ── Header ───────────────────────────────────────────────────────────────
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(12, 24);
  canvas.print("EARTH & SEASONS "); canvas.print(weatherLocation);

  String evtStr = String(s.nextEvent);
  int sp = evtStr.indexOf(' ');
  if (sp > 0) evtStr = evtStr.substring(sp + 1);
  evtStr.replace("Equinox",  "EQ");
  evtStr.replace("Solstice", "SOL");
  String evtFull = evtStr + " " + doyToDateStr(s.nextEventDoy, year_);
  int16_t ex1, ey1; uint16_t etw, eth;
  canvas.getTextBounds(evtFull.c_str(), 0, 0, &ex1, &ey1, &etw, &eth);
  canvas.setCursor(388 - (int)etw, 24);
  canvas.print(evtFull);

  // ── Corner countdown labels ───────────────────────────────────────────────
  canvas.setFont(&FONT_SMALL);
  int cDays[4]  = { dSep, dJun, dDec, dMar }; // TL=SEP EQU, TR=JUN SOL, BL=DEC SOL, BR=MAR EQU
  int cX[4]     = { 10,   390,  10,   390  };
  int cY[4]     = { 52,   52,   261,  261  };
  bool cRight[4]= { false,true, false,true  };
  for (int i = 0; i < 4; i++) {
    String lbl = (cDays[i] == 0) ? String("TODAY") : String(cDays[i]) + "d";
    int16_t bx1, by1; uint16_t btw, bth;
    canvas.getTextBounds(lbl.c_str(), 0, 0, &bx1, &by1, &btw, &bth);
    int tx = cRight[i] ? cX[i] - (int)btw - 4 : cX[i] + 2;
    canvas.setCursor(tx, cY[i]);
    canvas.print(lbl);
    if (cDays[i] == minDays) {
      canvas.drawRect(tx - 4, cY[i] - 16, (int)btw + 8, 23, 1);
    }
  }

  // ── Orbit geometry ────────────────────────────────────────────────────────
  const int ocx = 200, ocy = 158, orx = 130, ory = 100;

  // ── Quadrant fills (scanline) ─────────────────────────────────────────────
  for (int fy = 0; fy < H; fy++) {
    float fdy = fy - ocy;
    float disc = 1.0f - (fdy * fdy) / (float)(ory * ory);
    if (disc < 0.0f) continue;
    float fex = orx * sqrtf(disc);
    for (int fx = ocx - (int)fex; fx <= ocx + (int)fex; fx++) {
      float fdx = fx - ocx;
      float fangle = atan2f(fdy, fdx) * 180.0f / 3.14159f;
      if (fangle < 0.0f) fangle += 360.0f;
      // All 4 quadrants = entire ellipse interior
      canvas.drawPixel(fx, fy, 1);
    }
  }

  // ── Quadrant dividing lines (crosshairs) drawn white to split fills ───────
  // Horizontal line (SEP EQ to MAR EQ)
  for (int fx = ocx - orx + 2; fx <= ocx + orx - 2; fx++)
    canvas.drawPixel(fx, ocy, 0);
  // Vertical line (JUN SOL to DEC SOL)
  for (int fy = ocy - ory + 2; fy <= ocy + ory - 2; fy++)
    canvas.drawPixel(ocx, fy, 0);

  // ── Dashed white crosshairs over the solid white lines ────────────────────
  for (int fx = ocx - orx + 2; fx < ocx + orx - 2; fx += 9) {
    for (int k = 0; k < 5 && fx+k < ocx+orx-2; k++)
      canvas.drawPixel(fx+k, ocy, 0);
  }
  for (int fy = ocy - ory + 2; fy < ocy + ory - 2; fy += 9) {
    for (int k = 0; k < 5 && fy+k < ocy+ory-2; k++)
      canvas.drawPixel(ocx, fy+k, 0);
  }

  // ── Ellipse outline: white gap then black ─────────────────────────────────
  for (int deg = 0; deg < 360; deg++) {
    float rd = deg * 3.14159f / 180.0f;
    canvas.drawPixel(ocx + (int)(orx * cosf(rd)), ocy + (int)(ory * sinf(rd)), 0);
  }
  for (int deg = 0; deg < 360; deg++) {
    float rd = deg * 3.14159f / 180.0f;
    canvas.drawPixel(ocx + (int)(orx * cosf(rd)), ocy + (int)(ory * sinf(rd)), 1);
  }

  // ── Earth angle ───────────────────────────────────────────────────────────
  float earthAngle;
  if      (doy >= e.decSol)                        earthAngle = 90.0f  - (float)(doy - e.decSol)       / (float)(e.marchEq + 365 - e.decSol) * 90.0f;
  else if (doy < e.marchEq)                        earthAngle = 90.0f  - (float)(doy + 365 - e.decSol) / (float)(e.marchEq + 365 - e.decSol) * 90.0f;
  else if (doy >= e.marchEq && doy < e.juneSol)    earthAngle = 360.0f - (float)(doy - e.marchEq)      / (float)(e.juneSol - e.marchEq)      * 90.0f;
  else if (doy >= e.juneSol && doy < e.septEq)     earthAngle = 270.0f - (float)(doy - e.juneSol)      / (float)(e.septEq  - e.juneSol)      * 90.0f;
  else                                              earthAngle = 180.0f - (float)(doy - e.septEq)       / (float)(e.decSol  - e.septEq)       * 90.0f;

  // ── Progress arc (white, inside orbit at 88%) ─────────────────────────────
  float nextAngle;
  if      (dMar == minDays) nextAngle = 0.0f;
  else if (dDec == minDays) nextAngle = 90.0f;
  else if (dSep == minDays) nextAngle = 180.0f;
  else                      nextAngle = 270.0f;

  float arcRX = orx * 0.88f, arcRY = ory * 0.88f;
  float sweepEnd = nextAngle;
  if (sweepEnd > earthAngle) sweepEnd -= 360.0f;
  for (float aa = earthAngle; aa >= sweepEnd; aa -= 0.5f) {
    float rd = aa * 3.14159f / 180.0f;
    int apx = ocx + (int)(arcRX * cosf(rd));
    int apy = ocy + (int)(arcRY * sinf(rd));
    canvas.drawPixel(apx,   apy,   0);
    canvas.drawPixel(apx,   apy-1, 0);
    canvas.drawPixel(apx,   apy+1, 0);
  }

  // ── Sun at centre ─────────────────────────────────────────────────────────
  canvas.fillCircle(ocx, ocy, 16, 0);
  canvas.fillCircle(ocx, ocy, 9,  1);
  canvas.fillCircle(ocx, ocy, 5,  0);
  for (int i = 0; i < 8; i++) {
    float ra = i * 3.14159f / 4.0f;
    canvas.drawLine(ocx+(int)(11*cosf(ra)), ocy+(int)(11*sinf(ra)),
                    ocx+(int)(15*cosf(ra)), ocy+(int)(15*sinf(ra)), 1);
  }
  canvas.setTextColor(0); canvas.setCursor(ocx-4, ocy+4); canvas.print("S");
  canvas.setTextColor(1);

  // ── Season names in white ─────────────────────────────────────────────────
  const char* sNames[] = { "SUMMER", "SPRING", "WINTER", "AUTUMN" };
  float       sAngles[] = { 45.0f, 135.0f, 225.0f, 315.0f };
  canvas.setFont(&FONT_SMALL);
  for (int i = 0; i < 4; i++) {
    float rd = sAngles[i] * 3.14159f / 180.0f;
    int snx = ocx + (int)(orx * 0.56f * cosf(rd));
    int sny = ocy + (int)(ory * 0.56f * sinf(rd));
    int16_t sx1, sy1; uint16_t stw, sth;
    canvas.getTextBounds(sNames[i], 0, 0, &sx1, &sy1, &stw, &sth);
    canvas.setTextColor(0);
    canvas.setCursor(snx - (int)stw/2, sny + 4);
    canvas.print(sNames[i]);
    canvas.setTextColor(1);
  }

  // ── Month ticks + labels ──────────────────────────────────────────────────
  const char* months[] = {"Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec","Jan","Feb"};
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  for (int i = 0; i < 12; i++) {
    float mrad = -i * 30.0f * 3.14159f / 180.0f;
    int mix = ocx + (int)(orx * cosf(mrad));
    int miy = ocy + (int)(ory * sinf(mrad));
    int mtx = ocx + (int)((orx+9) * cosf(mrad));
    int mty = ocy + (int)((ory+9) * sinf(mrad));
    canvas.drawLine(mix, miy, mtx, mty, 1);
    float mlr  = (i == 0 || i == 6) ? orx + 30.0f : orx + 20.0f;
    float mlry = (i == 0 || i == 6) ? ory + 30.0f : ory + 20.0f;
    int mlx = ocx + (int)(mlr  * cosf(mrad));
    int mly = ocy + (int)(mlry * sinf(mrad));
    int16_t mx1, my1; uint16_t mtw, mth;
    canvas.getTextBounds(months[i], 0, 0, &mx1, &my1, &mtw, &mth);
    canvas.setCursor(mlx - (int)mtw/2, mly + 4);
    canvas.print(months[i]);
  }

  // ── Bullseye markers at SOL/EQ points ────────────────────────────────────
  int bullA[] = {0, 90, 180, 270};
  for (int i = 0; i < 4; i++) {
    float rd = bullA[i] * 3.14159f / 180.0f;
    int bpx = ocx + (int)(orx * cosf(rd));
    int bpy = ocy + (int)(ory * sinf(rd));
    canvas.fillCircle(bpx, bpy, 8, 0);
    canvas.drawCircle(bpx, bpy, 6, 1);
    canvas.drawCircle(bpx, bpy, 5, 1);
    canvas.fillCircle(bpx, bpy, 4, 0);
    canvas.fillCircle(bpx, bpy, 2, 1);
  }

  // ── Earth dot — flashing ─────────────────────────────────────────────────
  unsigned long nowMs = millis();
  if (nowMs - earthFlashLast >= 600) {
    earthFlashOn   = !earthFlashOn;
    earthFlashLast = nowMs;
  }
  if (earthFlashOn) {
    float erd = earthAngle * 3.14159f / 180.0f;
    int epx = ocx + (int)(orx * cosf(erd));
    int epy = ocy + (int)(ory * sinf(erd));
    canvas.fillCircle(epx, epy, 8, 0);
    canvas.drawCircle(epx, epy, 8, 1);
    canvas.fillCircle(epx, epy, 5, 1);
    canvas.fillCircle(epx, epy, 3, 0);
  }

  pushCanvasToRLCD(false);
}

void draw() {
  switch (currentPage) {
    case 0:  drawDashboardPage();      break;
    case 1:  drawAnalogClockPage();    break;
    case 2:  drawTimeZonePage();       break;
    case 3:  drawCurrentWeatherPage(); break;
    case 4:  drawHourlyPage();         break;
    case 5:  drawForecastPage();       break;
    case 6:  drawAstronomyPage();      break;
    case 7:  drawLunarOrbitPage();     break;
    case 8:  drawSunArcPage();         break;
    case 9:  drawSeasonsPage();        break;
    case 10: drawSeasonsOrbitPage();   break;
    case 11: drawTempGraphPage();      break;
    case 12: drawHumidityGraphPage();  break;
    case 13: drawSystemPage();         break;
  }
}

// ===== WIFI & NTP =====
void connectWiFi() {
  Serial.print("Connecting: "); Serial.println(ssid);
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) { delay(500); Serial.print("."); attempts++; }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK"); Serial.println(WiFi.localIP());
    wifiConnected = true; wifiRSSI = WiFi.RSSI();
  } else { Serial.println("\nWiFi failed"); wifiConnected = false; }
}

void syncRTCWithNTP() {
  if (!wifiConnected) return;
  // Use POSIX TZ string — ESP32 handles DST transitions automatically.
  // Pass 0,0 for gmtOffset/daylightOffset; the TZ string contains all the rules.
  configTime(0, 0, ntpServer);
  setenv("TZ", posixTZ, 1);
  tzset();

  struct tm timeinfo; int retries = 0;
  while (!getLocalTime(&timeinfo) && retries < 10) { delay(500); retries++; }
  if (getLocalTime(&timeinfo)) {
    // Push correct local time (already DST-adjusted by the TZ rule) to RTC
    rtc.setTime(timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    rtc.setDate(timeinfo.tm_wday, timeinfo.tm_mday, timeinfo.tm_mon+1, timeinfo.tm_year+1900);
    ntpLastSync = millis();

    // Derive current UTC offset by comparing local epoch to UTC directly.
    // Use gmtime_r to get UTC fields, then compute the difference in minutes
    // by comparing hours/minutes — avoids mktime() double-conversion issue on newlib.
    time_t localEpoch = time(nullptr);
    struct tm utcCheck;
    gmtime_r(&localEpoch, &utcCheck);
    int localMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int utcMinutes   = utcCheck.tm_hour  * 60 + utcCheck.tm_min;
    int diffMins     = localMinutes - utcMinutes;
    // Handle day boundary wrap
    if (diffMins >  720) diffMins -= 1440;
    if (diffMins < -720) diffMins += 1440;
    gmtOffset_sec = (long)(diffMins * 60);

    Serial.print("RTC synced! UTC offset: ");
    Serial.print(gmtOffset_sec / 3600);
    Serial.print("h, DST: ");
    Serial.println(timeinfo.tm_isdst ? "YES" : "NO");
  } else Serial.println("NTP failed");
}

void handleButtons() {
  unsigned long now = millis();
  if (now - lastButtonPress < debounceDelay) return;
  if (digitalRead(BTN_LEFT) == LOW && !btn_left_pressed) {
    btn_left_pressed = true; lastButtonPress = now;
    currentPage = (currentPage + 1) % totalPages;
  } else if (digitalRead(BTN_LEFT) == HIGH) btn_left_pressed = false;

  if (digitalRead(BTN_MIDDLE) == LOW && !btn_middle_pressed) {
    btn_middle_pressed = true; lastButtonPress = now;
    if (wifiConnected) fetchWeatherData();
  } else if (digitalRead(BTN_MIDDLE) == HIGH) btn_middle_pressed = false;
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200); delay(200);
  Serial.println("\n=== ESP32-S3 RLCD ===");

  pinMode(BTN_LEFT,   INPUT_PULLUP);
  pinMode(BTN_MIDDLE, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);

  Wire.begin(13, 14);

  RlcdPort.RLCD_Init();
  rtc.begin();

  // ===== BOOT SCREEN =====
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1);
  canvas.drawRect(1, 1, W-2, H-2, 1);
  canvas.setTextColor(1);

  // Build combined "Sydney AEDT" string — NTP not synced yet at this point,
  // so show the configured POSIX TZ standard name as a placeholder
  // getTZLabel() will return correct DST-aware label once NTP syncs
  char locTzBuf[32];
  snprintf(locTzBuf, sizeof(locTzBuf), "%s %s", weatherLocation, getTZLabel());

  printCentered(&FONT_MEDIUM, 36,  "ESP32-S3 RLCD");
  printCentered(&FONT_MEDIUM, 58,  "Weather Dashboard");
  printCentered(&FONT_SMALL,  78,  "by John Willie Gee");
  printCentered(&FONT_MEDIUM, 108, locTzBuf);

  pushCanvasToRLCD(false);
  delay(600);

  // ===== BOOT STATUS LINES =====
  int bootLine = 0;
  auto drawBoot = [&](const char* msg, int state) {
    int ly = 136 + bootLine * 24;
    canvas.fillRect(14, ly-14, 372, 18, 0);
    canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
    canvas.setCursor(16, ly); canvas.print(msg);
    if      (state ==  1) { canvas.setCursor(358, ly); canvas.print("OK"); }
    else if (state == -1) { canvas.setCursor(358, ly); canvas.print("--"); }
    pushCanvasToRLCD(false);
    bootLine++;
  };

  weatherData.valid = false; weatherData.lastUpdate = 0;
  astroData.valid   = false; hourlyData.valid        = false;

  drawBoot("Connecting WiFi...", 0); connectWiFi(); bootLine--;
  drawBoot(wifiConnected ? "WiFi connected" : "WiFi failed", wifiConnected ? 1 : -1);

  if (wifiConnected) {
    drawBoot("Syncing RTC...", 0); syncRTCWithNTP(); bootLine--;
    drawBoot("RTC synced", 1);
    drawBoot("Fetching weather...", 0); fetchWeatherData(); bootLine--;
    drawBoot(weatherData.valid ? "Weather loaded" : "Weather failed", weatherData.valid ? 1 : -1);
  }

  drawBoot("Reading sensors...", 0);
  if (shtc3_read(temperature, humidity)) { sensorReadCount++; bootLine--; drawBoot("Sensors OK", 1); }
  else { sensorFailCount++; bootLine--; drawBoot("Sensor error", -1); }
  batteryVoltage = readBatteryVoltage();

  delay(1800);

  history.initialized  = false;
  history.currentIndex = 0;
  history.sampleCount  = 0;
  history.lastLogTime  = millis();
  for (int i = 0; i < HISTORY_SIZE; i++) {
    history.tempHistory[i]     = temperature;
    history.humidityHistory[i] = humidity;
  }

  Serial.println("Ready!");
  delay(500);
}

// ===== LOOP =====
void loop() {
  handleButtons();
  // Read all RTC values in one pass to minimise I2C transactions
  hour24    = rtc.getHour();
  minuteVal = rtc.getMinute();
  // secondVal needed for dashboard (page 0) and analogue clock (page 1) second hands
  if (currentPage == 0 || currentPage == 1) secondVal = rtc.getSecond();

  if (sensorUpdateCounter == 0) {
    batteryVoltage = readBatteryVoltage();
    if (shtc3_read(temperature, humidity)) {
      sensorReadCount++;
      Serial.printf("T=%.1fC RH=%.1f%% V=%.2f", temperature, humidity, batteryVoltage);
    } else { sensorFailCount++; Serial.print("Sensor read failed"); }
    if (wifiConnected) { wifiRSSI = WiFi.RSSI(); Serial.printf(" WiFi=%d", wifiRSSI); }
    Serial.println();
  }
  if (++sensorUpdateCounter >= 600) sensorUpdateCounter = 0;

  unsigned long now = millis();
  if (now - history.lastLogTime >= 900000UL) {
    history.tempHistory[history.currentIndex]     = temperature;
    history.humidityHistory[history.currentIndex] = humidity;
    history.currentIndex = (history.currentIndex + 1) % HISTORY_SIZE;
    history.lastLogTime  = now;
    if (history.sampleCount < HISTORY_SIZE) history.sampleCount++;
    history.initialized = (history.sampleCount >= HISTORY_SIZE);
  }

  if (wifiConnected && weatherData.valid && (now - weatherData.lastUpdate) > 1800000UL)
    fetchWeatherData();

  // Re-sync RTC with NTP every 24 hours
  if (wifiConnected && (ntpLastSync == 0 || (now - ntpLastSync) > 86400000UL))
    syncRTCWithNTP();

  draw();
  delay(16);
}
