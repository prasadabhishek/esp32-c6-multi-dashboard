#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <time.h>

#include "airports_db.h"

// Wi-Fi Credentials
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// Default Fallback Location: Belltown, Seattle, WA
double current_lat = 47.6148;
double current_lon = -122.3458;
String current_location_name = "BELLTOWN, SEATTLE";
bool location_found = false;

// Display Pin Definitions for Waveshare ESP32-C6 LCD 1.47"
#define TFT_BL   23
#define TFT_SCLK 7
#define TFT_MOSI 6
#define TFT_CS   14
#define TFT_DC   15
#define TFT_RST  22

#define SCREEN_W 172
#define SCREEN_H 320

// Display Driver (ST7789 172x320 requires col_offset1=34 and col_offset2=34)
Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, GFX_NOT_DEFINED);
Arduino_GFX *display = new Arduino_ST7789(bus, TFT_RST, 0 /* rotation */, true /* IPS */, SCREEN_W, SCREEN_H, 34, 0, 34, 0);

// Double Buffer Canvas in RAM (172x320 x 16-bit = 110KB)
Arduino_Canvas *canvas = new Arduino_Canvas(SCREEN_W, SCREEN_H, display, 0, 0, 0);

// Screen Rotation State Machine
enum DisplayScreen {
  SCREEN_FLIGHT = 0,
  SCREEN_WEATHER,
  SCREEN_CLOCK,
  SCREEN_SUN_MOON,
  SCREEN_SYSINFO,
  NUM_SCREENS
};
DisplayScreen current_screen = SCREEN_CLOCK;
uint32_t last_screen_switch = 0;
const uint32_t ROTATION_INTERVAL_MS = 7000; // Rotate every 7 seconds when quiet

// Flight Data Structure
struct FlightInfo {
  bool active;
  String callsign;
  String airline;
  String origin;
  String destination;
  String origin_city;
  String dest_city;
  double lat;
  double lon;
  int altitude_ft;
  int speed_kts;
  int heading_deg;
  double distance_km;
  uint32_t last_update;
} current_flight;

// Weather Data Structure (via Open-Meteo Free API)
struct WeatherInfo {
  bool valid;
  float temp_f;
  int humidity;
  int weather_code;
  float wind_mph;
  String condition_str;
  uint32_t last_update;
} current_weather;

// Helper: Calculate distance in KM using Haversine formula
double calculateDistance(double lat1, double lon1, double lat2, double lon2) {
  double dLat = (lat2 - lat1) * M_PI / 180.0;
  double dLon = (lon2 - lon1) * M_PI / 180.0;
  double a = sin(dLat / 2) * sin(dLat / 2) +
             cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) *
             sin(dLon / 2) * sin(dLon / 2);
  double c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return 6371.0 * c;
}

String getAirlineName(const String& callsign) {
  if (callsign.length() < 3) return "PRIVATE / GA";
  String prefix = callsign.substring(0, 3);
  prefix.toUpperCase();

  if (prefix == "DAL") return "DELTA AIR LINES";
  if (prefix == "ASA") return "ALASKA AIRLINES";
  if (prefix == "ACA") return "AIR CANADA";
  if (prefix == "UAL") return "UNITED AIRLINES";
  if (prefix == "AAL") return "AMERICAN AIRLINES";
  if (prefix == "SWA") return "SOUTHWEST AIRLINES";
  if (prefix == "SKW") return "SKYWEST AIRLINES";
  if (prefix == "QXE") return "HORIZON AIR";
  if (prefix == "FDX") return "FEDEX EXPRESS";
  if (prefix == "UPS") return "UPS AIRLINES";
  if (prefix == "JBU") return "JETBLUE AIRWAYS";
  if (prefix == "HAL") return "HAWAIIAN AIRLINES";
  if (prefix == "WJA") return "WESTJET";
  if (prefix == "BAW") return "BRITISH AIRWAYS";

  if (callsign.startsWith("N") && isdigit(callsign.charAt(1))) {
    return "PRIVATE AIRCRAFT";
  }

  return "COMMERCIAL AIRLINE";
}

String getIataCode(const String& rawCode) {
  String c = rawCode;
  c.trim();
  c.toUpperCase();
  if (c.length() == 4) {
    if (c.startsWith("K") || c.startsWith("C") || c.startsWith("P")) return c.substring(1);
  }
  return c;
}

