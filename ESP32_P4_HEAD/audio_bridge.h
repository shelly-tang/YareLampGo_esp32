// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <Arduino.h>
#include <esp_http_server.h>

#include "pairing_store.h"

class AudioBridge {
 public:
  explicit AudioBridge(PairingStore& pairing) : pairing_(pairing) {}

  bool begin();
  bool microphoneReady() const;
  bool microphoneEnabled() const;
  void setMicrophoneEnabled(bool enabled);
  bool speakerReady() const;
  bool aecReady() const;
  bool aecEnabled() const;
  httpd_handle_t httpServer() const;
  const char* profile() const;
  bool setProfile(const String& profile);
  float speakerVolume() const;
  void setSpeakerVolume(float volume);
  uint32_t micFrames() const;
  uint32_t speakerPackets() const;
  uint32_t speakerDrops() const;
  uint8_t audioClientCount() const;
  uint32_t audioConnections() const;
  uint32_t audioFramesQueued() const;
  uint32_t audioFramesSent() const;
  uint32_t audioSendFailures() const;
  uint32_t audioQueueFailures() const;
  uint32_t audioAuthFailures() const;

 private:
  PairingStore& pairing_;
};
