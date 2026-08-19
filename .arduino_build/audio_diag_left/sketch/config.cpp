#line 1 "C:\\Users\\Wits student\\Desktop\\eLingo Hub\\elingohubMVP\\firmware\\esp32_hub\\config.cpp"
#include "config.h"

namespace Config {

const char WIFI_SSID[] = "YOUR_WIFI_SSID";
const char WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD";

// Use the LAN IP address of the computer running Flask.
// Do not use 127.0.0.1 from the ESP32; that points back to the ESP32 itself.
const char API_BASE_URL[] = "http://10.139.2.1:5000";

String endpoint(const char *path) {
  String base(API_BASE_URL);
  String suffix(path);

  if (base.endsWith("/") && suffix.startsWith("/")) {
    base.remove(base.length() - 1);
  } else if (!base.endsWith("/") && !suffix.startsWith("/")) {
    base += "/";
  }

  return base + suffix;
}

}  // namespace Config
