// SPDX-FileCopyrightText: 2026 @ninsmiracle, @shelly-tang, and LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "expression_store.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <ctype.h>
#include <string.h>

namespace ExpressionStore {
namespace {

bool g_ready = false;
char g_lastError[96] = "";

void setError(const char *message) {
  snprintf(g_lastError, sizeof(g_lastError), "%s", message ? message : "unknown");
}

bool sanitizeId(const char *input, char *out, size_t outLen) {
  if (!input || !input[0] || !out || outLen < 2) {
    setError("invalid asset id");
    return false;
  }
  size_t used = 0;
  for (const char *cursor = input; *cursor; ++cursor) {
    unsigned char ch = (unsigned char)*cursor;
    if (!(isalnum(ch) || ch == '-' || ch == '_') || used + 1 >= outLen) {
      setError("invalid asset id");
      return false;
    }
    out[used++] = (char)tolower(ch);
  }
  out[used] = 0;
  return used > 0;
}

bool buildPath(const char *prefix, const char *id, const char *suffix, char *out, size_t outLen) {
  char safe[40];
  if (!sanitizeId(id, safe, sizeof(safe))) return false;
  int written = snprintf(out, outLen, "/%s_%s.%s", prefix, safe, suffix);
  if (written <= 0 || (size_t)written >= outLen) {
    setError("asset path too long");
    return false;
  }
  return true;
}

void scanPrefix(const char *prefix, size_t &bytes, int &count) {
  bytes = 0;
  count = 0;
  File root = SPIFFS.open("/");
  if (!root || !root.isDirectory()) return;
  File file = root.openNextFile();
  while (file) {
    const char *name = file.name();
    const char *normalized = name && name[0] == '/' ? name : nullptr;
    const char *prefixWithoutSlash = prefix && prefix[0] == '/' ? prefix + 1 : prefix;
    bool matches = normalized ? strstr(normalized, prefix) == normalized :
                   (name && prefixWithoutSlash && strstr(name, prefixWithoutSlash) == name);
    if (matches) {
      bytes += file.size();
      count++;
    }
    file = root.openNextFile();
  }
}

bool atomicWrite(const char *path, const char *json, size_t len) {
  char temporary[80];
  int written = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  if (written <= 0 || (size_t)written >= sizeof(temporary)) {
    setError("temporary path too long");
    return false;
  }
  SPIFFS.remove(temporary);
  File file = SPIFFS.open(temporary, "w");
  if (!file) {
    setError("asset temporary open failed");
    return false;
  }
  bool ok = len == 0 || file.write((const uint8_t *)json, len) == len;
  file.flush();
  file.close();
  if (!ok) {
    SPIFFS.remove(temporary);
    setError("asset write failed");
    return false;
  }
  SPIFFS.remove(path);
  if (!SPIFFS.rename(temporary, path)) {
    SPIFFS.remove(temporary);
    setError("asset commit failed");
    return false;
  }
  g_lastError[0] = 0;
  return true;
}

bool saveAsset(const char *prefix,
               const char *id,
               const char *json,
               size_t len,
               size_t singleLimit,
               size_t poolLimit,
               int countLimit) {
  if (!begin()) return false;
  if (!json || len == 0 || len > singleLimit) {
    setError("asset exceeds single-file budget");
    return false;
  }
  char path[72];
  if (!buildPath(prefix, id, "json", path, sizeof(path))) return false;
  size_t used = 0;
  int count = 0;
  char scanPrefixValue[12];
  snprintf(scanPrefixValue, sizeof(scanPrefixValue), "/%s_", prefix);
  scanPrefix(scanPrefixValue, used, count);
  File current = SPIFFS.open(path, "r");
  size_t currentSize = current ? current.size() : 0;
  bool exists = (bool)current;
  if (current) current.close();
  if (!exists && count >= countLimit) {
    setError("asset count budget reached");
    return false;
  }
  if (used - currentSize + len > poolLimit) {
    setError("asset byte budget reached");
    return false;
  }
  Capacity currentCapacity = capacity();
  size_t extra = len > currentSize ? len - currentSize : 0;
  if (currentCapacity.fsFreeBytes < extra + kFsReservedBytes) {
    setError("filesystem safety reserve would be crossed");
    return false;
  }
  return atomicWrite(path, json, len);
}

}  // namespace

bool begin() {
  if (g_ready) return true;
  if (!SPIFFS.begin(true)) {
    setError("SPIFFS mount failed");
    return false;
  }
  g_ready = true;
  return true;
}

const char *lastError() {
  return g_lastError[0] ? g_lastError : "ok";
}

Capacity capacity() {
  Capacity result{};
  if (!begin()) return result;
  result.fsTotalBytes = SPIFFS.totalBytes();
  result.fsUsedBytes = SPIFFS.usedBytes();
  result.fsFreeBytes = result.fsTotalBytes > result.fsUsedBytes ? result.fsTotalBytes - result.fsUsedBytes : 0;
  scanPrefix("/le_", result.ledUsedBytes, result.ledCount);
  scanPrefix("/ep_", result.presetUsedBytes, result.presetCount);
  return result;
}

bool canStageLcd(size_t bytes) {
  if (!begin()) return false;
  if (bytes == 0 || bytes > kLcdStagingBytes) {
    setError("LCD package exceeds staging budget");
    return false;
  }
  Capacity current = capacity();
  if (current.fsFreeBytes < bytes + kFsReservedBytes) {
    setError("filesystem safety reserve would be crossed");
    return false;
  }
  return true;
}

bool saveLedEffect(const char *effectId, const char *json, size_t len) {
  return saveAsset("le", effectId, json, len, kSingleLedBytes, kLedBudgetBytes, kMaxLedEffects);
}

bool savePreset(const char *presetId, const char *json, size_t len) {
  return saveAsset("ep", presetId, json, len, kSinglePresetBytes, kPresetBudgetBytes, kMaxPresets);
}

}  // namespace ExpressionStore
