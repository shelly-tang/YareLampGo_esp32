// SPDX-FileCopyrightText: 2026 @ninsmiracle, @shelly-tang, and LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ExpressionStore {

struct Capacity {
  size_t fsTotalBytes;
  size_t fsUsedBytes;
  size_t fsFreeBytes;
  size_t ledUsedBytes;
  size_t presetUsedBytes;
  int ledCount;
  int presetCount;
};

struct LedEffectInfo {
  char effectId[40];
  char sha256[65];
  size_t bytes;
};

bool begin();
const char *lastError();
bool canStageLcd(size_t bytes);
bool saveLedEffect(const char *effectId, const char *json, size_t len);
bool beginLedEffectUpload(const char *effectId, size_t expectedBytes, const char *expectedSha256);
bool appendLedEffectUpload(size_t offset, const uint8_t *data, size_t len);
bool commitLedEffectUpload();
void abortLedEffectUpload();
bool removeLedEffect(const char *effectId);
bool hasLedEffect(const char *effectId);
bool ledEffectPath(const char *effectId, char *out, size_t outLen);
int listLedEffects(LedEffectInfo *out, int maxCount);
bool savePreset(const char *presetId, const char *json, size_t len);
Capacity capacity();

constexpr size_t kFsReservedBytes = 256 * 1024;
constexpr size_t kLcdStagingBytes = 256 * 1024;
constexpr size_t kLedBudgetBytes = 192 * 1024;
constexpr size_t kPresetBudgetBytes = 64 * 1024;
constexpr size_t kSingleLedBytes = 8 * 1024;
constexpr size_t kSinglePresetBytes = 1024;
constexpr int kMaxLedEffects = 24;
constexpr int kMaxPresets = 64;

}  // namespace ExpressionStore
