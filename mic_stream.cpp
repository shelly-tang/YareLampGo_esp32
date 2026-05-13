#include "mic_stream.h"

#include <Arduino.h>
#include <ESP_SR.h>
#include <ESP_I2S.h>
#include <esp_afe_aec.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

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
#define ENABLE_MIC_AEC    1
#define MIN_INTERNAL_HEAP_AFTER_AEC 24576
#define AEC_INPUT_FORMAT  "MR"
#define AEC_FILTER_LENGTH 4

namespace {

I2SClass i2sMic;
bool g_micReady = false;
bool g_aecReady = false;
afe_aec_handle_t *g_aec = nullptr;
int g_aecFrameSamples = 0;
int16_t *g_aecInput = nullptr;
int16_t *g_aecOutput = nullptr;
int16_t *g_refScratch = nullptr;
size_t g_aecFillSamples = 0;

httpd_handle_t g_server = nullptr;

struct WsClient {
  int fd;
  bool active;
};
WsClient g_clients[MAX_WS_CLIENTS] = {};
SemaphoreHandle_t g_clientMutex = nullptr;

TaskHandle_t g_pushTask = nullptr;
volatile bool g_pushRunning = false;
volatile uint32_t g_bytesRead = 0;
volatile uint32_t g_framesSent = 0;

void addClient(int fd) {
  xSemaphoreTake(g_clientMutex, portMAX_DELAY);
  for (int i = 0; i < MAX_WS_CLIENTS; i++) {
    if (!g_clients[i].active) {
      g_clients[i].fd = fd;
      g_clients[i].active = true;
      Serial.printf("[mic_stream] client added fd=%d slot=%d\n", fd, i);
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

void destroyAec() {
  if (g_aec) {
    afe_aec_destroy(g_aec);
    g_aec = nullptr;
  }
  if (g_aecInput) {
    heap_caps_free(g_aecInput);
    g_aecInput = nullptr;
  }
  if (g_aecOutput) {
    heap_caps_free(g_aecOutput);
    g_aecOutput = nullptr;
  }
  if (g_refScratch) {
    heap_caps_free(g_refScratch);
    g_refScratch = nullptr;
  }
  g_aecFrameSamples = 0;
  g_aecFillSamples = 0;
  g_aecReady = false;
}

bool initAec() {
  if (g_aecReady) return true;

  g_aec = afe_aec_create(AEC_INPUT_FORMAT, AEC_FILTER_LENGTH, AFE_TYPE_VC, AFE_MODE_LOW_COST);
  if (!g_aec) {
    Serial.println("[mic_stream] AEC init FAILED, falling back to raw mic");
    return false;
  }

  g_aecFrameSamples = afe_aec_get_chunksize(g_aec);
  if (g_aecFrameSamples <= 0) {
    Serial.println("[mic_stream] AEC invalid frame size");
    destroyAec();
    return false;
  }

  g_aecInput = (int16_t *)heap_caps_aligned_calloc(
      16, g_aecFrameSamples * 2, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  g_aecOutput = (int16_t *)heap_caps_aligned_calloc(
      16, g_aecFrameSamples, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  g_refScratch = (int16_t *)heap_caps_aligned_calloc(
      16, CHUNK_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!g_refScratch) {
    g_refScratch = (int16_t *)heap_caps_aligned_calloc(
        16, CHUNK_SAMPLES, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }

  if (!g_aecInput || !g_aecOutput || !g_refScratch) {
    Serial.println("[mic_stream] AEC buffer allocation FAILED");
    destroyAec();
    return false;
  }

  g_aecFillSamples = 0;
  g_aecReady = true;
  SpeakerStream::clearReference();
  Serial.printf("[mic_stream] ESP-SR AEC ready: input=%s frame=%d samples filter=%d\n",
                AEC_INPUT_FORMAT, g_aecFrameSamples, AEC_FILTER_LENGTH);
  size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  Serial.printf("[mic_stream] internal heap after AEC: free=%u largest=%u\n",
                (unsigned)internalFree, (unsigned)internalLargest);
  if (internalFree < MIN_INTERNAL_HEAP_AFTER_AEC) {
    Serial.printf("[mic_stream] AEC disabled: internal heap below %u bytes\n",
                  (unsigned)MIN_INTERNAL_HEAP_AFTER_AEC);
    destroyAec();
    return false;
  }
  return true;
}

void processAndSendAudio(const uint8_t *payload, size_t len) {
  if (!g_aecReady || !g_aec || !g_aecInput || !g_aecOutput || !g_refScratch) {
    sendAudioFrame(payload, len);
    return;
  }

  size_t sampleCount = len / sizeof(int16_t);
  if (sampleCount == 0) return;
  if (sampleCount > CHUNK_SAMPLES) sampleCount = CHUNK_SAMPLES;

  const int16_t *mic = (const int16_t *)payload;
  SpeakerStream::readReference(g_refScratch, sampleCount);

  for (size_t i = 0; i < sampleCount; i++) {
    size_t idx = g_aecFillSamples * 2;
    g_aecInput[idx] = mic[i];
    g_aecInput[idx + 1] = g_refScratch[i];
    g_aecFillSamples++;

    if (g_aecFillSamples >= (size_t)g_aecFrameSamples) {
      size_t outBytes = afe_aec_process(g_aec, g_aecInput, g_aecOutput);
      if (outBytes > 0) {
        sendAudioFrame((const uint8_t *)g_aecOutput, outBytes);
      } else {
        for (int j = 0; j < g_aecFrameSamples; j++) {
          g_aecOutput[j] = g_aecInput[j * 2];
        }
        sendAudioFrame((const uint8_t *)g_aecOutput,
                       g_aecFrameSamples * sizeof(int16_t));
      }
      g_aecFillSamples = 0;
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
    if (activeClientCount() == 0) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    size_t bytesRead = i2sMic.readBytes((char *)buf, CHUNK_BYTES);
    if (bytesRead == 0) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    g_bytesRead += bytesRead;

    processAndSendAudio(buf, bytesRead);
  }

  Serial.println("[mic_stream] push task ended");
  heap_caps_free(buf);
  vTaskDelete(nullptr);
}

// WebSocket endpoint handler: /ws/audio
// GET (upgrade) → register client; CLOSE frame → unregister.
esp_err_t wsAudioHandler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    int fd = httpd_req_to_sockfd(req);
    Serial.printf("[mic_stream] WS handshake fd=%d\n", fd);
    addClient(fd);
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

  i2sMic.setPinsPdmRx(MIC_PDM_CLK_PIN, MIC_PDM_DATA_PIN);

  if (!i2sMic.begin(I2S_MODE_PDM_RX, MIC_SAMPLE_RATE,
                     I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("[mic_stream] I2S PDM init FAILED");
    return false;
  }

  g_micReady = true;
#if ENABLE_MIC_AEC
  initAec();
#else
  Serial.println("[mic_stream] AEC disabled, streaming raw mic");
#endif
  Serial.printf("[mic_stream] I2S PDM ready: %d Hz, mono, 16-bit\n",
                MIC_SAMPLE_RATE);
  return true;
}

void stop() {
  g_pushRunning = false;
  if (g_pushTask) {
    vTaskDelay(pdMS_TO_TICKS(200));
    g_pushTask = nullptr;
  }
  if (g_micReady) {
    i2sMic.end();
    g_micReady = false;
  }
  destroyAec();
}

bool isRunning() {
  return g_micReady && g_pushRunning;
}

bool isAecReady() {
  return g_aecReady;
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

  g_pushRunning = true;
  xTaskCreatePinnedToCore(pushTaskFn, "mic_push", 6144, nullptr, 5,
                          &g_pushTask, 1);

  Serial.println("[mic_stream] /ws/audio registered, push task launched");
  return true;
}

}  // namespace MicStream
