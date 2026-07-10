// SPDX-FileCopyrightText: 2026 @ninsmiracle, @shelly-tang, and LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "device_api.h"

#include <Arduino.h>
#include <WiFi.h>
#include <stdarg.h>
#include <stdlib.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_camera.h>

#include "display_link.h"
#include "expression_clips.h"
#include "expression_store.h"
#include "led_serial.h"
#include "mic_stream.h"
#include "net_config.h"
#include "speaker_stream.h"

namespace {

const char *FIRMWARE_VERSION = "lampgo-cam 0.4.0";

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

bool queryValue(httpd_req_t *req, const char *key, char *out, size_t outLen) {
  if (!req || !key || !out || outLen == 0) return false;
  out[0] = 0;
  size_t queryLen = httpd_req_get_url_query_len(req) + 1;
  if (queryLen <= 1) return false;
  char *query = (char *)malloc(queryLen);
  if (!query) return false;
  bool ok = false;
  if (httpd_req_get_url_query_str(req, query, queryLen) == ESP_OK &&
      httpd_query_key_value(query, key, out, outLen) == ESP_OK) {
    ok = true;
  }
  free(query);
  return ok;
}

String queryString(httpd_req_t *req, const char *key, size_t maxLen = 128) {
  if (maxLen < 2) maxLen = 2;
  char *buf = (char *)malloc(maxLen);
  if (!buf) return String("");
  bool ok = queryValue(req, key, buf, maxLen);
  String value = ok ? String(buf) : String("");
  free(buf);
  return value;
}

uint32_t queryUInt(httpd_req_t *req, const char *key, uint32_t fallback = 0) {
  char buf[24] = {};
  if (!queryValue(req, key, buf, sizeof(buf)) || !buf[0]) return fallback;
  char *end = nullptr;
  unsigned long value = strtoul(buf, &end, 10);
  return end && *end == 0 ? (uint32_t)value : fallback;
}

bool requestAuthorizedQuery(httpd_req_t *req) {
  if (!NetConfig::hasPairing()) return true;
  String ownerId = queryString(req, "owner_id", 96);
  String pairingSecret = queryString(req, "pairing_secret", 128);
  return NetConfig::verifyPairing(ownerId, pairingSecret);
}

esp_err_t sendForbidden(httpd_req_t *req, const char *error) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", false);
  cJSON_AddStringToObject(root, "error", error);
  httpd_resp_set_status(req, "403 Forbidden");
  return sendJson(req, root);
}

esp_err_t sendBadRequestJson(httpd_req_t *req, const char *error) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", false);
  cJSON_AddStringToObject(root, "error", error ? error : "bad request");
  httpd_resp_set_status(req, "400 Bad Request");
  return sendJson(req, root);
}

