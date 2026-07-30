#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <SPI.h>
#include <ctype.h>
#include <mbedtls/sha256.h>
#include "lcd_faces.h"

// Waveshare ESP32-C6-LCD-1.47 LCD pins (ST7789, 172x320)
#define LCD_MOSI 6
#define LCD_SCLK 7
#define LCD_CS 14
#define LCD_DC 15
#define LCD_RST 21
#define LCD_BL 22

#define LCD_WIDTH 172
#define LCD_HEIGHT 320
#define LCD_X_OFFSET 34
#define LCD_Y_OFFSET 0

// Change these two pins to the ESP32-C6 GPIOs you wired to the S3.
// S3 GPIO43 TX -> C6_LINK_RX
// S3 GPIO44 RX -> C6_LINK_TX
#ifndef C6_LINK_RX
#define C6_LINK_RX 17
#endif
#ifndef C6_LINK_TX
#define C6_LINK_TX 16
#endif
#ifndef C6_LINK_BAUD
#define C6_LINK_BAUD 460800
#endif
#ifndef C6_LINK_RX_BUFFER
#define C6_LINK_RX_BUFFER 8192
#endif
#ifndef C6_CLIP_MIN_FPS
#define C6_CLIP_MIN_FPS 8
#endif
#ifndef C6_CLIP_MAX_FPS
#define C6_CLIP_MAX_FPS 30
#endif
#ifndef C6_LCD_SPI_HZ
#define C6_LCD_SPI_HZ 40000000
#endif
#define C6_CLIP_WIDTH 320
#define C6_CLIP_HEIGHT 172
#define C6_CLIP_TILE_SIZE 16
#define C6_CLIP_TILE_COLS ((C6_CLIP_WIDTH + C6_CLIP_TILE_SIZE - 1) / C6_CLIP_TILE_SIZE)
#define C6_CLIP_TILE_ROWS ((C6_CLIP_HEIGHT + C6_CLIP_TILE_SIZE - 1) / C6_CLIP_TILE_SIZE)
#define C6_CLIP_PERF_REPORT_FRAMES 30
#define CLIP_DEBUG_LOG_PATH "/clip_debug.log"
#define CLIP_DEBUG_LOG_MAX_BYTES 8192

#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xFFFF
#define COLOR_RED 0xF800
#define COLOR_GREEN 0x07E0
#define COLOR_BLUE 0x001F
#define COLOR_CYAN 0x07FF
#define COLOR_YELLOW 0xFFE0
#define COLOR_MAGENTA 0xF81F
#define COLOR_ORANGE 0xFC00
#define COLOR_PINK 0xF81F
#define COLOR_PURPLE 0x8010
#define COLOR_DARK 0x0841

static SPIClass lcdSpi(FSPI);
static HardwareSerial linkSerial(1);
static String inputLine;
static bool inputOverflow = false;
static uint32_t lastMessageMs = 0;
static uint32_t lastExpressionMs = 0;
static String lastRenderedLine;
static bool g_landscape = false;
static bool g_rotationSet = false;
static uint16_t g_screenWidth = LCD_WIDTH;
static uint16_t g_screenHeight = LCD_HEIGHT;
static uint16_t g_xOffset = LCD_X_OFFSET;
static uint16_t g_yOffset = LCD_Y_OFFSET;
static String g_currentEyeExpression = "smiley";
static bool g_eyeScreenActive = false;
static bool g_blinkClosed = false;
static uint32_t g_nextBlinkMs = 0;
static uint32_t g_blinkOpenMs = 0;
static uint8_t g_currentFaceIndex = 0;
static uint32_t g_nextFaceSlideMs = 0;
static File g_clipFile;
static bool g_clipPlaying = false;
static String g_clipId;
static uint16_t g_clipFrameCount = 0;
static uint16_t g_clipFps = 0;
static uint16_t g_clipFrameIndex = 0;
static uint32_t g_nextClipFrameMs = 0;
static bool g_clipLoop = true;
// The clip framebuffer is stored in LCD wire order (RGB565 high byte first).
// It lets the C6 compare real pixel changes instead of blindly transmitting the
// encoder's bounding rectangle, whose unchanged interior can be very large.
static uint8_t g_clipShadow[C6_CLIP_WIDTH * C6_CLIP_HEIGHT * 2];
static bool g_clipShadowValid = false;
static uint32_t g_clipPerfFrames = 0;
static uint32_t g_clipPerfDecodeUs = 0;
static uint32_t g_clipPerfLcdUs = 0;
static uint32_t g_clipPerfMaxFrameUs = 0;
static uint32_t g_clipPerfBudgetMisses = 0;
static uint32_t g_clipPerfLcdPixels = 0;
static uint32_t g_clipPerfLcdRects = 0;
static String g_syncClipId;
static uint32_t g_syncExpectedBytes = 0;
static String g_syncExpectedSha256;
static uint32_t g_syncReceivedBytes = 0;
static bool g_syncHadError = false;
static String g_syncLastError;
static String g_validatedClipId;
static bool g_syncInProgress = false;
static bool g_syncErrorShown = false;
static bool g_syncFirstChunkLogged = false;
static File g_syncFile;
static uint32_t g_syncWriteOffset = 0;
static uint8_t g_syncLastProgressPct = 255;

static void clipStorageUsage(uint32_t &bytes, int &count);
static bool g_fsReady = false;
static uint8_t g_debugLogPrintCount = 0;
static uint32_t g_nextDebugLogPrintMs = 0;

