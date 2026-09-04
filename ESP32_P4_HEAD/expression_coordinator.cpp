// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "expression_coordinator.h"

#include <LittleFS.h>

#include "board_config.h"

namespace {
const char* const kExpressionNames[] = {
    "off",       "red",       "green",    "blue",      "white",    "theater",
    "theaterred", "theatergreen", "theaterblue", "rainbow", "rainbowchase",
    "left",      "right",     "up",       "down",      "check",    "cross",
    "exclaim",   "question",  "star",     "music",     "smiley",   "sad",
    "heart",     "surprised", "blush",    "angry",     "thinking", "sleep",
    "helpless",  "cool",      "focused",  "wink",      "myu7gt",
};
constexpr size_t kExpressionCount = sizeof(kExpressionNames) / sizeof(kExpressionNames[0]);
}  // namespace

bool ExpressionCoordinator::play(const String& expression, int mode, const String& eyeClipId,
                                 const String& ledEffectId, uint8_t brightness, bool loop) {
  int resolvedMode = mode;
  if (resolvedMode < 0) {
    resolvedMode = modeForExpression(expression);
  }
  if (resolvedMode < 0 || resolvedMode >= static_cast<int>(kExpressionCount)) {
    return false;
  }
  if ((!eyeClipId.isEmpty() && !safeAssetId(eyeClipId)) ||
      (!ledEffectId.isEmpty() && !safeAssetId(ledEffectId))) {
    return false;
  }

  const uint32_t startAt = millis() + 30;
  const String ledPath = ExpressionCoordinator::ledPath(ledEffectId);
  if (!ledEffectId.isEmpty() && LittleFS.exists(ledPath)) {
    pixels_.playEffect(ledPath.c_str(), static_cast<uint8_t>(resolvedMode), brightness, loop, startAt);
  } else {
    pixels_.showMode(static_cast<uint8_t>(resolvedMode), brightness, startAt);
  }
  const String path = eyePath(eyeClipId);
  if (!eyeClipId.isEmpty() && LittleFS.exists(path)) {
    display_.playClip(path.c_str(), loop, startAt);
  } else {
    display_.showFace(static_cast<uint8_t>(resolvedMode), startAt);
  }
  currentMode_ = resolvedMode;
  currentEyeClip_ = eyeClipId;
  currentLedEffect_ = ledEffectId;
  currentBrightness_ = brightness;
  return true;
}

void ExpressionCoordinator::setBrightness(uint8_t brightness) {
  play("", currentMode_, currentEyeClip_, currentLedEffect_, brightness, true);
}

void ExpressionCoordinator::showClock(uint8_t hour, uint8_t minute, uint32_t color,
                                      uint8_t brightness) {
  pixels_.showClock(hour, minute, color, brightness, millis() + 20);
  currentBrightness_ = brightness;
}

void ExpressionCoordinator::stop() {
  const uint32_t startAt = millis() + 20;
  pixels_.off(startAt);
  display_.showFace(0, startAt);
  currentMode_ = 0;
  currentEyeClip_.clear();
  currentLedEffect_.clear();
  currentBrightness_ = 1;
}

int ExpressionCoordinator::modeForExpression(const String& expression) {
  String normalized = expression;
  normalized.toLowerCase();
  normalized.replace("-", "");
  normalized.replace("_", "");
  if (normalized == "myu7" || normalized == "mgt" || normalized == "yu7") {
    normalized = "myu7gt";
  }
  for (size_t index = 0; index < kExpressionCount; ++index) {
    String candidate = kExpressionNames[index];
    candidate.replace("-", "");
    candidate.replace("_", "");
    if (normalized == candidate) {
      return index;
    }
  }
  return -1;
}

bool ExpressionCoordinator::safeAssetId(const String& value) {
  if (value.isEmpty() || value.length() > 32) return false;
  for (const char character : value) {
    if (!isalnum(static_cast<unsigned char>(character)) && character != '-' && character != '_') {
      return false;
    }
  }
  return true;
}
