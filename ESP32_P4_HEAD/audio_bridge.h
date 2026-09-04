// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <Arduino.h>

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
  const char* profile() const;
  bool setProfile(const String& profile);
  float speakerVolume() const;
  void setSpeakerVolume(float volume);
  uint32_t micFrames() const;
  uint32_t speakerPackets() const;
  uint32_t speakerDrops() const;

 private:
  PairingStore& pairing_;
};
