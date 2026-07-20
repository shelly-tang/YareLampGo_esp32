// SPDX-FileCopyrightText: 2026 @ninsmiracle, @shelly-tang, and LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "expression_store.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <ctype.h>
#include <mbedtls/sha256.h>
#include <string.h>

#include "led_clip_player.h"

namespace ExpressionStore {
namespace {

bool g_ready = false;
char g_lastError[96] = "";
File g_ledUploadFile;
char g_ledUploadId[40] = "";
char g_ledUploadTemporary[80] = "";
char g_ledUploadFinal[80] = "";
char g_ledUploadSha256[65] = "";
size_t g_ledUploadExpected = 0;
size_t g_ledUploadWritten = 0;

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
    bool temporary = name && (strstr(name, ".tmp") || strstr(name, ".bak"));
    if (matches && !temporary) {
      bytes += file.size();
      count++;
    }
    file = root.openNextFile();
  }
}

bool fileSha256Hex(const char *path, char *out, size_t outLen) {
  if (!path || !out || outLen < 65) {
    setError("sha output too small");
    return false;
  }
  File file = SPIFFS.open(path, "r");
  if (!file) {
    setError("LED effect file missing");
    return false;
  }
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  if (mbedtls_sha256_starts(&ctx, 0) != 0) {
    file.close();
    mbedtls_sha256_free(&ctx);
    setError("sha init failed");
    return false;
  }
  uint8_t buffer[256];
  while (file.available()) {
    int read = file.read(buffer, sizeof(buffer));
    if (read < 0 || (read > 0 && mbedtls_sha256_update(&ctx, buffer, (size_t)read) != 0)) {
      file.close();
      mbedtls_sha256_free(&ctx);
      setError("sha read failed");
      return false;
    }
  }
  file.close();
  uint8_t digest[32];
  if (mbedtls_sha256_finish(&ctx, digest) != 0) {
    mbedtls_sha256_free(&ctx);
    setError("sha finish failed");
    return false;
  }
  mbedtls_sha256_free(&ctx);
  static const char *hex = "0123456789abcdef";
  for (int index = 0; index < 32; ++index) {
    out[index * 2] = hex[digest[index] >> 4];
    out[index * 2 + 1] = hex[digest[index] & 0x0F];
  }
  out[64] = 0;
  return true;
}

bool equalsIgnoreCase(const char *left, const char *right) {
  if (!left || !right) return false;
  while (*left && *right) {
    if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) return false;
    ++left;
    ++right;
  }
  return *left == 0 && *right == 0;
}

