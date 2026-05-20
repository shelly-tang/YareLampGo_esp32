#include "device_api.h"

#include <Arduino.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_camera.h>

#include "led_serial.h"
#include "mic_stream.h"
#include "net_config.h"
#include "speaker_stream.h"

namespace {

const char *FIRMWARE_VERSION = "lampgo-cam 0.3.2";

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
  for (int mode = 0; mode <= 29; ++mode) {
    cJSON *item = cJSON_CreateObject();
    if (!item) continue;
    cJSON_AddNumberToObject(item, "mode", mode);
    cJSON_AddStringToObject(item, "name", LedSerial::modeName(mode));
    cJSON_AddItemToArray(modes, item);
  }
}

esp_err_t deviceStatusHandler(httpd_req_t *req) {
  setJsonHeaders(req);

  sensor_t *sensor = esp_camera_sensor_get();
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "configured", true);
  cJSON_AddStringToObject(root, "mode", "work");
  cJSON_AddStringToObject(root, "firmware", FIRMWARE_VERSION);
  cJSON_AddStringToObject(root, "hostname", NetConfig::deviceHostname().c_str());
  addPairingStatus(root);
  cJSON_AddStringToObject(root, "ip", WiFi.localIP().toString().c_str());
  cJSON_AddStringToObject(root, "mac", macStr);
  cJSON_AddNumberToObject(root, "rssi", WiFi.RSSI());
  cJSON_AddStringToObject(root, "ssid", WiFi.SSID().c_str());
  cJSON_AddNumberToObject(root, "uptime_ms", (double)millis());
  cJSON_AddNumberToObject(root, "free_heap", (double)ESP.getFreeHeap());
  cJSON_AddNumberToObject(root, "free_psram", (double)ESP.getFreePsram());
  cJSON_AddNumberToObject(root, "internal_free_heap",
                          (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  cJSON_AddNumberToObject(root, "internal_min_free_heap",
                          (double)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  cJSON_AddNumberToObject(root, "internal_largest_free_block",
                          (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  cJSON_AddNumberToObject(root, "psram_free_heap",
                          (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  cJSON_AddNumberToObject(root, "psram_min_free_heap",
                          (double)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  cJSON_AddNumberToObject(root, "psram_largest_free_block",
                          (double)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  cJSON_AddBoolToObject(root, "mic_streaming", MicStream::isRunning());
  cJSON_AddBoolToObject(root, "mic_aec_ready", MicStream::isAecReady());
  cJSON_AddNumberToObject(root, "mic_ws_clients", MicStream::clientCount());
  cJSON_AddNumberToObject(root, "mic_bytes_read", (double)MicStream::bytesRead());
  cJSON_AddNumberToObject(root, "mic_frames_sent", (double)MicStream::framesSent());
  cJSON_AddBoolToObject(root, "wake_ready", MicStream::isWakeReady());
  cJSON_AddStringToObject(root, "wake_model", MicStream::wakeModel());
  cJSON_AddStringToObject(root, "wake_requested_model", MicStream::requestedWakeModel());
  cJSON *wakeModels = cJSON_AddArrayToObject(root, "wake_supported_models");
  if (wakeModels) {
    cJSON_AddItemToArray(wakeModels, cJSON_CreateString("wn9_jarvis_tts"));
    cJSON_AddItemToArray(wakeModels, cJSON_CreateString("wn9_xiaomeitongxue_tts"));
    cJSON_AddItemToArray(wakeModels, cJSON_CreateString("wn9_xiaoyaxiaoya_tts2"));
    cJSON_AddItemToArray(wakeModels, cJSON_CreateString("wn9_xiaoluxiaolu_tts2"));
    cJSON_AddItemToArray(wakeModels, cJSON_CreateString("wn9_hixiaoxing_tts"));
  }
  cJSON_AddNumberToObject(root, "wake_detections", (double)MicStream::wakeDetections());
  cJSON_AddNumberToObject(root, "wake_last_ms", (double)MicStream::lastWakeMs());
  cJSON_AddNumberToObject(root, "wake_event_clients", MicStream::wakeEventClientCount());
  cJSON_AddNumberToObject(root, "wake_threshold", MicStream::wakeThreshold());
  cJSON_AddNumberToObject(root, "wake_afe_gain", MicStream::wakeAfeGain());
  cJSON_AddNumberToObject(root, "wake_detection_mode", MicStream::wakeDetectionMode());
  cJSON_AddNumberToObject(root, "wake_afe_feed_samples", MicStream::afeFeedSamples());
  cJSON_AddNumberToObject(root, "wake_afe_feed_channels", MicStream::afeFeedChannels());
  cJSON_AddNumberToObject(root, "mic_push_task_create_result", MicStream::pushTaskCreateResult());
  cJSON_AddNumberToObject(root, "wake_fetch_task_create_result", MicStream::fetchTaskCreateResult());
  cJSON_AddBoolToObject(root, "wake_inline_fetch", MicStream::isInlineFetch());
  cJSON_AddNumberToObject(root, "wake_afe_feeds", (double)MicStream::afeFeeds());
  cJSON_AddNumberToObject(root, "wake_afe_fetch_attempts", (double)MicStream::afeFetchAttempts());
  cJSON_AddNumberToObject(root, "wake_afe_fetches", (double)MicStream::afeFetches());
  cJSON_AddNumberToObject(root, "wake_afe_fetch_nulls", (double)MicStream::afeFetchNulls());
  cJSON_AddNumberToObject(root, "wake_afe_last_ret", MicStream::lastAfeRet());
  cJSON_AddNumberToObject(root, "wake_afe_last_ms", (double)MicStream::lastAfeMs());
  cJSON_AddNumberToObject(root, "wake_afe_last_state", MicStream::lastWakeupState());
  cJSON_AddNumberToObject(root, "wake_afe_last_word_index", MicStream::lastWakeWordIndex());
  cJSON_AddNumberToObject(root, "wake_afe_last_trigger_channel", MicStream::lastTriggerChannel());
  cJSON_AddNumberToObject(root, "wake_afe_last_vad_state", MicStream::lastVadState());
  cJSON_AddNumberToObject(root, "wake_afe_last_volume_db", MicStream::lastAfeVolumeDb());
  cJSON_AddNumberToObject(root, "wake_afe_ring_free_pct", MicStream::lastAfeRingFreePct());
  cJSON_AddNumberToObject(root, "mic_last_rms", (double)MicStream::lastMicRms());
  cJSON_AddNumberToObject(root, "mic_last_peak", (double)MicStream::lastMicPeak());
  cJSON_AddBoolToObject(root, "speaker_streaming", SpeakerStream::isRunning());
  cJSON_AddNumberToObject(root, "speaker_volume", SpeakerStream::getVolume());
  addLedStatus(root);

  if (sensor != nullptr) {
    cJSON_AddNumberToObject(root, "framesize", sensor->status.framesize);
    cJSON_AddNumberToObject(root, "jpeg_quality", sensor->status.quality);
    cJSON_AddNumberToObject(root, "brightness", sensor->status.brightness);
    cJSON_AddNumberToObject(root, "contrast", sensor->status.contrast);
    cJSON_AddNumberToObject(root, "saturation", sensor->status.saturation);
    cJSON_AddBoolToObject(root, "hmirror", sensor->status.hmirror);
    cJSON_AddBoolToObject(root, "vflip", sensor->status.vflip);
  }

  return sendJson(req, root);
}

esp_err_t deviceConfigGetHandler(httpd_req_t *req) {
  return deviceStatusHandler(req);
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

  cJSON_Delete(doc);

  char reply[64];
  snprintf(reply, sizeof(reply), "{\"ok\":true,\"applied\":%d}", applied);
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
    if (mode > 29) {
      cJSON_Delete(doc);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be 0-29");
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
