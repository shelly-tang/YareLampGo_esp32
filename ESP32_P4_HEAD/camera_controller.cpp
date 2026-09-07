// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "camera_controller.h"

#include <ESP_Video.h>
#include <driver/jpeg_encode.h>

#include "board_config.h"

namespace {
ESPVideoClass gVideo;
ESPVideoCaptureDevClass gCapture;
jpeg_encoder_handle_t gJpegEncoder = nullptr;
uint8_t* gJpegInput = nullptr;
size_t gJpegInputSize = 0;
uint8_t* gJpegOutput = nullptr;
size_t gJpegOutputSize = 0;

bool beginJpegEncoder(size_t rawFrameBytes) {
  jpeg_encode_engine_cfg_t engineConfig = {};
  engineConfig.timeout_ms = 200;
  if (jpeg_new_encoder_engine(&engineConfig, &gJpegEncoder) != ESP_OK) return false;

  jpeg_encode_memory_alloc_cfg_t inputConfig = {};
  inputConfig.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER;
  gJpegInput = static_cast<uint8_t*>(
      jpeg_alloc_encoder_mem(rawFrameBytes, &inputConfig, &gJpegInputSize));

  jpeg_encode_memory_alloc_cfg_t outputConfig = {};
  outputConfig.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
  // A single incompressible frame can approach its source size.  Reserve the
  // full UYVY frame rather than making successful captures content-dependent.
  gJpegOutput = static_cast<uint8_t*>(
      jpeg_alloc_encoder_mem(rawFrameBytes, &outputConfig, &gJpegOutputSize));
  return gJpegInput != nullptr && gJpegOutput != nullptr;
}

uint8_t encoderQuality(uint8_t legacyQuality) {
  // Preserve the esp_camera convention used by the existing backend: a lower
  // quality number means a higher quality image.  The P4 driver uses 1..100.
  return static_cast<uint8_t>(96 - ((legacyQuality - 4) * 61) / 59);
}
}  // namespace

bool CameraController::begin() {
  ESPVideoCamConfigClass sensor;
  ESPVideoDVPPinsConfigClass pins;
  ESPVideoDVPConfigClass config;
  const bool sensorReady = sensor.begin(
      I2C_NUM_0, BoardConfig::kCameraScl, BoardConfig::kCameraSda, 100000,
      BoardConfig::kCameraReset, BoardConfig::kCameraPowerDown);
  const bool pinsReady = pins.begin(
      BoardConfig::kCameraVsync, BoardConfig::kCameraHref, BoardConfig::kCameraPclk,
      BoardConfig::kCameraXclk, BoardConfig::kCameraD0, BoardConfig::kCameraD1,
      BoardConfig::kCameraD2, BoardConfig::kCameraD3, BoardConfig::kCameraD4,
      BoardConfig::kCameraD5, BoardConfig::kCameraD6, BoardConfig::kCameraD7);
  const bool configReady = sensorReady && pinsReady && config.begin(sensor, pins, 20000000);
  const bool videoReady = configReady && gVideo.begin(config);
  const bool captureReady = videoReady && gCapture.begin(ESP_VIDEO_DVP_DEVICE_NAME, 2);
  const bool formatReady = captureReady && gCapture.setFormat(ESP_VIDEO_FORMAT_YUV422_UYVY);
  const size_t rawFrameBytes = static_cast<size_t>(gCapture.getWidth()) * gCapture.getHeight() * 2;
  const bool encoderReady = formatReady && beginJpegEncoder(rawFrameBytes);
  const bool started = encoderReady && gCapture.startCapture();
  ready_ = started;
  Serial.printf(
      "[CAMERA] OV5640 DVP ready=%d sensor=%d pins=%d config=%d video=%d capture=%d "
      "format=%d jpeg=%d start=%d format_name=%s size=%lux%lu raw=%u jpeg_buffer=%u buffers=2\n",
      ready_, sensorReady, pinsReady, configReady, videoReady, captureReady, formatReady,
      encoderReady, started, gCapture.getFormatName(),
      static_cast<unsigned long>(gCapture.getWidth()),
      static_cast<unsigned long>(gCapture.getHeight()), static_cast<unsigned>(rawFrameBytes),
      static_cast<unsigned>(gJpegOutputSize));
  return ready_;
}

bool CameraController::setJpegQuality(uint8_t legacyQuality) {
  if (legacyQuality < 4 || legacyQuality > 63) return false;
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
  const size_t rawFrameBytes = static_cast<size_t>(width()) * height() * 2;
  if (!frame.valid() || frame.formatType() != ESP_VIDEO_FORMAT_YUV422_UYVY ||
      frame.size() < rawFrameBytes || gJpegEncoder == nullptr || gJpegInput == nullptr ||
      gJpegOutput == nullptr || gJpegInputSize < rawFrameBytes) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"camera capture failed\"}");
    return false;
  }

  // The board's OV5640 DVP lane is UYVY.  The encoder receives that packed
  // YUV422 stream from an aligned DMA buffer and retains the backend's JPEG
  // HTTP contract without pretending the sensor emits JPEG itself.
  memcpy(gJpegInput, frame.data(), rawFrameBytes);
  jpeg_encode_cfg_t encodeConfig = {};
  encodeConfig.width = width();
  encodeConfig.height = height();
  encodeConfig.src_type = JPEG_ENCODE_IN_FORMAT_YUV422;
  encodeConfig.sub_sample = JPEG_DOWN_SAMPLING_YUV422;
  encodeConfig.image_quality = encoderQuality(jpegQuality_);
  encodeConfig.pixel_reverse = false;
  uint32_t jpegBytes = 0;
  if (jpeg_encoder_process(gJpegEncoder, &encodeConfig, gJpegInput, rawFrameBytes, gJpegOutput,
                           gJpegOutputSize, &jpegBytes) != ESP_OK || jpegBytes == 0) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"camera JPEG encoding failed\"}");
    return false;
  }
  server.setContentLength(jpegBytes);
  server.send(200, "image/jpeg", "");
  return server.client().write(gJpegOutput, jpegBytes) == jpegBytes;
}
