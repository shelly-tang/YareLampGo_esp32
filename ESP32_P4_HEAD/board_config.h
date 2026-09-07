// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <Arduino.h>

namespace BoardConfig {

constexpr char kFirmwareVersion[] = "p4-head-0.1.1";
constexpr char kMotionProtocol[] = "lampgo-motion-v1";

constexpr int kServoTx = 31;
constexpr int kServoRx = 32;
constexpr uint32_t kServoBaud = 1000000;
constexpr uint8_t kServoCount = 5;

constexpr int kLedData = 53;
constexpr uint16_t kLedWidth = 54;
constexpr uint16_t kLedHeight = 9;
constexpr uint16_t kLedCount = kLedWidth * kLedHeight;
// Logical pixel (0, 0) is the top-left corner when the lamp is viewed from
// the front.  The diagnostic endpoint renders fixed *physical* indices so
// this transform can be confirmed before changing either flag.
constexpr bool kLedMirrorX = false;
constexpr bool kLedMirrorY = false;
constexpr uint8_t kLedSafeBrightness = 32;
// Conservative until whole-board current and thermal tests establish a
// product ceiling. One equivalent full-white WS2812 pixel is about 60 mA.
constexpr uint16_t kLedEquivalentWhiteBudget = 24;

constexpr int kLcdSclk = 26;
constexpr int kLcdMosi = 27;
constexpr int kLcdCs = 28;
constexpr int kLcdDc = 29;
constexpr int kLcdReset = 30;
constexpr uint16_t kLcdWidth = 320;
constexpr uint16_t kLcdHeight = 172;

constexpr int kCameraD0 = 2;
constexpr int kCameraD1 = 3;
constexpr int kCameraD2 = 4;
constexpr int kCameraD3 = 5;
constexpr int kCameraD4 = 6;
constexpr int kCameraD5 = 7;
constexpr int kCameraD6 = 8;
constexpr int kCameraD7 = 9;
constexpr int kCameraPclk = 10;
constexpr int kCameraVsync = 11;
constexpr int kCameraHref = 12;
constexpr int kCameraXclk = 13;
constexpr int kCameraSda = 20;
constexpr int kCameraScl = 21;
constexpr int kCameraReset = 22;
constexpr int kCameraPowerDown = 23;

constexpr int kMicClock = 51;
constexpr int kMicData = 52;
constexpr int kSpeakerBclk = 33;
constexpr int kSpeakerLrclk = 34;
constexpr int kSpeakerData = 49;
constexpr uint32_t kAudioSampleRate = 16000;

constexpr uint16_t kHttpPort = 80;
constexpr uint16_t kAudioWsPort = 81;
constexpr uint16_t kMotionWsPort = 82;
constexpr char kSetupSsidPrefix[] = "Lampgo-P4-Setup-";
constexpr char kSetupPassword[] = "lampgo-p4-setup";

}  // namespace BoardConfig
