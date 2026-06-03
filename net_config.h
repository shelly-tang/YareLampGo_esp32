// SPDX-FileCopyrightText: 2026 @ninsmiracle, @shelly-tang, and LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef LAMPGO_NET_CONFIG_H
#define LAMPGO_NET_CONFIG_H

#include <Arduino.h>

namespace NetConfig {

void begin();

bool hasCredentials();

bool loadWifi(String &ssid, String &password);

bool saveWifi(const String &ssid, const String &password);

bool clearWifi();

bool loadPairing(String &ownerId, String &ownerLabel, String &secretHash);

bool savePairing(const String &ownerId, const String &ownerLabel, const String &pairingSecret);

bool clearPairing();

bool hasPairing();

bool verifyPairing(const String &ownerId, const String &pairingSecret);

String deviceIdSuffix();

String deviceHostname();

String apSsid();

const char *apPassword();

}

#endif
