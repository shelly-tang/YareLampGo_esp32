// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "asset_upload_server.h"

#include <LittleFS.h>

#include "board_config.h"
#include "expression_coordinator.h"

namespace {
constexpr size_t kMaxEyeBytes = 512 * 1024;
constexpr size_t kMaxLedBytes = 8 * 1024;
constexpr size_t kReceiveBufferBytes = 1024;
constexpr size_t kMaxHeaderValueBytes = 128;
constexpr char kOwnerHeader[] = "X-Lampgo-Owner";
constexpr char kTokenHeader[] = "X-Lampgo-Token";
constexpr char kClipIdHeader[] = "X-Lampgo-Clip-Id";
constexpr char kEffectIdHeader[] = "X-Lampgo-Effect-Id";

bool readHeader(httpd_req_t* request, const char* name, String& value) {
  const size_t length = httpd_req_get_hdr_value_len(request, name);
  if (length == 0 || length > kMaxHeaderValueBytes) return false;
  char buffer[kMaxHeaderValueBytes + 1]{};
  if (httpd_req_get_hdr_value_str(request, name, buffer, length + 1) != ESP_OK) return false;
  value = String(buffer);
  return true;
}

esp_err_t sendJson(httpd_req_t* request, const char* status, const String& body) {
  httpd_resp_set_status(request, status);
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(request, body.c_str(), body.length());
}

esp_err_t sendError(httpd_req_t* request, const char* status, const char* message) {
  return sendJson(request, status, String("{\"ok\":false,\"error\":\"") + message + "\"}");
}

bool hasExpectedMagic(const String& path, bool eyeAsset) {
  File file = LittleFS.open(path, "r");
  if (!file) return false;
  char magic[6]{};
  const size_t size = eyeAsset ? 6 : 4;
  const bool valid = file.readBytes(magic, size) == size &&
                     (eyeAsset ? memcmp(magic, "LGLCD1", 6) == 0 : memcmp(magic, "LEF1", 4) == 0);
  file.close();
  return valid;
}
}  // namespace

bool AssetUploadServer::begin() {
  uploadMutex_ = xSemaphoreCreateMutex();
  if (!uploadMutex_) return false;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = BoardConfig::kAssetUploadPort;
  config.ctrl_port = 32771;
  config.stack_size = 8192;
  config.max_open_sockets = 2;
  config.max_uri_handlers = 2;
  config.recv_wait_timeout = 15;
  config.send_wait_timeout = 15;
  if (httpd_start(&server_, &config) != ESP_OK) {
    vSemaphoreDelete(uploadMutex_);
    uploadMutex_ = nullptr;
    return false;
  }

  httpd_uri_t eyeRoute{};
  eyeRoute.uri = "/device/expression-clips/upload";
  eyeRoute.method = HTTP_POST;
  eyeRoute.handler = handleEyeUpload;
  eyeRoute.user_ctx = this;
  httpd_uri_t ledRoute{};
  ledRoute.uri = "/device/led-effects/upload";
  ledRoute.method = HTTP_POST;
  ledRoute.handler = handleLedUpload;
  ledRoute.user_ctx = this;
  if (httpd_register_uri_handler(server_, &eyeRoute) != ESP_OK ||
      httpd_register_uri_handler(server_, &ledRoute) != ESP_OK) {
    httpd_stop(server_);
    server_ = nullptr;
    vSemaphoreDelete(uploadMutex_);
    uploadMutex_ = nullptr;
    return false;
  }
  Serial.printf("[ASSET] HTTP streaming server ready port=%u\n", BoardConfig::kAssetUploadPort);
  return true;
}

esp_err_t AssetUploadServer::handleEyeUpload(httpd_req_t* request) {
  return static_cast<AssetUploadServer*>(request->user_ctx)->handleUpload(request, true);
}

esp_err_t AssetUploadServer::handleLedUpload(httpd_req_t* request) {
  return static_cast<AssetUploadServer*>(request->user_ctx)->handleUpload(request, false);
}

esp_err_t AssetUploadServer::handleUpload(httpd_req_t* request, bool eyeAsset) {
  String owner;
  String token;
  String assetId;
  const char* idHeader = eyeAsset ? kClipIdHeader : kEffectIdHeader;
  if (!readHeader(request, kOwnerHeader, owner) || !readHeader(request, kTokenHeader, token) ||
      !readHeader(request, idHeader, assetId) || !pairing_.authorize(owner, token)) {
    return sendError(request, "403 Forbidden", "pairing mismatch");
  }
  if (!ExpressionCoordinator::safeAssetId(assetId)) {
    return sendError(request, "400 Bad Request", "invalid asset id");
  }
  const size_t limit = eyeAsset ? kMaxEyeBytes : kMaxLedBytes;
  if (request->content_len <= 0 || static_cast<size_t>(request->content_len) > limit) {
    return sendError(request, "413 Payload Too Large", "asset exceeds limit");
  }
  if (!uploadMutex_ || xSemaphoreTake(uploadMutex_, pdMS_TO_TICKS(3000)) != pdTRUE) {
    return sendError(request, "409 Conflict", "another asset upload is active");
  }

  const String tempPath = eyeAsset ? "/upload_eye.tmp" : "/upload_led.tmp";
  const String destination = eyeAsset ? ExpressionCoordinator::eyePath(assetId) :
                                       ExpressionCoordinator::ledPath(assetId);
  LittleFS.remove(tempPath);
  File file = LittleFS.open(tempPath, "w");
  if (!file) {
    xSemaphoreGive(uploadMutex_);
    return sendError(request, "500 Internal Server Error", "cannot open upload file");
  }

  uint8_t buffer[kReceiveBufferBytes]{};
  size_t receivedTotal = 0;
  while (receivedTotal < static_cast<size_t>(request->content_len)) {
    const size_t remaining = static_cast<size_t>(request->content_len) - receivedTotal;
    const size_t requested = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    const int received = httpd_req_recv(request, reinterpret_cast<char*>(buffer), requested);
    if (received <= 0 || file.write(buffer, static_cast<size_t>(received)) != static_cast<size_t>(received)) {
      file.close();
      LittleFS.remove(tempPath);
      xSemaphoreGive(uploadMutex_);
      return sendError(request, "400 Bad Request", "asset stream interrupted");
    }
    receivedTotal += static_cast<size_t>(received);
  }
  file.close();

  if (!hasExpectedMagic(tempPath, eyeAsset)) {
    LittleFS.remove(tempPath);
    xSemaphoreGive(uploadMutex_);
    return sendError(request, "400 Bad Request", "invalid asset magic");
  }
  LittleFS.remove(destination);
  if (!LittleFS.rename(tempPath, destination)) {
    LittleFS.remove(tempPath);
    xSemaphoreGive(uploadMutex_);
    return sendError(request, "500 Internal Server Error", "cannot commit uploaded asset");
  }
  xSemaphoreGive(uploadMutex_);

  String response = String("{\"ok\":true,\"action\":\"upload\",\"") +
                    (eyeAsset ? "clip_id" : "effect_id") + "\":\"" + assetId +
                    "\",\"bytes\":" + String(receivedTotal);
  if (eyeAsset) response += ",\"display_confirmed\":true";
  response += "}";
  Serial.printf("[ASSET] upload kind=%s id=%s bytes=%u\n", eyeAsset ? "eye" : "led",
                assetId.c_str(), static_cast<unsigned int>(receivedTotal));
  return sendJson(request, "200 OK", response);
}
