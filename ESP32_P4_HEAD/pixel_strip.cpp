// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pixel_strip.h"

#include <algorithm>
#include <cmath>
#include <LittleFS.h>

#include "esp32-hal-rmt.h"
#include "esp_heap_caps.h"

namespace {
constexpr uint32_t kRmtFrequency = 10000000;
constexpr uint16_t kFramePeriodMs = 40;
// A 486-pixel frame has 11,664 symbols. Reserve all four P4 TX memory blocks
// (192 symbols) instead of the Arduino default of 48, reducing refill pressure
// fourfold without competing for the GDMA channels used by camera, I2S and
// ESP-Hosted.
constexpr rmt_reserve_memsize_t kRmtMemoryBlocks = RMT_MEM_NUM_BLOCKS_4;
constexpr uint8_t kLegacyRowLengths[] = {47, 49, 51, 51, 51, 51, 51, 49, 47};
constexpr size_t kEffectHeaderBytes = 32;

uint16_t readU16(const uint8_t* data) {
  return data[0] | static_cast<uint16_t>(data[1]) << 8;
}

uint32_t readU32(const uint8_t* data) {
  return data[0] | static_cast<uint32_t>(data[1]) << 8 |
         static_cast<uint32_t>(data[2]) << 16 | static_cast<uint32_t>(data[3]) << 24;
}
}  // namespace

