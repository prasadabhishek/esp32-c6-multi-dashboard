#ifndef CONFIG_H
#define CONFIG_H

// =========================================================================
// 🌐 WI-FI NETWORK CONFIGURATION
// Placeholders for public repository. Update with your Wi-Fi credentials.
// =========================================================================
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// =========================================================================
// 📍 LOCATION & GEOLOCATION SETTINGS
// Set USE_AUTO_IP_GEOLOCATION to true for dynamic location auto-detection.
// Set to false to use fixed custom coordinates below.
// =========================================================================
#define USE_AUTO_IP_GEOLOCATION true
#define CUSTOM_LATITUDE        47.6148
#define CUSTOM_LONGITUDE       -122.3458
#define CUSTOM_LOCATION_NAME   "SEATTLE, WA"

// =========================================================================
// 🌡️ DISPLAY & UNIT PREFERENCES
// USE_CELSIUS: true = Celsius (°C), false = Fahrenheit (°F)
// =========================================================================
#define USE_CELSIUS            true
#define SCREEN_ROTATION_MS     6000     // Auto-rotation interval in milliseconds
#define BACKLIGHT_BRIGHTNESS   75       // PWM Duty Cycle (75/255 = 30% brightness)

#endif // CONFIG_H
