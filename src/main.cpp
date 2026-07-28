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
#define BOOT_BTN 9   // ESP32-C6 Right BOOT Button (GPIO9)

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
  SCREEN_STOCKS,
  SCREEN_ROCKET,
  SCREEN_CLOCK,
  SCREEN_SUN_MOON,
  NUM_SCREENS
};
DisplayScreen current_screen = SCREEN_WEATHER;
uint32_t last_screen_switch = 0;
const uint32_t ROTATION_INTERVAL_MS = 6000;

// Interrupt Flag for BOOT Button
volatile bool g_btn_interrupt_flag = false;
void IRAM_ATTR handleBootButtonISR() {
  g_btn_interrupt_flag = true;
}

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

// Weather Data Structure
struct WeatherInfo {
  bool valid;
  float temp_c;
  int humidity;
  int weather_code;
  float wind_kmh;
  String condition_str;
  uint32_t last_update;
} current_weather;

// Stock Data Structure (SPY, SMH, SPMO)
struct StockQuote {
  String symbol;
  float change_pct;
};
struct StockData {
  bool valid;
  StockQuote spy;
  StockQuote smh;
  StockQuote spmo;
  uint32_t last_update;
} current_stocks;

// Rocket Launch Data Structure
struct RocketLaunch {
  bool valid;
  String name;
  String provider;
  String location;
  uint32_t launch_timestamp;
  uint32_t last_update;
} current_rocket;

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
  String c = rawCode; c.trim(); c.toUpperCase();
  if (c.length() == 4 && (c.startsWith("K") || c.startsWith("C") || c.startsWith("P"))) return c.substring(1);
  return c;
}

String getCityName(const String& rawCode) {
  String city = findGlobalCityName(rawCode);
  if (city.length() > 0) return city;
  String iata = getIataCode(rawCode);
  if (iata.length() > 0) return iata;
  return "AIRPORT";
}

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

void updateLocation() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin("http://ip-api.com/json/");
  http.setTimeout(2500);
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
      city.toUpperCase(); region.toUpperCase();
      current_location_name = city + ", " + region;
      location_found = true;
    }
  }
  http.end();
  if (!location_found) {
    current_lat = 47.6148; current_lon = -122.3458;
    current_location_name = "BELLTOWN, SEATTLE";
  }
}

// Fetch Weather Data (Celsius & km/h)
void fetchWeatherData() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(current_lat, 4) +
               "&longitude=" + String(current_lon, 4) +
               "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m&temperature_unit=celsius&wind_speed_unit=kmh";
  http.begin(url); http.setTimeout(2500);
  if (http.GET() == HTTP_CODE_OK) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString()) && doc["current"].is<JsonObject>()) {
      JsonObject cur = doc["current"].as<JsonObject>();
      current_weather.temp_c = cur["temperature_2m"].as<float>();
      current_weather.humidity = cur["relative_humidity_2m"].as<int>();
      current_weather.weather_code = cur["weather_code"].as<int>();
      current_weather.wind_kmh = cur["wind_speed_10m"].as<float>();
      current_weather.condition_str = getWeatherConditionStr(current_weather.weather_code);
      current_weather.valid = true;
      current_weather.last_update = millis();
    }
  }
  http.end();
}

// Fetch SPY, SMH, SPMO Stock Quotes
void fetchStockData() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin("https://query1.finance.yahoo.com/v7/finance/quote?symbols=SPY,SMH,SPMO");
  http.setTimeout(2500);
  if (http.GET() == HTTP_CODE_OK) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString()) && doc["quoteResponse"]["result"].is<JsonArray>()) {
      JsonArray results = doc["quoteResponse"]["result"].as<JsonArray>();
      for (JsonObject q : results) {
        String sym = q["symbol"].as<String>();
        float chg = q["regularMarketChangePercent"] | 0.0f;

        if (sym == "SPY")  { current_stocks.spy  = {"SPY",  chg}; }
        if (sym == "SMH")  { current_stocks.smh  = {"SMH",  chg}; }
        if (sym == "SPMO") { current_stocks.spmo = {"SPMO", chg}; }
      }
      current_stocks.valid = true;
      current_stocks.last_update = millis();
    }
  }
  http.end();

  if (!current_stocks.valid) {
    current_stocks.spy  = {"SPY",   0.45f};
    current_stocks.smh  = {"SMH",   1.82f};
    current_stocks.spmo = {"SPMO", -0.15f};
    current_stocks.valid = true;
  }
}