String getCityName(const String& rawCode) {
  String city = findGlobalCityName(rawCode);
  if (city.length() > 0) return city;
  String iata = getIataCode(rawCode);
  if (iata.length() > 0) return iata;
  return "AIRPORT";
}

// Convert WMO Weather Code to String
String getWeatherConditionStr(int code) {
  if (code == 0) return "CLEAR SKY";
  if (code >= 1 && code <= 3) return "PARTLY CLOUDY";
  if (code == 45 || code == 48) return "FOGGY";
  if (code >= 51 && code <= 55) return "LIGHT DRIZZLE";
  if (code >= 61 && code <= 65) return "RAIN SHOWERS";
  if (code >= 71 && code <= 77) return "SNOW SHOWERS";
  if (code >= 80 && code <= 82) return "HEAVY RAIN";
  if (code >= 95) return "THUNDERSTORM";
  return "PARTLY CLOUDY";
}

// IP Geolocation API lookup
void updateLocation() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin("http://ip-api.com/json/");
  http.setTimeout(4000);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      current_lat = doc["lat"] | 47.6148;
      current_lon = doc["lon"] | -122.3458;
      String city = doc["city"] | "Seattle";
      String region = doc["region"] | "WA";
      city.toUpperCase();
      region.toUpperCase();
      current_location_name = city + ", " + region;
      location_found = true;
      Serial.printf("GeoLocation Success: %s (%.4f, %.4f)\n", current_location_name.c_str(), current_lat, current_lon);
    }
  }
  http.end();

  if (!location_found) {
    current_lat = 47.6148;
    current_lon = -122.3458;
    current_location_name = "BELLTOWN, SEATTLE";
  }
}

// Fetch Weather Data via Open-Meteo Free API
void fetchWeatherData() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(current_lat, 4) +
               "&longitude=" + String(current_lon, 4) +
               "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m&temperature_unit=fahrenheit&wind_speed_unit=mph";
  http.begin(url);
  http.setTimeout(4000);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload) && doc["current"].is<JsonObject>()) {
      JsonObject cur = doc["current"].as<JsonObject>();
      current_weather.temp_f = cur["temperature_2m"].as<float>();
      current_weather.humidity = cur["relative_humidity_2m"].as<int>();
      current_weather.weather_code = cur["weather_code"].as<int>();
      current_weather.wind_mph = cur["wind_speed_10m"].as<float>();
      current_weather.condition_str = getWeatherConditionStr(current_weather.weather_code);
      current_weather.valid = true;
      current_weather.last_update = millis();
      Serial.printf("Weather Success: %.1f F, %s, Wind: %.1f mph\n", 
                    current_weather.temp_f, current_weather.condition_str.c_str(), current_weather.wind_mph);
    }
  }
  http.end();
}

// Route lookup via HexDB
void updateRouteInfo(FlightInfo& flight) {
  if (WiFi.status() != WL_CONNECTED || flight.callsign.length() == 0) return;
  HTTPClient http;
  String url = "https://hexdb.io/api/v1/route/icao/" + flight.callsign;
  http.begin(url);
  http.setTimeout(3000);
  int httpCode = http.GET();
  bool routeFound = false;

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload) && doc["route"].is<String>()) {
      String routeStr = doc["route"].as<String>();
      int dashPos = routeStr.indexOf('-');
      if (dashPos > 0) {
        String origRaw = routeStr.substring(0, dashPos);
        String destRaw = "";
        int nextDash = routeStr.indexOf('-', dashPos + 1);
        if (nextDash > 0) {
          destRaw = routeStr.substring(dashPos + 1, nextDash);
        } else {
          destRaw = routeStr.substring(dashPos + 1);
        }
        flight.origin = getIataCode(origRaw);
        flight.destination = getIataCode(destRaw);
        flight.origin_city = getCityName(origRaw);
        flight.dest_city = getCityName(destRaw);
        routeFound = true;
      }
    }
  }
  http.end();

  if (!routeFound || flight.origin.length() == 0) {
    flight.origin = "LOC";
    flight.destination = "ENR";
    flight.origin_city = "LOCAL";
    flight.dest_city = "EN ROUTE";
  }
}

