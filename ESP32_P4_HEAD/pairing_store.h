// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <Arduino.h>
#include <Preferences.h>

class PairingStore {
 public:
  bool begin();
  bool pair(const String& ownerId, const String& ownerLabel, const String& secret, String& error);
  bool authorize(const String& ownerId, const String& secret) const;
  bool unpair(const String& ownerId, const String& secret);
  bool isPaired() const { return !ownerId_.isEmpty(); }
  const String& ownerId() const { return ownerId_; }
  const String& ownerLabel() const { return ownerLabel_; }
  uint32_t revision() const { return revision_; }

 private:
  static String sha256(const String& value);

  Preferences preferences_;
  String ownerId_;
  String ownerLabel_;
  String secretHash_;
  uint32_t revision_ = 1;
};
