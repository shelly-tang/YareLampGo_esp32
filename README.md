# LampGo ESP32 Firmware

Firmware for the LampGo ESP32 camera, microphone, speaker, LED, and WiFi provisioning board.

中文烧录指南：[`README.zh-CN.md`](README.zh-CN.md)

The target board is `XIAO_ESP32S3` with OPI PSRAM enabled.

## License

This firmware repository is distributed under GPL-3.0-only unless a file says
otherwise. See [LICENSE](LICENSE), [AUTHORS.md](AUTHORS.md),
[COPYRIGHT](COPYRIGHT), and [NOTICE](NOTICE).

Some camera server support code is based on Espressif ESP32 examples and keeps
its upstream Apache-2.0 notices where present. When distributing prebuilt
firmware binaries, publish the corresponding source, build scripts, partition
table, and flashing instructions.

## Quick Start

Connect the ESP32 over USB, then list ports:

```bash
cd YareLampGo_esp32
./scripts/flash.sh --list-ports
```

For a clean first flash or recovery flash:

```bash
./scripts/flash.sh --port /dev/cu.usbmodem2101 --erase --monitor
```

`--erase` clears old WiFi credentials and pairing state. Omit it for a normal firmware update where you want to keep the existing WiFi binding.

After a successful clean flash, the serial log should show:

```text
No WiFi credentials stored. Entering provisioning mode.
[provision] WiFi not configured, SoftAP+STA mode
[provision] SSID: Lampgo-Setup-XXXX
```

## If You Have Arduino

Install either:

- Arduino IDE, then install the `esp32` board package in Boards Manager.
- Or standalone `arduino-cli` with the `esp32:esp32` core installed.

The script auto-detects:

- `arduino-cli` on `PATH`
- macOS Arduino IDE's bundled CLI
- Arduino ESP32 core's bundled `esptool`

Build and flash from source:

```bash
./scripts/flash.sh --erase --monitor
```

Choose the port explicitly if more than one serial device is connected:

```bash
./scripts/flash.sh --port /dev/cu.usbmodem2101 --erase --monitor
```

Build only:

```bash
./scripts/flash.sh --build-only
```

## If You Do Not Have Arduino

Use a prebuilt firmware package from a LampGo release. It should contain:

- `ESP32_CAMERA.ino.bootloader.bin`
- `ESP32_CAMERA.ino.partitions.bin`
- `boot_app0.bin`
- `ESP32_CAMERA.ino.bin`
- `srmodels.bin`

Install `esptool`:

```bash
python3 -m pip install --user esptool
```

Then flash the downloaded package:

```bash
cd lampgo-esp32-firmware
./flash.sh --prebuilt . --port /dev/cu.usbmodem2101 --erase --monitor
```

This path does not require Arduino IDE or `arduino-cli`.

## Maintainer Release Package

To create a prebuilt package for users who do not have Arduino:

```bash
./scripts/flash.sh --build-only --package ./dist/lampgo-esp32-firmware
```

Upload the generated directory or zip it for a GitHub release. The package includes a copy of `flash.sh`, so users without Arduino can flash from inside the unzipped package.

## Wake Word Model

The firmware advertises and accepts a single ESP-SR WakeNet model for LampGo hot switching:

- `Hi,小星` → `wn9_hixiaoxing_tts`

When LampGo saves a wake word, it pushes the model to `/device/config` with `audio_profile=wake_only` (or `aec_experiment` for the experimental AEC call mode), so the ESP32 restarts WakeNet without rebooting the board.

## Upload Troubleshooting

If upload cannot connect:

1. Hold the ESP32 `BOOT` button.
2. Tap `RESET`.
3. Release `RESET`.
4. Release `BOOT`.
5. Run the flash command again.

If the serial port is missing, unplug and replug USB, then run:

```bash
./scripts/flash.sh --list-ports
```

On macOS, valid ports usually look like `/dev/cu.usbmodem*` or `/dev/cu.usbserial*`. On Linux, they usually look like `/dev/ttyACM*` or `/dev/ttyUSB*`.

## Provisioning Diagnostics

If the LampGo web UI says WiFi scan or probe failed, connect the computer to `Lampgo-Setup-XXXX` first, then run:

```bash
./scripts/provision_diag.sh
```

The script writes a timestamped log under `logs/`. Switch back to your normal WiFi and share that log when reporting the issue. It records:

- current WiFi/DHCP/route state
- direct ESP32 `http://192.168.4.1/status` and `/scan`
- LampGo backend `/api/device/probe` results

If your LampGo backend is not on the default port:

```bash
./scripts/provision_diag.sh --lampgo-url http://127.0.0.1:8420
```