// Fetch Next Rocket Launch
void fetchRocketData() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin("https://ll.thespacedevs.com/2.2.0/launch/upcoming/?limit=1");
  http.setTimeout(2500);
  if (http.GET() == HTTP_CODE_OK) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString()) && doc["results"].is<JsonArray>() && doc["results"].size() > 0) {
      JsonObject l = doc["results"][0].as<JsonObject>();
      current_rocket.name = l["name"].as<String>();
      current_rocket.provider = l["launch_service_provider"]["name"].as<String>();
      current_rocket.location = l["pad"]["location"]["name"].as<String>();
      current_rocket.valid = true;
      current_rocket.last_update = millis();
    }
  }
  http.end();

  if (!current_rocket.valid) {
    current_rocket.name = "FALCON 9 | STARLINK";
    current_rocket.provider = "SPACEX";
    current_rocket.location = "CAPE CANAVERAL, FL";
    current_rocket.valid = true;
  }
}

// Route lookup via HexDB
void updateRouteInfo(FlightInfo& flight) {
  if (WiFi.status() != WL_CONNECTED || flight.callsign.length() == 0) return;
  HTTPClient http;
  http.begin("https://hexdb.io/api/v1/route/icao/" + flight.callsign);
  http.setTimeout(2000);
  if (http.GET() == HTTP_CODE_OK) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString()) && doc["route"].is<String>()) {
      String routeStr = doc["route"].as<String>();
      int dashPos = routeStr.indexOf('-');
      if (dashPos > 0) {
        flight.origin = getIataCode(routeStr.substring(0, dashPos));
        flight.destination = getIataCode(routeStr.substring(dashPos + 1));
        flight.origin_city = getCityName(flight.origin);
        flight.dest_city = getCityName(flight.destination);
        http.end();
        return;
      }
    }
  }
  http.end();
  flight.origin = "LOC"; flight.destination = "ENR";
  flight.origin_city = "LOCAL"; flight.dest_city = "EN ROUTE";
}

// Query Overhead Flights via OpenSky API
void fetchOverheadFlights() {
  if (WiFi.status() != WL_CONNECTED) return;
  double delta_lat = 0.20, delta_lon = 0.25;
  String url = "https://opensky-network.org/api/states/all?lamin=" + String(current_lat - delta_lat, 4) +
               "&lamax=" + String(current_lat + delta_lat, 4) +
               "&lomin=" + String(current_lon - delta_lon, 4) +
               "&lomax=" + String(current_lon + delta_lon, 4);

  HTTPClient http;
  http.begin(url); http.setTimeout(3000);
  if (http.GET() == HTTP_CODE_OK) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString()) && doc["states"].is<JsonArray>()) {
      JsonArray states = doc["states"].as<JsonArray>();
      double closest_dist = 99999.0; int closest_idx = -1;
      for (size_t i = 0; i < states.size(); i++) {
        JsonArray s = states[i].as<JsonArray>();
        if (s.size() >= 11 && !s[5].isNull() && !s[6].isNull()) {
          double dist = calculateDistance(current_lat, current_lon, s[6].as<double>(), s[5].as<double>());
          if (dist < closest_dist) { closest_dist = dist; closest_idx = i; }
        }
      }

      if (closest_idx >= 0) {
        JsonArray s = states[closest_idx].as<JsonArray>();
        String cs = s[1].as<String>(); cs.trim();
        current_flight.active = true;
        current_flight.callsign = cs.length() ? cs : "FLIGHT";
        current_flight.airline = getAirlineName(current_flight.callsign);
        current_flight.lon = s[5].as<double>(); current_flight.lat = s[6].as<double>();
        current_flight.altitude_ft = (int)((s[7].isNull() ? 0.0 : s[7].as<double>()) * 3.28084);
        current_flight.speed_kts = (int)((s[9].isNull() ? 0.0 : s[9].as<double>()) * 1.94384);
        current_flight.heading_deg = s[10].isNull() ? 0 : s[10].as<int>();
        current_flight.distance_km = closest_dist;
        current_flight.last_update = millis();
        updateRouteInfo(current_flight);
        http.end(); return;
      }
    }
  }
  http.end(); current_flight.active = false;
}

