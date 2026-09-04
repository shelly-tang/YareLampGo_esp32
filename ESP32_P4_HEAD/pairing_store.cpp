// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pairing_store.h"

#include <mbedtls/sha256.h>

bool PairingStore::begin() {
  if (!preferences_.begin("lampgo-p4", false)) {
    return false;
  }
  ownerId_ = preferences_.getString("owner", "");
  ownerLabel_ = preferences_.getString("owner_label", "");
  secretHash_ = preferences_.getString("secret_hash", "");
  if (ownerId_.isEmpty() || secretHash_.length() != 64) {
    ownerId_.clear();
    ownerLabel_.clear();
    secretHash_.clear();
  }
  return true;
}

bool PairingStore::pair(const String& ownerId, const String& ownerLabel, const String& secret,
                        String& error) {
  if (ownerId.isEmpty() || secret.length() < 16) {
    error = "owner_id and pairing_secret are required";
    return false;
  }
  if (isPaired() && !authorize(ownerId, secret)) {
    error = "device is paired to another backend";
    return false;
  }
  if (isPaired()) {
    if (ownerLabel == ownerLabel_) return true;
    if (preferences_.putString("owner_label", ownerLabel) != ownerLabel.length()) {
      error = "failed to persist owner label";
      return false;
    }
    ownerLabel_ = ownerLabel;
    return true;
  }

  const String secretHash = sha256(secret);
  const bool ok = preferences_.putString("owner", ownerId) == ownerId.length() &&
                  preferences_.putString("owner_label", ownerLabel) == ownerLabel.length() &&
                  preferences_.putString("secret_hash", secretHash) == secretHash.length();
  if (!ok) {
    preferences_.remove("owner");
    preferences_.remove("owner_label");
    preferences_.remove("secret_hash");
    error = "failed to persist pairing";
    return false;
  }
  ownerId_ = ownerId;
  ownerLabel_ = ownerLabel;
  secretHash_ = secretHash;
  ++revision_;
  return true;
}

bool PairingStore::authorize(const String& ownerId, const String& secret) const {
  return isPaired() && !secret.isEmpty() && ownerId == ownerId_ && sha256(secret) == secretHash_;
}

bool PairingStore::unpair(const String& ownerId, const String& secret) {
  if (!authorize(ownerId, secret)) {
    return false;
  }
  preferences_.remove("owner");
  preferences_.remove("owner_label");
  preferences_.remove("secret_hash");
  ownerId_.clear();
  ownerLabel_.clear();
  secretHash_.clear();
  ++revision_;
  return true;
}

String PairingStore::sha256(const String& value) {
  uint8_t digest[32]{};
  mbedtls_sha256(reinterpret_cast<const unsigned char*>(value.c_str()), value.length(), digest, 0);
  static constexpr char kHex[] = "0123456789abcdef";
  char encoded[65]{};
  for (size_t index = 0; index < sizeof(digest); ++index) {
    encoded[index * 2] = kHex[digest[index] >> 4];
    encoded[index * 2 + 1] = kHex[digest[index] & 0x0F];
  }
  return String(encoded);
}
