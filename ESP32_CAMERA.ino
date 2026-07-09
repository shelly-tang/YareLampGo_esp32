// SPDX-FileCopyrightText: 2026 @ninsmiracle, @shelly-tang, and LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "esp_camera.h"
#include <WiFi.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"

#include "avi_recorder.h"
#include "display_link.h"
#include "expression_clips.h"
#include "led_serial.h"
#include "net_config.h"
#include "provision_ap.h"
#include "work_mode.h"

// ===================
// Select camera model
// ===================
#define CAMERA_MODEL_XIAO_ESP32S3 // Has PSRAM
#include "camera_pins.h"

void startCameraServer();
void setupLedFlash(int pin);

static bool g_sdReady = false;

static bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      config.frame_size = FRAMESIZE_QVGA;
      config.jpeg_quality = 18;
      config.fb_count = 1;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s != nullptr) {
    if (s->id.PID == OV3660_PID) {
      s->set_vflip(s, 1);
      s->set_brightness(s, 1);
      s->set_saturation(s, -2);
    }
    if (config.pixel_format == PIXFORMAT_JPEG) {
      s->set_framesize(s, psramFound() ? FRAMESIZE_SVGA : FRAMESIZE_QVGA);
    }
  }

#if defined(LED_GPIO_NUM)
  setupLedFlash(LED_GPIO_NUM);
#endif

  return true;
}

static void initSd() {
  g_sdReady = SD.begin(21);
  if (g_sdReady) {
    Serial.println("SD Card Mounted.");
  } else {
    Serial.println("SD Card Mount Failed! Recording disabled.");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  Serial.printf("PSRAM found: %s\n", psramFound() ? "YES" : "NO");
  Serial.printf("PSRAM size : %u bytes\n", (unsigned)ESP.getPsramSize());
  Serial.printf("Free PSRAM : %u bytes\n", (unsigned)ESP.getFreePsram());
#ifdef BOARD_HAS_PSRAM
  Serial.println("BOARD_HAS_PSRAM is DEFINED -> face detect compiled IN");
#else
  Serial.println("BOARD_HAS_PSRAM NOT defined -> face detect compiled OUT!");
#endif

  NetConfig::begin();
  Serial.printf("Device suffix: %s, hostname: %s.local\n",
                NetConfig::deviceIdSuffix().c_str(),
                NetConfig::deviceHostname().c_str());

  DisplayLink::begin();
  ExpressionClips::begin();

  if (!NetConfig::hasCredentials()) {
    Serial.println("No WiFi credentials stored. Entering provisioning mode.");
    DisplayLink::sendStatus("provision", "Connect to Lampgo-Setup WiFi");
    ProvisionAP::start();
    return;
  }

  if (!WorkMode::connectNetwork()) {
    Serial.println("WiFi connect failed. Clearing credentials and entering provisioning mode.");
    DisplayLink::sendStatus("wifi_failed", "Entering setup mode");
    NetConfig::clearWifi();
    ProvisionAP::start();
    return;
  }

  bool cameraReady = initCamera();
  if (!cameraReady) {
    Serial.println("Camera init failed; continuing with mic/speaker only.");
  }
  initSd();

  if (!WorkMode::startServices()) {
    Serial.println("Work services failed. Halting.");
    DisplayLink::sendStatus("service_failed", "Work services failed");
    while (true) {
      DisplayLink::loop();
      delay(1000);
    }
  }

  LedSerial::begin();

  if (g_sdReady) {
    startRecording();
  }

  DisplayLink::sendStatus("ready", NetConfig::deviceHostname().c_str());
}

void loop() {
  DisplayLink::loop();

  if (ProvisionAP::isActive()) {
    ProvisionAP::loop();
  } else if (WorkMode::isActive()) {
    WorkMode::loop();
  } else {
    delay(1000);
  }
}
