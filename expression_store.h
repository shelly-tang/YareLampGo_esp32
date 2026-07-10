// SPDX-FileCopyrightText: 2026 @ninsmiracle, @shelly-tang, and LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <stddef.h>

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

bool begin();
const char *lastError();
bool canStageLcd(size_t bytes);
bool saveLedEffect(const char *effectId, const char *json, size_t len);
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
