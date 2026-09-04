// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "motion_server.h"

#include <algorithm>
#include <WiFi.h>

#include "board_config.h"

MotionServer::MotionServer(ServoExecutor& servos, PairingStore& pairing)
    : servos_(servos), pairing_(pairing), socket_(BoardConfig::kMotionWsPort) {}

void MotionServer::begin() {
  socket_.begin();
  socket_.enableHeartbeat(15000, 3000, 2);
  socket_.onEvent([this](uint8_t client, WStype_t type, uint8_t* payload, size_t length) {
    onEvent(client, type, payload, length);
  });
  Serial.printf("[MOTION READY] ws://%s:%u/ws/motion\n", WiFi.localIP().toString().c_str(),
                BoardConfig::kMotionWsPort);
}

void MotionServer::loop() {
  socket_.loop();
  ServoEvent event{};
  while (servos_.pollEvent(event)) {
    sendEvent(event);
  }
  if (millis() - lastTelemetryMs_ >= 50) {
    sendTelemetry();
    lastTelemetryMs_ = millis();
  }
}

void MotionServer::onEvent(uint8_t client, WStype_t type, uint8_t* payload, size_t length) {
  if (client >= WEBSOCKETS_SERVER_CLIENT_MAX) {
    return;
  }
  switch (type) {
    case WStype_CONNECTED:
      authenticated_[client] = false;
      pairingRevision_[client] = 0;
      Serial.printf("[MOTION] client=%u connected path=%.*s\n", client, static_cast<int>(length), payload);
      break;
    case WStype_DISCONNECTED:
      authenticated_[client] = false;
      pairingRevision_[client] = 0;
      Serial.printf("[MOTION] client=%u disconnected\n", client);
      break;
    case WStype_TEXT:
      onText(client, payload, length);
      break;
    default:
      break;
  }
}

void MotionServer::onText(uint8_t client, const uint8_t* payload, size_t length) {
  if (length == 0 || length > 32 * 1024) {
    sendError(client, "", "invalid message size");
    return;
  }
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, payload, length);
  if (error || !document.is<JsonObject>()) {
    sendError(client, "", "invalid JSON message");
    return;
  }
  JsonObjectConst message = document.as<JsonObjectConst>();
  const String type = message["type"] | "";
  if (type == "hello") {
    handleHello(client, message);
    return;
  }
  if (!authenticated_[client] || pairingRevision_[client] != pairing_.revision()) {
    authenticated_[client] = false;
    sendError(client, message["request_id"] | "", "motion client is not authenticated");
    return;
  }
  if (type == "profile") {
    handleProfile(client, message);
  } else if (type == "frame") {
    handleFrame(client, message);
  } else if (type == "control") {
    handleControl(client, message);
  } else {
    sendError(client, message["request_id"] | "", "unsupported motion message");
  }
}

void MotionServer::handleHello(uint8_t client, JsonObjectConst message) {
  const char* requestId = message["request_id"] | "";
  const String protocol = message["protocol"] | "";
  const String ownerId = message["owner_id"] | "";
  const String secret = message["pairing_secret"] | "";
  if (protocol != BoardConfig::kMotionProtocol) {
    sendError(client, requestId, "unsupported motion protocol");
    return;
  }
  if (!pairing_.authorize(ownerId, secret)) {
    sendError(client, requestId, pairing_.isPaired() ? "pairing mismatch" : "device is not paired");
    return;
  }
  authenticated_[client] = true;
  pairingRevision_[client] = pairing_.revision();

  JsonDocument response;
  response["type"] = "hello";
  response["ok"] = true;
  response["request_id"] = requestId;
  response["protocol"] = BoardConfig::kMotionProtocol;
  response["firmware"] = BoardConfig::kFirmwareVersion;
  addSnapshot(response.as<JsonObject>(), servos_.snapshot());
  String encoded;
  serializeJson(response, encoded);
  socket_.sendTXT(client, encoded);
}

void MotionServer::handleProfile(uint8_t client, JsonObjectConst message) {
  JsonArrayConst joints = message["joints"].as<JsonArrayConst>();
  if (joints.size() != BoardConfig::kServoCount) {
    sendError(client, message["request_id"] | "", "profile must contain five joints");
    return;
  }

  MotionProfile profile{};
  profile.client = client;
  copyJsonString(profile.requestId, sizeof(profile.requestId), message["request_id"]);
  copyJsonString(profile.profileSha256, sizeof(profile.profileSha256), message["profile_sha256"]);
  copyJsonString(profile.lampId, sizeof(profile.lampId), message["lamp_id"]);
  profile.maxTorquePct = std::max(10, std::min(100, message["max_torque_pct"] | 80));
  profile.commandTimeoutMs = std::max(100, std::min(1000, message["command_timeout_ms"] | 250));
  profile.releaseTimeoutMs =
      std::max<int>(profile.commandTimeoutMs, std::min(10000, message["release_timeout_ms"] | 2000));
  profile.jointCount = 0;
  for (JsonObjectConst source : joints) {
    JointProfile& joint = profile.joints[profile.jointCount++];
    copyJsonString(joint.name, sizeof(joint.name), source["name"]);
    joint.id = source["id"] | 0;
    joint.rangeMin = source["range_min"] | 0;
    joint.rangeMax = source["range_max"] | 0;
    joint.homingOffset = source["homing_offset"] | 0;
  }
  activeProfile_ = profile;
  if (!servos_.submitProfile(profile)) {
    sendError(client, profile.requestId, "servo control queue is full");
  }
}