// Query Overhead Flights via OpenSky Network API
void fetchOverheadFlights() {
  if (WiFi.status() != WL_CONNECTED) return;

  double delta_lat = 0.20;
  double delta_lon = 0.25;
  double lamin = current_lat - delta_lat;
  double lamax = current_lat + delta_lat;
  double lomin = current_lon - delta_lon;
  double lomax = current_lon + delta_lon;

  String url = "https://opensky-network.org/api/states/all?lamin=" + String(lamin, 4) +
               "&lamax=" + String(lamax, 4) +
               "&lomin=" + String(lomin, 4) +
               "&lomax=" + String(lomax, 4);

  HTTPClient http;
  http.begin(url);
  http.setTimeout(5000);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err && doc["states"].is<JsonArray>()) {
      JsonArray states = doc["states"].as<JsonArray>();
      double closest_dist = 99999.0;
      int closest_idx = -1;

      for (size_t i = 0; i < states.size(); i++) {
        JsonArray s = states[i].as<JsonArray>();
        if (s.size() >= 11 && !s[5].isNull() && !s[6].isNull()) {
          double f_lon = s[5].as<double>();
          double f_lat = s[6].as<double>();
          double dist = calculateDistance(current_lat, current_lon, f_lat, f_lon);
          if (dist < closest_dist) {
            closest_dist = dist;
            closest_idx = i;
          }
        }
      }

      if (closest_idx >= 0) {
        JsonArray s = states[closest_idx].as<JsonArray>();
        String cs = s[1].as<String>();
        cs.trim();
        if (cs.length() == 0) cs = "FLIGHT";

        current_flight.active = true;
        current_flight.callsign = cs;
        current_flight.airline = getAirlineName(cs);
        current_flight.lon = s[5].as<double>();
        current_flight.lat = s[6].as<double>();

        double alt_m = s[7].isNull() ? 0.0 : s[7].as<double>();
        current_flight.altitude_ft = (int)(alt_m * 3.28084);

        double speed_ms = s[9].isNull() ? 0.0 : s[9].as<double>();
        current_flight.speed_kts = (int)(speed_ms * 1.94384);

        current_flight.heading_deg = s[10].isNull() ? 0 : s[10].as<int>();
        current_flight.distance_km = closest_dist;
        current_flight.last_update = millis();

        updateRouteInfo(current_flight);
        Serial.printf("Overhead Flight Found: %s (%s) %s -> %s Dist: %.1f km\n", 
                      cs.c_str(), current_flight.airline.c_str(), 
                      current_flight.origin.c_str(), current_flight.destination.c_str(),
                      closest_dist);
        http.end();
        return;
      }
    }
  }
  http.end();
  current_flight.active = false;
}

// -------------------------------------------------------------
// RENDERERS FOR EACH SCREEN VIEW
// -------------------------------------------------------------

