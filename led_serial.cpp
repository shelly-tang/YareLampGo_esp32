#include "led_serial.h"

#include <Arduino.h>
#include <ctype.h>
#include <esp_heap_caps.h>
#include <esp32-hal-rmt.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#ifndef LAMPGO_LED_PIXEL_PIN
#define LAMPGO_LED_PIXEL_PIN D2
#endif

#ifndef LAMPGO_LED_PANEL_PIXELS
#define LAMPGO_LED_PANEL_PIXELS 64
#endif

#ifndef LAMPGO_LED_PANEL_COUNT
#define LAMPGO_LED_PANEL_COUNT 2
#endif

#define LAMPGO_LED_PIXEL_COUNT (LAMPGO_LED_PANEL_PIXELS * LAMPGO_LED_PANEL_COUNT)

#ifndef LAMPGO_LED_TASK_STACK
#define LAMPGO_LED_TASK_STACK 4096
#endif

namespace {

SemaphoreHandle_t g_mutex = nullptr;
TaskHandle_t g_task = nullptr;
bool g_ready = false;
bool g_outputStarted = false;
bool g_rmtReady = false;
bool g_lastShowOk = false;
int g_mode = 0;
int g_brightness = 127;
uint32_t g_lastWriteMs = 0;
char g_lastCommand[32] = "";
uint32_t g_animFrame = 0;
uint32_t g_lastFrameMs = 0;
uint8_t g_pixels[LAMPGO_LED_PIXEL_COUNT * 3] = {0};
rmt_data_t *g_rmtData = nullptr;

const char *const kModeNames[30] = {
    "off", "red", "green", "blue", "white",
    "theater", "theaterred", "theatergreen", "theaterblue", "rainbow",
    "smiley", "crying", "left", "right", "check", "cross",
    "music", "blush", "angry", "surprised", "exclaim", "question",
    "star", "up", "down", "sleep", "thinking", "heart", "heartbreak", "helpless",
};

const uint8_t kPatterns[16][8][8] = {
    {{0,0,0,0,0,0,0,0},{0,1,0,0,0,0,1,0},{1,0,1,0,0,1,0,1},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,1,1,1,1,0,0},{0,0,0,0,0,0,0,0}},
    {{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{1,1,1,0,0,1,1,1},{0,0,0,0,0,0,0,0},{0,0,0,1,1,0,0,0},{0,0,1,0,0,1,0,0},{0,1,0,0,0,0,1,0},{0,0,0,0,0,0,0,0}},
    {{0,0,0,1,0,0,0,0},{0,0,1,0,0,0,0,0},{0,1,0,0,0,0,0,0},{1,1,1,1,1,1,1,1},{1,1,1,1,1,1,1,1},{0,1,0,0,0,0,0,0},{0,0,1,0,0,0,0,0},{0,0,0,1,0,0,0,0}},
    {{0,0,0,0,1,0,0,0},{0,0,0,0,0,1,0,0},{0,0,0,0,0,0,1,0},{1,1,1,1,1,1,1,1},{1,1,1,1,1,1,1,1},{0,0,0,0,0,0,1,0},{0,0,0,0,0,1,0,0},{0,0,0,0,1,0,0,0}},
    {{0,0,0,0,0,0,0,1},{0,0,0,0,0,0,1,1},{0,0,0,0,0,1,1,0},{1,0,0,0,1,1,0,0},{1,1,0,1,1,0,0,0},{0,1,1,1,0,0,0,0},{0,0,1,0,0,0,0,0},{0,0,0,0,0,0,0,0}},
    {{1,0,0,0,0,0,0,1},{1,1,0,0,0,0,1,1},{0,1,1,0,0,1,1,0},{0,0,1,1,1,1,0,0},{0,0,1,1,1,1,0,0},{0,1,1,0,0,1,1,0},{1,1,0,0,0,0,1,1},{1,0,0,0,0,0,0,1}},
    {{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,1,0,0,0,0,1,0},{1,1,1,0,0,1,1,1},{1,1,1,0,0,1,1,1},{0,1,0,0,0,0,1,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0}},
    {{0,0,0,0,0,0,0,0},{1,0,0,0,0,0,0,1},{0,1,0,0,0,0,1,0},{0,0,1,0,0,1,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,1,1,1,1,1,1,0},{1,0,0,0,0,0,0,1}},
    {{0,0,0,0,0,0,0,0},{0,1,1,0,0,1,1,0},{0,0,0,0,0,0,0,0},{0,1,1,0,0,1,1,0},{0,0,0,0,0,0,0,0},{0,0,0,1,1,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,1,1,0,0,0}},
    {{0,0,0,1,1,0,0,0},{0,0,0,1,1,0,0,0},{0,0,0,1,1,0,0,0},{0,0,0,1,1,0,0,0},{0,0,0,1,1,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,1,1,0,0,0},{0,0,0,1,1,0,0,0}},
    {{0,0,1,1,1,1,0,0},{0,1,1,0,0,1,1,0},{0,0,0,0,0,1,1,0},{0,0,0,0,1,1,0,0},{0,0,0,1,1,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,1,1,0,0,0},{0,0,0,1,1,0,0,0}},
    {{0,0,0,1,1,0,0,0},{0,0,0,1,1,0,0,0},{0,1,1,1,1,1,1,0},{0,0,1,1,1,1,0,0},{0,1,1,1,1,1,1,0},{1,1,0,1,1,0,1,1},{1,0,0,0,0,0,0,1},{0,0,0,0,0,0,0,0}},
    {{0,0,0,1,1,0,0,0},{0,0,1,1,1,1,0,0},{0,1,1,1,1,1,1,0},{1,1,0,1,1,0,1,1},{0,0,0,1,1,0,0,0},{0,0,0,1,1,0,0,0},{0,0,0,1,1,0,0,0},{0,0,0,1,1,0,0,0}},
    {{0,0,0,1,1,0,0,0},{0,0,0,1,1,0,0,0},{0,0,0,1,1,0,0,0},{0,0,0,1,1,0,0,0},{1,1,0,1,1,0,1,1},{0,1,1,1,1,1,1,0},{0,0,1,1,1,1,0,0},{0,0,0,1,1,0,0,0}},
    {{0,0,0,0,0,1,1,1},{0,0,0,0,0,0,1,0},{0,0,0,0,0,1,0,0},{0,1,1,1,0,1,1,1},{0,1,1,1,0,0,0,0},{0,0,0,0,0,0,0,0},{1,1,1,1,0,0,0,0},{1,1,1,1,0,0,0,0}},
    {{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{1,1,1,0,0,1,1,1},{0,0,1,0,0,0,0,1},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,1,0,1,0,0},{0,0,1,0,1,0,1,0}},
};

const uint8_t kMusicNote[8][8] = {
    {0,0,0,0,1,1,1,0},{0,0,0,0,1,0,1,1},{0,0,0,0,1,0,0,0},{0,0,0,0,1,0,0,0},
    {0,0,0,0,1,0,0,0},{0,1,1,0,1,0,0,0},{1,1,1,1,0,0,0,0},{0,1,1,0,0,0,0,0},
};

const uint8_t kCirclePattern[8][8] = {
    {0,0,1,1,1,1,0,0},{0,1,0,0,0,0,1,0},{1,0,0,0,0,0,0,1},{1,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,1},{1,0,0,0,0,0,0,1},{0,1,0,0,0,0,1,0},{0,0,1,1,1,1,0,0},
};

const uint8_t kThinkingPath[20][2] = {
    {0,2},{0,3},{0,4},{0,5},{1,6},{2,7},{3,7},{4,7},{5,7},{6,6},
    {7,5},{7,4},{7,3},{7,2},{6,1},{5,0},{4,0},{3,0},{2,0},{1,1},
};

const uint8_t kHeartFull[8][8] = {
    {0,1,1,0,0,1,1,0},{1,1,1,1,1,1,1,1},{1,1,1,1,1,1,1,1},{1,1,1,1,1,1,1,1},
    {0,1,1,1,1,1,1,0},{0,0,1,1,1,1,0,0},{0,0,0,1,1,0,0,0},{0,0,0,0,0,0,0,0},
};

const uint8_t kHeartCracked[8][8] = {
    {0,1,1,0,0,1,1,0},{1,1,1,1,0,0,1,1},{1,1,1,0,0,1,1,1},{1,1,1,1,0,0,1,1},
    {0,1,1,0,0,1,1,0},{0,0,1,1,0,1,0,0},{0,0,0,1,0,0,0,0},{0,0,0,0,0,0,0,0},
};

const uint8_t kSurprisedFrames[3][8][8] = {
    {{0,0,0,0,0,0,0,0},{0,1,1,0,0,1,1,0},{0,1,1,0,0,1,1,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,1,1,0,0,0},{0,0,0,1,1,0,0,0},{0,0,0,0,0,0,0,0}},
    {{0,0,0,0,0,0,0,0},{0,1,1,0,0,1,1,0},{0,0,0,0,0,0,0,0},{0,1,1,0,0,1,1,0},{0,0,0,0,0,0,0,0},{0,0,0,1,1,0,0,0},{0,0,0,1,1,0,0,0},{0,0,0,0,0,0,0,0}},
    {{0,0,0,0,0,0,0,0},{0,1,1,0,0,1,1,0},{0,0,0,0,0,0,0,0},{0,1,1,0,0,1,1,0},{0,0,0,0,0,0,0,0},{0,0,0,1,1,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,1,1,0,0,0}},
};

struct ModeAlias {
  const char *name;
  int mode;
};

const ModeAlias kAliases[] = {
    {"off", 0}, {"black", 0}, {"red", 1}, {"green", 2}, {"blue", 3}, {"white", 4},
    {"theater", 5}, {"theaterred", 6}, {"redtheater", 6}, {"theatergreen", 7},
    {"greentheater", 7}, {"theaterblue", 8}, {"bluetheater", 8}, {"rainbow", 9},
    {"smiley", 10}, {"smile", 10}, {"happy", 10}, {"crying", 11}, {"cry", 11},
    {"sad", 11}, {"left", 12}, {"right", 13}, {"check", 14}, {"yes", 14},
    {"ok", 14}, {"cross", 15}, {"no", 15}, {"x", 15}, {"music", 16}, {"note", 16},
    {"blush", 17}, {"angry", 18}, {"anger", 18}, {"surprised", 19}, {"surprise", 19},
    {"exclaim", 20}, {"exclamation", 20}, {"question", 21}, {"star", 22},
    {"up", 23}, {"down", 24}, {"sleep", 25}, {"thinking", 26}, {"think", 26},
    {"heart", 27}, {"love", 27}, {"heartbreak", 28}, {"broken", 28}, {"helpless", 29},
};

void normalizeKey(const char *input, char *out, size_t outLen) {
  if (!out || outLen == 0) return;
  out[0] = 0;
  if (!input) return;

  size_t n = 0;
  for (const char *p = input; *p && n + 1 < outLen; ++p) {
    unsigned char c = (unsigned char)*p;
    if (isalnum(c)) {
      out[n++] = (char)tolower(c);
    }
  }
  out[n] = 0;
}

int resolveNormalizedKey(const char *key) {
  if (!key || !key[0]) return -1;

  char *end = nullptr;
  long numeric = strtol(key, &end, 10);
  if (end && *end == 0 && numeric >= 0 && numeric <= 29) {
    return (int)numeric;
  }

  for (const auto &alias : kAliases) {
    if (strcmp(key, alias.name) == 0) {
      return alias.mode;
    }
  }
  return -1;
}

uint32_t color(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

uint8_t scaledBrightness(uint8_t level) {
  return (uint8_t)((g_brightness * (uint16_t)level) / 255);
}

void setMirrorPixel(int idx, uint32_t pixelColor) {
  if (idx < 0 || idx >= LAMPGO_LED_PANEL_PIXELS) return;
  for (int panel = 0; panel < LAMPGO_LED_PANEL_COUNT; ++panel) {
    int pixel = panel * LAMPGO_LED_PANEL_PIXELS + idx;
    int offset = pixel * 3;
    g_pixels[offset + 0] = (uint8_t)((pixelColor >> 8) & 0xFF);   // GRB
    g_pixels[offset + 1] = (uint8_t)((pixelColor >> 16) & 0xFF);
    g_pixels[offset + 2] = (uint8_t)(pixelColor & 0xFF);
  }
}

void clearPixels() {
  memset(g_pixels, 0, sizeof(g_pixels));
}

uint32_t hsvColor(uint16_t hue, uint8_t sat, uint8_t val) {
  uint8_t region = hue / 10923;
  uint16_t remainder = (hue - (region * 10923)) * 6;
  uint8_t p = (uint8_t)(((uint16_t)val * (255 - sat)) / 255);
  uint8_t q = (uint8_t)(((uint16_t)val * (255 - (((uint16_t)sat * remainder) / 65535))) / 255);
  uint8_t t = (uint8_t)(((uint16_t)val * (255 - (((uint16_t)sat * (65535 - remainder)) / 65535))) / 255);

  switch (region) {
    case 0: return color(val, t, p);
    case 1: return color(q, val, p);
    case 2: return color(p, val, t);
    case 3: return color(p, q, val);
    case 4: return color(t, p, val);
    default: return color(val, p, q);
  }
}

bool ensureRmtLocked() {
  if (!g_rmtData) {
    size_t symbols = LAMPGO_LED_PIXEL_COUNT * 24;
    g_rmtData = (rmt_data_t *)heap_caps_malloc(symbols * sizeof(rmt_data_t),
                                               MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!g_rmtData) {
      g_rmtData = (rmt_data_t *)heap_caps_malloc(symbols * sizeof(rmt_data_t), MALLOC_CAP_8BIT);
    }
    if (!g_rmtData) {
      Serial.printf("[led_matrix] rmt buffer allocation failed: symbols=%u bytes=%u\n",
                    (unsigned)symbols,
                    (unsigned)(symbols * sizeof(rmt_data_t)));
      return false;
    }
  }

  if (!g_rmtReady) {
    g_rmtReady = rmtInit(LAMPGO_LED_PIXEL_PIN, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000);
    if (!g_rmtReady) {
      Serial.printf("[led_matrix] rmtInit failed on pin=%d\n", (int)LAMPGO_LED_PIXEL_PIN);
      return false;
    }
  }
  return true;
}

bool showPixelsLocked() {
  if (!ensureRmtLocked()) {
    g_lastShowOk = false;
    return false;
  }

  size_t symbol = 0;
  for (size_t byteIndex = 0; byteIndex < sizeof(g_pixels); ++byteIndex) {
    uint8_t value = g_pixels[byteIndex];
    for (int bit = 7; bit >= 0; --bit) {
      bool one = (value & (1 << bit)) != 0;
      g_rmtData[symbol].level0 = 1;
      g_rmtData[symbol].duration0 = one ? 8 : 4;
      g_rmtData[symbol].level1 = 0;
      g_rmtData[symbol].duration1 = one ? 4 : 8;
      symbol++;
    }
  }

  g_lastShowOk = rmtWrite(LAMPGO_LED_PIXEL_PIN, g_rmtData, symbol, 50);
  if (!g_lastShowOk) {
    Serial.printf("[led_matrix] rmtWrite timeout pin=%d symbols=%u\n",
                  (int)LAMPGO_LED_PIXEL_PIN,
                  (unsigned)symbol);
    return false;
  }
  g_lastWriteMs = millis();
  return true;
}

void drawBitmap(const uint8_t bitmap[8][8], uint32_t pixelColor, int rowOffset = 0) {
  for (int row = 0; row < 8; ++row) {
    int shiftedRow = row + rowOffset;
    if (shiftedRow < 0 || shiftedRow >= 8) continue;
    for (int col = 0; col < 8; ++col) {
      if (bitmap[row][col] == 1) {
        setMirrorPixel(shiftedRow * 8 + col, pixelColor);
      }
    }
  }
}

uint32_t patternColor(int patternIndex) {
  switch (patternIndex) {
    case 0:
    case 1:
    case 8:
    case 11:
    case 15:
      return color(g_brightness, g_brightness, 0);
    case 2:
    case 3:
    case 4:
    case 12:
    case 13:
      return color(0, g_brightness, 0);
    case 5:
    case 6:
    case 7:
    case 10:
      return color(g_brightness, 0, 0);
    case 9:
      return color(g_brightness, (uint8_t)(g_brightness * 7 / 10), 0);
    case 14:
      return color(g_brightness, g_brightness, g_brightness);
    default:
      return color(g_brightness, g_brightness, g_brightness);
  }
}

void renderPattern(int patternIndex) {
  clearPixels();
  if (patternIndex >= 0 && patternIndex < 16) {
    drawBitmap(kPatterns[patternIndex], patternColor(patternIndex));
  }
}

void renderFill(uint32_t pixelColor) {
  clearPixels();
  for (int i = 0; i < LAMPGO_LED_PANEL_PIXELS; ++i) {
    setMirrorPixel(i, pixelColor);
  }
}

void renderTheater(uint32_t pixelColor, uint32_t frame) {
  clearPixels();
  for (int i = frame % 3; i < LAMPGO_LED_PANEL_PIXELS; i += 3) {
    setMirrorPixel(i, pixelColor);
  }
}

void renderRainbow(uint32_t frame) {
  clearPixels();
  uint16_t firstHue = (uint16_t)((frame * 1024) & 0xFFFF);
  for (int i = 0; i < LAMPGO_LED_PANEL_PIXELS; ++i) {
    uint16_t hue = firstHue + (uint32_t)i * 65536UL / LAMPGO_LED_PANEL_PIXELS;
    setMirrorPixel(i, hsvColor(hue, 255, g_brightness));
  }
}

void renderMusic(uint32_t frame) {
  static const int8_t offsets[] = {0, -1, -2, -1, 0, 1, 0};
  uint32_t colors[] = {
      color(g_brightness, 0, g_brightness),
      color(0, g_brightness, g_brightness),
      color(g_brightness, g_brightness, 0),
      color(g_brightness, 0, 0),
  };
  clearPixels();
  drawBitmap(kMusicNote, colors[(frame / 7) % 4], offsets[frame % 7]);
}

void renderSurprised(uint32_t frame) {
  clearPixels();
  uint32_t step = frame % 12;
  int idx = step == 0 ? 0 : (step == 1 ? 1 : 2);
  drawBitmap(kSurprisedFrames[idx], color(g_brightness, g_brightness, 0));
}

void renderThinking(uint32_t frame) {
  clearPixels();
  uint8_t low = g_brightness / 10;
  drawBitmap(kCirclePattern, color(low, low, low));
  const uint8_t *pos = kThinkingPath[frame % 20];
  setMirrorPixel(pos[0] * 8 + pos[1], color(g_brightness, g_brightness, g_brightness));
}

void renderHeart(uint32_t frame) {
  static const uint8_t levels[] = {255, 128, 77, 128, 255};
  clearPixels();
  drawBitmap(kHeartFull, color(scaledBrightness(levels[frame % 5]), 0, 0));
}

void renderHeartbreak(uint32_t frame) {
  clearPixels();
  drawBitmap((frame % 2 == 0) ? kHeartFull : kHeartCracked, color(g_brightness, 0, 0));
}

bool modeIsAnimated(int mode) {
  return (mode >= 5 && mode <= 9) || mode == 16 || mode == 19 || mode == 26 || mode == 27 || mode == 28;
}

uint32_t frameIntervalMs(int mode) {
  switch (mode) {
    case 5:
    case 6:
    case 7:
    case 8:
      return 90;
    case 9:
      return 35;
    case 16:
      return 150;
    case 19:
      return 200;
    case 26:
      return 80;
    case 27:
      return 180;
    case 28:
      return 500;
    default:
      return 1000;
  }
}

void renderCurrentLocked() {
  if (!g_outputStarted) {
    g_outputStarted = true;
  }

  switch (g_mode) {
    case 0: clearPixels(); break;
    case 1: renderFill(color(g_brightness, 0, 0)); break;
    case 2: renderFill(color(0, g_brightness, 0)); break;
    case 3: renderFill(color(0, 0, g_brightness)); break;
    case 4: renderFill(color(g_brightness, g_brightness, g_brightness)); break;
    case 5: renderTheater(color(g_brightness, g_brightness, g_brightness), g_animFrame); break;
    case 6: renderTheater(color(g_brightness, 0, 0), g_animFrame); break;
    case 7: renderTheater(color(0, g_brightness, 0), g_animFrame); break;
    case 8: renderTheater(color(0, 0, g_brightness), g_animFrame); break;
    case 9: renderRainbow(g_animFrame); break;
    case 10: renderPattern(0); break;
    case 11: renderPattern(1); break;
    case 12: renderPattern(2); break;
    case 13: renderPattern(3); break;
    case 14: renderPattern(4); break;
    case 15: renderPattern(5); break;
    case 16: renderMusic(g_animFrame); break;
    case 17: renderPattern(6); break;
    case 18: renderPattern(7); break;
    case 19: renderSurprised(g_animFrame); break;
    case 20: renderPattern(9); break;
    case 21: renderPattern(10); break;
    case 22: renderPattern(11); break;
    case 23: renderPattern(12); break;
    case 24: renderPattern(13); break;
    case 25: renderPattern(14); break;
    case 26: renderThinking(g_animFrame); break;
    case 27: renderHeart(g_animFrame); break;
    case 28: renderHeartbreak(g_animFrame); break;
    case 29: renderPattern(15); break;
    default: clearPixels(); break;
  }

  showPixelsLocked();
}

void ledTask(void *) {
  while (true) {
    if (g_ready && modeIsAnimated(g_mode)) {
      uint32_t now = millis();
      if (g_lastFrameMs == 0 || now - g_lastFrameMs >= frameIntervalMs(g_mode)) {
        if (!g_mutex || xSemaphoreTake(g_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
          if (modeIsAnimated(g_mode)) {
            g_animFrame++;
            g_lastFrameMs = now;
            renderCurrentLocked();
          }
          if (g_mutex) {
            xSemaphoreGive(g_mutex);
          }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

bool takeLedLock() {
  return !g_mutex || xSemaphoreTake(g_mutex, pdMS_TO_TICKS(250)) == pdTRUE;
}

void giveLedLock() {
  if (g_mutex) {
    xSemaphoreGive(g_mutex);
  }
}

void recordCommand(const char *command) {
  if (command && command[0]) {
    snprintf(g_lastCommand, sizeof(g_lastCommand), "%s", command);
  }
}

}

namespace LedSerial {

bool begin() {
  if (!g_mutex) {
    g_mutex = xSemaphoreCreateMutex();
  }

  g_ready = true;

  if (!g_task) {
    BaseType_t taskResult = xTaskCreatePinnedToCore(
        ledTask, "led_pixels", LAMPGO_LED_TASK_STACK, nullptr, 1, &g_task, 1);
    if (taskResult != pdPASS) {
      g_task = nullptr;
      Serial.printf("[led_matrix] animation task create failed: %d\n", (int)taskResult);
    }
  }

  Serial.printf("[led_matrix] ready: driver=neopixel pin=%d pixels=%d panels=%d brightness=%d deferred=1\n",
                (int)LAMPGO_LED_PIXEL_PIN,
                (int)LAMPGO_LED_PIXEL_COUNT,
                (int)LAMPGO_LED_PANEL_COUNT,
                g_brightness);
  return true;
}

bool isReady() {
  return g_ready;
}

bool setMode(int mode) {
  if (!g_ready || mode < 0 || mode > 29 || !takeLedLock()) {
    return false;
  }

  char command[8];
  snprintf(command, sizeof(command), "m%d", mode);
  recordCommand(command);
  g_mode = mode;
  g_animFrame = 0;
  g_lastFrameMs = 0;
  renderCurrentLocked();
  bool ok = g_lastShowOk;
  giveLedLock();

  Serial.printf("[led_matrix] mode=%d name=%s pin=%d\n", mode, modeName(mode), (int)LAMPGO_LED_PIXEL_PIN);
  return ok;
}

bool setModeName(const char *name) {
  int mode = resolveMode(name);
  if (mode < 0) {
    return false;
  }
  return setMode(mode);
}

bool setBrightness(int brightness) {
  if (!g_ready || brightness < 1 || brightness > 255 || !takeLedLock()) {
    return false;
  }

  char command[8];
  snprintf(command, sizeof(command), "b%d", brightness);
  recordCommand(command);
  g_brightness = brightness;
  g_animFrame = 0;
  g_lastFrameMs = 0;
  renderCurrentLocked();
  bool ok = g_lastShowOk;
  giveLedLock();

  Serial.printf("[led_matrix] brightness=%d pin=%d\n", brightness, (int)LAMPGO_LED_PIXEL_PIN);
  return ok;
}

int resolveMode(const char *name) {
  if (!name) return -1;

  char key[32];
  normalizeKey(name, key, sizeof(key));
  if (!key[0]) return -1;

  int mode = resolveNormalizedKey(key);
  if (mode >= 0) return mode;
  if (key[0] == 'm' && key[1]) {
    return resolveNormalizedKey(key + 1);
  }
  return mode;
}

const char *modeName(int mode) {
  if (mode < 0 || mode > 29) {
    return "unknown";
  }
  return kModeNames[mode];
}

int currentMode() {
  return g_mode;
}

int currentBrightness() {
  return g_brightness;
}

uint32_t lastWriteMs() {
  return g_lastWriteMs;
}

const char *lastCommand() {
  return g_lastCommand;
}

const char *driverName() {
  return "neopixel";
}

int pixelPin() {
  return LAMPGO_LED_PIXEL_PIN;
}

int pixelCount() {
  return LAMPGO_LED_PIXEL_COUNT;
}

int panelCount() {
  return LAMPGO_LED_PANEL_COUNT;
}

bool outputOk() {
  return g_lastShowOk || g_lastWriteMs == 0;
}

int txPin() {
  return -1;
}

int rxPin() {
  return -1;
}

uint32_t baudRate() {
  return 0;
}

}
