// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <Arduino.h>

#include "board_config.h"

class PixelStrip {
 public:
  bool begin();
  void showMode(uint8_t mode, uint8_t brightness, uint32_t startAtMs = 0);
  void playEffect(const char* path, uint8_t fallbackMode, uint8_t brightness, bool loop,
                  uint32_t startAtMs = 0);
  void showClock(uint8_t hour, uint8_t minute, uint32_t color, uint8_t brightness,
                 uint32_t startAtMs = 0);
  // These values match LampGo's backend ocean controller, allowing wrist
  // telemetry to drive the P4's full rectangular panel without a remap.
  void startOcean(uint32_t color, uint8_t brightness, uint8_t fillPercent,
                  uint16_t sensitivityPercent, uint8_t edgeHighlightPercent,
                  uint16_t tiltPercent, uint16_t impactPercent, uint16_t dampingPercent,
                  uint32_t startAtMs = 0);
  void updateOcean(float angleDeg, float angularVelocityDps, uint32_t sequence);
  void stopOcean(uint32_t startAtMs = 0);
  // Shows fixed physical LED indices for a visual wiring/orientation check.
  // It never passes through the logical mirror transform.
  void showTopologyTest(uint8_t brightness, uint32_t startAtMs = 0);
  void off(uint32_t startAtMs = 0) { showMode(0, 1, startAtMs); }
  bool ready() const { return queue_ != nullptr && symbols_ != nullptr; }
  uint8_t mode() const { return mode_; }
  uint8_t brightness() const { return brightness_; }

 private:
  enum class CommandType : uint8_t {
    kMode,
    kEffect,
    kClock,
    kTopologyTest,
    kOceanStart,
    kOceanInput,
    kOceanStop,
  };
  struct Command {
    CommandType type;
    uint8_t mode;
    uint8_t brightness;
    bool loop;
    uint32_t startAtMs;
    uint32_t color;
    uint8_t hour;
    uint8_t minute;
    uint8_t fillPercent;
    uint16_t sensitivityPercent;
    uint8_t edgeHighlightPercent;
    uint16_t tiltPercent;
    uint16_t impactPercent;
    uint16_t dampingPercent;
    int16_t angleTenths;
    int16_t velocityTenths;
    uint32_t sequence;
    char path[48];
  };

  static void taskEntry(void* context);
  void taskLoop();
  void render(uint8_t mode, uint32_t phase);
  bool openEffect(const char* path, bool loop);
  void renderEffect(uint32_t phase);
  void renderClock(uint32_t phase);
  void renderOcean(uint32_t phase);
  void renderTopologyTest();
  void transmit();
  void clear();
  void setPixel(int row, int column, uint8_t red, uint8_t green, uint8_t blue);
  void setPixelPhysical(int index, uint8_t red, uint8_t green, uint8_t blue);
  void drawPointPattern(uint8_t mode, uint32_t phase);
  static uint32_t wheel(uint8_t value);

  QueueHandle_t queue_ = nullptr;
  TaskHandle_t task_ = nullptr;
  uint8_t pixels_[BoardConfig::kLedCount][3]{};
  uint8_t effectData_[8192]{};
  size_t effectSize_ = 0;
  uint8_t effectTicks_ = 0;
  uint8_t effectFrames_ = 0;
  uint8_t effectColors_ = 0;
  uint8_t effectWidth_ = 0;
  uint16_t effectFrameBytes_ = 0;
  size_t effectPaletteOffset_ = 0;
  size_t effectTimelineOffset_ = 0;
  size_t effectFramesOffset_ = 0;
  bool effectActive_ = false;
  bool effectLoop_ = false;
  bool clockActive_ = false;
  bool topologyTestActive_ = false;
  bool oceanActive_ = false;
  uint32_t clockColor_ = 0xFFFFFF;
  uint8_t clockHour_ = 0;
  uint8_t clockMinute_ = 0;
  uint32_t oceanColor_ = 0x37D6FF;
  uint8_t oceanFillPercent_ = 45;
  uint16_t oceanSensitivityPercent_ = 100;
  uint8_t oceanEdgeHighlightPercent_ = 45;
  uint16_t oceanTiltPercent_ = 100;
  uint16_t oceanImpactPercent_ = 100;
  uint16_t oceanDampingPercent_ = 130;
  float oceanAngleDeg_ = 0.0f;
  float oceanAngularVelocityDps_ = 0.0f;
  uint32_t oceanSequence_ = 0;
  void* symbols_ = nullptr;
  uint8_t mode_ = 0;
  uint8_t brightness_ = 1;
};