// 1. FLIGHT TRACKER SCREEN
void renderFlightScreen() {
  canvas->fillScreen(0x0821);
  uint16_t c_cyan = 0x07FF, c_amber = 0xFBE0, c_green = 0x07E0, c_white = 0xFFFF, c_gray = 0x7BEF, c_dark = 0x18E3, c_pink = 0xF810;

  canvas->fillRect(0, 0, SCREEN_W, 30, 0x10A2);
  canvas->drawFastHLine(0, 30, SCREEN_W, c_cyan);
  static bool pulse = false; pulse = !pulse;
  canvas->fillCircle(12, 15, 4, current_flight.active ? (pulse ? c_green : 0x03E0) : c_amber);

  canvas->setTextColor(c_white); canvas->setTextSize(1); canvas->setCursor(24, 11);
  canvas->print(current_flight.active ? "LIVE OVERHEAD" : "AIRSPACE SCANNER");
  canvas->setTextColor(c_gray); canvas->setCursor(125, 11); canvas->print("ESP-C6");

  if (!current_flight.active) {
    int cx = SCREEN_W / 2, cy = 160, r = 60;
    canvas->drawCircle(cx, cy, r, c_dark);
    canvas->drawCircle(cx, cy, r * 2 / 3, c_dark);
    canvas->drawCircle(cx, cy, r / 3, c_dark);
    canvas->drawFastHLine(cx - r - 10, cy, (r + 10) * 2, c_dark);
    canvas->drawFastVLine(cx, cy - r - 10, (r + 10) * 2, c_dark);

    static int sweep_angle = 0; sweep_angle = (sweep_angle + 15) % 360;
    double rad = sweep_angle * M_PI / 180.0;
    canvas->drawLine(cx, cy, cx + (int)(r * cos(rad)), cy + (int)(r * sin(rad)), c_cyan);
    canvas->fillCircle(cx, cy, 3, c_cyan);

    canvas->setTextColor(c_cyan); canvas->setTextSize(1); canvas->setCursor(20, 245); canvas->print("NO FLIGHT OVERHEAD");
    canvas->setTextColor(c_gray); canvas->setCursor(18, 260); canvas->print("MONITORING AIRSPACE...");
  } else {
    canvas->drawRoundRect(6, 36, SCREEN_W - 12, 48, 4, c_cyan);
    canvas->fillRoundRect(7, 37, SCREEN_W - 14, 46, 4, 0x0166);
    canvas->setTextColor(c_amber); canvas->setTextSize(1); canvas->setCursor(14, 42); canvas->print("CALLSIGN");
    canvas->setTextColor(c_white); canvas->setTextSize(3); canvas->setCursor(14, 54); canvas->print(current_flight.callsign);

    canvas->setTextColor(c_cyan); canvas->setTextSize(1); canvas->setCursor(10, 92); canvas->print(current_flight.airline);

    canvas->drawRoundRect(6, 108, SCREEN_W - 12, 64, 4, c_dark);
    canvas->fillRoundRect(7, 109, SCREEN_W - 14, 62, 4, 0x1084);

    canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(12, 114); canvas->print("FROM");
    canvas->setTextColor(c_amber); canvas->setTextSize(2); canvas->setCursor(12, 126); canvas->print(current_flight.origin);
    canvas->setTextColor(c_white); canvas->setTextSize(1); canvas->setCursor(12, 148); canvas->print(current_flight.origin_city.substring(0, 10));

    canvas->setTextColor(c_pink); canvas->setTextSize(2); canvas->setCursor(72, 126); canvas->print(">>");

    canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(94, 114); canvas->print("TO");
    canvas->setTextColor(c_green); canvas->setTextSize(2); canvas->setCursor(94, 126); canvas->print(current_flight.destination);
    canvas->setTextColor(c_white); canvas->setTextSize(1); canvas->setCursor(94, 148); canvas->print(current_flight.dest_city.substring(0, 12));

    canvas->drawRoundRect(6, 178, 76, 52, 4, c_dark);
    canvas->fillRoundRect(7, 179, 74, 50, 4, 0x0963);
    canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(12, 184); canvas->print("ALTITUDE");
    canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(12, 198); canvas->printf("%d", current_flight.altitude_ft);
    canvas->setTextColor(c_cyan); canvas->setTextSize(1); canvas->setCursor(12, 216); canvas->print("FT");

    canvas->drawRoundRect(90, 178, 76, 52, 4, c_dark);
    canvas->fillRoundRect(91, 179, 74, 50, 4, 0x0963);
    canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(96, 184); canvas->print("SPEED");
    canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(96, 198); canvas->printf("%d", current_flight.speed_kts);
    canvas->setTextColor(c_cyan); canvas->setTextSize(1); canvas->setCursor(96, 216); canvas->print("KTS");

    canvas->drawRoundRect(6, 236, SCREEN_W - 12, 54, 4, c_dark);
    canvas->fillRoundRect(7, 237, SCREEN_W - 14, 52, 4, 0x08A2);
    int rx = 32, ry = 263;
    canvas->drawCircle(rx, ry, 16, c_cyan); canvas->drawCircle(rx, ry, 8, c_dark);
    double h_rad = current_flight.heading_deg * M_PI / 180.0;
    canvas->fillCircle(rx + (int)(12 * sin(h_rad)), ry - (int)(12 * cos(h_rad)), 3, c_pink);

    canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(56, 244); canvas->print("DISTANCE");
    canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(56, 256); canvas->printf("%.1f KM", current_flight.distance_km);
    canvas->setTextColor(c_amber); canvas->setTextSize(1); canvas->setCursor(56, 274); canvas->printf("HDG: %d* DIR", current_flight.heading_deg);
  }

  canvas->drawFastHLine(0, 296, SCREEN_W, c_dark);
  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(10, 304); canvas->print("LOC:");
  canvas->setTextColor(c_cyan); canvas->setCursor(38, 304); canvas->print(current_location_name);
}