esp_err_t sendDisplaySyncError(httpd_req_t *req, const char *stage, const char *detail, uint32_t elapsedMs) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", false);
  cJSON_AddStringToObject(root, "error", "display_sync_failed");
  cJSON_AddStringToObject(root, "stage", stage ? stage : "unknown");
  cJSON_AddStringToObject(root, "detail", detail ? detail : "C6 did not confirm transfer");
  cJSON_AddBoolToObject(root, "c6_confirmed", false);
  cJSON_AddNumberToObject(root, "transfer_ms", (double)elapsedMs);
  cJSON_AddBoolToObject(root, "wifi_connected", WiFi.status() == WL_CONNECTED);
  cJSON_AddNumberToObject(root, "wifi_rssi", (double)WiFi.RSSI());
  httpd_resp_set_status(req, "502 Bad Gateway");
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

  bool clipPlayed = false;
  const cJSON *clipJson = cJSON_GetObjectItemCaseSensitive(doc, "clip_id");
  if (cJSON_IsString(clipJson) && clipJson->valuestring && clipJson->valuestring[0]) {
    if (LedSerial::playClip(clipJson->valuestring)) {
      Serial.printf("[device_api] /device/led clip=%s\n", clipJson->valuestring);
      applied++;
      clipPlayed = true;
    } else {
      Serial.printf("[device_api] /device/led clip fallback=%s\n", clipJson->valuestring);
    }
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

  if (!clipPlayed && mode >= 0) {
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
  } else if (!clipPlayed && modeRequested) {
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

bool parseHexColor(const char *value, uint8_t &red, uint8_t &green, uint8_t &blue) {
  if (!value || strlen(value) != 7 || value[0] != '#') return false;
  char channel[3] = {0, 0, 0};
  char *end = nullptr;
  channel[0] = value[1]; channel[1] = value[2];
  long parsedRed = strtol(channel, &end, 16);
  if (!end || *end) return false;
  channel[0] = value[3]; channel[1] = value[4];
  long parsedGreen = strtol(channel, &end, 16);
  if (!end || *end) return false;
  channel[0] = value[5]; channel[1] = value[6];
  long parsedBlue = strtol(channel, &end, 16);
  if (!end || *end) return false;
  red = (uint8_t)parsedRed;
  green = (uint8_t)parsedGreen;
  blue = (uint8_t)parsedBlue;
  return true;
}

void applyEffectParams(const cJSON *params,
                       String &direction,
                       uint8_t &red,
                       uint8_t &green,
                       uint8_t &blue,
                       uint8_t &secondaryRed,
                       uint8_t &secondaryGreen,
                       uint8_t &secondaryBlue,
                       uint8_t &brightness,
                       uint8_t &intensityPercent) {
  if (!cJSON_IsObject(params)) return;
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "direction");
  if (cJSON_IsString(item) && item->valuestring) direction = item->valuestring;
  item = cJSON_GetObjectItemCaseSensitive(params, "color");
  if (cJSON_IsString(item) && item->valuestring) parseHexColor(item->valuestring, red, green, blue);
  item = cJSON_GetObjectItemCaseSensitive(params, "secondary_color");
  if (cJSON_IsString(item) && item->valuestring) {
    parseHexColor(item->valuestring, secondaryRed, secondaryGreen, secondaryBlue);
  }
  item = cJSON_GetObjectItemCaseSensitive(params, "brightness");
  if (cJSON_IsNumber(item)) brightness = (uint8_t)constrain(item->valueint, 1, 96);
  item = cJSON_GetObjectItemCaseSensitive(params, "intensity");
  if (cJSON_IsNumber(item)) intensityPercent = (uint8_t)constrain((int)(item->valuedouble * 100.0), 10, 100);
}

esp_err_t deviceExpressionPlayHandler(httpd_req_t *req) {
  setJsonHeaders(req);
  size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (internalFree < 64 * 1024 || largestBlock < 32 * 1024) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"expression_heap_guard\"}");
  }

  cJSON *doc = readJsonBody(req, ExpressionStore::kSingleLedBytes + 2048);
  if (!doc) return sendBadRequestJson(req, "bad expression JSON");
  if (!requestAuthorized(doc)) {
    cJSON_Delete(doc);
    return sendForbidden(req, "pairing_mismatch");
  }

  String eyeClipId = jsonString(doc, "eye_clip_id");
  String ledEffectId = jsonString(doc, "led_effect_id");
  String presetId = jsonString(doc, "preset_id");
  String playback = jsonString(doc, "playback");
  bool loopPlayback = playback == "loop";
  const cJSON *durationItem = cJSON_GetObjectItemCaseSensitive(doc, "duration_ms");
  uint32_t durationMs = cJSON_IsNumber(durationItem) ? (uint32_t)durationItem->valuedouble : 3000;
  if (durationMs < 2500 || durationMs > 3500) {
    cJSON_Delete(doc);
    return sendBadRequestJson(req, "duration_ms must be 2500-3500");
  }

  const cJSON *modeItem = cJSON_GetObjectItemCaseSensitive(doc, "led_mode");
  const cJSON *program = cJSON_GetObjectItemCaseSensitive(doc, "led_program");
  const cJSON *params = cJSON_GetObjectItemCaseSensitive(doc, "led_params");
  bool ledOk = true;
  bool hasLed = ledEffectId.length() > 0;
  if (cJSON_IsNumber(modeItem)) {
    int mode = modeItem->valueint;
    int safeBrightness = LedSerial::currentBrightness();
    const cJSON *brightnessItem = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "brightness") : nullptr;
    if (cJSON_IsNumber(brightnessItem)) safeBrightness = constrain(brightnessItem->valueint, 1, 96);
    ledOk = LedSerial::setBrightness(safeBrightness) && mode >= 0 && mode <= LedSerial::maxMode() &&
            LedSerial::setModeLocal(mode, loopPlayback, durationMs);
  } else if (cJSON_IsObject(program) && hasLed) {
    const cJSON *templateItem = cJSON_GetObjectItemCaseSensitive(program, "template");
    const cJSON *variantItem = cJSON_GetObjectItemCaseSensitive(program, "variant");
    const cJSON *defaults = cJSON_GetObjectItemCaseSensitive(program, "defaults");
    String templateName = cJSON_IsString(templateItem) && templateItem->valuestring ? templateItem->valuestring : "";
    String variant = cJSON_IsString(variantItem) && variantItem->valuestring ? variantItem->valuestring : "";
    if (templateName != "mouth" && templateName != "arrow" && templateName != "heart" && templateName != "pulse") {
      cJSON_Delete(doc);
      return sendBadRequestJson(req, "unsupported LED template");
    }
    String direction = "right";
    uint8_t red = 255, green = 255, blue = 255;
    uint8_t secondaryRed = 255, secondaryGreen = 45, secondaryBlue = 125;
    uint8_t brightness = (uint8_t)LedSerial::currentBrightness();
    uint8_t intensityPercent = 100;
    applyEffectParams(defaults, direction, red, green, blue,
                      secondaryRed, secondaryGreen, secondaryBlue, brightness, intensityPercent);
    applyEffectParams(params, direction, red, green, blue,
                      secondaryRed, secondaryGreen, secondaryBlue, brightness, intensityPercent);
    if (direction != "left" && direction != "right" && direction != "up" && direction != "down") {
      cJSON_Delete(doc);
      return sendBadRequestJson(req, "unsupported LED direction");
    }

    char *programJson = cJSON_PrintUnformatted(program);
    if (!programJson || !ExpressionStore::saveLedEffect(ledEffectId.c_str(), programJson, strlen(programJson))) {
      if (programJson) cJSON_free(programJson);
      String error = ExpressionStore::lastError();
      cJSON_Delete(doc);
      return sendBadRequestJson(req, error.c_str());
    }
    cJSON_free(programJson);
    LedSerial::EffectConfig config{
        ledEffectId.c_str(), templateName.c_str(), variant.c_str(), direction.c_str(),
        red, green, blue, secondaryRed, secondaryGreen, secondaryBlue,
        brightness, intensityPercent, loopPlayback, durationMs};
    ledOk = LedSerial::playEffect(config);
  } else if (hasLed) {
    int mode = LedSerial::resolveMode(ledEffectId.c_str());
    int safeBrightness = LedSerial::currentBrightness();
    const cJSON *brightnessItem = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "brightness") : nullptr;
    if (cJSON_IsNumber(brightnessItem)) safeBrightness = constrain(brightnessItem->valueint, 1, 96);
    ledOk = LedSerial::setBrightness(safeBrightness) && mode >= 0 &&
            LedSerial::setModeLocal(mode, loopPlayback, durationMs);
  } else {
    ledOk = LedSerial::setModeLocal(0, true, 0);
  }

  if (!ledOk) {
    cJSON_Delete(doc);
    return sendBadRequestJson(req, "LED expression apply failed");
  }

  if (presetId.length()) {
    cJSON *storedPreset = cJSON_CreateObject();
    cJSON_AddStringToObject(storedPreset, "preset_id", presetId.c_str());
    cJSON_AddStringToObject(storedPreset, "eye_clip_id", eyeClipId.c_str());
    cJSON_AddStringToObject(storedPreset, "led_effect_id", ledEffectId.c_str());
    cJSON_AddStringToObject(storedPreset, "playback", loopPlayback ? "loop" : "once");
    cJSON_AddNumberToObject(storedPreset, "duration_ms", durationMs);
    char *presetJson = cJSON_PrintUnformatted(storedPreset);
    cJSON_Delete(storedPreset);
    if (!presetJson || !ExpressionStore::savePreset(presetId.c_str(), presetJson, strlen(presetJson))) {
      if (presetJson) cJSON_free(presetJson);
      String error = ExpressionStore::lastError();
      cJSON_Delete(doc);
      return sendBadRequestJson(req, error.c_str());
    }
    cJSON_free(presetJson);
  }

  if (eyeClipId.length()) {
    DisplayLink::sendClipPlay(eyeClipId.c_str(), loopPlayback);
  }
  cJSON_Delete(doc);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddStringToObject(root, "eye_clip_id", eyeClipId.c_str());
  cJSON_AddStringToObject(root, "led_effect_id", ledEffectId.c_str());
  cJSON_AddStringToObject(root, "playback", loopPlayback ? "loop" : "once");
  cJSON_AddNumberToObject(root, "duration_ms", durationMs);
  cJSON_AddNumberToObject(root, "internal_free_heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return sendJson(req, root);
}

