// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pairing_store.h"

#include <mbedtls/sha256.h>
#include <mbedtls/md.h>
#include <esp_system.h>

namespace {
constexpr uint32_t kChallengeTtlMs = 15'000;
constexpr char kAuthDomain[] = "lampgo-p4-auth-v1";
}

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

String PairingStore::issueChallenge(const String& purpose) {
  if (!isPaired() || purpose.isEmpty() || purpose.length() > 96) return "";
  const uint32_t now = millis();
  uint8_t slot = nextChallenge_;
  for (uint8_t index = 0; index < 8; ++index) {
    if (challenges_[index].expiresAtMs == 0 ||
        static_cast<int32_t>(now - challenges_[index].expiresAtMs) >= 0) {
      slot = index;
      break;
    }
  }
  char nonce[33]{};
  snprintf(nonce, sizeof(nonce), "%08lx%08lx%08lx%08lx",
           static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
  challenges_[slot].purpose = purpose;
  challenges_[slot].nonce = nonce;
  challenges_[slot].expiresAtMs = now + kChallengeTtlMs;
  nextChallenge_ = static_cast<uint8_t>((slot + 1) % 8);
  return challenges_[slot].nonce;
}

bool PairingStore::authorizeProof(const String& ownerId, const String& purpose, const String& nonce,
                                  const String& proof) {
  if (!isPaired() || ownerId != ownerId_ || purpose.isEmpty() || nonce.isEmpty() || proof.length() != 64) {
    return false;
  }
  const uint32_t now = millis();
  int slot = -1;
  for (uint8_t index = 0; index < 8; ++index) {
    if (challenges_[index].expiresAtMs != 0 &&
        static_cast<int32_t>(now - challenges_[index].expiresAtMs) >= 0) {
      challenges_[index] = Challenge{};
      continue;
    }
    if (challenges_[index].purpose == purpose && challenges_[index].nonce == nonce) {
      slot = index;
      break;
    }
  }
  if (slot < 0) return false;
  // Consume before comparing so a captured valid proof cannot be replayed.
  challenges_[slot] = Challenge{};
  return constantTimeEquals(hmacSha256(secretHash_, String(kAuthDomain) + "\n" + purpose + "\n" + ownerId + "\n" + nonce), proof);
}

bool PairingStore::unpair(const String& ownerId, const String& secret) {
  if (!authorize(ownerId, secret)) {
    return false;
  }
  return clear();
}

bool PairingStore::clear() {
  preferences_.remove("owner");
  preferences_.remove("owner_label");
  preferences_.remove("secret_hash");
  ownerId_.clear();
  ownerLabel_.clear();
  secretHash_.clear();
  for (Challenge& challenge : challenges_) challenge = Challenge{};
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

String PairingStore::hmacSha256(const String& key, const String& value) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return "";
  uint8_t digest[32]{};
  if (mbedtls_md_hmac(info, reinterpret_cast<const unsigned char*>(key.c_str()), key.length(),
                      reinterpret_cast<const unsigned char*>(value.c_str()), value.length(), digest) != 0) {
    return "";
  }
  static constexpr char kHex[] = "0123456789abcdef";
  char encoded[65]{};
  for (size_t index = 0; index < sizeof(digest); ++index) {
    encoded[index * 2] = kHex[digest[index] >> 4];
    encoded[index * 2 + 1] = kHex[digest[index] & 0x0F];
  }
  return String(encoded);
}

bool PairingStore::constantTimeEquals(const String& left, const String& right) {
  if (left.length() != right.length()) return false;
  uint8_t difference = 0;
  for (size_t index = 0; index < left.length(); ++index) difference |= left[index] ^ right[index];
  return difference == 0;
}