bool PixelStrip::begin() {
  if (!rmtInit(BoardConfig::kLedData, RMT_TX_MODE, kRmtMemoryBlocks, kRmtFrequency)) {
    return false;
  }
  rmtSetEOT(BoardConfig::kLedData, LOW);
  queue_ = xQueueCreate(1, sizeof(Command));
  if (!queue_) {
    return false;
  }
  constexpr size_t symbolCount = BoardConfig::kLedCount * 24;
  symbols_ = heap_caps_malloc(symbolCount * sizeof(rmt_data_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  if (!symbols_) {
    return false;
  }
  clear();
  transmit();
  return xTaskCreate(taskEntry, "led-render", 6144, this, 2, &task_) == pdPASS;
}

void PixelStrip::showMode(uint8_t mode, uint8_t brightness, uint32_t startAtMs) {
  if (!queue_) {
    return;
  }
  Command command{};
  command.type = CommandType::kMode;
  command.mode = static_cast<uint8_t>(std::min<int>(mode, 33));
  command.brightness = static_cast<uint8_t>(
      std::max<int>(1, std::min<int>(brightness, BoardConfig::kLedSafeBrightness)));
  command.loop = true;
  command.startAtMs = startAtMs;
  xQueueOverwrite(queue_, &command);
}

void PixelStrip::playEffect(const char* path, uint8_t fallbackMode, uint8_t brightness, bool loop,
                            uint32_t startAtMs) {
  if (!queue_) return;
  Command command{};
  command.type = CommandType::kEffect;
  command.mode = static_cast<uint8_t>(std::min<int>(fallbackMode, 33));
  command.brightness = static_cast<uint8_t>(
      std::max<int>(1, std::min<int>(brightness, BoardConfig::kLedSafeBrightness)));
  command.loop = loop;
  command.startAtMs = startAtMs;
  strlcpy(command.path, path, sizeof(command.path));
  xQueueOverwrite(queue_, &command);
}

void PixelStrip::showClock(uint8_t hour, uint8_t minute, uint32_t color, uint8_t brightness,
                           ClockEffect effect, uint32_t startAtMs) {
  if (!queue_) return;
  Command command{};
  command.type = CommandType::kClock;
  command.brightness = static_cast<uint8_t>(
      std::max<int>(1, std::min<int>(brightness, BoardConfig::kLedSafeBrightness)));
  command.loop = true;
  command.startAtMs = startAtMs;
  command.color = color;
  command.hour = static_cast<uint8_t>(std::min<int>(hour, 23));
  command.minute = static_cast<uint8_t>(std::min<int>(minute, 59));
  command.clockEffect = effect;
  xQueueOverwrite(queue_, &command);
}

void PixelStrip::startOcean(uint32_t color, uint8_t brightness, uint8_t fillPercent,
                            uint16_t sensitivityPercent, uint8_t edgeHighlightPercent,
                            uint16_t tiltPercent, uint16_t impactPercent, uint16_t dampingPercent,
                            uint32_t startAtMs) {
  if (!queue_) return;
  Command command{};
  command.type = CommandType::kOceanStart;
  command.brightness = static_cast<uint8_t>(
      std::max<int>(1, std::min<int>(brightness, BoardConfig::kLedSafeBrightness)));
  command.startAtMs = startAtMs;
  command.color = color;
  command.fillPercent = std::min<uint8_t>(fillPercent, 100);
  command.sensitivityPercent = std::max<uint16_t>(25, std::min<uint16_t>(sensitivityPercent, 200));
  command.edgeHighlightPercent = std::min<uint8_t>(edgeHighlightPercent, 100);
  command.tiltPercent = std::max<uint16_t>(50, std::min<uint16_t>(tiltPercent, 160));
  command.impactPercent = std::min<uint16_t>(impactPercent, 200);
  command.dampingPercent = std::max<uint16_t>(80, std::min<uint16_t>(dampingPercent, 200));
  xQueueOverwrite(queue_, &command);
}

void PixelStrip::updateOcean(float angleDeg, float angularVelocityDps, uint32_t sequence) {
  if (!queue_) return;
  Command command{};
  command.type = CommandType::kOceanInput;
  command.angleTenths = static_cast<int16_t>(
      std::max(-350.0f, std::min(350.0f, angleDeg)) * 10.0f);
  command.velocityTenths = static_cast<int16_t>(
      std::max(-1800.0f, std::min(1800.0f, angularVelocityDps)) * 10.0f);
  command.sequence = sequence;
  xQueueOverwrite(queue_, &command);
}

void PixelStrip::stopOcean(uint32_t startAtMs) {
  if (!queue_) return;
  Command command{};
  command.type = CommandType::kOceanStop;
  command.startAtMs = startAtMs;
  xQueueOverwrite(queue_, &command);
}

void PixelStrip::showTopologyTest(uint8_t brightness, uint32_t startAtMs) {
  if (!queue_) return;
  Command command{};
  command.type = CommandType::kTopologyTest;
  // This is a calibration image, not illumination.  Keep it visibly dim even
  // if a caller accidentally sends a larger brightness value.
  command.brightness = static_cast<uint8_t>(std::max<int>(1, std::min<int>(brightness, 8)));
  command.startAtMs = startAtMs;
  xQueueOverwrite(queue_, &command);
}

void PixelStrip::taskEntry(void* context) {
  static_cast<PixelStrip*>(context)->taskLoop();
}

void PixelStrip::taskLoop() {
  uint32_t phase = 0;
  uint32_t nextFrame = millis();
  uint32_t startAt = 0;
  bool frameDirty = true;
  for (;;) {
    Command command{};
    if (xQueueReceive(queue_, &command, 0) == pdTRUE) {
      if (command.type == CommandType::kOceanInput) {
        // Ignore reordered telemetry so a delayed browser/device packet cannot
        // make the water visibly jump backwards.
        if (command.sequence >= oceanSequence_) {
          oceanAngleDeg_ = static_cast<float>(command.angleTenths) / 10.0f;
          oceanAngularVelocityDps_ = static_cast<float>(command.velocityTenths) / 10.0f;
          oceanSequence_ = command.sequence;
        }
      } else {
        mode_ = command.mode;
        brightness_ = command.brightness;
        effectActive_ = command.type == CommandType::kEffect && openEffect(command.path, command.loop);
        clockActive_ = command.type == CommandType::kClock;
        topologyTestActive_ = command.type == CommandType::kTopologyTest;
        oceanActive_ = command.type == CommandType::kOceanStart;
        if (clockActive_) {
          clockHour_ = command.hour;
          clockMinute_ = command.minute;
          clockColor_ = command.color;
          clockEffect_ = command.clockEffect;
        }
        if (oceanActive_) {
          oceanColor_ = command.color;
          oceanFillPercent_ = command.fillPercent;
          oceanSensitivityPercent_ = command.sensitivityPercent;
          oceanEdgeHighlightPercent_ = command.edgeHighlightPercent;
          oceanTiltPercent_ = command.tiltPercent;
          oceanImpactPercent_ = command.impactPercent;
          oceanDampingPercent_ = command.dampingPercent;
          oceanAngleDeg_ = 0.0f;
          oceanAngularVelocityDps_ = 0.0f;
          oceanSequence_ = 0;
        }
        if (command.type == CommandType::kOceanStop) {
          mode_ = 0;
          brightness_ = 1;
        }
        startAt = command.startAtMs;
        phase = 0;
        frameDirty = true;
        nextFrame = std::max(millis(), startAt);
      }
    }
    const uint32_t now = millis();
    const bool animatedClock = clockActive_ && clockEffect_ != ClockEffect::kSteady;
    const bool renderContinuously = animatedClock || oceanActive_ || effectActive_ ||
                                    (!clockActive_ && !topologyTestActive_ && !oceanActive_ &&
                                     !effectActive_ && mode_ != 0);
    if (static_cast<int32_t>(now - nextFrame) >= 0 && static_cast<int32_t>(now - startAt) >= 0) {
      if (frameDirty || renderContinuously) {
        if (topologyTestActive_) {
          renderTopologyTest();
        } else if (clockActive_) {
          renderClock(phase);
        } else if (oceanActive_) {
          renderOcean(phase);
        } else if (effectActive_) {
          renderEffect(phase);
          if (!effectLoop_ && phase >= effectTicks_) effectActive_ = false;
        } else {
          render(mode_, phase);
        }
        transmit();
        frameDirty = false;
        if (renderContinuously) ++phase;
      }
      nextFrame = now + kFramePeriodMs;
    }
    vTaskDelay(pdMS_TO_TICKS(4));
  }
}

void PixelStrip::renderClock(uint32_t phase) {
  // Five-by-seven numerals use the rectangular panel deliberately: the old
  // 3x5 clock was designed for the tapered 51x9 matrix and looked undersized
  // on a full-width 54x9 board.
  static constexpr uint8_t kDigits[10][7] = {
      {0b11111, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b11111},
      {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110},
      {0b11110, 0b00001, 0b00001, 0b01110, 0b10000, 0b10000, 0b11111},
      {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110},
      {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010},
      {0b11111, 0b10000, 0b10000, 0b11110, 0b00001, 0b00001, 0b11110},
      {0b01110, 0b10000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110},
      {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000},
      {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110},
      {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00001, 0b01110},
  };
  clear();
  const uint8_t red = clockColor_ >> 16;
  const uint8_t green = clockColor_ >> 8;
  const uint8_t blue = clockColor_;
  const uint8_t values[4] = {static_cast<uint8_t>(clockHour_ / 10),
                             static_cast<uint8_t>(clockHour_ % 10),
                             static_cast<uint8_t>(clockMinute_ / 10),
                             static_cast<uint8_t>(clockMinute_ % 10)};
  constexpr int kClockWidth = 25;
  constexpr int kStartX = (BoardConfig::kLedWidth - kClockWidth) / 2;
  constexpr int kStartY = 1;
  constexpr uint8_t kDigitX[4] = {0, 6, 14, 20};
  const bool visible = clockEffect_ != ClockEffect::kBlink || (phase / 12) % 2 == 0;
  if (visible) {
    for (uint8_t digit = 0; digit < 4; ++digit) {
      const int x = kStartX + kDigitX[digit];
      for (uint8_t row = 0; row < 7; ++row) {
        for (uint8_t column = 0; column < 5; ++column) {
          if (kDigits[values[digit]][row] & (1U << (4 - column))) {
            setPixel(kStartY + row, x + column, red, green, blue);
          }
        }
      }
    }
    setPixel(kStartY + 2, kStartX + 12, red, green, blue);
    setPixel(kStartY + 5, kStartX + 12, red, green, blue);
  }
  if (clockEffect_ == ClockEffect::kOrbit) {
    const int perimeter = 2 * BoardConfig::kLedWidth + 2 * (BoardConfig::kLedHeight - 2);
    int position = phase % perimeter;
    int row = 0;
    int column = 0;
    if (position < BoardConfig::kLedWidth) {
      column = position;
    } else if ((position -= BoardConfig::kLedWidth) < BoardConfig::kLedHeight - 1) {
      column = BoardConfig::kLedWidth - 1;
      row = position;
    } else if ((position -= BoardConfig::kLedHeight - 1) < BoardConfig::kLedWidth) {
      column = BoardConfig::kLedWidth - 1 - position;
      row = BoardConfig::kLedHeight - 1;
    } else {
      position -= BoardConfig::kLedWidth;
      row = BoardConfig::kLedHeight - 1 - position;
    }
    setPixel(row, column, red, green, blue);
  }
}

void PixelStrip::renderOcean(uint32_t phase) {
  clear();
  const uint8_t red = oceanColor_ >> 16;
  const uint8_t green = oceanColor_ >> 8;
  const uint8_t blue = oceanColor_;
  const float fillRows = static_cast<float>(BoardConfig::kLedHeight) * oceanFillPercent_ / 100.0f;
  const float baseLine = static_cast<float>(BoardConfig::kLedHeight) - fillRows;
  const float tilt = oceanAngleDeg_ / 35.0f * oceanTiltPercent_ / 160.0f * 3.0f;
  const float motion = std::min(1.0f, fabsf(oceanAngularVelocityDps_) / 180.0f);
  const float amplitude = 0.35f + oceanImpactPercent_ / 200.0f * (0.45f + 1.2f * motion);
  const float damping = 0.05f + (200.0f - oceanDampingPercent_) / 120.0f * 0.22f;
  const float speed = 0.06f + oceanSensitivityPercent_ / 200.0f * 0.22f;
  const float time = phase * speed;
  const uint8_t edgeScale = 70 + oceanEdgeHighlightPercent_ * 185 / 100;

  for (int column = 0; column < BoardConfig::kLedWidth; ++column) {
    const float position = static_cast<float>(column) / (BoardConfig::kLedWidth - 1) * 2.0f - 1.0f;
    const float wave = sinf(column * 0.34f + time) * amplitude +
                       sinf(column * 0.13f - time * 0.67f) * amplitude * damping * 2.0f;
    const int waterLine = static_cast<int>(roundf(std::max(0.0f, std::min(
        static_cast<float>(BoardConfig::kLedHeight), baseLine + position * tilt + wave))));
    for (int row = waterLine; row < BoardConfig::kLedHeight; ++row) {
      const uint8_t depthScale = static_cast<uint8_t>(
          150 + (row - waterLine) * 105 / std::max(1, BoardConfig::kLedHeight - waterLine));
      setPixel(row, column, static_cast<uint16_t>(red) * depthScale / 255,
               static_cast<uint16_t>(green) * depthScale / 255,
               static_cast<uint16_t>(blue) * depthScale / 255);
    }
    if (waterLine >= 0 && waterLine < BoardConfig::kLedHeight) {
      setPixel(waterLine, column, static_cast<uint16_t>(red) * edgeScale / 255,
               static_cast<uint16_t>(green) * edgeScale / 255,
               static_cast<uint16_t>(blue) * edgeScale / 255);
    }
  }
}

void PixelStrip::renderTopologyTest() {
  clear();
  // Colors identify serial positions, not screen coordinates. They expose
  // the two ends of the first columns and the final left column, even if a
  // logical orientation setting is wrong.
  setPixelPhysical(0, 255, 0, 0);
  setPixelPhysical(BoardConfig::kLedHeight - 1, 0, 255, 0);
  setPixelPhysical(BoardConfig::kLedHeight, 0, 0, 255);
  setPixelPhysical(2 * BoardConfig::kLedHeight - 1, 255, 255, 0);
  setPixelPhysical((BoardConfig::kLedWidth - 1) * BoardConfig::kLedHeight, 0, 255, 255);
  setPixelPhysical(BoardConfig::kLedCount - 1, 255, 0, 255);
}

bool PixelStrip::openEffect(const char* path, bool loop) {
  File file = LittleFS.open(path, "r");
  if (!file || file.size() < kEffectHeaderBytes || file.size() > sizeof(effectData_)) return false;
  effectSize_ = file.read(effectData_, sizeof(effectData_));
  file.close();
  if (effectSize_ < kEffectHeaderBytes || memcmp(effectData_, "LEF1", 4) != 0 ||
      effectData_[4] != 1 ||
      (effectData_[5] != 51 && effectData_[5] != BoardConfig::kLedWidth) ||
      effectData_[6] != BoardConfig::kLedHeight || effectData_[7] != 10) {
    return false;
  }
  effectWidth_ = effectData_[5];
  effectTicks_ = effectData_[8];
  effectFrames_ = effectData_[9];
  effectColors_ = effectData_[10];
  effectFrameBytes_ = readU16(effectData_ + 12);
  const uint16_t headerBytes = readU16(effectData_ + 14);
  const uint32_t payloadBytes = readU32(effectData_ + 16);
  const uint16_t effectPixels = effectWidth_ * BoardConfig::kLedHeight;
  if (headerBytes != kEffectHeaderBytes || effectTicks_ == 0 || effectTicks_ > 30 ||
      effectFrames_ == 0 || effectFrames_ > 30 || effectColors_ == 0 || effectColors_ > 16 ||
      effectFrameBytes_ != (effectPixels + 1) / 2 ||
      static_cast<size_t>(headerBytes) + payloadBytes != effectSize_) {
    return false;
  }
  effectPaletteOffset_ = headerBytes;
  effectTimelineOffset_ = effectPaletteOffset_ + effectColors_ * 3U;
  effectFramesOffset_ = effectTimelineOffset_ + effectTicks_;
  if (effectFramesOffset_ + effectFrames_ * effectFrameBytes_ > effectSize_) return false;
  for (uint8_t tick = 0; tick < effectTicks_; ++tick) {
    if (effectData_[effectTimelineOffset_ + tick] >= effectFrames_) return false;
  }
  effectLoop_ = loop;
  return true;
}

void PixelStrip::renderEffect(uint32_t phase) {
  clear();
  if (effectTicks_ == 0) return;
  const uint8_t tick = effectLoop_ ? phase % effectTicks_ : std::min<uint32_t>(phase, effectTicks_ - 1);
  const uint8_t frameIndex = effectData_[effectTimelineOffset_ + tick];
  const uint8_t* frame = effectData_ + effectFramesOffset_ + frameIndex * effectFrameBytes_;
  if (effectWidth_ == BoardConfig::kLedWidth) {
    // Native P4 assets are already packed in this panel's serpentine physical
    // order.  They use all 486 pixels and must not pass through the legacy
    // tapered 51-column transform below.
    for (uint16_t index = 0; index < BoardConfig::kLedCount; ++index) {
      const uint8_t packed = frame[index / 2];
      const uint8_t paletteIndex =
          (index & 1) ? packed & 0x0F : static_cast<uint8_t>(packed >> 4);
      if (paletteIndex >= effectColors_) continue;
      const uint8_t* color = effectData_ + effectPaletteOffset_ + paletteIndex * 3U;
      setPixelPhysical(index, color[0], color[1], color[2]);
    }
    return;
  }
  // LEF1 v1 is the historical 447-pixel S3 layout.  Its bytes are in wired
  // order (bottom/right-facing), not editor row order.  Decode that legacy
  // transform first, then centre its 51-column canvas on the P4's 54-column
  // panel.  Treating the bytes as top-left rows was the source of rotated and
  // scrambled imported images on the new board.
  uint16_t legacyIndex = 0;
  constexpr int kLegacyCanvasWidth = 51;
  constexpr int kP4LeftPad = (BoardConfig::kLedWidth - kLegacyCanvasWidth) / 2;
  for (uint8_t wiredRow = 0; wiredRow < BoardConfig::kLedHeight; ++wiredRow) {
    const uint8_t rowLength = kLegacyRowLengths[wiredRow];
    const uint8_t legacyLeftPad = (kLegacyCanvasWidth - rowLength) / 2;
    for (uint8_t wiredColumn = 0; wiredColumn < rowLength; ++wiredColumn, ++legacyIndex) {
      const uint8_t packed = frame[legacyIndex / 2];
      const uint8_t paletteIndex =
          (legacyIndex & 1) ? packed & 0x0F : static_cast<uint8_t>(packed >> 4);
      if (paletteIndex >= effectColors_) continue;
      const uint8_t* color = effectData_ + effectPaletteOffset_ + paletteIndex * 3U;
      const int sourceRow = BoardConfig::kLedHeight - 1 - wiredRow;
      const int sourceColumn = kLegacyCanvasWidth - 1 - (legacyLeftPad + wiredColumn);
      setPixel(sourceRow, kP4LeftPad + sourceColumn, color[0], color[1], color[2]);
    }
  }
}

void PixelStrip::render(uint8_t mode, uint32_t phase) {
  clear();
  if (mode == 0) {
    return;
  }
  if (mode >= 1 && mode <= 4) {
    const uint8_t progress = std::min<int>(BoardConfig::kLedWidth, phase % (BoardConfig::kLedWidth + 8));
    const uint8_t red = mode == 1 || mode == 4 ? 255 : 0;
    const uint8_t green = mode == 2 || mode == 4 ? 255 : 0;
    const uint8_t blue = mode == 3 || mode == 4 ? 255 : 0;
    for (int row = 0; row < BoardConfig::kLedHeight; ++row) {
      for (int column = 0; column < progress; ++column) {
        setPixel(row, column, red, green, blue);
      }
    }
    return;
  }
  if (mode >= 5 && mode <= 8) {
    const uint8_t red = mode == 6 ? 255 : mode == 5 ? 180 : 0;
    const uint8_t green = mode == 7 ? 255 : mode == 5 ? 90 : 0;
    const uint8_t blue = mode == 8 ? 255 : mode == 5 ? 255 : 0;
    // Advance logical columns, not physical indices.  The latter zig-zagged
    // through every odd serpentine row and made the rectangular panel look
    // corrupted despite correct wiring.
    for (int column = phase % 3; column < BoardConfig::kLedWidth; column += 3) {
      for (int row = 0; row < BoardConfig::kLedHeight; ++row) {
        setPixel(row, column, red, green, blue);
      }
    }
    return;
  }
  if (mode == 9 || mode == 10) {
    for (int column = 0; column < BoardConfig::kLedWidth; ++column) {
      const uint32_t color = wheel(static_cast<uint8_t>(column * 255 / BoardConfig::kLedWidth + phase * 3));
      for (int row = 0; row < BoardConfig::kLedHeight; ++row) {
        if (mode == 9 || ((row + column + phase) % 3 == 0)) {
          setPixel(row, column, color >> 16, color >> 8, color);
        }
      }
    }
    return;
  }
  drawPointPattern(mode, phase);
}

void PixelStrip::drawPointPattern(uint8_t mode, uint32_t phase) {
  const int centreX = BoardConfig::kLedWidth / 2;
  const int centreY = BoardConfig::kLedHeight / 2;
  const uint8_t pulse = static_cast<uint8_t>(160 + 95 * (0.5f + 0.5f * sinf(phase * 0.22f)));
  uint8_t red = 80, green = 170, blue = 255;
  if (mode == 23 || mode == 25 || mode == 32) red = 255, green = 40, blue = 90;
  if (mode == 26) red = 255, green = 35, blue = 0;
  if (mode == 27 || mode == 28 || mode == 29) red = 100, green = 90, blue = 255;
  if (mode == 31) red = 80, green = 255, blue = 150;
  red = static_cast<uint16_t>(red) * pulse / 255;
  green = static_cast<uint16_t>(green) * pulse / 255;
  blue = static_cast<uint16_t>(blue) * pulse / 255;

  for (int row = 0; row < BoardConfig::kLedHeight; ++row) {
    for (int column = 0; column < BoardConfig::kLedWidth; ++column) {
      const int x = column - centreX;
      const int y = row - centreY;
      bool on = false;
      switch (mode) {
        case 11: on = (x < 0 && abs(y) <= (-x) / 2 && x > -14) || (abs(y) <= 1 && x < 12); break;
        case 12: on = (x > 0 && abs(y) <= x / 2 && x < 14) || (abs(y) <= 1 && x > -12); break;
        case 13: on = (y < 1 && abs(x) <= -y * 2 && y > -5) || (abs(x) <= 1 && y < 4); break;
        case 14: on = (y > -1 && abs(x) <= y * 2 && y < 5) || (abs(x) <= 1 && y > -4); break;
        case 15: on = abs(y - (x + 5) / 3) <= 1 && x < 0;
                 on = on || (x >= -1 && abs(y + x / 3) <= 1); break;
        case 16: on = abs(x - y * 3) <= 1 || abs(x + y * 3) <= 1; break;
        case 17: on = abs(x) <= 1 && (y < 2 || y == 4); break;
        case 18: on = (abs(x) < 6 && (y == -3 || abs(x) == 5)) || (abs(x) <= 1 && y >= 0); break;
        case 19: on = abs(x) + abs(y) * 3 < 11 || (abs(x) < 3 && abs(y) < 4); break;
        case 20: on = ((column + phase) % 13 < 2 && row >= 2) || (row > 5 && (column + phase) % 13 < 5); break;
        case 21: on = (y == 2 && abs(x) < 8) || (y == 1 && abs(x) >= 7 && abs(x) < 10); break;
        case 22: on = (y == 2 && abs(x) >= 7 && abs(x) < 10) || (y == 3 && abs(x) < 8); break;
        case 23: on = (abs(x) + abs(y * 2) < 12 && y >= -2) || (y < 0 && (abs(x - 5) < 5 || abs(x + 5) < 5)); break;
        case 24: on = x * x / 36 + y * y < 9; break;
        case 25: on = y >= 1 && abs(x) > 8 && abs(x) < 17; break;
        case 26: on = (y == -2 && abs(x) > 4 && abs(x) < 14) || (y == 3 && abs(x) < 8); break;
        case 27: on = ((column + phase / 3) % 11 == 0) || (abs(x) < 2 && y > 0); break;
        case 28: on = ((column + phase / 4) % 17 < 3 && row == (column / 4) % 5); break;
        case 29: on = (abs(x) > 6 && abs(x) < 9 && abs(y) < 3) || (y == 3 && abs(x) < 4); break;
        case 30: on = (y == -1 && abs(x) < 14) || (y == 0 && abs(x) > 3 && abs(x) < 13); break;
        case 31: on = abs(y) <= 1 && abs(x) < 6 + static_cast<int>(phase % 8); break;
        case 32: on = (y == 1 && x > -10 && x < -2) || (y == 2 && x > 3 && x < 10); break;
        case 33: on = (abs(x) < 15 && (abs(y) == 3 || abs(x) == 14)) || (abs(x) < 2); break;
        default: on = abs(y) <= 1 && abs(x) < 10; break;
      }
      if (on) {
        setPixel(row, column, red, green, blue);
      }
    }
  }
}

void PixelStrip::transmit() {
  constexpr size_t symbolCount = BoardConfig::kLedCount * 24;
  auto* symbols = static_cast<rmt_data_t*>(symbols_);
  if (!symbols) return;

  uint32_t channelSum = 0;
  for (const auto& pixel : pixels_) {
    channelSum += (static_cast<uint32_t>(pixel[0]) + pixel[1] + pixel[2]) * brightness_ / 255U;
  }
  const uint32_t budget = BoardConfig::kLedEquivalentWhiteBudget * 3U * 255U;
  const uint32_t scale = channelSum > budget ? (budget * 65535U / channelSum) : 65535U;

  size_t symbol = 0;
  for (const auto& pixel : pixels_) {
    const uint8_t ordered[3] = {pixel[1], pixel[0], pixel[2]};  // GRB
    for (uint8_t source : ordered) {
      const uint8_t value = static_cast<uint32_t>(source) * brightness_ * scale / (255U * 65535U);
      for (int bit = 7; bit >= 0; --bit) {
        const bool one = value & (1U << bit);
        symbols[symbol].level0 = 1;
        symbols[symbol].duration0 = one ? 8 : 4;
        symbols[symbol].level1 = 0;
        symbols[symbol].duration1 = one ? 4 : 8;
        ++symbol;
      }
    }
  }
  rmtWrite(BoardConfig::kLedData, symbols, symbolCount, 50);
}

void PixelStrip::clear() {
  memset(pixels_, 0, sizeof(pixels_));
}

void PixelStrip::setPixel(int row, int column, uint8_t red, uint8_t green, uint8_t blue) {
  if (row < 0 || row >= BoardConfig::kLedHeight || column < 0 || column >= BoardConfig::kLedWidth) {
    return;
  }
  // The panel is column-serpentine: DIN is the top-right pixel, runs down the
  // first nine-pixel column, then up the next column toward the left. Keep
  // that physical knowledge here so clocks, ocean and authored LED frames all
  // share the same front-facing coordinate system.
  const int logicalRow = BoardConfig::kLedMirrorY ? BoardConfig::kLedHeight - 1 - row : row;
  const int logicalColumn = BoardConfig::kLedMirrorX ? BoardConfig::kLedWidth - 1 - column : column;
  if constexpr (BoardConfig::kLedCascadeVertical) {
    const int cascadeColumn = BoardConfig::kLedDataStartsAtRight
                                  ? BoardConfig::kLedWidth - 1 - logicalColumn
                                  : logicalColumn;
    const bool reverseColumn = static_cast<bool>(cascadeColumn & 1) !=
                               BoardConfig::kLedFirstColumnTopDown;
    const int cascadeRow = reverseColumn ? BoardConfig::kLedHeight - 1 - logicalRow : logicalRow;
    setPixelPhysical(cascadeColumn * BoardConfig::kLedHeight + cascadeRow, red, green, blue);
    return;
  }
  const int physicalColumn = (logicalRow & 1) ? BoardConfig::kLedWidth - 1 - logicalColumn : logicalColumn;
  setPixelPhysical(logicalRow * BoardConfig::kLedWidth + physicalColumn, red, green, blue);
}

void PixelStrip::setPixelPhysical(int index, uint8_t red, uint8_t green, uint8_t blue) {
  if (index < 0 || index >= BoardConfig::kLedCount) {
    return;
  }
  pixels_[index][0] = red;
  pixels_[index][1] = green;
  pixels_[index][2] = blue;
}

uint32_t PixelStrip::wheel(uint8_t value) {
  value = 255 - value;
  if (value < 85) return ((255 - value * 3) << 16) | (value * 3);
  if (value < 170) {
    value -= 85;
    return (value * 3 << 8) | (255 - value * 3);
  }
  value -= 170;
  return (value * 3 << 16) | ((255 - value * 3) << 8);
}
