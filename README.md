# Wavelink-RLCD-ESP32S3-Weather-dashboard
A multi page time and weather dashboard that uses both on board sensors and weather API data to create a data rich 13 page dashboard. RTC set via NTP with times zones display , on board sensor data plus trending, weather API data dna forecast, luna cycles and orbit, sun and seasons data plus orbit graphics and finally a systems page read out
Coded in Arduino IDE
To setup Arduino and ensure you flash the board correcly you should review the Waveshare gudies 
https://docs.waveshare.com/ESP32-S3-RLCD-4.2/Development-Environment-Setup-Arduino
https://docs.waveshare.com/ESP32-Arduino-Tutorials/Arduino-IDE-Setup
In addtion to the inlcuded files you will need some libraries installed into Arduino IDE for this code to complie
ESP32S3 support in order to flash to the board, this should have been setup if you followed the guide links above
Adafruit-GXF for the drawinngs and font
PCF85063A-Soldered for the RTC
Weather API and JSON parsing is Setup for https://www.weatherapi.com/ you will need to setup a free account and copy your API
Update the Secrets.h file for your WIFI, Weather API and city location. 
