// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "camera_controller.h"

#include <ESP_Video.h>

#include "board_config.h"

namespace {
ESPVideoClass gVideo;
ESPVideoCaptureDevClass gCapture;
}  // namespace

bool CameraController::begin() {
  ESPVideoCamConfigClass sensor;
  ESPVideoDVPPinsConfigClass pins;
  ESPVideoDVPConfigClass config;
  const bool configured =
      sensor.begin(I2C_NUM_0, BoardConfig::kCameraScl, BoardConfig::kCameraSda, 100000,
                   BoardConfig::kCameraReset, BoardConfig::kCameraPowerDown) &&
      pins.begin(BoardConfig::kCameraVsync, BoardConfig::kCameraHref,
                 BoardConfig::kCameraPclk, BoardConfig::kCameraXclk, BoardConfig::kCameraD0,
                 BoardConfig::kCameraD1, BoardConfig::kCameraD2, BoardConfig::kCameraD3,
                 BoardConfig::kCameraD4, BoardConfig::kCameraD5, BoardConfig::kCameraD6,
                 BoardConfig::kCameraD7) &&
      config.begin(sensor, pins, 20000000);
  ready_ = configured && gVideo.begin(config) &&
           gCapture.begin(ESP_VIDEO_DVP_DEVICE_NAME, 2) &&
           gCapture.setFormat(ESP_VIDEO_FORMAT_JPEG) && setJpegQuality(jpegQuality_) &&
           gCapture.startCapture();
  Serial.printf("[CAMERA] OV5640 DVP ready=%d format=%s size=%lux%lu buffers=2\n", ready_,
                gCapture.getFormatName(), static_cast<unsigned long>(gCapture.getWidth()),
                static_cast<unsigned long>(gCapture.getHeight()));
  return ready_;
}

bool CameraController::setJpegQuality(uint8_t legacyQuality) {
  if (legacyQuality < 4 || legacyQuality > 63) return false;
  // Keep the existing backend's esp_camera convention (smaller is better)
  // while ESP_Video exposes V4L2 quality as a percentage (larger is better).
  const int qualityPercent = map(legacyQuality, 4, 63, 96, 35);
  if (!gCapture.setSensorJPEGQuality(qualityPercent)) return false;
  jpegQuality_ = legacyQuality;
  return true;
}

uint32_t CameraController::width() const { return gCapture.getWidth(); }
uint32_t CameraController::height() const { return gCapture.getHeight(); }

bool CameraController::sendJpeg(WebServer& server) {
  if (!ready_) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"camera unavailable\"}");
    return false;
  }

  ESPVideoBufferClass frame = gCapture.captureBuffer();
  if (!frame.valid() || frame.formatType() != ESP_VIDEO_FORMAT_JPEG) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"camera capture failed\"}");
    return false;
  }
  server.setContentLength(frame.size());
  server.send(200, "image/jpeg", "");
  return server.client().write(frame.data(), frame.size()) == frame.size();
}
