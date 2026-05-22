#include "mic_stream.h"

#include <Arduino.h>
#include "sdkconfig.h"
#include <Preferences.h>
#include <ESP_SR.h>
#include <ESP_I2S.h>
#include <esp_afe_config.h>
#include <esp_afe_sr_iface.h>
#include <esp_afe_sr_models.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <math.h>
#if (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4) && (CONFIG_MODEL_IN_FLASH || CONFIG_MODEL_IN_SDCARD)
#define LAMPGO_HAS_WAKE_WORD 1
#include <esp_wn_iface.h>
#include <esp_wn_models.h>
#include <model_path.h>
#else
#define LAMPGO_HAS_WAKE_WORD 0
#define MODEL_NAME_MAX_LENGTH 64
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <string.h>

#include "net_config.h"
#include "speaker_stream.h"

// XIAO ESP32S3 Sense PDM microphone pins (from Seeed Wiki)
#define MIC_PDM_CLK_PIN  42
#define MIC_PDM_DATA_PIN 41

// Audio parameters — must match lampgo backend (16kHz, mono, 16-bit PCM)
#define MIC_SAMPLE_RATE   16000

// WebSocket push: 30ms chunks = 480 samples × 2 bytes = 960 bytes per frame
#define CHUNK_DURATION_MS 30
#define CHUNK_SAMPLES     (MIC_SAMPLE_RATE * CHUNK_DURATION_MS / 1000)
#define CHUNK_BYTES       (CHUNK_SAMPLES * 2)

#define MAX_WS_CLIENTS    2
#define MAX_EVENT_CLIENTS 2
#define MIC_PUSH_TASK_STACK 6144
#define AFE_FETCH_TASK_STACK 8192
#define WAKE_COOLDOWN_MS 2500
#define WAKE_DETECTION_THRESHOLD 0.28f
#define WAKE_AFE_LINEAR_GAIN 2.0f
#define WAKE_DET_MODE DET_MODE_90
#define MIN_INTERNAL_HEAP_AFTER_AFE 16384
#define AFE_INPUT_FORMAT  "MR"
#define AFE_FILTER_LENGTH 4
#define DEFAULT_WAKE_MODEL "wn9_jarvis_tts"

