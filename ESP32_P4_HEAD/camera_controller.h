// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <Arduino.h>
#include <WebServer.h>

class CameraController {
 public:
  bool begin();
  bool ready() const { return ready_; }
  bool setJpegQuality(uint8_t legacyQuality);
  uint8_t jpegQuality() const { return jpegQuality_; }
  uint32_t width() const;
  uint32_t height() const;
  bool sendJpeg(WebServer& server);

 private:
  bool ready_ = false;
  uint8_t jpegQuality_ = 10;
};
