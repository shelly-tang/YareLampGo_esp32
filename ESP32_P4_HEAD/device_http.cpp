// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "device_http.h"

#include <algorithm>

#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#if __has_include(<esp32-hal-hosted.h>)
#include <esp32-hal-hosted.h>
#define LAMPGO_HAS_ESP_HOSTED_DIAGNOSTICS 1
#else
#define LAMPGO_HAS_ESP_HOSTED_DIAGNOSTICS 0
#endif

#include "board_config.h"
#include "motion_protocol.h"

namespace {
constexpr size_t kMaxEyeBytes = 512 * 1024;
constexpr size_t kMaxLedBytes = 8 * 1024;
constexpr char kContentTypeHeader[] = "Content-Type";
constexpr char kOwnerHeader[] = "X-Lampgo-Owner";
constexpr char kAuthPurposeHeader[] = "X-Lampgo-Auth-Purpose";
constexpr char kAuthNonceHeader[] = "X-Lampgo-Auth-Nonce";
constexpr char kAuthProofHeader[] = "X-Lampgo-Auth-Proof";
constexpr char kClipIdHeader[] = "X-Lampgo-Clip-Id";
constexpr char kEffectIdHeader[] = "X-Lampgo-Effect-Id";
constexpr char kUploadPhaseHeader[] = "X-Lampgo-Upload-Phase";

void addHostedDiagnostics(JsonDocument& response) {
  JsonObject hosted = response["hosted"].to<JsonObject>();
  hosted["internal_dma_free"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  hosted["internal_dma_largest"] =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  hosted["internal_dma_low_water"] =
      heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

#if LAMPGO_HAS_ESP_HOSTED_DIAGNOSTICS
  uint32_t hostMajor = 0;
  uint32_t hostMinor = 0;
  uint32_t hostPatch = 0;
  uint32_t slaveMajor = 0;
  uint32_t slaveMinor = 0;
  uint32_t slavePatch = 0;
  hostedGetHostVersion(&hostMajor, &hostMinor, &hostPatch);
  hostedGetSlaveVersion(&slaveMajor, &slaveMinor, &slavePatch);
  hosted["initialized"] = hostedIsInitialized();
  hosted["host_version"] = String(hostMajor) + "." + String(hostMinor) + "." + String(hostPatch);
  hosted["slave_version"] = String(slaveMajor) + "." + String(slaveMinor) + "." + String(slavePatch);
#else
  hosted["initialized"] = false;
  hosted["host_version"] = "unavailable";
  hosted["slave_version"] = "unavailable";
#endif
}

uint32_t parseColor(String value) {
  value.trim();
  if (value.startsWith("#")) value.remove(0, 1);
  if (value.length() != 6) return 0xFFFFFF;
  char* end = nullptr;
  const uint32_t result = strtoul(value.c_str(), &end, 16);
  return end && *end == '\0' ? result : 0xFFFFFF;
}

ClockEffect parseClockEffect(String value) {
  value.trim();
  value.toLowerCase();
  if (value == "blink") return ClockEffect::kBlink;
  if (value == "orbit") return ClockEffect::kOrbit;
  return ClockEffect::kSteady;
}

uint8_t percent(JsonVariantConst value, uint8_t fallback) {
  if (value.isNull()) return fallback;
  return static_cast<uint8_t>(std::max(0, std::min(100, value.as<int>())));
}

uint16_t range(JsonVariantConst value, uint16_t fallback, uint16_t minimum, uint16_t maximum) {
  if (value.isNull()) return fallback;
  return static_cast<uint16_t>(std::max<int>(minimum, std::min<int>(maximum, value.as<int>())));
}
}  // namespace

DeviceHttp::DeviceHttp(PairingStore& pairing, ServoExecutor& servos,
                       ExpressionCoordinator& expressions, CameraController& camera,
                       AudioBridge& audio, AssetUploadServer& assets, const String& hostname)
    : pairing_(pairing), servos_(servos), expressions_(expressions), camera_(camera),
      audio_(audio), assets_(assets), hostname_(hostname), server_(BoardConfig::kHttpPort) {}

void DeviceHttp::begin() {
  // Raw body callbacks run before WebServer parses query arguments.  Retain
  // the headers that identify the paired owner and target asset.
  const char* headers[] = {kContentTypeHeader, kOwnerHeader, kAuthPurposeHeader,
                           kAuthNonceHeader, kAuthProofHeader, kClipIdHeader,
                           kEffectIdHeader, kUploadPhaseHeader};
  server_.collectHeaders(headers, 8);
  registerRoutes();
  server_.begin();
  Serial.printf("[HTTP READY] http://%s/\n", hostname_.c_str());
}

void DeviceHttp::loop() {
  server_.handleClient();
  if (restartAtMs_ != 0 && static_cast<int32_t>(millis() - restartAtMs_) >= 0) {
    Serial.println("[HTTP] restarting now");
    delay(50);
    ESP.restart();
  }
}

void DeviceHttp::registerRoutes() {
  server_.on("/", HTTP_GET, [this]() {
    server_.send(200, "text/plain; charset=utf-8",
                 "LampGo ESP32-P4 head board\nGET /device/status\nPOST /api/wifi\n");
  });
  server_.on("/device/status", HTTP_GET, [this]() { handleStatus(); });
  server_.on("/status", HTTP_GET, [this]() { handleProvisionStatus(); });
  server_.on("/scan", HTTP_GET, [this]() { handleScan(); });
  server_.on("/connect", HTTP_POST, [this]() { handleConnect(); });
  server_.on("/config", HTTP_GET, [this]() { handleConfigRead(); });
  server_.on("/config", HTTP_POST, [this]() { handleConfig(); });
  server_.on("/device/config", HTTP_GET, [this]() { handleConfigRead(); });
  server_.on("/device/config", HTTP_POST, [this]() { handleConfig(); });
  server_.on("/device/auth/challenge", HTTP_GET, [this]() { handleAuthChallenge(); });
  server_.on("/capture", HTTP_GET, [this]() { camera_.sendJpeg(server_); });
  server_.on("/device/pair", HTTP_POST, [this]() { handlePair(); });
  server_.on("/device/unpair", HTTP_POST, [this]() { handleUnpair(); });
  server_.on("/device/claim", HTTP_POST, [this]() { handleClaim(); });
  server_.on("/device/release", HTTP_POST, [this]() { handleClaim(); });
  server_.on("/device/led", HTTP_GET, [this]() { handleLedStatus(); });
  server_.on("/device/led", HTTP_POST, [this]() { handleLed(); });
  server_.on("/device/expressions/play", HTTP_POST, [this]() { handleExpressionPlay(); });
  server_.on("/device/expressions/stop", HTTP_POST, [this]() { handleExpressionStop(); });
  server_.on("/device/expression-clips", HTTP_GET,
             [this]() { handleAssetList(UploadKind::kEye); });
  server_.on("/device/expression-clips/delete", HTTP_POST,
             [this]() { handleAssetDelete(UploadKind::kEye); });
  server_.on(
      "/device/expression-clips/upload", HTTP_POST,
      [this]() { finishUploadRequest(UploadKind::kEye); },
      [this]() { handleUploadData(UploadKind::kEye); });
  server_.on("/device/led-effects", HTTP_GET,
             [this]() { handleAssetList(UploadKind::kLed); });
  server_.on("/device/led-effects/delete", HTTP_POST,
             [this]() { handleAssetDelete(UploadKind::kLed); });
  server_.on(
      "/device/led-effects/upload", HTTP_POST,
      [this]() { finishUploadRequest(UploadKind::kLed); },
      [this]() { handleUploadData(UploadKind::kLed); });
  server_.on("/device/clock", HTTP_POST, [this]() { handleClock(); });
  server_.on("/device/ocean", HTTP_POST, [this]() { handleOcean(); });
  server_.on("/api/wifi", HTTP_POST, [this]() { handleConnect(); });
  server_.on("/device/forget-wifi", HTTP_POST, [this]() { handleForgetWifi(); });
  server_.on("/device/reboot", HTTP_POST, [this]() { handleReboot(); });
  server_.on("/api/servos", HTTP_GET, [this]() { handleServoStatus(); });
  server_.onNotFound([this]() { sendError(404, "not found"); });
}

void DeviceHttp::handleProvisionStatus() {
  Preferences preferences;
  preferences.begin("lampgo-net", true);
  const bool configured = !preferences.getString("ssid", "").isEmpty();
  preferences.end();

  uint8_t mac[6]{};
  WiFi.macAddress(mac);
  char macText[18]{};
  snprintf(macText, sizeof(macText), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);

  JsonDocument response;
  response["ok"] = true;
  response["configured"] = configured;
  response["mode"] = WiFi.status() == WL_CONNECTED ? "sta" : "ap_sta";
  response["mac"] = macText;
  response["hostname"] = hostname_;
  response["ap_ssid"] = String(BoardConfig::kSetupSsidPrefix) + hostname_.substring(hostname_.length() - 4);
  response["pairing_supported"] = true;
  response["paired"] = pairing_.isPaired();
  response["paired_owner_id"] = pairing_.ownerId();
  sendJson(200, response);
}

void DeviceHttp::handleScan() {
  const int16_t count = WiFi.scanNetworks(false, true);
  JsonDocument response;
  response["ok"] = count >= 0;
  response["scan_ok"] = count >= 0;
  response["scan_result"] = count;
  JsonArray networks = response["networks"].to<JsonArray>();
  for (int index = 0; index < count && index < 30; ++index) {
    JsonObject network = networks.add<JsonObject>();
    network["ssid"] = WiFi.SSID(index);
    network["rssi"] = WiFi.RSSI(index);
    network["bssid"] = WiFi.BSSIDstr(index);
    network["channel"] = WiFi.channel(index);
    const bool open = WiFi.encryptionType(index) == WIFI_AUTH_OPEN;
    network["open"] = open;
    network["encrypt"] = !open;
  }
  if (count < 0) response["error"] = "scan_failed";
  WiFi.scanDelete();
  sendJson(200, response);
}

void DeviceHttp::handleConnect() {
  JsonDocument request;
  if (!parseJson(request)) return;
  JsonObjectConst body = request.as<JsonObjectConst>();
  const String ssid = body["ssid"] | "";
  const String password = body["password"] | "";
  if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 64) {
    sendError(400, "invalid WiFi credentials");
    return;
  }

