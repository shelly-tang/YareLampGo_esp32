#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SKETCH_NAME="ESP32_CAMERA"
FQBN="${FQBN:-esp32:esp32:XIAO_ESP32S3:PSRAM=opi}"
PARTITION_NAME="${PARTITION_NAME:-lampgo_sr_8mb}"
BUILD_PATH="${BUILD_PATH:-/private/tmp/lampgo_esp32_camera_build}"
MODEL_OFFSET="${MODEL_OFFSET:-0x3F0000}"
MODEL_SIZE="${MODEL_SIZE:-0x400000}"
BAUD="${ESP32_BAUD:-921600}"
PORT="${ESP32_PORT:-}"

PREBUILT_DIR=""
PACKAGE_DIR=""
ERASE_FLASH=0
MONITOR=0
BUILD_ONLY=0
LIST_PORTS=0

ARDUINO_CLI=""
ESPTOOL_CMD=()
TEMP_SKETCH_DIR=""

usage() {
  cat <<'EOF'
LampGo ESP32 firmware flasher

Usage:
  scripts/flash.sh [options]

Common:
  scripts/flash.sh --erase --monitor
  scripts/flash.sh --port /dev/cu.usbmodem2101 --erase
  scripts/flash.sh --prebuilt ./dist/lampgo-esp32-firmware --erase
  scripts/flash.sh --build-only --package ./dist/lampgo-esp32-firmware

Options:
  --port PORT        ESP32 serial port. Can also use ESP32_PORT.
  --erase            Erase the whole flash before writing firmware.
                     This clears WiFi credentials and pairing state.
  --monitor          Open a 115200 baud serial monitor after flashing.
  --build-only       Compile/package only; do not flash.
  --prebuilt DIR     Flash an already-built firmware package.
                     This path does not need Arduino, only esptool.
  --package DIR      Copy build outputs into a release-friendly directory.
  --list-ports       Print likely serial ports and exit.
  -h, --help         Show this help.

Environment:
  ESP32_PORT         Same as --port.
  ESP32_BAUD         Upload baud rate, default 921600.
  ARDUINO_CLI        Path to arduino-cli. Auto-detected on PATH and macOS
                     Arduino IDE installs.
  ESPTOOL            Path to esptool/esptool.py. Auto-detected from Arduino
                     ESP32 core, PATH, or python -m esptool.
EOF
}

log() {
  printf '[lampgo-flash] %s\n' "$*"
}

die() {
  printf '[lampgo-flash] ERROR: %s\n' "$*" >&2
  exit 1
}

cleanup() {
  if [ -n "${TEMP_SKETCH_DIR:-}" ] && [ -d "$TEMP_SKETCH_DIR" ]; then
    rm -rf "$(dirname "$TEMP_SKETCH_DIR")"
  fi
}
trap cleanup EXIT

while [ "$#" -gt 0 ]; do
  case "$1" in
    --port)
      [ "$#" -ge 2 ] || die "--port needs a value"
      PORT="$2"
      shift 2
      ;;
    --erase)
      ERASE_FLASH=1
      shift
      ;;
    --monitor)
      MONITOR=1
      shift
      ;;
    --build-only)
      BUILD_ONLY=1
      shift
      ;;
    --prebuilt)
      [ "$#" -ge 2 ] || die "--prebuilt needs a directory"
      PREBUILT_DIR="$2"
      shift 2
      ;;
    --package)
      [ "$#" -ge 2 ] || die "--package needs a directory"
      PACKAGE_DIR="$2"
      shift 2
      ;;
    --list-ports)
      LIST_PORTS=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown option: $1"
      ;;
  esac
done

resolve_arduino_cli() {
  if [ -n "${ARDUINO_CLI:-}" ] && [ -x "$ARDUINO_CLI" ]; then
    return 0
  fi
  if command -v arduino-cli >/dev/null 2>&1; then
    ARDUINO_CLI="$(command -v arduino-cli)"
    return 0
  fi
  local mac_ide_cli="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
  if [ -x "$mac_ide_cli" ]; then
    ARDUINO_CLI="$mac_ide_cli"
    return 0
  fi
  return 1
}

resolve_esptool() {
  ESPTOOL_CMD=()
  if [ -n "${ESPTOOL:-}" ] && [ -x "$ESPTOOL" ]; then
    ESPTOOL_CMD=("$ESPTOOL")
    return 0
  fi
  if command -v esptool.py >/dev/null 2>&1; then
    ESPTOOL_CMD=("$(command -v esptool.py)")
    return 0
  fi
  if command -v esptool >/dev/null 2>&1; then
    ESPTOOL_CMD=("$(command -v esptool)")
    return 0
  fi
  local arduino_data_dir
  for arduino_data_dir in "$HOME/Library/Arduino15" "$HOME/.arduino15"; do
    local arduino_esptool="$arduino_data_dir/packages/esp32/tools/esptool_py/5.2.0/esptool"
    if [ -x "$arduino_esptool" ]; then
      ESPTOOL_CMD=("$arduino_esptool")
      return 0
    fi
  done
  local py=""
  py="$(command -v python3 || true)"
  if [ -n "$py" ] && "$py" -c 'import esptool' >/dev/null 2>&1; then
    ESPTOOL_CMD=("$py" "-m" "esptool")
    return 0
  fi
  return 1
}

