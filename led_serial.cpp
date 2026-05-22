#include "led_serial.h"

#include <Arduino.h>
#include <ctype.h>
#include <esp_heap_caps.h>
#include <esp32-hal-rmt.h>
#include <esp_system.h>
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

constexpr int kPanelRows = 8;
constexpr int kPanelCols = 8;
constexpr int kCombinedCols = 16;
constexpr int kMaxMode = 32;

SemaphoreHandle_t g_mutex = nullptr;
TaskHandle_t g_task = nullptr;
bool g_ready = false;
bool g_outputStarted = false;
bool g_rmtReady = false;
bool g_lastShowOk = false;
bool g_useEllipseMask = false;
int g_mode = 0;
int g_brightness = 127;
uint32_t g_lastWriteMs = 0;
uint32_t g_animFrame = 0;
uint32_t g_lastFrameMs = 0;
uint32_t g_modeStartedMs = 0;
char g_lastCommand[32] = "";
uint8_t g_pixels[LAMPGO_LED_PIXEL_COUNT * 3] = {0};
rmt_data_t *g_rmtData = nullptr;

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

bool isEllipseMasked(int panelIndex, int ledIndex) {
  if (panelIndex == 0) {
    if (ledIndex == 0 || ledIndex == 1) return true;
    if (ledIndex == 8) return true;
    if (ledIndex == 48) return true;
    if (ledIndex == 56 || ledIndex == 57) return true;
  } else if (panelIndex == 1) {
    if (ledIndex == 6 || ledIndex == 7) return true;
    if (ledIndex == 15) return true;
    if (ledIndex == 55) return true;
    if (ledIndex == 62 || ledIndex == 63) return true;
  }
  return false;
}

void setPhysicalPixel(int pixelIndex, uint32_t pixelColor) {
  if (pixelIndex < 0 || pixelIndex >= LAMPGO_LED_PIXEL_COUNT) return;
  int offset = pixelIndex * 3;
  g_pixels[offset + 0] = (uint8_t)((pixelColor >> 8) & 0xFF);
  g_pixels[offset + 1] = (uint8_t)((pixelColor >> 16) & 0xFF);
  g_pixels[offset + 2] = (uint8_t)(pixelColor & 0xFF);
}

void setPanelPixel(int panelIndex, int ledIndex, uint32_t pixelColor) {
  if (panelIndex < 0 || panelIndex >= LAMPGO_LED_PANEL_COUNT) return;
  if (ledIndex < 0 || ledIndex >= LAMPGO_LED_PANEL_PIXELS) return;
  if (g_useEllipseMask && isEllipseMasked(panelIndex, ledIndex)) {
    pixelColor = 0;
  }
  setPhysicalPixel(panelIndex * LAMPGO_LED_PANEL_PIXELS + ledIndex, pixelColor);
}

void setMirroredPixel(int ledIndex, uint32_t pixelColor) {
  if (ledIndex < 0 || ledIndex >= LAMPGO_LED_PANEL_PIXELS) return;
  for (int panel = 0; panel < LAMPGO_LED_PANEL_COUNT; ++panel) {
    setPanelPixel(panel, ledIndex, pixelColor);
  }
}

