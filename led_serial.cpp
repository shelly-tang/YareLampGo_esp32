// SPDX-FileCopyrightText: 2026 @ninsmiracle, @shelly-tang, and LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "led_serial.h"
#include "display_link.h"
#include "led_clip_player.h"

#include <Arduino.h>
#include <ctype.h>
#include <esp_heap_caps.h>
#include <esp32-hal-rmt.h>
#include <esp_system.h>
#include <math.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#ifndef LAMPGO_LED_PIXEL_PIN
#define LAMPGO_LED_PIXEL_PIN D2
#endif

#ifndef LAMPGO_LED_PANEL_PIXELS
#define LAMPGO_LED_PANEL_PIXELS 447
#endif

#ifndef LAMPGO_LED_PANEL_COUNT
#define LAMPGO_LED_PANEL_COUNT 1
#endif

#define LAMPGO_LED_PIXEL_COUNT (LAMPGO_LED_PANEL_PIXELS * LAMPGO_LED_PANEL_COUNT)

#ifndef LAMPGO_LED_MAX_CHANNEL_SUM
#define LAMPGO_LED_MAX_CHANNEL_SUM (LAMPGO_LED_PIXEL_COUNT * 64UL)
#endif

#ifndef LAMPGO_LED_TASK_STACK
#define LAMPGO_LED_TASK_STACK 4096
#endif

namespace {

constexpr int kPanelRows = 9;
constexpr int kCombinedCols = 51;
constexpr int kMaxMode = 33;
constexpr int kPatternRows = 8;
constexpr int kPatternCols = 16;
constexpr int kIrregularRowCount = 9;
constexpr int kIrregularMaxCols = 51;

const int kIrregularRowLengths[kIrregularRowCount] = {47, 49, 51, 51, 51, 51, 51, 49, 47};
const int kIrregularRowStarts[kIrregularRowCount] = {0, 47, 96, 147, 198, 249, 300, 351, 400};

SemaphoreHandle_t g_mutex = nullptr;
TaskHandle_t g_task = nullptr;
bool g_ready = false;
bool g_outputStarted = false;
bool g_rmtReady = false;
bool g_lastShowOk = false;
bool g_useEllipseMask = false;
int g_mode = 0;
int g_brightness = 64;
uint32_t g_lastWriteMs = 0;
uint32_t g_animFrame = 0;
uint32_t g_lastFrameMs = 0;
uint32_t g_modeStartedMs = 0;
char g_lastCommand[32] = "";
uint8_t g_pixels[LAMPGO_LED_PIXEL_COUNT * 3] = {0};
rmt_data_t *g_rmtData = nullptr;

bool g_clipActive = false;
bool g_storedClipActive = false;
char g_clipId[40] = "";
uint16_t g_clipFrameCount = 0;
uint16_t g_clipFps = 0;
uint32_t g_clipFrame = 0;
uint32_t g_clipLastFrameMs = 0;
char g_effectTemplate[16] = "";
char g_effectVariant[16] = "";
char g_effectDirection[8] = "right";
uint8_t g_effectRed = 255;
uint8_t g_effectGreen = 255;
uint8_t g_effectBlue = 255;
uint8_t g_effectSecondaryRed = 255;
uint8_t g_effectSecondaryGreen = 45;
uint8_t g_effectSecondaryBlue = 125;
uint8_t g_effectIntensityPercent = 100;
uint8_t g_storedPackedFrame[LedClipPlayer::kFrameBytes] = {};
bool g_expressionLoop = true;
uint32_t g_expressionEndMs = 0;

bool g_clockActive = false;
uint8_t g_clockHour = 0;
uint8_t g_clockMinute = 0;
uint8_t g_clockRed = 55;
uint8_t g_clockGreen = 214;
uint8_t g_clockBlue = 255;
char g_clockEffect[8] = "steady";
uint32_t g_clockLastFrameMs = 0;

constexpr uint32_t kOceanFrameIntervalMs = 50;
constexpr uint32_t kOceanInputTimeoutMs = 1000;
volatile bool g_oceanActive = false;
uint8_t g_oceanRed = 0;
uint8_t g_oceanGreen = 184;
uint8_t g_oceanBlue = 224;
uint8_t g_oceanFillPercent = 55;
uint8_t g_oceanSensitivityPercent = 100;
uint8_t g_oceanEdgeHighlightPercent = 75;
uint8_t g_oceanTiltPercent = 100;
uint8_t g_oceanImpactPercent = 100;
uint8_t g_oceanDampingPercent = 130;
uint32_t g_oceanLastFrameMs = 0;
float g_oceanHeight[kCombinedCols] = {};
float g_oceanVelocity[kCombinedCols] = {};
float g_oceanNextHeight[kCombinedCols] = {};
float g_oceanFilteredAngle = 0.0f;
float g_oceanBulkTilt = 0.0f;
float g_oceanBulkVelocity = 0.0f;
float g_oceanPreviousInputVelocity = 0.0f;
float g_oceanLeftImpactEnergy = 0.0f;
float g_oceanRightImpactEnergy = 0.0f;
bool g_oceanLeftContact = false;
bool g_oceanRightContact = false;
portMUX_TYPE g_oceanInputMux = portMUX_INITIALIZER_UNLOCKED;
volatile float g_oceanInputAngleDeg = 0.0f;
volatile float g_oceanInputAngularVelocityDps = 0.0f;
volatile uint32_t g_oceanInputSequence = 0;
volatile uint32_t g_oceanInputAtMs = 0;

bool g_focusEyesOpen = true;
uint8_t g_focusBlinksRemaining = 0;
uint32_t g_focusNextTransitionMs = 0;

const char *const kModeNames[kMaxMode + 1] = {
    "off",
    "red",
    "green",
    "blue",
    "white",
    "theater",
    "theaterred",
    "theatergreen",
    "theaterblue",
    "rainbow",
    "rainbowchase",
    "left",
    "right",
    "up",
    "down",
    "check",
    "cross",
    "exclaim",
    "question",
    "star",
    "music",
    "smiley",
    "sad",
    "heart",
    "surprised",
    "blush",
    "angry",
    "thinking",
    "sleep",
    "helpless",
    "cool",
    "focused",
    "wink",
    "myu7gt",
};

const uint8_t kCheckPattern[8][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1},
    {0, 0, 0, 0, 0, 0, 1, 1},
    {0, 0, 0, 0, 0, 1, 1, 0},
    {1, 0, 0, 0, 1, 1, 0, 0},
    {1, 1, 0, 1, 1, 0, 0, 0},
    {0, 1, 1, 1, 0, 0, 0, 0},
    {0, 0, 1, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0},
};

const uint8_t kCrossPattern[8][8] = {
    {1, 0, 0, 0, 0, 0, 0, 1},
    {1, 1, 0, 0, 0, 0, 1, 1},
    {0, 1, 1, 0, 0, 1, 1, 0},
    {0, 0, 1, 1, 1, 1, 0, 0},
    {0, 0, 1, 1, 1, 1, 0, 0},
    {0, 1, 1, 0, 0, 1, 1, 0},
    {1, 1, 0, 0, 0, 0, 1, 1},
    {1, 0, 0, 0, 0, 0, 0, 1},
};

const uint8_t kExclaimPattern[8][8] = {
    {0, 0, 0, 1, 1, 0, 0, 0},
    {0, 0, 0, 1, 1, 0, 0, 0},
    {0, 0, 0, 1, 1, 0, 0, 0},
    {0, 0, 0, 1, 1, 0, 0, 0},
    {0, 0, 0, 1, 1, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 1, 1, 0, 0, 0},
    {0, 0, 0, 1, 1, 0, 0, 0},
};

const uint8_t kQuestionPattern[8][8] = {
    {0, 0, 1, 1, 1, 1, 0, 0},
    {0, 1, 1, 0, 0, 1, 1, 0},
    {0, 0, 0, 0, 0, 1, 1, 0},
    {0, 0, 0, 0, 1, 1, 0, 0},
    {0, 0, 0, 1, 1, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 1, 1, 0, 0, 0},
    {0, 0, 0, 1, 1, 0, 0, 0},
};

const uint8_t kStarPattern[8][8] = {
    {0, 0, 0, 0, 1, 0, 0, 0},
    {0, 0, 0, 0, 1, 0, 0, 0},
    {0, 0, 0, 1, 1, 0, 0, 0},
    {1, 1, 1, 1, 1, 1, 1, 0},
    {0, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 1, 1, 0, 0, 0},
    {0, 0, 0, 1, 0, 0, 0, 0},
    {0, 0, 0, 1, 0, 0, 0, 0},
};

const uint8_t kMusicNote[8][8] = {
    {0, 0, 0, 0, 1, 1, 1, 0},
    {0, 0, 0, 0, 1, 0, 1, 1},
    {0, 0, 0, 0, 1, 0, 0, 0},
    {0, 0, 0, 0, 1, 0, 0, 0},
    {0, 0, 0, 0, 1, 0, 0, 0},
    {0, 1, 1, 0, 1, 0, 0, 0},
    {1, 1, 1, 1, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 0, 0, 0},
};

const uint8_t kCirclePattern[8][8] = {
    {0, 0, 1, 1, 1, 1, 0, 0},
    {0, 1, 0, 0, 0, 0, 1, 0},
    {1, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 1},
    {0, 1, 0, 0, 0, 0, 1, 0},
    {0, 0, 1, 1, 1, 1, 0, 0},
};