print_ports() {
  if resolve_arduino_cli; then
    "$ARDUINO_CLI" board list || true
    return
  fi
  shopt -s nullglob
  local ports=(/dev/cu.usbmodem* /dev/cu.usbserial* /dev/ttyACM* /dev/ttyUSB*)
  shopt -u nullglob
  if [ "${#ports[@]}" -eq 0 ]; then
    log "No USB serial ports found."
    return
  fi
  printf '%s\n' "${ports[@]}"
}

detect_port() {
  if [ -n "$PORT" ]; then
    return 0
  fi
  shopt -s nullglob
  local ports=(/dev/cu.usbmodem* /dev/cu.usbserial* /dev/ttyACM* /dev/ttyUSB*)
  shopt -u nullglob
  if [ "${#ports[@]}" -eq 1 ]; then
    PORT="${ports[0]}"
    log "Using detected port: $PORT"
    return 0
  fi
  if [ "${#ports[@]}" -gt 1 ]; then
    printf '[lampgo-flash] Multiple candidate ports found:\n' >&2
    printf '  %s\n' "${ports[@]}" >&2
    die "choose one with --port PORT"
  fi
  die "no ESP32 serial port found; connect USB or pass --port PORT"
}

prepare_sketch_copy() {
  local temp_base="${TMPDIR:-/tmp}/lampgo_esp32_sketch_$$"
  TEMP_SKETCH_DIR="$temp_base/$SKETCH_NAME"
  rm -rf "$temp_base"
  mkdir -p "$TEMP_SKETCH_DIR"
  rsync -a --exclude .git --exclude build --exclude dist "$ROOT_DIR/" "$TEMP_SKETCH_DIR/"
}

ensure_esp32_core() {
  "$ARDUINO_CLI" core list | grep -q '^esp32:esp32[[:space:]]' || die "Arduino ESP32 core is not installed. Install it in Arduino IDE Boards Manager, or run: arduino-cli core install esp32:esp32"
}

build_from_source() {
  resolve_arduino_cli || die "arduino-cli not found. Install Arduino IDE/arduino-cli, or use --prebuilt DIR."
  ensure_esp32_core
  prepare_sketch_copy
  log "Compiling with $ARDUINO_CLI"
  "$ARDUINO_CLI" compile \
    --fqbn "$FQBN" \
    --build-property "build.partitions=$PARTITION_NAME" \
    --build-path "$BUILD_PATH" \
    "$TEMP_SKETCH_DIR"
  [ -f "$BUILD_PATH/$SKETCH_NAME.ino.bin" ] || die "app binary missing after compile"
  [ -f "$BUILD_PATH/srmodels.bin" ] || die "WakeNet model binary missing after compile: $BUILD_PATH/srmodels.bin"
}

find_boot_app0() {
  if [ -f "$BUILD_PATH/boot_app0.bin" ]; then
    printf '%s\n' "$BUILD_PATH/boot_app0.bin"
    return 0
  fi
  local arduino_data_dir
  for arduino_data_dir in "$HOME/Library/Arduino15" "$HOME/.arduino15"; do
    local found=""
    found="$(find "$arduino_data_dir/packages/esp32/hardware/esp32" -path '*/tools/partitions/boot_app0.bin' 2>/dev/null | sort | tail -1 || true)"
    if [ -n "$found" ] && [ -f "$found" ]; then
      printf '%s\n' "$found"
      return 0
    fi
  done
  return 1
}