void clearLedUpload() {
  if (g_ledUploadFile) g_ledUploadFile.close();
  g_ledUploadId[0] = 0;
  g_ledUploadTemporary[0] = 0;
  g_ledUploadFinal[0] = 0;
  g_ledUploadSha256[0] = 0;
  g_ledUploadExpected = 0;
  g_ledUploadWritten = 0;
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

bool ledEffectPath(const char *effectId, char *out, size_t outLen) {
  return buildPath("le", effectId, "lef", out, outLen);
}

bool beginLedEffectUpload(const char *effectId, size_t expectedBytes, const char *expectedSha256) {
  if (!begin()) return false;
  abortLedEffectUpload();
  if (expectedBytes < LedClipPlayer::kHeaderBytes || expectedBytes > kSingleLedBytes ||
      !expectedSha256 || strlen(expectedSha256) != 64) {
    setError("invalid LED effect upload metadata");
    return false;
  }
  if (!sanitizeId(effectId, g_ledUploadId, sizeof(g_ledUploadId)) ||
      !ledEffectPath(g_ledUploadId, g_ledUploadFinal, sizeof(g_ledUploadFinal))) {
    clearLedUpload();
    return false;
  }
  snprintf(g_ledUploadTemporary, sizeof(g_ledUploadTemporary), "%s.tmp", g_ledUploadFinal);

  Capacity current = capacity();
  File existing = SPIFFS.open(g_ledUploadFinal, "r");
  size_t existingSize = existing ? existing.size() : 0;
  bool exists = (bool)existing;
  if (existing) existing.close();
  char legacyPath[72];
  if (buildPath("le", g_ledUploadId, "json", legacyPath, sizeof(legacyPath))) {
    File legacy = SPIFFS.open(legacyPath, "r");
    if (legacy) {
      existingSize += legacy.size();
      exists = true;
      legacy.close();
    }
  }
  if (!exists && current.ledCount >= kMaxLedEffects) {
    clearLedUpload();
    setError("asset count budget reached");
    return false;
  }
  if (current.ledUsedBytes - existingSize + expectedBytes > kLedBudgetBytes) {
    clearLedUpload();
    setError("asset byte budget reached");
    return false;
  }
  if (current.fsFreeBytes < expectedBytes + kFsReservedBytes) {
    clearLedUpload();
    setError("filesystem safety reserve would be crossed");
    return false;
  }
  SPIFFS.remove(g_ledUploadTemporary);
  g_ledUploadFile = SPIFFS.open(g_ledUploadTemporary, "w");
  if (!g_ledUploadFile) {
    clearLedUpload();
    setError("LED effect temporary open failed");
    return false;
  }
  g_ledUploadExpected = expectedBytes;
  snprintf(g_ledUploadSha256, sizeof(g_ledUploadSha256), "%s", expectedSha256);
  g_lastError[0] = 0;
  return true;
}

bool appendLedEffectUpload(size_t offset, const uint8_t *data, size_t len) {
  if (!g_ledUploadFile || !data || len == 0 || offset != g_ledUploadWritten ||
      g_ledUploadWritten + len > g_ledUploadExpected) {
    setError("LED effect upload offset mismatch");
    return false;
  }
  if (g_ledUploadFile.write(data, len) != len) {
    setError("LED effect upload write failed");
    return false;
  }
  g_ledUploadWritten += len;
  return true;
}

bool commitLedEffectUpload() {
  if (!g_ledUploadFile || g_ledUploadWritten != g_ledUploadExpected) {
    setError("LED effect upload is incomplete");
    abortLedEffectUpload();
    return false;
  }
  g_ledUploadFile.flush();
  g_ledUploadFile.close();
  char actualSha256[65];
  if (!fileSha256Hex(g_ledUploadTemporary, actualSha256, sizeof(actualSha256))) {
    SPIFFS.remove(g_ledUploadTemporary);
    clearLedUpload();
    return false;
  }
  if (!equalsIgnoreCase(actualSha256, g_ledUploadSha256)) {
    setError("LED effect SHA256 mismatch");
    SPIFFS.remove(g_ledUploadTemporary);
    clearLedUpload();
    return false;
  }
  if (!LedClipPlayer::validatePath(g_ledUploadTemporary)) {
    setError(LedClipPlayer::lastError());
    SPIFFS.remove(g_ledUploadTemporary);
    clearLedUpload();
    return false;
  }

  char backup[88];
  snprintf(backup, sizeof(backup), "%s.bak", g_ledUploadFinal);
  SPIFFS.remove(backup);
  bool hadPrevious = SPIFFS.exists(g_ledUploadFinal);
  if (hadPrevious && !SPIFFS.rename(g_ledUploadFinal, backup)) {
    SPIFFS.remove(g_ledUploadTemporary);
    clearLedUpload();
    setError("LED effect backup failed");
    return false;
  }
  if (!SPIFFS.rename(g_ledUploadTemporary, g_ledUploadFinal)) {
    if (hadPrevious) SPIFFS.rename(backup, g_ledUploadFinal);
    SPIFFS.remove(g_ledUploadTemporary);
    clearLedUpload();
    setError("LED effect commit failed");
    return false;
  }
  SPIFFS.remove(backup);
  char legacy[72];
  if (buildPath("le", g_ledUploadId, "json", legacy, sizeof(legacy))) SPIFFS.remove(legacy);
  clearLedUpload();
  g_lastError[0] = 0;
  return true;
}

void abortLedEffectUpload() {
  if (g_ledUploadFile) g_ledUploadFile.close();
  if (g_ledUploadTemporary[0]) SPIFFS.remove(g_ledUploadTemporary);
  clearLedUpload();
}

bool removeLedEffect(const char *effectId) {
  if (!begin()) return false;
  char lef[72];
  char json[72];
  if (!ledEffectPath(effectId, lef, sizeof(lef)) || !buildPath("le", effectId, "json", json, sizeof(json))) {
    return false;
  }
  bool existed = SPIFFS.exists(lef) || SPIFFS.exists(json);
  bool ok = (!SPIFFS.exists(lef) || SPIFFS.remove(lef)) &&
            (!SPIFFS.exists(json) || SPIFFS.remove(json));
  if (!existed) setError("LED effect not found");
  else if (!ok) setError("LED effect delete failed");
  else g_lastError[0] = 0;
  return existed && ok;
}

bool hasLedEffect(const char *effectId) {
  if (!begin()) return false;
  char path[72];
  return ledEffectPath(effectId, path, sizeof(path)) && SPIFFS.exists(path);
}

int listLedEffects(LedEffectInfo *out, int maxCount) {
  if (!begin() || !out || maxCount <= 0) return 0;
  int count = 0;
  File root = SPIFFS.open("/");
  if (!root || !root.isDirectory()) return 0;
  File file = root.openNextFile();
  while (file && count < maxCount) {
    String name = file.name();
    int start = name.startsWith("/") ? 1 : 0;
    if (name.startsWith("le_", start) && name.endsWith(".lef")) {
      String id = name.substring(start + 3, name.length() - 4);
      snprintf(out[count].effectId, sizeof(out[count].effectId), "%s", id.c_str());
      out[count].bytes = file.size();
      file.close();
      char path[72];
      if (ledEffectPath(out[count].effectId, path, sizeof(path)) &&
          fileSha256Hex(path, out[count].sha256, sizeof(out[count].sha256))) {
        count++;
      }
      file = root.openNextFile();
      continue;
    }
    file = root.openNextFile();
  }
  return count;
}

bool savePreset(const char *presetId, const char *json, size_t len) {
  return saveAsset("ep", presetId, json, len, kSinglePresetBytes, kPresetBudgetBytes, kMaxPresets);
}

}  // namespace ExpressionStore
