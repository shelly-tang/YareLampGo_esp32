// SPDX-FileCopyrightText: 2026 @ninsmiracle, @shelly-tang, and LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef LAMPGO_LED_SERIAL_H
#define LAMPGO_LED_SERIAL_H

#include <stdint.h>

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

bool begin();
bool isReady();

bool setMode(int mode);
bool setModeLocal(int mode, bool loop = true, uint32_t durationMs = 0);
bool setModeName(const char *name);
bool setBrightness(int brightness);
bool playClip(const char *clipId);
bool playEffect(const EffectConfig &config);
bool stopExpression(bool syncDisplay = true);

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

int txPin();
int rxPin();
uint32_t baudRate();

}

#endif
