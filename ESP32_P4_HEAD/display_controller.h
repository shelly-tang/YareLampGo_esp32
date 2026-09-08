// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <Arduino.h>
#include <FS.h>
#include <esp_lcd_panel_ops.h>

class DisplayController {
 public:
  bool begin();
  void showFace(uint8_t mode, uint32_t startAtMs = 0);
  void playClip(const char* path, bool loop, uint32_t startAtMs = 0);
  bool ready() const { return panel_ != nullptr; }

 private:
  enum class CommandType : uint8_t { kFace, kClip };
  struct Command {
    CommandType type;
    uint8_t mode;
    bool loop;
    uint32_t startAtMs;
    char path[48];
  };

  static void taskEntry(void* context);
  void taskLoop();
  void renderFace(uint8_t mode);
  bool openClip(const Command& command);
  bool renderNextClipFrame();
  void fill(uint16_t color);
  void fillRect(int x, int y, int width, int height, uint16_t color);
  void fillEllipse(int centreX, int centreY, int radiusX, int radiusY, uint16_t color);
  static uint16_t readU16(File& file);

  QueueHandle_t queue_ = nullptr;
  TaskHandle_t task_ = nullptr;
  esp_lcd_panel_handle_t panel_ = nullptr;
  uint16_t* framebuffer_ = nullptr;
  uint16_t* transferBuffer_ = nullptr;
  File clip_;
  char clipPath_[48]{};
  bool clipLoop_ = false;
  uint16_t clipFrames_ = 0;
  uint16_t clipFrameIndex_ = 0;
  uint32_t nextClipFrameMs_ = 0;
};
