#include "speaker_stream.h"

#include <Arduino.h>
#include <Preferences.h>
#include <ESP_I2S.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

#include "net_config.h"

// MAX98357A I2S pins (Seeed XIAO ESP32S3 pin labels from user's wiring)
#define SPK_I2S_BCLK_PIN 1  // D0 -> GPIO1
#define SPK_I2S_LRC_PIN  2  // D1 -> GPIO2
#define SPK_I2S_DOUT_PIN 4  // D3 -> GPIO4

// Input from browser is 16 kHz mono PCM16LE (matches mic / AEC reference rate).
#define SPK_INPUT_SAMPLE_RATE 16000
// I2S output runs at 48 kHz so the BCLK (~1.5 MHz) sits comfortably above
// MAX98357A's minimum stable BCLK; low BCLKs make the chip output continuous
// hiss even when fed silence.
#define SPK_OUTPUT_SAMPLE_RATE 48000
#define SPK_UPSAMPLE_FACTOR (SPK_OUTPUT_SAMPLE_RATE / SPK_INPUT_SAMPLE_RATE)  // 3
#define SPK_MAX_FRAME_BYTES 4096
#define SPK_QUEUE_DEPTH 12
#define SPK_DEFAULT_VOLUME 0.7f
// AEC reference ring keeps 16 kHz samples (same rate as mic).
// Keep one second in PSRAM; older reference audio is not useful for AEC and
// costs scarce internal SRAM.
#define REF_RING_SAMPLES SPK_INPUT_SAMPLE_RATE

namespace {

I2SClass i2sSpeaker;
bool g_speakerReady = false;
bool g_handlerRegistered = false;
QueueHandle_t g_audioQueue = nullptr;
TaskHandle_t g_playTask = nullptr;
volatile bool g_playRunning = false;
SemaphoreHandle_t g_refMutex = nullptr;
httpd_handle_t g_server = nullptr;
int g_clientFd = -1;
int16_t *g_refRing = nullptr;
size_t g_refRead = 0;
size_t g_refWrite = 0;
size_t g_refCount = 0;
float g_volume = SPK_DEFAULT_VOLUME;

const char *kSpeakerPrefsNamespace = "lampgo-audio";
const char *kSpeakerVolumeKey = "speaker_volume";

struct AudioPacket {
  uint8_t *data;
  size_t len;
};

void playTaskFn(void *);

bool extractQueryValue(httpd_req_t *req, const char *key, char *out, size_t outLen) {
  if (!req || !key || !out || outLen == 0) return false;
  out[0] = 0;
  char query[256] = {};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
  return httpd_query_key_value(query, key, out, outLen) == ESP_OK && out[0] != 0;
}

bool authorizeWsRequest(httpd_req_t *req, char *ownerOut, size_t ownerOutLen) {
  char owner[80] = {};
  char token[128] = {};
  bool hasOwner = extractQueryValue(req, "owner", owner, sizeof(owner));
  bool hasToken = extractQueryValue(req, "token", token, sizeof(token));
  if (!hasOwner || !hasToken) {
    Serial.println("[speaker_stream] WS reject reason=missing_owner_token");
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "missing owner token");
    return false;
  }
  if (!NetConfig::verifyPairing(String(owner), String(token))) {
    Serial.printf("[speaker_stream] WS reject owner=%s reason=pairing_mismatch\n", owner);
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "pairing mismatch");
    return false;
  }
  strlcpy(ownerOut, owner, ownerOutLen);
  return true;
}

float clampVolume(float volume) {
  if (volume < 0.0f) return 0.0f;
  if (volume > 1.0f) return 1.0f;
  return volume;
}

void loadVolumePreference() {
  Preferences prefs;
  if (!prefs.begin(kSpeakerPrefsNamespace, true)) {
    g_volume = SPK_DEFAULT_VOLUME;
    return;
  }
  g_volume = clampVolume(prefs.getFloat(kSpeakerVolumeKey, SPK_DEFAULT_VOLUME));
  prefs.end();
  Serial.printf("[speaker_stream] loaded volume=%.2f\n", g_volume);
}