  const bool wasPaired = pairing_.isPaired();
  String pairingError;
  if (!pairing_.pair(body["owner_id"] | "", body["owner_label"] | "",
                     body["pairing_secret"] | "", pairingError)) {
    sendError(pairing_.isPaired() ? 409 : 400, pairingError.c_str());
    return;
  }

  Preferences preferences;
  if (!preferences.begin("lampgo-net", false)) {
    if (!wasPaired) {
      pairing_.unpair(body["owner_id"] | "", body["pairing_secret"] | "");
    }
    sendError(500, "cannot open WiFi settings");
    return;
  }
  const String previousSsid = preferences.getString("ssid", "");
  const String previousPassword = preferences.getString("password", "");
  const bool saved = preferences.putString("ssid", ssid) > 0 &&
                     preferences.putString("password", password) == password.length();
  if (!saved) {
    if (previousSsid.isEmpty()) {
      preferences.clear();
    } else {
      preferences.putString("ssid", previousSsid);
      preferences.putString("password", previousPassword);
    }
  }
  preferences.end();
  if (!saved) {
    if (!wasPaired) {
      pairing_.unpair(body["owner_id"] | "", body["pairing_secret"] | "");
    }
    sendError(500, "cannot persist WiFi credentials");
    return;
  }

  JsonDocument response;
  response["ok"] = true;
  response["message"] = "restarting";
  response["paired"] = true;
  response["paired_owner_id"] = pairing_.ownerId();
  sendJson(200, response);
  scheduleRestart();
}

