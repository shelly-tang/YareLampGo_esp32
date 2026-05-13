#include "device_api.h"

#include <Arduino.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_camera.h>

#include "mic_stream.h"
#include "net_config.h"
#include "speaker_stream.h"

namespace {

const char *FIRMWARE_VERSION = "lampgo-cam 0.1.0";

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
  cJSON_AddBoolToObject(root, "speaker_streaming", SpeakerStream::isRunning());
  cJSON_AddNumberToObject(root, "speaker_volume", SpeakerStream::getVolume());

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

  char buf[1025];
  int received = 0;
  while (received < total) {
    int r = httpd_req_recv(req, buf + received, total - received);
    if (r <= 0) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
      return ESP_FAIL;
    }
    received += r;
  }
  buf[received] = 0;

  cJSON *doc = cJSON_Parse(buf);
  if (!doc) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    return ESP_FAIL;
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

  cJSON_Delete(doc);

  char reply[64];
  snprintf(reply, sizeof(reply), "{\"ok\":true,\"applied\":%d}", applied);
  return httpd_resp_sendstr(req, reply);
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
      {.uri = "/device/reboot", .method = HTTP_POST, .handler = deviceRebootHandler, .user_ctx = NULL},
      {.uri = "/device/forget-wifi", .method = HTTP_POST, .handler = deviceForgetWifiHandler, .user_ctx = NULL},
      {.uri = "/device/status", .method = HTTP_OPTIONS, .handler = deviceOptionsHandler, .user_ctx = NULL},
      {.uri = "/device/config", .method = HTTP_OPTIONS, .handler = deviceOptionsHandler, .user_ctx = NULL},
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
