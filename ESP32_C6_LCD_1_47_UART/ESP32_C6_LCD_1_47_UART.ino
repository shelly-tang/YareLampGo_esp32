#include <Arduino.h>
#include <SPI.h>
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
#define C6_LINK_BAUD 115200

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

static String sanitizeText(String text) {
  text.replace("_", " ");
  for (uint16_t i = 0; i < text.length(); ++i) {
    char c = text[i];
    if (c < 32 || c > 126) text.setCharAt(i, '?');
  }
  return text;
}

static void showScreen(const String &title, const String &line1, const String &line2, uint16_t accent) {
  lcdSetRotation(false);
  lcdFillScreen(COLOR_BLACK);
  lcdFillRect(0, 0, g_screenWidth, 38, accent);
  lcdDrawText(10, 10, title, COLOR_BLACK, accent, 2);
  lcdDrawText(10, 58, line1, COLOR_WHITE, COLOR_BLACK, 2);
  lcdDrawText(10, 98, line2, COLOR_CYAN, COLOR_BLACK, 1);
  lcdDrawText(10, 286, "UART 115200", COLOR_DARK | COLOR_WHITE, COLOR_BLACK, 1);
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

static void showStatusLine(const String &line) {
  String status = jsonValue(line, "status");
  String detail = jsonValue(line, "detail");
  String expression = jsonValue(line, "name");

  if (expression.length()) {
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
  lcdSpi.setFrequency(40000000);

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
  scheduleNextBlink(millis());
  showEyePair("smiley");

  linkSerial.begin(C6_LINK_BAUD, SERIAL_8N1, C6_LINK_RX, C6_LINK_TX);
  Serial.printf("UART link RX=%d TX=%d baud=%d\n", C6_LINK_RX, C6_LINK_TX, C6_LINK_BAUD);
}

void loop() {
  while (linkSerial.available()) {
    char c = (char)linkSerial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      inputLine.trim();
      if (inputLine.length()) {
        Serial.println(inputLine);
        lastMessageMs = millis();
        if (inputLine != lastRenderedLine) {
          lastRenderedLine = inputLine;
          showStatusLine(inputLine);
        }
      }
      inputLine = "";
    } else if (inputLine.length() < 240) {
      inputLine += c;
    }
  }

  updateBlink();
  updateFaceSlideshow();
}