void DeviceHttp::handleStatus() {
  const ServoSnapshot snapshot = servos_.snapshot();
  JsonDocument response;
  response["ok"] = true;
  response["device_id"] = hostname_;
  response["hostname"] = hostname_;
  response["firmware"] = BoardConfig::kFirmwareVersion;
  response["platform"] = "esp32-p4";
  response["motion_port"] = BoardConfig::kMotionWsPort;
  response["asset_upload_port"] = BoardConfig::kAssetUploadPort;
  response["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  response["network_mode"] = WiFi.status() == WL_CONNECTED ? "sta" : "softap";
  response["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  response["pairing_supported"] = true;
  response["paired"] = pairing_.isPaired();
  response["pairing_state"] = pairing_.isPaired() ? "paired" : "unpaired";
  response["paired_owner_id"] = pairing_.ownerId();
  response["paired_owner_label"] = pairing_.ownerLabel();
  response["free_heap"] = ESP.getFreeHeap();
  response["free_psram"] = ESP.getFreePsram();
  addHostedDiagnostics(response);
  response["mic_streaming"] = audio_.microphoneReady() && audio_.microphoneEnabled();
  response["wake_ready"] = false;
  response["wake_event_clients"] = 0;
  response["audio_profile"] = audio_.profile();
  response["wake_model"] = "";
  response["wake_requested_model"] = "";
  response["wake_supported_models"].to<JsonArray>();
  JsonObject audio = response["audio"].to<JsonObject>();
  audio["sample_rate"] = BoardConfig::kAudioSampleRate;
  audio["ws_port"] = BoardConfig::kAudioWsPort;
  audio["microphone_ready"] = audio_.microphoneReady();
  audio["microphone_enabled"] = audio_.microphoneEnabled();
  audio["speaker_ready"] = audio_.speakerReady();
  audio["aec_ready"] = audio_.aecReady();
  audio["aec_enabled"] = audio_.aecEnabled();
  audio["speaker_volume"] = audio_.speakerVolume();
  audio["mic_frames"] = audio_.micFrames();
  audio["speaker_packets"] = audio_.speakerPackets();
  audio["speaker_drops"] = audio_.speakerDrops();
  audio["ws_clients"] = audio_.audioClientCount();
  audio["ws_connections"] = audio_.audioConnections();
  audio["ws_frames_queued"] = audio_.audioFramesQueued();
  audio["ws_frames_sent"] = audio_.audioFramesSent();
  audio["ws_send_failures"] = audio_.audioSendFailures();
  audio["ws_queue_failures"] = audio_.audioQueueFailures();
  audio["ws_auth_failures"] = audio_.audioAuthFailures();
  response["led_ready"] = expressions_.ledReady();
  response["led_mode"] = expressions_.currentMode();
  response["led_brightness"] = expressions_.currentBrightness();
  response["led_driver"] = "p4-rmt-buffered";
  response["led_pixel_pin"] = BoardConfig::kLedData;
  response["led_pixel_count"] = BoardConfig::kLedCount;
  response["led_width"] = BoardConfig::kLedWidth;
  response["led_height"] = BoardConfig::kLedHeight;
  response["led_panel_count"] = 1;
  response["led_output_ok"] = expressions_.ledReady();
  response["lcd_width"] = BoardConfig::kLcdWidth;
  response["lcd_height"] = BoardConfig::kLcdHeight;
  response["lcd_ready"] = expressions_.displayReady();
  JsonObject capabilities = response["capabilities"].to<JsonObject>();
  capabilities["motion_ws"] = true;
  capabilities["asset_upload_port"] = BoardConfig::kAssetUploadPort;
  capabilities["servo_count"] = BoardConfig::kServoCount;
  capabilities["led_pixels"] = BoardConfig::kLedCount;
  capabilities["lcd_width"] = BoardConfig::kLcdWidth;
  capabilities["lcd_height"] = BoardConfig::kLcdHeight;
  capabilities["lcd_ready"] = expressions_.displayReady();
  capabilities["camera"] = camera_.ready();
  capabilities["microphone"] = audio_.microphoneReady();
  capabilities["speaker"] = audio_.speakerReady();
  capabilities["aec"] = audio_.aecReady();
  capabilities["wake_word"] = false;
  addServoStatus(response["servos"].to<JsonObject>(), snapshot);
  sendJson(200, response);
}

void DeviceHttp::handleConfigRead() {
  JsonDocument response;
  response["ok"] = true;
  response["platform"] = "esp32-p4";
  response["audio_profile"] = audio_.profile();
  response["speaker_volume"] = audio_.speakerVolume();
  response["mic_enabled"] = audio_.microphoneEnabled();
  response["aec_ready"] = audio_.aecReady();
  response["aec_enabled"] = audio_.aecEnabled();
  response["wake_word_supported"] = false;
  response["wake_model"] = "";
  response["wake_requested_model"] = "";
  response["wake_supported_models"].to<JsonArray>();
  response["camera_ready"] = camera_.ready();
  response["jpeg_quality"] = camera_.jpegQuality();
  response["camera_width"] = camera_.width();
  response["camera_height"] = camera_.height();
  response["framesize_fixed"] = true;
  sendJson(200, response);
}

void DeviceHttp::handleAuthChallenge() {
  const String purpose = server_.arg("purpose");
  if (!purpose.startsWith("http:POST:/") && !purpose.startsWith("asset:POST:/")) {
    sendError(400, "invalid authentication purpose");
    return;
  }
  const String nonce = pairing_.issueChallenge(purpose);
  if (nonce.isEmpty()) {
    sendError(403, pairing_.isPaired() ? "cannot issue authentication challenge" : "device is not paired");
    return;
  }
  JsonDocument response;
  response["ok"] = true;
  response["purpose"] = purpose;
  response["nonce"] = nonce;
  sendJson(200, response);
}

void DeviceHttp::handleConfig() {
  JsonDocument request;
  if (!parseJson(request)) return;
  JsonObjectConst body = request.as<JsonObjectConst>();
  if (!authorize(body)) {
    sendError(403, "pairing mismatch");
    return;
  }
  if (!body["audio_profile"].isNull() &&
      !audio_.setProfile(String(body["audio_profile"] | ""))) {
    sendError(400, "unsupported audio profile");
    return;
  }
  if (!body["speaker_volume"].isNull()) {
    audio_.setSpeakerVolume(body["speaker_volume"].as<float>());
  }
  if (!body["mic_enabled"].isNull()) {
    audio_.setMicrophoneEnabled(body["mic_enabled"].as<bool>());
  }
  if (!body["jpeg_quality"].isNull() &&
      !camera_.setJpegQuality(body["jpeg_quality"].as<uint8_t>())) {
    sendError(400, "jpeg_quality must be 4..63");
    return;
  }
  JsonDocument response;
  response["ok"] = true;
  response["audio_profile"] = audio_.profile();
  response["aec_ready"] = audio_.aecReady();
  response["aec_enabled"] = audio_.aecEnabled();
  response["mic_enabled"] = audio_.microphoneEnabled();
  response["speaker_volume"] = audio_.speakerVolume();
  response["jpeg_quality"] = camera_.jpegQuality();
  response["camera_width"] = camera_.width();
  response["camera_height"] = camera_.height();
  response["framesize_fixed"] = true;
  response["wake_word_supported"] = false;
  sendJson(200, response);
}

void DeviceHttp::handlePair() {
  JsonDocument request;
  if (!parseJson(request)) return;
  JsonObjectConst body = request.as<JsonObjectConst>();
  String error;
  if (!pairing_.pair(body["owner_id"] | "", body["owner_label"] | "",
                     body["pairing_secret"] | "", error)) {
    sendError(409, error.c_str());
    return;
  }
  JsonDocument response;
  response["ok"] = true;
  response["paired"] = true;
  response["paired_owner_id"] = pairing_.ownerId();
  response["pairing_supported"] = true;
  sendJson(200, response);
}

void DeviceHttp::handleUnpair() {
  JsonDocument request;
  if (!parseJson(request)) return;
  JsonObjectConst body = request.as<JsonObjectConst>();
  if (!authorize(body) || !pairing_.clear()) {
    sendError(403, "pairing mismatch");
    return;
  }
  expressions_.stop();
  JsonDocument response;
  response["ok"] = true;
  response["paired"] = false;
  sendJson(200, response);
}

void DeviceHttp::handleClaim() {
  JsonDocument request;
  if (!parseJson(request)) return;
  if (!authorize(request.as<JsonObjectConst>())) {
    sendError(403, "pairing mismatch");
    return;
  }
  JsonDocument response;
  response["ok"] = true;
  response["owner_id"] = pairing_.ownerId();
  response["ttl_ms"] = 120000;
  sendJson(200, response);
}

void DeviceHttp::handleLed() {
  JsonDocument request;
  if (!parseJson(request)) return;
  JsonObjectConst body = request.as<JsonObjectConst>();
  if (!authorize(body)) {
    sendError(403, "pairing mismatch");
    return;
  }
  const String diagnostic = body["diagnostic"] | "";
  if (diagnostic == "topology") {
    expressions_.showLedTopologyTest(body["brightness"] | 8);
  } else if (!body["enabled"].isNull() && !body["enabled"].as<bool>()) {
    expressions_.stop();
  } else if (!body["expression"].isNull() || !body["mode"].isNull() ||
             !body["led_mode"].isNull() || !body["clip_id"].isNull()) {
    const String expression = body["expression"] | "focused";
    const int mode = !body["mode"].isNull() ? body["mode"].as<int>() :
                     !body["led_mode"].isNull() ? body["led_mode"].as<int>() : -1;
    const uint8_t brightness = body["brightness"] | BoardConfig::kLedSafeBrightness;
    const String eye = body["clip_id"] | "";
    if (!expressions_.play(expression, mode, eye, "", brightness, true)) {
      sendError(400, "invalid expression");
      return;
    }
  } else if (!body["brightness"].isNull()) {
    expressions_.setBrightness(body["brightness"].as<uint8_t>());
  }
  JsonDocument response;
  response["ok"] = true;
  response["mode"] = expressions_.currentMode();
  if (diagnostic == "topology") response["diagnostic"] = "topology";
  sendJson(200, response);
}

void DeviceHttp::handleLedStatus() {
  JsonDocument response;
  response["ok"] = true;
  response["mode"] = expressions_.currentMode();
  response["brightness"] = expressions_.currentBrightness();
  response["eye_clip_id"] = expressions_.currentEyeClip();
  response["led_effect_id"] = expressions_.currentLedEffect();
  sendJson(200, response);
}

void DeviceHttp::handleClock() {
  JsonDocument request;
  if (!parseJson(request)) return;
  JsonObjectConst body = request.as<JsonObjectConst>();
  if (!authorize(body)) {
    sendError(403, "pairing mismatch");
    return;
  }
  if (!body["enabled"].isNull() && !body["enabled"].as<bool>()) {
    expressions_.stop();
  } else {
    expressions_.showClock(body["hour"] | 0, body["minute"] | 0,
                           parseColor(String(body["color"] | "#ffffff")),
                           body["brightness"] | BoardConfig::kLedSafeBrightness,
                           parseClockEffect(String(body["effect"] | "steady")));
  }
  JsonDocument response;
  response["ok"] = true;
  sendJson(200, response);
}

void DeviceHttp::handleOcean() {
  JsonDocument request;
  if (!parseJson(request)) return;
  JsonObjectConst body = request.as<JsonObjectConst>();
  if (!authorize(body)) {
    sendError(403, "pairing mismatch");
    return;
  }

  const String action = body["action"] | "";
  if (action == "start") {
    expressions_.startOcean(
        parseColor(String(body["color"] | "#37d6ff")),
        static_cast<uint8_t>(std::max(1, std::min<int>(
            BoardConfig::kLedSafeBrightness, body["brightness"] | BoardConfig::kLedSafeBrightness))),
        percent(body["fill_percent"], 55), range(body["sensitivity_percent"], 100, 25, 200),
        percent(body["edge_highlight_percent"], 75), range(body["tilt_percent"], 100, 50, 160),
        range(body["impact_percent"], 100, 0, 200), range(body["damping_percent"], 130, 80, 200));
  } else if (action == "input") {
    expressions_.updateOcean(body["angle_deg"] | 0.0f,
                             body["angular_velocity_dps"] | 0.0f,
                             body["sequence"] | 0U);
  } else if (action == "stop") {
    expressions_.stopOcean();
  } else {
    sendError(400, "ocean action must be start, input, or stop");
    return;
  }

  JsonDocument response;
  response["ok"] = true;
  response["action"] = action;
  sendJson(200, response);
}

void DeviceHttp::handleExpressionPlay() {
  JsonDocument request;
  if (!parseJson(request)) return;
  JsonObjectConst body = request.as<JsonObjectConst>();
  if (!authorize(body)) {
    sendError(403, "pairing mismatch");
    return;
  }
  const String preset = body["preset_id"] | "";
  const String expression = preset.isEmpty() ? String(body["expression"] | "focused") : preset;
  const int mode = !body["led_mode"].isNull() ? body["led_mode"].as<int>() : -1;
  const String eye = body["eye_clip_id"] | "";
  const String led = body["led_effect_id"] | "";
  JsonObjectConst ledParams = body["led_params"].as<JsonObjectConst>();
  const uint8_t brightness = ledParams["brightness"] | BoardConfig::kLedSafeBrightness;
  const String playback = body["playback"] | "loop";
  if (!expressions_.play(expression, mode, eye, led, brightness, playback != "once")) {
    sendError(400, "invalid expression composition");
    return;
  }
  JsonDocument response;
  response["ok"] = true;
  response["started_at_ms"] = millis();
  response["display_confirmed"] = true;
  sendJson(200, response);
}

void DeviceHttp::handleExpressionStop() {
  JsonDocument request;
  if (!parseJson(request)) return;
  if (!authorize(request.as<JsonObjectConst>())) {
    sendError(403, "pairing mismatch");
    return;
  }
  expressions_.stop();
  JsonDocument response;
  response["ok"] = true;
  sendJson(200, response);
}

void DeviceHttp::handleAssetList(UploadKind kind) {
  JsonDocument response;
  response["ok"] = true;
  JsonArray assets = response["result"][kind == UploadKind::kEye ? "expression_clips" : "led_effects"]
                         .to<JsonArray>();
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  const String prefix = kind == UploadKind::kEye ? "/eye_" : "/led_";
  while (file) {
    const String name = file.name();
    if (name.startsWith(prefix) && name.endsWith(".bin")) {
      JsonObject asset = assets.add<JsonObject>();
      asset[kind == UploadKind::kEye ? "clip_id" : "effect_id"] =
          name.substring(prefix.length(), name.length() - 4);
      asset["bytes"] = file.size();
    }
    file = root.openNextFile();
  }
  sendJson(200, response);
}

void DeviceHttp::handleAssetDelete(UploadKind kind) {
  JsonDocument request;
  if (!parseJson(request)) return;
  JsonObjectConst body = request.as<JsonObjectConst>();
  if (!authorize(body)) {
    sendError(403, "pairing mismatch");
    return;
  }
  const String id = kind == UploadKind::kEye ? String(body["clip_id"] | "") :
                                               String(body["effect_id"] | "");
  if (!ExpressionCoordinator::safeAssetId(id)) {
    sendError(400, "invalid asset id");
    return;
  }
  JsonDocument response;
  response["ok"] = LittleFS.remove(assetPath(kind, id));
  sendJson(response["ok"].as<bool>() ? 200 : 404, response);
}

void DeviceHttp::handleUploadData(UploadKind kind) {
  const String phase = server_.header(kUploadPhaseHeader);
  if (!phase.isEmpty()) {
    HTTPRaw& raw = server_.raw();
    const String id = kind == UploadKind::kEye ? server_.header(kClipIdHeader) :
                                                 server_.header(kEffectIdHeader);
    if (phase == "start") {
      if (raw.status == RAW_START) {
        startUpload(kind, id, authorizeRequest(String("asset:POST:") + server_.uri() + ":start"));
      } else if (raw.status == RAW_WRITE && raw.currentSize != 0) {
        uploadOk_ = false;
        uploadError_ = "start body must be empty";
      } else if (raw.status == RAW_ABORTED) {
        endUpload(kind, true);
      }
    } else if (phase == "chunk") {
      if (raw.status == RAW_START) {
        if (!authorizeRequest(String("asset:POST:") + server_.uri() + ":chunk") || !uploadOk_ || kind != uploadKind_ || id != uploadId_ || !uploadFile_) {
          uploadOk_ = false;
          uploadError_ = "no matching asset upload";
        }
      } else if (raw.status == RAW_WRITE) {
        appendUpload(kind, raw.buf, raw.currentSize);
      } else if (raw.status == RAW_ABORTED) {
        endUpload(kind, true);
      }
    } else if (phase == "finish") {
      if (raw.status == RAW_START) {
        if (!authorizeRequest(String("asset:POST:") + server_.uri() + ":finish") || !uploadOk_ || kind != uploadKind_ || id != uploadId_ || !uploadFile_) {
          uploadOk_ = false;
          uploadError_ = "no matching asset upload";
        }
      } else if (raw.status == RAW_WRITE && raw.currentSize != 0) {
        uploadOk_ = false;
        uploadError_ = "finish body must be empty";
      } else if (raw.status == RAW_ABORTED) {
        endUpload(kind, true);
      }
    } else {
      uploadOk_ = false;
      uploadError_ = "invalid upload phase";
    }
    return;
  }

  if (!server_.header(kContentTypeHeader).startsWith("multipart/")) {
    HTTPRaw& raw = server_.raw();
    if (raw.status == RAW_START) {
      const String id = kind == UploadKind::kEye ? server_.header(kClipIdHeader) :
                                                   server_.header(kEffectIdHeader);
      startUpload(kind, id, authorizeRequest(String("asset:POST:") + server_.uri() + ":single"));
    } else if (raw.status == RAW_WRITE) {
      appendUpload(kind, raw.buf, raw.currentSize);
    } else if (raw.status == RAW_END || raw.status == RAW_ABORTED) {
      endUpload(kind, raw.status == RAW_ABORTED);
    }
    return;
  }

  HTTPUpload& upload = server_.upload();
  if (upload.status == UPLOAD_FILE_START) {
    startUpload(kind, server_.arg(kind == UploadKind::kEye ? "clip_id" : "effect_id"),
                authorizeRequest(String("asset:POST:") + server_.uri() + ":single"));
  } else if (upload.status == UPLOAD_FILE_WRITE && uploadOk_) {
    appendUpload(kind, upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END || upload.status == UPLOAD_FILE_ABORTED) {
    endUpload(kind, upload.status == UPLOAD_FILE_ABORTED);
  }
}

void DeviceHttp::finishUploadRequest(UploadKind kind) {
  const String phase = server_.header(kUploadPhaseHeader);
  if (phase.isEmpty()) {
    finishUpload(kind);
    return;
  }

  const HTTPRaw& raw = server_.raw();
  if (phase == "start") {
    if (raw.totalSize != 0) {
      uploadOk_ = false;
      uploadError_ = "start body must be empty";
    }
    if (!uploadOk_) {
      if (uploadFile_) uploadFile_.close();
      LittleFS.remove(uploadTempPath_);
      sendError(400, uploadError_.isEmpty() ? "cannot start asset upload" : uploadError_.c_str());
      return;
    }
    sendUploadProgress("started");
    return;
  }
  if (phase == "chunk") {
    if (raw.totalSize == 0) {
      uploadOk_ = false;
      uploadError_ = "asset chunk must not be empty";
    }
    if (!uploadOk_) {
      if (uploadFile_) uploadFile_.close();
      LittleFS.remove(uploadTempPath_);
      sendError(400, uploadError_.isEmpty() ? "asset chunk rejected" : uploadError_.c_str());
      return;
    }
    sendUploadProgress("chunk");
    return;
  }
  if (phase == "finish") {
    if (raw.totalSize != 0) {
      uploadOk_ = false;
      uploadError_ = "finish body must be empty";
    }
    if (!uploadOk_) {
      if (uploadFile_) uploadFile_.close();
      LittleFS.remove(uploadTempPath_);
      sendError(400, uploadError_.isEmpty() ? "asset upload failed" : uploadError_.c_str());
      return;
    }
    finishUpload(kind);
    return;
  }
  sendError(400, "invalid upload phase");
}

void DeviceHttp::startUpload(UploadKind kind, const String& assetId, bool authorized) {
  if (uploadFile_) uploadFile_.close();
  uploadKind_ = kind;
  uploadId_ = assetId;
  uploadTempPath_ = kind == UploadKind::kEye ? "/upload_eye.tmp" : "/upload_led.tmp";
  uploadBytes_ = 0;
  uploadOk_ = authorized && ExpressionCoordinator::safeAssetId(uploadId_);
  uploadError_ = uploadOk_ ? "" : "unauthorized or invalid asset id";
  if (uploadOk_) {
    LittleFS.remove(uploadTempPath_);
    uploadFile_ = LittleFS.open(uploadTempPath_, "w");
  }
  if (uploadOk_ && !uploadFile_) {
    uploadOk_ = false;
    uploadError_ = "cannot open upload file";
  }
  Serial.printf("[HTTP UPLOAD] start kind=%s id=%s accepted=%d\n",
                kind == UploadKind::kEye ? "eye" : "led", uploadId_.c_str(), uploadOk_);
}

void DeviceHttp::appendUpload(UploadKind kind, const uint8_t* data, size_t size) {
  if (!uploadOk_) return;
  const size_t limit = kind == UploadKind::kEye ? kMaxEyeBytes : kMaxLedBytes;
  if (uploadBytes_ + size > limit || uploadFile_.write(data, size) != size) {
    uploadOk_ = false;
    uploadError_ = "asset exceeds limit or storage write failed";
    return;
  }
  uploadBytes_ += size;
}

void DeviceHttp::endUpload(UploadKind kind, bool aborted) {
  if (uploadFile_) uploadFile_.close();
  if (aborted) {
    uploadOk_ = false;
    uploadError_ = "upload aborted";
  }
  Serial.printf("[HTTP UPLOAD] end kind=%s bytes=%u ok=%d\n",
                kind == UploadKind::kEye ? "eye" : "led",
                static_cast<unsigned int>(uploadBytes_), uploadOk_);
}

void DeviceHttp::finishUpload(UploadKind kind) {
  if (kind != uploadKind_ || !uploadOk_) {
    LittleFS.remove(uploadTempPath_);
    sendError(400, uploadError_.isEmpty() ? "upload failed" : uploadError_.c_str());
    return;
  }
  if (uploadFile_) uploadFile_.close();
  String error;
  if (!validateStoredAsset(kind, uploadTempPath_, error)) {
    LittleFS.remove(uploadTempPath_);
    sendError(400, error.c_str());
    return;
  }
  const String destination = assetPath(kind, uploadId_);
  LittleFS.remove(destination);
  if (!LittleFS.rename(uploadTempPath_, destination)) {
    LittleFS.remove(uploadTempPath_);
    sendError(500, "cannot commit uploaded asset");
    return;
  }
  JsonDocument response;
  response["ok"] = true;
  response["action"] = "upload";
  response[kind == UploadKind::kEye ? "clip_id" : "effect_id"] = uploadId_;
  response["bytes"] = uploadBytes_;
  if (kind == UploadKind::kEye) response["display_confirmed"] = true;
  sendJson(200, response);
}

void DeviceHttp::sendUploadProgress(const char* action) {
  JsonDocument response;
  response["ok"] = true;
  response["action"] = action;
  sendJson(200, response);
}

void DeviceHttp::handleForgetWifi() {
  JsonDocument request;
  if (!parseJson(request)) return;
  if (!authorize(request.as<JsonObjectConst>())) {
    sendError(403, "pairing mismatch");
    return;
  }
  Preferences preferences;
  if (!preferences.begin("lampgo-net", false)) {
    sendError(500, "cannot open WiFi settings");
    return;
  }
  const bool cleared = preferences.clear();
  preferences.end();
  if (!cleared) {
    sendError(500, "cannot clear WiFi settings");
    return;
  }
  if (!pairing_.clear()) {
    sendError(500, "cannot clear pairing");
    return;
  }
  expressions_.stop();
  JsonDocument response;
  response["ok"] = true;
  response["restarting"] = true;
  sendJson(200, response);
  scheduleRestart();
}

void DeviceHttp::handleReboot() {
  JsonDocument request;
  if (!parseJson(request)) return;
  if (!authorize(request.as<JsonObjectConst>())) {
    sendError(403, "pairing mismatch");
    return;
  }
  JsonDocument response;
  response["ok"] = true;
  response["restarting"] = true;
  sendJson(200, response);
  scheduleRestart();
}

void DeviceHttp::handleServoStatus() {
  JsonDocument response;
  response["ok"] = true;
  addServoStatus(response["servos"].to<JsonObject>(), servos_.snapshot());
  sendJson(200, response);
}

bool DeviceHttp::parseJson(JsonDocument& document) {
  if (deserializeJson(document, server_.arg("plain")) || !document.is<JsonObject>()) {
    sendError(400, "invalid JSON body");
    return false;
  }
  return true;
}

bool DeviceHttp::authorize(JsonObjectConst body) const {
  const String purpose = String("http:POST:") + server_.uri();
  return String(body["auth_purpose"] | "") == purpose &&
         pairing_.authorizeProof(body["owner_id"] | "", purpose, body["auth_nonce"] | "",
                                body["auth_proof"] | "");
}

bool DeviceHttp::authorizeRequest(const String& purpose) const {
  return server_.header(kAuthPurposeHeader) == purpose &&
         pairing_.authorizeProof(server_.header(kOwnerHeader), purpose,
                                server_.header(kAuthNonceHeader), server_.header(kAuthProofHeader));
}

bool DeviceHttp::validateStoredAsset(UploadKind kind, const String& path, String& error) const {
  File file = LittleFS.open(path, "r");
  if (!file) {
    error = "uploaded asset is missing";
    return false;
  }
  char magic[6]{};
  const size_t magicSize = kind == UploadKind::kEye ? 6 : 4;
  if (file.readBytes(magic, magicSize) != magicSize ||
      (kind == UploadKind::kEye ? memcmp(magic, "LGLCD1", 6) : memcmp(magic, "LEF1", 4)) != 0) {
    error = "invalid asset magic";
    return false;
  }
  return true;
}

String DeviceHttp::assetPath(UploadKind kind, const String& id) const {
  return kind == UploadKind::kEye ? ExpressionCoordinator::eyePath(id) :
                                    ExpressionCoordinator::ledPath(id);
}

void DeviceHttp::sendJson(int status, JsonDocument& document) {
  String encoded;
  serializeJson(document, encoded);
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.send(status, "application/json", encoded);
}

void DeviceHttp::sendError(int status, const char* error) {
  JsonDocument response;
  response["ok"] = false;
  response["error"] = error;
  sendJson(status, response);
}

void DeviceHttp::scheduleRestart(uint32_t delayMs) {
  restartAtMs_ = millis() + delayMs;
}

void DeviceHttp::addServoStatus(JsonObject target, const ServoSnapshot& snapshot) const {
  target["startup_state"] = startupStateName(snapshot.startupState);
  target["recovery_reason"] = snapshot.recoveryReason;
  target["torque_enabled"] = snapshot.torqueEnabled;
  target["estopped"] = snapshot.estopped;
  target["profile_loaded"] = snapshot.profileLoaded;
  target["safety_clamp_count"] = snapshot.safetyClampCount;
  JsonArray motors = target["motors"].to<JsonArray>();
  for (uint8_t index = 0; index < BoardConfig::kServoCount; ++index) {
    JsonObject motor = motors.add<JsonObject>();
    motor["id"] = index + 1;
    motor["online"] = snapshot.online[index];
    motor["position"] = snapshot.position[index];
    motor["speed_raw"] = snapshot.speed[index];
    motor["load_raw"] = snapshot.load[index];
    motor["voltage_raw"] = snapshot.voltage[index];
    motor["temperature_raw"] = snapshot.temperature[index];
    motor["current_raw"] = snapshot.current[index];
  }
}
