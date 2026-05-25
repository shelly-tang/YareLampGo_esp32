#include "provision_ap.h"

#include <WebServer.h>
#include <WiFi.h>
#include <cJSON.h>

#include "net_config.h"

namespace {

bool g_active = false;
WebServer g_server(80);
unsigned long g_scheduledRestartMs = 0;

void addCors() {
  g_server.sendHeader("Access-Control-Allow-Origin", "*");
  g_server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  g_server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptions() {
  addCors();
  g_server.send(204);
}

void handleStatus() {
  addCors();

  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "configured", false);
  cJSON_AddStringToObject(root, "mode", "ap_sta");
  cJSON_AddStringToObject(root, "mac", macStr);
  cJSON_AddStringToObject(root, "hostname", NetConfig::deviceHostname().c_str());
  cJSON_AddStringToObject(root, "ap_ssid", NetConfig::apSsid().c_str());
  cJSON_AddBoolToObject(root, "pairing_supported", true);
  cJSON_AddBoolToObject(root, "paired", NetConfig::hasPairing());

  char *out = cJSON_PrintUnformatted(root);
  g_server.send(200, "application/json", out ? out : "{}");
  if (out) cJSON_free(out);
  cJSON_Delete(root);
}

void handleScan() {
  addCors();

  int16_t n = WiFi.scanNetworks(false, true);
  Serial.printf("[provision] scan result=%d\n", n);
  cJSON *root = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(root, "networks");
  cJSON_AddNumberToObject(root, "scan_result", n);
  cJSON_AddBoolToObject(root, "scan_ok", n >= 0);

  if (n > 0) {
    for (int i = 0; i < n && i < 30; i++) {
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "ssid", WiFi.SSID(i).c_str());
      cJSON_AddNumberToObject(item, "rssi", WiFi.RSSI(i));
      cJSON_AddStringToObject(item, "bssid", WiFi.BSSIDstr(i).c_str());
      cJSON_AddNumberToObject(item, "channel", WiFi.channel(i));
      cJSON_AddBoolToObject(item, "encrypt", WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      cJSON_AddItemToArray(arr, item);
    }
  } else if (n < 0) {
    cJSON_AddStringToObject(root, "error", "scan_failed");
  }
  WiFi.scanDelete();

  char *out = cJSON_PrintUnformatted(root);
  g_server.send(200, "application/json", out ? out : "{\"networks\":[]}");
  if (out) cJSON_free(out);
  cJSON_Delete(root);
}

void handleConnect() {
  addCors();

  if (!g_server.hasArg("plain")) {
    g_server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty body\"}");
    return;
  }

  cJSON *root = cJSON_Parse(g_server.arg("plain").c_str());
  if (!root) {
    g_server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }

  const cJSON *ssidJson = cJSON_GetObjectItemCaseSensitive(root, "ssid");
  const cJSON *passJson = cJSON_GetObjectItemCaseSensitive(root, "password");
  const cJSON *ownerJson = cJSON_GetObjectItemCaseSensitive(root, "owner_id");
  const cJSON *labelJson = cJSON_GetObjectItemCaseSensitive(root, "owner_label");
  const cJSON *secretJson = cJSON_GetObjectItemCaseSensitive(root, "pairing_secret");
  const char *ssid = cJSON_IsString(ssidJson) ? ssidJson->valuestring : "";
  const char *pass = cJSON_IsString(passJson) ? passJson->valuestring : "";
  const char *ownerId = cJSON_IsString(ownerJson) ? ownerJson->valuestring : "";
  const char *ownerLabel = cJSON_IsString(labelJson) ? labelJson->valuestring : "";
  const char *pairingSecret = cJSON_IsString(secretJson) ? secretJson->valuestring : "";

  if (strlen(ssid) == 0) {
    cJSON_Delete(root);
    g_server.send(400, "application/json", "{\"ok\":false,\"error\":\"ssid required\"}");
    return;
  }

  bool saved = NetConfig::saveWifi(ssid, pass);
  bool paired = true;
  if (strlen(ownerId) > 0 || strlen(pairingSecret) > 0) {
    paired = NetConfig::savePairing(String(ownerId), String(ownerLabel), String(pairingSecret));
  }
  cJSON_Delete(root);

  if (!saved || !paired) {
    g_server.send(500, "application/json", "{\"ok\":false,\"error\":\"nvs save failed\"}");
    return;
  }

  g_server.send(200, "application/json", "{\"ok\":true,\"message\":\"restarting\"}");
  g_scheduledRestartMs = millis() + 2000;
  Serial.printf("[provision] credentials saved, paired=%d owner=%s, restarting in 2s\n", paired ? 1 : 0, ownerId);
}

void handleNotFound() {
  if (g_server.method() == HTTP_OPTIONS) {
    handleOptions();
    return;
  }
  addCors();
  g_server.send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
}

}

namespace ProvisionAP {

void start() {
  g_active = true;

  WiFi.persistent(false);
  WiFi.disconnect(false, true);
  delay(200);
  WiFi.mode(WIFI_OFF);
  delay(500);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  const String ssid = NetConfig::apSsid();
  bool ok = WiFi.softAP(ssid.c_str(), NetConfig::apPassword());
  if (!ok) {
    Serial.println("[provision] softAP first attempt failed, retrying");
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(800);
    WiFi.mode(WIFI_AP_STA);
    delay(200);
    ok = WiFi.softAP(ssid.c_str(), NetConfig::apPassword());
  }
  IPAddress ip = WiFi.softAPIP();

  Serial.println();
  Serial.println("============================================");
  Serial.println("[provision] WiFi not configured, SoftAP+STA mode");
  Serial.printf("[provision] SSID: %s\n", ssid.c_str());
  Serial.printf("[provision] Password: %s\n", NetConfig::apPassword());
  Serial.printf("[provision] IP: %s\n", ip.toString().c_str());
  Serial.printf("[provision] softAP start ok=%d\n", ok);
  Serial.println("============================================");

  g_server.on("/status", HTTP_GET, handleStatus);
  g_server.on("/status", HTTP_OPTIONS, handleOptions);
  g_server.on("/scan", HTTP_GET, handleScan);
  g_server.on("/scan", HTTP_OPTIONS, handleOptions);
  g_server.on("/connect", HTTP_POST, handleConnect);
  g_server.on("/connect", HTTP_OPTIONS, handleOptions);
  g_server.onNotFound(handleNotFound);

  g_server.begin();
  Serial.println("[provision] HTTP server listening on :80");
}

void loop() {
  g_server.handleClient();
  if (g_scheduledRestartMs != 0 && millis() >= g_scheduledRestartMs) {
    Serial.println("[provision] restarting now");
    delay(100);
    ESP.restart();
  }
}

bool isActive() {
  return g_active;
}

}
