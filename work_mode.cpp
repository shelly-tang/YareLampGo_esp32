#include "work_mode.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "device_api.h"
#include "mic_stream.h"
#include "net_config.h"

void startCameraServer();
httpd_handle_t getCameraHttpd();

namespace {

bool g_active = false;
unsigned long g_startupMs = 0;

const unsigned long kWifiTimeoutMs = 20000;
const char *kMdnsService = "lampgo-cam";

bool connectWifi(const String &ssid, const String &password) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);

  const String hostname = NetConfig::deviceHostname();
  WiFi.setHostname(hostname.c_str());

  Serial.printf("[work_mode] connecting to \"%s\" (hostname=%s)\n", ssid.c_str(), hostname.c_str());
  WiFi.begin(ssid.c_str(), password.c_str());
  WiFi.setSleep(false);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > kWifiTimeoutMs) {
      Serial.println();
      Serial.println("[work_mode] WiFi connect timeout");
      return false;
    }
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("[work_mode] WiFi connected, IP=%s, RSSI=%d\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

void startMdns() {
  const String hostname = NetConfig::deviceHostname();
  if (!MDNS.begin(hostname.c_str())) {
    Serial.println("[work_mode] mDNS start failed");
    return;
  }
  MDNS.addService(kMdnsService, "tcp", 80);
  MDNS.addServiceTxt(kMdnsService, "tcp", "version", "0.1");
  MDNS.addServiceTxt(kMdnsService, "tcp", "hostname", hostname);
  Serial.printf("[work_mode] mDNS hostname: %s.local\n", hostname.c_str());
  Serial.printf("[work_mode] mDNS service: _%s._tcp\n", kMdnsService);
}

}

namespace WorkMode {

bool start() {
  String ssid, password;
  if (!NetConfig::loadWifi(ssid, password) || ssid.length() == 0) {
    Serial.println("[work_mode] no WiFi credentials in NVS");
    return false;
  }

  if (!connectWifi(ssid, password)) {
    return false;
  }

  startMdns();

  startCameraServer();

  httpd_handle_t server = getCameraHttpd();
  DeviceApi::registerHandlers(server);

  if (MicStream::begin()) {
    MicStream::registerWsHandler(server);
  }

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");
  Serial.printf("Also reachable at http://%s.local\n", NetConfig::deviceHostname().c_str());

  g_active = true;
  g_startupMs = millis();
  return true;
}

void loop() {
  static unsigned long lastCheck = 0;
  unsigned long now = millis();
  if (now - lastCheck < 5000) {
    delay(50);
    return;
  }
  lastCheck = now;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[work_mode] WiFi dropped, reconnecting...");
    WiFi.reconnect();
  }
}

bool isActive() {
  return g_active;
}

unsigned long uptimeSeconds() {
  if (g_startupMs == 0) {
    return 0;
  }
  return (millis() - g_startupMs) / 1000UL;
}

}
