// SPDX-FileCopyrightText: 2026 @ninsmiracle, @shelly-tang, and LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace LedClipPlayer {

constexpr uint8_t kWidth = 51;
constexpr uint8_t kHeight = 9;
constexpr uint16_t kPixelCount = 447;
constexpr uint8_t kFps = 10;
constexpr uint8_t kTickCount = 30;
constexpr uint16_t kFrameBytes = (kPixelCount + 1) / 2;
constexpr uint16_t kHeaderBytes = 32;

struct ColorOverride {
  bool enabled;
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};

struct Overrides {
  ColorOverride primary;
  ColorOverride secondary;
  ColorOverride accent;
};

bool validatePath(const char *path);
bool open(const char *effectId, const Overrides &overrides);
void close();
bool isOpen();
bool readTick(uint8_t tick, uint8_t *packedPixels, size_t packedLen);
uint32_t paletteColor(uint8_t paletteIndex);
uint8_t paletteIndexAt(const uint8_t *packedPixels, uint16_t pixelIndex);
const char *lastError();

}  // namespace LedClipPlayer