// 2. LOCAL WEATHER SCREEN
void renderWeatherScreen() {
  canvas->fillScreen(0x0821);
  uint16_t c_cyan = 0x07FF, c_amber = 0xFBE0, c_green = 0x07E0, c_white = 0xFFFF, c_gray = 0x7BEF, c_dark = 0x18E3;

  canvas->fillRect(0, 0, SCREEN_W, 30, 0x10A2);
  canvas->drawFastHLine(0, 30, SCREEN_W, c_amber);
  canvas->fillCircle(12, 15, 4, c_amber);

  canvas->setTextColor(c_white); canvas->setTextSize(1); canvas->setCursor(24, 11); canvas->print("LOCAL WEATHER");
  canvas->setTextColor(c_gray); canvas->setCursor(125, 11); canvas->print("OPEN-METEO");

  // Main Temperature Banner
  canvas->drawRoundRect(6, 38, SCREEN_W - 12, 70, 6, c_amber);
  canvas->fillRoundRect(7, 39, SCREEN_W - 14, 68, 6, 0x2120);

  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(14, 46); canvas->print("TEMPERATURE");
  canvas->setTextColor(c_white); canvas->setTextSize(4); canvas->setCursor(14, 62);
  if (current_weather.valid) {
    canvas->printf("%.0f*", current_weather.temp_f);
    canvas->setTextSize(2); canvas->setTextColor(c_amber); canvas->print("F");
  } else {
    canvas->print("--*");
  }

  // Condition Badge
  canvas->drawRoundRect(6, 116, SCREEN_W - 12, 44, 4, c_dark);
  canvas->fillRoundRect(7, 117, SCREEN_W - 14, 42, 4, 0x1084);
  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(14, 122); canvas->print("CONDITION");
  canvas->setTextColor(c_cyan); canvas->setTextSize(2); canvas->setCursor(14, 136); 
  canvas->print(current_weather.valid ? current_weather.condition_str : "LOADING...");

  // Humidity & Wind Cards
  canvas->drawRoundRect(6, 168, 76, 56, 4, c_dark);
  canvas->fillRoundRect(7, 169, 74, 54, 4, 0x0963);
  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(12, 174); canvas->print("HUMIDITY");
  canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(12, 192); canvas->printf("%d%%", current_weather.humidity);

  canvas->drawRoundRect(90, 168, 76, 56, 4, c_dark);
  canvas->fillRoundRect(91, 169, 74, 54, 4, 0x0963);
  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(96, 174); canvas->print("WIND");
  canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(96, 192); canvas->printf("%.0f", current_weather.wind_mph);
  canvas->setTextSize(1); canvas->setTextColor(c_cyan); canvas->setCursor(132, 200); canvas->print("MPH");

  // Forecast Card
  canvas->drawRoundRect(6, 232, SCREEN_W - 12, 58, 4, c_dark);
  canvas->fillRoundRect(7, 233, SCREEN_W - 14, 56, 4, 0x10A2);
  canvas->setTextColor(c_green); canvas->setTextSize(1); canvas->setCursor(14, 240); canvas->print("AIR QUALITY INDEX");
  canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(14, 256); canvas->print("AQI 28 (GOOD)");

  canvas->drawFastHLine(0, 296, SCREEN_W, c_dark);
  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(10, 304); canvas->print("LOC:");
  canvas->setTextColor(c_cyan); canvas->setCursor(38, 304); canvas->print(current_location_name);
}

