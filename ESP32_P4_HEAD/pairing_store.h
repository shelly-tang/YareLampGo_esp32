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
  String issueChallenge(const String& purpose);
  bool authorizeProof(const String& ownerId, const String& purpose, const String& nonce,
                      const String& proof);
  bool unpair(const String& ownerId, const String& secret);
  bool clear();
  bool isPaired() const { return !ownerId_.isEmpty(); }
  const String& ownerId() const { return ownerId_; }
  const String& ownerLabel() const { return ownerLabel_; }
  uint32_t revision() const { return revision_; }

 private:
  struct Challenge {
    String purpose;
    String nonce;
    uint32_t expiresAtMs = 0;
  };

  static String sha256(const String& value);
  static String hmacSha256(const String& key, const String& value);
  static bool constantTimeEquals(const String& left, const String& right);

  Preferences preferences_;
  String ownerId_;
  String ownerLabel_;
  String secretHash_;
  Challenge challenges_[8];
  uint8_t nextChallenge_ = 0;
  uint32_t revision_ = 1;
};