namespace {

I2SClass i2sMic;
bool g_micReady = false;
bool g_afeReady = false;
const esp_afe_sr_iface_t *g_afeHandle = nullptr;
esp_afe_sr_data_t *g_afeData = nullptr;
afe_config_t *g_afeConfig = nullptr;
int g_afeFeedSamples = 0;
int g_afeFeedChannels = 0;
int16_t *g_afeInput = nullptr;
int16_t *g_refScratch = nullptr;
size_t g_afeFillSamples = 0;

httpd_handle_t g_server = nullptr;

struct WsClient {
  int fd;
  bool active;
  char owner[80];
};
WsClient g_clients[MAX_WS_CLIENTS] = {};
WsClient g_eventClients[MAX_EVENT_CLIENTS] = {};
SemaphoreHandle_t g_clientMutex = nullptr;
SemaphoreHandle_t g_eventClientMutex = nullptr;

#if LAMPGO_HAS_WAKE_WORD
srmodel_list_t *g_wakeModels = nullptr;
#endif
bool g_wakeReady = false;
char g_wakeModelName[MODEL_NAME_MAX_LENGTH] = "";
char g_requestedWakeModelName[MODEL_NAME_MAX_LENGTH] = DEFAULT_WAKE_MODEL;
volatile uint32_t g_wakeDetections = 0;
volatile uint32_t g_lastWakeMs = 0;
volatile uint32_t g_wakeSeq = 0;

TaskHandle_t g_pushTask = nullptr;
TaskHandle_t g_fetchTask = nullptr;
volatile bool g_pushRunning = false;
volatile uint32_t g_bytesRead = 0;
volatile uint32_t g_framesSent = 0;
volatile uint32_t g_afeFeeds = 0;
volatile uint32_t g_afeFetchAttempts = 0;
volatile uint32_t g_afeFetches = 0;
volatile uint32_t g_afeFetchNulls = 0;
volatile uint32_t g_lastAfeMs = 0;
volatile int g_lastAfeRet = 0;
volatile int g_lastWakeupState = 0;
volatile int g_lastWakeWordIndex = 0;
volatile int g_lastTriggerChannel = -1;
volatile int g_lastVadState = 0;
volatile float g_lastAfeVolumeDb = 0.0f;
volatile float g_lastAfeRingFreePct = 0.0f;
volatile uint32_t g_lastMicRms = 0;
volatile uint32_t g_lastMicPeak = 0;
volatile int g_pushTaskCreateResult = 0;
volatile int g_fetchTaskCreateResult = 0;
volatile bool g_inlineFetch = true;
char g_activeOwnerId[80] = "";
uint32_t g_ownerLeaseUntilMs = 0;

const char *kWakePrefsNamespace = "lampgo-audio";
const char *kWakeModelKey = "wake_model";

const char *kSupportedWakeModels[] = {
    "wn9_jarvis_tts",
    "wn9_xiaomeitongxue_tts",
    "wn9_xiaoyaxiaoya_tts2",
    "wn9_xiaoluxiaolu_tts2",
    "wn9_hixiaoxing_tts",
};

bool isSupportedWakeModel(const char *modelName) {
  if (!modelName || !modelName[0]) return false;
  for (const char *supported : kSupportedWakeModels) {
    if (strcmp(modelName, supported) == 0) return true;
  }
  return false;
}

void loadWakeModelPreference() {
  Preferences prefs;
  if (!prefs.begin(kWakePrefsNamespace, true)) return;
  String value = prefs.getString(kWakeModelKey, DEFAULT_WAKE_MODEL);
  prefs.end();
  if (isSupportedWakeModel(value.c_str())) {
    strlcpy(g_requestedWakeModelName, value.c_str(), sizeof(g_requestedWakeModelName));
  } else {
    strlcpy(g_requestedWakeModelName, DEFAULT_WAKE_MODEL, sizeof(g_requestedWakeModelName));
  }
}

bool saveWakeModelPreference(const char *modelName) {
  Preferences prefs;
  if (!prefs.begin(kWakePrefsNamespace, false)) return false;
  size_t written = prefs.putString(kWakeModelKey, modelName);
  prefs.end();
  return written > 0;
}

bool extractQueryValue(httpd_req_t *req, const char *key, char *out, size_t outLen) {
  if (!req || !key || !out || outLen == 0) return false;
  out[0] = 0;
  char query[256] = {};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
  return httpd_query_key_value(query, key, out, outLen) == ESP_OK && out[0] != 0;
}

bool authorizeWsRequest(httpd_req_t *req, char *ownerOut, size_t ownerOutLen, const char *path) {
  char owner[80] = {};
  char token[128] = {};
  bool hasOwner = extractQueryValue(req, "owner", owner, sizeof(owner));
  bool hasToken = extractQueryValue(req, "token", token, sizeof(token));
  if (!hasOwner || !hasToken) {
    Serial.printf("[mic_stream] WS reject path=%s reason=missing_owner_token\n", path);
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "missing owner token");
    return false;
  }
  if (!NetConfig::verifyPairing(String(owner), String(token))) {
    Serial.printf("[mic_stream] WS reject path=%s owner=%s reason=pairing_mismatch\n", path, owner);
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "pairing mismatch");
    return false;
  }
  strlcpy(ownerOut, owner, ownerOutLen);
  return true;
}

void addClient(int fd, const char *owner) {
  xSemaphoreTake(g_clientMutex, portMAX_DELAY);
  for (int i = 0; i < MAX_WS_CLIENTS; i++) {
    if (!g_clients[i].active) {
      g_clients[i].fd = fd;
      g_clients[i].active = true;
      strlcpy(g_clients[i].owner, owner ? owner : "", sizeof(g_clients[i].owner));
      Serial.printf("[mic_stream] client added fd=%d slot=%d owner=%s\n", fd, i, g_clients[i].owner);
      xSemaphoreGive(g_clientMutex);
      return;
    }
  }
  xSemaphoreGive(g_clientMutex);
  Serial.printf("[mic_stream] client rejected fd=%d (all slots full)\n", fd);
  httpd_sess_trigger_close(g_server, fd);
}

void removeClient(int fd) {
  xSemaphoreTake(g_clientMutex, portMAX_DELAY);
  for (int i = 0; i < MAX_WS_CLIENTS; i++) {
    if (g_clients[i].active && g_clients[i].fd == fd) {
      g_clients[i].active = false;
      g_clients[i].owner[0] = 0;
      Serial.printf("[mic_stream] client removed fd=%d\n", fd);
      break;
    }
  }
  xSemaphoreGive(g_clientMutex);
}