package_build() {
  [ -n "$PACKAGE_DIR" ] || return 0
  local boot_app0=""
  boot_app0="$(find_boot_app0)" || die "boot_app0.bin not found; cannot create a complete prebuilt package"
  mkdir -p "$PACKAGE_DIR"
  cp "$BUILD_PATH/$SKETCH_NAME.ino.bin" "$PACKAGE_DIR/"
  cp "$BUILD_PATH/$SKETCH_NAME.ino.bootloader.bin" "$PACKAGE_DIR/"
  cp "$BUILD_PATH/$SKETCH_NAME.ino.partitions.bin" "$PACKAGE_DIR/"
  cp "$BUILD_PATH/srmodels.bin" "$PACKAGE_DIR/"
  cp "$boot_app0" "$PACKAGE_DIR/boot_app0.bin"
  if [ -f "$BUILD_PATH/$SKETCH_NAME.ino.merged.bin" ]; then
    cp "$BUILD_PATH/$SKETCH_NAME.ino.merged.bin" "$PACKAGE_DIR/"
  fi
  cp "$SCRIPT_DIR/flash.sh" "$PACKAGE_DIR/flash.sh"
  chmod +x "$PACKAGE_DIR/flash.sh"
  cat > "$PACKAGE_DIR/flash_args.txt" <<EOF
--flash-mode dio --flash-freq 80m --flash-size 8MB
0x0 $SKETCH_NAME.ino.bootloader.bin
0x8000 $SKETCH_NAME.ino.partitions.bin
0xe000 boot_app0.bin
0x10000 $SKETCH_NAME.ino.bin
$MODEL_OFFSET srmodels.bin
EOF
  cat > "$PACKAGE_DIR/README_FLASH.txt" <<EOF
LampGo ESP32 prebuilt firmware package

List ports:
  ./flash.sh --list-ports

Clean flash:
  ./flash.sh --prebuilt . --port /dev/cu.usbmodem2101 --erase --monitor

If you do not have Arduino, install esptool first:
  python3 -m pip install --user esptool
EOF
  if command -v shasum >/dev/null 2>&1; then
    (cd "$PACKAGE_DIR" && shasum -a 256 *.bin > SHA256SUMS)
  fi
  log "Packaged firmware into $PACKAGE_DIR"
}

require_file() {
  [ -f "$1" ] || die "missing file: $1"
}

flash_prebuilt() {
  local dir="$1"
  require_file "$dir/$SKETCH_NAME.ino.bootloader.bin"
  require_file "$dir/$SKETCH_NAME.ino.partitions.bin"
  require_file "$dir/boot_app0.bin"
  require_file "$dir/$SKETCH_NAME.ino.bin"
  require_file "$dir/srmodels.bin"
  detect_port
  resolve_esptool || die "esptool not found. Install it with: python3 -m pip install --user esptool"
  if [ "$ERASE_FLASH" -eq 1 ]; then
    log "Erasing flash on $PORT"
    "${ESPTOOL_CMD[@]}" --chip esp32s3 -p "$PORT" -b "$BAUD" erase_flash
  fi
  log "Writing prebuilt firmware to $PORT"
  "${ESPTOOL_CMD[@]}" --chip esp32s3 -p "$PORT" -b "$BAUD" write-flash \
    --flash-mode dio --flash-freq 80m --flash-size 8MB \
    0x0 "$dir/$SKETCH_NAME.ino.bootloader.bin" \
    0x8000 "$dir/$SKETCH_NAME.ino.partitions.bin" \
    0xe000 "$dir/boot_app0.bin" \
    0x10000 "$dir/$SKETCH_NAME.ino.bin" \
    "$MODEL_OFFSET" "$dir/srmodels.bin"
}

flash_from_build() {
  detect_port
  resolve_esptool || die "esptool not found. Install Arduino ESP32 core or python esptool."
  if [ "$ERASE_FLASH" -eq 1 ]; then
    log "Erasing flash on $PORT"
    "${ESPTOOL_CMD[@]}" --chip esp32s3 -p "$PORT" -b "$BAUD" erase_flash
  fi
  log "Uploading firmware to $PORT"
  "$ARDUINO_CLI" upload -p "$PORT" --fqbn "$FQBN" --input-dir "$BUILD_PATH" "$TEMP_SKETCH_DIR"
  log "Writing WakeNet model partition at $MODEL_OFFSET"
  "${ESPTOOL_CMD[@]}" --chip esp32s3 -p "$PORT" -b "$BAUD" write-flash "$MODEL_OFFSET" "$BUILD_PATH/srmodels.bin"
}

open_monitor() {
  [ "$MONITOR" -eq 1 ] || return 0
  if resolve_arduino_cli; then
    log "Opening Arduino serial monitor at 115200 baud"
    "$ARDUINO_CLI" monitor -p "$PORT" -c baudrate=115200
    return
  fi
  if command -v screen >/dev/null 2>&1; then
    log "Opening screen monitor at 115200 baud. Press Ctrl-A then K to quit."
    screen "$PORT" 115200
    return
  fi
  log "No monitor tool found. Open $PORT at 115200 baud in your serial terminal."
}

if [ "$LIST_PORTS" -eq 1 ]; then
  print_ports
  exit 0
fi

if [ -n "$PREBUILT_DIR" ]; then
  [ -d "$PREBUILT_DIR" ] || die "prebuilt directory does not exist: $PREBUILT_DIR"
  if [ "$BUILD_ONLY" -eq 1 ]; then
    die "--build-only cannot be combined with --prebuilt"
  fi
  flash_prebuilt "$PREBUILT_DIR"
  open_monitor
  exit 0
fi

build_from_source
package_build
if [ "$BUILD_ONLY" -eq 1 ]; then
  log "Build-only requested; skipping flash."
  exit 0
fi
flash_from_build
open_monitor
