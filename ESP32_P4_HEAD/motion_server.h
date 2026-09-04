// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsServer.h>

#include "pairing_store.h"
#include "servo_executor.h"

class MotionServer {
 public:
  MotionServer(ServoExecutor& servos, PairingStore& pairing);
  void begin();
  void loop();

 private:
  void onEvent(uint8_t client, WStype_t type, uint8_t* payload, size_t length);
  void onText(uint8_t client, const uint8_t* payload, size_t length);
  void handleHello(uint8_t client, JsonObjectConst message);
  void handleProfile(uint8_t client, JsonObjectConst message);
  void handleFrame(uint8_t client, JsonObjectConst message);
  void handleControl(uint8_t client, JsonObjectConst message);
  void sendError(uint8_t client, const char* requestId, const char* error);
  void sendEvent(const ServoEvent& event);
  void sendTelemetry();
  void addSnapshot(JsonObject response, const ServoSnapshot& snapshot) const;
  static void copyJsonString(char* destination, size_t size, JsonVariantConst value);

  ServoExecutor& servos_;
  PairingStore& pairing_;
  WebSocketsServer socket_;
  bool authenticated_[WEBSOCKETS_SERVER_CLIENT_MAX]{};
  uint32_t pairingRevision_[WEBSOCKETS_SERVER_CLIENT_MAX]{};
  MotionProfile activeProfile_{};
  uint32_t lastTelemetryMs_ = 0;
};