esp_err_t deviceExpressionStopHandler(httpd_req_t *req) {
  setJsonHeaders(req);
  cJSON *doc = readJsonBody(req, 1024);
  if (!doc) return sendBadRequestJson(req, "bad JSON");
  if (!requestAuthorized(doc)) {
    cJSON_Delete(doc);
    return sendForbidden(req, "pairing_mismatch");
  }
  cJSON_Delete(doc);
  bool ok = LedSerial::stopExpression(true);
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", ok);
  cJSON_AddStringToObject(root, "action", "stop");
  return sendJson(req, root);
}

esp_err_t deviceExpressionCapabilitiesHandler(httpd_req_t *req) {
  setJsonHeaders(req);
  ExpressionStore::Capacity capacity = ExpressionStore::capacity();
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON *result = cJSON_AddObjectToObject(root, "result");
  cJSON_AddNumberToObject(result, "spiffs_total_bytes", capacity.fsTotalBytes);
  cJSON_AddNumberToObject(result, "spiffs_used_bytes", capacity.fsUsedBytes);
  cJSON_AddNumberToObject(result, "spiffs_free_bytes", capacity.fsFreeBytes);
  cJSON_AddNumberToObject(result, "spiffs_reserved_bytes", ExpressionStore::kFsReservedBytes);
  cJSON_AddNumberToObject(result, "lcd_staging_max_bytes", ExpressionStore::kLcdStagingBytes);
  cJSON_AddNumberToObject(result, "led_effect_used_bytes", capacity.ledUsedBytes);
  cJSON_AddNumberToObject(result, "led_effect_budget_bytes", ExpressionStore::kLedBudgetBytes);
  cJSON_AddNumberToObject(result, "led_effect_count", capacity.ledCount);
  cJSON_AddNumberToObject(result, "led_effect_max_count", ExpressionStore::kMaxLedEffects);
  cJSON_AddNumberToObject(result, "preset_used_bytes", capacity.presetUsedBytes);
  cJSON_AddNumberToObject(result, "preset_budget_bytes", ExpressionStore::kPresetBudgetBytes);
  cJSON_AddNumberToObject(result, "preset_count", capacity.presetCount);
  cJSON_AddNumberToObject(result, "preset_max_count", ExpressionStore::kMaxPresets);
  cJSON_AddNumberToObject(result, "c6_fs_total_bytes", DisplayLink::c6FsTotalBytes());
  cJSON_AddNumberToObject(result, "c6_fs_used_bytes", DisplayLink::c6FsUsedBytes());
  cJSON_AddNumberToObject(result, "c6_eye_installed_bytes", DisplayLink::c6InstalledBytes());
  cJSON_AddNumberToObject(result, "c6_eye_installed_count", DisplayLink::c6InstalledCount());
  cJSON_AddNumberToObject(result, "c6_eye_max_count", DisplayLink::c6MaxClipCount());
  cJSON_AddNumberToObject(result, "c6_eye_budget_bytes", DisplayLink::c6InstalledBudgetBytes());
  cJSON_AddNumberToObject(result, "internal_free_heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  cJSON_AddNumberToObject(result, "internal_largest_free_block",
                          heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return sendJson(req, root);
}

esp_err_t deviceExpressionClipSyncHandler(httpd_req_t *req) {
  setJsonHeaders(req);

  cJSON *doc = readJsonBody(req, 4096);
  if (!doc) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    return ESP_FAIL;
  }
  if (!requestAuthorized(doc)) {
    cJSON_Delete(doc);
    Serial.println("[device_api] /device/expression-clips/sync rejected: pairing_mismatch");
    return sendForbidden(req, "pairing_mismatch");
  }

  String action = jsonString(doc, "action");
  String clipId = jsonString(doc, "clip_id");
  bool ok = false;
  String displayError;

  if (action == "begin") {
    String expression = jsonString(doc, "expression");
    int fps = 0;
    int frameCount = 0;
    int durationMs = 0;
    size_t lcdBytes = 0;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(doc, "fps");
    if (cJSON_IsNumber(item)) fps = item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(doc, "frame_count");
    if (cJSON_IsNumber(item)) frameCount = item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(doc, "duration_ms");
    if (cJSON_IsNumber(item)) durationMs = item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(doc, "lcd_bytes");
    if (cJSON_IsNumber(item)) lcdBytes = (size_t)item->valuedouble;
    String lcdSha = jsonString(doc, "lcd_sha256");
    ok = ExpressionClips::beginSync(clipId.c_str(),
                                    expression.c_str(),
                                    fps,
                                    frameCount,
                                    durationMs,
                                    lcdBytes,
                                    lcdSha.c_str());
    if (ok && !DisplayLink::sendClipBegin(clipId.c_str(), fps, frameCount, durationMs, lcdBytes, lcdSha.c_str())) {
      ok = false;
      displayError = DisplayLink::lastClipError();
    }
  } else if (action == "chunk") {
    String target = jsonString(doc, "target");
    String data = jsonString(doc, "data");
    size_t offset = 0;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(doc, "offset");
    if (cJSON_IsNumber(item)) offset = (size_t)item->valuedouble;
    ok = ExpressionClips::appendChunk(clipId.c_str(), target.c_str(), offset, data.c_str());
    if (ok && target == "lcd" && !DisplayLink::sendClipChunk(clipId.c_str(), offset, data.c_str())) {
      ok = false;
      displayError = DisplayLink::lastClipError();
    }
  } else if (action == "commit") {
    ok = ExpressionClips::commitSync(clipId.c_str());
    if (ok && !DisplayLink::sendClipCommit(clipId.c_str())) {
      ok = false;
      displayError = DisplayLink::lastClipError();
    }
    if (ok) ExpressionClips::releaseLcdPayload(clipId.c_str());
  } else if (action == "delete") {
    ok = ExpressionClips::removeClip(clipId.c_str());
  } else {
    cJSON_Delete(doc);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown sync action");
    return ESP_FAIL;
  }

  cJSON_Delete(doc);
  if (!ok) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", displayError.length() ? displayError.c_str() : ExpressionClips::lastError());
    httpd_resp_set_status(req, "400 Bad Request");
    return sendJson(req, root);
  }

  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddStringToObject(root, "clip_id", clipId.c_str());
  cJSON_AddStringToObject(root, "action", action.c_str());
  return sendJson(req, root);
}

