// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <Arduino.h>
#include <esp_http_server.h>

#include "pairing_store.h"

class AssetUploadServer {
 public:
  explicit AssetUploadServer(PairingStore& pairing) : pairing_(pairing) {}

  bool begin();
  bool ready() const { return server_ != nullptr; }

 private:
  static esp_err_t handleEyeUpload(httpd_req_t* request);
  static esp_err_t handleLedUpload(httpd_req_t* request);
  esp_err_t handleUpload(httpd_req_t* request, bool eyeAsset);

  PairingStore& pairing_;
  httpd_handle_t server_ = nullptr;
  SemaphoreHandle_t uploadMutex_ = nullptr;
};
