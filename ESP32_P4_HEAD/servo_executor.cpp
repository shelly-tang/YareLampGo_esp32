// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "servo_executor.h"

#include <algorithm>

#include "board_config.h"

namespace {
constexpr uint8_t kAcceleration = 20;
constexpr uint16_t kDefaultMoveTimeMs = 20;
constexpr uint8_t kTorqueLimitAddress = 48;
constexpr uint8_t kAccelerationAddress = 41;
}  // namespace

bool ServoExecutor::begin() {
  controlQueue_ = xQueueCreate(8, sizeof(ServoControl));
  frameQueue_ = xQueueCreate(1, sizeof(MotionFrame));
  eventQueue_ = xQueueCreate(8, sizeof(ServoEvent));
  if (!controlQueue_ || !frameQueue_ || !eventQueue_) {
    return false;
  }

  Serial1.begin(BoardConfig::kServoBaud, SERIAL_8N1, BoardConfig::kServoRx, BoardConfig::kServoTx);
  bus_.pSerial = &Serial1;
  bus_.IOTimeOut = 12;

  snapshot_.startupState = MotorStartupState::kDisconnected;
  snapshot_.torqueEnabled = false;
  snapshot_.estopped = false;
  snapshot_.profileLoaded = false;
  for (uint8_t id = 1; id <= BoardConfig::kServoCount; ++id) {
    bus_.EnableTorque(id, 0);
  }

  return xTaskCreate(taskEntry, "servo-executor", 8192, this, 5, &task_) == pdPASS;
}

bool ServoExecutor::submitProfile(const MotionProfile& profile) {
  ServoControl control{};
  control.type = ServoControlType::kProfile;
  control.client = profile.client;
  strlcpy(control.requestId, profile.requestId, sizeof(control.requestId));
  control.profile = profile;
  return xQueueSend(controlQueue_, &control, pdMS_TO_TICKS(20)) == pdTRUE;
}

bool ServoExecutor::submitTorque(const char* requestId, uint8_t client, bool enabled) {
  ServoControl control{};
  control.type = ServoControlType::kTorque;
  control.client = client;
  control.enabled = enabled;
  strlcpy(control.requestId, requestId, sizeof(control.requestId));
  return xQueueSend(controlQueue_, &control, pdMS_TO_TICKS(20)) == pdTRUE;
}

bool ServoExecutor::submitEstop(const char* requestId, uint8_t client) {
  ServoControl control{};
  control.type = ServoControlType::kEstop;
  control.client = client;
  strlcpy(control.requestId, requestId, sizeof(control.requestId));
  return xQueueSend(controlQueue_, &control, pdMS_TO_TICKS(20)) == pdTRUE;
}

bool ServoExecutor::submitClearEstop(const char* requestId, uint8_t client) {
  ServoControl control{};
  control.type = ServoControlType::kClearEstop;
  control.client = client;
  strlcpy(control.requestId, requestId, sizeof(control.requestId));
  return xQueueSend(controlQueue_, &control, pdMS_TO_TICKS(20)) == pdTRUE;
}

void ServoExecutor::submitFrame(const MotionFrame& frame) {
  xQueueOverwrite(frameQueue_, &frame);
}

bool ServoExecutor::pollEvent(ServoEvent& event) {
  return xQueueReceive(eventQueue_, &event, 0) == pdTRUE;
}

ServoSnapshot ServoExecutor::snapshot() const {
  portENTER_CRITICAL(&stateMux_);
  ServoSnapshot copy = snapshot_;
  portEXIT_CRITICAL(&stateMux_);
  return copy;
}

void ServoExecutor::taskEntry(void* context) {
  static_cast<ServoExecutor*>(context)->taskLoop();
}

