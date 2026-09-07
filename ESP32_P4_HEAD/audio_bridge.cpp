// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "audio_bridge.h"

#include <ESP_I2S.h>
#include <esp_afe_config.h>
#include <esp_afe_sr_iface.h>
#include <esp_afe_sr_models.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "board_config.h"

namespace {
constexpr size_t kChunkSamples = BoardConfig::kAudioSampleRate * 30 / 1000;
constexpr size_t kChunkBytes = kChunkSamples * sizeof(int16_t);
constexpr size_t kSpeakerMaxFrameBytes = 4096;
constexpr size_t kSpeakerQueueDepth = 8;
constexpr size_t kReferenceSamples = BoardConfig::kAudioSampleRate;
constexpr uint32_t kSendStaleMs = 1500;
constexpr uint32_t kSpeakerOutputRate = 48000;
constexpr uint8_t kUpsample = kSpeakerOutputRate / BoardConfig::kAudioSampleRate;

struct AudioClient {
  int fd = -1;
  bool sending = false;
  uint32_t sendingSinceMs = 0;
  uint32_t pairingRevision = 0;
};

struct PendingFrame {
  int fd = -1;
  httpd_ws_frame_t frame{};
  uint8_t* payload = nullptr;
};

struct AudioPacket {
  uint8_t* data = nullptr;
  size_t length = 0;
  uint32_t pairingRevision = 0;
};

PairingStore* gPairing = nullptr;
I2SClass gMic;
I2SClass gSpeaker;
httpd_handle_t gServer = nullptr;
AudioClient gAudioClients[2];
SemaphoreHandle_t gClientMutex = nullptr;
QueueHandle_t gSpeakerQueue = nullptr;
TaskHandle_t gMicTask = nullptr;
TaskHandle_t gSpeakerTask = nullptr;
SemaphoreHandle_t gReferenceMutex = nullptr;
int16_t* gReference = nullptr;
size_t gReferenceRead = 0;
size_t gReferenceWrite = 0;
size_t gReferenceCount = 0;
volatile int gSpeakerClient = -1;
volatile uint32_t gSpeakerRevision = 0;
volatile bool gMicReady = false;
volatile bool gMicEnabled = true;
volatile bool gSpeakerReady = false;
volatile bool gAecReady = false;
volatile bool gAecEnabled = true;
volatile uint32_t gMicFrames = 0;
volatile uint32_t gSpeakerPackets = 0;
volatile uint32_t gSpeakerDrops = 0;
volatile float gSpeakerVolume = 0.25f;
char gProfile[24] = "aec_experiment";

const esp_afe_sr_iface_t* gAfe = nullptr;
esp_afe_sr_data_t* gAfeData = nullptr;
afe_config_t* gAfeConfig = nullptr;
int16_t* gAfeInput = nullptr;
int16_t* gReferenceScratch = nullptr;
int gAfeFeedSamples = 0;
int gAfeChannels = 0;
size_t gAfeFill = 0;

bool extractQuery(httpd_req_t* request, const char* key, char* output, size_t length) {
  char query[320]{};
  output[0] = 0;
  return httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
         httpd_query_key_value(query, key, output, length) == ESP_OK && output[0] != 0;
}

bool authorize(httpd_req_t* request) {
  char owner[96]{};
  char secret[160]{};
  return gPairing && extractQuery(request, "owner", owner, sizeof(owner)) &&
         extractQuery(request, "token", secret, sizeof(secret)) &&
         gPairing->authorize(owner, secret);
}

void removeAudioClient(int fd) {
  if (!gClientMutex) return;
  xSemaphoreTake(gClientMutex, portMAX_DELAY);
  for (AudioClient& client : gAudioClients) {
    if (client.fd == fd) client = AudioClient{};
  }
  xSemaphoreGive(gClientMutex);
}

bool addAudioClient(int fd, uint32_t pairingRevision) {
  bool added = false;
  xSemaphoreTake(gClientMutex, portMAX_DELAY);
  for (AudioClient& client : gAudioClients) {
    if (client.fd < 0) {
      client.fd = fd;
      client.pairingRevision = pairingRevision;
      added = true;
      break;
    }
  }
  xSemaphoreGive(gClientMutex);
  return added;
}

bool hasAudioClient() {
  bool present = false;
  xSemaphoreTake(gClientMutex, portMAX_DELAY);
  for (AudioClient& client : gAudioClients) {
    if (client.fd >= 0 && (!gPairing || client.pairingRevision != gPairing->revision())) {
      httpd_sess_trigger_close(gServer, client.fd);
      client = AudioClient{};
    }
    present = present || client.fd >= 0;
  }
  xSemaphoreGive(gClientMutex);
  return present;
}

void sendComplete(esp_err_t result, int fd, void* argument) {
  PendingFrame* pending = static_cast<PendingFrame*>(argument);
  if (gClientMutex) {
    xSemaphoreTake(gClientMutex, portMAX_DELAY);
    for (AudioClient& client : gAudioClients) {
      if (client.fd == fd) {
        client.sending = false;
        client.sendingSinceMs = 0;
        if (result != ESP_OK) client = AudioClient{};
      }
    }
    xSemaphoreGive(gClientMutex);
  }
  if (pending) {
    heap_caps_free(pending->payload);
    free(pending);
  }
}

void sendWork(void* argument) {
  PendingFrame* pending = static_cast<PendingFrame*>(argument);
  if (!pending || !gServer ||
      httpd_ws_get_fd_info(gServer, pending->fd) != HTTPD_WS_CLIENT_WEBSOCKET ||
      httpd_ws_send_data_async(gServer, pending->fd, &pending->frame, sendComplete, pending) !=
          ESP_OK) {
    if (pending) {
      removeAudioClient(pending->fd);
      heap_caps_free(pending->payload);
      free(pending);
    }
  }
}

void queueAudioFrame(const uint8_t* data, size_t length) {
  int targets[2]{};
  int targetCount = 0;
  const uint32_t now = millis();
  xSemaphoreTake(gClientMutex, portMAX_DELAY);
  for (AudioClient& client : gAudioClients) {
    if (client.fd < 0) continue;
    if (!gPairing || client.pairingRevision != gPairing->revision()) {
      httpd_sess_trigger_close(gServer, client.fd);
      client = AudioClient{};
      continue;
    }
    if (client.sending) {
      if (now - client.sendingSinceMs > kSendStaleMs) {
        httpd_sess_trigger_close(gServer, client.fd);
        client = AudioClient{};
      }
      continue;
    }
    client.sending = true;
    client.sendingSinceMs = now;
    targets[targetCount++] = client.fd;
  }
  xSemaphoreGive(gClientMutex);

  for (int index = 0; index < targetCount; ++index) {
    PendingFrame* pending = static_cast<PendingFrame*>(calloc(1, sizeof(PendingFrame)));
    if (pending) {
      pending->payload = static_cast<uint8_t*>(
          heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (!pending || !pending->payload) {
      if (pending) free(pending);
      removeAudioClient(targets[index]);
      continue;
    }
    memcpy(pending->payload, data, length);
    pending->fd = targets[index];
    pending->frame.type = HTTPD_WS_TYPE_BINARY;
    pending->frame.payload = pending->payload;
    pending->frame.len = length;
    if (httpd_queue_work(gServer, sendWork, pending) != ESP_OK) {
      removeAudioClient(pending->fd);
      heap_caps_free(pending->payload);
      free(pending);
    }
  }
}

void pushReference(const int16_t* samples, size_t count) {
  if (!gReference || !gReferenceMutex) return;
  xSemaphoreTake(gReferenceMutex, portMAX_DELAY);
  for (size_t index = 0; index < count; ++index) {
    gReference[gReferenceWrite] = samples[index];
    gReferenceWrite = (gReferenceWrite + 1) % kReferenceSamples;
    if (gReferenceCount < kReferenceSamples) {
      ++gReferenceCount;
    } else {
      gReferenceRead = (gReferenceRead + 1) % kReferenceSamples;
    }
  }
  xSemaphoreGive(gReferenceMutex);
}

void readReference(int16_t* output, size_t count) {
  size_t copied = 0;
  xSemaphoreTake(gReferenceMutex, portMAX_DELAY);
  while (copied < count && gReferenceCount > 0) {
    output[copied++] = gReference[gReferenceRead];
    gReferenceRead = (gReferenceRead + 1) % kReferenceSamples;
    --gReferenceCount;
  }
  xSemaphoreGive(gReferenceMutex);
  while (copied < count) output[copied++] = 0;
}

bool beginAec() {
  gAfeConfig = afe_config_init("MR", nullptr, AFE_TYPE_VC, AFE_MODE_LOW_COST);
  if (!gAfeConfig) return false;
  gAfeConfig->aec_init = true;
  gAfeConfig->aec_filter_length = 4;
  gAfeConfig->se_init = false;
  gAfeConfig->ns_init = false;
  gAfeConfig->vad_init = false;
  gAfeConfig->wakenet_init = false;
  gAfeConfig->agc_init = false;
  gAfeConfig->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
  gAfeConfig->fixed_output_channel = true;
  gAfeConfig = afe_config_check(gAfeConfig);
  if (!gAfeConfig) return false;
  gAfe = esp_afe_handle_from_config(gAfeConfig);
  gAfeData = gAfe ? gAfe->create_from_config(gAfeConfig) : nullptr;
  if (!gAfeData) return false;
  gAfeFeedSamples = gAfe->get_feed_chunksize(gAfeData);
  gAfeChannels = gAfe->get_feed_channel_num(gAfeData);
  if (gAfe->get_samp_rate(gAfeData) != static_cast<int>(BoardConfig::kAudioSampleRate) ||
      gAfeFeedSamples <= 0 || gAfeChannels < 2) {
    return false;
  }
  gAfeInput = static_cast<int16_t*>(heap_caps_aligned_calloc(
      16, gAfeFeedSamples * gAfeChannels, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  gReferenceScratch = static_cast<int16_t*>(heap_caps_aligned_calloc(
      16, kChunkSamples, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!gAfeInput || !gReferenceScratch) return false;
  Serial.printf("[AUDIO] ESP-SR AEC ready feed=%d channels=%d\n", gAfeFeedSamples, gAfeChannels);
  return true;
}

void processMic(const int16_t* samples, size_t count) {
  if (!gAecReady || !gAecEnabled) {
    queueAudioFrame(reinterpret_cast<const uint8_t*>(samples), count * sizeof(int16_t));
    return;
  }
  readReference(gReferenceScratch, count);
  for (size_t index = 0; index < count; ++index) {
    const size_t offset = gAfeFill * gAfeChannels;
    gAfeInput[offset] = samples[index];
    gAfeInput[offset + 1] = gReferenceScratch[index];
    for (int channel = 2; channel < gAfeChannels; ++channel) gAfeInput[offset + channel] = 0;
    if (++gAfeFill < static_cast<size_t>(gAfeFeedSamples)) continue;
    gAfe->feed(gAfeData, gAfeInput);
    gAfeFill = 0;
    afe_fetch_result_t* result = gAfe->fetch_with_delay(gAfeData, pdMS_TO_TICKS(5));
    if (result && result->ret_value == ESP_OK && result->data && result->data_size > 0) {
      queueAudioFrame(reinterpret_cast<const uint8_t*>(result->data), result->data_size);
    }
  }
}

void micTask(void*) {
  int16_t* samples = static_cast<int16_t*>(
      heap_caps_malloc(kChunkBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  while (gMicReady && samples) {
    const size_t bytes = gMic.readBytes(reinterpret_cast<char*>(samples), kChunkBytes);
    if (bytes >= sizeof(int16_t)) {
      if (gMicEnabled && hasAudioClient()) processMic(samples, bytes / sizeof(int16_t));
      gMicFrames = gMicFrames + 1;
    } else {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
  if (samples) heap_caps_free(samples);
  vTaskDelete(nullptr);
}

void freePacket(AudioPacket& packet) {
  if (packet.data) heap_caps_free(packet.data);
  packet = AudioPacket{};
}

void speakerTask(void*) {
  constexpr size_t kSilenceSamples = 768;
  static int16_t silence[kSilenceSamples]{};
  int16_t* output = static_cast<int16_t*>(heap_caps_malloc(
      kSpeakerMaxFrameBytes * kUpsample / 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  int16_t* referenceChunk = static_cast<int16_t*>(
      heap_caps_malloc(kChunkBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  while (gSpeakerReady && output && referenceChunk) {
    AudioPacket packet;
    if (xQueueReceive(gSpeakerQueue, &packet, pdMS_TO_TICKS(5)) != pdTRUE) {
      gSpeaker.write(reinterpret_cast<uint8_t*>(silence), sizeof(silence));
      continue;
    }
    if (!gPairing || packet.pairingRevision != gPairing->revision()) {
      freePacket(packet);
      continue;
    }
    const int16_t* input = reinterpret_cast<const int16_t*>(packet.data);
    const size_t count = packet.length / sizeof(int16_t);
    size_t outIndex = 0;
    for (size_t index = 0; index < count; ++index) {
      const int32_t scaled = static_cast<int32_t>(input[index] * gSpeakerVolume);
      const int16_t sample = constrain(scaled, -32768, 32767);
      referenceChunk[index % kChunkSamples] = sample;
      for (uint8_t copy = 0; copy < kUpsample; ++copy) output[outIndex++] = sample;
      if ((index + 1) % kChunkSamples == 0) pushReference(referenceChunk, kChunkSamples);
    }
    const size_t remainder = count % kChunkSamples;
    if (remainder) pushReference(referenceChunk, remainder);
    gSpeaker.write(reinterpret_cast<uint8_t*>(output), outIndex * sizeof(int16_t));
    freePacket(packet);
  }
  if (output) heap_caps_free(output);
  if (referenceChunk) heap_caps_free(referenceChunk);
  vTaskDelete(nullptr);
}

void enqueueSpeaker(const uint8_t* data, size_t length) {
  if (!gSpeakerQueue || length == 0 || length > kSpeakerMaxFrameBytes || (length & 1)) return;
  AudioPacket packet;
  packet.data = static_cast<uint8_t*>(
      heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!packet.data) {
    gSpeakerDrops = gSpeakerDrops + 1;
    return;
  }
  memcpy(packet.data, data, length);
  packet.length = length;
  packet.pairingRevision = gPairing ? gPairing->revision() : 0;
  if (xQueueSend(gSpeakerQueue, &packet, 0) != pdTRUE) {
    AudioPacket oldest;
    if (xQueueReceive(gSpeakerQueue, &oldest, 0) == pdTRUE) freePacket(oldest);
    if (xQueueSend(gSpeakerQueue, &packet, 0) != pdTRUE) freePacket(packet);
    gSpeakerDrops = gSpeakerDrops + 1;
  } else {
    gSpeakerPackets = gSpeakerPackets + 1;
  }
}

esp_err_t audioHandler(httpd_req_t* request) {
  const int fd = httpd_req_to_sockfd(request);
  if (request->method == HTTP_GET) {
    if (!authorize(request) || !addAudioClient(fd, gPairing->revision())) return ESP_FAIL;
    return ESP_OK;
  }
  httpd_ws_frame_t frame{};
  if (httpd_ws_recv_frame(request, &frame, 0) != ESP_OK) return ESP_FAIL;
  if (frame.type == HTTPD_WS_TYPE_CLOSE) removeAudioClient(fd);
  return ESP_OK;
}

esp_err_t eventHandler(httpd_req_t* request) {
  if (request->method == HTTP_GET && !authorize(request)) return ESP_FAIL;
  httpd_ws_frame_t frame{};
  if (request->method != HTTP_GET) httpd_ws_recv_frame(request, &frame, 0);
  return ESP_OK;
}

esp_err_t speakerHandler(httpd_req_t* request) {
  const int fd = httpd_req_to_sockfd(request);
  if (request->method == HTTP_GET) {
    if (!authorize(request)) return ESP_FAIL;
    gSpeakerClient = fd;
    gSpeakerRevision = gPairing->revision();
    return ESP_OK;
  }
  if (!gPairing || gSpeakerRevision != gPairing->revision()) {
    if (gSpeakerClient == fd) gSpeakerClient = -1;
    httpd_sess_trigger_close(gServer, fd);
    return ESP_FAIL;
  }
  httpd_ws_frame_t frame{};
  if (httpd_ws_recv_frame(request, &frame, 0) != ESP_OK) return ESP_FAIL;
  if (frame.type == HTTPD_WS_TYPE_CLOSE) {
    if (gSpeakerClient == fd) {
      gSpeakerClient = -1;
      gSpeakerRevision = 0;
    }
    return ESP_OK;
  }
  if (frame.type != HTTPD_WS_TYPE_BINARY || frame.len == 0 ||
      frame.len > kSpeakerMaxFrameBytes) {
    return ESP_OK;
  }
  static uint8_t payload[kSpeakerMaxFrameBytes];
  frame.payload = payload;
  if (httpd_ws_recv_frame(request, &frame, frame.len) == ESP_OK) enqueueSpeaker(payload, frame.len);
  return ESP_OK;
}

bool registerWs(const char* path, esp_err_t (*handler)(httpd_req_t*)) {
  httpd_uri_t uri{};
  uri.uri = path;
  uri.method = HTTP_GET;
  uri.handler = handler;
  uri.is_websocket = true;
  uri.handle_ws_control_frames = true;
  return httpd_register_uri_handler(gServer, &uri) == ESP_OK;
}
}  // namespace

bool AudioBridge::begin() {
  gPairing = &pairing_;
  gClientMutex = xSemaphoreCreateMutex();
  gReferenceMutex = xSemaphoreCreateMutex();
  gSpeakerQueue = xQueueCreate(kSpeakerQueueDepth, sizeof(AudioPacket));
  gReference = static_cast<int16_t*>(
      heap_caps_calloc(kReferenceSamples, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!gClientMutex || !gReferenceMutex || !gSpeakerQueue || !gReference) return false;

  pinMode(BoardConfig::kSpeakerBclk, OUTPUT);
  pinMode(BoardConfig::kSpeakerLrclk, OUTPUT);
  pinMode(BoardConfig::kSpeakerData, OUTPUT);
  digitalWrite(BoardConfig::kSpeakerBclk, LOW);
  digitalWrite(BoardConfig::kSpeakerLrclk, LOW);
  digitalWrite(BoardConfig::kSpeakerData, LOW);
  gSpeaker.setPins(BoardConfig::kSpeakerBclk, BoardConfig::kSpeakerLrclk,
                   BoardConfig::kSpeakerData);
  gSpeakerReady = gSpeaker.begin(I2S_MODE_STD, kSpeakerOutputRate, I2S_DATA_BIT_WIDTH_16BIT,
                                 I2S_SLOT_MODE_MONO);

  gMic.setPinsPdmRx(BoardConfig::kMicClock, BoardConfig::kMicData);
  gMicReady = gMic.begin(I2S_MODE_PDM_RX, BoardConfig::kAudioSampleRate,
                         I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  gAecReady = gMicReady && gSpeakerReady && beginAec();

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = BoardConfig::kAudioWsPort;
  config.ctrl_port = 32769;
  config.stack_size = 8192;
  config.max_uri_handlers = 5;
  const bool serverReady = httpd_start(&gServer, &config) == ESP_OK &&
                           registerWs("/ws/audio", audioHandler) &&
                           registerWs("/ws/events", eventHandler) &&
                           registerWs("/ws/speaker", speakerHandler);
  if (gMicReady) xTaskCreate(micTask, "audio-mic", 6144, nullptr, 4, &gMicTask);
  if (gSpeakerReady) xTaskCreate(speakerTask, "audio-speaker", 6144, nullptr, 4, &gSpeakerTask);
  Serial.printf("[AUDIO] mic=%d speaker=%d aec=%d ws=%d port=%u\n", gMicReady,
                gSpeakerReady, gAecReady, serverReady, BoardConfig::kAudioWsPort);
  return serverReady && gMicReady && gSpeakerReady;
}

bool AudioBridge::microphoneReady() const { return gMicReady; }
bool AudioBridge::microphoneEnabled() const { return gMicEnabled; }
void AudioBridge::setMicrophoneEnabled(bool enabled) { gMicEnabled = enabled; }
bool AudioBridge::speakerReady() const { return gSpeakerReady; }
bool AudioBridge::aecReady() const { return gAecReady; }
bool AudioBridge::aecEnabled() const { return gAecReady && gAecEnabled; }
httpd_handle_t AudioBridge::httpServer() const { return gServer; }
const char* AudioBridge::profile() const { return gProfile; }

bool AudioBridge::setProfile(const String& profile) {
  if (profile == "aec_experiment") {
    gAecEnabled = true;
  } else if (profile == "stable_raw" || profile == "interruptible_raw") {
    gAecEnabled = false;
  } else {
    return false;
  }
  strlcpy(gProfile, profile.c_str(), sizeof(gProfile));
  return true;
}

float AudioBridge::speakerVolume() const { return gSpeakerVolume; }

void AudioBridge::setSpeakerVolume(float volume) {
  gSpeakerVolume = constrain(volume, 0.0f, 0.6f);
}

uint32_t AudioBridge::micFrames() const { return gMicFrames; }
uint32_t AudioBridge::speakerPackets() const { return gSpeakerPackets; }
uint32_t AudioBridge::speakerDrops() const { return gSpeakerDrops; }