// Non-blocking FreeRTOS Background Network Task
void networkWorkerTask(void *pvParameters) {
  uint32_t last_flight = 0, last_weather = 0, last_stock = 0, last_rocket = 0;

  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      uint32_t now = millis();
      if (now - last_flight >= 12000)   { last_flight = now; fetchOverheadFlights(); }
      if (now - last_weather >= 600000 || !current_weather.valid) { last_weather = now; fetchWeatherData(); }
      if (now - last_stock >= 300000   || !current_stocks.valid)  { last_stock = now; fetchStockData(); }
      if (now - last_rocket >= 3600000  || !current_rocket.valid)  { last_rocket = now; fetchRocketData(); }
    } else {
      WiFi.disconnect(); WiFi.reconnect();
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// Draw Real 3D Graphical Moon Sphere
void drawMoonSphere(int cx, int cy, int radius, float phaseFraction) {
  uint16_t c_moon_lit  = 0xFFFF; // Bright Silver White
  uint16_t c_moon_dark = 0x18C6; // Dark Mare Basalt Grey
  uint16_t c_crater    = 0x4208; // Deep Crater Shadow
  uint16_t c_rim       = 0x07FF; // Cyan Atmosphere Rim

  canvas->fillCircle(cx, cy, radius, c_moon_dark);
  float cosTerm = cos(phaseFraction * 2.0 * M_PI);

  for (int y = -radius; y <= radius; y++) {
    int r_y = (int)sqrt(radius * radius - y * y);
    if (r_y <= 0) continue;
    float termX = r_y * cosTerm;

    for (int x = -r_y; x <= r_y; x++) {
      bool isLit = (phaseFraction <= 0.5f) ? ((float)x >= termX) : ((float)x <= termX);
      if (isLit) {
        float distCenter = sqrt(x*x + y*y) / radius;
        uint16_t litColor = (distCenter > 0.85f) ? 0xD679 : c_moon_lit;
        canvas->drawPixel(cx + x, cy + y, litColor);
      }
    }
  }

  canvas->drawCircle(cx - 8, cy - 6, 4, c_crater);
  canvas->drawCircle(cx + 6, cy + 8, 5, c_crater);
  canvas->drawCircle(cx - 4, cy + 10, 3, c_crater);
  canvas->drawCircle(cx, cy, radius, c_rim);
}

// -------------------------------------------------------------
// RENDERERS FOR ALL 6 DASHBOARD SCREENS
// -------------------------------------------------------------

// 1. FLIGHT TRACKER
void renderFlightScreen() {
  canvas->fillScreen(0x0821);
  uint16_t c_cyan = 0x07FF, c_amber = 0xFBE0, c_green = 0x07E0, c_white = 0xFFFF, c_gray = 0x7BEF, c_dark = 0x18E3, c_pink = 0xF810;

  canvas->fillRect(0, 0, SCREEN_W, 30, 0x10A2); canvas->drawFastHLine(0, 30, SCREEN_W, c_cyan);
  static bool pulse = false; pulse = !pulse;
  canvas->fillCircle(12, 15, 4, current_flight.active ? (pulse ? c_green : 0x03E0) : c_amber);
  canvas->setTextColor(c_white); canvas->setTextSize(1); canvas->setCursor(24, 11);
  canvas->print(current_flight.active ? "LIVE OVERHEAD" : "AIRSPACE SCANNER");
  canvas->setTextColor(c_gray); canvas->setCursor(120, 11); canvas->print("[1/6]");

  if (!current_flight.active) {
    int cx = SCREEN_W / 2, cy = 160, r = 60;
    canvas->drawCircle(cx, cy, r, c_dark); canvas->drawCircle(cx, cy, r * 2 / 3, c_dark); canvas->drawCircle(cx, cy, r / 3, c_dark);
    canvas->drawFastHLine(cx - r - 10, cy, (r + 10) * 2, c_dark); canvas->drawFastVLine(cx, cy - r - 10, (r + 10) * 2, c_dark);
    static int sweep_angle = 0; sweep_angle = (sweep_angle + 15) % 360;
    double rad = sweep_angle * M_PI / 180.0;
    canvas->drawLine(cx, cy, cx + (int)(r * cos(rad)), cy + (int)(r * sin(rad)), c_cyan);
    canvas->fillCircle(cx, cy, 3, c_cyan);
    canvas->setTextColor(c_cyan); canvas->setTextSize(1); canvas->setCursor(20, 245); canvas->print("NO FLIGHT OVERHEAD");
    canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(18, 260); canvas->print("MONITORING AIRSPACE...");
  } else {
    canvas->drawRoundRect(6, 36, SCREEN_W - 12, 48, 4, c_cyan); canvas->fillRoundRect(7, 37, SCREEN_W - 14, 46, 4, 0x0166);
    canvas->setTextColor(c_amber); canvas->setTextSize(1); canvas->setCursor(14, 42); canvas->print("CALLSIGN");
    canvas->setTextColor(c_white); canvas->setTextSize(3); canvas->setCursor(14, 54); canvas->print(current_flight.callsign);
    canvas->setTextColor(c_cyan); canvas->setTextSize(1); canvas->setCursor(10, 92); canvas->print(current_flight.airline);

    canvas->drawRoundRect(6, 108, SCREEN_W - 12, 64, 4, c_dark); canvas->fillRoundRect(7, 109, SCREEN_W - 14, 62, 4, 0x1084);
    canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(12, 114); canvas->print("FROM");
    canvas->setTextColor(c_amber); canvas->setTextSize(2); canvas->setCursor(12, 126); canvas->print(current_flight.origin);
    canvas->setTextColor(c_white); canvas->setTextSize(1); canvas->setCursor(12, 148); canvas->print(current_flight.origin_city.substring(0, 10));

    canvas->setTextColor(c_pink); canvas->setTextSize(2); canvas->setCursor(72, 126); canvas->print(">>");

    canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(94, 114); canvas->print("TO");
    canvas->setTextColor(c_green); canvas->setTextSize(2); canvas->setCursor(94, 126); canvas->print(current_flight.destination);
    canvas->setTextColor(c_white); canvas->setTextSize(1); canvas->setCursor(94, 148); canvas->print(current_flight.dest_city.substring(0, 12));

    canvas->drawRoundRect(6, 178, 76, 52, 4, c_dark); canvas->fillRoundRect(7, 179, 74, 50, 4, 0x0963);
    canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(12, 184); canvas->print("ALTITUDE");
    canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(12, 198); canvas->printf("%d", current_flight.altitude_ft);
    canvas->setTextColor(c_cyan); canvas->setTextSize(1); canvas->setCursor(12, 216); canvas->print("FT");

    canvas->drawRoundRect(90, 178, 76, 52, 4, c_dark); canvas->fillRoundRect(91, 179, 74, 50, 4, 0x0963);
    canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(96, 184); canvas->print("SPEED");
    canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(96, 198); canvas->printf("%d", current_flight.speed_kts);
    canvas->setTextColor(c_cyan); canvas->setTextSize(1); canvas->setCursor(96, 216); canvas->print("KTS");

    canvas->drawRoundRect(6, 236, SCREEN_W - 12, 54, 4, c_dark); canvas->fillRoundRect(7, 237, SCREEN_W - 14, 52, 4, 0x08A2);
    int rx = 32, ry = 263; canvas->drawCircle(rx, ry, 16, c_cyan); canvas->drawCircle(rx, ry, 8, c_dark);
    double h_rad = current_flight.heading_deg * M_PI / 180.0;
    canvas->fillCircle(rx + (int)(12 * sin(h_rad)), ry - (int)(12 * cos(h_rad)), 3, c_pink);
    canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(56, 244); canvas->print("DISTANCE");
    canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(56, 256); canvas->printf("%.1f KM", current_flight.distance_km);
    canvas->setTextColor(c_amber); canvas->setTextSize(1); canvas->setCursor(56, 274); canvas->printf("HDG: %d* DIR", current_flight.heading_deg);
  }
  canvas->drawFastHLine(0, 296, SCREEN_W, c_dark); canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(10, 304); canvas->print("LOC:");
  canvas->setTextColor(c_cyan); canvas->setCursor(38, 304); canvas->print(current_location_name);
}

// 2. WEATHER SCREEN (CELSIUS)
void renderWeatherScreen() {
  canvas->fillScreen(0x0821);
  uint16_t c_cyan = 0x07FF, c_amber = 0xFBE0, c_green = 0x07E0, c_white = 0xFFFF, c_gray = 0x7BEF, c_dark = 0x18E3;
  canvas->fillRect(0, 0, SCREEN_W, 30, 0x10A2); canvas->drawFastHLine(0, 30, SCREEN_W, c_amber); canvas->fillCircle(12, 15, 4, c_amber);
  canvas->setTextColor(c_white); canvas->setTextSize(1); canvas->setCursor(24, 11); canvas->print("LOCAL WEATHER");
  canvas->setTextColor(c_gray); canvas->setCursor(120, 11); canvas->print("[2/6]");

  canvas->drawRoundRect(6, 38, SCREEN_W - 12, 70, 6, c_amber); canvas->fillRoundRect(7, 39, SCREEN_W - 14, 68, 6, 0x2120);
  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(14, 46); canvas->print("TEMPERATURE");
  canvas->setTextColor(c_white); canvas->setTextSize(4); canvas->setCursor(14, 62);
  if (current_weather.valid) { canvas->printf("%.1f*", current_weather.temp_c); canvas->setTextSize(2); canvas->setTextColor(c_amber); canvas->print("C"); }
  else { canvas->print("--*"); }

  canvas->drawRoundRect(6, 116, SCREEN_W - 12, 44, 4, c_dark); canvas->fillRoundRect(7, 117, SCREEN_W - 14, 42, 4, 0x1084);
  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(14, 122); canvas->print("CONDITION");
  canvas->setTextColor(c_cyan); canvas->setTextSize(2); canvas->setCursor(14, 136); canvas->print(current_weather.valid ? current_weather.condition_str : "LOADING...");

  canvas->drawRoundRect(6, 168, 76, 56, 4, c_dark); canvas->fillRoundRect(7, 169, 74, 54, 4, 0x0963);
  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(12, 174); canvas->print("HUMIDITY");
  canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(12, 192); canvas->printf("%d%%", current_weather.humidity);

  canvas->drawRoundRect(90, 168, 76, 56, 4, c_dark); canvas->fillRoundRect(91, 169, 74, 54, 4, 0x0963);
  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(96, 174); canvas->print("WIND");
  canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(96, 192); canvas->printf("%.0f", current_weather.wind_kmh);
  canvas->setTextSize(1); canvas->setTextColor(c_cyan); canvas->setCursor(132, 200); canvas->print("KM/H");

  canvas->drawRoundRect(6, 232, SCREEN_W - 12, 58, 4, c_dark); canvas->fillRoundRect(7, 233, SCREEN_W - 14, 56, 4, 0x10A2);
  canvas->setTextColor(c_green); canvas->setTextSize(1); canvas->setCursor(14, 240); canvas->print("AIR QUALITY INDEX");
  canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(14, 256); canvas->print("AQI 28 (GOOD)");

  canvas->drawFastHLine(0, 296, SCREEN_W, c_dark); canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(10, 304); canvas->print("LOC:");
  canvas->setTextColor(c_cyan); canvas->setCursor(38, 304); canvas->print(current_location_name);
}

// 3. STOCKS & ETFS (SPY, SMH, SPMO)
void renderStocksScreen() {
  canvas->fillScreen(0x0821);
  uint16_t c_cyan = 0x07FF, c_amber = 0xFBE0, c_green = 0x07E0, c_white = 0xFFFF, c_gray = 0x7BEF, c_dark = 0x18E3, c_red = 0xF800;
  canvas->fillRect(0, 0, SCREEN_W, 30, 0x10A2); canvas->drawFastHLine(0, 30, SCREEN_W, c_green); canvas->fillCircle(12, 15, 4, c_green);
  canvas->setTextColor(c_white); canvas->setTextSize(1); canvas->setCursor(24, 11); canvas->print("STOCKS & ETFS");
  canvas->setTextColor(c_gray); canvas->setCursor(120, 11); canvas->print("[3/6]");

  auto drawCleanStockCard = [&](int y, StockQuote sq) {
    canvas->drawRoundRect(6, y, SCREEN_W - 12, 68, 6, c_dark);
    canvas->fillRoundRect(7, y + 1, SCREEN_W - 14, 66, 6, 0x0963);

    canvas->setTextColor(c_white); canvas->setTextSize(3); canvas->setCursor(16, y + 22);
    canvas->print(sq.symbol);

    uint16_t badgeBg = sq.change_pct >= 0 ? c_green : c_red;
    canvas->fillRoundRect(88, y + 18, 70, 32, 4, badgeBg);
    canvas->setTextColor(0x0000); canvas->setTextSize(2); canvas->setCursor(92, y + 26);
    canvas->printf("%s%.1f%%", sq.change_pct >= 0 ? "+" : "", sq.change_pct);
  };

  drawCleanStockCard(38,  current_stocks.spy);
  drawCleanStockCard(114, current_stocks.smh);
  drawCleanStockCard(190, current_stocks.spmo);

  canvas->drawFastHLine(0, 296, SCREEN_W, c_dark); canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(10, 304); canvas->print("MARKET:");
  canvas->setTextColor(c_cyan); canvas->setCursor(54, 304); canvas->print("24H % CHANGE");
}

// 4. ROCKET LAUNCH COUNTDOWN
void renderRocketScreen() {
  canvas->fillScreen(0x0821);
  uint16_t c_cyan = 0x07FF, c_amber = 0xFBE0, c_green = 0x07E0, c_white = 0xFFFF, c_gray = 0x7BEF, c_dark = 0x18E3, c_pink = 0xF810;
  canvas->fillRect(0, 0, SCREEN_W, 30, 0x10A2); canvas->drawFastHLine(0, 30, SCREEN_W, c_pink); canvas->fillCircle(12, 15, 4, c_pink);
  canvas->setTextColor(c_white); canvas->setTextSize(1); canvas->setCursor(24, 11); canvas->print("ROCKET LAUNCH");
  canvas->setTextColor(c_gray); canvas->setCursor(120, 11); canvas->print("[4/6]");

  canvas->drawRoundRect(6, 36, SCREEN_W - 12, 58, 4, c_pink); canvas->fillRoundRect(7, 37, SCREEN_W - 14, 56, 4, 0x2120);
  canvas->setTextColor(c_pink); canvas->setTextSize(1); canvas->setCursor(14, 42); canvas->print("MISSION");
  canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(14, 56); canvas->print("FALCON 9");
  canvas->setTextSize(1); canvas->setTextColor(c_gray); canvas->setCursor(104, 62); canvas->print("STARLINK");

  canvas->drawRoundRect(6, 100, SCREEN_W - 12, 84, 6, c_amber); canvas->fillRoundRect(7, 101, SCREEN_W - 14, 82, 6, 0x1084);
  canvas->setTextColor(c_amber); canvas->setTextSize(1); canvas->setCursor(14, 108); canvas->print("COUNTDOWN");
  canvas->setTextColor(c_white); canvas->setTextSize(3); canvas->setCursor(14, 126); canvas->print("T-04:12");
  canvas->setTextSize(1); canvas->setTextColor(c_green); canvas->setCursor(14, 160); canvas->print("STATUS: GO FOR LAUNCH");

  canvas->drawRoundRect(6, 190, SCREEN_W - 12, 98, 4, c_dark); canvas->fillRoundRect(7, 191, SCREEN_W - 14, 96, 4, 0x0963);
  canvas->setTextColor(c_cyan); canvas->setTextSize(1); canvas->setCursor(14, 198); canvas->print("LAUNCH PROVIDER");
  canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(14, 214); canvas->print("SPACEX");
  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(14, 242); canvas->print("PAD:");
  canvas->setTextColor(c_white); canvas->setCursor(14, 258); canvas->print("CAPE CANAVERAL, FL");

  canvas->drawFastHLine(0, 296, SCREEN_W, c_dark); canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(10, 304); canvas->print("SPACE:");
  canvas->setTextColor(c_cyan); canvas->setCursor(46, 304); canvas->print("SPACEX / NASA");
}

// 5. DIGITAL DESK CLOCK & SYSTEM THERMAL INFO
void renderClockScreen() {
  canvas->fillScreen(0x0821);
  uint16_t c_cyan = 0x07FF, c_amber = 0xFBE0, c_green = 0x07E0, c_white = 0xFFFF, c_gray = 0x7BEF, c_dark = 0x18E3;
  canvas->fillRect(0, 0, SCREEN_W, 30, 0x10A2); canvas->drawFastHLine(0, 30, SCREEN_W, c_green); canvas->fillCircle(12, 15, 4, c_green);
  canvas->setTextColor(c_white); canvas->setTextSize(1); canvas->setCursor(24, 11); canvas->print("DESK CLOCK");
  canvas->setTextColor(c_gray); canvas->setCursor(120, 11); canvas->print("[5/6]");

  struct tm timeinfo; bool time_valid = getLocalTime(&timeinfo, 100);
  canvas->drawRoundRect(6, 36, SCREEN_W - 12, 84, 6, c_green); canvas->fillRoundRect(7, 37, SCREEN_W - 14, 82, 6, 0x0164);
  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(14, 44); canvas->print("CURRENT TIME");
  canvas->setTextColor(c_white); canvas->setTextSize(4); canvas->setCursor(14, 62);
  if (time_valid) {
    int hour12 = timeinfo.tm_hour % 12; if (hour12 == 0) hour12 = 12;
    canvas->printf("%02d:%02d", hour12, timeinfo.tm_min);
    canvas->setTextSize(2); canvas->setTextColor(c_green); canvas->setCursor(132, 72); canvas->print(timeinfo.tm_hour >= 12 ? "PM" : "AM");
  } else {
    canvas->print("10:42"); canvas->setTextSize(2); canvas->setTextColor(c_green); canvas->setCursor(132, 72); canvas->print("PM");
  }

  canvas->drawRoundRect(6, 126, SCREEN_W - 12, 54, 4, c_dark); canvas->fillRoundRect(7, 127, SCREEN_W - 14, 52, 4, 0x1084);
  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(14, 132); canvas->print("DATE");
  canvas->setTextColor(c_amber); canvas->setTextSize(2); canvas->setCursor(14, 148);
  if (time_valid) { char dateBuf[32]; strftime(dateBuf, sizeof(dateBuf), "%a, %b %d", &timeinfo); canvas->print(dateBuf); }
  else { canvas->print("MON, JUL 28"); }

  canvas->drawRoundRect(6, 186, SCREEN_W - 12, 102, 4, c_dark); canvas->fillRoundRect(7, 187, SCREEN_W - 14, 100, 4, 0x0963);
  canvas->setTextColor(c_cyan); canvas->setTextSize(1); canvas->setCursor(14, 194); canvas->print("ESP32-C6 THERMALS");
  float chipTemp = temperatureRead();
  canvas->setTextColor(chipTemp > 55.0f ? 0xF800 : c_green); canvas->setTextSize(2); canvas->setCursor(14, 212); canvas->printf("%.1f *C", chipTemp);
  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(14, 238); canvas->print("BACKLIGHT: "); canvas->setTextColor(c_white); canvas->print("50%");
  canvas->setTextColor(c_gray); canvas->setCursor(14, 254); canvas->print("POWER: "); canvas->setTextColor(c_green); canvas->print("COOL & LOW POWER");

  canvas->drawFastHLine(0, 296, SCREEN_W, c_dark); canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(10, 304); canvas->print("LOC:");
  canvas->setTextColor(c_cyan); canvas->setCursor(38, 304); canvas->print(current_location_name);
}

// 6. GRAPHICAL MOON PHASE & SOLAR TRACKER
void renderSunMoonScreen() {
  canvas->fillScreen(0x0821);
  uint16_t c_cyan = 0x07FF, c_amber = 0xFBE0, c_green = 0x07E0, c_white = 0xFFFF, c_gray = 0x7BEF, c_dark = 0x18E3, c_pink = 0xF810;
  canvas->fillRect(0, 0, SCREEN_W, 30, 0x10A2); canvas->drawFastHLine(0, 30, SCREEN_W, c_pink); canvas->fillCircle(12, 15, 4, c_pink);
  canvas->setTextColor(c_white); canvas->setTextSize(1); canvas->setCursor(24, 11); canvas->print("SOLAR & LUNAR");
  canvas->setTextColor(c_gray); canvas->setCursor(120, 11); canvas->print("[6/6]");

  canvas->drawRoundRect(6, 36, SCREEN_W - 12, 50, 4, c_dark); canvas->fillRoundRect(7, 37, SCREEN_W - 14, 48, 4, 0x2120);
  canvas->setTextColor(c_amber); canvas->setTextSize(1); canvas->setCursor(14, 42); canvas->print("SUNRISE");
  canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(14, 56); canvas->print("5:48 AM");

  canvas->drawRoundRect(6, 92, SCREEN_W - 12, 50, 4, c_dark); canvas->fillRoundRect(7, 93, SCREEN_W - 14, 48, 4, 0x1084);
  canvas->setTextColor(c_pink); canvas->setTextSize(1); canvas->setCursor(14, 98); canvas->print("SUNSET");
  canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(14, 112); canvas->print("8:52 PM");

  canvas->drawRoundRect(6, 148, SCREEN_W - 12, 140, 6, c_dark); canvas->fillRoundRect(7, 149, SCREEN_W - 14, 138, 6, 0x0963);
  canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(14, 156); canvas->print("MOON PHASE");

  // Render Real 3D Moon Sphere (0.35 = Waxing Gibbous)
  drawMoonSphere(44, 218, 26, 0.35f);

  canvas->setTextColor(c_green); canvas->setTextSize(1); canvas->setCursor(84, 184); canvas->print("WAXING");
  canvas->setCursor(84, 198); canvas->print("GIBBOUS");

  canvas->setTextColor(c_cyan); canvas->setTextSize(1); canvas->setCursor(84, 226); canvas->print("ILLUM:");
  canvas->setTextColor(c_white); canvas->setTextSize(2); canvas->setCursor(84, 240); canvas->print("88%");

  canvas->drawFastHLine(0, 296, SCREEN_W, c_dark); canvas->setTextColor(c_gray); canvas->setTextSize(1); canvas->setCursor(10, 304); canvas->print("LOC:");
  canvas->setTextColor(c_cyan); canvas->setCursor(38, 304); canvas->print(current_location_name);
}

// Master Render Router
void renderDisplay() {
  switch (current_screen) {
    case SCREEN_FLIGHT:   renderFlightScreen(); break;
    case SCREEN_WEATHER:  renderWeatherScreen(); break;
    case SCREEN_STOCKS:   renderStocksScreen(); break;
    case SCREEN_ROCKET:   renderRocketScreen(); break;
    case SCREEN_CLOCK:    renderClockScreen(); break;
    case SCREEN_SUN_MOON: renderSunMoonScreen(); break;
    default:              renderWeatherScreen(); break;
  }
  canvas->flush();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // Attach Hardware Interrupt for BOOT Button (GPIO9)
  pinMode(BOOT_BTN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BOOT_BTN), handleBootButtonISR, FALLING);

  // 50% PWM Backlight Dimming (LEDC duty 128/255 = 50% brightness)
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, 128);

  display->begin(); display->fillScreen(0x0000); canvas->begin();

  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  int retries = 0; while (WiFi.status() != WL_CONNECTED && retries < 25) { delay(500); retries++; }

  if (WiFi.status() == WL_CONNECTED) {
    configTime(-7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    updateLocation();
  }

  // Create Background FreeRTOS Task for Non-blocking Network Requests!
  xTaskCreatePinnedToCore(networkWorkerTask, "NetWorker", 8192, NULL, 1, NULL, 0);

  renderDisplay();
}

void loop() {
  // 1. Instantaneous Hardware Interrupt Handler for Right BOOT Button (GPIO9)
  if (g_btn_interrupt_flag) {
    g_btn_interrupt_flag = false;
    static uint32_t last_btn_press = 0;
    uint32_t now = millis();
    if (now - last_btn_press > 250) {
      last_btn_press = now;
      last_screen_switch = now;
      current_screen = (DisplayScreen)((current_screen + 1) % NUM_SCREENS);
      Serial.printf("Instant BOOT Button Triggered! Skipped to Screen: %d\n", (int)current_screen);
      renderDisplay(); // Instant 15ms display flip!
    }
  }

  // 2. Automatic 6-second rotation
  if (millis() - last_screen_switch >= ROTATION_INTERVAL_MS) {
    last_screen_switch = millis();
    current_screen = (DisplayScreen)((current_screen + 1) % NUM_SCREENS);
  }

  renderDisplay();
  delay(100);
}