esp_err_t deviceExpressionClipUploadHandler(httpd_req_t *req) {
  const uint32_t transferStartedMs = millis();
  setJsonHeaders(req);
  if (!requestAuthorizedQuery(req)) {
    Serial.println("[device_api] /device/expression-clips/upload rejected: pairing_mismatch");
    return sendForbidden(req, "pairing_mismatch");
  }
  if (req->content_len <= 0) {
    return sendBadRequestJson(req, "empty upload body");
  }
  if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 64 * 1024 ||
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 32 * 1024) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"expression_heap_guard\"}");
  }

  String clipId = queryString(req, "clip_id", 48);
  String expression = queryString(req, "expression", 48);
  String lcdSha = queryString(req, "lcd_sha256", 72);
  int fps = (int)queryUInt(req, "fps", 0);
  int frameCount = (int)queryUInt(req, "frame_count", 0);
  int durationMs = (int)queryUInt(req, "duration_ms", 0);
  size_t lcdBytes = (size_t)queryUInt(req, "lcd_bytes", 0);

  if (!clipId.length() || fps <= 0 || frameCount <= 0 || durationMs <= 0 || lcdBytes == 0 || !lcdSha.length()) {
    return sendBadRequestJson(req, "missing clip metadata");
  }
  if ((size_t)req->content_len != lcdBytes) {
    Serial.printf("[device_api] upload size mismatch clip=%s body=%d expected=%u\n",
                  clipId.c_str(), req->content_len, (unsigned)lcdBytes);
    return sendBadRequestJson(req, "upload size mismatch");
  }
  if (expression.length() == 0) {
    expression = clipId;
  }

  if (!ExpressionClips::beginSync(clipId.c_str(),
                                  expression.c_str(),
                                  fps,
                                  frameCount,
                                  durationMs,
                                  lcdBytes,
                                  lcdSha.c_str())) {
    return sendBadRequestJson(req, ExpressionClips::lastError());
  }
  bool displayOk = DisplayLink::sendClipBegin(clipId.c_str(), fps, frameCount, durationMs, lcdBytes, lcdSha.c_str());
  String displayStage = displayOk ? String("") : String("begin");
  String displayError = displayOk ? String("") : String(DisplayLink::lastClipError());

  uint8_t buffer[1024];
  size_t received = 0;
  size_t uartChunks = 0;
  constexpr size_t kDisplayChunkBytes = 256;
  while (received < lcdBytes) {
    size_t want = lcdBytes - received;
    if (want > sizeof(buffer)) want = sizeof(buffer);
    int read = httpd_req_recv(req, (char *)buffer, want);
    if (read == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;
    }
    if (read <= 0) {
      Serial.printf("[device_api] upload recv failed clip=%s received=%u expected=%u err=%d\n",
                    clipId.c_str(), (unsigned)received, (unsigned)lcdBytes, read);
      return sendBadRequestJson(req, "upload receive failed");
    }

    if (!ExpressionClips::appendBytes(clipId.c_str(), "lcd", received, buffer, (size_t)read)) {
      Serial.printf("[device_api] upload append failed clip=%s offset=%u error=%s\n",
                    clipId.c_str(), (unsigned)received, ExpressionClips::lastError());
      return sendBadRequestJson(req, ExpressionClips::lastError());
    }

    if (displayOk) {
      size_t relayed = 0;
      while (relayed < (size_t)read) {
        size_t part = (size_t)read - relayed;
        if (part > kDisplayChunkBytes) part = kDisplayChunkBytes;
        if (!DisplayLink::sendClipChunkBytes(clipId.c_str(), received + relayed, buffer + relayed, part)) {
          displayOk = false;
          displayStage = "chunk";
          displayError = DisplayLink::lastClipError();
          break;
        }
        relayed += part;
        uartChunks++;
      }
    }
    received += (size_t)read;
  }

  bool ok = ExpressionClips::commitSync(clipId.c_str());
  if (!ok) {
    return sendBadRequestJson(req, ExpressionClips::lastError());
  }
  if (displayOk && !DisplayLink::sendClipCommit(clipId.c_str(), received)) {
    displayOk = false;
    displayStage = "commit";
    displayError = DisplayLink::lastClipError();
  }
  if (!displayOk) {
    ExpressionClips::releaseLcdPayload(clipId.c_str());
    Serial.printf("[device_api] C6 sync failed clip=%s stage=%s detail=%s\n",
                  clipId.c_str(), displayStage.c_str(), displayError.c_str());
    return sendDisplaySyncError(req, displayStage.c_str(), displayError.c_str(), millis() - transferStartedMs);
  }
  if (!ExpressionClips::releaseLcdPayload(clipId.c_str())) {
    Serial.printf("[device_api] staging cleanup warning clip=%s error=%s\n",
                  clipId.c_str(), ExpressionClips::lastError());
  }

  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddStringToObject(root, "clip_id", clipId.c_str());
  cJSON_AddStringToObject(root, "action", "upload");
  cJSON_AddNumberToObject(root, "bytes", (double)received);
  cJSON_AddNumberToObject(root, "uart_chunks", (double)uartChunks);
  cJSON_AddBoolToObject(root, "c6_confirmed", true);
  cJSON_AddNumberToObject(root, "transfer_ms", (double)(millis() - transferStartedMs));
  cJSON_AddBoolToObject(root, "wifi_connected", WiFi.status() == WL_CONNECTED);
  cJSON_AddNumberToObject(root, "wifi_rssi", (double)WiFi.RSSI());
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
      {.uri = "/device/expressions/play", .method = HTTP_POST, .handler = deviceExpressionPlayHandler, .user_ctx = NULL},
      {.uri = "/device/expressions/stop", .method = HTTP_POST, .handler = deviceExpressionStopHandler, .user_ctx = NULL},
      {.uri = "/device/expression-capabilities", .method = HTTP_GET, .handler = deviceExpressionCapabilitiesHandler, .user_ctx = NULL},
      {.uri = "/device/expression-clips/sync", .method = HTTP_POST, .handler = deviceExpressionClipSyncHandler, .user_ctx = NULL},
      {.uri = "/device/expression-clips/upload", .method = HTTP_POST, .handler = deviceExpressionClipUploadHandler, .user_ctx = NULL},
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
      {.uri = "/device/expressions/play", .method = HTTP_OPTIONS, .handler = deviceOptionsHandler, .user_ctx = NULL},
      {.uri = "/device/expressions/stop", .method = HTTP_OPTIONS, .handler = deviceOptionsHandler, .user_ctx = NULL},
      {.uri = "/device/expression-capabilities", .method = HTTP_OPTIONS, .handler = deviceOptionsHandler, .user_ctx = NULL},
      {.uri = "/device/expression-clips/sync", .method = HTTP_OPTIONS, .handler = deviceOptionsHandler, .user_ctx = NULL},
      {.uri = "/device/expression-clips/upload", .method = HTTP_OPTIONS, .handler = deviceOptionsHandler, .user_ctx = NULL},
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