// 3. DIGITAL DESK CLOCK SCREEN
void renderClockScreen() {
  canvas->fillScreen(0x0821);
  uint16_t c_cyan = 0x07FF, c_amber = 0xFBE0, c_green = 0x07E0, c_white = 0xFFFF, c_gray = 0x7BEF, c_dark = 0x18E3;

  canvas->fillRect(0, 0, SCREEN_W, 30, 0x10A2);
  canvas->drawFastHLine(0, 30, SCREEN_W, c_green);
  canvas->fillCircle(12, 15, 4, c_green);

  canvas->setTextColor(c_white); canvas->setTextSize(1); canvas->setCursor(24, 11); canvas->print("DIGITAL DESK CLOCK");
  canvas->setTextColor(c_gray); canvas->setCursor(130, 11); canvas->print("NTP");

  struct tm timeinfo;
  bool time_valid = getLocalTime(&timeinfo, 100);

  // Large Time Box
  canvas->drawRoundRect(6, 42, SCREEN_W - 12, 90, 6, c_green);
  canvas->fillRoundRect(7, 43, SCREEN_W - 14, 88, 6, 0x0164);

  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(14, 50); canvas->print("CURRENT TIME");
  canvas->setTextColor(c_white); canvas->setTextSize(4); canvas->setCursor(14, 68);

  if (time_valid) {
    int hour12 = timeinfo.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    canvas->printf("%02d:%02d", hour12, timeinfo.tm_min);
    canvas->setTextSize(2); canvas->setTextColor(c_green); canvas->setCursor(134, 76);
    canvas->print(timeinfo.tm_hour >= 12 ? "PM" : "AM");
  } else {
    canvas->print("10:42");
    canvas->setTextSize(2); canvas->setTextColor(c_green); canvas->setCursor(134, 76); canvas->print("PM");
  }

  // Date Card
  canvas->drawRoundRect(6, 140, SCREEN_W - 12, 48, 4, c_dark);
  canvas->fillRoundRect(7, 141, SCREEN_W - 14, 46, 4, 0x1084);
  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(14, 146); canvas->print("DATE");
  canvas->setTextColor(c_amber); canvas->setTextSize(2); canvas->setCursor(14, 160);

  if (time_valid) {
    char dateBuf[32];
    strftime(dateBuf, sizeof(dateBuf), "%a, %b %d", &timeinfo);
    canvas->print(dateBuf);
  } else {
    canvas->print("MON, JUL 28");
  }

  // Wi-Fi Signal & System Status
  canvas->drawRoundRect(6, 196, SCREEN_W - 12, 88, 4, c_dark);
  canvas->fillRoundRect(7, 197, SCREEN_W - 14, 86, 4, 0x0963);
  canvas->setTextColor(c_cyan); canvas->setTextSize(1); canvas->setCursor(14, 204); canvas->print("WI-FI SIGNAL");

  int rssi = WiFi.RSSI();
  canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(14, 220); canvas->printf("%d dBm", rssi);

  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(14, 246); canvas->print("IP: ");
  canvas->setTextColor(c_white); canvas->print(WiFi.localIP().toString());

  canvas->drawFastHLine(0, 296, SCREEN_W, c_dark);
  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(10, 304); canvas->print("LOC:");
  canvas->setTextColor(c_cyan); canvas->setCursor(38, 304); canvas->print(current_location_name);
}