void saveVolumePreference(float volume) {
  Preferences prefs;
  if (!prefs.begin(kSpeakerPrefsNamespace, false)) return;
  prefs.putFloat(kSpeakerVolumeKey, clampVolume(volume));
  prefs.end();
}

bool ensurePlayTaskRunning() {
  if (g_playRunning) return true;
  if (!g_audioQueue || !g_speakerReady) return false;

  g_playRunning = true;
  BaseType_t taskOk = xTaskCreatePinnedToCore(playTaskFn, "speaker_play", 4096,
                                              nullptr, 5, &g_playTask, 1);
  if (taskOk != pdPASS) {
    g_playRunning = false;
    g_playTask = nullptr;
    Serial.println("[speaker_stream] play task create FAILED");
    return false;
  }
  Serial.println("[speaker_stream] play task launched");
  return true;
}

void freePacket(AudioPacket &pkt) {
  if (pkt.data) {
    free(pkt.data);
    pkt.data = nullptr;
  }
  pkt.len = 0;
}

void pushReference(const uint8_t *payload, size_t len) {
  if (!g_refMutex || !g_refRing || !payload || len < 2) return;
  const int16_t *samples = (const int16_t *)payload;
  size_t sampleCount = len / sizeof(int16_t);

  xSemaphoreTake(g_refMutex, portMAX_DELAY);
  for (size_t i = 0; i < sampleCount; i++) {
    g_refRing[g_refWrite] = samples[i];
    g_refWrite = (g_refWrite + 1) % REF_RING_SAMPLES;
    if (g_refCount < REF_RING_SAMPLES) {
      g_refCount++;
    } else {
      g_refRead = (g_refRead + 1) % REF_RING_SAMPLES;
    }
  }
  xSemaphoreGive(g_refMutex);
}

void applyVolume(uint8_t *payload, size_t len) {
  if (!payload || len < 2) return;
  float volume = g_volume;
  if (volume < 0.0f) volume = 0.0f;
  if (volume > 1.0f) volume = 1.0f;
  if (volume >= 0.999f) return;

  int16_t *samples = (int16_t *)payload;
  size_t sampleCount = len / sizeof(int16_t);
  for (size_t i = 0; i < sampleCount; i++) {
    int32_t v = (int32_t)((float)samples[i] * volume);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    samples[i] = (int16_t)v;
  }
}

// Zero-order hold upsample: each 16 kHz sample becomes N consecutive 48 kHz
// samples. Cheap on CPU and good enough for speech; the spectral image at the
// input Nyquist is well above MAX98357A's class-D output filter cutoff.
size_t upsample16to48(const int16_t *in, size_t inSamples, int16_t *out) {
  size_t j = 0;
  for (size_t i = 0; i < inSamples; i++) {
    int16_t s = in[i];
    for (size_t k = 0; k < SPK_UPSAMPLE_FACTOR; k++) {
      out[j++] = s;
    }
  }
  return j;
}

void playTaskFn(void *) {
  Serial.println("[speaker_stream] play task started");

  // Keep DIN driven with zeros whenever there's no payload. MAX98357A treats
  // an undriven/floating data line as random bits and produces audible hiss
  // even with no signal — feeding silence frames at the I2S rate avoids that.
  static const size_t SILENCE_SAMPLES_48K = 768;  // 16 ms @ 48 kHz mono
  static int16_t silence48k[SILENCE_SAMPLES_48K] = {0};

  // Scratch buffer big enough for any incoming frame upsampled 3x.
  static int16_t upsampleScratch[SPK_MAX_FRAME_BYTES / 2 * SPK_UPSAMPLE_FACTOR];

  while (g_playRunning) {
    AudioPacket pkt = {};
    if (xQueueReceive(g_audioQueue, &pkt, pdMS_TO_TICKS(5)) == pdTRUE) {
      if (g_speakerReady && pkt.data && pkt.len > 0) {
        applyVolume(pkt.data, pkt.len);
        pushReference(pkt.data, pkt.len);  // 16 kHz reference for AEC
        size_t inSamples = pkt.len / sizeof(int16_t);
        size_t outSamples = upsample16to48((const int16_t *)pkt.data,
                                           inSamples, upsampleScratch);
        size_t outBytes = outSamples * sizeof(int16_t);
        size_t written = i2sSpeaker.write((uint8_t *)upsampleScratch, outBytes);
        if (written != outBytes) {
          Serial.printf("[speaker_stream] short write: %u/%u\n",
                        (unsigned)written, (unsigned)outBytes);
        }
      }
      freePacket(pkt);
    } else if (g_speakerReady) {
      // I2S write is blocking on the DMA buffer, so this naturally paces to
      // ~16 ms per iteration and won't spin the CPU.
      i2sSpeaker.write((uint8_t *)silence48k, sizeof(silence48k));
    }
  }

  AudioPacket pkt = {};
  while (xQueueReceive(g_audioQueue, &pkt, 0) == pdTRUE) {
    freePacket(pkt);
  }

  Serial.println("[speaker_stream] play task ended");
  vTaskDelete(nullptr);
}

