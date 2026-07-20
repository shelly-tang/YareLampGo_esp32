#include "display_link.h"

#include <Arduino.h>
#include <cJSON.h>
#include <stdint.h>

#ifndef LAMPGO_DISPLAY_UART_BAUD
#define LAMPGO_DISPLAY_UART_BAUD 460800
#endif

#ifndef LAMPGO_DISPLAY_CLIP_ACK_TIMEOUT_MS
#define LAMPGO_DISPLAY_CLIP_ACK_TIMEOUT_MS 2000
#endif

#ifndef LAMPGO_DISPLAY_CLIP_COMMIT_TIMEOUT_MS
#define LAMPGO_DISPLAY_CLIP_COMMIT_TIMEOUT_MS 10000
#endif

#ifndef LAMPGO_DISPLAY_UART_RX
#define LAMPGO_DISPLAY_UART_RX 44
#endif

#ifndef LAMPGO_DISPLAY_UART_TX
#define LAMPGO_DISPLAY_UART_TX 43
#endif

namespace DisplayLink {
namespace {

HardwareSerial g_displaySerial(1);
bool g_ready = false;
uint32_t g_lastHeartbeatMs = 0;
String g_ackLine;
String g_lastClipError;
size_t g_c6FsTotalBytes = 0;
size_t g_c6FsUsedBytes = 0;
size_t g_c6InstalledBytes = 0;
int g_c6InstalledCount = 0;
int g_c6MaxClipCount = 0;
size_t g_c6InstalledBudgetBytes = 0;

void updateC6Capacity(const cJSON *doc) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(doc, "fs_total_bytes");
  if (cJSON_IsNumber(item)) g_c6FsTotalBytes = (size_t)item->valuedouble;
  item = cJSON_GetObjectItemCaseSensitive(doc, "fs_used_bytes");
  if (cJSON_IsNumber(item)) g_c6FsUsedBytes = (size_t)item->valuedouble;
  item = cJSON_GetObjectItemCaseSensitive(doc, "installed_bytes");
  if (cJSON_IsNumber(item)) g_c6InstalledBytes = (size_t)item->valuedouble;
  item = cJSON_GetObjectItemCaseSensitive(doc, "installed_count");
  if (cJSON_IsNumber(item)) g_c6InstalledCount = item->valueint;
  item = cJSON_GetObjectItemCaseSensitive(doc, "max_clip_count");
  if (cJSON_IsNumber(item)) g_c6MaxClipCount = item->valueint;
  item = cJSON_GetObjectItemCaseSensitive(doc, "installed_budget_bytes");
  if (cJSON_IsNumber(item)) g_c6InstalledBudgetBytes = (size_t)item->valuedouble;
}

void sendLine(const String &line) {
  if (!g_ready) return;
  g_displaySerial.print(line);
  g_displaySerial.print('\n');
  g_displaySerial.flush();
  if (line.indexOf("\"type\":\"clip_chunk\"") >= 0) {
    static uint32_t chunkLogCounter = 0;
    chunkLogCounter++;
    if (chunkLogCounter == 1 || chunkLogCounter % 64 == 0) {
      Serial.printf("[display_link] clip_chunk count=%lu bytes=%u\n",
                    (unsigned long)chunkLogCounter, line.length());
    }
  } else {
    Serial.printf("[display_link] %s\n", line.c_str());
  }
}

void setClipError(const String &error) {
  g_lastClipError = error;
  Serial.printf("[display_link] clip ack error: %s\n", g_lastClipError.c_str());
}

void clearAckInput() {
  while (g_displaySerial.available()) {
    g_displaySerial.read();
  }
  g_ackLine = "";
}

bool waitForClipAck(const char *clipId, const char *stage, size_t expectedOffset, uint32_t timeoutMs) {
  if (!g_ready) {
    setClipError("display uart not ready");
    return false;
  }

  const uint32_t started = millis();
  while ((uint32_t)(millis() - started) < timeoutMs) {
    while (g_displaySerial.available()) {
      char ch = (char)g_displaySerial.read();
      if (ch == '\r') continue;
      if (ch != '\n') {
        if (g_ackLine.length() < 384) {
          g_ackLine += ch;
        } else {
          g_ackLine = "";
        }
        continue;
      }

      String line = g_ackLine;
      g_ackLine = "";
      line.trim();
      if (!line.length()) continue;

      cJSON *doc = cJSON_Parse(line.c_str());
      if (!doc) continue;
      updateC6Capacity(doc);
      const cJSON *typeItem = cJSON_GetObjectItemCaseSensitive(doc, "type");
      const cJSON *clipItem = cJSON_GetObjectItemCaseSensitive(doc, "clip_id");
      const cJSON *stageItem = cJSON_GetObjectItemCaseSensitive(doc, "stage");
      bool matches = cJSON_IsString(typeItem) && typeItem->valuestring && strcmp(typeItem->valuestring, "clip_ack") == 0 &&
                     cJSON_IsString(clipItem) && clipItem->valuestring && strcmp(clipItem->valuestring, clipId ? clipId : "") == 0 &&
                     cJSON_IsString(stageItem) && stageItem->valuestring && strcmp(stageItem->valuestring, stage ? stage : "") == 0;
      if (!matches) {
        cJSON_Delete(doc);
        continue;
      }

      const cJSON *okItem = cJSON_GetObjectItemCaseSensitive(doc, "ok");
      const cJSON *offsetItem = cJSON_GetObjectItemCaseSensitive(doc, "next_offset");
      const cJSON *errorItem = cJSON_GetObjectItemCaseSensitive(doc, "error");
      bool ok = cJSON_IsTrue(okItem);
      size_t nextOffset = cJSON_IsNumber(offsetItem) ? (size_t)offsetItem->valuedouble : SIZE_MAX;
      String c6Error = cJSON_IsString(errorItem) && errorItem->valuestring ? String(errorItem->valuestring) : String("");
      cJSON_Delete(doc);

      if (!ok) {
        setClipError(String("C6 ") + stage + " rejected: " + (c6Error.length() ? c6Error : String("unknown error")));
        return false;
      }
      if (expectedOffset != SIZE_MAX && nextOffset != expectedOffset) {
        setClipError(String("C6 ") + stage + " offset " + String((unsigned)nextOffset) + " expected " +
                     String((unsigned)expectedOffset));
        return false;
      }
      g_lastClipError = "";
      return true;
    }
    delay(1);
  }

  setClipError(String("C6 ") + stage + " ack timeout");
  return false;
}

String escapeJson(const char *value) {
  String out;
  if (!value) return out;
  for (const char *p = value; *p; ++p) {
    if (*p == '"' || *p == '\\') out += '\\';
    out += *p;
  }
  return out;
}

}  // namespace

