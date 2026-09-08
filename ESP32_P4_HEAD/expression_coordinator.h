// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <Arduino.h>

#include "display_controller.h"
#include "pixel_strip.h"

class ExpressionCoordinator {
 public:
  ExpressionCoordinator(DisplayController& display, PixelStrip& pixels)
      : display_(display), pixels_(pixels) {}

  bool play(const String& expression, int mode, const String& eyeClipId,
            const String& ledEffectId, uint8_t brightness, bool loop);
  void setBrightness(uint8_t brightness);
  void showClock(uint8_t hour, uint8_t minute, uint32_t color, uint8_t brightness,
                 ClockEffect effect);
  void startOcean(uint32_t color, uint8_t brightness, uint8_t fillPercent,
                  uint16_t sensitivityPercent, uint8_t edgeHighlightPercent,
                  uint16_t tiltPercent, uint16_t impactPercent, uint16_t dampingPercent);
  void updateOcean(float angleDeg, float angularVelocityDps, uint32_t sequence);
  void stopOcean();
  void showLedTopologyTest(uint8_t brightness);
  void stop();
  uint8_t currentMode() const { return currentMode_; }
  const String& currentEyeClip() const { return currentEyeClip_; }
  const String& currentLedEffect() const { return currentLedEffect_; }
  uint8_t currentBrightness() const { return currentBrightness_; }
  bool ledReady() const { return pixels_.ready(); }
  bool displayReady() const { return display_.ready(); }
  static int modeForExpression(const String& expression);
  static bool safeAssetId(const String& value);
  static String eyePath(const String& id) { return "/eye_" + id + ".bin"; }
  static String ledPath(const String& id) { return "/led_" + id + ".bin"; }

 private:
  DisplayController& display_;
  PixelStrip& pixels_;
  uint8_t currentMode_ = 0;
  String currentEyeClip_;
  String currentLedEffect_;
  uint8_t currentBrightness_ = 1;
};
