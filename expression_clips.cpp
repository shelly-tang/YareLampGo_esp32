// SPDX-FileCopyrightText: 2026 @ninsmiracle, @shelly-tang, and LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "expression_clips.h"
#include "expression_store.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <ctype.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>

namespace ExpressionClips {
namespace {

constexpr size_t kMaxLcdBytes = 256 * 1024;

bool g_ready = false;
char g_lastError[96] = "";
char g_syncClipId[40] = "";
size_t g_expectedLcdBytes = 0;
char g_expectedLcdSha256[65] = "";

void setError(const char *message) {
  snprintf(g_lastError, sizeof(g_lastError), "%s", message ? message : "unknown");
}

bool sanitizeId(const char *input, char *out, size_t outLen) {
  if (!input || !out || outLen == 0) {
    setError("bad clip id");
    return false;
  }
  size_t n = 0;
  for (const char *p = input; *p && n + 1 < outLen; ++p) {
    unsigned char c = (unsigned char)*p;
    if (isalnum(c) || c == '-' || c == '_') {
      out[n++] = (char)tolower(c);
    }
  }
  out[n] = 0;
  if (n == 0) {
    setError("empty clip id");
    return false;
  }
  return true;
}

bool buildPath(const char *clipId, const char *suffix, char *out, size_t outLen) {
  char safe[40];
  if (!sanitizeId(clipId, safe, sizeof(safe))) return false;
  int written = snprintf(out, outLen, "/ec_%s_%s", safe, suffix);
  if (written <= 0 || (size_t)written >= outLen) {
    setError("clip path too long");
    return false;
  }
  return true;
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool appendHexToFile(const char *path, size_t expectedOffset, const char *hexData) {
  if (!hexData) {
    setError("missing hex data");
    return false;
  }
  size_t hexLen = strlen(hexData);
  if ((hexLen % 2) != 0) {
    setError("hex data length must be even");
    return false;
  }

  File check = SPIFFS.open(path, "r");
  size_t currentSize = check ? check.size() : 0;
  if (check) check.close();
  if (currentSize != expectedOffset) {
    setError("chunk offset mismatch");
    return false;
  }

  File file = SPIFFS.open(path, expectedOffset == 0 ? "w" : "a");
  if (!file) {
    setError("open clip file failed");
    return false;
  }

  uint8_t buffer[64];
  size_t used = 0;
  for (size_t i = 0; i < hexLen; i += 2) {
    int high = hexValue(hexData[i]);
    int low = hexValue(hexData[i + 1]);
    if (high < 0 || low < 0) {
      file.close();
      setError("invalid hex data");
      return false;
    }
    buffer[used++] = (uint8_t)((high << 4) | low);
    if (used == sizeof(buffer)) {
      if (file.write(buffer, used) != used) {
        file.close();
        setError("write clip chunk failed");
        return false;
      }
      used = 0;
    }
  }
  if (used > 0 && file.write(buffer, used) != used) {
    file.close();
    setError("write clip chunk failed");
    return false;
  }
  file.close();
  g_lastError[0] = 0;
  return true;
}

bool appendBytesToFile(const char *path, size_t expectedOffset, const uint8_t *data, size_t len) {
  if (!data && len > 0) {
    setError("missing byte data");
    return false;
  }

  File check = SPIFFS.open(path, "r");
  size_t currentSize = check ? check.size() : 0;
  if (check) check.close();
  if (currentSize != expectedOffset) {
    setError("chunk offset mismatch");
    return false;
  }

  File file = SPIFFS.open(path, expectedOffset == 0 ? "w" : "a");
  if (!file) {
    setError("open clip file failed");
    return false;
  }
  if (len > 0 && file.write(data, len) != len) {
    file.close();
    setError("write clip chunk failed");
    return false;
  }
  file.close();
  g_lastError[0] = 0;
  return true;
}

size_t fileSizeOrZero(const char *path) {
  File file = SPIFFS.open(path, "r");
  if (!file) return 0;
  size_t size = file.size();
  file.close();
  return size;
}

bool equalsIgnoreCase(const char *a, const char *b) {
  if (!a || !b) return false;
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    ++a;
    ++b;
  }
  return *a == 0 && *b == 0;
}

bool fileSha256Hex(const char *path, char *out, size_t outLen) {
  if (!out || outLen < 65) {
    setError("sha output too small");
    return false;
  }
  File file = SPIFFS.open(path, "r");
  if (!file) {
    setError("clip file missing");
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
    if (read < 0) {
      file.close();
      mbedtls_sha256_free(&ctx);
      setError("sha read failed");
      return false;
    }
    if (read > 0 && mbedtls_sha256_update(&ctx, buffer, (size_t)read) != 0) {
      file.close();
      mbedtls_sha256_free(&ctx);
      setError("sha update failed");
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
  for (int i = 0; i < 32; ++i) {
    out[i * 2] = hex[digest[i] >> 4];
    out[i * 2 + 1] = hex[digest[i] & 0x0F];
  }
  out[64] = 0;
  return true;
}

}  // namespace

bool begin() {
  if (g_ready) return true;
  if (!SPIFFS.begin(true)) {
    setError("SPIFFS mount failed");
    Serial.println("[expression_clips] SPIFFS mount failed");
    return false;
  }
  g_ready = true;
  Serial.println("[expression_clips] SPIFFS ready");
  return true;
}

const char *lastError() {
  return g_lastError[0] ? g_lastError : "ok";
}

bool lcdPath(const char *clipId, char *out, size_t outLen) {
  return buildPath(clipId, "lcd.bin", out, outLen);
}

bool manifestPath(const char *clipId, char *out, size_t outLen) {
  return buildPath(clipId, "manifest.json", out, outLen);
}

bool beginSync(const char *clipId,
               const char *expression,
               int fps,
               int frameCount,
               int durationMs,
               size_t lcdBytes,
               const char *lcdSha256) {
  if (!begin()) return false;
  if (fps < 1 || frameCount < 1 || durationMs < 1) {
    setError("bad clip metadata");
    return false;
  }
  if (lcdBytes > kMaxLcdBytes) {
    setError("clip exceeds cache budget");
    return false;
  }

  char manifest[64], lcd[64], staleLed[64];
  if (!manifestPath(clipId, manifest, sizeof(manifest)) ||
      !lcdPath(clipId, lcd, sizeof(lcd))) {
    return false;
  }
  SPIFFS.remove(lcd);
  if (!ExpressionStore::canStageLcd(lcdBytes)) {
    setError(ExpressionStore::lastError());
    return false;
  }
  if (buildPath(clipId, "led.bin", staleLed, sizeof(staleLed))) {
    SPIFFS.remove(staleLed);
  }

  File file = SPIFFS.open(manifest, "w");
  if (!file) {
    setError("manifest open failed");
    return false;
  }
  file.printf("{\"clip_id\":\"%s\",\"expression\":\"%s\",\"fps\":%d,\"frame_count\":%d,\"duration_ms\":%d,"
              "\"lcd_bytes\":%u,\"lcd_sha256\":\"%s\",\"led_effect\":\"%s\"}\n",
              clipId,
              expression ? expression : clipId,
              fps,
              frameCount,
              durationMs,
              (unsigned)lcdBytes,
              lcdSha256 ? lcdSha256 : "",
              expression ? expression : clipId);
  file.close();
  snprintf(g_syncClipId, sizeof(g_syncClipId), "%s", clipId ? clipId : "");
  g_expectedLcdBytes = lcdBytes;
  snprintf(g_expectedLcdSha256, sizeof(g_expectedLcdSha256), "%s", lcdSha256 ? lcdSha256 : "");
  g_lastError[0] = 0;
  Serial.printf("[expression_clips] begin clip=%s frames=%d fps=%d lcd=%u led=procedural\n",
                clipId, frameCount, fps, (unsigned)lcdBytes);
  return true;
}

bool appendChunk(const char *clipId, const char *target, size_t offset, const char *hexData) {
  if (!begin()) return false;
  char path[64];
  if (strcmp(target ? target : "", "lcd") == 0) {
    if (!lcdPath(clipId, path, sizeof(path))) return false;
  } else {
    setError("target must be lcd");
    return false;
  }
  return appendHexToFile(path, offset, hexData);
}

bool appendBytes(const char *clipId, const char *target, size_t offset, const uint8_t *data, size_t len) {
  if (!begin()) return false;
  char path[64];
  if (strcmp(target ? target : "", "lcd") == 0) {
    if (!lcdPath(clipId, path, sizeof(path))) return false;
  } else {
    setError("target must be lcd");
    return false;
  }
  return appendBytesToFile(path, offset, data, len);
}

bool commitSync(const char *clipId) {
  if (!begin()) return false;
  char lcd[64];
  if (!lcdPath(clipId, lcd, sizeof(lcd))) {
    return false;
  }
  if (fileSizeOrZero(lcd) == 0) {
    setError("clip files missing");
    return false;
  }
  size_t lcdSize = fileSizeOrZero(lcd);
  if (g_syncClipId[0] && strcmp(g_syncClipId, clipId ? clipId : "") == 0) {
    if (g_expectedLcdBytes > 0 && lcdSize != g_expectedLcdBytes) {
      setError("clip lcd size mismatch");
      Serial.printf("[expression_clips] commit failed clip=%s size=%u expected=%u\n",
                    clipId, (unsigned)lcdSize, (unsigned)g_expectedLcdBytes);
      return false;
    }
    if (g_expectedLcdSha256[0]) {
      char actualSha[65];
      if (!fileSha256Hex(lcd, actualSha, sizeof(actualSha))) return false;
      if (!equalsIgnoreCase(actualSha, g_expectedLcdSha256)) {
        setError("clip lcd sha mismatch");
        Serial.printf("[expression_clips] commit failed clip=%s sha=%s expected=%s\n",
                      clipId, actualSha, g_expectedLcdSha256);
        return false;
      }
    }
  }
  Serial.printf("[expression_clips] committed clip=%s lcd=%u led=procedural\n",
                clipId, (unsigned)lcdSize);
  g_lastError[0] = 0;
  return true;
}

bool releaseLcdPayload(const char *clipId) {
  if (!begin()) return false;
  char lcd[64];
  if (!lcdPath(clipId, lcd, sizeof(lcd))) return false;
  if (SPIFFS.exists(lcd) && !SPIFFS.remove(lcd)) {
    setError("staging cleanup failed");
    return false;
  }
  g_lastError[0] = 0;
  return true;
}

bool removeClip(const char *clipId) {
  if (!begin()) return false;
  char lcd[64], manifest[64], staleLed[64];
  bool ok = true;
  ok = ok && lcdPath(clipId, lcd, sizeof(lcd));
  ok = ok && manifestPath(clipId, manifest, sizeof(manifest));
  if (!ok) return false;
  SPIFFS.remove(lcd);
  SPIFFS.remove(manifest);
  if (buildPath(clipId, "led.bin", staleLed, sizeof(staleLed))) {
    SPIFFS.remove(staleLed);
  }
  return true;
}

}  // namespace ExpressionClips