bool begin() {
  if (g_ready) return true;
  g_displaySerial.begin(LAMPGO_DISPLAY_UART_BAUD, SERIAL_8N1, LAMPGO_DISPLAY_UART_RX, LAMPGO_DISPLAY_UART_TX);
  g_ackLine.reserve(384);
  g_ready = true;
  delay(50);
  sendStatus("boot", "LampGo S3 online");
  return true;
}

void loop() {
  if (!g_ready) return;
  uint32_t now = millis();
  if (now - g_lastHeartbeatMs >= 30000) {
    g_lastHeartbeatMs = now;
    Serial.println("[display_link] online");
  }
}

void sendStatus(const char *status, const char *detail) {
  String line = "{\"type\":\"status\",\"status\":\"";
  line += escapeJson(status);
  line += "\"";
  if (detail && detail[0]) {
    line += ",\"detail\":\"";
    line += escapeJson(detail);
    line += "\"";
  }
  line += "}";
  sendLine(line);
}

void sendExpression(const char *expression) {
  String line = "{\"type\":\"expression\",\"name\":\"";
  line += escapeJson(expression);
  line += "\"}";
  sendLine(line);
}

const char *lastClipError() {
  return g_lastClipError.c_str();
}

bool sendClipBegin(const char *clipId, int fps, int frameCount, int durationMs, size_t lcdBytes, const char *lcdSha256) {
  clearAckInput();
  String line = "{\"type\":\"clip_begin\",\"clip_id\":\"";
  line += escapeJson(clipId);
  line += "\",\"fps\":";
  line += fps;
  line += ",\"frame_count\":";
  line += frameCount;
  line += ",\"duration_ms\":";
  line += durationMs;
  line += ",\"lcd_bytes\":";
  line += (unsigned)lcdBytes;
  line += ",\"lcd_sha256\":\"";
  line += escapeJson(lcdSha256 ? lcdSha256 : "");
  line += "\"";
  line += "}";
  sendLine(line);
  return waitForClipAck(clipId, "begin", 0, LAMPGO_DISPLAY_CLIP_ACK_TIMEOUT_MS);
}

bool sendClipChunk(const char *clipId, size_t offset, const char *hexData) {
  String line = "{\"type\":\"clip_chunk\",\"clip_id\":\"";
  line += escapeJson(clipId);
  line += "\",\"target\":\"lcd\",\"offset\":";
  line += (unsigned)offset;
  line += ",\"data\":\"";
  line += escapeJson(hexData);
  line += "\"}";
  sendLine(line);
  size_t bytes = hexData ? strlen(hexData) / 2 : 0;
  return waitForClipAck(clipId, "chunk", offset + bytes, LAMPGO_DISPLAY_CLIP_ACK_TIMEOUT_MS);
}

bool sendClipChunkBytes(const char *clipId, size_t offset, const uint8_t *data, size_t len) {
  if (!data && len > 0) {
    setClipError("missing clip chunk data");
    return false;
  }
  static const char *hex = "0123456789abcdef";
  String encoded;
  encoded.reserve(len * 2 + 1);
  for (size_t i = 0; i < len; ++i) {
    encoded += hex[data[i] >> 4];
    encoded += hex[data[i] & 0x0F];
  }
  return sendClipChunk(clipId, offset, encoded.c_str());
}

bool sendClipCommit(const char *clipId, size_t expectedOffset) {
  String line = "{\"type\":\"clip_commit\",\"clip_id\":\"";
  line += escapeJson(clipId);
  line += "\"}";
  sendLine(line);
  return waitForClipAck(clipId, "commit", expectedOffset, LAMPGO_DISPLAY_CLIP_COMMIT_TIMEOUT_MS);
}

void sendClipPlay(const char *clipId, bool loop) {
  String line = "{\"type\":\"clip_play\",\"clip_id\":\"";
  line += escapeJson(clipId);
  line += "\",\"playback\":\"";
  line += loop ? "loop" : "once";
  line += "\"}";
  sendLine(line);
}

void sendClipStop() {
  sendLine("{\"type\":\"clip_stop\"}");
}

size_t c6FsTotalBytes() { return g_c6FsTotalBytes; }
size_t c6FsUsedBytes() { return g_c6FsUsedBytes; }
size_t c6InstalledBytes() { return g_c6InstalledBytes; }
int c6InstalledCount() { return g_c6InstalledCount; }
int c6MaxClipCount() { return g_c6MaxClipCount; }
size_t c6InstalledBudgetBytes() { return g_c6InstalledBudgetBytes; }

}  // namespace DisplayLink
