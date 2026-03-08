# Wavelink RLCD ESP32-S3 Weather Dashboard

A data-rich, 12-page time and weather dashboard built on an **ESP32-S3** microcontroller, driving a **400×300 monochrome reflective LCD (RLCD)**. Combines live weather API data, on-board environmental sensing, RTC timekeeping, and original astronomical diagram designs — all rendered in crisp 1-bit monochrome.

---

## ✨ Features

- **12 display pages** cycled via a hardware button
- **Live weather data** from WeatherAPI.com — current conditions, 3-day forecast, 6-hour hourly breakdown, air quality
- **Indoor environment** — temperature and humidity from an SHTC3 sensor with 6-hour history graphs
- **Astronomy data** — sunrise/sunset, moon phase, lunar orbit diagram, sun arc, and seasons orbit
- **DSEG7 digital clock** on the dashboard page, driven by PCF85063A RTC with periodic NTP sync
- **Battery voltage monitoring** via ADC
- **Pixel-accurate browser preview** (`preview.html`) for layout verification before flashing — no guesswork

https://youtu.be/wd1mQkY0oWk


---

## 📟 Display Pages

| Page | Name | Description |
|------|------|-------------|
| 0 | **Dashboard** | Live DSEG7 clock, indoor temp & humidity, date, battery, Wi-Fi status |
| 1 | **Current Conditions** | Outdoor temp, feels like, condition, high/low, UV, wind, air quality, rainfall |
| 2 | **Hourly Forecast** | 6-hour grid — temp, rain chance, rainfall, UV and wind per hour |
| 3 | **3-Day Forecast** | Three day cards with high/low, condition, rain amount and rain chance |
| 4 | **Astronomy** | Sunrise/sunset times, day length, solar noon, moon phase, illumination, next phase |
| 5 | **Lunar Orbit** | Orbit diagram with phase markers, cycle progress arc, illumination %, age, days to next phase |
| 6 | **Sun Arc** | Sun position arc with current position, day length, solar noon, sunrise/sunset countdown |
| 7 | **Seasons (Classic)** | Clean ellipse orbit with Earth position, season names in corners, EQU/SOL labels and dates |
| 8 | **Seasons (Orbit)** | NOAA-inspired filled quadrant diagram with bullseye markers, flashing Earth dot, progress arc, corner countdowns |
| 9 | **Temp Graph** | 6-hour indoor temperature history with min/max markers, trend arrow, stats bar |
| 10 | **Humidity Graph** | 6-hour indoor humidity history with min/max markers, trend arrow, stats bar |
| 11 | **System Info** | Wi-Fi signal, IP address, weather refresh timer, uptime, NTP sync status, sensor health |

---

## 🔧 Hardware

| Component | Detail |
|-----------|--------|
| MCU | ESP32-S3 |
| Display | 400×300 px, 1-bit monochrome RLCD (landscape), SPI |
| Temp/Humidity | SHTC3 sensor, I2C |
| RTC | PCF85063A, I2C |
| Buttons | BTN_LEFT (GPIO 0) — next page · BTN_MIDDLE (GPIO 18) — refresh weather |
| Battery ADC | GPIO 4 |

This project was developed on the **Waveshare ESP32-S3 RLCD 4.2"** development board. Refer to the Waveshare documentation for pinout and wiring details.
https://docs.waveshare.com/ESP32-S3-RLCD-4.2

---

## 🗂️ Project Files

| File | Description |
|------|-------------|
| `main.cpp` | Full firmware source — all 12 pages, helper functions, WiFi/NTP/sensor/weather logic |
| `secrets.h` | Credentials template — WiFi SSID/password, WeatherAPI key, location (**keep out of version control**) |
| `preview.html` | Pixel-accurate browser preview using embedded Adafruit GFX bitmap font data |
| `display_bsp.cpp / .h` | Low-level RLCD display driver (GFXcanvas1 → pushCanvasToRLCD) |
| `font.h` | DSEG7 Classic Bold 84pt (dashboard clock) |
| `secfont.h` | DSEG7 Classic Bold 36pt (dashboard seconds) |
| `FreeSans9pt7b.h` | Adafruit GFX bitmap font — 9pt |
| `FreeSans12pt7b.h` | Adafruit GFX bitmap font — 12pt |

> ⚠️ `secrets.h` contains your credentials. It is included in `.gitignore` by default — **never commit it to a public repository.**

---

## 🚀 Getting Started

### 1. Arduino IDE Setup

Before flashing, set up the Arduino IDE for the ESP32-S3 and Waveshare board by following the official guides:

- [Waveshare ESP32-S3 RLCD 4.2" Development Environment Setup](https://docs.waveshare.com/ESP32-S3-RLCD-4.2/Development-Environment-Setup-Arduino)
- [Waveshare Arduino IDE Setup Guide](https://docs.waveshare.com/ESP32-Arduino-Tutorials/Arduino-IDE-Setup)

### 2. Required Libraries

Install the following libraries via the Arduino Library Manager:

| Library | Purpose |
|---------|---------|
| **Adafruit GFX Library** | Drawing primitives and bitmap font rendering |
| **PCF85063A-Soldered** | RTC hardware driver |
| **ArduinoJson** *(optional)* | Not required — a custom JSON parser is used |

> ESP32-S3 board support is installed via the Espressif Arduino core as part of the Waveshare setup guides above.

### 3. Configure Credentials

Copy `secrets.h` into your project folder and fill in your details:

```cpp
#define WIFI_SSID      "your_wifi_network"
#define WIFI_PASSWORD  "your_wifi_password"
#define WEATHER_API_KEY "your_weatherapi_key"
#define WEATHER_LOCATION "your_city_or_coordinates"
```

### 4. Weather API

Weather data is fetched from [WeatherAPI.com](https://www.weatherapi.com/).

- Create a **free account** at weatherapi.com
- Copy your API key into `secrets.h`
- The free tier supports current conditions, forecast, hourly, astronomy, and air quality in a single HTTPS call — no paid plan required for this project

### 5. Flash the Board

Open `main.cpp` in Arduino IDE, select the correct board and port, and upload. The display will initialise, connect to Wi-Fi, sync the RTC via NTP, and fetch weather data on first boot.

---

## 🌐 Data & Timing

- Weather data refreshes every **30 minutes** automatically, or on demand via BTN_MIDDLE
- NTP time sync occurs on boot and every **24 hours**
- Indoor sensor history is sampled every **15 minutes**, storing a 6-hour rolling buffer (24 samples)
- **Southern Hemisphere seasons** are used throughout (Summer = Dec–Mar, Winter = Jun–Sep)

---

## 🖥️ Browser Preview

`preview.html` is a pixel-accurate browser-based preview of the display output. It uses the embedded Adafruit GFX bitmap font data to render text identically to the hardware. Open it in any modern browser to check layout changes before flashing.

---

## 🔮 Potential Future Enhancements

- Bluetooth or web interface for configuration (location, timezone, units)
- Alerts for severe weather conditions
- Additional sensor support (pressure, CO2, PM2.5)
- OTA firmware updates over Wi-Fi
- SD card logging for extended sensor history
- Portrait orientation variant

---

## 📄 License

This project is open source. See [LICENSE](LICENSE) for details.

---

## 🙏 Acknowledgements

- [Waveshare](https://www.waveshare.com/) for the ESP32-S3 RLCD hardware and documentation
- [Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library) for the display rendering framework
- [WeatherAPI.com](https://www.weatherapi.com/) for the weather data API
- [DSEG Font](https://www.keshikan.net/fonts-e.html) for the digital clock typeface