void enqueueAudio(const uint8_t *payload, size_t len) {
  if (!g_audioQueue || !payload || len == 0) return;
  if (len > SPK_MAX_FRAME_BYTES) {
    Serial.printf("[speaker_stream] frame too large: %u\n", (unsigned)len);
    return;
  }

  uint8_t *copy = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!copy) {
    copy = (uint8_t *)malloc(len);
  }
  if (!copy) {
    Serial.println("[speaker_stream] malloc failed");
    return;
  }
  memcpy(copy, payload, len);

  AudioPacket pkt = {copy, len};
  if (xQueueSend(g_audioQueue, &pkt, 0) != pdTRUE) {
    AudioPacket dropped = {};
    if (xQueueReceive(g_audioQueue, &dropped, 0) == pdTRUE) {
      freePacket(dropped);
    }
    if (xQueueSend(g_audioQueue, &pkt, 0) != pdTRUE) {
      freePacket(pkt);
      Serial.println("[speaker_stream] queue full, dropped packet");
    }
  }
}

esp_err_t wsSpeakerHandler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    int fd = httpd_req_to_sockfd(req);
    char owner[80] = {};
    if (!authorizeWsRequest(req, owner, sizeof(owner))) {
      return ESP_FAIL;
    }
    g_clientFd = fd;
    Serial.printf("[speaker_stream] WS handshake fd=%d owner=%s\n", fd, owner);
    return ESP_OK;
  }

  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_BINARY;
  esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
  if (ret != ESP_OK) return ret;

  if (frame.type == HTTPD_WS_TYPE_CLOSE) {
    int fd = httpd_req_to_sockfd(req);
    if (g_clientFd == fd) g_clientFd = -1;
    Serial.println("[speaker_stream] WS closed");
    return ESP_OK;
  }

  if (frame.type != HTTPD_WS_TYPE_BINARY || frame.len == 0) {
    return ESP_OK;
  }
  if (frame.len > SPK_MAX_FRAME_BYTES) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "audio frame too large");
    return ESP_FAIL;
  }

  // Static buffer to avoid blowing the ~4 KB httpd task stack with a frame-sized
  // local; the esp_http_server processes WS frames for one URI sequentially on
  // a single worker task, so reusing one buffer here is safe.
  static uint8_t buf[SPK_MAX_FRAME_BYTES];
  frame.payload = buf;
  ret = httpd_ws_recv_frame(req, &frame, frame.len);
  if (ret != ESP_OK) return ret;

  enqueueAudio(buf, frame.len);
  return ESP_OK;
}

}  // namespace

