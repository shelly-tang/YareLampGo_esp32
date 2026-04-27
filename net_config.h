#ifndef LAMPGO_NET_CONFIG_H
#define LAMPGO_NET_CONFIG_H

#include <Arduino.h>

namespace NetConfig {

void begin();

bool hasCredentials();

bool loadWifi(String &ssid, String &password);

bool saveWifi(const String &ssid, const String &password);

bool clearWifi();

String deviceIdSuffix();

String deviceHostname();

String apSsid();

const char *apPassword();

}

#endif