const uint8_t kHeartPattern[8][8] = {
    {0, 1, 1, 0, 0, 1, 1, 0},
    {1, 1, 1, 1, 1, 1, 1, 1},
    {1, 1, 1, 1, 1, 1, 1, 1},
    {1, 1, 1, 1, 1, 1, 1, 1},
    {0, 1, 1, 1, 1, 1, 1, 0},
    {0, 0, 1, 1, 1, 1, 0, 0},
    {0, 0, 0, 1, 1, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0},
};

// Compact 5x7 glyphs spell CODEX across the 51x9 irregular LED matrix.
// Each row is stored as a five-bit bitmap, with the high bit on the left.
const uint8_t kCodexGlyphRows[5][7] = {
    {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110},  // C
    {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110},  // O
    {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110},  // D
    {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111},  // E
    {0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b01010, 0b10001},  // X
};

const uint8_t kCombinedPatterns[][8][16] = {
    {
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,1,1,0,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,1,1,0,0,1,1,0, 0,1,1,0,0,1,1,0},
        {0,1,0,0,0,0,1,0, 0,1,0,0,0,0,1,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
    },
    {
        {1,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,1},
        {0,1,0,0,0,0,0,0, 0,0,0,0,0,0,1,0},
        {0,0,1,0,0,0,0,0, 0,0,0,0,0,1,0,0},
        {0,0,0,1,0,0,0,0, 0,0,0,0,1,0,0,0},
        {0,0,1,1,1,0,0,0, 0,0,0,1,1,1,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,1,1,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
    },
    {
        {0,0,0,0,0,0,0,0, 0,0,0,0,1,0,0,0},
        {0,1,0,1,0,1,0,0, 0,0,0,1,0,0,0,0},
        {0,1,1,1,1,1,0,0, 1,0,1,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,1,0,0,0,0,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,1,1,0,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,1,1,0,0,0},
    },
    {
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,1,1,0,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,1,1,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {2,2,2,0,0,0,0,0, 0,0,0,0,0,2,2,2},
        {2,2,2,0,0,0,0,0, 0,0,0,0,0,2,2,2},
    },
    {
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
    },
    {
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,2,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,2,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,2,2,2},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,2,2,2},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,2,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
    },
    {
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,1,1,0,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,1,1,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
    },
    {
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
    },
    {
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,1,1,0,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,1,1,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
    },
    {
        {0,0,0,0,0,0,0,0, 0,0,0,0,1,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,1,0,0,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,1,0,0,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,0,0,0,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,0,0,0,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,1,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,1,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
    },
    {
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,1,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,1,1,1,1,1,1, 1,1,1,1,1,0,0,0},
        {0,0,1,1,1,1,1,1, 1,1,1,1,1,0,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,1,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
    },
    {
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,1,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,1,1,0,0,0},
        {0,0,0,1,1,1,1,1, 1,1,1,1,1,1,0,0},
        {0,0,0,1,1,1,1,1, 1,1,1,1,1,1,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,1,1,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,1,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
    },
    {
        {0,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,1,1, 1,1,0,0,0,0,0,0},
        {0,0,0,0,0,1,1,1, 1,1,1,0,0,0,0,0},
        {0,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
    },
    {
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,0},
        {0,0,0,0,0,1,1,1, 1,1,1,0,0,0,0,0},
        {0,0,0,0,0,0,1,1, 1,1,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,0},
    },
    {
        {0,0,0,0,0,1,1,0, 1,1,0,0,0,0,0,0},
        {0,0,0,0,1,1,1,1, 1,1,1,1,0,0,0,0},
        {0,0,0,1,1,1,1,1, 1,1,1,1,1,0,0,0},
        {0,0,0,1,1,1,1,1, 1,1,1,1,1,0,0,0},
        {0,0,0,0,1,1,1,1, 1,1,1,1,0,0,0,0},
        {0,0,0,0,0,1,1,1, 1,1,1,0,0,0,0,0},
        {0,0,0,0,0,0,1,1, 1,1,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,0},
    },
    {
        {0,0,0,0,0,1,1,0, 1,1,0,0,0,0,0,0},
        {0,0,0,0,1,1,1,0, 0,1,1,1,0,0,0,0},
        {0,0,0,1,1,1,1,0, 0,1,1,1,1,0,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,1,1,0,0,0},
        {0,0,0,0,1,1,1,0, 0,1,1,1,0,0,0,0},
        {0,0,0,0,0,1,1,0, 0,1,1,0,0,0,0,0},
        {0,0,0,0,0,0,1,1, 1,1,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,0},
    },
    {
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,1,0,0, 0,0,1,0,0,0,0,0},
        {0,0,0,0,1,1,0,0, 0,0,1,1,0,0,0,0},
        {0,0,0,1,1,1,0,0, 0,0,1,1,1,0,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,1,1,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
    },
    {
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,1,1,1,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,1,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,1,1,1,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,1,1,1,1,0,0,0, 1,1,1,1,0,0,0,0},
        {0,1,1,1,1,0,0,0, 1,1,1,1,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
    },
    {
        {0,0,0,0,0,0,0,0, 0,0,0,0,1,1,1,1},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,1,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,1,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,1,1,1,1},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,1,1,1,1,0,0,0, 1,1,1,1,0,0,0,0},
        {0,1,1,1,1,0,0,0, 1,1,1,1,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
    },
    {
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,1,1,0,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,1,1,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
    },
    {
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,1,1,0,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,1,1,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
    },
    {
        {0,0,0,1,1,0,0,0, 0,0,0,1,1,0,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,1,1,1,1,0,0, 0,0,1,1,1,1,0,0},
        {0,0,0,1,1,0,0,0, 0,0,0,1,1,0,0,0},
    },
    {
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
        {1,0,1,0,0,1,1,1, 0,2,2,2,2,0,0,0},
        {1,0,1,0,0,0,0,1, 0,2,0,0,0,2,2,2},
        {1,1,1,0,0,0,0,1, 0,2,0,2,2,0,2,0},
        {0,1,0,1,0,1,0,1, 0,2,0,0,2,0,2,0},
        {0,1,0,1,0,1,0,1, 0,2,2,2,2,0,2,0},
        {0,1,0,1,1,1,0,1, 0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
    },
};

struct ModeAlias {
  const char *name;
  int mode;
};

