// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <Arduino.h>

#include "board_config.h"

enum class MotorStartupState : uint8_t {
  kDisconnected,
  kReady,
  kRecoveryRequired,
  kHardFault,
};

struct JointProfile {
  char name[24];
  uint8_t id;
  uint16_t rangeMin;
  uint16_t rangeMax;
  int16_t homingOffset;
};

struct MotionProfile {
  char requestId[40];
  char profileSha256[65];
  char lampId[24];
  JointProfile joints[BoardConfig::kServoCount];
  uint8_t jointCount;
  uint8_t maxTorquePct;
  uint16_t commandTimeoutMs;
  uint16_t releaseTimeoutMs;
  uint8_t client;
};

struct MotionFrame {
  uint32_t sequence;
  uint16_t moveTimeMs;
  uint16_t positions[BoardConfig::kServoCount];
  bool present[BoardConfig::kServoCount];
};

struct ServoSnapshot {
  bool online[BoardConfig::kServoCount];
  int16_t position[BoardConfig::kServoCount];
  int16_t speed[BoardConfig::kServoCount];
  int16_t load[BoardConfig::kServoCount];
  int16_t voltage[BoardConfig::kServoCount];
  int16_t temperature[BoardConfig::kServoCount];
  int16_t current[BoardConfig::kServoCount];
  bool torqueEnabled;
  bool estopped;
  bool profileLoaded;
  uint32_t lastCommandMs;
  uint32_t safetyClampCount;
  MotorStartupState startupState;
  char recoveryReason[128];
};

enum class ServoControlType : uint8_t {
  kProfile,
  kTorque,
  kEstop,
  kClearEstop,
};

struct ServoControl {
  ServoControlType type;
  char requestId[40];
  uint8_t client;
  bool enabled;
  MotionProfile profile;
};

struct ServoEvent {
  char requestId[40];
  uint8_t client;
  bool ok;
  char error[128];
};

inline const char* startupStateName(MotorStartupState state) {
  switch (state) {
    case MotorStartupState::kReady:
      return "ready";
    case MotorStartupState::kRecoveryRequired:
      return "recovery_required";
    case MotorStartupState::kHardFault:
      return "hard_fault";
    default:
      return "disconnected";
  }
}
