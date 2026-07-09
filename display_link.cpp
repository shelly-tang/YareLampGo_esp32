#include "display_link.h"

#include <Arduino.h>

#ifndef LAMPGO_DISPLAY_UART_BAUD
#define LAMPGO_DISPLAY_UART_BAUD 115200
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

void sendLine(const String &line) {
  if (!g_ready) return;
  g_displaySerial.print(line);
  g_displaySerial.print('\n');
  g_displaySerial.flush();
  if (line.indexOf("\"type\":\"clip_chunk\"") >= 0) {
    Serial.printf("[display_link] clip_chunk bytes=%u\n", line.length());
  } else {
    Serial.printf("[display_link] %s\n", line.c_str());
  }
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

void sendClipBegin(const char *clipId, int fps, int frameCount, int durationMs, size_t lcdBytes, const char *lcdSha256) {
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
  delay(300);
}

void sendClipChunk(const char *clipId, size_t offset, const char *hexData) {
  String line = "{\"type\":\"clip_chunk\",\"clip_id\":\"";
  line += escapeJson(clipId);
  line += "\",\"target\":\"lcd\",\"offset\":";
  line += (unsigned)offset;
  line += ",\"data\":\"";
  line += escapeJson(hexData);
  line += "\"}";
  sendLine(line);
  delay(15);
}

void sendClipCommit(const char *clipId) {
  String line = "{\"type\":\"clip_commit\",\"clip_id\":\"";
  line += escapeJson(clipId);
  line += "\"}";
  sendLine(line);
}

void sendClipPlay(const char *clipId) {
  String line = "{\"type\":\"clip_play\",\"clip_id\":\"";
  line += escapeJson(clipId);
  line += "\"}";
  sendLine(line);
}

}  // namespace DisplayLink
