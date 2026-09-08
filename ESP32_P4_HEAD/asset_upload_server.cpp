// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "asset_upload_server.h"

#include <LittleFS.h>

#include "board_config.h"
#include "expression_coordinator.h"

namespace {
constexpr size_t kMaxEyeBytes = 512 * 1024;
constexpr size_t kMaxLedBytes = 8 * 1024;
constexpr size_t kMaxChunkBytes = 1024;
constexpr size_t kMaxHeaderValueBytes = 128;
constexpr char kOwnerHeader[] = "X-Lampgo-Owner";
constexpr char kAuthPurposeHeader[] = "X-Lampgo-Auth-Purpose";
constexpr char kAuthNonceHeader[] = "X-Lampgo-Auth-Nonce";
constexpr char kAuthProofHeader[] = "X-Lampgo-Auth-Proof";
constexpr char kClipIdHeader[] = "X-Lampgo-Clip-Id";
constexpr char kEffectIdHeader[] = "X-Lampgo-Effect-Id";
constexpr char kUploadPhaseHeader[] = "X-Lampgo-Upload-Phase";

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

bool AssetUploadServer::begin(httpd_handle_t server) {
  if (!server) return false;
  uploadMutex_ = xSemaphoreCreateMutex();
  if (!uploadMutex_) return false;

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
  if (httpd_register_uri_handler(server, &eyeRoute) != ESP_OK ||
      httpd_register_uri_handler(server, &ledRoute) != ESP_OK) {
    vSemaphoreDelete(uploadMutex_);
    uploadMutex_ = nullptr;
    return false;
  }
  ready_ = true;
  Serial.printf("[ASSET] HTTP chunk upload routes ready port=%u chunk=%u\n",
                BoardConfig::kAssetUploadPort, static_cast<unsigned int>(kMaxChunkBytes));
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
  String authPurpose;
  String nonce;
  String proof;
  String assetId;
  String phase;
  const UploadKind kind = eyeAsset ? UploadKind::kEye : UploadKind::kLed;
  const char* idHeader = eyeAsset ? kClipIdHeader : kEffectIdHeader;
  if (!readHeader(request, kOwnerHeader, owner) || !readHeader(request, kAuthPurposeHeader, authPurpose) ||
      !readHeader(request, kAuthNonceHeader, nonce) || !readHeader(request, kAuthProofHeader, proof) ||
      !readHeader(request, idHeader, assetId) || !readHeader(request, kUploadPhaseHeader, phase)) {
    return sendError(request, "403 Forbidden", "pairing mismatch");
  }
  const String expectedPurpose = String("asset:POST:") + request->uri + ":" + phase;
  if (authPurpose != expectedPurpose || !pairing_.authorizeProof(owner, expectedPurpose, nonce, proof)) {
    return sendError(request, "403 Forbidden", "pairing mismatch");
  }
  if (!ExpressionCoordinator::safeAssetId(assetId)) {
    return sendError(request, "400 Bad Request", "invalid asset id");
  }
  if (!uploadMutex_ || xSemaphoreTake(uploadMutex_, pdMS_TO_TICKS(3000)) != pdTRUE) {
    return sendError(request, "409 Conflict", "another asset upload is active");
  }

  esp_err_t result = ESP_OK;
  if (phase == "start") {
    if (request->content_len != 0 || !beginUpload(kind, assetId)) {
      result = sendError(request, "400 Bad Request", "cannot start asset upload");
    } else {
      result = sendJson(request, "200 OK", "{\"ok\":true,\"action\":\"started\"}");
    }
  } else if (phase == "chunk") {
    if (!appendChunk(kind, assetId, request)) {
      clearUpload(true);
      result = sendError(request, "400 Bad Request", "asset chunk rejected");
    } else {
      result = sendJson(request, "200 OK", "{\"ok\":true,\"action\":\"chunk\"}");
    }
  } else if (phase == "finish") {
    if (request->content_len != 0) {
      result = sendError(request, "400 Bad Request", "finish body must be empty");
    } else {
      result = finishUpload(request, kind, assetId);
    }
  } else {
    result = sendError(request, "400 Bad Request", "invalid upload phase");
  }
  xSemaphoreGive(uploadMutex_);
  return result;
}

bool AssetUploadServer::beginUpload(UploadKind kind, const String& assetId) {
  clearUpload(true);
  activeKind_ = kind;
  activeId_ = assetId;
  activeTempPath_ = kind == UploadKind::kEye ? "/upload_eye.tmp" : "/upload_led.tmp";
  activeFile_ = LittleFS.open(activeTempPath_, "w");
  if (!activeFile_) {
    clearUpload(true);
    return false;
  }
  activeBytes_ = 0;
  return true;
}

bool AssetUploadServer::appendChunk(UploadKind kind, const String& assetId, httpd_req_t* request) {
  const size_t limit = kind == UploadKind::kEye ? kMaxEyeBytes : kMaxLedBytes;
  if (activeKind_ != kind || activeId_ != assetId || !activeFile_ || request->content_len <= 0 ||
      static_cast<size_t>(request->content_len) > kMaxChunkBytes ||
      activeBytes_ + static_cast<size_t>(request->content_len) > limit) {
    return false;
  }
  uint8_t buffer[kMaxChunkBytes]{};
  const int received = httpd_req_recv(request, reinterpret_cast<char*>(buffer), request->content_len);
  if (received != request->content_len ||
      activeFile_.write(buffer, static_cast<size_t>(received)) != static_cast<size_t>(received)) {
    return false;
  }
  activeFile_.flush();
  activeBytes_ += static_cast<size_t>(received);
  return true;
}

esp_err_t AssetUploadServer::finishUpload(httpd_req_t* request, UploadKind kind,
                                          const String& assetId) {
  if (activeKind_ != kind || activeId_ != assetId || !activeFile_ || activeBytes_ == 0) {
    return sendError(request, "400 Bad Request", "no matching asset upload");
  }
  const bool eyeAsset = kind == UploadKind::kEye;
  const String completedId = activeId_;
  const String tempPath = activeTempPath_;
  activeFile_.close();
  if (!hasExpectedMagic(tempPath, eyeAsset)) {
    clearUpload(true);
    return sendError(request, "400 Bad Request", "invalid asset magic");
  }
  const String destination = eyeAsset ? ExpressionCoordinator::eyePath(completedId) :
                                       ExpressionCoordinator::ledPath(completedId);
  LittleFS.remove(destination);
  if (!LittleFS.rename(tempPath, destination)) {
    clearUpload(true);
    return sendError(request, "500 Internal Server Error", "cannot commit uploaded asset");
  }
  const size_t completedBytes = activeBytes_;
  clearUpload(false);
  String response = String("{\"ok\":true,\"action\":\"upload\",\"") +
                    (eyeAsset ? "clip_id" : "effect_id") + "\":\"" + completedId +
                    "\",\"bytes\":" + String(completedBytes);
  if (eyeAsset) response += ",\"display_confirmed\":true";
  response += "}";
  Serial.printf("[ASSET] upload kind=%s id=%s bytes=%u\n", eyeAsset ? "eye" : "led",
                completedId.c_str(), static_cast<unsigned int>(completedBytes));
  return sendJson(request, "200 OK", response);
}

void AssetUploadServer::clearUpload(bool removeTempFile) {
  if (activeFile_) activeFile_.close();
  if (removeTempFile && !activeTempPath_.isEmpty()) LittleFS.remove(activeTempPath_);
  activeKind_ = UploadKind::kNone;
  activeId_.clear();
  activeTempPath_.clear();
  activeBytes_ = 0;
}