namespace SpeakerStream {

bool begin() {
  if (g_speakerReady) return true;
  loadVolumePreference();

  if (!g_audioQueue) {
    g_audioQueue = xQueueCreate(SPK_QUEUE_DEPTH, sizeof(AudioPacket));
  }
  if (!g_refMutex) {
    g_refMutex = xSemaphoreCreateMutex();
  }
  if (!g_refRing) {
    g_refRing = (int16_t *)heap_caps_calloc(
        REF_RING_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!g_audioQueue) {
    Serial.println("[speaker_stream] queue init FAILED");
    return false;
  }
  if (!g_refMutex) {
    Serial.println("[speaker_stream] reference mutex init FAILED");
    return false;
  }
  if (!g_refRing) {
    Serial.println("[speaker_stream] reference ring PSRAM allocation FAILED");
    return false;
  }

  // Pre-drive I2S pins low so MAX98357A doesn't see floating DIN/BCLK during
  // boot (which causes random pops/hiss before begin() configures the bus).
  pinMode(SPK_I2S_BCLK_PIN, OUTPUT);
  pinMode(SPK_I2S_LRC_PIN, OUTPUT);
  pinMode(SPK_I2S_DOUT_PIN, OUTPUT);
  digitalWrite(SPK_I2S_BCLK_PIN, LOW);
  digitalWrite(SPK_I2S_LRC_PIN, LOW);
  digitalWrite(SPK_I2S_DOUT_PIN, LOW);

  i2sSpeaker.setPins(SPK_I2S_BCLK_PIN, SPK_I2S_LRC_PIN, SPK_I2S_DOUT_PIN);
  if (!i2sSpeaker.begin(I2S_MODE_STD, SPK_OUTPUT_SAMPLE_RATE,
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("[speaker_stream] I2S STD TX init FAILED");
    return false;
  }

  g_speakerReady = true;
  Serial.printf("[speaker_stream] I2S TX ready: %d Hz out (input %d Hz, %dx upsample), mono, 16-bit, BCLK=%d LRC=%d DOUT=%d\n",
                SPK_OUTPUT_SAMPLE_RATE, SPK_INPUT_SAMPLE_RATE, SPK_UPSAMPLE_FACTOR,
                SPK_I2S_BCLK_PIN, SPK_I2S_LRC_PIN, SPK_I2S_DOUT_PIN);
  return true;
}

bool isRunning() {
  return g_speakerReady && g_playRunning;
}

size_t readReference(int16_t *out, size_t samples) {
  if (!out || samples == 0) return 0;
  size_t copied = 0;
  if (g_refMutex && g_refRing) {
    xSemaphoreTake(g_refMutex, portMAX_DELAY);
    while (copied < samples && g_refCount > 0) {
      out[copied++] = g_refRing[g_refRead];
      g_refRead = (g_refRead + 1) % REF_RING_SAMPLES;
      g_refCount--;
    }
    xSemaphoreGive(g_refMutex);
  }
  while (copied < samples) {
    out[copied++] = 0;
  }
  return copied;
}

void clearReference() {
  if (!g_refMutex || !g_refRing) return;
  xSemaphoreTake(g_refMutex, portMAX_DELAY);
  g_refRead = 0;
  g_refWrite = 0;
  g_refCount = 0;
  memset(g_refRing, 0, REF_RING_SAMPLES * sizeof(int16_t));
  xSemaphoreGive(g_refMutex);
}

void setVolume(float volume) {
  g_volume = clampVolume(volume);
  saveVolumePreference(g_volume);
  Serial.printf("[speaker_stream] volume=%.2f\n", g_volume);
}

float getVolume() {
  return g_volume;
}

bool registerWsHandler(httpd_handle_t server) {
  if (!server || g_handlerRegistered) return false;
  g_server = server;

  httpd_uri_t wsUri = {
    .uri = "/ws/speaker",
    .method = HTTP_GET,
    .handler = wsSpeakerHandler,
    .user_ctx = nullptr,
    .is_websocket = true,
    .handle_ws_control_frames = true,
    .supported_subprotocol = nullptr,
  };

  esp_err_t err = httpd_register_uri_handler(server, &wsUri);
  if (err != ESP_OK) {
    Serial.printf("[speaker_stream] register /ws/speaker failed: %d\n", err);
    return false;
  }

  if (!ensurePlayTaskRunning()) {
    return false;
  }
  g_handlerRegistered = true;
  Serial.println("[speaker_stream] /ws/speaker registered");
  return true;
}

void closeClients() {
  if (g_server && g_clientFd >= 0) {
    int fd = g_clientFd;
    g_clientFd = -1;
    httpd_sess_trigger_close(g_server, fd);
    Serial.printf("[speaker_stream] closed owner-bound client fd=%d\n", fd);
  }
}

}  // namespace SpeakerStream
