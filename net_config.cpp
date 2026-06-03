// SPDX-FileCopyrightText: 2026 @ninsmiracle, @shelly-tang, and LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "net_config.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <mbedtls/sha256.h>

namespace {

const char *kNvsNamespace = "lampgo-net";
const char *kKeySsid = "ssid";
const char *kKeyPass = "pass";
const char *kKeyPairOwner = "pair_owner";
const char *kKeyPairLabel = "pair_label";
const char *kKeyPairHash = "pair_hash";
const char *kApPrefix = "Lampgo-Setup-";
const char *kApPassword = "lampgo123";

Preferences g_prefs;
String g_deviceSuffix;

String macSuffixFromMac(const uint8_t mac[6]) {
  char buf[5];
  snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
  return String(buf);
}

String sha256Hex(const String &value) {
  unsigned char digest[32] = {0};
  int ret = mbedtls_sha256(
      reinterpret_cast<const unsigned char *>(value.c_str()),
      value.length(),
      digest,
      0);
  if (ret != 0) return "";
  char hex[65];
  for (int i = 0; i < 32; i++) {
    snprintf(hex + i * 2, 3, "%02x", digest[i]);
  }
  hex[64] = 0;
  return String(hex);
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
  g_prefs.remove(kKeySsid);
  g_prefs.remove(kKeyPass);
  g_prefs.end();
  return true;
}

bool loadPairing(String &ownerId, String &ownerLabel, String &secretHash) {
  if (!g_prefs.begin(kNvsNamespace, true)) {
    return false;
  }
  ownerId = g_prefs.getString(kKeyPairOwner, "");
  ownerLabel = g_prefs.getString(kKeyPairLabel, "");
  secretHash = g_prefs.getString(kKeyPairHash, "");
  g_prefs.end();
  return ownerId.length() > 0 && secretHash.length() > 0;
}

bool savePairing(const String &ownerId, const String &ownerLabel, const String &pairingSecret) {
  if (ownerId.length() == 0 || pairingSecret.length() == 0) {
    return false;
  }
  String hash = sha256Hex(pairingSecret);
  if (hash.length() == 0) {
    return false;
  }
  if (!g_prefs.begin(kNvsNamespace, false)) {
    return false;
  }
  g_prefs.putString(kKeyPairOwner, ownerId);
  g_prefs.putString(kKeyPairLabel, ownerLabel);
  g_prefs.putString(kKeyPairHash, hash);
  g_prefs.end();
  return true;
}

bool clearPairing() {
  if (!g_prefs.begin(kNvsNamespace, false)) {
    return false;
  }
  g_prefs.remove(kKeyPairOwner);
  g_prefs.remove(kKeyPairLabel);
  g_prefs.remove(kKeyPairHash);
  g_prefs.end();
  return true;
}

bool hasPairing() {
  String ownerId;
  String ownerLabel;
  String secretHash;
  return loadPairing(ownerId, ownerLabel, secretHash);
}

bool verifyPairing(const String &ownerId, const String &pairingSecret) {
  String pairedOwner;
  String pairedLabel;
  String pairedHash;
  if (!loadPairing(pairedOwner, pairedLabel, pairedHash)) {
    return false;
  }
  if (ownerId.length() == 0 || pairingSecret.length() == 0) {
    return false;
  }
  String hash = sha256Hex(pairingSecret);
  return ownerId == pairedOwner && hash.length() > 0 && hash == pairedHash;
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