const ModeAlias kAliases[] = {
    {"off", 0},
    {"red", 1}, {"green", 2}, {"blue", 3}, {"white", 4},
    {"theater", 5},
    {"theaterred", 6},
    {"theatergreen", 7},
    {"theaterblue", 8},
    {"rainbow", 9},
    {"rainbowchase", 10},
    {"left", 11}, {"right", 12}, {"up", 13}, {"down", 14},
    {"check", 15},
    {"cross", 16},
    {"exclaim", 17},
    {"question", 18},
    {"star", 19},
    {"music", 20},
    {"smiley", 21},
    {"sad", 22},
    {"heart", 23},
    {"surprised", 24},
    {"blush", 25},
    {"angry", 26},
    {"thinking", 27},
    {"sleep", 28},
    {"helpless", 29},
    {"cool", 30},
    {"focused", 31},
    {"wink", 32},
    {"myu7gt", 33},
    {"myu7", 33},
    {"mgt", 33},
    {"yu7gt", 33},
    {"yu7", 33},
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
  if (end && *end == 0 && numeric >= 0 && numeric <= kMaxMode) {
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

uint8_t effectChannel(uint8_t channel) {
  uint32_t scaled = (uint32_t)channel * (uint32_t)g_brightness * (uint32_t)g_effectIntensityPercent;
  return (uint8_t)(scaled / (255UL * 100UL));
}

uint32_t effectColor(bool secondary = false) {
  return color(
      effectChannel(secondary ? g_effectSecondaryRed : g_effectRed),
      effectChannel(secondary ? g_effectSecondaryGreen : g_effectGreen),
      effectChannel(secondary ? g_effectSecondaryBlue : g_effectBlue));
}

void setPhysicalPixel(int pixelIndex, uint32_t pixelColor) {
  if (pixelIndex < 0 || pixelIndex >= LAMPGO_LED_PIXEL_COUNT) return;
  int offset = pixelIndex * 3;
  g_pixels[offset + 0] = (uint8_t)((pixelColor >> 8) & 0xFF);
  g_pixels[offset + 1] = (uint8_t)((pixelColor >> 16) & 0xFF);
  g_pixels[offset + 2] = (uint8_t)(pixelColor & 0xFF);
}

int irregularPixelIndex(int row, int col) {
  if (row < 0 || row >= kIrregularRowCount || col < 0 || col >= kIrregularMaxCols) {
    return -1;
  }
  int rowLength = kIrregularRowLengths[row];
  int leftPad = (kIrregularMaxCols - rowLength) / 2;
  int localCol = col - leftPad;
  if (localCol < 0 || localCol >= rowLength) return -1;
  return kIrregularRowStarts[row] + localCol;
}

void setMatrixPixel(int row, int col, uint32_t pixelColor) {
  int pixelIndex = irregularPixelIndex(row, col);
  if (pixelIndex >= 0) {
    setPhysicalPixel(pixelIndex, pixelColor);
  }
}

void setCombinedPixel(int row, int col, uint32_t pixelColor) {
  if (row < 0 || row >= kPanelRows || col < 0 || col >= kCombinedCols) return;
  setMatrixPixel(kPanelRows - 1 - row, kCombinedCols - 1 - col, pixelColor);
}

void fillLogicalCell(int row, int col, uint32_t pixelColor) {
  int row0 = row * kPanelRows / kPatternRows;
  int row1 = ((row + 1) * kPanelRows + kPatternRows - 1) / kPatternRows;
  int col0 = col * kCombinedCols / kPatternCols;
  int col1 = ((col + 1) * kCombinedCols + kPatternCols - 1) / kPatternCols;
  if (row1 <= row0) row1 = row0 + 1;
  if (col1 <= col0) col1 = col0 + 1;
  for (int targetRow = row0; targetRow < row1; ++targetRow) {
    for (int targetCol = col0; targetCol < col1; ++targetCol) {
      setCombinedPixel(targetRow, targetCol, pixelColor);
    }
  }
}

void drawBitmap8OnMatrix(const uint8_t bitmap[8][8], uint32_t pixelColor, int colOffset, int rowOffset = 0) {
  for (int row = 0; row < 8; ++row) {
    int shiftedRow = row + rowOffset;
    if (shiftedRow < 0 || shiftedRow >= 8) continue;
    for (int col = 0; col < 8; ++col) {
      if (bitmap[row][col] == 1) {
        fillLogicalCell(shiftedRow, col + colOffset, pixelColor);
      }
    }
  }
}

void drawBitmap8PairCell(const uint8_t bitmap[8][8], uint32_t pixelColor, int cellOffset, int rowOffset = 0) {
  for (int row = 0; row < 8; ++row) {
    int shiftedRow = row + rowOffset;
    if (shiftedRow < 0 || shiftedRow >= 8) continue;
    for (int col = 0; col < 8; ++col) {
      if (bitmap[row][col] == 1) {
        int mappedCol = (col * 6) / 8;
        fillLogicalCell(shiftedRow, mappedCol + cellOffset, pixelColor);
      }
    }
  }
}

void clearPixels() {
  memset(g_pixels, 0, sizeof(g_pixels));
}

void releaseClipLocked() {
  LedClipPlayer::close();
  g_clipActive = false;
  g_storedClipActive = false;
  g_clipId[0] = 0;
  g_clipFrameCount = 0;
  g_clipFps = 0;
  g_clipFrame = 0;
  g_clipLastFrameMs = 0;
  g_effectTemplate[0] = 0;
  g_effectVariant[0] = 0;
  snprintf(g_effectDirection, sizeof(g_effectDirection), "right");
  g_expressionLoop = true;
  g_expressionEndMs = 0;
  g_clockActive = false;
  g_oceanActive = false;
  g_oceanLastFrameMs = 0;
}

bool showPixelsLocked();
void renderRingFill(uint32_t pixelColor, uint32_t frame);
void drawBitmap8Pair(const uint8_t bitmap[8][8], uint32_t pixelColor, int rowOffset = 0);
void drawCombinedPattern(int patternIndex, uint32_t primaryColor, uint32_t secondaryColor = 0);

constexpr uint8_t kClockDigits[10][7] = {
    {0b111, 0b101, 0b101, 0b101, 0b101, 0b101, 0b111},
    {0b010, 0b110, 0b010, 0b010, 0b010, 0b010, 0b111},
    {0b111, 0b001, 0b001, 0b111, 0b100, 0b100, 0b111},
    {0b111, 0b001, 0b001, 0b111, 0b001, 0b001, 0b111},
    {0b101, 0b101, 0b101, 0b111, 0b001, 0b001, 0b001},
    {0b111, 0b100, 0b100, 0b111, 0b001, 0b001, 0b111},
    {0b111, 0b100, 0b100, 0b111, 0b101, 0b101, 0b111},
    {0b111, 0b001, 0b001, 0b010, 0b010, 0b010, 0b010},
    {0b111, 0b101, 0b101, 0b111, 0b101, 0b101, 0b111},
    {0b111, 0b101, 0b101, 0b111, 0b001, 0b001, 0b111},
};

uint32_t clockColor(uint8_t percent = 100) {
  uint32_t scale = (uint32_t)g_brightness * percent;
  return color(
      (uint8_t)((g_clockRed * scale) / 25500UL),
      (uint8_t)((g_clockGreen * scale) / 25500UL),
      (uint8_t)((g_clockBlue * scale) / 25500UL));
}

void drawClockDigit(int digit, int startCol, uint32_t pixelColor) {
  if (digit < 0 || digit > 9) return;
  for (int row = 0; row < 7; ++row) {
    for (int col = 0; col < 3; ++col) {
      if ((kClockDigits[digit][row] & (1 << (2 - col))) == 0) continue;
      setCombinedPixel(row + 1, startCol + col * 2, pixelColor);
      setCombinedPixel(row + 1, startCol + col * 2 + 1, pixelColor);
    }
  }
}

void renderClockOrbit(uint32_t now) {
  constexpr int kPerimeter = 2 * kCombinedCols + 2 * (kPanelRows - 2);
  int position = (int)((now / 45UL) % kPerimeter);
  int row = 0;
  int col = 0;
  if (position < kCombinedCols) {
    col = position;
  } else if ((position -= kCombinedCols) < kPanelRows - 1) {
    col = kCombinedCols - 1;
    row = position;
  } else if ((position -= kPanelRows - 1) < kCombinedCols) {
    col = kCombinedCols - 1 - position;
    row = kPanelRows - 1;
  } else {
    position -= kCombinedCols;
    col = 0;
    row = kPanelRows - 1 - position;
  }
  setCombinedPixel(row, col, clockColor());
  setCombinedPixel(row == 0 ? 1 : row == kPanelRows - 1 ? kPanelRows - 2 : row, col, clockColor(35));
}

void renderClockLocked(uint32_t now) {
  clearPixels();
  bool visible = strcmp(g_clockEffect, "blink") != 0 || ((now / 500UL) % 2U) == 0;
  if (visible) {
    uint32_t primary = clockColor();
    drawClockDigit(g_clockHour / 10, 7, primary);
    drawClockDigit(g_clockHour % 10, 15, primary);
    setCombinedPixel(3, 23, primary);
    setCombinedPixel(6, 23, primary);
    drawClockDigit(g_clockMinute / 10, 27, primary);
    drawClockDigit(g_clockMinute % 10, 35, primary);
  }
  if (strcmp(g_clockEffect, "orbit") == 0) renderClockOrbit(now);
  showPixelsLocked();
}

uint8_t oceanChannel(uint8_t channel, float scale) {
  float value = channel * ((float)g_brightness / 255.0f) * scale;
  if (value < 0.0f) value = 0.0f;
  if (value > 255.0f) value = 255.0f;
  return (uint8_t)(value + 0.5f);
}

void constrainOceanPowerLocked() {
  uint32_t channelSum = 0;
  for (size_t offset = 0; offset < sizeof(g_pixels); ++offset) channelSum += g_pixels[offset];
  if (channelSum <= LAMPGO_LED_MAX_CHANNEL_SUM) return;
  for (size_t offset = 0; offset < sizeof(g_pixels); ++offset) {
    g_pixels[offset] = (uint8_t)(((uint32_t)g_pixels[offset] * LAMPGO_LED_MAX_CHANNEL_SUM) / channelSum);
  }
}

void stepOceanLocked(uint32_t now) {
  float inputAngle = 0.0f;
  float inputVelocity = 0.0f;
  uint32_t inputAt = 0;
  portENTER_CRITICAL(&g_oceanInputMux);
  inputAngle = g_oceanInputAngleDeg;
  inputVelocity = g_oceanInputAngularVelocityDps;
  inputAt = g_oceanInputAtMs;
  portEXIT_CRITICAL(&g_oceanInputMux);

  bool inputFresh = inputAt != 0 && now - inputAt <= kOceanInputTimeoutMs;
  if (!inputFresh) {
    inputAngle = 0.0f;
    inputVelocity = 0.0f;
  }
  inputAngle = fmaxf(-35.0f, fminf(35.0f, inputAngle));
  inputVelocity = fmaxf(-180.0f, fminf(180.0f, inputVelocity));
  g_oceanFilteredAngle += (inputAngle - g_oceanFilteredAngle) * 0.22f;

  constexpr float dt = 0.05f;
  float sensitivity = (float)g_oceanSensitivityPercent / 100.0f;
  float tiltScale = (float)g_oceanTiltPercent / 100.0f;
  float impactScale = (float)g_oceanImpactPercent / 100.0f;
  float dampingScale = (float)g_oceanDampingPercent / 100.0f;
  float baseHeight = 1.2f + 6.3f * ((float)g_oceanFillPercent / 100.0f);
  float angularAcceleration = inputFresh ? (inputVelocity - g_oceanPreviousInputVelocity) / dt : 0.0f;
  angularAcceleration = fmaxf(-900.0f, fminf(900.0f, angularAcceleration));
  g_oceanPreviousInputVelocity = inputFresh ? inputVelocity : 0.0f;

  float targetBulkTilt = sinf(g_oceanFilteredAngle * 3.14159265f / 180.0f)
      * 9.5f * sensitivity * tiltScale;
  targetBulkTilt = fmaxf(-4.25f, fminf(4.25f, targetBulkTilt));
  float bulkAcceleration = 11.0f * (targetBulkTilt - g_oceanBulkTilt)
      - 1.5f * dampingScale * g_oceanBulkVelocity - 0.018f * angularAcceleration * impactScale;
  g_oceanBulkVelocity += bulkAcceleration * dt;
  g_oceanBulkVelocity = fmaxf(-12.0f, fminf(12.0f, g_oceanBulkVelocity));
  g_oceanBulkTilt += g_oceanBulkVelocity * dt;
  if (g_oceanBulkTilt < -4.45f || g_oceanBulkTilt > 4.45f) {
    g_oceanBulkTilt = fmaxf(-4.45f, fminf(4.45f, g_oceanBulkTilt));
    g_oceanBulkVelocity *= -0.35f;
  }

  bool idle = fabsf(inputVelocity) < 2.0f && fabsf(inputAngle - g_oceanFilteredAngle) < 0.6f;
  float meanBefore = 0.0f;
  for (int col = 0; col < kCombinedCols; ++col) meanBefore += g_oceanHeight[col];
  meanBefore /= kCombinedCols;

  for (int col = 0; col < kCombinedCols; ++col) {
    int left = col > 0 ? col - 1 : 1;
    int right = col + 1 < kCombinedCols ? col + 1 : kCombinedCols - 2;
    float x = ((float)col / (kCombinedCols - 1)) * 2.0f - 1.0f;
    float equilibrium = baseHeight + g_oceanBulkTilt * x;
    float laplacian = g_oceanHeight[left] - 2.0f * g_oceanHeight[col] + g_oceanHeight[right];
    float idleForce = idle ? 0.18f * sinf(now * 0.0015f + col * 0.31f) : 0.0f;
    float acceleration = 14.0f * laplacian + 3.0f * (equilibrium - g_oceanHeight[col])
        - 1.5f * dampingScale * g_oceanVelocity[col] + idleForce;
    g_oceanVelocity[col] += acceleration * dt;
    g_oceanNextHeight[col] = g_oceanHeight[col] + g_oceanVelocity[col] * dt;
  }

  float meanAfter = 0.0f;
  for (int col = 0; col < kCombinedCols; ++col) meanAfter += g_oceanNextHeight[col];
  meanAfter /= kCombinedCols;
  float volumeCorrection = meanBefore - meanAfter;
  float leftPenetration = 0.0f;
  float rightPenetration = 0.0f;
  for (int col = 0; col < kCombinedCols; ++col) {
    float height = g_oceanNextHeight[col] + volumeCorrection;
    if (col < 6) leftPenetration = fmaxf(leftPenetration, height - 8.05f);
    if (col >= kCombinedCols - 6) rightPenetration = fmaxf(rightPenetration, height - 8.05f);
    if (height < 0.35f) {
      height = 0.35f;
      g_oceanVelocity[col] *= -0.45f;
    } else if (height > 8.65f) {
      height = 8.65f;
      g_oceanVelocity[col] *= -0.55f;
    }
    g_oceanHeight[col] = height;
  }

  bool leftContact = leftPenetration > 0.0f;
  bool rightContact = rightPenetration > 0.0f;
  if (leftContact && !g_oceanLeftContact) {
    float energy = fminf(1.0f, (leftPenetration * 1.7f + fabsf(g_oceanVelocity[1]) * 0.16f) * impactScale);
    g_oceanLeftImpactEnergy = fmaxf(g_oceanLeftImpactEnergy, energy);
    for (int col = 0; col < 12; ++col) {
      float weight = 1.0f - (float)col / 12.0f;
      if (g_oceanVelocity[col] > 0.0f) g_oceanVelocity[col] *= -0.55f;
      g_oceanVelocity[col] -= energy * 4.8f * weight;
    }
    g_oceanBulkVelocity += energy * 1.8f;
  }
  if (rightContact && !g_oceanRightContact) {
    float energy = fminf(
        1.0f, (rightPenetration * 1.7f + fabsf(g_oceanVelocity[kCombinedCols - 2]) * 0.16f) * impactScale);
    g_oceanRightImpactEnergy = fmaxf(g_oceanRightImpactEnergy, energy);
    for (int col = kCombinedCols - 12; col < kCombinedCols; ++col) {
      float weight = (float)(col - (kCombinedCols - 12)) / 12.0f;
      if (g_oceanVelocity[col] > 0.0f) g_oceanVelocity[col] *= -0.55f;
      g_oceanVelocity[col] -= energy * 4.8f * weight;
    }
    g_oceanBulkVelocity -= energy * 1.8f;
  }
  g_oceanLeftContact = leftContact;
  g_oceanRightContact = rightContact;
  g_oceanLeftImpactEnergy *= 0.82f;
  g_oceanRightImpactEnergy *= 0.82f;
}

void renderOceanLocked(uint32_t now) {
  stepOceanLocked(now);
  clearPixels();
  float edgeMixBase = (float)g_oceanEdgeHighlightPercent / 100.0f;
  for (int col = 0; col < kCombinedCols; ++col) {
    float surface = kPanelRows - g_oceanHeight[col];
    float left = g_oceanHeight[col > 0 ? col - 1 : col];
    float right = g_oceanHeight[col + 1 < kCombinedCols ? col + 1 : col];
    float leftImpact = col < 12 ? g_oceanLeftImpactEnergy * (1.0f - (float)col / 12.0f) : 0.0f;
    float rightImpact = col >= kCombinedCols - 12
        ? g_oceanRightImpactEnergy * ((float)(col - (kCombinedCols - 12)) / 12.0f) : 0.0f;
    float impact = fmaxf(leftImpact, rightImpact);
    float activity = fminf(
        1.0f, fabsf(right - left) * 0.32f + fabsf(g_oceanVelocity[col]) * 0.18f + impact);
    int edgeRow = (int)floorf(surface + 0.5f);
    for (int row = 0; row < kPanelRows; ++row) {
      float coverage = (row + 1.0f) - surface;
      if (coverage <= 0.0f) continue;
      if (coverage > 1.0f) coverage = 1.0f;
      float depth = fminf(1.0f, ((row + 0.5f) - surface) / 4.0f);
      float level = coverage * (0.42f + depth * 0.48f);
      setCombinedPixel(row, col, color(
          oceanChannel(g_oceanRed, level),
          oceanChannel(g_oceanGreen, level),
          oceanChannel(g_oceanBlue, level)));
    }
    if (edgeRow >= 0 && edgeRow < kPanelRows) {
      float edgeMix = fminf(1.0f, edgeMixBase * (0.55f + activity * 0.75f));
      float glow = 0.65f + activity * 0.55f;
      uint8_t r = oceanChannel((uint8_t)(g_oceanRed + (255 - g_oceanRed) * edgeMix), glow);
      uint8_t g = oceanChannel((uint8_t)(g_oceanGreen + (255 - g_oceanGreen) * edgeMix), glow);
      uint8_t b = oceanChannel((uint8_t)(g_oceanBlue + (255 - g_oceanBlue) * edgeMix), glow);
      setCombinedPixel(edgeRow, col, color(r, g, b));
    }
  }
  uint32_t splashPhase = now / kOceanFrameIntervalMs;
  if (g_oceanLeftImpactEnergy > 0.18f) {
    int count = 2 + (int)(g_oceanLeftImpactEnergy * 5.0f);
    for (int i = 0; i < count; ++i) {
      int col = 2 + ((i * 3 + splashPhase) % 9);
      int row = (i + splashPhase) % 3;
      float glow = 0.75f + g_oceanLeftImpactEnergy * 0.45f;
      setCombinedPixel(row, col, color(
          oceanChannel(190, glow), oceanChannel(245, glow), oceanChannel(255, glow)));
    }
  }
  if (g_oceanRightImpactEnergy > 0.18f) {
    int count = 2 + (int)(g_oceanRightImpactEnergy * 5.0f);
    for (int i = 0; i < count; ++i) {
      int col = kCombinedCols - 3 - ((i * 3 + splashPhase) % 9);
      int row = (i + splashPhase + 1) % 3;
      float glow = 0.75f + g_oceanRightImpactEnergy * 0.45f;
      setCombinedPixel(row, col, color(
          oceanChannel(190, glow), oceanChannel(245, glow), oceanChannel(255, glow)));
    }
  }
  constrainOceanPowerLocked();
  showPixelsLocked();
}

void drawDizzyMouthLocked(uint32_t frameIndex) {
  int phase = (int)(frameIndex % 30);
  int wave = phase <= 15 ? phase : 30 - phase;
  int cx = 25 + ((phase % 6) < 3 ? -1 : 1);
  int cy = 4;
  int halfW = 12 + (wave * 13) / 15;
  int halfH = 2 + (wave * 3) / 15;
  bool customMouth = strcmp(g_effectTemplate, "mouth") == 0;
  uint32_t white = customMouth ? effectColor(false) : color(g_brightness, g_brightness, g_brightness);
  uint32_t cyan = color(0, scaledBrightness(210), g_brightness);
  uint32_t blue = color(0, scaledBrightness(95), g_brightness);
  uint32_t pink = customMouth ? effectColor(true) : color(g_brightness, scaledBrightness(45), scaledBrightness(125));
  uint32_t deep = color(scaledBrightness(105), 0, scaledBrightness(45));
  uint32_t yellow = color(g_brightness, scaledBrightness(210), 0);

  int innerW = halfW > 4 ? halfW - 3 : halfW;
  int innerH = halfH > 2 ? halfH - 1 : halfH;
  for (int row = 0; row < kPanelRows; ++row) {
    for (int col = 0; col < kCombinedCols; ++col) {
      int dx = col - cx;
      int dy = row - cy;
      int outer = (dx * dx * 100) / (halfW * halfW) + (dy * dy * 100) / (halfH * halfH);
      int inner = (dx * dx * 100) / (innerW * innerW) + (dy * dy * 100) / (innerH * innerH);
      if (outer <= 112 && inner >= 58) {
        setCombinedPixel(row, col, row > cy + 1 ? pink : white);
      } else if (outer <= 50 && wave > 6 && row >= cy) {
        setCombinedPixel(row, col, ((col + phase) % 2) ? deep : pink);
      }
    }
  }

  int left = cx - halfW;
  int right = cx + halfW;
  for (int i = 0; i < 4; ++i) {
    uint32_t accent = (i % 2) ? cyan : blue;
    setCombinedPixel(3 + (i % 2), left - 2 + i, accent);
    setCombinedPixel(5 - (i % 2), right + 2 - i, accent);
  }
  if (wave > 5) {
    setCombinedPixel(1, left - 4, yellow);
    setCombinedPixel(2, left - 5, yellow);
    setCombinedPixel(6, right + 4, yellow);
    setCombinedPixel(7, right + 5, yellow);
  }
}

void drawGenericClipMarkerLocked(uint32_t frameIndex) {
  int phase = (int)(frameIndex % 30);
  int wave = phase <= 15 ? phase : 30 - phase;
  int halfW = 10 + (wave * 10) / 15;
  uint32_t white = color(g_brightness, g_brightness, g_brightness);
  uint32_t cyan = color(0, scaledBrightness(180), g_brightness);
  for (int col = 25 - halfW; col <= 25 + halfW; ++col) {
    setCombinedPixel(4, col, white);
    if (wave > 7) {
      setCombinedPixel(3, col, cyan);
      setCombinedPixel(5, col, cyan);
    }
  }
}

uint32_t dimColor(uint32_t pixelColor, uint8_t percent) {
  uint8_t red = (uint8_t)((pixelColor >> 16) & 0xFF);
  uint8_t green = (uint8_t)((pixelColor >> 8) & 0xFF);
  uint8_t blue = (uint8_t)(pixelColor & 0xFF);
  return color(
      (uint8_t)((red * (uint16_t)percent) / 100),
      (uint8_t)((green * (uint16_t)percent) / 100),
      (uint8_t)((blue * (uint16_t)percent) / 100));
}

void drawCodexWordLocked(uint32_t frameIndex) {
  clearPixels();

  // Two deliberate black gaps make the whole word visibly blink. The short
  // dim phase and cyan scan row give it a compact terminal/glitch character.
  uint8_t phase = (uint8_t)(frameIndex % 20);
  bool visible = phase < 6 || (phase >= 9 && phase < 16) || phase >= 18;
  if (!visible) return;

  uint8_t level = phase == 18 ? 35 : 100;
  uint32_t primary = dimColor(effectColor(false), level);
  uint32_t secondary = dimColor(effectColor(true), level);
  int scanRow = (int)(frameIndex % 7);
  constexpr int kStartCol = 7;
  constexpr int kGlyphWidth = 5;
  constexpr int kGlyphGap = 3;

  for (int glyph = 0; glyph < 5; ++glyph) {
    int glyphCol = kStartCol + glyph * (kGlyphWidth + kGlyphGap);
    for (int row = 0; row < 7; ++row) {
      uint8_t bits = kCodexGlyphRows[glyph][row];
      for (int col = 0; col < kGlyphWidth; ++col) {
        if (bits & (1U << (kGlyphWidth - 1 - col))) {
          setCombinedPixel(row + 1, glyphCol + col, row == scanRow ? secondary : primary);
        }
      }
    }
  }

  // A blinking cursor completes the command-line/Codex visual language.
  if ((phase % 10) < 5) {
    for (int row = 2; row <= 7; ++row) setCombinedPixel(row, 47, secondary);
  }
}

void renderStoredClipFrameLocked(uint32_t frameIndex) {
  clearPixels();
  if (!LedClipPlayer::readTick((uint8_t)(frameIndex % LedClipPlayer::kTickCount),
                               g_storedPackedFrame,
                               sizeof(g_storedPackedFrame))) {
    Serial.printf("[led_matrix] stored frame read failed: %s\n", LedClipPlayer::lastError());
    return;
  }

  uint32_t channelSum = 0;
  for (uint16_t pixel = 0; pixel < LedClipPlayer::kPixelCount; ++pixel) {
    uint8_t paletteIndex = LedClipPlayer::paletteIndexAt(g_storedPackedFrame, pixel);
    uint32_t raw = LedClipPlayer::paletteColor(paletteIndex);
    uint8_t red = (uint8_t)((raw >> 16) & 0xFF);
    uint8_t green = (uint8_t)((raw >> 8) & 0xFF);
    uint8_t blue = (uint8_t)(raw & 0xFF);
    red = (uint8_t)(((uint32_t)red * g_brightness * g_effectIntensityPercent) / (255UL * 100UL));
    green = (uint8_t)(((uint32_t)green * g_brightness * g_effectIntensityPercent) / (255UL * 100UL));
    blue = (uint8_t)(((uint32_t)blue * g_brightness * g_effectIntensityPercent) / (255UL * 100UL));
    channelSum += red + green + blue;
    setPhysicalPixel(pixel, color(red, green, blue));
  }
  if (channelSum > LAMPGO_LED_MAX_CHANNEL_SUM) {
    for (uint16_t pixel = 0; pixel < LedClipPlayer::kPixelCount; ++pixel) {
      int offset = pixel * 3;
      g_pixels[offset + 0] = (uint8_t)(((uint32_t)g_pixels[offset + 0] * LAMPGO_LED_MAX_CHANNEL_SUM) / channelSum);
      g_pixels[offset + 1] = (uint8_t)(((uint32_t)g_pixels[offset + 1] * LAMPGO_LED_MAX_CHANNEL_SUM) / channelSum);
      g_pixels[offset + 2] = (uint8_t)(((uint32_t)g_pixels[offset + 2] * LAMPGO_LED_MAX_CHANNEL_SUM) / channelSum);
    }
  }
  showPixelsLocked();
}

void renderClipFrameLocked(uint32_t frameIndex) {
  if (!g_clipActive || g_clipFrameCount == 0) return;
  if (g_storedClipActive) {
    renderStoredClipFrameLocked(frameIndex);
    return;
  }
  clearPixels();
  if (strcmp(g_effectTemplate, "codex") == 0) {
    drawCodexWordLocked(frameIndex);
    showPixelsLocked();
    return;
  }
  if (strcmp(g_effectTemplate, "mouth") == 0) {
    if (strcmp(g_effectVariant, "flat") == 0) {
      for (int col = 10; col <= 40; ++col) setCombinedPixel(4, col, effectColor(false));
    } else if (strcmp(g_effectVariant, "smile") == 0) {
      for (int col = 12; col <= 38; ++col) {
        int distance = abs(col - 25);
        int row = 3 + (distance * distance) / 190;
        setCombinedPixel(row, col, effectColor(false));
      }
    } else if (strcmp(g_effectVariant, "dizzy") == 0) {
      drawDizzyMouthLocked(frameIndex);
    } else {
      int pulse = 2 + (int)(frameIndex % 10 < 5 ? frameIndex % 5 : 9 - frameIndex % 10);
      int halfW = 17 + pulse;
      int halfH = 3 + pulse / 2;
      for (int row = 0; row < kPanelRows; ++row) {
        for (int col = 0; col < kCombinedCols; ++col) {
          int dx = col - 25;
          int dy = row - 4;
          int outer = (dx * dx * 100) / (halfW * halfW) + (dy * dy * 100) / (halfH * halfH);
          if (outer >= 48 && outer <= 115) setCombinedPixel(row, col, effectColor(false));
        }
      }
    }
    showPixelsLocked();
    return;
  }
  if (strcmp(g_effectTemplate, "arrow") == 0) {
    int pattern = 11;
    if (strcmp(g_effectDirection, "left") == 0) pattern = 10;
    else if (strcmp(g_effectDirection, "up") == 0) pattern = 12;
    else if (strcmp(g_effectDirection, "down") == 0) pattern = 13;
    drawCombinedPattern(pattern, effectColor(false));
    showPixelsLocked();
    return;
  }
  if (strcmp(g_effectTemplate, "heart") == 0) {
    uint8_t pulse = (frameIndex % 10) < 5 ? 100 : 65;
    uint8_t previousIntensity = g_effectIntensityPercent;
    g_effectIntensityPercent = (uint8_t)((previousIntensity * pulse) / 100);
    drawBitmap8Pair(kHeartPattern, effectColor(false));
    g_effectIntensityPercent = previousIntensity;
    showPixelsLocked();
    return;
  }
  if (strcmp(g_effectTemplate, "pulse") == 0) {
    renderRingFill(effectColor(false), frameIndex);
    showPixelsLocked();
    return;
  }
  char key[32];
  normalizeKey(g_clipId, key, sizeof(key));
  if (strcmp(key, "dizzy") == 0) {
    drawDizzyMouthLocked(frameIndex);
  } else {
    drawGenericClipMarkerLocked(frameIndex);
  }
  showPixelsLocked();
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

void drawBitmap8(const uint8_t bitmap[8][8], uint32_t pixelColor, int rowOffset = 0) {
  drawBitmap8OnMatrix(bitmap, pixelColor, 4, rowOffset);
}

void drawBitmap8Pair(const uint8_t bitmap[8][8], uint32_t pixelColor, int rowOffset) {
  drawBitmap8PairCell(bitmap, pixelColor, 1, rowOffset);
  drawBitmap8PairCell(bitmap, pixelColor, 9, rowOffset);
}

void drawBitmap8PairWithBottomDots(const uint8_t bitmap[8][8], uint32_t pixelColor) {
  drawBitmap8Pair(bitmap, pixelColor);
  fillLogicalCell(6, 3, pixelColor);
  fillLogicalCell(7, 3, pixelColor);
  fillLogicalCell(6, 4, pixelColor);
  fillLogicalCell(7, 4, pixelColor);
  fillLogicalCell(6, 11, pixelColor);
  fillLogicalCell(7, 11, pixelColor);
  fillLogicalCell(6, 12, pixelColor);
  fillLogicalCell(7, 12, pixelColor);
}

void drawCombinedPattern(int patternIndex, uint32_t primaryColor, uint32_t secondaryColor) {
  clearPixels();
  if (patternIndex < 0 ||
      patternIndex >= (int)(sizeof(kCombinedPatterns) / sizeof(kCombinedPatterns[0]))) {
    return;
  }
  for (int row = 0; row < kPatternRows; ++row) {
    for (int col = 0; col < kPatternCols; ++col) {
      uint8_t value = kCombinedPatterns[patternIndex][row][col];
      if (value == 0) continue;
      fillLogicalCell(row, col, value == 2 ? secondaryColor : primaryColor);
    }
  }
}

void renderRingFill(uint32_t pixelColor, uint32_t frame) {
  clearPixels();
  int maxRing = (int)(frame > 4 ? 4 : frame);
  for (int row = 0; row < kPanelRows; ++row) {
    for (int col = 0; col < kCombinedCols; ++col) {
      int ring = min(min(row, kPanelRows - 1 - row), min(col, kCombinedCols - 1 - col));
      if (ring <= maxRing) {
        setCombinedPixel(row, col, pixelColor);
      }
    }
  }
}

void renderTheater(uint32_t pixelColor, uint32_t frame) {
  clearPixels();
  int start = (int)(frame % 3);
  for (int index = start; index < LAMPGO_LED_PIXEL_COUNT; index += 3) {
    setPhysicalPixel(index, pixelColor);
  }
}

void renderRainbow(uint32_t frame) {
  clearPixels();
  uint16_t firstHue = (uint16_t)((frame * 256U) & 0xFFFF);
  for (int index = 0; index < LAMPGO_LED_PIXEL_COUNT; ++index) {
    uint16_t hue = firstHue + (uint32_t)index * 65536UL / LAMPGO_LED_PIXEL_COUNT;
    setPhysicalPixel(index, hsvColor(hue, 255, g_brightness));
  }
}

void renderRainbowChase(uint32_t frame) {
  clearPixels();
  int start = (int)(frame % 3);
  uint16_t firstHue = (uint16_t)(((frame * (65536U / 90U))) & 0xFFFF);
  for (int index = start; index < LAMPGO_LED_PIXEL_COUNT; index += 3) {
    uint16_t hue = firstHue + (uint32_t)index * 65536UL / LAMPGO_LED_PIXEL_COUNT;
    setPhysicalPixel(index, hsvColor(hue, 255, g_brightness));
  }
}

void renderMusic(uint32_t frame) {
  static const int8_t offsets[] = {0, -1, -2, -1, 0, 1, 0};
  const uint32_t colors[] = {
      color(g_brightness, 0, g_brightness),
      color(0, g_brightness, g_brightness),
      color(g_brightness, g_brightness, 0),
      color(g_brightness, 0, 0),
  };

  clearPixels();
  drawBitmap8Pair(kMusicNote, colors[(frame / 7) % 4], offsets[frame % 7]);
}

void renderThinking(uint32_t frame) {
  clearPixels();
  uint8_t low = g_brightness / 10;
  drawBitmap8Pair(kCirclePattern, color(low, low, low));

  static const uint8_t path[20][2] = {
      {0,2},{0,3},{0,4},{0,5},{1,6},{2,7},{3,7},{4,7},{5,7},{6,6},
      {7,5},{7,4},{7,3},{7,2},{6,1},{5,0},{4,0},{3,0},{2,0},{1,1},
  };

  const uint8_t *point = path[frame % 20];
  fillLogicalCell(point[0], (point[1] * 6) / 8 + 1, color(g_brightness, g_brightness, g_brightness));
  fillLogicalCell(point[0], (point[1] * 6) / 8 + 9, color(g_brightness, g_brightness, g_brightness));
}

void renderHeart(uint32_t frame) {
  static const uint8_t levels[] = {255, 128, 77, 128, 255};
  clearPixels();
  drawBitmap8(kHeartPattern, color(scaledBrightness(levels[frame % 5]), 0, 0));
}

void renderSurprised(uint32_t frame) {
  uint32_t phase = frame % 12;
  int patternIndex = phase == 0 ? 19 : (phase == 1 ? 20 : 21);
  drawCombinedPattern(patternIndex, color(0, g_brightness, g_brightness));
}

void renderSleep(uint32_t frame) {
  uint32_t pixelColor = color(g_brightness / 2, g_brightness / 2, g_brightness);
  drawCombinedPattern(frame % 2 == 0 ? 17 : 18, pixelColor);
}

void renderHelpless(uint32_t frame) {
  uint32_t faceColor = color(0, g_brightness, g_brightness);
  uint32_t sweatColor = color(0, g_brightness / 2, g_brightness);
  drawCombinedPattern((frame % 8) < 5 ? 4 : 5, faceColor, sweatColor);
}

void renderWink(uint32_t frame) {
  uint32_t pixelColor = color(0, g_brightness, g_brightness);
  if (frame >= 17) {
    drawCombinedPattern(8, pixelColor);
  } else if (frame < 10) {
    drawCombinedPattern(8, pixelColor);
  } else {
    drawCombinedPattern(9, pixelColor);
  }
}

void renderFocused() {
  drawCombinedPattern(g_focusEyesOpen ? 6 : 7, color(0, g_brightness, g_brightness));
}

bool modeIsAnimated(int mode) {
  return (mode >= 1 && mode <= 10) ||
         mode == 20 ||
         mode == 23 ||
         mode == 24 ||
         mode == 27 ||
         mode == 28 ||
         mode == 29 ||
         mode == 31 ||
         mode == 32;
}

bool animationStopsAfterHold(int mode) {
  return mode >= 1 && mode <= 4;
}

bool animationIsComplete(int mode) {
  if (animationStopsAfterHold(mode)) {
    return g_animFrame >= 3;
  }
  if (mode == 32) {
    return g_animFrame >= 17;
  }
  return false;
}

uint32_t frameIntervalMs(int mode) {
  switch (mode) {
    case 1:
    case 2:
    case 3:
    case 4:
      return 200;
    case 5:
    case 6:
    case 7:
    case 8:
      return 50;
    case 9:
      return 20;
    case 10:
      return 50;
    case 20:
      return 150;
    case 23:
      return 200;
    case 24:
      return 200;
    case 27:
      return 80;
    case 28:
      return 1200;
    case 29:
      return 200;
    case 32:
      return 50;
    default:
      return 1000;
  }
}

void renderCurrentLocked() {
  if (!g_outputStarted) {
    g_outputStarted = true;
  }

  g_useEllipseMask = g_mode >= 1 && g_mode <= 10;

  switch (g_mode) {
    case 0:
      clearPixels();
      break;
    case 1:
      renderRingFill(color(g_brightness, 0, 0), g_animFrame);
      break;
    case 2:
      renderRingFill(color(0, g_brightness, 0), g_animFrame);
      break;
    case 3:
      renderRingFill(color(0, 0, g_brightness), g_animFrame);
      break;
    case 4:
      renderRingFill(color(g_brightness, g_brightness, g_brightness), g_animFrame);
      break;
    case 5:
      renderTheater(color(g_brightness, g_brightness, g_brightness), g_animFrame);
      break;
    case 6:
      renderTheater(color(g_brightness, 0, 0), g_animFrame);
      break;
    case 7:
      renderTheater(color(0, g_brightness, 0), g_animFrame);
      break;
    case 8:
      renderTheater(color(0, 0, g_brightness), g_animFrame);
      break;
    case 9:
      renderRainbow(g_animFrame);
      break;
    case 10:
      renderRainbowChase(g_animFrame);
      break;
    case 11:
      drawCombinedPattern(10, color(0, g_brightness, 0));
      break;
    case 12:
      drawCombinedPattern(11, color(0, g_brightness, 0));
      break;
    case 13:
      drawCombinedPattern(12, color(0, g_brightness, 0));
      break;
    case 14:
      drawCombinedPattern(13, color(0, g_brightness, 0));
      break;
    case 15:
      clearPixels();
      drawBitmap8Pair(kCheckPattern, color(0, g_brightness, 0));
      break;
    case 16:
      clearPixels();
      drawBitmap8Pair(kCrossPattern, color(g_brightness, 0, 0));
      break;
    case 17:
      clearPixels();
      drawBitmap8PairWithBottomDots(kExclaimPattern, color(g_brightness, (uint8_t)(g_brightness * 7 / 10), 0));
      break;
    case 18:
      clearPixels();
      drawBitmap8PairWithBottomDots(kQuestionPattern, color(g_brightness, 0, 0));
      break;
    case 19:
      clearPixels();
      drawBitmap8Pair(kStarPattern, color(g_brightness, g_brightness, 0));
      break;
    case 20:
      renderMusic(g_animFrame);
      break;
    case 21:
      drawCombinedPattern(0, color(0, g_brightness, g_brightness));
      break;
    case 22:
      drawCombinedPattern(16, color(0, g_brightness / 3, g_brightness));
      break;
    case 23:
      renderHeart(g_animFrame);
      break;
    case 24:
      renderSurprised(g_animFrame);
      break;
    case 25:
      drawCombinedPattern(3, color(0, g_brightness, g_brightness), color(g_brightness, 0, 0));
      break;
    case 26:
      drawCombinedPattern(1, color(g_brightness, 0, 0));
      break;
    case 27:
      renderThinking(g_animFrame);
      break;
    case 28:
      renderSleep(g_animFrame);
      break;
    case 29:
      renderHelpless(g_animFrame);
      break;
    case 30:
      drawCombinedPattern(2, color(0, g_brightness, g_brightness));
      break;
    case 31:
      renderFocused();
      break;
    case 32:
      renderWink(g_animFrame);
      break;
    case 33:
      drawCombinedPattern(22, color(g_brightness, g_brightness, g_brightness), color(g_brightness, 0, 0));
      break;
    default:
      clearPixels();
      break;
  }

  showPixelsLocked();
}

void resetFocusedAnimationLocked(uint32_t now) {
  g_focusEyesOpen = true;
  g_focusBlinksRemaining = 0;
  g_focusNextTransitionMs = now + 2000 + (esp_random() % 3001);
}

void resetAnimationStateLocked(uint32_t now) {
  g_animFrame = 0;
  // setMode/setBrightness renders frame 0 immediately, so the next step
  // must wait a full frame interval instead of advancing on the next task tick.
  g_lastFrameMs = now;
  g_modeStartedMs = now;
  if (g_mode == 31) {
    resetFocusedAnimationLocked(now);
  }
}

bool updateFocusedAnimationLocked(uint32_t now) {
  if (g_mode != 31 || now < g_focusNextTransitionMs) {
    return false;
  }

  if (g_focusEyesOpen) {
    if (g_focusBlinksRemaining == 0) {
      uint32_t roll = esp_random() % 100;
      if (roll < 70) g_focusBlinksRemaining = 1;
      else if (roll < 95) g_focusBlinksRemaining = 2;
      else g_focusBlinksRemaining = 3;
    }
    g_focusEyesOpen = false;
    g_focusNextTransitionMs = now + 100 + (esp_random() % 81);
  } else {
    g_focusEyesOpen = true;
    if (g_focusBlinksRemaining > 0) {
      --g_focusBlinksRemaining;
    }
    if (g_focusBlinksRemaining > 0) {
      g_focusNextTransitionMs = now + 100 + (esp_random() % 101);
    } else {
      g_focusNextTransitionMs = now + 2000 + (esp_random() % 3001);
    }
  }
  return true;
}

void ledTask(void *) {
  while (true) {
    if (g_ready) {
      uint32_t now = millis();
      if (g_mutex && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        bool shouldRender = false;
        if (!g_expressionLoop && g_expressionEndMs > 0 && (int32_t)(now - g_expressionEndMs) >= 0) {
          releaseClipLocked();
          g_mode = 0;
          clearPixels();
          showPixelsLocked();
        } else if (g_oceanActive) {
          if (g_oceanLastFrameMs == 0 || now - g_oceanLastFrameMs >= kOceanFrameIntervalMs) {
            g_oceanLastFrameMs = now;
            shouldRender = true;
          }
        } else if (g_clockActive) {
          bool animated = strcmp(g_clockEffect, "steady") != 0;
          if (animated && (g_clockLastFrameMs == 0 || now - g_clockLastFrameMs >= 50)) {
            g_clockLastFrameMs = now;
            shouldRender = true;
          }
        } else if (g_clipActive) {
          uint32_t interval = g_clipFps > 0 ? (1000UL / g_clipFps) : 100;
          if (interval < 16) interval = 16;
          if (g_clipLastFrameMs == 0 || now - g_clipLastFrameMs >= interval) {
            g_clipFrame = (g_clipFrame + 1) % g_clipFrameCount;
            g_clipLastFrameMs = now;
            shouldRender = true;
          }
        } else if (g_mode == 31) {
          shouldRender = updateFocusedAnimationLocked(now);
        } else if (modeIsAnimated(g_mode) && !animationIsComplete(g_mode)) {
          if (g_lastFrameMs == 0 || now - g_lastFrameMs >= frameIntervalMs(g_mode)) {
            g_animFrame++;
            g_lastFrameMs = now;
            shouldRender = true;
          }
        }
        if (shouldRender) {
          if (g_oceanActive) {
            renderOceanLocked(now);
          } else if (g_clockActive) {
            renderClockLocked(now);
          } else if (g_clipActive) {
            renderClipFrameLocked(g_clipFrame);
          } else {
            renderCurrentLocked();
          }
        }
        xSemaphoreGive(g_mutex);
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

}  // namespace

namespace LedSerial {

bool begin() {
  if (!g_mutex) {
    g_mutex = xSemaphoreCreateMutex();
  }

  randomSeed(esp_random());
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
  bool ok = setModeLocal(mode, true, 0);
  if (ok) {
    DisplayLink::sendExpression(modeName(mode));
  }
  return ok;
}

bool setModeLocal(int mode, bool loop, uint32_t durationMs) {
  if (!g_ready || mode < 0 || mode > kMaxMode || !takeLedLock()) {
    return false;
  }

  char command[8];
  snprintf(command, sizeof(command), "m%d", mode);
  recordCommand(command);
  releaseClipLocked();
  g_mode = mode;
  g_expressionLoop = loop;
  g_expressionEndMs = !loop && durationMs > 0 ? millis() + durationMs : 0;
  resetAnimationStateLocked(millis());
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
  if (g_oceanActive) {
    renderOceanLocked(millis());
  } else if (g_clockActive) {
    renderClockLocked(millis());
  } else if (g_clipActive) {
    renderClipFrameLocked(g_clipFrame);
  } else {
    resetAnimationStateLocked(millis());
    renderCurrentLocked();
  }
  bool ok = g_lastShowOk;
  giveLedLock();

  Serial.printf("[led_matrix] brightness=%d pin=%d\n", brightness, (int)LAMPGO_LED_PIXEL_PIN);
  return ok;
}

bool playClip(const char *clipId) {
  if (!g_ready || !clipId || !clipId[0] || !takeLedLock()) {
    return false;
  }

  releaseClipLocked();
  snprintf(g_clipId, sizeof(g_clipId), "%s", clipId);
  g_clipActive = true;
  g_clipFrameCount = 30;
  g_clipFps = 10;
  g_clipFrame = 0;
  g_clipLastFrameMs = millis();
  g_expressionLoop = true;
  g_expressionEndMs = 0;
  recordCommand("clip");
  renderClipFrameLocked(0);
  bool ok = g_lastShowOk;
  giveLedLock();

  Serial.printf("[led_matrix] procedural clip=%s frames=%u fps=%u\n",
                clipId, (unsigned)g_clipFrameCount, (unsigned)g_clipFps);
  DisplayLink::sendClipPlay(clipId);
  return ok;
}

bool playEffect(const EffectConfig &config) {
  if (!g_ready || !config.effectId || !config.effectId[0] || !config.templateName || !config.templateName[0] ||
      !takeLedLock()) {
    return false;
  }
  if (strcmp(config.templateName, "mouth") != 0 && strcmp(config.templateName, "arrow") != 0 &&
      strcmp(config.templateName, "heart") != 0 && strcmp(config.templateName, "pulse") != 0 &&
      strcmp(config.templateName, "codex") != 0) {
    giveLedLock();
    return false;
  }

  releaseClipLocked();
  snprintf(g_clipId, sizeof(g_clipId), "%s", config.effectId);
  snprintf(g_effectTemplate, sizeof(g_effectTemplate), "%s", config.templateName);
  snprintf(g_effectVariant, sizeof(g_effectVariant), "%s", config.variant ? config.variant : "");
  snprintf(g_effectDirection, sizeof(g_effectDirection), "%s", config.direction ? config.direction : "right");
  g_effectRed = config.red;
  g_effectGreen = config.green;
  g_effectBlue = config.blue;
  g_effectSecondaryRed = config.secondaryRed;
  g_effectSecondaryGreen = config.secondaryGreen;
  g_effectSecondaryBlue = config.secondaryBlue;
  g_effectIntensityPercent = config.intensityPercent < 10 ? 10 :
                             (config.intensityPercent > 100 ? 100 : config.intensityPercent);
  if (config.brightness >= 1) g_brightness = config.brightness;
  g_clipActive = true;
  g_clipFps = 10;
  uint32_t durationMs = config.durationMs > 0 ? config.durationMs : 3000;
  g_clipFrameCount = (uint16_t)max((uint32_t)1, durationMs / 100UL);
  g_clipFrame = 0;
  g_clipLastFrameMs = millis();
  g_expressionLoop = config.loop;
  g_expressionEndMs = config.loop ? 0 : millis() + durationMs;
  recordCommand("effect");
  renderClipFrameLocked(0);
  bool ok = g_lastShowOk;
  giveLedLock();

  Serial.printf("[led_matrix] effect=%s template=%s loop=%d duration=%lu\n",
                config.effectId, config.templateName, config.loop ? 1 : 0, (unsigned long)durationMs);
  return ok;
}

bool playStoredEffect(const StoredEffectConfig &config) {
  if (!g_ready || !config.effectId || !config.effectId[0] || !takeLedLock()) return false;
  releaseClipLocked();
  if (!LedClipPlayer::open(config.effectId, config.colors)) {
    Serial.printf("[led_matrix] stored effect open failed id=%s error=%s\n",
                  config.effectId, LedClipPlayer::lastError());
    giveLedLock();
    return false;
  }
  snprintf(g_clipId, sizeof(g_clipId), "%s", config.effectId);
  snprintf(g_effectTemplate, sizeof(g_effectTemplate), "pixel_clip");
  g_effectIntensityPercent = config.intensityPercent < 10 ? 10 :
                             (config.intensityPercent > 100 ? 100 : config.intensityPercent);
  if (config.brightness >= 1) g_brightness = config.brightness > 96 ? 96 : config.brightness;
  g_clipActive = true;
  g_storedClipActive = true;
  g_clipFps = LedClipPlayer::kFps;
  g_clipFrameCount = LedClipPlayer::kTickCount;
  g_clipFrame = 0;
  g_clipLastFrameMs = millis();
  uint32_t durationMs = config.durationMs > 0 ? config.durationMs : 3000;
  g_expressionLoop = config.loop;
  g_expressionEndMs = config.loop ? 0 : millis() + durationMs;
  recordCommand("stored_effect");
  renderStoredClipFrameLocked(0);
  bool ok = g_lastShowOk;
  giveLedLock();
  Serial.printf("[led_matrix] stored effect=%s loop=%d bytes/frame=%u\n",
                config.effectId, config.loop ? 1 : 0, (unsigned)LedClipPlayer::kFrameBytes);
  return ok;
}

bool showClock(const ClockConfig &config) {
  if (!g_ready || config.hour > 23 || config.minute > 59 || !config.effect || !takeLedLock()) return false;
  if (strcmp(config.effect, "steady") != 0 && strcmp(config.effect, "blink") != 0 &&
      strcmp(config.effect, "orbit") != 0) {
    giveLedLock();
    return false;
  }

  releaseClipLocked();
  g_mode = 0;
  g_clockActive = true;
  g_clockHour = config.hour;
  g_clockMinute = config.minute;
  g_clockRed = config.red;
  g_clockGreen = config.green;
  g_clockBlue = config.blue;
  snprintf(g_clockEffect, sizeof(g_clockEffect), "%s", config.effect);
  if (config.brightness >= 1) g_brightness = config.brightness > 96 ? 96 : config.brightness;
  g_clockLastFrameMs = millis();
  recordCommand("clock");
  renderClockLocked(g_clockLastFrameMs);
  bool ok = g_lastShowOk;
  giveLedLock();
  Serial.printf("[led_matrix] clock=%02u:%02u effect=%s\n", g_clockHour, g_clockMinute, g_clockEffect);
  return ok;
}

bool stopClock() {
  if (!g_ready || !takeLedLock()) return false;
  bool wasActive = g_clockActive;
  g_clockActive = false;
  g_clockLastFrameMs = 0;
  if (wasActive) {
    g_mode = 0;
    recordCommand("clock_stop");
    clearPixels();
    showPixelsLocked();
  }
  bool ok = !wasActive || g_lastShowOk;
  giveLedLock();
  return ok;
}

bool startOcean(const OceanConfig &config) {
  if (!g_ready || config.brightness < 1 || config.brightness > 96 ||
      config.fillPercent < 20 || config.fillPercent > 80 ||
      config.sensitivityPercent < 25 || config.sensitivityPercent > 200 ||
      config.edgeHighlightPercent > 100 ||
      config.tiltPercent < 50 || config.tiltPercent > 160 ||
      config.impactPercent > 200 ||
      config.dampingPercent < 80 || config.dampingPercent > 200 || !takeLedLock()) {
    return false;
  }

  releaseClipLocked();
  g_mode = 0;
  g_oceanActive = true;
  g_oceanRed = config.red;
  g_oceanGreen = config.green;
  g_oceanBlue = config.blue;
  g_brightness = config.brightness;
  g_oceanFillPercent = config.fillPercent;
  g_oceanSensitivityPercent = config.sensitivityPercent;
  g_oceanEdgeHighlightPercent = config.edgeHighlightPercent;
  g_oceanTiltPercent = config.tiltPercent;
  g_oceanImpactPercent = config.impactPercent;
  g_oceanDampingPercent = config.dampingPercent;
  float baseHeight = 1.2f + 6.3f * ((float)g_oceanFillPercent / 100.0f);
  for (int col = 0; col < kCombinedCols; ++col) {
    g_oceanHeight[col] = baseHeight + 0.08f * sinf(col * 0.31f);
    g_oceanVelocity[col] = 0.0f;
    g_oceanNextHeight[col] = g_oceanHeight[col];
  }
  g_oceanFilteredAngle = 0.0f;
  g_oceanBulkTilt = 0.0f;
  g_oceanBulkVelocity = 0.0f;
  g_oceanPreviousInputVelocity = 0.0f;
  g_oceanLeftImpactEnergy = 0.0f;
  g_oceanRightImpactEnergy = 0.0f;
  g_oceanLeftContact = false;
  g_oceanRightContact = false;
  uint32_t now = millis();
  portENTER_CRITICAL(&g_oceanInputMux);
  g_oceanInputAngleDeg = 0.0f;
  g_oceanInputAngularVelocityDps = 0.0f;
  g_oceanInputSequence = 0;
  g_oceanInputAtMs = now;
  portEXIT_CRITICAL(&g_oceanInputMux);
  g_oceanLastFrameMs = now;
  recordCommand("ocean");
  renderOceanLocked(now);
  bool ok = g_lastShowOk;
  giveLedLock();
  Serial.printf("[led_matrix] ocean start fill=%u sensitivity=%u tilt=%u impact=%u damping=%u fps=%u\n",
                (unsigned)g_oceanFillPercent, (unsigned)g_oceanSensitivityPercent,
                (unsigned)g_oceanTiltPercent, (unsigned)g_oceanImpactPercent,
                (unsigned)g_oceanDampingPercent,
                (unsigned)(1000 / kOceanFrameIntervalMs));
  return ok;
}

bool updateOceanInput(float angleDeg, float angularVelocityDps, uint32_t sequence) {
  if (!g_ready || !g_oceanActive || !isfinite(angleDeg) || !isfinite(angularVelocityDps)) return false;
  portENTER_CRITICAL(&g_oceanInputMux);
  if (sequence >= g_oceanInputSequence) {
    g_oceanInputAngleDeg = fmaxf(-35.0f, fminf(35.0f, angleDeg));
    g_oceanInputAngularVelocityDps = fmaxf(-180.0f, fminf(180.0f, angularVelocityDps));
    g_oceanInputSequence = sequence;
    g_oceanInputAtMs = millis();
  }
  portEXIT_CRITICAL(&g_oceanInputMux);
  return true;
}

bool stopOcean() {
  if (!g_ready || !takeLedLock()) return false;
  bool wasActive = g_oceanActive;
  g_oceanActive = false;
  g_oceanLastFrameMs = 0;
  if (wasActive) {
    g_mode = 0;
    recordCommand("ocean_stop");
    clearPixels();
    showPixelsLocked();
  }
  bool ok = !wasActive || g_lastShowOk;
  giveLedLock();
  if (wasActive) Serial.println("[led_matrix] ocean stop");
  return ok;
}

bool stopExpression(bool syncDisplay) {
  bool ok = setModeLocal(0, true, 0);
  if (syncDisplay) DisplayLink::sendClipStop();
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
  return -1;
}

const char *modeName(int mode) {
  if (mode < 0 || mode > kMaxMode) {
    return "unknown";
  }
  return kModeNames[mode];
}

int maxMode() {
  return kMaxMode;
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

bool clockActive() {
  return g_clockActive;
}

const char *clockEffect() {
  return g_clockEffect;
}

void clockTime(char *out, size_t outLen) {
  if (!out || outLen == 0) return;
  snprintf(out, outLen, "%02u:%02u", g_clockHour, g_clockMinute);
}

bool oceanActive() {
  return g_oceanActive;
}

uint32_t oceanInputAgeMs() {
  uint32_t inputAt = 0;
  portENTER_CRITICAL(&g_oceanInputMux);
  inputAt = g_oceanInputAtMs;
  portEXIT_CRITICAL(&g_oceanInputMux);
  return inputAt == 0 ? 0 : millis() - inputAt;
}

uint8_t oceanRenderFps() {
  return (uint8_t)(1000 / kOceanFrameIntervalMs);
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

}  // namespace LedSerial