void MotionServer::handleFrame(uint8_t, JsonObjectConst message) {
  if (activeProfile_.jointCount != BoardConfig::kServoCount) {
    return;
  }
  JsonObjectConst targets = message["targets"].as<JsonObjectConst>();
  if (targets.isNull()) {
    return;
  }
  MotionFrame frame{};
  frame.sequence = message["seq"] | 0;
  frame.moveTimeMs = message["move_time_ms"] | 20;
  for (uint8_t index = 0; index < activeProfile_.jointCount; ++index) {
    char id[4]{};
    snprintf(id, sizeof(id), "%u", activeProfile_.joints[index].id);
    JsonVariantConst value = targets[id];
    if (!value.isNull()) {
      frame.positions[index] = value.as<uint16_t>();
      frame.present[index] = true;
    }
  }
  servos_.submitFrame(frame);
}

void MotionServer::handleControl(uint8_t client, JsonObjectConst message) {
  const char* requestId = message["request_id"] | "";
  const String op = message["op"] | "";
  bool accepted = false;
  if (op == "torque") {
    accepted = servos_.submitTorque(requestId, client, message["enabled"] | false);
  } else if (op == "estop") {
    accepted = servos_.submitEstop(requestId, client);
  } else if (op == "clear_estop") {
    accepted = servos_.submitClearEstop(requestId, client);
  } else {
    sendError(client, requestId, "unsupported control operation");
    return;
  }
  if (!accepted) {
    sendError(client, requestId, "servo control queue is full");
  }
}

void MotionServer::sendError(uint8_t client, const char* requestId, const char* error) {
  JsonDocument response;
  response["type"] = "error";
  response["ok"] = false;
  response["request_id"] = requestId;
  response["error"] = error;
  String encoded;
  serializeJson(response, encoded);
  socket_.sendTXT(client, encoded);
}

void MotionServer::sendEvent(const ServoEvent& event) {
  JsonDocument response;
  response["type"] = "ack";
  response["ok"] = event.ok;
  response["request_id"] = event.requestId;
  if (event.error[0] != '\0') {
    response["error"] = event.error;
  }
  addSnapshot(response.as<JsonObject>(), servos_.snapshot());
  String encoded;
  serializeJson(response, encoded);
  socket_.sendTXT(event.client, encoded);
}

void MotionServer::sendTelemetry() {
  bool hasClient = false;
  for (uint8_t client = 0; client < WEBSOCKETS_SERVER_CLIENT_MAX; ++client) {
    if (authenticated_[client] && pairingRevision_[client] != pairing_.revision()) {
      authenticated_[client] = false;
      pairingRevision_[client] = 0;
      servos_.submitTorque("pairing-revoked", client, false);
      socket_.disconnect(client);
    }
    hasClient = hasClient || authenticated_[client];
  }
  if (!hasClient) {
    return;
  }
  JsonDocument response;
  response["type"] = "telemetry";
  response["ok"] = true;
  addSnapshot(response.as<JsonObject>(), servos_.snapshot());
  String encoded;
  serializeJson(response, encoded);
  for (uint8_t client = 0; client < WEBSOCKETS_SERVER_CLIENT_MAX; ++client) {
    if (authenticated_[client]) {
      socket_.sendTXT(client, encoded);
    }
  }
}

void MotionServer::addSnapshot(JsonObject response, const ServoSnapshot& snapshot) const {
  response["startup_state"] = startupStateName(snapshot.startupState);
  response["recovery_reason"] = snapshot.recoveryReason;
  response["torque_enabled"] = snapshot.torqueEnabled;
  response["estopped"] = snapshot.estopped;
  response["last_command_ms"] = snapshot.lastCommandMs;
  response["safety_clamp_count"] = snapshot.safetyClampCount;
  JsonObject positions = response["positions"].to<JsonObject>();
  JsonArray onlineIds = response["online_ids"].to<JsonArray>();
  JsonObject metrics = response["metrics"].to<JsonObject>();
  const uint8_t count = activeProfile_.jointCount == 0 ? BoardConfig::kServoCount : activeProfile_.jointCount;
  for (uint8_t index = 0; index < count; ++index) {
    const uint8_t id = activeProfile_.jointCount == 0 ? index + 1 : activeProfile_.joints[index].id;
    char key[4]{};
    snprintf(key, sizeof(key), "%u", id);
    if (snapshot.online[index]) {
      onlineIds.add(id);
      positions[key] = snapshot.position[index];
    }
    JsonObject metric = metrics[key].to<JsonObject>();
    metric["online"] = snapshot.online[index];
    metric["speed_raw"] = snapshot.speed[index];
    metric["load_raw"] = snapshot.load[index];
    metric["voltage_raw"] = snapshot.voltage[index];
    metric["temperature_raw"] = snapshot.temperature[index];
    metric["current_raw"] = snapshot.current[index];
  }
}

void MotionServer::copyJsonString(char* destination, size_t size, JsonVariantConst value) {
  strlcpy(destination, value.as<const char*>() ? value.as<const char*>() : "", size);
}