static const uint8_t font5x7[][5] = {
  {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14},
  {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62}, {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00},
  {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
  {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},
  {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
  {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
  {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14}, {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06},
  {0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
  {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A},
  {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
  {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
  {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31},
  {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F},
  {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
  {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40},
  {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78}, {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20},
  {0x38,0x44,0x44,0x48,0x7F}, {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E},
  {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00}, {0x7F,0x10,0x28,0x44,0x00},
  {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78}, {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38},
  {0x7C,0x14,0x14,0x14,0x08}, {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
  {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C}, {0x3C,0x40,0x30,0x40,0x3C},
  {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C}, {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00},
  {0x00,0x00,0x7F,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00}, {0x08,0x04,0x08,0x10,0x08}
};

static void lcdWriteCommand(uint8_t command) {
  digitalWrite(LCD_DC, LOW);
  digitalWrite(LCD_CS, LOW);
  lcdSpi.write(command);
  digitalWrite(LCD_CS, HIGH);
}

static void lcdWriteData(uint8_t data) {
  digitalWrite(LCD_DC, HIGH);
  digitalWrite(LCD_CS, LOW);
  lcdSpi.write(data);
  digitalWrite(LCD_CS, HIGH);
}

static void lcdSetRotation(bool landscape) {
  if (g_rotationSet && g_landscape == landscape) return;
  g_clipShadowValid = false;
  g_rotationSet = true;
  g_landscape = landscape;
  g_screenWidth = landscape ? 320 : LCD_WIDTH;
  g_screenHeight = landscape ? LCD_WIDTH : LCD_HEIGHT;
  g_xOffset = landscape ? 0 : LCD_X_OFFSET;
  g_yOffset = landscape ? LCD_X_OFFSET : LCD_Y_OFFSET;

  lcdWriteCommand(0x36);
  lcdWriteData(landscape ? 0x60 : 0x00);
}

static void lcdWriteData16(uint16_t data) {
  digitalWrite(LCD_DC, HIGH);
  digitalWrite(LCD_CS, LOW);
  lcdSpi.write(data >> 8);
  lcdSpi.write(data & 0xFF);
  digitalWrite(LCD_CS, HIGH);
}

static void lcdSetWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  uint16_t x0 = x + g_xOffset;
  uint16_t x1 = x0 + w - 1;
  uint16_t y0 = y + g_yOffset;
  uint16_t y1 = y0 + h - 1;

  lcdWriteCommand(0x2A);
  lcdWriteData(x0 >> 8);
  lcdWriteData(x0 & 0xFF);
  lcdWriteData(x1 >> 8);
  lcdWriteData(x1 & 0xFF);

  lcdWriteCommand(0x2B);
  lcdWriteData(y0 >> 8);
  lcdWriteData(y0 & 0xFF);
  lcdWriteData(y1 >> 8);
  lcdWriteData(y1 & 0xFF);

  lcdWriteCommand(0x2C);
}

static void lcdFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x >= (int16_t)g_screenWidth || y >= (int16_t)g_screenHeight) return;
  if (x + w > (int16_t)g_screenWidth) w = g_screenWidth - x;
  if (y + h > (int16_t)g_screenHeight) h = g_screenHeight - y;
  if (w <= 0 || h <= 0) return;

  g_clipShadowValid = false;
  lcdSetWindow((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h);
  digitalWrite(LCD_DC, HIGH);
  digitalWrite(LCD_CS, LOW);
  for (uint32_t i = 0; i < (uint32_t)w * h; ++i) {
    lcdSpi.write(color >> 8);
    lcdSpi.write(color & 0xFF);
  }
  digitalWrite(LCD_CS, HIGH);
}

static void lcdFillScreen(uint16_t color) {
  lcdFillRect(0, 0, g_screenWidth, g_screenHeight, color);
}

static void lcdDrawFaceImage(uint8_t faceIndex) {
  if (faceIndex >= LcdFaces::kFaceCount) faceIndex = 0;
  g_currentFaceIndex = faceIndex;
  g_nextFaceSlideMs = millis() + 3000;
  lcdSetRotation(true);

  LcdFaces::FaceImage face;
  memcpy_P(&face, &LcdFaces::kFaces[faceIndex], sizeof(face));

  g_clipShadowValid = false;
  lcdSetWindow(0, 0, LcdFaces::kWidth, LcdFaces::kHeight);
  digitalWrite(LCD_DC, HIGH);
  digitalWrite(LCD_CS, LOW);

  for (uint16_t run = 0; run < face.runCount; ++run) {
    uint16_t count = pgm_read_word(&face.runs[run * 2]);
    uint16_t color = pgm_read_word(&face.runs[run * 2 + 1]);
    uint8_t high = color >> 8;
    uint8_t low = color & 0xFF;
    for (uint16_t i = 0; i < count; ++i) {
      lcdSpi.write(high);
      lcdSpi.write(low);
    }
  }

  digitalWrite(LCD_CS, HIGH);
}


static void lcdDrawPixel(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= (int16_t)g_screenWidth || y >= (int16_t)g_screenHeight) return;
  g_clipShadowValid = false;
  lcdSetWindow((uint16_t)x, (uint16_t)y, 1, 1);
  lcdWriteData16(color);
}

static void lcdDrawChar(int16_t x, int16_t y, char c, uint16_t color, uint16_t bg, uint8_t size) {
  if (c < 32 || c > 126) c = '?';
  const uint8_t *glyph = font5x7[c - 32];
  for (uint8_t col = 0; col < 5; ++col) {
    uint8_t bits = glyph[col];
    for (uint8_t row = 0; row < 8; ++row) {
      uint16_t pixelColor = (bits & 0x01) ? color : bg;
      if (size == 1) {
        lcdDrawPixel(x + col, y + row, pixelColor);
      } else {
        lcdFillRect(x + col * size, y + row * size, size, size, pixelColor);
      }
      bits >>= 1;
    }
  }
  if (bg != color) {
    lcdFillRect(x + 5 * size, y, size, 8 * size, bg);
  }
}

static void lcdDrawText(int16_t x, int16_t y, const String &text, uint16_t color, uint16_t bg, uint8_t size) {
  int16_t cursorX = x;
  int16_t cursorY = y;
  int16_t charWidth = 6 * size;
  int16_t charHeight = 9 * size;
  for (uint16_t i = 0; i < text.length(); ++i) {
    char c = text[i];
    if (c == '\n' || cursorX + charWidth > (int16_t)g_screenWidth) {
      cursorX = x;
      cursorY += charHeight;
      if (c == '\n') continue;
    }
    if (cursorY + charHeight > (int16_t)g_screenHeight) break;
    lcdDrawChar(cursorX, cursorY, c, color, bg, size);
    cursorX += charWidth;
  }
}

static String jsonValue(const String &line, const String &key) {
  String pattern = String("\"") + key + "\":\"";
  int start = line.indexOf(pattern);
  if (start < 0) return "";
  start += pattern.length();
  int end = line.indexOf('"', start);
  if (end < 0) return line.substring(start);
  return line.substring(start, end);
}

static long jsonNumber(const String &line, const String &key, long fallback = 0) {
  String pattern = String("\"") + key + "\":";
  int start = line.indexOf(pattern);
  if (start < 0) return fallback;
  start += pattern.length();
  int end = start;
  while (end < (int)line.length() && (isDigit(line[end]) || line[end] == '-')) {
    ++end;
  }
  if (end == start) return fallback;
  return line.substring(start, end).toInt();
}

static void sendClipAck(const String &clipId,
                        const char *stage,
                        bool ok,
                        uint32_t nextOffset,
                        const String &error = "") {
  String line = "{\"type\":\"clip_ack\",\"clip_id\":\"";
  line += clipId;
  line += "\",\"stage\":\"";
  line += stage ? stage : "";
  line += "\",\"ok\":";
  line += ok ? "true" : "false";
  line += ",\"next_offset\":";
  line += nextOffset;
  if (error.length()) {
    line += ",\"error\":\"";
    line += error;
    line += "\"";
  }
  if (strcmp(stage ? stage : "", "begin") == 0 || strcmp(stage ? stage : "", "commit") == 0) {
    uint32_t installedBytes = 0;
    int installedCount = 0;
    clipStorageUsage(installedBytes, installedCount);
    bool productPartition = g_fsReady && LittleFS.totalBytes() >= 5UL * 1024UL * 1024UL;
    line += ",\"fs_total_bytes\":";
    line += g_fsReady ? LittleFS.totalBytes() : 0;
    line += ",\"fs_used_bytes\":";
    line += g_fsReady ? LittleFS.usedBytes() : 0;
    line += ",\"installed_bytes\":";
    line += installedBytes;
    line += ",\"installed_count\":";
    line += installedCount;
    line += ",\"max_clip_count\":";
    line += productPartition ? 10 : 5;
    line += ",\"installed_budget_bytes\":";
    line += productPartition ? 3UL * 1024UL * 1024UL : 896UL * 1024UL;
  }
  line += "}";
  linkSerial.println(line);
  linkSerial.flush();
  if (!ok || strcmp(stage ? stage : "", "commit") == 0) {
    Serial.printf("clip ack stage=%s ok=%d next=%lu error=%s\n",
                  stage ? stage : "", ok ? 1 : 0, (unsigned long)nextOffset, error.c_str());
  }
}

static String clipPath(String clipId) {
  clipId.toLowerCase();
  String safe;
  for (uint16_t i = 0; i < clipId.length() && safe.length() < 32; ++i) {
    char c = clipId[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_') safe += c;
  }
  if (!safe.length()) safe = "default";
  return String("/ec_") + safe + ".lcd";
}

static String clipTempPath(String clipId) {
  return clipPath(clipId) + ".tmp";
}

static void clipStorageUsage(uint32_t &bytes, int &count) {
  bytes = 0;
  count = 0;
  File root = LittleFS.open("/");
  if (!root || !root.isDirectory()) return;
  File file = root.openNextFile();
  while (file) {
    String name = file.name();
    if ((name.startsWith("/ec_") || name.startsWith("ec_")) && name.endsWith(".lcd")) {
      bytes += file.size();
      count++;
    }
    file = root.openNextFile();
  }
}

static int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static uint16_t readLe16(File &file) {
  uint8_t low = file.read();
  uint8_t high = file.read();
  return (uint16_t)low | ((uint16_t)high << 8);
}

static bool equalsIgnoreCase(const String &a, const String &b) {
  if (a.length() != b.length()) return false;
  for (uint16_t i = 0; i < a.length(); ++i) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
  }
  return true;
}

static String clipDiagLine(uint32_t actual, uint32_t expected) {
  if (expected == 0) return String(actual);
  return String(actual) + "/" + String(expected);
}

static uint8_t syncProgressPct() {
  if (g_syncExpectedBytes == 0) return 0;
  uint32_t pct = (uint32_t)(((uint64_t)g_syncReceivedBytes * 100ULL) / g_syncExpectedBytes);
  return (uint8_t)min((uint32_t)100, pct);
}

static bool fileSha256Hex(const String &path, String &outSha, uint32_t *sizeOut = nullptr) {
  File file = LittleFS.open(path, "r");
  if (!file) return false;
  if (sizeOut) *sizeOut = file.size();

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  if (mbedtls_sha256_starts(&ctx, 0) != 0) {
    file.close();
    mbedtls_sha256_free(&ctx);
    return false;
  }

  uint8_t buffer[256];
  while (file.available()) {
    int read = file.read(buffer, sizeof(buffer));
    if (read < 0) {
      file.close();
      mbedtls_sha256_free(&ctx);
      return false;
    }
    if (read > 0 && mbedtls_sha256_update(&ctx, buffer, (size_t)read) != 0) {
      file.close();
      mbedtls_sha256_free(&ctx);
      return false;
    }
  }
  file.close();

  uint8_t digest[32];
  if (mbedtls_sha256_finish(&ctx, digest) != 0) {
    mbedtls_sha256_free(&ctx);
    return false;
  }
  mbedtls_sha256_free(&ctx);

  char hex[65];
  static const char *digits = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) {
    hex[i * 2] = digits[digest[i] >> 4];
    hex[i * 2 + 1] = digits[digest[i] & 0x0F];
  }
  hex[64] = 0;
  outSha = hex;
  return true;
}

static void stopClipPlayback() {
  if (g_clipFile) g_clipFile.close();
  g_clipPlaying = false;
  g_clipId = "";
  g_clipFrameCount = 0;
  g_clipFps = 0;
  g_clipFrameIndex = 0;
  g_nextClipFrameMs = 0;
}

static void closeSyncFile() {
  if (g_syncFile) {
    g_syncFile.flush();
    g_syncFile.close();
  }
}

static void debugLogAppend(const String &line) {
  if (!g_fsReady) return;
  File existing = LittleFS.open(CLIP_DEBUG_LOG_PATH, "r");
  size_t size = existing ? existing.size() : 0;
  if (existing) existing.close();
  if (size > CLIP_DEBUG_LOG_MAX_BYTES) return;

  File file = LittleFS.open(CLIP_DEBUG_LOG_PATH, "a");
  if (!file) return;
  file.print(millis());
  file.print(' ');
  file.println(line);
  file.close();
}

static void debugLogReset(const String &reason) {
  if (!g_fsReady) return;
  LittleFS.remove(CLIP_DEBUG_LOG_PATH);
  debugLogAppend(String("RESET ") + reason);
}

static void debugLogPrintStored() {
  if (!g_fsReady) return;
  File file = LittleFS.open(CLIP_DEBUG_LOG_PATH, "r");
  if (!file || file.size() == 0) {
    if (file) file.close();
    Serial.println("clip debug log: empty");
    return;
  }
  Serial.println("---- C6 CLIP DEBUG LOG BEGIN ----");
  while (file.available()) {
    Serial.write(file.read());
  }
  file.close();
  Serial.println("---- C6 CLIP DEBUG LOG END ----");
}

static void updateDebugLogPrinter() {
  if (!g_fsReady || g_debugLogPrintCount >= 3 || g_nextDebugLogPrintMs == 0) return;
  uint32_t now = millis();
  if (now < g_nextDebugLogPrintMs) return;
  debugLogPrintStored();
  g_debugLogPrintCount++;
  if (g_debugLogPrintCount == 1) {
    g_nextDebugLogPrintMs = now + 4000;
  } else if (g_debugLogPrintCount == 2) {
    g_nextDebugLogPrintMs = now + 6000;
  } else {
    g_nextDebugLogPrintMs = 0;
  }
}

static String sanitizeText(String text) {
  text.replace("_", " ");
  for (uint16_t i = 0; i < text.length(); ++i) {
    char c = text[i];
    if (c < 32 || c > 126) text.setCharAt(i, '?');
  }
  return text;
}

static void showScreen(const String &title, const String &line1, const String &line2, uint16_t accent) {
  g_eyeScreenActive = false;
  g_nextFaceSlideMs = 0;
  lcdSetRotation(false);
  lcdFillScreen(COLOR_BLACK);
  lcdFillRect(0, 0, g_screenWidth, 38, accent);
  lcdDrawText(10, 10, title, COLOR_BLACK, accent, 2);
  lcdDrawText(10, 58, line1, COLOR_WHITE, COLOR_BLACK, 2);
  lcdDrawText(10, 98, line2, COLOR_CYAN, COLOR_BLACK, 1);
  lcdDrawText(10, 286, String("UART ") + String(C6_LINK_BAUD), COLOR_DARK | COLOR_WHITE, COLOR_BLACK, 1);
}

static void showClipSyncProgress(bool force = false) {
  if (!g_syncInProgress || g_syncExpectedBytes == 0 || g_syncHadError) return;
  uint8_t pct = syncProgressPct();
  uint8_t bucket = pct >= 100 ? 100 : (uint8_t)((pct / 10) * 10);
  if (!force && g_syncLastProgressPct != 255 && bucket <= g_syncLastProgressPct) return;
  g_syncLastProgressPct = bucket;
  showScreen("CLIP", String("SYNC ") + String(bucket) + "%", clipDiagLine(g_syncReceivedBytes, g_syncExpectedBytes), COLOR_BLUE);
}

static bool nameHas(const String &name, const char *token) {
  return name.indexOf(token) >= 0;
}

static void lcdFillCircle(int16_t cx, int16_t cy, int16_t radius, uint16_t color) {
  for (int16_t y = -radius; y <= radius; ++y) {
    int16_t x = 0;
    while ((int32_t)(x + 1) * (x + 1) + (int32_t)y * y <= (int32_t)radius * radius) {
      ++x;
    }
    lcdFillRect(cx - x, cy + y, x * 2 + 1, 1, color);
  }
}

static void lcdDrawThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t thickness, uint16_t color) {
  int16_t dx = x1 - x0;
  int16_t dy = y1 - y0;
  int16_t steps = max(abs(dx), abs(dy));
  if (steps == 0) {
    lcdFillCircle(x0, y0, thickness, color);
    return;
  }
  for (int16_t i = 0; i <= steps; ++i) {
    int16_t x = x0 + ((int32_t)dx * i) / steps;
    int16_t y = y0 + ((int32_t)dy * i) / steps;
    lcdFillCircle(x, y, thickness, color);
  }
}

static void drawHeart(int16_t x, int16_t y, uint8_t scale, uint16_t color) {
  static const char *heart[] = {
      "  XX  XX  ",
      " XXXXXXXX ",
      "XXXXXXXXXX",
      "XXXXXXXXXX",
      " XXXXXXXX ",
      "  XXXXXX  ",
      "   XXXX   ",
      "    XX    ",
  };
  for (uint8_t row = 0; row < 8; ++row) {
    for (uint8_t col = 0; col < 10; ++col) {
      if (heart[row][col] != ' ') {
        lcdFillRect(x + col * scale, y + row * scale, scale, scale, color);
      }
    }
  }
}

static void drawEyeFrame(int16_t cx, int16_t cy, int16_t w, int16_t h, uint16_t eyeColor) {
  int16_t radius = h / 2;
  lcdFillRect(cx - w / 2 + radius, cy - h / 2, w - radius * 2, h, eyeColor);
  lcdFillCircle(cx - w / 2 + radius, cy, radius, eyeColor);
  lcdFillCircle(cx + w / 2 - radius, cy, radius, eyeColor);
}

static void drawPupil(int16_t cx, int16_t cy, int8_t lookX, int8_t lookY, uint16_t pupilColor) {
  int16_t px = cx + lookX;
  int16_t py = cy + lookY;
  lcdFillCircle(px, py, 18, pupilColor);
  lcdFillCircle(px - 7, py - 7, 5, COLOR_WHITE);
}

static void drawClosedLid(int16_t cx, int16_t cy, int16_t w, uint16_t color) {
  lcdDrawThickLine(cx - w / 2, cy, cx + w / 2, cy, 5, color);
}

static void drawHappyLid(int16_t cx, int16_t cy, int16_t w, uint16_t color) {
  for (int16_t x = -w / 2; x <= w / 2; x += 3) {
    int16_t y = cy + 14 + (x * x) / 90;
    lcdFillCircle(cx + x, y, 3, color);
  }
}

static void drawAngryBrows(uint16_t color) {
  lcdDrawThickLine(0, 36, 96, 54, 6, color);
  lcdDrawThickLine(320, 36, 224, 54, 6, color);
}

static void scheduleNextBlink(uint32_t now) {
  g_nextBlinkMs = now + 2500 + (esp_random() % 4500);
}

static void drawSleepMarks() {
  lcdDrawText(244, 24, "Z", COLOR_CYAN, COLOR_BLACK, 3);
  lcdDrawText(272, 8, "Z", COLOR_WHITE, COLOR_BLACK, 2);
}

static void drawEyeSparkle(int16_t x, int16_t y, uint16_t color) {
  lcdDrawThickLine(x - 10, y, x + 10, y, 2, color);
  lcdDrawThickLine(x, y - 10, x, y + 10, 2, color);
  lcdDrawThickLine(x - 7, y - 7, x + 7, y + 7, 1, color);
  lcdDrawThickLine(x - 7, y + 7, x + 7, y - 7, 1, color);
}

static void drawHeartEye(int16_t x, int16_t y) {
  drawHeart(x - 24, y - 20, 5, COLOR_RED);
}


static uint8_t faceIndexForExpression(const String &key) {
  if (nameHas(key, "wink") || nameHas(key, "blush") || nameHas(key, "helpless")) return 1;
  if (nameHas(key, "surprised") || nameHas(key, "focused") || nameHas(key, "thinking")) return 2;
  if (nameHas(key, "angry") || nameHas(key, "cool") || nameHas(key, "sad")) return 3;
  return 0;
}

static void updateFaceSlideshow() {
  if (!g_eyeScreenActive || g_blinkClosed || LcdFaces::kFaceCount <= 1) return;
  uint32_t now = millis();
  if (g_nextFaceSlideMs == 0) {
    g_nextFaceSlideMs = now + 3000;
    return;
  }
  if (now < g_nextFaceSlideMs) return;
  lcdDrawFaceImage((g_currentFaceIndex + 1) % LcdFaces::kFaceCount);
}

static void showEyePair(String expression, bool forceClosed = false, bool clearScreen = true) {
  expression = sanitizeText(expression);
  String key = expression;
  key.toLowerCase();

  g_currentEyeExpression = expression;
  g_eyeScreenActive = true;

  if (!forceClosed) {
    lcdDrawFaceImage(faceIndexForExpression(key));
    return;
  }

  lcdSetRotation(true);
  if (clearScreen) {
    lcdFillScreen(COLOR_BLACK);
  } else {
    lcdFillRect(0, 28, 126, 98, COLOR_BLACK);
    lcdFillRect(194, 28, 126, 98, COLOR_BLACK);
  }

  uint16_t eyeColor = COLOR_CYAN;
  uint16_t pupilColor = COLOR_BLUE;
  int8_t lookX = 0;
  int8_t lookY = 0;
  bool closed = false;
  bool happy = false;
  bool heart = false;
  bool angry = false;
  bool sleepy = false;
  bool surprised = false;
  bool cool = false;
  bool sparkle = false;

  if (nameHas(key, "heart")) {
    heart = true;
    eyeColor = COLOR_PINK;
  } else if (nameHas(key, "sad")) {
    eyeColor = COLOR_BLUE;
    lookY = 10;
  } else if (nameHas(key, "surprised")) {
    surprised = true;
    eyeColor = COLOR_WHITE;
    pupilColor = COLOR_BLACK;
  } else if (nameHas(key, "angry")) {
    angry = true;
    eyeColor = COLOR_ORANGE;
    pupilColor = COLOR_RED;
  } else if (nameHas(key, "sleep")) {
    closed = true;
    sleepy = true;
    eyeColor = COLOR_CYAN;
  } else if (nameHas(key, "cool")) {
    cool = true;
    eyeColor = COLOR_WHITE;
  } else if (nameHas(key, "wink")) {
    happy = true;
    eyeColor = COLOR_YELLOW;
  } else if (nameHas(key, "thinking")) {
    lookX = -10;
    eyeColor = COLOR_YELLOW;
  } else if (nameHas(key, "blush") || nameHas(key, "helpless")) {
    happy = true;
    eyeColor = COLOR_PINK;
  } else if (nameHas(key, "focused")) {
    lookX = 6;
    eyeColor = COLOR_GREEN;
    sparkle = true;
  } else {
    eyeColor = COLOR_CYAN;
  }

  const int16_t leftX = 50;
  const int16_t rightX = 270;
  const int16_t eyeY = 82;
  int16_t eyeW = surprised ? 100 : 100;
  int16_t eyeH = surprised ? 86 : 62;

  if (forceClosed && !heart && !cool) {
    drawClosedLid(leftX, eyeY, 100, eyeColor);
    drawClosedLid(rightX, eyeY, 100, eyeColor);
  } else if (closed) {
    drawClosedLid(leftX, eyeY, 100, eyeColor);
    drawClosedLid(rightX, eyeY, 100, eyeColor);
    if (sleepy) drawSleepMarks();
  } else if (heart) {
    drawHeartEye(leftX, eyeY);
    drawHeartEye(rightX, eyeY);
  } else if (cool) {
    lcdFillRect(0, 54, 104, 42, COLOR_WHITE);
    lcdFillRect(216, 54, 104, 42, COLOR_WHITE);
    lcdFillRect(4, 58, 96, 34, COLOR_BLACK);
    lcdFillRect(220, 58, 96, 34, COLOR_BLACK);
    lcdDrawThickLine(104, 74, 216, 74, 4, COLOR_WHITE);
  } else if (happy) {
    drawHappyLid(leftX, eyeY - 8, 96, eyeColor);
    drawHappyLid(rightX, eyeY - 8, 96, eyeColor);
  } else {
    drawEyeFrame(leftX, eyeY, eyeW, eyeH, eyeColor);
    drawEyeFrame(rightX, eyeY, eyeW, eyeH, eyeColor);
    drawPupil(leftX, eyeY, lookX, lookY, pupilColor);
    drawPupil(rightX, eyeY, lookX, lookY, pupilColor);
  }

  if (!clearScreen && !(forceClosed && !heart && !cool)) {
    lcdFillRect(0, 28, 126, 24, COLOR_BLACK);
    lcdFillRect(194, 28, 126, 24, COLOR_BLACK);
  }
  if (angry) drawAngryBrows(COLOR_RED);
  if (sparkle) {
    drawEyeSparkle(52, 42, COLOR_WHITE);
    drawEyeSparkle(268, 42, COLOR_WHITE);
  }
  if (nameHas(key, "sad")) {
    lcdFillCircle(104, 112, 6, COLOR_BLUE);
    lcdFillCircle(284, 112, 6, COLOR_BLUE);
  }
  if (nameHas(key, "thinking")) {
    lcdDrawText(264, 26, "?", COLOR_YELLOW, COLOR_BLACK, 3);
  }

  if (clearScreen) {
    lcdDrawText(116, 150, expression, COLOR_DARK | COLOR_WHITE, COLOR_BLACK, 1);
  }
}

static void showExpressionFace(String expression) {
  g_blinkClosed = false;
  scheduleNextBlink(millis());
  showEyePair(expression);
}

static void updateBlink() {
  if (!g_eyeScreenActive) return;

  String key = g_currentEyeExpression;
  key.toLowerCase();
  if (nameHas(key, "sleep") || nameHas(key, "heart") || nameHas(key, "cool")) {
    return;
  }
  return;

  uint32_t now = millis();
  if (g_nextBlinkMs == 0) {
    scheduleNextBlink(now);
  }

  if (!g_blinkClosed && now >= g_nextBlinkMs) {
    g_blinkClosed = true;
    g_blinkOpenMs = now + 120;
    showEyePair(g_currentEyeExpression, true, false);
    return;
  }

  if (g_blinkClosed && now >= g_blinkOpenMs) {
    g_blinkClosed = false;
    scheduleNextBlink(now);
    showEyePair(g_currentEyeExpression, false, false);
  }
}

static bool appendClipHexChunk(const String &clipId, uint32_t offset, const String &hexData) {
  String path = clipTempPath(clipId);
  if (offset == 0) {
    if (!g_syncInProgress || g_syncClipId != clipId || g_syncWriteOffset != 0) {
      closeSyncFile();
      LittleFS.remove(path);
      g_syncClipId = clipId;
      g_syncExpectedBytes = 0;
      g_syncExpectedSha256 = "";
      g_syncReceivedBytes = 0;
      g_syncHadError = false;
      g_syncLastError = "";
      g_validatedClipId = "";
      g_syncInProgress = true;
      g_syncErrorShown = false;
      g_syncFirstChunkLogged = false;
      g_syncWriteOffset = 0;
      g_syncLastProgressPct = 255;
      debugLogAppend(String("CHUNK_RESTART id=") + clipId);
    }
  }
  if (!g_syncFirstChunkLogged) {
    g_syncFirstChunkLogged = true;
    String msg = String("FIRST_CHUNK id=") + clipId;
    msg += " offset=";
    msg += offset;
    msg += " expected=";
    msg += g_syncWriteOffset;
    msg += " hex=";
    msg += hexData.length();
    debugLogAppend(msg);
  }
  if (g_syncClipId.length() && g_syncClipId != clipId) {
    Serial.printf("clip chunk reject id=%s active=%s offset=%lu\n",
                  clipId.c_str(), g_syncClipId.c_str(), (unsigned long)offset);
    if (!g_syncErrorShown) {
      String msg = String("ERROR BAD_CHUNK_ID id=") + clipId;
      msg += " active=";
      msg += g_syncClipId;
      msg += " offset=";
      msg += offset;
      debugLogAppend(msg);
    }
    g_syncHadError = true;
    g_syncLastError = "BAD CHUNK";
    closeSyncFile();
    return false;
  }
  if (g_syncWriteOffset != offset || (hexData.length() % 2) != 0) {
    Serial.printf("clip chunk reject id=%s offset=%lu expected=%lu hex=%u\n",
                  clipId.c_str(), (unsigned long)offset, (unsigned long)g_syncWriteOffset, hexData.length());
    if (!g_syncErrorShown) {
      String msg = String("ERROR BAD_CHUNK id=") + clipId;
      msg += " offset=";
      msg += offset;
      msg += " expected=";
      msg += g_syncWriteOffset;
      msg += " hex=";
      msg += hexData.length();
      msg += " received=";
      msg += g_syncReceivedBytes;
      msg += " target=";
      msg += g_syncExpectedBytes;
      debugLogAppend(msg);
    }
    g_syncHadError = true;
    g_syncLastError = "BAD CHUNK";
    closeSyncFile();
    return false;
  }
  if (!g_syncFile) {
    g_syncFile = LittleFS.open(path, offset == 0 ? "w" : "a");
  }
  if (!g_syncFile) {
    if (!g_syncErrorShown) {
      debugLogAppend(String("ERROR OPEN_FAIL id=") + clipId + " offset=" + String(offset));
    }
    g_syncHadError = true;
    g_syncLastError = "OPEN FAIL";
    return false;
  }
  uint8_t buffer[64];
  size_t used = 0;
  for (uint16_t i = 0; i < hexData.length(); i += 2) {
    int high = hexValue(hexData[i]);
    int low = hexValue(hexData[i + 1]);
    if (high < 0 || low < 0) {
      if (!g_syncErrorShown) {
        debugLogAppend(String("ERROR BAD_HEX id=") + clipId + " offset=" + String(offset) + " i=" + String(i));
      }
      g_syncHadError = true;
      g_syncLastError = "BAD HEX";
      closeSyncFile();
      return false;
    }
    buffer[used++] = (uint8_t)((high << 4) | low);
    if (used == sizeof(buffer)) {
      if (g_syncFile.write(buffer, used) != used) {
        if (!g_syncErrorShown) {
          debugLogAppend(String("ERROR WRITE_FAIL id=") + clipId + " offset=" + String(offset) + " wrote=" + String(g_syncWriteOffset));
        }
        g_syncHadError = true;
        g_syncLastError = "WRITE FAIL";
        closeSyncFile();
        return false;
      }
      used = 0;
    }
  }
  if (used && g_syncFile.write(buffer, used) != used) {
    if (!g_syncErrorShown) {
      debugLogAppend(String("ERROR WRITE_FAIL_TAIL id=") + clipId + " offset=" + String(offset) + " wrote=" + String(g_syncWriteOffset));
    }
    g_syncHadError = true;
    g_syncLastError = "WRITE FAIL";
    closeSyncFile();
    return false;
  }
  g_syncWriteOffset = offset + (hexData.length() / 2);
  g_syncReceivedBytes = g_syncWriteOffset;
  showClipSyncProgress(false);
  return true;
}

static bool validateClipFile(const String &clipId, String &status, String &detail, const String &pathOverride = "") {
  String path = pathOverride.length() ? pathOverride : clipPath(clipId);
  File file = LittleFS.open(path, "r");
  if (!file) {
    status = "MISSING";
    detail = clipId;
    return false;
  }
  uint32_t actualSize = file.size();
  file.close();

  if (g_syncHadError) {
    status = g_syncLastError.length() ? g_syncLastError : "SYNC ERROR";
    detail = clipDiagLine(actualSize, g_syncExpectedBytes);
    debugLogAppend(String("VALIDATE_SYNC_ERROR id=") + clipId + " status=" + status + " detail=" + detail);
    return false;
  }
  if (g_syncExpectedBytes > 0 && actualSize != g_syncExpectedBytes) {
    status = "BAD SIZE";
    detail = clipDiagLine(actualSize, g_syncExpectedBytes);
    debugLogAppend(String("VALIDATE_BAD_SIZE id=") + clipId + " detail=" + detail);
    return false;
  }
  if (g_syncExpectedSha256.length()) {
    String actualSha;
    uint32_t shaSize = 0;
    if (!fileSha256Hex(path, actualSha, &shaSize)) {
      status = "SHA FAIL";
      detail = clipId;
      debugLogAppend(String("VALIDATE_SHA_FAIL id=") + clipId + " size=" + String(shaSize));
      return false;
    }
    if (!equalsIgnoreCase(actualSha, g_syncExpectedSha256)) {
      status = "BAD SHA";
      detail = actualSha.substring(0, 8);
      String msg = String("VALIDATE_BAD_SHA id=") + clipId;
      msg += " actual=";
      msg += actualSha;
      msg += " expected=";
      msg += g_syncExpectedSha256;
      msg += " size=";
      msg += shaSize;
      debugLogAppend(msg);
      return false;
    }
  }

  status = "READY";
  detail = clipDiagLine(actualSize, g_syncExpectedBytes);
  debugLogAppend(String("VALIDATE_READY id=") + clipId + " detail=" + detail);
  return true;
}

static bool openClipForPlayback(const String &clipId) {
  stopClipPlayback();
  if (g_validatedClipId != clipId) {
    String status, detail;
    if (!validateClipFile(clipId, status, detail)) {
      Serial.printf("clip validation failed: %s %s\n", status.c_str(), detail.c_str());
      showScreen("CLIP", status, detail, COLOR_RED);
      return false;
    }
    g_validatedClipId = clipId;
  }

  String path = clipPath(clipId);
  g_clipFile = LittleFS.open(path, "r");
  if (!g_clipFile) {
    Serial.printf("clip not cached: %s\n", clipId.c_str());
    return false;
  }
  if (g_clipFile.size() < 14) {
    stopClipPlayback();
    Serial.printf("clip too small: %s size=%lu\n", clipId.c_str(), (unsigned long)g_clipFile.size());
    showScreen("CLIP", "TOO SMALL", clipId, COLOR_RED);
    return false;
  }
  char magic[7] = {0};
  if (g_clipFile.readBytes(magic, 6) != 6 || strcmp(magic, "LGLCD1") != 0) {
    stopClipPlayback();
    Serial.printf("clip bad magic: %s\n", clipId.c_str());
    showScreen("CLIP", "BAD MAGIC", clipId, COLOR_RED);
    return false;
  }
  uint16_t width = readLe16(g_clipFile);
  uint16_t height = readLe16(g_clipFile);
  g_clipFrameCount = readLe16(g_clipFile);
  g_clipFps = readLe16(g_clipFile);
  if (width != C6_CLIP_WIDTH || height != C6_CLIP_HEIGHT || g_clipFrameCount == 0 ||
      g_clipFps < C6_CLIP_MIN_FPS || g_clipFps > C6_CLIP_MAX_FPS) {
    stopClipPlayback();
    Serial.printf("clip bad header: %s size=%ux%u frames=%u fps=%u allowed=%d-%d\n",
                  clipId.c_str(), width, height, g_clipFrameCount, g_clipFps,
                  C6_CLIP_MIN_FPS, C6_CLIP_MAX_FPS);
    showScreen("CLIP", "BAD HEADER", clipId, COLOR_RED);
    return false;
  }
  g_clipId = clipId;
  g_clipPlaying = true;
  g_clipFrameIndex = 0;
  g_nextClipFrameMs = 0;
  g_eyeScreenActive = false;
  lcdSetRotation(true);
  // A new clip starts from a black canvas. Loop rewinds bypass this function so
  // they can diff the last frame directly against the first without a flash.
  g_clipShadowValid = false;
  return true;
}

static void resetClipPerformance() {
  g_clipPerfFrames = 0;
  g_clipPerfDecodeUs = 0;
  g_clipPerfLcdUs = 0;
  g_clipPerfMaxFrameUs = 0;
  g_clipPerfBudgetMisses = 0;
  g_clipPerfLcdPixels = 0;
  g_clipPerfLcdRects = 0;
}

static void recordClipPerformance(uint32_t decodeUs,
                                  uint32_t lcdUs,
                                  uint32_t frameUs,
                                  uint32_t frameBudgetUs,
                                  uint32_t lcdPixels,
                                  uint32_t lcdRects) {
  g_clipPerfFrames++;
  g_clipPerfDecodeUs += decodeUs;
  g_clipPerfLcdUs += lcdUs;
  g_clipPerfLcdPixels += lcdPixels;
  g_clipPerfLcdRects += lcdRects;
  if (frameUs > g_clipPerfMaxFrameUs) g_clipPerfMaxFrameUs = frameUs;
  if (frameUs > frameBudgetUs) g_clipPerfBudgetMisses++;
  if (g_clipPerfFrames < C6_CLIP_PERF_REPORT_FRAMES) return;

  Serial.printf(
      "clip perf id=%s frames=%lu decode_avg=%luus lcd_avg=%luus "
      "frame_max=%luus budget_miss=%lu lcd_pixels=%lu lcd_rects=%lu\n",
      g_clipId.c_str(),
      (unsigned long)g_clipPerfFrames,
      (unsigned long)(g_clipPerfDecodeUs / g_clipPerfFrames),
      (unsigned long)(g_clipPerfLcdUs / g_clipPerfFrames),
      (unsigned long)g_clipPerfMaxFrameUs,
      (unsigned long)g_clipPerfBudgetMisses,
      (unsigned long)g_clipPerfLcdPixels,
      (unsigned long)g_clipPerfLcdRects);
  resetClipPerformance();
}

static uint32_t lcdWriteClipShadowRect(uint16_t x,
                                       uint16_t y,
                                       uint16_t w,
                                       uint16_t h) {
  if (w == 0 || h == 0) return 0;
  lcdSetWindow(x, y, w, h);
  lcdSpi.beginTransaction(SPISettings(C6_LCD_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(LCD_DC, HIGH);
  digitalWrite(LCD_CS, LOW);

  if (x == 0 && w == C6_CLIP_WIDTH) {
    const size_t offset = (size_t)y * C6_CLIP_WIDTH * 2;
    lcdSpi.writeBytes(g_clipShadow + offset, (uint32_t)w * h * 2);
  } else {
    for (uint16_t row = 0; row < h; ++row) {
      const size_t offset =
          ((size_t)(y + row) * C6_CLIP_WIDTH + x) * 2;
      lcdSpi.writeBytes(g_clipShadow + offset, (uint32_t)w * 2);
    }
  }

  digitalWrite(LCD_CS, HIGH);
  lcdSpi.endTransaction();
  return (uint32_t)w * h;
}

static uint32_t lcdWriteDirtyClipTiles(
    const bool dirtyTiles[C6_CLIP_TILE_ROWS][C6_CLIP_TILE_COLS],
    uint32_t &rectCount) {
  uint32_t pixelCount = 0;
  rectCount = 0;
  for (uint16_t tileY = 0; tileY < C6_CLIP_TILE_ROWS; ++tileY) {
    int16_t runStart = -1;
    for (uint16_t tileX = 0; tileX <= C6_CLIP_TILE_COLS; ++tileX) {
      const bool dirty =
          tileX < C6_CLIP_TILE_COLS && dirtyTiles[tileY][tileX];
      if (dirty && runStart < 0) {
        runStart = tileX;
        continue;
      }
      if (dirty || runStart < 0) continue;

      const uint16_t x = (uint16_t)runStart * C6_CLIP_TILE_SIZE;
      const uint16_t xEnd =
          min((uint16_t)(tileX * C6_CLIP_TILE_SIZE),
              (uint16_t)C6_CLIP_WIDTH);
      const uint16_t y = tileY * C6_CLIP_TILE_SIZE;
      const uint16_t yEnd =
          min((uint16_t)(y + C6_CLIP_TILE_SIZE),
              (uint16_t)C6_CLIP_HEIGHT);
      pixelCount += lcdWriteClipShadowRect(x, y, xEnd - x, yEnd - y);
      rectCount++;
      runStart = -1;
    }
  }
  return pixelCount;
}

static bool drawNextClipFrame() {
  if (!g_clipPlaying || !g_clipFile) return false;
  if (g_clipFrameIndex >= g_clipFrameCount) {
    if (!g_clipLoop) {
      stopClipPlayback();
      showEyePair(g_currentEyeExpression);
      return false;
    }
    if (!g_clipFile.seek(14)) {
      Serial.printf("clip rewind failed: %s\n", g_clipId.c_str());
      stopClipPlayback();
      showScreen("CLIP", "REWIND FAIL", "", COLOR_RED);
      return false;
    }
    g_clipFrameIndex = 0;
  }

  if (g_clipFile.available() < 12) {
    Serial.printf("clip missing frame header frame=%u available=%d\n",
                  g_clipFrameIndex, g_clipFile.available());
    stopClipPlayback();
    showScreen("CLIP", "TRUNC FRAME", String(g_clipFrameIndex), COLOR_RED);
    return false;
  }
  const uint32_t frameStartedUs = micros();
  uint16_t x = readLe16(g_clipFile);
  uint16_t y = readLe16(g_clipFile);
  uint16_t w = readLe16(g_clipFile);
  uint16_t h = readLe16(g_clipFile);
  uint16_t durationMs = readLe16(g_clipFile);
  uint16_t runCount = readLe16(g_clipFile);
  if (w == 0 || h == 0 || x + w > C6_CLIP_WIDTH || y + h > C6_CLIP_HEIGHT) {
    Serial.printf("clip bad rect frame=%u x=%u y=%u w=%u h=%u\n",
                  g_clipFrameIndex, x, y, w, h);
    stopClipPlayback();
    showScreen("CLIP", "BAD RECT", String(g_clipFrameIndex), COLOR_RED);
    return false;
  }
  if ((uint32_t)g_clipFile.available() < (uint32_t)runCount * 4UL) {
    Serial.printf("clip truncated rle frame=%u runs=%u available=%d\n",
                  g_clipFrameIndex, runCount, g_clipFile.available());
    stopClipPlayback();
    showScreen("CLIP", "TRUNC RLE", String(g_clipFrameIndex), COLOR_RED);
    return false;
  }

  bool dirtyTiles[C6_CLIP_TILE_ROWS][C6_CLIP_TILE_COLS] = {};
  const bool forceFullRefresh = !g_clipShadowValid;
  if (forceFullRefresh) {
    memset(g_clipShadow, 0, sizeof(g_clipShadow));
  }

  uint32_t writtenPixels = 0;
  const uint32_t expectedPixels = (uint32_t)w * h;
  for (uint16_t run = 0; run < runCount; ++run) {
    uint16_t count = readLe16(g_clipFile);
    uint16_t color = readLe16(g_clipFile);
    if (writtenPixels + count > expectedPixels) {
      g_clipShadowValid = false;
      Serial.printf("clip bad pixels frame=%u written=%lu count=%u expected=%lu\n",
                    g_clipFrameIndex, (unsigned long)writtenPixels, count, (unsigned long)expectedPixels);
      stopClipPlayback();
      showScreen("CLIP", "BAD PIXELS", String(g_clipFrameIndex), COLOR_RED);
      return false;
    }
    uint8_t high = color >> 8;
    uint8_t low = color & 0xFF;
    uint16_t remaining = count;
    while (remaining > 0) {
      const uint16_t localY = writtenPixels / w;
      const uint16_t localX = writtenPixels - (uint32_t)localY * w;
      const uint16_t rowPixels = min(remaining, (uint16_t)(w - localX));
      const uint16_t screenX = x + localX;
      const uint16_t screenY = y + localY;
      size_t offset =
          ((size_t)screenY * C6_CLIP_WIDTH + screenX) * 2;
      for (uint16_t i = 0; i < rowPixels; ++i, offset += 2) {
        if (forceFullRefresh) {
          g_clipShadow[offset] = high;
          g_clipShadow[offset + 1] = low;
        } else if (g_clipShadow[offset] != high ||
                   g_clipShadow[offset + 1] != low) {
          dirtyTiles[screenY / C6_CLIP_TILE_SIZE]
                    [(screenX + i) / C6_CLIP_TILE_SIZE] = true;
          g_clipShadow[offset] = high;
          g_clipShadow[offset + 1] = low;
        }
      }
      writtenPixels += rowPixels;
      remaining -= rowPixels;
    }
  }
  if (writtenPixels != expectedPixels) {
    g_clipShadowValid = false;
    Serial.printf("clip short pixels frame=%u written=%lu expected=%lu\n",
                  g_clipFrameIndex, (unsigned long)writtenPixels, (unsigned long)expectedPixels);
    stopClipPlayback();
    showScreen("CLIP", "SHORT PIX", String(g_clipFrameIndex), COLOR_RED);
    return false;
  }

  const uint32_t decodedAtUs = micros();
  uint32_t lcdRects = 0;
  uint32_t lcdPixels = 0;
  if (forceFullRefresh) {
    lcdPixels =
        lcdWriteClipShadowRect(0, 0, C6_CLIP_WIDTH, C6_CLIP_HEIGHT);
    lcdRects = 1;
  } else {
    lcdPixels = lcdWriteDirtyClipTiles(dirtyTiles, lcdRects);
  }
  g_clipShadowValid = true;
  const uint32_t renderedAtUs = micros();

  g_clipFrameIndex++;
  const uint32_t frameDurationMs =
      durationMs ? durationMs : (1000 / max((uint16_t)1, g_clipFps));
  const uint32_t renderedAtMs = millis();
  if (g_nextClipFrameMs == 0 ||
      (int32_t)(renderedAtMs - g_nextClipFrameMs) >= (int32_t)frameDurationMs) {
    // Rebase after a large miss instead of trying to render a burst of stale frames.
    g_nextClipFrameMs = renderedAtMs + frameDurationMs;
  } else {
    // Keep the next deadline anchored to the prior deadline. This subtracts LCD
    // draw time from the wait and avoids the old "draw time + frame time" drift.
    g_nextClipFrameMs += frameDurationMs;
  }
  recordClipPerformance(
      decodedAtUs - frameStartedUs,
      renderedAtUs - decodedAtUs,
      renderedAtUs - frameStartedUs,
      frameDurationMs * 1000UL,
      lcdPixels,
      lcdRects);
  return true;
}

static void updateClipPlayback() {
  if (!g_clipPlaying) return;
  uint32_t now = millis();
  if (g_nextClipFrameMs == 0 || (int32_t)(now - g_nextClipFrameMs) >= 0) {
    drawNextClipFrame();
  }
}

static bool handleClipLine(const String &line) {
  String type = jsonValue(line, "type");
  if (!type.startsWith("clip_")) return false;
  String clipId = jsonValue(line, "clip_id");
  if (type == "clip_begin") {
    stopClipPlayback();
    closeSyncFile();
    uint32_t lcdBytes = (uint32_t)jsonNumber(line, "lcd_bytes", 0);
    int fps = (int)jsonNumber(line, "fps", 0);
    int frameCount = (int)jsonNumber(line, "frame_count", 0);
    int durationMs = (int)jsonNumber(line, "duration_ms", 0);
    g_syncClipId = clipId;
    g_syncExpectedBytes = lcdBytes;
    g_syncExpectedSha256 = jsonValue(line, "lcd_sha256");
    g_syncReceivedBytes = 0;
    g_syncHadError = false;
    g_syncLastError = "";
    g_validatedClipId = "";
    g_syncInProgress = true;
    g_syncErrorShown = false;
    g_syncFirstChunkLogged = false;
    g_syncWriteOffset = 0;
    g_syncLastProgressPct = 255;
    debugLogReset(String("clip_begin id=") + clipId);
    String beginMsg = String("BEGIN id=") + clipId;
    beginMsg += " bytes=";
    beginMsg += lcdBytes;
    beginMsg += " sha=";
    beginMsg += g_syncExpectedSha256;
    beginMsg += " fps=";
    beginMsg += fps;
    beginMsg += " frames=";
    beginMsg += frameCount;
    beginMsg += " duration=";
    beginMsg += durationMs;
    beginMsg += " lineLen=";
    beginMsg += line.length();
    debugLogAppend(beginMsg);
    if (lcdBytes > 256UL * 1024UL) {
      g_syncInProgress = false;
      g_syncHadError = true;
      g_syncLastError = "TOO LARGE";
      debugLogAppend(String("ERROR TOO_LARGE id=") + clipId + " bytes=" + String(lcdBytes));
      showScreen("CLIP", "TOO LARGE", clipId, COLOR_RED);
      sendClipAck(clipId, "begin", false, 0, g_syncLastError);
      return true;
    }
    const uint32_t maxClipBytes = 256UL * 1024UL;
    bool productPartition = LittleFS.totalBytes() >= 5UL * 1024UL * 1024UL;
    const uint32_t installedBudgetBytes = productPartition ? 3UL * 1024UL * 1024UL : 896UL * 1024UL;
    const uint32_t reservedBytes = productPartition ? 1024UL * 1024UL : 256UL * 1024UL;
    const int maxClipCount = productPartition ? 10 : 5;
    uint32_t installedBytes = 0;
    int installedCount = 0;
    clipStorageUsage(installedBytes, installedCount);
    String targetPath = clipPath(clipId);
    File existing = LittleFS.open(targetPath, "r");
    uint32_t existingBytes = existing ? existing.size() : 0;
    bool replacing = (bool)existing;
    if (existing) existing.close();
    uint32_t fsTotal = LittleFS.totalBytes();
    uint32_t fsUsed = LittleFS.usedBytes();
    uint32_t fsFree = fsTotal > fsUsed ? fsTotal - fsUsed : 0;
    if (lcdBytes > maxClipBytes || (!replacing && installedCount >= maxClipCount) ||
        installedBytes - existingBytes + lcdBytes > installedBudgetBytes ||
        fsFree < lcdBytes + reservedBytes) {
      g_syncInProgress = false;
      g_syncHadError = true;
      g_syncLastError = "NO SPACE";
      debugLogAppend(String("ERROR NO_SPACE id=") + clipId + " bytes=" + String(lcdBytes) +
                     " installed=" + String(installedBytes) + " count=" + String(installedCount));
      showScreen("CLIP", "NO SPACE", clipId, COLOR_RED);
      sendClipAck(clipId, "begin", false, 0, g_syncLastError);
      return true;
    }
    LittleFS.remove(clipTempPath(clipId));
    g_syncFile = LittleFS.open(clipTempPath(clipId), "w");
    if (!g_syncFile) {
      g_syncInProgress = false;
      g_syncHadError = true;
      g_syncLastError = "OPEN FAIL";
      debugLogAppend(String("ERROR OPEN_FAIL_BEGIN id=") + clipId);
      showScreen("CLIP", "OPEN FAIL", clipId, COLOR_RED);
      sendClipAck(clipId, "begin", false, 0, g_syncLastError);
      return true;
    }
    showClipSyncProgress(true);
    sendClipAck(clipId, "begin", true, 0);
    return true;
  }
  if (type == "clip_chunk") {
    uint32_t offset = (uint32_t)jsonNumber(line, "offset", 0);
    String data = jsonValue(line, "data");
    bool chunkOk = appendClipHexChunk(clipId, offset, data);
    if (!chunkOk) {
      if (!g_syncErrorShown) {
        g_syncErrorShown = true;
        Serial.printf("clip sync error id=%s offset=%lu error=%s\n",
                      clipId.c_str(), (unsigned long)offset, g_syncLastError.c_str());
      }
    }
    sendClipAck(clipId, "chunk", chunkOk, g_syncWriteOffset, chunkOk ? String("") : g_syncLastError);
    return true;
  }
  if (type == "clip_commit") {
    closeSyncFile();
    debugLogAppend(String("COMMIT_RX id=") + clipId + " received=" + String(g_syncReceivedBytes) + " expected=" + String(g_syncExpectedBytes));
    String status, detail;
    String temporaryPath = clipTempPath(clipId);
    if (validateClipFile(clipId, status, detail, temporaryPath)) {
      String finalPath = clipPath(clipId);
      String backupPath = finalPath + ".bak";
      LittleFS.remove(backupPath);
      bool hadPrevious = LittleFS.exists(finalPath);
      if (hadPrevious && !LittleFS.rename(finalPath, backupPath)) {
        g_validatedClipId = "";
        g_syncInProgress = false;
        debugLogAppend(String("COMMIT_FAIL id=") + clipId + " status=BACKUP_FAIL");
        showScreen("CLIP", "BACKUP FAIL", clipId, COLOR_RED);
        sendClipAck(clipId, "commit", false, g_syncReceivedBytes, "BACKUP FAIL");
        return true;
      }
      if (!LittleFS.rename(temporaryPath, finalPath)) {
        if (hadPrevious) LittleFS.rename(backupPath, finalPath);
        g_validatedClipId = "";
        g_syncInProgress = false;
        debugLogAppend(String("COMMIT_FAIL id=") + clipId + " status=RENAME_FAIL");
        showScreen("CLIP", "RENAME FAIL", clipId, COLOR_RED);
        sendClipAck(clipId, "commit", false, g_syncReceivedBytes, "RENAME FAIL");
        return true;
      }
      LittleFS.remove(backupPath);
      g_validatedClipId = clipId;
      g_syncInProgress = false;
      debugLogAppend(String("COMMIT_OK id=") + clipId + " detail=" + detail + " sha=" + g_syncExpectedSha256);
      showScreen("CLIP", "READY", detail, COLOR_GREEN);
      Serial.printf("clip ready id=%s size=%s sha=%s\n",
                    clipId.c_str(), detail.c_str(), g_syncExpectedSha256.c_str());
      sendClipAck(clipId, "commit", true, g_syncReceivedBytes);
    } else {
      LittleFS.remove(temporaryPath);
      g_validatedClipId = "";
      g_syncInProgress = false;
      debugLogAppend(String("COMMIT_FAIL id=") + clipId + " status=" + status + " detail=" + detail + " sha=" + g_syncExpectedSha256);
      showScreen("CLIP", status, detail, COLOR_RED);
      Serial.printf("clip commit failed id=%s status=%s detail=%s expected_sha=%s\n",
                    clipId.c_str(), status.c_str(), detail.c_str(), g_syncExpectedSha256.c_str());
      sendClipAck(clipId, "commit", false, g_syncReceivedBytes, status);
    }
    return true;
  }
  if (type == "clip_play") {
    closeSyncFile();
    bool loopPlayback = jsonValue(line, "playback") == "loop";
    if (!openClipForPlayback(clipId)) {
      Serial.printf("clip play failed id=%s\n", clipId.c_str());
    } else {
      g_clipLoop = loopPlayback;
      resetClipPerformance();
    }
    return true;
  }
  if (type == "clip_stop") {
    closeSyncFile();
    stopClipPlayback();
    showEyePair(g_currentEyeExpression);
    return true;
  }
  return true;
}

static void showStatusLine(const String &line) {
  if (handleClipLine(line)) return;
  if (g_syncInProgress) {
    g_syncHadError = true;
    g_syncLastError = "BAD UART";
    if (!g_syncErrorShown) {
      g_syncErrorShown = true;
      String msg = String("ERROR BAD_UART len=") + line.length();
      msg += " line=";
      msg += sanitizeText(line.substring(0, 96));
      debugLogAppend(msg);
    }
    Serial.printf("clip bad uart line len=%u: %s\n", line.length(), line.c_str());
    return;
  }
  String status = jsonValue(line, "status");
  String detail = jsonValue(line, "detail");
  String expression = jsonValue(line, "name");

  if (expression.length()) {
    stopClipPlayback();
    lastExpressionMs = millis();
    showExpressionFace(expression);
    return;
  }

  if (!status.length()) {
    g_eyeScreenActive = false;
    showScreen("UART", "RX DATA", sanitizeText(line), COLOR_BLUE);
    return;
  }

  status = sanitizeText(status);
  detail = sanitizeText(detail);
  if (status == "boot") {
    g_eyeScreenActive = false;
    showScreen("LAMPGO", "S3 BOOT", detail, COLOR_BLUE);
  } else if (status == "provision") {
    g_eyeScreenActive = false;
    showScreen("SETUP", "WiFi SETUP", detail, COLOR_YELLOW);
  } else if (status == "wifi_failed") {
    g_eyeScreenActive = false;
    showScreen("WIFI", "FAILED", detail, COLOR_RED);
  } else if (status == "service_failed") {
    g_eyeScreenActive = false;
    showScreen("ERROR", "SERVICE", detail, COLOR_RED);
  } else if (status == "ready") {
    showEyePair("focused");
  } else if (status == "online") {
    if (millis() - lastExpressionMs < 5000) return;
    if (lastRenderedLine.length()) return;
    g_eyeScreenActive = false;
    showScreen("ONLINE", "S3 Connected", "heartbeat ok", COLOR_GREEN);
  } else {
    g_eyeScreenActive = false;
    showScreen("STATUS", status, detail, COLOR_CYAN);
  }
}

static void lcdInit() {
  pinMode(LCD_CS, OUTPUT);
  pinMode(LCD_DC, OUTPUT);
  pinMode(LCD_RST, OUTPUT);
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_CS, HIGH);
  digitalWrite(LCD_BL, HIGH);

  lcdSpi.begin(LCD_SCLK, -1, LCD_MOSI, LCD_CS);
  lcdSpi.setFrequency(C6_LCD_SPI_HZ);

  digitalWrite(LCD_RST, HIGH);
  delay(20);
  digitalWrite(LCD_RST, LOW);
  delay(20);
  digitalWrite(LCD_RST, HIGH);
  delay(120);

  lcdWriteCommand(0x01);
  delay(150);
  lcdWriteCommand(0x11);
  delay(120);

  lcdWriteCommand(0x3A);
  lcdWriteData(0x55);

  lcdSetRotation(false);

  lcdWriteCommand(0x21);
  lcdWriteCommand(0x13);
  lcdWriteCommand(0x29);
  delay(20);

  lcdFillScreen(COLOR_BLACK);
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("ESP32-C6 LCD UART display booting...");

  lcdInit();
  g_fsReady = LittleFS.begin(true);
  if (!g_fsReady) {
    Serial.println("LittleFS mount failed; clip cache disabled");
  } else {
    g_nextDebugLogPrintMs = millis() + 1500;
  }
  scheduleNextBlink(millis());
  showEyePair("smiley");

  inputLine.reserve(768);
  linkSerial.setRxBufferSize(C6_LINK_RX_BUFFER);
  linkSerial.begin(C6_LINK_BAUD, SERIAL_8N1, C6_LINK_RX, C6_LINK_TX);
  Serial.printf("UART link RX=%d TX=%d baud=%d rxbuf=%d\n",
                C6_LINK_RX, C6_LINK_TX, C6_LINK_BAUD, C6_LINK_RX_BUFFER);
}

void loop() {
  while (linkSerial.available()) {
    char c = (char)linkSerial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      inputLine.trim();
      if (inputOverflow) {
        Serial.println("uart line overflow; dropped");
        g_syncHadError = true;
        g_syncLastError = "UART LONG";
        if (g_syncInProgress && !g_syncErrorShown) {
          g_syncErrorShown = true;
          debugLogAppend(String("ERROR UART_LONG len=") + inputLine.length());
        }
      } else if (inputLine.length()) {
        bool isClipChunk = inputLine.indexOf("\"type\":\"clip_chunk\"") >= 0;
        if (!isClipChunk) {
          Serial.println(inputLine);
        }
        lastMessageMs = millis();
        if (isClipChunk || inputLine != lastRenderedLine) {
          if (!isClipChunk) {
            lastRenderedLine = inputLine;
          }
          showStatusLine(inputLine);
        }
      }
      inputLine = "";
      inputOverflow = false;
    } else if (inputLine.length() < 768) {
      inputLine += c;
    } else {
      inputOverflow = true;
    }
  }

  updateClipPlayback();
  updateBlink();
  updateFaceSlideshow();
  updateDebugLogPrinter();
}
