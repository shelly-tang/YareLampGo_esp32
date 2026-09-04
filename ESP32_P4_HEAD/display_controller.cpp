// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "display_controller.h"

#include <LittleFS.h>
#include <algorithm>

#include "board_config.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_st7789.h"

namespace {
constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kEye = 0xFFFF;
constexpr uint16_t kAccent = 0xF9AB;
constexpr size_t kFramePixels = BoardConfig::kLcdWidth * BoardConfig::kLcdHeight;
constexpr size_t kClipHeaderBytes = 14;
}  // namespace

bool DisplayController::begin() {
  framebuffer_ = static_cast<uint16_t*>(
      heap_caps_calloc(kFramePixels, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  transferBuffer_ = static_cast<uint16_t*>(
      heap_caps_malloc(kFramePixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!framebuffer_ || !transferBuffer_) {
    return false;
  }

  spi_bus_config_t bus{};
  bus.sclk_io_num = BoardConfig::kLcdSclk;
  bus.mosi_io_num = BoardConfig::kLcdMosi;
  bus.miso_io_num = -1;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = kFramePixels * sizeof(uint16_t) + 8;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
    return false;
  }

  esp_lcd_panel_io_handle_t io = nullptr;
  esp_lcd_panel_io_spi_config_t ioConfig{};
  ioConfig.cs_gpio_num = BoardConfig::kLcdCs;
  ioConfig.dc_gpio_num = BoardConfig::kLcdDc;
  ioConfig.spi_mode = 0;
  ioConfig.pclk_hz = 40 * 1000 * 1000;
  ioConfig.trans_queue_depth = 10;
  ioConfig.lcd_cmd_bits = 8;
  ioConfig.lcd_param_bits = 8;
  if (esp_lcd_new_panel_io_spi(SPI2_HOST, &ioConfig, &io) != ESP_OK) {
    return false;
  }

  esp_lcd_panel_dev_config_t panelConfig{};
  panelConfig.reset_gpio_num = BoardConfig::kLcdReset;
  panelConfig.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panelConfig.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
  panelConfig.bits_per_pixel = 16;
  if (esp_lcd_new_panel_st7789(io, &panelConfig, &panel_) != ESP_OK) {
    panel_ = nullptr;
    return false;
  }
  if (esp_lcd_panel_reset(panel_) != ESP_OK || esp_lcd_panel_init(panel_) != ESP_OK) {
    panel_ = nullptr;
    return false;
  }
  esp_lcd_panel_invert_color(panel_, true);
  esp_lcd_panel_swap_xy(panel_, true);
  esp_lcd_panel_mirror(panel_, true, false);
  // A 172-wide active area is centered in the ST7789's 240-wide RAM.
  // After swap_xy the 34-pixel controller offset is on logical Y.
  esp_lcd_panel_set_gap(panel_, 0, 34);
  esp_lcd_panel_disp_on_off(panel_, true);

  queue_ = xQueueCreate(1, sizeof(Command));
  if (!queue_) {
    return false;
  }
  renderFace(0);
  return xTaskCreate(taskEntry, "lcd-render", 8192, this, 2, &task_) == pdPASS;
}

void DisplayController::showFace(uint8_t mode, uint32_t startAtMs) {
  if (!queue_) return;
  Command command{};
  command.type = CommandType::kFace;
  command.mode = mode;
  command.startAtMs = startAtMs;
  xQueueOverwrite(queue_, &command);
}

void DisplayController::playClip(const char* path, bool loop, uint32_t startAtMs) {
  if (!queue_) return;
  Command command{};
  command.type = CommandType::kClip;
  command.loop = loop;
  command.startAtMs = startAtMs;
  strlcpy(command.path, path, sizeof(command.path));
  xQueueOverwrite(queue_, &command);
}

void DisplayController::taskEntry(void* context) {
  static_cast<DisplayController*>(context)->taskLoop();
}

void DisplayController::taskLoop() {
  uint32_t pendingStart = 0;
  Command pending{};
  bool hasPending = false;
  for (;;) {
    Command command{};
    if (xQueueReceive(queue_, &command, 0) == pdTRUE) {
      pending = command;
      pendingStart = command.startAtMs;
      hasPending = true;
    }
    const uint32_t now = millis();
    if (hasPending && static_cast<int32_t>(now - pendingStart) >= 0) {
      if (clip_) clip_.close();
      if (pending.type == CommandType::kClip) {
        if (!openClip(pending)) renderFace(0);
      } else {
        renderFace(pending.mode);
      }
      hasPending = false;
    }
    if (clip_ && static_cast<int32_t>(now - nextClipFrameMs_) >= 0) {
      if (!renderNextClipFrame()) {
        clip_.close();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(3));
  }
}

void DisplayController::renderFace(uint8_t mode) {
  fill(kBlack);
  if (mode == 0) {
    esp_lcd_panel_draw_bitmap(panel_, 0, 0, BoardConfig::kLcdWidth, BoardConfig::kLcdHeight,
                              framebuffer_);
    return;
  }
  const int eyeY = 78;
  int eyeHeight = 42;
  int leftY = eyeY;
  int rightY = eyeY;
  if (mode == 22 || mode == 26 || mode == 29) leftY += 10, rightY += 10;
  if (mode == 24) eyeHeight = 62;
  if (mode == 28) eyeHeight = 8;
  fillEllipse(98, leftY, 36, eyeHeight, kEye);
  if (mode != 32) {
    fillEllipse(222, rightY, 36, eyeHeight, kEye);
  } else {
    fillRect(188, rightY - 3, 68, 7, kEye);
  }
  if (mode == 23 || mode == 25) {
    fillEllipse(52, 132, 24, 10, kAccent);
    fillEllipse(268, 132, 24, 10, kAccent);
  }
  if (mode == 27) {
    fillEllipse(272, 28, 10, 10, kAccent);
    fillEllipse(296, 14, 6, 6, kAccent);
  }
  esp_lcd_panel_draw_bitmap(panel_, 0, 0, BoardConfig::kLcdWidth, BoardConfig::kLcdHeight,
                            framebuffer_);
}

bool DisplayController::openClip(const Command& command) {
  clip_ = LittleFS.open(command.path, "r");
  if (!clip_) return false;
  char magic[6]{};
  if (clip_.readBytes(magic, sizeof(magic)) != sizeof(magic) || memcmp(magic, "LGLCD1", 6) != 0) {
    clip_.close();
    return false;
  }
  const uint16_t width = readU16(clip_);
  const uint16_t height = readU16(clip_);
  clipFrames_ = readU16(clip_);
  readU16(clip_);  // nominal fps; each frame carries its exact duration
  if (width != BoardConfig::kLcdWidth || height != BoardConfig::kLcdHeight || clipFrames_ == 0) {
    clip_.close();
    return false;
  }
  strlcpy(clipPath_, command.path, sizeof(clipPath_));
  clipLoop_ = command.loop;
  clipFrameIndex_ = 0;
  nextClipFrameMs_ = millis();
  return true;
}

bool DisplayController::renderNextClipFrame() {
  if (clipFrameIndex_ >= clipFrames_) {
    if (!clipLoop_) return false;
    clip_.seek(kClipHeaderBytes);
    clipFrameIndex_ = 0;
  }
  if (clip_.available() < 12) return false;
  const uint16_t x = readU16(clip_);
  const uint16_t y = readU16(clip_);
  const uint16_t width = readU16(clip_);
  const uint16_t height = readU16(clip_);
  const uint16_t durationMs = readU16(clip_);
  const uint16_t runs = readU16(clip_);
  const size_t pixelCount = static_cast<size_t>(width) * height;
  if (x + width > BoardConfig::kLcdWidth || y + height > BoardConfig::kLcdHeight ||
      pixelCount > kFramePixels || runs == 0 || clip_.available() < runs * 4) {
    return false;
  }
  size_t cursor = 0;
  for (uint16_t run = 0; run < runs; ++run) {
    const uint16_t count = readU16(clip_);
    const uint16_t color = readU16(clip_);
    if (count == 0 || cursor + count > pixelCount) return false;
    std::fill_n(transferBuffer_ + cursor, count, color);
    cursor += count;
  }
  if (cursor != pixelCount) return false;
  esp_lcd_panel_draw_bitmap(panel_, x, y, x + width, y + height, transferBuffer_);
  ++clipFrameIndex_;
  nextClipFrameMs_ = millis() + std::max<uint16_t>(1, durationMs);
  return true;
}

void DisplayController::fill(uint16_t color) {
  std::fill_n(framebuffer_, kFramePixels, color);
}

void DisplayController::fillRect(int x, int y, int width, int height, uint16_t color) {
  const int x0 = std::max(0, x);
  const int y0 = std::max(0, y);
  const int x1 = std::min<int>(BoardConfig::kLcdWidth, x + width);
  const int y1 = std::min<int>(BoardConfig::kLcdHeight, y + height);
  for (int row = y0; row < y1; ++row) {
    std::fill(framebuffer_ + row * BoardConfig::kLcdWidth + x0,
              framebuffer_ + row * BoardConfig::kLcdWidth + x1, color);
  }
}

void DisplayController::fillEllipse(int centreX, int centreY, int radiusX, int radiusY,
                                    uint16_t color) {
  for (int y = -radiusY; y <= radiusY; ++y) {
    const float ratio = 1.0f - static_cast<float>(y * y) / static_cast<float>(radiusY * radiusY);
    const int halfWidth = ratio > 0 ? static_cast<int>(radiusX * sqrtf(ratio)) : 0;
    fillRect(centreX - halfWidth, centreY + y, halfWidth * 2 + 1, 1, color);
  }
}

uint16_t DisplayController::readU16(File& file) {
  const int low = file.read();
  const int high = file.read();
  if (low < 0 || high < 0) return 0;
  return static_cast<uint16_t>(low | (high << 8));
}
