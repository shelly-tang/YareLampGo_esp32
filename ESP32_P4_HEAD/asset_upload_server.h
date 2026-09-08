// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <Arduino.h>
#include <FS.h>
#include <esp_http_server.h>

#include "pairing_store.h"

class AssetUploadServer {
 public:
  explicit AssetUploadServer(PairingStore& pairing) : pairing_(pairing) {}

  bool begin(httpd_handle_t server);
  bool ready() const { return ready_; }

 private:
  enum class UploadKind : uint8_t { kNone, kEye, kLed };

  static esp_err_t handleEyeUpload(httpd_req_t* request);
  static esp_err_t handleLedUpload(httpd_req_t* request);
  esp_err_t handleUpload(httpd_req_t* request, bool eyeAsset);
  bool beginUpload(UploadKind kind, const String& assetId);
  bool appendChunk(UploadKind kind, const String& assetId, httpd_req_t* request);
  esp_err_t finishUpload(httpd_req_t* request, UploadKind kind, const String& assetId);
  void clearUpload(bool removeTempFile);

  PairingStore& pairing_;
  bool ready_ = false;
  SemaphoreHandle_t uploadMutex_ = nullptr;
  UploadKind activeKind_ = UploadKind::kNone;
  String activeId_;
  String activeTempPath_;
  File activeFile_;
  size_t activeBytes_ = 0;
};