int activeClientCount() {
  int count = 0;
  xSemaphoreTake(g_clientMutex, portMAX_DELAY);
  for (int i = 0; i < MAX_WS_CLIENTS; i++) {
    if (g_clients[i].active) count++;
  }
  xSemaphoreGive(g_clientMutex);
  return count;
}

void addEventClient(int fd, const char *owner) {
  xSemaphoreTake(g_eventClientMutex, portMAX_DELAY);
  for (int i = 0; i < MAX_EVENT_CLIENTS; i++) {
    if (!g_eventClients[i].active) {
      g_eventClients[i].fd = fd;
      g_eventClients[i].active = true;
      strlcpy(g_eventClients[i].owner, owner ? owner : "", sizeof(g_eventClients[i].owner));
      Serial.printf("[mic_stream] event client added fd=%d slot=%d owner=%s\n", fd, i, g_eventClients[i].owner);
      xSemaphoreGive(g_eventClientMutex);
      return;
    }
  }
  xSemaphoreGive(g_eventClientMutex);
  Serial.printf("[mic_stream] event client rejected fd=%d (all slots full)\n", fd);
  httpd_sess_trigger_close(g_server, fd);
}

void removeEventClient(int fd) {
  xSemaphoreTake(g_eventClientMutex, portMAX_DELAY);
  for (int i = 0; i < MAX_EVENT_CLIENTS; i++) {
    if (g_eventClients[i].active && g_eventClients[i].fd == fd) {
      g_eventClients[i].active = false;
      g_eventClients[i].owner[0] = 0;
      Serial.printf("[mic_stream] event client removed fd=%d\n", fd);
      break;
    }
  }
  xSemaphoreGive(g_eventClientMutex);
}

int activeEventClientCount() {
  int count = 0;
  xSemaphoreTake(g_eventClientMutex, portMAX_DELAY);
  for (int i = 0; i < MAX_EVENT_CLIENTS; i++) {
    if (g_eventClients[i].active) count++;
  }
  xSemaphoreGive(g_eventClientMutex);
  return count;
}

void sendWakeEvent() {
  g_wakeDetections++;
  g_lastWakeMs = millis();
  g_wakeSeq++;
  Serial.printf("[mic_stream] wake word detected model=%s seq=%u\n",
                g_wakeModelName[0] ? g_wakeModelName : "unknown", (unsigned)g_wakeSeq);

  if (!g_server || activeEventClientCount() == 0) return;

  char json[192];
  snprintf(json, sizeof(json),
           "{\"type\":\"wake_word_detected\",\"model\":\"%s\",\"seq\":%u,\"uptime_ms\":%u}",
           g_wakeModelName[0] ? g_wakeModelName : "unknown",
           (unsigned)g_wakeSeq, (unsigned)millis());

  int fds[MAX_EVENT_CLIENTS] = {};
  int fdCount = 0;
  xSemaphoreTake(g_eventClientMutex, portMAX_DELAY);
  for (int i = 0; i < MAX_EVENT_CLIENTS; i++) {
    if (!g_eventClients[i].active) continue;
    fds[fdCount++] = g_eventClients[i].fd;
  }
  xSemaphoreGive(g_eventClientMutex);

  for (int i = 0; i < fdCount; i++) {
    if (httpd_ws_get_fd_info(g_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) {
      removeEventClient(fds[i]);
      continue;
    }
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)json;
    frame.len = strlen(json);
    esp_err_t ret = httpd_ws_send_data(g_server, fds[i], &frame);
    if (ret != ESP_OK) {
      Serial.printf("[mic_stream] event send failed fd=%d err=%d, removing\n", fds[i], ret);
      removeEventClient(fds[i]);
    }
  }
}

