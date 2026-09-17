# Continuous Audio Logger

ESP32 + I2S microphone + SD card. Records continuous mono 16-bit 48kHz PCM audio to the SD card, rotating into a new file every 30 minutes. No Wi-Fi or Bluetooth involved.

## Parts

- ESP32 DOIT DevKit V1 (classic ESP32 dev board)
- INMP441 I2S microphone breakout
- SPI microSD breakout (the kind with an onboard 3.3V regulator + logic-level shifter)
- Breadboard + jumper wires
- microSD card, formatted FAT32
- USB cable (powers everything, for now)

## Wire it up

**SD breakout → ESP32**

| SD | ESP32 |
| --- | --- |
| `VCC` | `5V` |
| `GND` | `GND` |
| `MOSI` | `GPIO23` |
| `MISO` | `GPIO19` |
| `SCK` | `GPIO18` |
| `CS` | `GPIO27` |

**INMP441 → ESP32**

| INMP441 | ESP32 |
| --- | --- |
| `VDD` | `3V3` |
| `GND` | `GND` |
| `SCK` | `GPIO26` |
| `WS` | `GPIO25` |
| `SD` | `GPIO33` |
| `L/R` | `GND` |

All grounds must be tied together. The mic must run on 3.3V, never 5V. Unplug USB before changing any wiring.

## Flash it

1. In Arduino IDE, install board package **esp32 by Espressif Systems**, version **3.3.11** (Boards Manager).
2. **Tools > Board > esp32 > DOIT ESP32 DEVKIT V1**.
3. **Tools > Port** — select your board.
4. Open `continuous_audio_logger.ino`. Confirm `config.h` appears as its own tab in the same window — it has to be in the same folder as the `.ino` or the sketch won't compile.
5. Click **Upload**.

## Check that it's recording

1. **Tools > Serial Monitor**, 115200 baud.
2. Within a couple seconds you should see `Opened /REC0001.PCM`.
3. Let it run a bit, then just unplug it — no shutdown sequence needed.
4. Put the SD card in your computer. Import `REC0001.PCM` into Audacity (or ffmpeg) as raw audio: **signed 16-bit PCM, little-endian, mono, 48000 Hz**.

That's the whole loop. It starts recording the moment it powers up and keeps going, rotating to `REC0002.PCM`, `REC0003.PCM`, etc. every 30 minutes, until you pull the power.

## If it doesn't work

- **Wrong-looking board name during upload** — make sure `ESP32-C3` isn't selected instead of the DOIT DevKit V1.
- **`SD.begin() failed` in Serial Monitor** — check the SD wiring for loose contacts, and try a different USB cable/port. A card's power-up spike can brown out the board on a weak USB connection.
- **Compiler says it can't find `setup()`** — `config.h` isn't sitting next to the `.ino` in the same folder.

## Other modes

`config.h` has a few other `LOGGER_MODE` options (SD-only write test, mic-only capture test, idle power baseline) meant for isolating a wiring problem to "SD" vs "mic" vs "board" — not needed for normal use. Leave `LOGGER_MODE` on `MODE_RECORD_RAW_PCM` unless you're debugging.
