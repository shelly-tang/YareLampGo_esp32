// SPDX-FileCopyrightText: 2026 LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

// The Arduino ESP32-P4 core bundles esp_cam_sensor with OV5640 enabled, but
// registers it only for MIPI auto-detection.  LampGo's head board wires the
// same sensor over DVP, so ESP_Video otherwise tries every other DVP sensor
// and never probes SCCB address 0x3c.  Keep this board-specific registry in
// the sketch rather than modifying a developer's Arduino installation.

extern "C" {
#include "esp_cam_sensor_detect.h"
#include "esp_cam_sensor_types.h"

esp_cam_sensor_device_t* ov5640_detect(esp_cam_sensor_config_t* config);
}

namespace {
constexpr uint16_t kOv5640SccbAddress = 0x3c;

esp_cam_sensor_device_t* detectOv5640Dvp(void* rawConfig) {
  auto* config = static_cast<esp_cam_sensor_config_t*>(rawConfig);
  config->sensor_port = ESP_CAM_SENSOR_DVP;
  return ov5640_detect(config);
}

esp_cam_sensor_detect_fn_t kLampGoDvpSensorDetectors[] = {
    {
        .detect = detectOv5640Dvp,
        .port = ESP_CAM_SENSOR_DVP,
        .sccb_addr = kOv5640SccbAddress,
    },
};
}  // namespace

extern "C" void esp_cam_sensor_detect_get_array(
    esp_cam_sensor_detect_fn_t** arrayStart,
    esp_cam_sensor_detect_fn_t** arrayEnd) {
  *arrayStart = kLampGoDvpSensorDetectors;
  *arrayEnd = kLampGoDvpSensorDetectors + 1;
}
