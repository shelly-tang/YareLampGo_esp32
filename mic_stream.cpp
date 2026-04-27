#include "mic_stream.h"

#include <Arduino.h>
#include <ESP_I2S.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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

namespace {

I2SClass i2sMic;
bool g_micReady = false;

httpd_handle_t g_server = nullptr;

struct WsClient {
  int fd;
  bool active;
};
WsClient g_clients[MAX_WS_CLIENTS] = {};
SemaphoreHandle_t g_clientMutex = nullptr;

TaskHandle_t g_pushTask = nullptr;
volatile bool g_pushRunning = false;

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

// Dedicated FreeRTOS task: reads I2S PDM data in 30ms chunks and pushes
// binary WebSocket frames to all connected clients.
void pushTaskFn(void *) {
  uint8_t buf[CHUNK_BYTES];
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

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = buf;
    frame.len = bytesRead;

    xSemaphoreTake(g_clientMutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
      if (!g_clients[i].active) continue;
      esp_err_t ret = httpd_ws_send_frame_async(g_server, g_clients[i].fd, &frame);
      if (ret != ESP_OK) {
        Serial.printf("[mic_stream] send failed fd=%d err=%d, removing\n",
                      g_clients[i].fd, ret);
        g_clients[i].active = false;
      }
    }
    xSemaphoreGive(g_clientMutex);
  }

  Serial.println("[mic_stream] push task ended");
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
}

bool isRunning() {
  return g_micReady && g_pushRunning;
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
  xTaskCreatePinnedToCore(pushTaskFn, "mic_push", 4096, nullptr, 5,
                          &g_pushTask, 1);

  Serial.println("[mic_stream] /ws/audio registered, push task launched");
  return true;
}

}  // namespace MicStream
