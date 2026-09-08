// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>

#include "audio_bridge.h"
#include "asset_upload_server.h"
#include "camera_controller.h"
#include "expression_coordinator.h"
#include "pairing_store.h"
#include "servo_executor.h"

class DeviceHttp {
 public:
  DeviceHttp(PairingStore& pairing, ServoExecutor& servos, ExpressionCoordinator& expressions,
             CameraController& camera, AudioBridge& audio, AssetUploadServer& assets,
             const String& hostname);
  void begin();
  void loop();

 private:
  enum class UploadKind : uint8_t { kNone, kEye, kLed };

  void registerRoutes();
  void handleStatus();
  void handleProvisionStatus();
  void handleScan();
  void handleConnect();
  void handleConfigRead();
  void handleAuthChallenge();
  void handleConfig();
  void handlePair();
  void handleUnpair();
  void handleClaim();
  void handleLed();
  void handleLedStatus();
  void handleClock();
  void handleOcean();
  void handleExpressionPlay();
  void handleExpressionStop();
  void handleAssetList(UploadKind kind);
  void handleAssetDelete(UploadKind kind);
  void handleUploadData(UploadKind kind);
  void finishUploadRequest(UploadKind kind);
  void startUpload(UploadKind kind, const String& assetId, bool authorized);
  void appendUpload(UploadKind kind, const uint8_t* data, size_t size);
  void endUpload(UploadKind kind, bool aborted);
  void finishUpload(UploadKind kind);
  void sendUploadProgress(const char* action);
  void handleForgetWifi();
  void handleReboot();
  void handleServoStatus();
  bool parseJson(JsonDocument& document);
  bool authorize(JsonObjectConst body) const;
  bool authorizeRequest(const String& purpose) const;
  bool validateStoredAsset(UploadKind kind, const String& path, String& error) const;
  String assetPath(UploadKind kind, const String& id) const;
  void sendJson(int status, JsonDocument& document);
  void sendError(int status, const char* error);
  void scheduleRestart(uint32_t delayMs = 750);
  void addServoStatus(JsonObject target, const ServoSnapshot& snapshot) const;

  PairingStore& pairing_;
  ServoExecutor& servos_;
  ExpressionCoordinator& expressions_;
  CameraController& camera_;
  AudioBridge& audio_;
  AssetUploadServer& assets_;
  String hostname_;
  WebServer server_;
  File uploadFile_;
  UploadKind uploadKind_ = UploadKind::kNone;
  String uploadId_;
  String uploadTempPath_;
  size_t uploadBytes_ = 0;
  bool uploadOk_ = false;
  String uploadError_;
  uint32_t restartAtMs_ = 0;
};
