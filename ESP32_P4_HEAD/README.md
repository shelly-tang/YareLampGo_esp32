# LampGo ESP32-P4 head runtime

This sketch moves all time-critical head I/O to the P4. The computer runs the
LampGo backend and talks to one paired device over the P4/C6 network link; it no
longer opens the servo bus over USB.

Recent P4-specific runtime changes are summarized in
[the Chinese update log](CHANGELOG.zh-CN.md).

## This is a parallel target, not an S3/C6 replacement flash

Use this directory only for the **ESP32-P4 head-board + C6 Wi-Fi** hardware.
It coexists with the legacy `XIAO_ESP32S3` root sketch and
`ESP32_C6_LCD_1_47_UART/` display sketch:

| Route | C6 role | Asset route | Backend motor setting |
| --- | --- | --- | --- |
| Legacy S3 + C6 display | LCD controller over UART | backend → S3 → C6 | `serial` (default) |
| This P4 head board | P4 network coprocessor over ESP-Hosted/SDIO | backend → P4 LittleFS → P4 LCD/LED | `p4` (explicit opt-in) |

Do not flash `ESP32_C6_LCD_1_47_UART` onto the C6 used by this board, do not
run the root `scripts/flash.sh` for this target, and do not enter P4's native
USB debug port as the legacy Feetech motor-bus port. The backend's configuration
and rollback instructions are in the main repository's P4 wireless-head guide.

```text
backend -- HTTP/WS over Wi-Fi --> ESP32-C6 -- internal SDIO --> ESP32-P4
                                                              |-- ST3215 x5
                                                              |-- 54x9 LED
                                                              |-- 320x172 LCD
                                                              |-- OV5640
                                                              `-- PDM mic + I2S speaker + AEC
```

The internal C6 link owns GPIO14..19 and reset GPIO54. Application code must
never configure those pins.

## Build

Required versions:

- Arduino-ESP32 `3.3.11`
- ArduinoJson `7.4.2` or compatible 7.x
- WebSockets `2.6.1`
- SCServo `1.0.2`

Build from the repository root:

```bash
arduino-cli compile \
  --fqbn 'esp32:esp32:esp32p4:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=custom,PSRAM=enabled,ChipVariant=prev3' \
  ESP32_P4_HEAD
```

To upload through the P4's native USB CDC port after a successful build, use
the same FQBN and the port reported by your system (for example
`/dev/cu.usbmodem101` on macOS):

```bash
arduino-cli upload \
  --port /dev/cu.usbmodem101 \
  --fqbn 'esp32:esp32:esp32p4:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=custom,PSRAM=enabled,ChipVariant=prev3' \
  ESP32_P4_HEAD
```

Do not erase a working board as part of an ordinary update: Wi-Fi and pairing
state live in NVS. A first install or intentional recovery may require a clean
flash, followed by provisioning and a new pairing.

The custom partition table keeps two 5 MiB OTA slots and a LittleFS asset
partition. This P4 image deliberately does not bundle a WakeNet model
partition; full-duplex microphone/speaker streaming and ESP-SR AEC are
independent of wake-word detection.

On the first boot after installing this partition table, an uninitialized or
invalid LittleFS asset partition is formatted automatically. Wi-Fi and pairing
state live in NVS and are not part of that format operation.

## Network and ports

- HTTP device API and `/capture`: port `80`
- microphone/speaker WebSockets: port `81`
- motion WebSocket (`lampgo-motion-v1`): port `82`
- setup AP: `Lampgo-P4-Setup-XXXX`, password `lampgo-p4-setup`
- mDNS service: `_lampgo-cam._tcp`

Pairing is shared by HTTP, audio, and motion. Pairing secrets are stored as a
SHA-256 digest. After provisioning, normal P4 HTTP, asset, audio, speaker, and
motion connections use a short-lived device nonce plus an HMAC proof; the
reusable pairing secret is never sent over those LAN transports. This requires
the matching P4 backend branch and firmware image to be upgraded together.

The existing LampGo setup wizard remains the provisioning entry point. While
the computer is joined to the setup AP it uses `GET /status`, `GET /scan`, and
`POST /connect`; the final request stores Wi-Fi credentials and pairs the P4 to
that backend in one operation. After the board rejoins the normal LAN, device
health/configuration use `/device/status` and `/device/config`. Reboot and the
atomic unpair-plus-forget operation require owner authentication.

Long-lived audio and motion sessions carry the pairing revision from their
handshake. Pairing changes revoke those sessions; queued speaker audio is also
discarded instead of being played for a new owner.

## Safety boundary

Boot turns off torque for servo IDs 1..5 and keeps the LED/LCD dark. A motion
client must send a complete five-joint calibration profile before torque can be
enabled. The P4 seeds every goal with the measured position, clamps every frame
to the profile, drops superseded frames, and releases torque after a prolonged
command timeout.

Do not flash or enable torque solely because the sketch compiles. Before the
first physical run, independently confirm all five servo IDs, single-turn mode,
joint directions and limits, 12 V polarity/current limiting, mechanical
clearance, LCD transform, LED origin/serpentine direction, microphone polarity,
speaker volume, and AEC reference alignment. Start with the arm unloaded and a
hand on power removal.