void sendAudioFrame(const uint8_t *payload, size_t len) {
  if (!payload || len == 0 || activeClientCount() == 0 || !g_server) return;

  int fds[MAX_WS_CLIENTS] = {};
  int fdCount = 0;

  xSemaphoreTake(g_clientMutex, portMAX_DELAY);
  for (int i = 0; i < MAX_WS_CLIENTS; i++) {
    if (!g_clients[i].active) continue;
    fds[fdCount++] = g_clients[i].fd;
  }
  xSemaphoreGive(g_clientMutex);

  for (int i = 0; i < fdCount; i++) {
    if (httpd_ws_get_fd_info(g_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) {
      removeClient(fds[i]);
      continue;
    }

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = (uint8_t *)payload;
    frame.len = len;

    esp_err_t ret = httpd_ws_send_data(g_server, fds[i], &frame);
    if (ret != ESP_OK) {
      Serial.printf("[mic_stream] send failed fd=%d err=%d, removing\n", fds[i], ret);
      removeClient(fds[i]);
    } else {
      g_framesSent++;
    }
  }
}

void destroyWakeWord() {
#if LAMPGO_HAS_WAKE_WORD
  if (g_afeHandle && g_afeData) {
    g_afeHandle->destroy(g_afeData);
  }
  g_afeData = nullptr;
  g_afeHandle = nullptr;
  if (g_afeConfig) {
    afe_config_free(g_afeConfig);
    g_afeConfig = nullptr;
  }
  if (g_afeInput) {
    heap_caps_free(g_afeInput);
    g_afeInput = nullptr;
  }
  if (g_refScratch) {
    heap_caps_free(g_refScratch);
    g_refScratch = nullptr;
  }
  if (g_wakeModels) {
    esp_srmodel_deinit(g_wakeModels);
    g_wakeModels = nullptr;
  }
  g_afeFeedSamples = 0;
  g_afeFeedChannels = 0;
  g_afeFillSamples = 0;
#endif
  g_afeReady = false;
  g_wakeReady = false;
  g_wakeModelName[0] = 0;
}

bool initWakeWord() {
#if !LAMPGO_HAS_WAKE_WORD
  Serial.println("[mic_stream] WakeNet disabled: ESP-SR model support is not enabled in this build");
  return false;
#else
#if !defined(ARDUINO_PARTITION_esp_sr_32) && !defined(ARDUINO_PARTITION_esp_sr_16) && !defined(ARDUINO_PARTITION_esp_sr_8) && !defined(ARDUINO_PARTITION_lampgo_sr_8mb)
  Serial.println("[mic_stream] WakeNet warning: current partition menu may not flash an ESP-SR model partition");
#endif
  if (g_wakeReady) return true;

  g_wakeModels = esp_srmodel_init("model");
  if (!g_wakeModels) {
    Serial.println("[mic_stream] WakeNet init FAILED: no model partition/list");
    return false;
  }

  char *modelName = nullptr;
  if (g_requestedWakeModelName[0] &&
      esp_srmodel_exists(g_wakeModels, g_requestedWakeModelName) >= 0) {
    modelName = g_requestedWakeModelName;
  } else if (g_requestedWakeModelName[0]) {
    Serial.printf("[mic_stream] WakeNet init FAILED: requested model not found: %s\n",
                  g_requestedWakeModelName);
    destroyWakeWord();
    return false;
  }
  if (!modelName) {
    Serial.println("[mic_stream] WakeNet init FAILED: no requested WakeNet model");
    destroyWakeWord();
    return false;
  }

  g_afeConfig = afe_config_init(AFE_INPUT_FORMAT, g_wakeModels, AFE_TYPE_VC, AFE_MODE_LOW_COST);
  if (!g_afeConfig) {
    Serial.println("[mic_stream] AFE WakeNet init FAILED: config allocation");
    destroyWakeWord();
    return false;
  }
  g_afeConfig->aec_init = true;
  g_afeConfig->aec_filter_length = AFE_FILTER_LENGTH;
  g_afeConfig->se_init = false;
  g_afeConfig->ns_init = false;
  g_afeConfig->vad_init = false;
  g_afeConfig->wakenet_init = true;
  g_afeConfig->wakenet_model_name = modelName;
  g_afeConfig->wakenet_mode = WAKE_DET_MODE;
  g_afeConfig->agc_init = true;
  g_afeConfig->agc_mode = AFE_AGC_MODE_WAKENET;
  g_afeConfig->afe_linear_gain = WAKE_AFE_LINEAR_GAIN;
  g_afeConfig->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
  g_afeConfig->fixed_output_channel = true;

  g_afeConfig = afe_config_check(g_afeConfig);
  g_afeHandle = esp_afe_handle_from_config(g_afeConfig);
  if (!g_afeHandle) {
    Serial.println("[mic_stream] AFE WakeNet init FAILED: no AFE handle");
    destroyWakeWord();
    return false;
  }
  g_afeData = g_afeHandle->create_from_config(g_afeConfig);
  if (!g_afeData) {
    Serial.printf("[mic_stream] AFE WakeNet init FAILED: create %s\n", modelName);
    destroyWakeWord();
    return false;
  }
  if (g_afeHandle->set_wakenet_threshold) {
    g_afeHandle->set_wakenet_threshold(g_afeData, 1, WAKE_DETECTION_THRESHOLD);
  }

  int rate = g_afeHandle->get_samp_rate(g_afeData);
  g_afeFeedSamples = g_afeHandle->get_feed_chunksize(g_afeData);
  g_afeFeedChannels = g_afeHandle->get_feed_channel_num(g_afeData);
  if (rate != MIC_SAMPLE_RATE || g_afeFeedSamples <= 0 || g_afeFeedChannels <= 0) {
    Serial.printf("[mic_stream] AFE unsupported format: rate=%d feed=%d channels=%d\n",
                  rate, g_afeFeedSamples, g_afeFeedChannels);
    destroyWakeWord();
    return false;
  }

  g_afeInput = (int16_t *)heap_caps_aligned_calloc(
      16, g_afeFeedSamples * g_afeFeedChannels, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  g_refScratch = (int16_t *)heap_caps_aligned_calloc(
      16, CHUNK_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!g_refScratch) {
    g_refScratch = (int16_t *)heap_caps_aligned_calloc(
        16, CHUNK_SAMPLES, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!g_afeInput || !g_refScratch) {
    Serial.println("[mic_stream] AFE buffer allocation FAILED");
    destroyWakeWord();
    return false;
  }

  strlcpy(g_wakeModelName, modelName, sizeof(g_wakeModelName));
  g_afeFillSamples = 0;
  g_afeReady = true;
  g_wakeReady = true;
  SpeakerStream::clearReference();
  Serial.printf("[mic_stream] ESP-SR AFE WakeNet ready: model=%s feed=%d samples channels=%d threshold=%.2f gain=%.1f mode=%d input=%s\n",
                g_wakeModelName, g_afeFeedSamples, g_afeFeedChannels,
                WAKE_DETECTION_THRESHOLD, WAKE_AFE_LINEAR_GAIN, (int)WAKE_DET_MODE, AFE_INPUT_FORMAT);
  if (g_afeHandle->print_pipeline) {
    g_afeHandle->print_pipeline(g_afeData);
  }
  size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  Serial.printf("[mic_stream] internal heap after AFE: free=%u largest=%u\n",
                (unsigned)internalFree, (unsigned)internalLargest);
  if (internalFree < MIN_INTERNAL_HEAP_AFTER_AFE) {
    Serial.printf("[mic_stream] AFE warning: internal heap below %u bytes\n",
                  (unsigned)MIN_INTERNAL_HEAP_AFTER_AFE);
  }
  return true;
#endif
}

void handleAfeResult(afe_fetch_result_t *res) {
  if (!res) {
    g_afeFetchNulls++;
    return;
  }
  g_lastAfeRet = res->ret_value;
  if (res->ret_value != ESP_OK) return;
  g_afeFetches++;
  g_lastAfeMs = millis();
  g_lastWakeupState = (int)res->wakeup_state;
  g_lastWakeWordIndex = res->wake_word_index;
  g_lastTriggerChannel = res->trigger_channel_id;
  g_lastVadState = (int)res->vad_state;
  g_lastAfeVolumeDb = res->data_volume;
  g_lastAfeRingFreePct = res->ringbuff_free_pct;
  if (res->data && res->data_size > 0) {
    sendAudioFrame((const uint8_t *)res->data, res->data_size);
  }
  if (res->wakeup_state == WAKENET_DETECTED) {
    uint32_t now = millis();
    if (now - g_lastWakeMs >= WAKE_COOLDOWN_MS) {
      sendWakeEvent();
    }
    if (g_afeHandle && g_afeHandle->reset_buffer && g_afeData) {
      g_afeHandle->reset_buffer(g_afeData);
    }
  }
}

void processInputAudio(const uint8_t *payload, size_t len) {
  size_t sampleCount = len / sizeof(int16_t);
  if (sampleCount == 0) return;
  if (sampleCount > CHUNK_SAMPLES) sampleCount = CHUNK_SAMPLES;

  const int16_t *mic = (const int16_t *)payload;
  int64_t sumSq = 0;
  uint32_t peak = 0;
  for (size_t i = 0; i < sampleCount; i++) {
    int32_t v = mic[i];
    uint32_t absV = (uint32_t)(v < 0 ? -v : v);
    if (absV > peak) peak = absV;
    sumSq += (int64_t)v * (int64_t)v;
  }
  g_lastMicPeak = peak;
  g_lastMicRms = sampleCount > 0 ? (uint32_t)sqrt((double)sumSq / (double)sampleCount) : 0;

  if (!g_afeReady || !g_afeHandle || !g_afeData || !g_afeInput || !g_refScratch) {
    sendAudioFrame(payload, sampleCount * sizeof(int16_t));
    return;
  }

  SpeakerStream::readReference(g_refScratch, sampleCount);

  for (size_t i = 0; i < sampleCount; i++) {
    size_t idx = g_afeFillSamples * g_afeFeedChannels;
    g_afeInput[idx] = mic[i];
    if (g_afeFeedChannels > 1) {
      g_afeInput[idx + 1] = g_refScratch[i];
    }
    for (int ch = 2; ch < g_afeFeedChannels; ch++) {
      g_afeInput[idx + ch] = 0;
    }
    g_afeFillSamples++;

    if (g_afeFillSamples >= (size_t)g_afeFeedSamples) {
      g_afeHandle->feed(g_afeData, g_afeInput);
      g_afeFeeds++;
      g_afeFillSamples = 0;
      if (g_inlineFetch) {
        g_afeFetchAttempts++;
        afe_fetch_result_t *res = g_afeHandle->fetch_with_delay(g_afeData, pdMS_TO_TICKS(5));
        handleAfeResult(res);
      }
    }
  }
}

// Dedicated FreeRTOS task: reads I2S PDM data in 30ms chunks and pushes
// binary WebSocket frames to all connected clients.
void pushTaskFn(void *) {
  uint8_t *buf = (uint8_t *)heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!buf) {
    Serial.println("[mic_stream] push buffer allocation FAILED");
    g_pushRunning = false;
    g_pushTask = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  Serial.println("[mic_stream] push task started");

  while (g_pushRunning) {
    if (activeClientCount() == 0 && !g_wakeReady) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    size_t bytesRead = i2sMic.readBytes((char *)buf, CHUNK_BYTES);
    if (bytesRead == 0) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    g_bytesRead += bytesRead;

    processInputAudio(buf, bytesRead);
  }

  Serial.println("[mic_stream] push task ended");
  heap_caps_free(buf);
  vTaskDelete(nullptr);
}

void fetchTaskFn(void *) {
  Serial.println("[mic_stream] AFE fetch task started");

  while (g_pushRunning) {
    if (!g_afeReady || !g_afeHandle || !g_afeData) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    g_afeFetchAttempts++;
    afe_fetch_result_t *res = g_afeHandle->fetch_with_delay(g_afeData, pdMS_TO_TICKS(50));
    handleAfeResult(res);
  }

  Serial.println("[mic_stream] AFE fetch task ended");
  vTaskDeleteWithCaps(nullptr);
}

// WebSocket endpoint handler: /ws/events
// GET (upgrade) registers event client; CLOSE frame unregisters.
esp_err_t wsEventsHandler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    int fd = httpd_req_to_sockfd(req);
    char owner[80] = {};
    if (!authorizeWsRequest(req, owner, sizeof(owner), "/ws/events")) {
      return ESP_FAIL;
    }
    Serial.printf("[mic_stream] WS events handshake fd=%d owner=%s\n", fd, owner);
    addEventClient(fd, owner);
    return ESP_OK;
  }

  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_TEXT;
  esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
  if (ret != ESP_OK) {
    return ret;
  }

  if (frame.type == HTTPD_WS_TYPE_CLOSE) {
    int fd = httpd_req_to_sockfd(req);
    removeEventClient(fd);
  }

  return ESP_OK;
}

esp_err_t wsAudioHandler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    int fd = httpd_req_to_sockfd(req);
    char owner[80] = {};
    if (!authorizeWsRequest(req, owner, sizeof(owner), "/ws/audio")) {
      return ESP_FAIL;
    }
    Serial.printf("[mic_stream] WS handshake fd=%d owner=%s\n", fd, owner);
    addClient(fd, owner);
    return ESP_OK;
  }

  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_BINARY;
  esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
  if (ret != ESP_OK) {
    return ret;
  }

  if (frame.type == HTTPD_WS_TYPE_CLOSE) {
    int fd = httpd_req_to_sockfd(req);
    removeClient(fd);
  }

  return ESP_OK;
}

}  // namespace