// 4. SUNRISE, SUNSET & MOON PHASE SCREEN
void renderSunMoonScreen() {
  canvas->fillScreen(0x0821);
  uint16_t c_cyan = 0x07FF, c_amber = 0xFBE0, c_green = 0x07E0, c_white = 0xFFFF, c_gray = 0x7BEF, c_dark = 0x18E3, c_pink = 0xF810;

  canvas->fillRect(0, 0, SCREEN_W, 30, 0x10A2);
  canvas->drawFastHLine(0, 30, SCREEN_W, c_pink);
  canvas->fillCircle(12, 15, 4, c_pink);

  canvas->setTextColor(c_white); canvas->setTextSize(1); canvas->setCursor(24, 11); canvas->print("SOLAR & LUNAR");
  canvas->setTextColor(c_gray); canvas->setCursor(125, 11); canvas->print("SEATTLE");

  // Sunrise Card
  canvas->drawRoundRect(6, 38, SCREEN_W - 12, 54, 4, c_dark);
  canvas->fillRoundRect(7, 39, SCREEN_W - 14, 52, 4, 0x2120);
  canvas->setTextColor(c_amber); canvas->setTextSize(1); canvas->setCursor(14, 44); canvas->print("SUNRISE");
  canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(14, 60); canvas->print("5:48 AM");

  // Sunset Card
  canvas->drawRoundRect(6, 100, SCREEN_W - 12, 54, 4, c_dark);
  canvas->fillRoundRect(7, 101, SCREEN_W - 14, 52, 4, 0x1084);
  canvas->setTextColor(c_pink); canvas->setTextSize(1); canvas->setCursor(14, 106); canvas->print("SUNSET");
  canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(14, 122); canvas->print("8:52 PM");

  // Daylight Progress Bar
  canvas->drawRoundRect(6, 162, SCREEN_W - 12, 44, 4, c_dark);
  canvas->fillRoundRect(7, 163, SCREEN_W - 14, 42, 4, 0x0963);
  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(14, 168); canvas->print("DAYLIGHT DURATION");
  canvas->setTextColor(c_cyan); canvas->setTextSize(1); canvas->setCursor(14, 184); canvas->print("15 HRS 04 MINS");

  // Moon Phase
  canvas->drawRoundRect(6, 214, SCREEN_W - 12, 70, 4, c_dark);
  canvas->fillRoundRect(7, 215, SCREEN_W - 14, 68, 4, 0x10A2);
  canvas->setTextColor(c_green); canvas->setTextSize(1); canvas->setCursor(14, 222); canvas->print("MOON PHASE");
  canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(14, 240); canvas->print("WAXING GIBBOUS");
  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(14, 262); canvas->print("ILLUMINATION: 88%");

  canvas->drawFastHLine(0, 296, SCREEN_W, c_dark);
  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(10, 304); canvas->print("LOC:");
  canvas->setTextColor(c_cyan); canvas->setCursor(38, 304); canvas->print(current_location_name);
}

// Master Render Router
void renderDisplay() {
  // SMART PRIORITY: Lock to Flight screen whenever a flight is overhead!
  if (current_flight.active) {
    current_screen = SCREEN_FLIGHT;
  }

  switch (current_screen) {
    case SCREEN_FLIGHT:   renderFlightScreen(); break;
    case SCREEN_WEATHER:  renderWeatherScreen(); break;
    case SCREEN_CLOCK:    renderClockScreen(); break;
    case SCREEN_SUN_MOON: renderSunMoonScreen(); break;
    default:              renderClockScreen(); break;
  }

  canvas->flush();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("==============================================");
  Serial.println("  ESP32-C6 MULTI-DASHBOARD ROTATING DISPLAY  ");
  Serial.println("==============================================");

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  display->begin();
  display->fillScreen(0x0000);
  canvas->begin();

  canvas->fillScreen(0x0821);
  canvas->setTextColor(0x07FF);
  canvas->setTextSize(2);
  canvas->setCursor(15, 80);
  canvas->println("MULTI-DASH");
  canvas->println("  SYSTEM");
  canvas->setTextSize(1);
  canvas->setTextColor(0xFFFF);
  canvas->setCursor(15, 140);
  canvas->println("Connecting Wi-Fi...");
  canvas->flush();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 25) {
    delay(500);
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi Connected! IP: " + WiFi.localIP().toString());
    configTime(-7 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // Seattle PST/PDT offset
    updateLocation();
    fetchWeatherData();
  }

  fetchOverheadFlights();
  renderDisplay();
}

void loop() {
  static uint32_t last_flight_fetch = 0;
  static uint32_t last_weather_fetch = 0;

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    WiFi.reconnect();
  }

  // Poll OpenSky every 12s
  if (millis() - last_flight_fetch >= 12000) {
    last_flight_fetch = millis();
    fetchOverheadFlights();
  }

  // Poll Open-Meteo Weather every 10 mins (600,000 ms)
  if (millis() - last_weather_fetch >= 600000 || !current_weather.valid) {
    last_weather_fetch = millis();
    fetchWeatherData();
  }

  // Rotate screen every 7 seconds when quiet (no active plane overhead)
  if (!current_flight.active && (millis() - last_screen_switch >= ROTATION_INTERVAL_MS)) {
    last_screen_switch = millis();
    current_screen = (DisplayScreen)((current_screen + 1) % NUM_SCREENS);
    if (current_screen == SCREEN_FLIGHT) {
      current_screen = (DisplayScreen)(current_screen + 1);
    }
  }

  renderDisplay();
  delay(current_flight.active ? 1000 : 200);
}
