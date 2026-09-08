// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <Arduino.h>
#include <SCServo.h>

#include "motion_protocol.h"

class ServoExecutor {
 public:
  bool begin();
  bool submitProfile(const MotionProfile& profile);
  bool submitTorque(const char* requestId, uint8_t client, bool enabled);
  bool submitEstop(const char* requestId, uint8_t client);
  bool submitClearEstop(const char* requestId, uint8_t client);
  void submitFrame(const MotionFrame& frame);
  bool pollEvent(ServoEvent& event);
  ServoSnapshot snapshot() const;

 private:
  static void taskEntry(void* context);
  void taskLoop();
  void handleControl(const ServoControl& control);
  void applyProfile(const ServoControl& control);
  void setTorque(const ServoControl& control);
  void applyFrame(const MotionFrame& frame);
  void refreshOneServo();
  bool refreshAllServos();
  void releaseTorque();
  bool allExpectedServosOnline() const;
  bool positionsInsideProfile(char* reason, size_t reasonSize) const;
  void publishEvent(const ServoControl& control, bool ok, const char* error = nullptr);
  int indexForId(uint8_t id) const;

  SMS_STS bus_;
  QueueHandle_t controlQueue_ = nullptr;
  QueueHandle_t frameQueue_ = nullptr;
  QueueHandle_t eventQueue_ = nullptr;
  TaskHandle_t task_ = nullptr;
  mutable portMUX_TYPE stateMux_ = portMUX_INITIALIZER_UNLOCKED;
  ServoSnapshot snapshot_{};
  MotionProfile profile_{};
  uint8_t feedbackIndex_ = 0;
  uint32_t lastFeedbackMs_ = 0;
};
