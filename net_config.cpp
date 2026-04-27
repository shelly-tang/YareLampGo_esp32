#include "net_config.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_mac.h>

namespace {

const char *kNvsNamespace = "lampgo-net";
const char *kKeySsid = "ssid";
const char *kKeyPass = "pass";
const char *kApPrefix = "Lampgo-Setup-";
const char *kApPassword = "lampgo123";

Preferences g_prefs;
String g_deviceSuffix;

String macSuffixFromMac(const uint8_t mac[6]) {
  char buf[5];
  snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
  return String(buf);
}

}

namespace NetConfig {

void begin() {
  uint8_t mac[6] = {0};
  // Read the factory-burned MAC straight from eFuse so we get real bytes even
  // before the WiFi driver has been started. WiFi.macAddress() only works
  // after WiFi.mode()/WiFi.begin() has initialised the stack, otherwise it
  // returns all zeros and the device ends up named Lampgo-Setup-0000.
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    WiFi.macAddress(mac);
  }
  g_deviceSuffix = macSuffixFromMac(mac);
}

bool hasCredentials() {
  if (!g_prefs.begin(kNvsNamespace, true)) {
    return false;
  }
  bool ok = g_prefs.isKey(kKeySsid);
  if (ok) {
    String s = g_prefs.getString(kKeySsid, "");
    ok = s.length() > 0;
  }
  g_prefs.end();
  return ok;
}

bool loadWifi(String &ssid, String &password) {
  if (!g_prefs.begin(kNvsNamespace, true)) {
    return false;
  }
  ssid = g_prefs.getString(kKeySsid, "");
  password = g_prefs.getString(kKeyPass, "");
  g_prefs.end();
  return ssid.length() > 0;
}

bool saveWifi(const String &ssid, const String &password) {
  if (!g_prefs.begin(kNvsNamespace, false)) {
    return false;
  }
  g_prefs.putString(kKeySsid, ssid);
  g_prefs.putString(kKeyPass, password);
  g_prefs.end();
  return true;
}

bool clearWifi() {
  if (!g_prefs.begin(kNvsNamespace, false)) {
    return false;
  }
  g_prefs.clear();
  g_prefs.end();
  return true;
}

String deviceIdSuffix() {
  if (g_deviceSuffix.length() == 0) {
    begin();
  }
  return g_deviceSuffix;
}

String deviceHostname() {
  return String("lampgo-cam-") + deviceIdSuffix();
}

String apSsid() {
  return String(kApPrefix) + deviceIdSuffix();
}

const char *apPassword() {
  return kApPassword;
}

}
