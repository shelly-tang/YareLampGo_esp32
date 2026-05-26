#include "device_api.h"

#include <Arduino.h>
#include <WiFi.h>
#include <stdarg.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_camera.h>

#include "led_serial.h"
#include "mic_stream.h"
#include "net_config.h"
#include "speaker_stream.h"

namespace {

const char *FIRMWARE_VERSION = "lampgo-cam 0.3.2";

bool appendRaw(char *dst, size_t dstLen, size_t *used, const char *src) {
  if (!dst || !used || !src || *used >= dstLen) return false;
  while (*src) {
    if (*used + 1 >= dstLen) return false;
    dst[*used] = *src;
    (*used)++;
    src++;
  }
  dst[*used] = '\0';
  return true;
}

bool appendFormat(char *dst, size_t dstLen, size_t *used, const char *fmt, ...) {
  if (!dst || !used || !fmt || *used >= dstLen) return false;
  va_list args;
  va_start(args, fmt);
  int written = vsnprintf(dst + *used, dstLen - *used, fmt, args);
  va_end(args);
  if (written < 0 || (size_t)written >= (dstLen - *used)) {
    if (dstLen > 0) dst[dstLen - 1] = '\0';
    return false;
  }
  *used += (size_t)written;
  return true;
}

bool appendJsonEscaped(char *dst, size_t dstLen, size_t *used, const char *src) {
  if (!src) return true;
  while (*src) {
    unsigned char ch = static_cast<unsigned char>(*src++);
    switch (ch) {
      case '\"':
        if (!appendRaw(dst, dstLen, used, "\\\"")) return false;
        break;
      case '\\':
        if (!appendRaw(dst, dstLen, used, "\\\\")) return false;
        break;
      case '\b':
        if (!appendRaw(dst, dstLen, used, "\\b")) return false;
        break;
      case '\f':
        if (!appendRaw(dst, dstLen, used, "\\f")) return false;
        break;
      case '\n':
        if (!appendRaw(dst, dstLen, used, "\\n")) return false;
        break;
      case '\r':
        if (!appendRaw(dst, dstLen, used, "\\r")) return false;
        break;
      case '\t':
        if (!appendRaw(dst, dstLen, used, "\\t")) return false;
        break;
      default:
        if (ch < 0x20) {
          if (!appendFormat(dst, dstLen, used, "\\u%04x", ch)) return false;
        } else {
          if (*used + 1 >= dstLen) return false;
          dst[*used] = static_cast<char>(ch);
          (*used)++;
          dst[*used] = '\0';
        }
        break;
    }
  }
  return true;
}

void setJsonHeaders(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

esp_err_t sendJson(httpd_req_t *req, cJSON *root) {
  char *out = cJSON_PrintUnformatted(root);
  esp_err_t res = httpd_resp_sendstr(req, out ? out : "{}");
  if (out) cJSON_free(out);
  cJSON_Delete(root);
  return res;
}

cJSON *readJsonBody(httpd_req_t *req, size_t maxBytes = 1024) {
  int total = req->content_len;
  if (total <= 0 || (size_t)total > maxBytes) {
    return nullptr;
  }
  char *buf = (char *)malloc(total + 1);
  if (!buf) return nullptr;
  int received = 0;
  while (received < total) {
    int r = httpd_req_recv(req, buf + received, total - received);
    if (r <= 0) {
      free(buf);
      return nullptr;
    }
    received += r;
  }
  buf[received] = 0;
  cJSON *doc = cJSON_Parse(buf);
  free(buf);
  return doc;
}

String jsonString(cJSON *doc, const char *key) {
  if (!doc) return String("");
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(doc, key);
  return cJSON_IsString(item) && item->valuestring ? String(item->valuestring) : String("");
}

bool requestAuthorized(cJSON *doc) {
  if (!NetConfig::hasPairing()) return true;
  return NetConfig::verifyPairing(jsonString(doc, "owner_id"), jsonString(doc, "pairing_secret"));
}

esp_err_t sendForbidden(httpd_req_t *req, const char *error) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", false);
  cJSON_AddStringToObject(root, "error", error);
  httpd_resp_set_status(req, "403 Forbidden");
  return sendJson(req, root);
}

void addPairingStatus(cJSON *root) {
  String ownerId;
  String ownerLabel;
  String secretHash;
  bool paired = NetConfig::loadPairing(ownerId, ownerLabel, secretHash);
  cJSON_AddBoolToObject(root, "pairing_supported", true);
  cJSON_AddBoolToObject(root, "paired", paired);
  cJSON_AddStringToObject(root, "pairing_state", paired ? "paired" : "unpaired");
  cJSON_AddStringToObject(root, "paired_owner_id", paired ? ownerId.c_str() : "");
  cJSON_AddStringToObject(root, "paired_owner_label", paired ? ownerLabel.c_str() : "");
  cJSON_AddStringToObject(root, "active_owner_id", MicStream::activeOwner());
  cJSON_AddNumberToObject(root, "owner_lease_remaining_ms", (double)MicStream::ownerLeaseRemainingMs());
}

void addLedStatus(cJSON *root) {
  cJSON_AddBoolToObject(root, "led_ready", LedSerial::isReady());
  cJSON_AddNumberToObject(root, "led_mode", LedSerial::currentMode());
  cJSON_AddStringToObject(root, "led_mode_name", LedSerial::modeName(LedSerial::currentMode()));
  cJSON_AddNumberToObject(root, "led_brightness", LedSerial::currentBrightness());
  cJSON_AddStringToObject(root, "led_last_command", LedSerial::lastCommand());
  cJSON_AddNumberToObject(root, "led_last_write_ms", (double)LedSerial::lastWriteMs());
  cJSON_AddStringToObject(root, "led_driver", LedSerial::driverName());
  cJSON_AddNumberToObject(root, "led_pixel_pin", LedSerial::pixelPin());
  cJSON_AddNumberToObject(root, "led_pixel_count", LedSerial::pixelCount());
  cJSON_AddNumberToObject(root, "led_panel_count", LedSerial::panelCount());
  cJSON_AddBoolToObject(root, "led_output_ok", LedSerial::outputOk());
}

void addLedSupportedModes(cJSON *root) {
  cJSON *modes = cJSON_AddArrayToObject(root, "led_supported_modes");
  if (!modes) return;
  for (int mode = 0; mode <= LedSerial::maxMode(); ++mode) {
    cJSON *item = cJSON_CreateObject();
    if (!item) continue;
    cJSON_AddNumberToObject(item, "mode", mode);
    cJSON_AddStringToObject(item, "name", LedSerial::modeName(mode));
    cJSON_AddItemToArray(modes, item);
  }
}

void addWakeSupportedModels(cJSON *root) {
  char modelsJson[512] = "[]";
  MicStream::copyWakeSupportedModelsJson(modelsJson, sizeof(modelsJson));
  cJSON *wakeModels = cJSON_Parse(modelsJson);
  if (wakeModels && cJSON_IsArray(wakeModels)) {
    cJSON_AddItemToObject(root, "wake_supported_models", wakeModels);
    return;
  }
  if (wakeModels) cJSON_Delete(wakeModels);
  cJSON_AddArrayToObject(root, "wake_supported_models");
}

bool appendWakeSupportedModelsJson(char *body, size_t bodyLen, size_t *used) {
  char modelsJson[512] = "[]";
  MicStream::copyWakeSupportedModelsJson(modelsJson, sizeof(modelsJson));
  return appendRaw(body, bodyLen, used, modelsJson);
}

bool writeDebugStatusJson(char *body, size_t bodyLen) {
  if (!body || bodyLen == 0) return false;

  sensor_t *sensor = esp_camera_sensor_get();
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  String hostname = NetConfig::deviceHostname();
  String ownerId;
  String ownerLabel;
  String secretHash;
  bool paired = NetConfig::loadPairing(ownerId, ownerLabel, secretHash);
  String ip = WiFi.localIP().toString();
  String ssid = WiFi.SSID();
  int ledMode = LedSerial::currentMode();

  size_t used = 0;
  body[0] = '\0';

  bool ok = true;
  ok = ok && appendRaw(body, bodyLen, &used, "{");
  ok = ok && appendRaw(body, bodyLen, &used, "\"configured\":true,\"mode\":\"work\"");
  ok = ok && appendRaw(body, bodyLen, &used, ",\"firmware\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, FIRMWARE_VERSION);
  ok = ok && appendRaw(body, bodyLen, &used, "\"");
  ok = ok && appendRaw(body, bodyLen, &used, ",\"hostname\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, hostname.c_str());
  ok = ok && appendRaw(body, bodyLen, &used, "\"");
  ok = ok && appendFormat(body, bodyLen, &used, ",\"pairing_supported\":true,\"paired\":%s",
                           paired ? "true" : "false");
  ok = ok && appendRaw(body, bodyLen, &used, ",\"pairing_state\":\"");
  ok = ok && appendRaw(body, bodyLen, &used, paired ? "paired" : "unpaired");
  ok = ok && appendRaw(body, bodyLen, &used, "\"");
  ok = ok && appendRaw(body, bodyLen, &used, ",\"paired_owner_id\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, paired ? ownerId.c_str() : "");
  ok = ok && appendRaw(body, bodyLen, &used, "\"");
  ok = ok && appendRaw(body, bodyLen, &used, ",\"paired_owner_label\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, paired ? ownerLabel.c_str() : "");
  ok = ok && appendRaw(body, bodyLen, &used, "\"");
  ok = ok && appendRaw(body, bodyLen, &used, ",\"active_owner_id\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, MicStream::activeOwner());
  ok = ok && appendFormat(body, bodyLen, &used, "\",\"owner_lease_remaining_ms\":%.0f",
                           (double)MicStream::ownerLeaseRemainingMs());
  ok = ok && appendRaw(body, bodyLen, &used, ",\"ip\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, ip.c_str());
  ok = ok && appendRaw(body, bodyLen, &used, "\",\"mac\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, macStr);
  ok = ok && appendFormat(body, bodyLen, &used, "\",\"rssi\":%d", WiFi.RSSI());
  ok = ok && appendRaw(body, bodyLen, &used, ",\"ssid\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, ssid.c_str());
  ok = ok && appendFormat(body, bodyLen, &used, "\",\"uptime_ms\":%.0f",
                           (double)millis());
  ok = ok && appendFormat(body, bodyLen, &used, ",\"free_heap\":%.0f,\"free_psram\":%.0f",
                           (double)ESP.getFreeHeap(), (double)ESP.getFreePsram());
  ok = ok && appendFormat(body, bodyLen, &used,
                           ",\"internal_free_heap\":%.0f,\"internal_min_free_heap\":%.0f,\"internal_largest_free_block\":%.0f",
                           (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                           (double)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                           (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  ok = ok && appendFormat(body, bodyLen, &used,
                           ",\"psram_free_heap\":%.0f,\"psram_min_free_heap\":%.0f,\"psram_largest_free_block\":%.0f",
                           (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                           (double)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                           (double)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  ok = ok && appendFormat(body, bodyLen, &used,
                           ",\"mic_streaming\":%s,\"mic_aec_ready\":%s,\"mic_aec_enabled\":%s,\"mic_ws_clients\":%d",
                           MicStream::isRunning() ? "true" : "false",
                           MicStream::isAecReady() ? "true" : "false",
                           MicStream::isAecEnabled() ? "true" : "false",
                           (int)MicStream::clientCount());
  ok = ok && appendRaw(body, bodyLen, &used, ",\"audio_profile\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, MicStream::audioProfile());
  ok = ok && appendRaw(body, bodyLen, &used, "\"");
  ok = ok && appendFormat(body, bodyLen, &used,
                           ",\"mic_bytes_read\":%.0f,\"mic_frames_sent\":%.0f",
                           (double)MicStream::bytesRead(), (double)MicStream::framesSent());
  ok = ok && appendFormat(body, bodyLen, &used, ",\"wake_ready\":%s",
                           MicStream::isWakeReady() ? "true" : "false");
  ok = ok && appendRaw(body, bodyLen, &used, ",\"wake_model\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, MicStream::wakeModel());
  ok = ok && appendRaw(body, bodyLen, &used, "\",\"wake_requested_model\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, MicStream::requestedWakeModel());
  ok = ok && appendRaw(body, bodyLen, &used, "\",\"wake_supported_models\":");
  ok = ok && appendWakeSupportedModelsJson(body, bodyLen, &used);
  ok = ok && appendFormat(body, bodyLen, &used,
                           ",\"wake_detections\":%.0f,\"wake_last_ms\":%.0f,\"wake_event_clients\":%d",
                           (double)MicStream::wakeDetections(), (double)MicStream::lastWakeMs(),
                           (int)MicStream::wakeEventClientCount());
  ok = ok && appendFormat(body, bodyLen, &used,
                           ",\"wake_threshold\":%.3f,\"wake_afe_gain\":%.3f,\"wake_detection_mode\":%d",
                           MicStream::wakeThreshold(), MicStream::wakeAfeGain(),
                           MicStream::wakeDetectionMode());
  ok = ok && appendFormat(body, bodyLen, &used,
                           ",\"wake_afe_feed_samples\":%d,\"wake_afe_feed_channels\":%d",
                           MicStream::afeFeedSamples(), MicStream::afeFeedChannels());
  ok = ok && appendFormat(body, bodyLen, &used,
                           ",\"mic_push_task_create_result\":%d,\"wake_fetch_task_create_result\":%d,\"wake_inline_fetch\":%s",
                           MicStream::pushTaskCreateResult(), MicStream::fetchTaskCreateResult(),
                           MicStream::isInlineFetch() ? "true" : "false");
  ok = ok && appendFormat(body, bodyLen, &used,
                           ",\"wake_afe_feeds\":%.0f,\"wake_afe_fetch_attempts\":%.0f,\"wake_afe_fetches\":%.0f,\"wake_afe_fetch_nulls\":%.0f",
                           (double)MicStream::afeFeeds(), (double)MicStream::afeFetchAttempts(),
                           (double)MicStream::afeFetches(), (double)MicStream::afeFetchNulls());
  ok = ok && appendFormat(body, bodyLen, &used,
                           ",\"wake_afe_last_ret\":%d,\"wake_afe_last_ms\":%.0f,\"wake_afe_last_state\":%d,\"wake_afe_last_word_index\":%d",
                           MicStream::lastAfeRet(), (double)MicStream::lastAfeMs(),
                           MicStream::lastWakeupState(), MicStream::lastWakeWordIndex());
  ok = ok && appendFormat(body, bodyLen, &used,
                           ",\"wake_afe_last_trigger_channel\":%d,\"wake_afe_last_vad_state\":%d,\"wake_afe_last_volume_db\":%.3f,\"wake_afe_ring_free_pct\":%.3f",
                           MicStream::lastTriggerChannel(), MicStream::lastVadState(),
                           MicStream::lastAfeVolumeDb(), MicStream::lastAfeRingFreePct());
  ok = ok && appendFormat(body, bodyLen, &used,
                           ",\"mic_last_rms\":%.3f,\"mic_last_peak\":%.3f",
                           (double)MicStream::lastMicRms(), (double)MicStream::lastMicPeak());
  ok = ok && appendFormat(body, bodyLen, &used,
                           ",\"speaker_streaming\":%s,\"speaker_volume\":%.3f",
                           SpeakerStream::isRunning() ? "true" : "false",
                           (double)SpeakerStream::getVolume());
  ok = ok && appendFormat(body, bodyLen, &used,
                           ",\"speaker_queue\":%.0f,\"speaker_packets_queued\":%.0f,\"speaker_packets_dropped\":%.0f,\"speaker_underruns\":%.0f,\"speaker_short_writes\":%.0f",
                           (double)SpeakerStream::queuedPackets(),
                           (double)SpeakerStream::packetsQueued(),
                           (double)SpeakerStream::packetsDropped(),
                           (double)SpeakerStream::underruns(),
                           (double)SpeakerStream::shortWrites());
  ok = ok && appendFormat(body, bodyLen, &used, ",\"led_ready\":%s,\"led_mode\":%d",
                           LedSerial::isReady() ? "true" : "false", ledMode);
  ok = ok && appendRaw(body, bodyLen, &used, ",\"led_mode_name\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, LedSerial::modeName(ledMode));
  ok = ok && appendFormat(body, bodyLen, &used, "\",\"led_brightness\":%d",
                           LedSerial::currentBrightness());
  ok = ok && appendRaw(body, bodyLen, &used, ",\"led_last_command\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, LedSerial::lastCommand());
  ok = ok && appendFormat(body, bodyLen, &used,
                           "\",\"led_last_write_ms\":%.0f,\"led_driver\":\"",
                           (double)LedSerial::lastWriteMs());
  ok = ok && appendJsonEscaped(body, bodyLen, &used, LedSerial::driverName());
  ok = ok && appendFormat(body, bodyLen, &used,
                           "\",\"led_pixel_pin\":%d,\"led_pixel_count\":%d,\"led_panel_count\":%d,\"led_output_ok\":%s",
                           LedSerial::pixelPin(), LedSerial::pixelCount(), LedSerial::panelCount(),
                           LedSerial::outputOk() ? "true" : "false");

  if (sensor != nullptr) {
    ok = ok && appendFormat(body, bodyLen, &used,
                             ",\"framesize\":%d,\"jpeg_quality\":%d,\"brightness\":%d,\"contrast\":%d,\"saturation\":%d,\"hmirror\":%s,\"vflip\":%s",
                             sensor->status.framesize, sensor->status.quality, sensor->status.brightness,
                             sensor->status.contrast, sensor->status.saturation,
                             sensor->status.hmirror ? "true" : "false",
                             sensor->status.vflip ? "true" : "false");
  }

  ok = ok && appendRaw(body, bodyLen, &used, "}");
  return ok;
}

cJSON *buildConfigJson() {
  sensor_t *sensor = esp_camera_sensor_get();
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddNumberToObject(root, "speaker_volume", SpeakerStream::getVolume());
  cJSON_AddStringToObject(root, "audio_profile", MicStream::audioProfile());
  cJSON_AddBoolToObject(root, "mic_aec_enabled", MicStream::isAecEnabled());
  cJSON_AddBoolToObject(root, "mic_aec_ready", MicStream::isAecReady());
  cJSON_AddStringToObject(root, "wake_model", MicStream::wakeModel());
  cJSON_AddStringToObject(root, "wake_requested_model", MicStream::requestedWakeModel());
  addWakeSupportedModels(root);
  if (sensor != nullptr) {
    cJSON_AddNumberToObject(root, "framesize", sensor->status.framesize);
    cJSON_AddNumberToObject(root, "jpeg_quality", sensor->status.quality);
    cJSON_AddNumberToObject(root, "brightness", sensor->status.brightness);
    cJSON_AddNumberToObject(root, "contrast", sensor->status.contrast);
    cJSON_AddNumberToObject(root, "saturation", sensor->status.saturation);
    cJSON_AddBoolToObject(root, "hmirror", sensor->status.hmirror);
    cJSON_AddBoolToObject(root, "vflip", sensor->status.vflip);
  }
  return root;
}

bool writeCompactStatusJson(char *body, size_t bodyLen) {
  if (!body || bodyLen == 0) return false;

  String hostname = NetConfig::deviceHostname();
  String ownerId;
  String ownerLabel;
  String secretHash;
  bool paired = NetConfig::loadPairing(ownerId, ownerLabel, secretHash);
  String ip = WiFi.localIP().toString();
  int ledMode = LedSerial::currentMode();

  size_t used = 0;
  body[0] = '\0';

  bool ok = true;
  ok = ok && appendRaw(body, bodyLen, &used, "{");
  ok = ok && appendRaw(body, bodyLen, &used, "\"configured\":true,\"mode\":\"work\"");
  ok = ok && appendRaw(body, bodyLen, &used, ",\"firmware\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, FIRMWARE_VERSION);
  ok = ok && appendRaw(body, bodyLen, &used, "\"");
  ok = ok && appendRaw(body, bodyLen, &used, ",\"hostname\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, hostname.c_str());
  ok = ok && appendRaw(body, bodyLen, &used, "\"");
  ok = ok && appendFormat(body, bodyLen, &used, ",\"pairing_supported\":true,\"paired\":%s",
                           paired ? "true" : "false");
  ok = ok && appendRaw(body, bodyLen, &used, ",\"pairing_state\":\"");
  ok = ok && appendRaw(body, bodyLen, &used, paired ? "paired" : "unpaired");
  ok = ok && appendRaw(body, bodyLen, &used, "\"");
  ok = ok && appendRaw(body, bodyLen, &used, ",\"paired_owner_id\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, paired ? ownerId.c_str() : "");
  ok = ok && appendRaw(body, bodyLen, &used, "\"");
  ok = ok && appendRaw(body, bodyLen, &used, ",\"paired_owner_label\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, paired ? ownerLabel.c_str() : "");
  ok = ok && appendRaw(body, bodyLen, &used, "\"");
  ok = ok && appendRaw(body, bodyLen, &used, ",\"ip\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, ip.c_str());
  ok = ok && appendRaw(body, bodyLen, &used, "\"");
  ok = ok && appendFormat(body, bodyLen, &used, ",\"mic_streaming\":%s",
                           MicStream::isRunning() ? "true" : "false");
  ok = ok && appendRaw(body, bodyLen, &used, ",\"audio_profile\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, MicStream::audioProfile());
  ok = ok && appendRaw(body, bodyLen, &used, "\"");
  ok = ok && appendFormat(body, bodyLen, &used, ",\"mic_aec_enabled\":%s,\"mic_aec_ready\":%s",
                           MicStream::isAecEnabled() ? "true" : "false",
                           MicStream::isAecReady() ? "true" : "false");
  ok = ok && appendFormat(body, bodyLen, &used, ",\"wake_ready\":%s",
                           MicStream::isWakeReady() ? "true" : "false");
  ok = ok && appendRaw(body, bodyLen, &used, ",\"wake_model\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, MicStream::wakeModel());
  ok = ok && appendRaw(body, bodyLen, &used, "\"");
  ok = ok && appendRaw(body, bodyLen, &used, ",\"wake_requested_model\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, MicStream::requestedWakeModel());
  ok = ok && appendRaw(body, bodyLen, &used, "\"");
  ok = ok && appendRaw(body, bodyLen, &used, ",\"wake_supported_models\":");
  ok = ok && appendWakeSupportedModelsJson(body, bodyLen, &used);
  ok = ok && appendFormat(body, bodyLen, &used, ",\"wake_event_clients\":%d",
                           MicStream::wakeEventClientCount());
  ok = ok && appendFormat(body, bodyLen, &used, ",\"led_ready\":%s",
                           LedSerial::isReady() ? "true" : "false");
  ok = ok && appendFormat(body, bodyLen, &used, ",\"led_mode\":%d", ledMode);
  ok = ok && appendRaw(body, bodyLen, &used, ",\"led_mode_name\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, LedSerial::modeName(ledMode));
  ok = ok && appendRaw(body, bodyLen, &used, "\"");
  ok = ok && appendFormat(body, bodyLen, &used, ",\"led_brightness\":%d",
                           LedSerial::currentBrightness());
  ok = ok && appendRaw(body, bodyLen, &used, ",\"led_last_command\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, LedSerial::lastCommand());
  ok = ok && appendRaw(body, bodyLen, &used, "\"");
  ok = ok && appendFormat(body, bodyLen, &used, ",\"led_last_write_ms\":%.0f",
                           (double)LedSerial::lastWriteMs());
  ok = ok && appendRaw(body, bodyLen, &used, ",\"led_driver\":\"");
  ok = ok && appendJsonEscaped(body, bodyLen, &used, LedSerial::driverName());
  ok = ok && appendRaw(body, bodyLen, &used, "\"");
  ok = ok && appendFormat(body, bodyLen, &used, ",\"led_pixel_pin\":%d",
                           LedSerial::pixelPin());
  ok = ok && appendFormat(body, bodyLen, &used, ",\"led_pixel_count\":%d",
                           LedSerial::pixelCount());
  ok = ok && appendFormat(body, bodyLen, &used, ",\"led_panel_count\":%d",
                           LedSerial::panelCount());
  ok = ok && appendFormat(body, bodyLen, &used, ",\"led_output_ok\":%s}",
                           LedSerial::outputOk() ? "true" : "false");
  return ok;
}

esp_err_t deviceStatusHandler(httpd_req_t *req) {
  setJsonHeaders(req);
  constexpr size_t kStatusBodyBytes = 1280;
  char *body = static_cast<char *>(heap_caps_malloc(kStatusBodyBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!body) {
    body = static_cast<char *>(heap_caps_malloc(kStatusBodyBytes, MALLOC_CAP_8BIT));
  }
  if (!body) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status oom");
    return ESP_FAIL;
  }
  if (!writeCompactStatusJson(body, kStatusBodyBytes)) {
    heap_caps_free(body);
    Serial.println("[device_api] /device/status overflow");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status overflow");
    return ESP_FAIL;
  }
  esp_err_t res = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
  heap_caps_free(body);
  return res;
}

esp_err_t deviceDebugStatusHandler(httpd_req_t *req) {
  setJsonHeaders(req);
  constexpr size_t kDebugStatusBodyBytes = 4096;
  char *body = static_cast<char *>(heap_caps_malloc(kDebugStatusBodyBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!body) {
    body = static_cast<char *>(heap_caps_malloc(kDebugStatusBodyBytes, MALLOC_CAP_8BIT));
  }
  if (!body) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "debug status oom");
    return ESP_FAIL;
  }
  if (!writeDebugStatusJson(body, kDebugStatusBodyBytes)) {
    heap_caps_free(body);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "debug status overflow");
    return ESP_FAIL;
  }
  esp_err_t res = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
  heap_caps_free(body);
  return res;
}

esp_err_t deviceConfigGetHandler(httpd_req_t *req) {
  setJsonHeaders(req);
  return sendJson(req, buildConfigJson());
}

esp_err_t deviceConfigPostHandler(httpd_req_t *req) {
  setJsonHeaders(req);

  int total = req->content_len;
  if (total <= 0 || total > 1024) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body size");
    return ESP_FAIL;
  }

  cJSON *doc = readJsonBody(req, 1024);
  if (!doc) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    return ESP_FAIL;
  }
  if (!requestAuthorized(doc)) {
    cJSON_Delete(doc);
    Serial.println("[device_api] /device/config rejected: pairing_mismatch");
    return sendForbidden(req, "pairing_mismatch");
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr) {
    cJSON_Delete(doc);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no sensor");
    return ESP_FAIL;
  }

  int applied = 0;
  cJSON *item;

  item = cJSON_GetObjectItemCaseSensitive(doc, "framesize");
  if (cJSON_IsNumber(item)) {
    int v = item->valueint;
    if (v >= 0 && v <= FRAMESIZE_UXGA) {
      sensor->set_framesize(sensor, (framesize_t)v);
      applied++;
    }
  }

  item = cJSON_GetObjectItemCaseSensitive(doc, "jpeg_quality");
  if (cJSON_IsNumber(item)) {
    int v = item->valueint;
    if (v >= 4 && v <= 63) {
      sensor->set_quality(sensor, v);
      applied++;
    }
  }

  item = cJSON_GetObjectItemCaseSensitive(doc, "brightness");
  if (cJSON_IsNumber(item)) {
    sensor->set_brightness(sensor, item->valueint);
    applied++;
  }

  item = cJSON_GetObjectItemCaseSensitive(doc, "contrast");
  if (cJSON_IsNumber(item)) {
    sensor->set_contrast(sensor, item->valueint);
    applied++;
  }

  item = cJSON_GetObjectItemCaseSensitive(doc, "saturation");
  if (cJSON_IsNumber(item)) {
    sensor->set_saturation(sensor, item->valueint);
    applied++;
  }

  item = cJSON_GetObjectItemCaseSensitive(doc, "hmirror");
  if (cJSON_IsBool(item)) {
    sensor->set_hmirror(sensor, cJSON_IsTrue(item) ? 1 : 0);
    applied++;
  }

  item = cJSON_GetObjectItemCaseSensitive(doc, "vflip");
  if (cJSON_IsBool(item)) {
    sensor->set_vflip(sensor, cJSON_IsTrue(item) ? 1 : 0);
    applied++;
  }

  item = cJSON_GetObjectItemCaseSensitive(doc, "speaker_volume");
  if (cJSON_IsNumber(item)) {
    double v = item->valuedouble;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    SpeakerStream::setVolume((float)v);
    applied++;
  }

  item = cJSON_GetObjectItemCaseSensitive(doc, "wake_model");
  if (cJSON_IsString(item) && item->valuestring) {
    if (!MicStream::setWakeModel(item->valuestring)) {
      cJSON_Delete(doc);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unsupported wake_model");
      return ESP_FAIL;
    }
    applied++;
  }

  item = cJSON_GetObjectItemCaseSensitive(doc, "audio_profile");
  if (cJSON_IsString(item) && item->valuestring) {
    if (!MicStream::setAudioProfile(item->valuestring)) {
      cJSON_Delete(doc);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unsupported audio_profile");
      return ESP_FAIL;
    }
    applied++;
  }

  cJSON_Delete(doc);

  char reply[192];
  snprintf(reply, sizeof(reply),
           "{\"ok\":true,\"applied\":%d,\"audio_profile\":\"%s\","
           "\"wake_model\":\"%s\",\"wake_ready\":%s}",
           applied, MicStream::audioProfile(), MicStream::wakeModel(),
           MicStream::isWakeReady() ? "true" : "false");
  return httpd_resp_sendstr(req, reply);
}

esp_err_t deviceLedGetHandler(httpd_req_t *req) {
  setJsonHeaders(req);
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  addLedStatus(root);
  addLedSupportedModes(root);
  return sendJson(req, root);
}

esp_err_t deviceLedPostHandler(httpd_req_t *req) {
  setJsonHeaders(req);

  cJSON *doc = readJsonBody(req, 1024);
  if (!doc) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    return ESP_FAIL;
  }
  if (!requestAuthorized(doc)) {
    cJSON_Delete(doc);
    Serial.println("[device_api] /device/led rejected: pairing_mismatch");
    return sendForbidden(req, "pairing_mismatch");
  }
  if (!LedSerial::isReady()) {
    cJSON_Delete(doc);
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"led_not_ready\"}");
  }

  int applied = 0;

  const cJSON *brightnessJson = cJSON_GetObjectItemCaseSensitive(doc, "brightness");
  if (cJSON_IsNumber(brightnessJson)) {
    int brightness = brightnessJson->valueint;
    if (brightness < 1 || brightness > 255) {
      cJSON_Delete(doc);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "brightness must be 1-255");
      return ESP_FAIL;
    }
    if (!LedSerial::setBrightness(brightness)) {
      cJSON_Delete(doc);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "led brightness apply failed");
      return ESP_FAIL;
    }
    Serial.printf("[device_api] /device/led brightness=%d\n", brightness);
    applied++;
  }

  int mode = -1;
  bool modeRequested = false;
  const cJSON *modeJson = cJSON_GetObjectItemCaseSensitive(doc, "mode");
  modeRequested = modeJson != nullptr;
  if (cJSON_IsNumber(modeJson)) {
    mode = modeJson->valueint;
  } else if (cJSON_IsString(modeJson) && modeJson->valuestring) {
    mode = LedSerial::resolveMode(modeJson->valuestring);
  }
  if (mode < 0) {
    const cJSON *expressionJson = cJSON_GetObjectItemCaseSensitive(doc, "expression");
    modeRequested = modeRequested || expressionJson != nullptr;
    if (!cJSON_IsString(expressionJson) || !expressionJson->valuestring) {
      expressionJson = cJSON_GetObjectItemCaseSensitive(doc, "name");
      modeRequested = modeRequested || expressionJson != nullptr;
    }
    if (cJSON_IsString(expressionJson) && expressionJson->valuestring) {
      mode = LedSerial::resolveMode(expressionJson->valuestring);
    }
  }

  if (mode >= 0) {
    if (mode > LedSerial::maxMode()) {
      cJSON_Delete(doc);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be 0-33");
      return ESP_FAIL;
    }
    if (!LedSerial::setMode(mode)) {
      cJSON_Delete(doc);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "led mode apply failed");
      return ESP_FAIL;
    }
    Serial.printf("[device_api] /device/led mode=%d name=%s\n", mode, LedSerial::modeName(mode));
    applied++;
  } else if (modeRequested) {
    cJSON_Delete(doc);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown led mode");
    return ESP_FAIL;
  }

  cJSON_Delete(doc);

  if (applied == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode/expression or brightness required");
    return ESP_FAIL;
  }

  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddNumberToObject(root, "applied", applied);
  addLedStatus(root);
  addLedSupportedModes(root);
  return sendJson(req, root);
}

esp_err_t devicePairHandler(httpd_req_t *req) {
  setJsonHeaders(req);
  cJSON *doc = readJsonBody(req, 1024);
  if (!doc) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    return ESP_FAIL;
  }
  String ownerId = jsonString(doc, "owner_id");
  String ownerLabel = jsonString(doc, "owner_label");
  String secret = jsonString(doc, "pairing_secret");
  if (ownerId.length() == 0 || secret.length() == 0) {
    cJSON_Delete(doc);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "owner_id and pairing_secret required");
    return ESP_FAIL;
  }

  String pairedOwner;
  String pairedLabel;
  String pairedHash;
  if (NetConfig::loadPairing(pairedOwner, pairedLabel, pairedHash) &&
      !NetConfig::verifyPairing(ownerId, secret)) {
    cJSON_Delete(doc);
    Serial.printf("[device_api] /device/pair rejected owner=%s paired_owner=%s\n", ownerId.c_str(), pairedOwner.c_str());
    return sendForbidden(req, "already_paired");
  }

  bool ok = NetConfig::savePairing(ownerId, ownerLabel, secret);
  cJSON_Delete(doc);
  if (!ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "pair save failed");
    return ESP_FAIL;
  }
  MicStream::claimOwner(ownerId.c_str(), 120000);
  MicStream::closeClients();
  Serial.printf("[device_api] paired owner=%s label=%s\n", ownerId.c_str(), ownerLabel.c_str());
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  addPairingStatus(root);
  return sendJson(req, root);
}

esp_err_t deviceUnpairHandler(httpd_req_t *req) {
  setJsonHeaders(req);
  cJSON *doc = readJsonBody(req, 1024);
  if (!doc) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    return ESP_FAIL;
  }
  if (!requestAuthorized(doc)) {
    cJSON_Delete(doc);
    Serial.println("[device_api] /device/unpair rejected: pairing_mismatch");
    return sendForbidden(req, "pairing_mismatch");
  }
  String ownerId = jsonString(doc, "owner_id");
  cJSON_Delete(doc);
  NetConfig::clearPairing();
  MicStream::releaseOwner(ownerId.c_str());
  MicStream::closeClients();
  Serial.printf("[device_api] unpaired owner=%s\n", ownerId.c_str());
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  addPairingStatus(root);
  return sendJson(req, root);
}

esp_err_t deviceClaimHandler(httpd_req_t *req) {
  setJsonHeaders(req);
  cJSON *doc = readJsonBody(req, 1024);
  if (!doc) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    return ESP_FAIL;
  }
  if (!NetConfig::hasPairing()) {
    cJSON_Delete(doc);
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"not_paired\"}");
  }
  if (!requestAuthorized(doc)) {
    cJSON_Delete(doc);
    Serial.println("[device_api] /device/claim rejected: pairing_mismatch");
    return sendForbidden(req, "pairing_mismatch");
  }
  String ownerId = jsonString(doc, "owner_id");
  const cJSON *ttlJson = cJSON_GetObjectItemCaseSensitive(doc, "ttl_ms");
  uint32_t ttlMs = cJSON_IsNumber(ttlJson) ? (uint32_t)ttlJson->valuedouble : 120000;
  if (ttlMs < 10000) ttlMs = 10000;
  if (ttlMs > 600000) ttlMs = 600000;
  cJSON_Delete(doc);
  MicStream::claimOwner(ownerId.c_str(), ttlMs);
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  addPairingStatus(root);
  return sendJson(req, root);
}

esp_err_t deviceReleaseHandler(httpd_req_t *req) {
  setJsonHeaders(req);
  cJSON *doc = readJsonBody(req, 1024);
  if (!doc) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    return ESP_FAIL;
  }
  if (!requestAuthorized(doc)) {
    cJSON_Delete(doc);
    Serial.println("[device_api] /device/release rejected: pairing_mismatch");
    return sendForbidden(req, "pairing_mismatch");
  }
  String ownerId = jsonString(doc, "owner_id");
  cJSON_Delete(doc);
  MicStream::releaseOwner(ownerId.c_str());
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  addPairingStatus(root);
  return sendJson(req, root);
}

esp_err_t deviceRebootHandler(httpd_req_t *req) {
  setJsonHeaders(req);
  httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"rebooting\"}");

  Serial.println("[device_api] /device/reboot requested");
  delay(500);
  ESP.restart();
  return ESP_OK;
}

esp_err_t deviceForgetWifiHandler(httpd_req_t *req) {
  setJsonHeaders(req);
  cJSON *doc = nullptr;
  if (req->content_len > 0) {
    doc = readJsonBody(req, 1024);
    if (!doc) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
      return ESP_FAIL;
    }
  }
  if (NetConfig::hasPairing() && !requestAuthorized(doc)) {
    if (doc) cJSON_Delete(doc);
    Serial.println("[device_api] /device/forget-wifi rejected: pairing_mismatch");
    return sendForbidden(req, "pairing_mismatch");
  }
  if (doc) cJSON_Delete(doc);
  NetConfig::clearPairing();
  MicStream::releaseOwner(nullptr);
  MicStream::closeClients();
  bool ok = NetConfig::clearWifi();
  char reply[64];
  snprintf(reply, sizeof(reply), "{\"ok\":%s,\"message\":\"will restart\"}", ok ? "true" : "false");
  httpd_resp_sendstr(req, reply);

  Serial.println("[device_api] /device/forget-wifi requested");
  delay(500);
  ESP.restart();
  return ESP_OK;
}

esp_err_t deviceOptionsHandler(httpd_req_t *req) {
  setJsonHeaders(req);
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, NULL, 0);
}

}

namespace DeviceApi {

bool registerHandlers(httpd_handle_t server) {
  if (server == nullptr) {
    Serial.println("[device_api] cannot register, server handle is null");
    return false;
  }

  const httpd_uri_t routes[] = {
      {.uri = "/device/status", .method = HTTP_GET, .handler = deviceStatusHandler, .user_ctx = NULL},
      {.uri = "/device/debug/status", .method = HTTP_GET, .handler = deviceDebugStatusHandler, .user_ctx = NULL},
      {.uri = "/device/config", .method = HTTP_GET, .handler = deviceConfigGetHandler, .user_ctx = NULL},
      {.uri = "/device/config", .method = HTTP_POST, .handler = deviceConfigPostHandler, .user_ctx = NULL},
      {.uri = "/device/led", .method = HTTP_GET, .handler = deviceLedGetHandler, .user_ctx = NULL},
      {.uri = "/device/led", .method = HTTP_POST, .handler = deviceLedPostHandler, .user_ctx = NULL},
      {.uri = "/device/pair", .method = HTTP_POST, .handler = devicePairHandler, .user_ctx = NULL},
      {.uri = "/device/unpair", .method = HTTP_POST, .handler = deviceUnpairHandler, .user_ctx = NULL},
      {.uri = "/device/claim", .method = HTTP_POST, .handler = deviceClaimHandler, .user_ctx = NULL},
      {.uri = "/device/release", .method = HTTP_POST, .handler = deviceReleaseHandler, .user_ctx = NULL},
      {.uri = "/device/reboot", .method = HTTP_POST, .handler = deviceRebootHandler, .user_ctx = NULL},
      {.uri = "/device/forget-wifi", .method = HTTP_POST, .handler = deviceForgetWifiHandler, .user_ctx = NULL},
      {.uri = "/device/status", .method = HTTP_OPTIONS, .handler = deviceOptionsHandler, .user_ctx = NULL},
      {.uri = "/device/debug/status", .method = HTTP_OPTIONS, .handler = deviceOptionsHandler, .user_ctx = NULL},
      {.uri = "/device/config", .method = HTTP_OPTIONS, .handler = deviceOptionsHandler, .user_ctx = NULL},
      {.uri = "/device/led", .method = HTTP_OPTIONS, .handler = deviceOptionsHandler, .user_ctx = NULL},
      {.uri = "/device/pair", .method = HTTP_OPTIONS, .handler = deviceOptionsHandler, .user_ctx = NULL},
      {.uri = "/device/unpair", .method = HTTP_OPTIONS, .handler = deviceOptionsHandler, .user_ctx = NULL},
      {.uri = "/device/claim", .method = HTTP_OPTIONS, .handler = deviceOptionsHandler, .user_ctx = NULL},
      {.uri = "/device/release", .method = HTTP_OPTIONS, .handler = deviceOptionsHandler, .user_ctx = NULL},
      {.uri = "/device/reboot", .method = HTTP_OPTIONS, .handler = deviceOptionsHandler, .user_ctx = NULL},
      {.uri = "/device/forget-wifi", .method = HTTP_OPTIONS, .handler = deviceOptionsHandler, .user_ctx = NULL},
  };

  int registered = 0;
  for (const auto &r : routes) {
    if (httpd_register_uri_handler(server, &r) == ESP_OK) {
      registered++;
    } else {
      Serial.printf("[device_api] failed to register %s\n", r.uri);
    }
  }
  Serial.printf("[device_api] registered %d routes\n", registered);
  return registered == (int)(sizeof(routes) / sizeof(routes[0]));
}

}
