// SPDX-FileCopyrightText: 2026 @ninsmiracle, @shelly-tang, and LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "led_clip_player.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <string.h>

#include "expression_store.h"

namespace LedClipPlayer {
namespace {

#pragma pack(push, 1)
struct Header {
  char magic[4];
  uint8_t version;
  uint8_t width;
  uint8_t height;
  uint8_t fps;
  uint8_t tickCount;
  uint8_t uniqueFrameCount;
  uint8_t paletteCount;
  uint8_t flags;
  uint16_t frameBytes;
  uint16_t headerBytes;
  uint32_t payloadBytes;
  uint32_t crc32;
  uint8_t primaryIndex;
  uint8_t secondaryIndex;
  uint8_t accentIndex;
  uint8_t reserved[5];
};
#pragma pack(pop)

static_assert(sizeof(Header) == kHeaderBytes, "LEF1 header size changed");

File g_file;
Header g_header{};
uint32_t g_palette[16] = {};
uint8_t g_timeline[kTickCount] = {};
uint32_t g_framesOffset = 0;
char g_lastError[96] = "";

void setError(const char *message) {
  snprintf(g_lastError, sizeof(g_lastError), "%s", message ? message : "unknown");
}

uint32_t updateCrc32(uint32_t crc, const uint8_t *data, size_t len) {
  for (size_t index = 0; index < len; ++index) {
    crc ^= data[index];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)-(int32_t)(crc & 1));
    }
  }
  return crc;
}

bool validRoleIndex(uint8_t value, uint8_t paletteCount) {
  return value == 255 || value < paletteCount;
}

bool readAndValidateHeader(File &file, Header &header) {
  if (!file || file.size() < kHeaderBytes || file.read((uint8_t *)&header, sizeof(header)) != sizeof(header)) {
    setError("LEF1 header is truncated");
    return false;
  }
  if (memcmp(header.magic, "LEF1", 4) != 0 || header.version != 1) {
    setError("unsupported LED package");
    return false;
  }
  if (header.width != kWidth || header.height != kHeight || header.fps != kFps ||
      header.tickCount != kTickCount || header.frameBytes != kFrameBytes ||
      header.headerBytes != kHeaderBytes || header.flags != 0) {
    setError("LED package topology mismatch");
    return false;
  }
  if (header.uniqueFrameCount < 1 || header.uniqueFrameCount > kTickCount ||
      header.paletteCount < 1 || header.paletteCount > 16 ||
      !validRoleIndex(header.primaryIndex, header.paletteCount) ||
      !validRoleIndex(header.secondaryIndex, header.paletteCount) ||
      !validRoleIndex(header.accentIndex, header.paletteCount)) {
    setError("LED package header is invalid");
    return false;
  }
  uint32_t expectedPayload = (uint32_t)header.paletteCount * 3UL + header.tickCount +
                             (uint32_t)header.uniqueFrameCount * header.frameBytes;
  if (header.payloadBytes != expectedPayload || file.size() != header.headerBytes + header.payloadBytes) {
    setError("LED package payload size mismatch");
    return false;
  }
  return true;
}

void applyOverride(uint8_t paletteIndex, const ColorOverride &color) {
  if (!color.enabled || paletteIndex == 255 || paletteIndex >= 16) return;
  g_palette[paletteIndex] = ((uint32_t)color.red << 16) |
                            ((uint32_t)color.green << 8) |
                            color.blue;
}

}  // namespace

bool validatePath(const char *path) {
  File file = SPIFFS.open(path, "r");
  Header header{};
  if (!readAndValidateHeader(file, header)) {
    if (file) file.close();
    return false;
  }
  uint32_t crc = 0xFFFFFFFFUL;
  uint8_t buffer[256];
  uint32_t remaining = header.payloadBytes;
  while (remaining > 0) {
    size_t want = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
    int read = file.read(buffer, want);
    if (read <= 0) {
      file.close();
      setError("LED package payload is truncated");
      return false;
    }
    crc = updateCrc32(crc, buffer, (size_t)read);
    remaining -= (uint32_t)read;
  }
  file.close();
  crc ^= 0xFFFFFFFFUL;
  if (crc != header.crc32) {
    setError("LED package CRC mismatch");
    return false;
  }
  g_lastError[0] = 0;
  return true;
}

bool open(const char *effectId, const Overrides &overrides) {
  close();
  char path[72];
  if (!ExpressionStore::ledEffectPath(effectId, path, sizeof(path)) || !validatePath(path)) return false;
  g_file = SPIFFS.open(path, "r");
  if (!readAndValidateHeader(g_file, g_header)) {
    close();
    return false;
  }
  for (uint8_t index = 0; index < g_header.paletteCount; ++index) {
    uint8_t rgb[3];
    if (g_file.read(rgb, sizeof(rgb)) != sizeof(rgb)) {
      close();
      setError("LED palette is truncated");
      return false;
    }
    g_palette[index] = ((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | rgb[2];
  }
  if (g_palette[0] != 0 || g_file.read(g_timeline, g_header.tickCount) != g_header.tickCount) {
    close();
    setError("LED palette or timeline is invalid");
    return false;
  }
  for (uint8_t tick = 0; tick < g_header.tickCount; ++tick) {
    if (g_timeline[tick] >= g_header.uniqueFrameCount) {
      close();
      setError("LED timeline frame index is invalid");
      return false;
    }
  }
  g_framesOffset = g_header.headerBytes + (uint32_t)g_header.paletteCount * 3UL + g_header.tickCount;
  applyOverride(g_header.primaryIndex, overrides.primary);
  applyOverride(g_header.secondaryIndex, overrides.secondary);
  applyOverride(g_header.accentIndex, overrides.accent);
  g_lastError[0] = 0;
  return true;
}

void close() {
  if (g_file) g_file.close();
  memset(&g_header, 0, sizeof(g_header));
  memset(g_palette, 0, sizeof(g_palette));
  memset(g_timeline, 0, sizeof(g_timeline));
  g_framesOffset = 0;
}

bool isOpen() {
  return (bool)g_file;
}

bool readTick(uint8_t tick, uint8_t *packedPixels, size_t packedLen) {
  if (!g_file || !packedPixels || packedLen < kFrameBytes) {
    setError("LED clip is not open");
    return false;
  }
  uint8_t normalizedTick = tick % g_header.tickCount;
  uint32_t offset = g_framesOffset + (uint32_t)g_timeline[normalizedTick] * g_header.frameBytes;
  if (!g_file.seek(offset, SeekSet) || g_file.read(packedPixels, g_header.frameBytes) != g_header.frameBytes) {
    setError("LED frame read failed");
    return false;
  }
  return true;
}

uint32_t paletteColor(uint8_t paletteIndex) {
  return paletteIndex < g_header.paletteCount ? g_palette[paletteIndex] : 0;
}

uint8_t paletteIndexAt(const uint8_t *packedPixels, uint16_t pixelIndex) {
  if (!packedPixels || pixelIndex >= kPixelCount) return 0;
  uint8_t packed = packedPixels[pixelIndex / 2];
  return (pixelIndex & 1) ? packed & 0x0F : packed >> 4;
}

const char *lastError() {
  return g_lastError[0] ? g_lastError : "ok";
}

}  // namespace LedClipPlayer
