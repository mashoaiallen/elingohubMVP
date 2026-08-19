#include "config.h"

namespace Config {

const char WIFI_SSID[] = "Allen";
const char WIFI_PASSWORD[] = "allenmashoai";

// Use the LAN IP address of the computer running Flask.
const char API_BASE_URL[] = "http://10.19.129.90:5000";

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