void setCombinedPixel(int row, int col, uint32_t pixelColor) {
  if (row < 0 || row >= kPanelRows || col < 0 || col >= kCombinedCols) return;
  int panel = col < kPanelCols ? 0 : 1;
  int ledCol = col < kPanelCols ? col : col - kPanelCols;
  int ledIndex = row * kPanelCols + ledCol;
  setPanelPixel(panel, ledIndex, pixelColor);
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

void drawBitmap8(const uint8_t bitmap[8][8], uint32_t pixelColor, int rowOffset = 0) {
  for (int row = 0; row < 8; ++row) {
    int shiftedRow = row + rowOffset;
    if (shiftedRow < 0 || shiftedRow >= 8) continue;
    for (int col = 0; col < 8; ++col) {
      if (bitmap[row][col] == 1) {
        setMirroredPixel(shiftedRow * 8 + col, pixelColor);
      }
    }
  }
}

void drawCombinedPattern(int patternIndex, uint32_t primaryColor, uint32_t secondaryColor = 0) {
  clearPixels();
  if (patternIndex < 0 ||
      patternIndex >= (int)(sizeof(kCombinedPatterns) / sizeof(kCombinedPatterns[0]))) {
    return;
  }
  for (int row = 0; row < 8; ++row) {
    for (int col = 0; col < 16; ++col) {
      uint8_t value = kCombinedPatterns[patternIndex][row][col];
      if (value == 0) continue;
      setCombinedPixel(row, col, value == 2 ? secondaryColor : primaryColor);
    }
  }
}

void renderRingFill(uint32_t pixelColor, uint32_t frame) {
  clearPixels();
  int maxRing = (int)(frame > 3 ? 3 : frame);
  for (int row = 0; row < 8; ++row) {
    for (int col = 0; col < 16; ++col) {
      int ring = min(min(row, 7 - row), min(col, 15 - col));
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
    int panel = index / LAMPGO_LED_PANEL_PIXELS;
    int ledIndex = index % LAMPGO_LED_PANEL_PIXELS;
    setPanelPixel(panel, ledIndex, pixelColor);
  }
}

void renderRainbow(uint32_t frame) {
  clearPixels();
  uint16_t firstHue = (uint16_t)((frame * 256U) & 0xFFFF);
  for (int index = 0; index < LAMPGO_LED_PIXEL_COUNT; ++index) {
    uint16_t hue = firstHue + (uint32_t)index * 65536UL / LAMPGO_LED_PIXEL_COUNT;
    int panel = index / LAMPGO_LED_PANEL_PIXELS;
    int ledIndex = index % LAMPGO_LED_PANEL_PIXELS;
    setPanelPixel(panel, ledIndex, hsvColor(hue, 255, g_brightness));
  }
}

void renderRainbowChase(uint32_t frame) {
  clearPixels();
  int start = (int)(frame % 3);
  uint16_t firstHue = (uint16_t)(((frame * (65536U / 90U))) & 0xFFFF);
  for (int index = start; index < LAMPGO_LED_PIXEL_COUNT; index += 3) {
    uint16_t hue = firstHue + (uint32_t)index * 65536UL / LAMPGO_LED_PIXEL_COUNT;
    int panel = index / LAMPGO_LED_PANEL_PIXELS;
    int ledIndex = index % LAMPGO_LED_PANEL_PIXELS;
    setPanelPixel(panel, ledIndex, hsvColor(hue, 255, g_brightness));
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
  drawBitmap8(kMusicNote, colors[(frame / 7) % 4], offsets[frame % 7]);
}

void renderThinking(uint32_t frame) {
  clearPixels();
  uint8_t low = g_brightness / 10;
  drawBitmap8(kCirclePattern, color(low, low, low));

  static const uint8_t path[20][2] = {
      {0,2},{0,3},{0,4},{0,5},{1,6},{2,7},{3,7},{4,7},{5,7},{6,6},
      {7,5},{7,4},{7,3},{7,2},{6,1},{5,0},{4,0},{3,0},{2,0},{1,1},
  };

  const uint8_t *point = path[frame % 20];
  setMirroredPixel(point[0] * 8 + point[1], color(g_brightness, g_brightness, g_brightness));
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
      drawBitmap8(kCheckPattern, color(0, g_brightness, 0));
      break;
    case 16:
      clearPixels();
      drawBitmap8(kCrossPattern, color(g_brightness, 0, 0));
      break;
    case 17:
      clearPixels();
      drawBitmap8(kExclaimPattern, color(g_brightness, (uint8_t)(g_brightness * 7 / 10), 0));
      break;
    case 18:
      clearPixels();
      drawBitmap8(kQuestionPattern, color(g_brightness, 0, 0));
      break;
    case 19:
      clearPixels();
      drawBitmap8(kStarPattern, color(g_brightness, g_brightness, 0));
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
        if (g_mode == 31) {
          shouldRender = updateFocusedAnimationLocked(now);
        } else if (modeIsAnimated(g_mode) && !animationIsComplete(g_mode)) {
          if (g_lastFrameMs == 0 || now - g_lastFrameMs >= frameIntervalMs(g_mode)) {
            g_animFrame++;
            g_lastFrameMs = now;
            shouldRender = true;
          }
        }
        if (shouldRender) {
          renderCurrentLocked();
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
  if (!g_ready || mode < 0 || mode > kMaxMode || !takeLedLock()) {
    return false;
  }

  char command[8];
  snprintf(command, sizeof(command), "m%d", mode);
  recordCommand(command);
  g_mode = mode;
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
  resetAnimationStateLocked(millis());
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
  return -1;
}

const char *modeName(int mode) {
  if (mode < 0 || mode > kMaxMode) {
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

}  // namespace LedSerial