void ServoExecutor::taskLoop() {
  for (;;) {
    ServoControl control{};
    while (xQueueReceive(controlQueue_, &control, 0) == pdTRUE) {
      handleControl(control);
    }

    MotionFrame frame{};
    if (xQueueReceive(frameQueue_, &frame, 0) == pdTRUE) {
      applyFrame(frame);
    }

    ServoSnapshot state = snapshot();
    const uint32_t now = millis();
    if (state.torqueEnabled && state.lastCommandMs != 0 && profile_.releaseTimeoutMs != 0 &&
        now - state.lastCommandMs > profile_.releaseTimeoutMs) {
      releaseTorque();
      portENTER_CRITICAL(&stateMux_);
      snapshot_.startupState = MotorStartupState::kHardFault;
      strlcpy(snapshot_.recoveryReason, "motion command timeout; torque released",
              sizeof(snapshot_.recoveryReason));
      portEXIT_CRITICAL(&stateMux_);
    }

    if (now - lastFeedbackMs_ >= 10) {
      refreshOneServo();
      lastFeedbackMs_ = now;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void ServoExecutor::handleControl(const ServoControl& control) {
  switch (control.type) {
    case ServoControlType::kProfile:
      applyProfile(control);
      break;
    case ServoControlType::kTorque:
      setTorque(control);
      break;
    case ServoControlType::kEstop:
      releaseTorque();
      portENTER_CRITICAL(&stateMux_);
      snapshot_.estopped = true;
      snapshot_.startupState = MotorStartupState::kHardFault;
      strlcpy(snapshot_.recoveryReason, "emergency stop active", sizeof(snapshot_.recoveryReason));
      portEXIT_CRITICAL(&stateMux_);
      publishEvent(control, true);
      break;
    case ServoControlType::kClearEstop:
      portENTER_CRITICAL(&stateMux_);
      snapshot_.estopped = false;
      snapshot_.startupState = MotorStartupState::kDisconnected;
      snapshot_.recoveryReason[0] = '\0';
      portEXIT_CRITICAL(&stateMux_);
      publishEvent(control, true);
      break;
  }
}

void ServoExecutor::applyProfile(const ServoControl& control) {
  const MotionProfile& incoming = control.profile;
  bool valid = incoming.jointCount == BoardConfig::kServoCount;
  bool seen[BoardConfig::kServoCount + 1]{};
  for (uint8_t index = 0; valid && index < incoming.jointCount; ++index) {
    const JointProfile& joint = incoming.joints[index];
    valid = joint.id >= 1 && joint.id <= BoardConfig::kServoCount && !seen[joint.id] &&
            joint.rangeMin < joint.rangeMax && joint.rangeMax <= 4095;
    if (valid) {
      seen[joint.id] = true;
    }
  }
  if (!valid) {
    publishEvent(control, false, "invalid calibration profile");
    return;
  }

  releaseTorque();
  profile_ = incoming;
  portENTER_CRITICAL(&stateMux_);
  snapshot_.profileLoaded = true;
  snapshot_.startupState = MotorStartupState::kDisconnected;
  snapshot_.recoveryReason[0] = '\0';
  portEXIT_CRITICAL(&stateMux_);

  if (!refreshAllServos() || !allExpectedServosOnline()) {
    portENTER_CRITICAL(&stateMux_);
    snapshot_.startupState = MotorStartupState::kHardFault;
    strlcpy(snapshot_.recoveryReason, "one or more configured servos are offline",
            sizeof(snapshot_.recoveryReason));
    portEXIT_CRITICAL(&stateMux_);
    publishEvent(control, true);
    return;
  }

  char reason[128]{};
  if (!positionsInsideProfile(reason, sizeof(reason))) {
    portENTER_CRITICAL(&stateMux_);
    snapshot_.startupState = MotorStartupState::kRecoveryRequired;
    strlcpy(snapshot_.recoveryReason, reason, sizeof(snapshot_.recoveryReason));
    portEXIT_CRITICAL(&stateMux_);
    publishEvent(control, true);
    return;
  }

  portENTER_CRITICAL(&stateMux_);
  snapshot_.startupState = MotorStartupState::kReady;
  portEXIT_CRITICAL(&stateMux_);
  publishEvent(control, true);
}

void ServoExecutor::setTorque(const ServoControl& control) {
  if (!control.enabled) {
    releaseTorque();
    publishEvent(control, true);
    return;
  }

  ServoSnapshot state = snapshot();
  if (!state.profileLoaded || state.estopped) {
    publishEvent(control, false, state.estopped ? "emergency stop active" : "profile not loaded");
    return;
  }
  if (!refreshAllServos() || !allExpectedServosOnline()) {
    publishEvent(control, false, "one or more configured servos are offline");
    return;
  }
  char reason[128]{};
  if (!positionsInsideProfile(reason, sizeof(reason))) {
    portENTER_CRITICAL(&stateMux_);
    snapshot_.startupState = MotorStartupState::kRecoveryRequired;
    strlcpy(snapshot_.recoveryReason, reason, sizeof(snapshot_.recoveryReason));
    portEXIT_CRITICAL(&stateMux_);
    publishEvent(control, false, reason);
    return;
  }
  state = snapshot();

  // Seed every goal to its measured position before torque-on. This prevents
  // a stale SRAM Goal_Position from moving the arm as torque is enabled.
  for (uint8_t index = 0; index < profile_.jointCount; ++index) {
    const JointProfile& joint = profile_.joints[index];
    const int slot = indexForId(joint.id);
    const bool configured = bus_.writeByte(joint.id, kAccelerationAddress, kAcceleration) >= 0 &&
                            bus_.writeWord(joint.id, kTorqueLimitAddress,
                                           static_cast<uint16_t>(profile_.maxTorquePct) * 10U) >= 0 &&
                            bus_.WritePosEx(joint.id, state.position[slot], 0, kAcceleration) >= 0;
    if (!configured) {
      releaseTorque();
      publishEvent(control, false, "servo safety profile write failed");
      return;
    }
  }
  for (uint8_t index = 0; index < profile_.jointCount; ++index) {
    if (bus_.EnableTorque(profile_.joints[index].id, 1) < 0) {
      releaseTorque();
      publishEvent(control, false, "torque enable failed");
      return;
    }
  }

  portENTER_CRITICAL(&stateMux_);
  snapshot_.torqueEnabled = true;
  snapshot_.lastCommandMs = millis();
  snapshot_.startupState = MotorStartupState::kReady;
  snapshot_.recoveryReason[0] = '\0';
  portEXIT_CRITICAL(&stateMux_);
  publishEvent(control, true);
}

void ServoExecutor::applyFrame(const MotionFrame& frame) {
  ServoSnapshot state = snapshot();
  if (!state.profileLoaded || !state.torqueEnabled || state.estopped ||
      state.startupState != MotorStartupState::kReady) {
    return;
  }

  uint8_t ids[BoardConfig::kServoCount]{};
  uint8_t data[BoardConfig::kServoCount * 4]{};
  uint8_t count = 0;
  uint32_t clamps = 0;
  const uint16_t moveTime = std::max<uint16_t>(1, std::min<uint16_t>(frame.moveTimeMs, 1000));
  for (uint8_t index = 0; index < profile_.jointCount; ++index) {
    if (!frame.present[index]) {
      continue;
    }
    const JointProfile& joint = profile_.joints[index];
    uint16_t position = frame.positions[index];
    if (position < joint.rangeMin || position > joint.rangeMax) {
      position = std::max(joint.rangeMin, std::min(joint.rangeMax, position));
      ++clamps;
    }
    ids[count] = joint.id;
    data[count * 4] = static_cast<uint8_t>(position & 0xFF);
    data[count * 4 + 1] = static_cast<uint8_t>(position >> 8);
    data[count * 4 + 2] = static_cast<uint8_t>(moveTime & 0xFF);
    data[count * 4 + 3] = static_cast<uint8_t>(moveTime >> 8);
    ++count;
  }
  if (count > 0) {
    bus_.syncWrite(ids, count, SMS_STS_GOAL_POSITION_L, data, 4);
  }
  portENTER_CRITICAL(&stateMux_);
  snapshot_.lastCommandMs = millis();
  snapshot_.safetyClampCount += clamps;
  portEXIT_CRITICAL(&stateMux_);
}

void ServoExecutor::refreshOneServo() {
  if (!snapshot().profileLoaded) {
    return;
  }
  const uint8_t index = feedbackIndex_++ % profile_.jointCount;
  const uint8_t id = profile_.joints[index].id;
  const bool online = bus_.FeedBack(id) >= 0;
  portENTER_CRITICAL(&stateMux_);
  snapshot_.online[index] = online;
  if (online) {
    snapshot_.position[index] = bus_.ReadPos(-1);
    snapshot_.speed[index] = bus_.ReadSpeed(-1);
    snapshot_.load[index] = bus_.ReadLoad(-1);
    snapshot_.voltage[index] = bus_.ReadVoltage(-1);
    snapshot_.temperature[index] = bus_.ReadTemper(-1);
    snapshot_.current[index] = bus_.ReadCurrent(-1);
  }
  portEXIT_CRITICAL(&stateMux_);
}

bool ServoExecutor::refreshAllServos() {
  bool allOnline = true;
  for (uint8_t index = 0; index < profile_.jointCount; ++index) {
    const uint8_t id = profile_.joints[index].id;
    const bool online = bus_.FeedBack(id) >= 0;
    portENTER_CRITICAL(&stateMux_);
    snapshot_.online[index] = online;
    if (online) {
      snapshot_.position[index] = bus_.ReadPos(-1);
      snapshot_.speed[index] = bus_.ReadSpeed(-1);
      snapshot_.load[index] = bus_.ReadLoad(-1);
      snapshot_.voltage[index] = bus_.ReadVoltage(-1);
      snapshot_.temperature[index] = bus_.ReadTemper(-1);
      snapshot_.current[index] = bus_.ReadCurrent(-1);
    }
    portEXIT_CRITICAL(&stateMux_);
    allOnline = allOnline && online;
  }
  return allOnline;
}

void ServoExecutor::releaseTorque() {
  const uint8_t count = profile_.jointCount == 0 ? BoardConfig::kServoCount : profile_.jointCount;
  for (uint8_t index = 0; index < count; ++index) {
    const uint8_t id = profile_.jointCount == 0 ? index + 1 : profile_.joints[index].id;
    bus_.EnableTorque(id, 0);
  }
  portENTER_CRITICAL(&stateMux_);
  snapshot_.torqueEnabled = false;
  portEXIT_CRITICAL(&stateMux_);
}

bool ServoExecutor::allExpectedServosOnline() const {
  ServoSnapshot state = snapshot();
  for (uint8_t index = 0; index < profile_.jointCount; ++index) {
    if (!state.online[index]) {
      return false;
    }
  }
  return true;
}

bool ServoExecutor::positionsInsideProfile(char* reason, size_t reasonSize) const {
  ServoSnapshot state = snapshot();
  for (uint8_t index = 0; index < profile_.jointCount; ++index) {
    const JointProfile& joint = profile_.joints[index];
    const int position = state.position[index];
    if (position < joint.rangeMin || position > joint.rangeMax) {
      snprintf(reason, reasonSize, "servo %u position %d outside %u..%u", joint.id, position,
               joint.rangeMin, joint.rangeMax);
      return false;
    }
  }
  return true;
}

void ServoExecutor::publishEvent(const ServoControl& control, bool ok, const char* error) {
  ServoEvent event{};
  event.client = control.client;
  event.ok = ok;
  strlcpy(event.requestId, control.requestId, sizeof(event.requestId));
  if (error) {
    strlcpy(event.error, error, sizeof(event.error));
  }
  xQueueSend(eventQueue_, &event, 0);
}

int ServoExecutor::indexForId(uint8_t id) const {
  for (uint8_t index = 0; index < profile_.jointCount; ++index) {
    if (profile_.joints[index].id == id) {
      return index;
    }
  }
  return -1;
}
