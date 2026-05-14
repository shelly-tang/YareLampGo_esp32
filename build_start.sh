#!/bin/sh
set -e

FQBN="esp32:esp32:XIAO_ESP32S3:PSRAM=opi"
BUILD_PATH="/private/tmp/lampgo_esp32_camera_build"
MODEL_BUILD_PATH="/private/tmp/lampgo_jarvis_model_build"
ESP_SR_PATH="${ESP_SR_PATH:-/private/tmp/esp-sr-master-src}"
ESP_SR_ZIP="/private/tmp/esp-sr-master.zip"
ESPTOOL="${ESPTOOL:-$HOME/Library/Arduino15/packages/esp32/tools/esptool_py/5.2.0/esptool}"
PORT="${ESP32_PORT:-/dev/cu.usbmodem21401}"
MODEL_OFFSET="0x3F0000"
MODEL_SIZE="0x400000"

arduino-cli compile \
  --fqbn "$FQBN" \
  --build-property build.partitions=lampgo_sr_8mb \
  --build-path "$BUILD_PATH" \
  .

if [ ! -f "$ESP_SR_PATH/model/movemodel.py" ]; then
  echo "Downloading ESP-SR model source for Jarvis WakeNet..."
  curl -L --fail https://github.com/espressif/esp-sr/archive/refs/heads/master.zip -o "$ESP_SR_ZIP"
  rm -rf /private/tmp/esp-sr-master /private/tmp/esp-sr-master-src
  unzip -q "$ESP_SR_ZIP" -d /private/tmp
  mv /private/tmp/esp-sr-master "$ESP_SR_PATH"
fi

mkdir -p "$MODEL_BUILD_PATH"
printf "%s\n" "CONFIG_SR_WN_WN9_JARVIS_TTS=y" > "$MODEL_BUILD_PATH/sdkconfig"
python "$ESP_SR_PATH/model/movemodel.py" \
  -d1 "$MODEL_BUILD_PATH/sdkconfig" \
  -d2 "$ESP_SR_PATH" \
  -d3 "$MODEL_BUILD_PATH"
MODEL_BIN="$MODEL_BUILD_PATH/srmodels/srmodels.bin"

arduino-cli upload -p "$PORT" --fqbn "$FQBN" --input-dir "$BUILD_PATH" .
"$ESPTOOL" --chip esp32s3 -p "$PORT" erase-region "$MODEL_OFFSET" "$MODEL_SIZE"
"$ESPTOOL" --chip esp32s3 -p "$PORT" write-flash "$MODEL_OFFSET" "$MODEL_BIN"
arduino-cli monitor -p "$PORT" -c baudrate=115200
