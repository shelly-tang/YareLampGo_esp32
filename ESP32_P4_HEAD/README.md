# LampGo ESP32-P4 head runtime

This sketch moves all time-critical head I/O to the P4. The computer runs the
LampGo backend and talks to one paired device over the P4/C6 network link; it no
longer opens the servo bus over USB.

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

The custom partition table keeps two 5 MiB OTA slots and a LittleFS asset
partition. This P4 image deliberately does not bundle a WakeNet model
partition; full-duplex microphone/speaker streaming and ESP-SR AEC are
independent of wake-word detection.

## Network and ports

- HTTP device API and `/capture`: port `80`
- microphone/speaker WebSockets: port `81`
- motion WebSocket (`lampgo-motion-v1`): port `82`
- setup AP: `Lampgo-P4-Setup-XXXX`, password `lampgo-p4-setup`
- mDNS service: `_lampgo-cam._tcp`

Pairing is shared by HTTP, audio, and motion. Pairing secrets are stored as a
SHA-256 digest, and asset or motion changes require the paired owner.

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
