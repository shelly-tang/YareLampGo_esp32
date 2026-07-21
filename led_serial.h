// SPDX-FileCopyrightText: 2026 @ninsmiracle, @shelly-tang, and LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef LAMPGO_LED_SERIAL_H
#define LAMPGO_LED_SERIAL_H

#include <stddef.h>
#include <stdint.h>

#include "led_clip_player.h"

namespace LedSerial {

struct EffectConfig {
  const char *effectId;
  const char *templateName;
  const char *variant;
  const char *direction;
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  uint8_t secondaryRed;
  uint8_t secondaryGreen;
  uint8_t secondaryBlue;
  uint8_t brightness;
  uint8_t intensityPercent;
  bool loop;
  uint32_t durationMs;
};

struct StoredEffectConfig {
  const char *effectId;
  LedClipPlayer::Overrides colors;
  uint8_t brightness;
  uint8_t intensityPercent;
  bool loop;
  uint32_t durationMs;
};

struct ClockConfig {
  uint8_t hour;
  uint8_t minute;
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  uint8_t brightness;
  const char *effect;
};

struct OceanConfig {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  uint8_t brightness;
  uint8_t fillPercent;
  uint8_t sensitivityPercent;
  uint8_t edgeHighlightPercent;
  uint8_t tiltPercent;
  uint8_t impactPercent;
  uint8_t dampingPercent;
};

bool begin();
bool isReady();

bool setMode(int mode);
bool setModeLocal(int mode, bool loop = true, uint32_t durationMs = 0);
bool setModeName(const char *name);
bool setBrightness(int brightness);
bool playClip(const char *clipId);
bool playEffect(const EffectConfig &config);
bool playStoredEffect(const StoredEffectConfig &config);
bool stopExpression(bool syncDisplay = true);
bool showClock(const ClockConfig &config);
bool stopClock();
bool startOcean(const OceanConfig &config);
bool updateOceanInput(float angleDeg, float angularVelocityDps, uint32_t sequence);
bool stopOcean();

int resolveMode(const char *name);
const char *modeName(int mode);
int maxMode();

int currentMode();
int currentBrightness();
uint32_t lastWriteMs();
const char *lastCommand();

const char *driverName();
int pixelPin();
int pixelCount();
int panelCount();
bool outputOk();
bool clockActive();
const char *clockEffect();
void clockTime(char *out, size_t outLen);
bool oceanActive();
uint32_t oceanInputAgeMs();
uint8_t oceanRenderFps();

int txPin();
int rxPin();
uint32_t baudRate();

}

#endif