namespace MicStream {

bool begin() {
  if (g_micReady) return true;

  if (!g_clientMutex) {
    g_clientMutex = xSemaphoreCreateMutex();
  }
  if (!g_eventClientMutex) {
    g_eventClientMutex = xSemaphoreCreateMutex();
  }
  loadWakeModelPreference();

  i2sMic.setPinsPdmRx(MIC_PDM_CLK_PIN, MIC_PDM_DATA_PIN);

  if (!i2sMic.begin(I2S_MODE_PDM_RX, MIC_SAMPLE_RATE,
                     I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("[mic_stream] I2S PDM init FAILED");
    return false;
  }

  g_micReady = true;
  initWakeWord();
  Serial.printf("[mic_stream] I2S PDM ready: %d Hz, mono, 16-bit\n",
                MIC_SAMPLE_RATE);
  return true;
}

bool isRunning() {
  return g_micReady && g_pushRunning;
}

bool isAecReady() {
  return g_afeReady;
}

int clientCount() {
  if (!g_clientMutex) return 0;
  return activeClientCount();
}

uint32_t bytesRead() {
  return g_bytesRead;
}

uint32_t framesSent() {
  return g_framesSent;
}

bool claimOwner(const char *ownerId, uint32_t ttlMs) {
  if (!ownerId || !ownerId[0]) return false;
  strlcpy(g_activeOwnerId, ownerId, sizeof(g_activeOwnerId));
  g_ownerLeaseUntilMs = millis() + (ttlMs ? ttlMs : 120000);
  Serial.printf("[mic_stream] owner claimed owner=%s ttl_ms=%u\n", g_activeOwnerId, (unsigned)(ttlMs ? ttlMs : 120000));
  return true;
}

bool releaseOwner(const char *ownerId) {
  if (ownerId && ownerId[0] && g_activeOwnerId[0] && strcmp(ownerId, g_activeOwnerId) != 0) {
    Serial.printf("[mic_stream] owner release ignored owner=%s active=%s\n", ownerId, g_activeOwnerId);
    return false;
  }
  g_activeOwnerId[0] = 0;
  g_ownerLeaseUntilMs = 0;
  Serial.println("[mic_stream] owner released");
  return true;
}

void closeClients() {
  int fds[MAX_WS_CLIENTS + MAX_EVENT_CLIENTS] = {};
  int fdCount = 0;
  if (g_clientMutex) {
    xSemaphoreTake(g_clientMutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
      if (!g_clients[i].active) continue;
      fds[fdCount++] = g_clients[i].fd;
      g_clients[i].active = false;
      g_clients[i].owner[0] = 0;
    }
    xSemaphoreGive(g_clientMutex);
  }
  if (g_eventClientMutex) {
    xSemaphoreTake(g_eventClientMutex, portMAX_DELAY);
    for (int i = 0; i < MAX_EVENT_CLIENTS; i++) {
      if (!g_eventClients[i].active) continue;
      fds[fdCount++] = g_eventClients[i].fd;
      g_eventClients[i].active = false;
      g_eventClients[i].owner[0] = 0;
    }
    xSemaphoreGive(g_eventClientMutex);
  }
  for (int i = 0; i < fdCount; i++) {
    if (g_server) httpd_sess_trigger_close(g_server, fds[i]);
  }
  SpeakerStream::closeClients();
  Serial.printf("[mic_stream] closed %d owner-bound clients\n", fdCount);
}

const char *activeOwner() {
  if (g_activeOwnerId[0] && g_ownerLeaseUntilMs && (int32_t)(g_ownerLeaseUntilMs - millis()) <= 0) {
    g_activeOwnerId[0] = 0;
    g_ownerLeaseUntilMs = 0;
  }
  return g_activeOwnerId;
}

uint32_t ownerLeaseRemainingMs() {
  if (!activeOwner()[0] || !g_ownerLeaseUntilMs) return 0;
  return (uint32_t)(g_ownerLeaseUntilMs - millis());
}

bool isWakeReady() {
  return g_wakeReady;
}

const char *wakeModel() {
  return g_wakeModelName;
}

const char *requestedWakeModel() {
  return g_requestedWakeModelName;
}

bool setWakeModel(const char *modelName) {
  if (!isSupportedWakeModel(modelName)) {
    Serial.printf("[mic_stream] rejected unsupported wake model: %s\n",
                  modelName ? modelName : "(null)");
    return false;
  }

  if (strcmp(g_requestedWakeModelName, modelName) == 0 &&
      strcmp(g_wakeModelName, modelName) == 0 &&
      g_wakeReady) {
    return true;
  }

  strlcpy(g_requestedWakeModelName, modelName, sizeof(g_requestedWakeModelName));
  saveWakeModelPreference(g_requestedWakeModelName);

  if (!g_micReady) {
    return true;
  }

  Serial.printf("[mic_stream] switching WakeNet model to %s\n", g_requestedWakeModelName);
  g_afeReady = false;
  g_wakeReady = false;
  vTaskDelay(pdMS_TO_TICKS(120));
  destroyWakeWord();
  bool ok = initWakeWord();
  return ok;
}

uint32_t wakeDetections() {
  return g_wakeDetections;
}

uint32_t lastWakeMs() {
  return g_lastWakeMs;
}

int wakeEventClientCount() {
  if (!g_eventClientMutex) return 0;
  return activeEventClientCount();
}

float wakeThreshold() {
  return WAKE_DETECTION_THRESHOLD;
}

float wakeAfeGain() {
  return WAKE_AFE_LINEAR_GAIN;
}

int wakeDetectionMode() {
  return (int)WAKE_DET_MODE;
}

uint32_t afeFetches() {
  return g_afeFetches;
}

uint32_t lastAfeMs() {
  return g_lastAfeMs;
}

int lastWakeupState() {
  return g_lastWakeupState;
}

int lastWakeWordIndex() {
  return g_lastWakeWordIndex;
}

int lastTriggerChannel() {
  return g_lastTriggerChannel;
}

int lastVadState() {
  return g_lastVadState;
}

float lastAfeVolumeDb() {
  return g_lastAfeVolumeDb;
}

float lastAfeRingFreePct() {
  return g_lastAfeRingFreePct;
}

uint32_t lastMicRms() {
  return g_lastMicRms;
}

uint32_t lastMicPeak() {
  return g_lastMicPeak;
}

uint32_t afeFeeds() {
  return g_afeFeeds;
}

uint32_t afeFetchAttempts() {
  return g_afeFetchAttempts;
}

uint32_t afeFetchNulls() {
  return g_afeFetchNulls;
}

int lastAfeRet() {
  return g_lastAfeRet;
}

int afeFeedSamples() {
  return g_afeFeedSamples;
}

int afeFeedChannels() {
  return g_afeFeedChannels;
}

int pushTaskCreateResult() {
  return g_pushTaskCreateResult;
}

int fetchTaskCreateResult() {
  return g_fetchTaskCreateResult;
}

bool isInlineFetch() {
  return g_inlineFetch;
}

bool registerWsHandler(httpd_handle_t server) {
  if (!server) return false;
  g_server = server;

  httpd_uri_t wsUri = {
    .uri = "/ws/audio",
    .method = HTTP_GET,
    .handler = wsAudioHandler,
    .user_ctx = nullptr,
    .is_websocket = true,
    .handle_ws_control_frames = true,
    .supported_subprotocol = nullptr,
  };

  esp_err_t err = httpd_register_uri_handler(server, &wsUri);
  if (err != ESP_OK) {
    Serial.printf("[mic_stream] register /ws/audio failed: %d\n", err);
    return false;
  }

  httpd_uri_t eventsUri = {
    .uri = "/ws/events",
    .method = HTTP_GET,
    .handler = wsEventsHandler,
    .user_ctx = nullptr,
    .is_websocket = true,
    .handle_ws_control_frames = true,
    .supported_subprotocol = nullptr,
  };

  err = httpd_register_uri_handler(server, &eventsUri);
  if (err != ESP_OK) {
    Serial.printf("[mic_stream] register /ws/events failed: %d\n", err);
    return false;
  }

  g_pushRunning = true;
  BaseType_t pushOk = xTaskCreatePinnedToCore(
      pushTaskFn, "mic_push", MIC_PUSH_TASK_STACK, nullptr, 3, &g_pushTask, 1);
  g_pushTaskCreateResult = (int)pushOk;
  if (pushOk != pdPASS) {
    Serial.printf("[mic_stream] mic_push task create FAILED: %d\n", (int)pushOk);
    g_pushRunning = false;
    return false;
  }

  BaseType_t fetchOk = xTaskCreatePinnedToCoreWithCaps(
      fetchTaskFn, "afe_fetch", AFE_FETCH_TASK_STACK, nullptr, 3, &g_fetchTask, 1,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  g_fetchTaskCreateResult = (int)fetchOk;
  g_inlineFetch = fetchOk != pdPASS;
  if (fetchOk != pdPASS) {
    Serial.printf("[mic_stream] afe_fetch task create FAILED: %d, using inline fetch\n", (int)fetchOk);
  } else {
    Serial.println("[mic_stream] afe_fetch task launched on PSRAM stack");
  }

  Serial.println("[mic_stream] /ws/audio and /ws/events registered");
  return true;
}

}  // namespace MicStream
