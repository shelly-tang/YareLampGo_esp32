// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

/**
 * LampGo ESP32-P4 head-board runtime.
 *
 * Board target: esp32:esp32:esp32p4
 * C6 Wi-Fi/BLE is accessed through the module's internal ESP-Hosted SDIO link;
 * GPIO14..19 and GPIO54 must remain untouched by the application.
 */

#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>

#include "audio_bridge.h"
#include "board_config.h"
#include "camera_controller.h"
#include "device_http.h"
#include "display_controller.h"
#include "expression_coordinator.h"
#include "motion_server.h"
#include "pairing_store.h"
#include "pixel_strip.h"
#include "servo_executor.h"

namespace {
ServoExecutor gServos;
PairingStore gPairing;
CameraController gCamera;
AudioBridge gAudio(gPairing);
PixelStrip gPixels;
DisplayController gDisplay;
ExpressionCoordinator gExpressions(gDisplay, gPixels);
MotionServer gMotion(gServos, gPairing);
DeviceHttp* gHttp = nullptr;
String gHostname;

String deviceSuffix() {
  const uint64_t mac = ESP.getEfuseMac();
  char suffix[5]{};
  snprintf(suffix, sizeof(suffix), "%04X", static_cast<uint16_t>(mac & 0xFFFF));
  return String(suffix);
}

bool connectNetwork() {
  Preferences preferences;
  preferences.begin("lampgo-net", true);
  const String ssid = preferences.getString("ssid", "");
  const String password = preferences.getString("password", "");
  preferences.end();

  if (!ssid.isEmpty()) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(gHostname.c_str());
    WiFi.begin(ssid.c_str(), password.c_str());
    const uint32_t deadline = millis() + 15000;
    while (WiFi.status() != WL_CONNECTED && static_cast<int32_t>(millis() - deadline) < 0) {
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WIFI READY] mode=sta ssid=%s ip=%s\n", ssid.c_str(),
                    WiFi.localIP().toString().c_str());
      return true;
    }
    WiFi.disconnect();
  }

  // Keep STA enabled while the setup AP is active so /scan can enumerate the
  // user's nearby networks without tearing down the provisioning connection.
  WiFi.mode(WIFI_AP_STA);
  const String setupSsid = String(BoardConfig::kSetupSsidPrefix) + deviceSuffix();
  const bool ready = WiFi.softAP(setupSsid.c_str(), BoardConfig::kSetupPassword);
  Serial.printf("[WIFI READY] mode=softap ssid=%s password=%s ip=%s ready=%d\n",
                setupSsid.c_str(), BoardConfig::kSetupPassword, WiFi.softAPIP().toString().c_str(),
                ready);
  return ready;
}

void startMdns() {
  if (!MDNS.begin(gHostname.c_str())) {
    Serial.println("[MDNS] failed");
    return;
  }
  MDNS.addService("lampgo-cam", "tcp", BoardConfig::kHttpPort);
  MDNS.addServiceTxt("lampgo-cam", "tcp", "version", BoardConfig::kFirmwareVersion);
  MDNS.addServiceTxt("lampgo-cam", "tcp", "hostname", gHostname);
  MDNS.addServiceTxt("lampgo-cam", "tcp", "platform", "esp32-p4");
  MDNS.addServiceTxt("lampgo-cam", "tcp", "audio_port", String(BoardConfig::kAudioWsPort));
  MDNS.addServiceTxt("lampgo-cam", "tcp", "motion_port", String(BoardConfig::kMotionWsPort));
}
}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  delay(200);
  Serial.println();
  Serial.printf("LampGo %s starting\n", BoardConfig::kFirmwareVersion);
  Serial.printf("PSRAM found=%d size=%lu free=%lu\n", psramFound(),
                static_cast<unsigned long>(ESP.getPsramSize()),
                static_cast<unsigned long>(ESP.getFreePsram()));
  if (!psramFound()) {
    Serial.println("[FATAL] PSRAM is required for the P4 display/camera runtime");
  }

  gHostname = "lampgo-p4-" + deviceSuffix();
  // A new custom partition table leaves the asset filesystem unformatted.
  // Format only that partition on first mount failure; Wi-Fi and pairing NVS
  // live in separate partitions and are intentionally preserved.
  const bool storageReady = LittleFS.begin(true);
  Serial.printf("[STORAGE] LittleFS ready=%d\n", storageReady);
  Serial.printf("[PAIRING] ready=%d paired=%d\n", gPairing.begin(), gPairing.isPaired());

  Serial.printf("[LED] ready=%d count=%u\n", gPixels.begin(), BoardConfig::kLedCount);
  Serial.printf("[LCD] ready=%d size=%ux%u\n", gDisplay.begin(), BoardConfig::kLcdWidth,
                BoardConfig::kLcdHeight);
  Serial.printf("[CAMERA] ready=%d\n", gCamera.begin());
  Serial.printf("[SERVO] executor ready=%d baud=%lu\n", gServos.begin(),
                static_cast<unsigned long>(BoardConfig::kServoBaud));

  if (!connectNetwork()) {
    Serial.println("[FATAL] ESP32-C6 hosted Wi-Fi failed");
  }
  Serial.printf("[AUDIO] bridge ready=%d\n", gAudio.begin());
  startMdns();
  gHttp = new DeviceHttp(gPairing, gServos, gExpressions, gCamera, gAudio, gHostname);
  gHttp->begin();
  gMotion.begin();
  // Stay visually dark until the paired backend requests an expression. This
  // keeps boot current deterministic while the 486-pixel power budget is
  // still awaiting whole-device validation.
  gDisplay.showFace(0, millis() + 30);
  gPixels.off(millis() + 30);
}

void loop() {
  if (gHttp) gHttp->loop();
  gMotion.loop();
  delay(1);
}
