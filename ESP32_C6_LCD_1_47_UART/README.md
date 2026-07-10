# LampGo C6 Eye Display

This firmware renders LCD eye clips received from the S3 over UART. The C6
does not render LED-board mouths or symbols.

## Product partition

`partitions.csv` targets the 8MB ESP32-C6FH8 board and provides a 5.375MiB
LittleFS partition. The runtime enables the ten-eye / 3MiB installed budget
only when it detects this partition; older layouts stay limited to five eyes
and 896KiB.

Compile with the 8MB flash option:

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32c6:FlashSize=8M \
  ESP32_C6_LCD_1_47_UART
```

The first migration to this partition layout requires an erase and reflash,
which removes previously cached eye clips. Sync the clips again from the
LampGo backend after flashing.

## Storage guarantees

- Single eye package: 256KiB maximum.
- Uploads are written to `.tmp`, checked for size and SHA256, then committed.
- A failed upload keeps the previous installed clip.
- Space exhaustion rejects the new clip; no existing clip is evicted.
- `once` playback returns to the default eye after one pass; `loop` repeats.
