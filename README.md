# Continuous Audio Logger

ESP32 + I2S microphone + SD card. Records continuous mono 16-bit 48kHz PCM audio to the SD card, rotating into a new file every 30 minutes. No Wi-Fi or Bluetooth involved.

Supports two boards: the classic **ESP32 DOIT DevKit V1** and the **Seeed XIAO ESP32-C3**. Pin numbers differ between them — everything below is split out per board where it matters.

## Parts

- ESP32 DOIT DevKit V1 **or** Seeed XIAO ESP32-C3
- INMP441 I2S microphone breakout
- SPI microSD breakout (the kind with an onboard 3.3V regulator + logic-level shifter)
- Breadboard + jumper wires
- microSD card, formatted FAT32
- USB cable (powers everything, for now)

## Wire it up

### DOIT ESP32 DEVKIT V1

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

### Seeed XIAO ESP32-C3

**SD breakout → XIAO**

| SD | XIAO pin | GPIO |
| --- | --- | --- |
| `VCC` | `5V` | — |
| `GND` | `GND` | — |
| `MOSI` | `D10` | GPIO10 |
| `MISO` | `D9` | GPIO9 |
| `SCK` | `D8` | GPIO8 |
| `CS` | `D3` | GPIO5 |

**INMP441 → XIAO**

| INMP441 | XIAO pin | GPIO |
| --- | --- | --- |
| `VDD` | `3V3` | — |
| `GND` | `GND` | — |
| `SCK` | `D1` | GPIO3 |
| `WS` | `D4` | GPIO6 |
| `SD` | `D5` | GPIO7 |
| `L/R` | `GND` | — |

Note: GPIO8/GPIO9 (`D8`/`D9`) are boot-strapping pins on the C3. Using them for SD `SCK`/`MISO` is the board's documented standard wiring and normally fine, but if the board ever fails to boot or flash after wiring the SD card, disconnect the SD card and retry — that pinpoints whether the card is interfering with boot strapping.

Both boards: all grounds must be tied together, and the mic must run on 3.3V, never 5V. Unplug USB before changing any wiring.

## Flash it

1. In Arduino IDE, install board package **esp32 by Espressif Systems**, version **3.3.11** (Boards Manager).
2. **Tools > Board > esp32 >** select **DOIT ESP32 DEVKIT V1** or **XIAO_ESP32C3**, matching your board.
3. **Tools > Port** — select your board.
4. Open `continuous_audio_logger.ino`. Confirm `config.h` appears as its own tab in the same window — it has to be in the same folder as the `.ino` or the sketch won't compile.
5. Click **Upload**.

`config.h` picks the right pin numbers automatically based on which board you selected in step 2 — no manual edits needed. Selecting a third board that isn't listed above will fail to compile with a clear error instead of silently using the wrong pins.

## Check that it's recording

1. **Tools > Serial Monitor**, 115200 baud.
2. Within a couple seconds you should see `Opened /REC0001.PCM`.
3. Let it run a bit, then just unplug it — no shutdown sequence needed.
4. Put the SD card in your computer. Import `REC0001.PCM` into Audacity (or ffmpeg) as raw audio: **signed 16-bit PCM, little-endian, mono, 48000 Hz**.

That's the whole loop. It starts recording the moment it powers up and keeps going, rotating to `REC0002.PCM`, `REC0003.PCM`, etc. every 30 minutes, until you pull the power.

## If it doesn't work

- **Wrong board selected** — make sure **Tools > Board** actually matches your physical board (DOIT DevKit V1 vs XIAO_ESP32C3). The wrong selection compiles fine but wires up the wrong GPIOs.
- **`SD.begin() failed` in Serial Monitor** — check the SD wiring for loose contacts, and try a different USB cable/port. A card's power-up spike can brown out the board on a weak USB connection.
- **Compiler says it can't find `setup()`** — `config.h` isn't sitting next to the `.ino` in the same folder.

## Power

`CPU_POWER_PROFILE` in `config.h` picks the CPU clock, resolved per board. Only frequencies the chip actually accepts are listed — both boards run a 40 MHz crystal and support crystal/1 and crystal/2, but only the classic ESP32 also supports crystal/4, so the C3 bottoms out at 20 MHz:

| Profile | Classic ESP32 | XIAO ESP32-C3 | Microphone modes |
| --- | --- | --- | --- |
| `POWER_SUPER_LOW` | 10 MHz | 20 MHz | Broken — build refused |
| `POWER_LOW` | 40 MHz | 40 MHz | Broken — build refused |
| `POWER_NORMAL` (default) | 80 MHz | 80 MHz | Works |
| `POWER_FULL` | 240 MHz | 160 MHz | Works |

**`POWER_NORMAL` is the floor for anything that uses the microphone.** This was measured, not predicted: at `POWER_LOW` and `POWER_SUPER_LOW` the I2S reads time out and return nothing, every metrics line reports `samples=0`, and the recording comes out empty with no error raised anywhere. `POWER_FULL` and `POWER_NORMAL` record correctly.

The boundary lines up exactly with the CPU clock source — 80 MHz and up run the CPU from the PLL, 40 MHz and below run it from the crystal, and crossing that line kills capture. The precise mechanism was never pinned down; the behavior is reproducible. `config.h` now **fails the build** if you combine a microphone mode with a sub-80 MHz profile, so this cannot silently produce an empty card again.

The lower profiles still build and run for `MODE_IDLE_BASELINE` and `MODE_SD_WRITE_TEST`, which never touch the microphone — useful for measuring a power floor.

One real effect of lowering the clock: the SD SPI clock is derived from APB, and APB follows the CPU clock below 80 MHz, so the requested SPI rate gets clamped. The firmware does that automatically and reports it at startup. Audio only needs 96 kB/s, so the clamp costs nothing.

Other firmware-side power settings: set `SERIAL_DIAGNOSTICS = false` for a deployed logger (stops the recurring metrics line), and note that larger `AUDIO_SAMPLES_PER_BLOCK` means fewer SD write transactions and less card active time.

### Where the watts actually go

A measured **0.35 W (about 70 mA at 5 V)** for the classic DevKit V1 + SD breakout + mic on a breadboard is normal, and most of it is *not* the ESP32:

| Source | Rough draw | Firmware can fix it? |
| --- | --- | --- |
| Power LEDs (one on the DevKit, usually one on the SD breakout) | 10–20 mA | No — desolder them |
| USB-serial chip (CP2102/CH340) on the DevKit | 10–20 mA | No |
| Two AMS1117 LDOs (quiescent + burning 34% of everything as heat at 5→3.3 V) | ~10 mA + losses | No |
| ESP32 core at 80 MHz, radios off | ~25–30 mA | Yes — `CPU_POWER_PROFILE` |
| SD card (bursty, while writing) | ~10–20 mA avg | Partly — larger writes |

So firmware alone gets this board to roughly 0.20–0.22 W at best. To go meaningfully below that:

1. **Use the XIAO ESP32-C3 instead.** It has no separate USB-serial chip, a more efficient regulator, and a lower-power core. This is the single biggest lever and you already have one.
2. **Remove the power LEDs** on both the dev board and the SD breakout.
3. **Feed 3.3 V directly** (bypassing the onboard LDO and USB-serial chip) from an efficient buck converter rather than 5 V USB.
4. In Arduino IDE, **Tools > Flash Frequency > 40 MHz** trims a little more.

Deeper tricks are not available here: automatic light sleep is fundamentally incompatible with continuous I2S capture (the DMA stops), and dynamic frequency scaling (`esp_pm_configure`) is compiled out of the stock Arduino ESP32 libraries — `CONFIG_PM_ENABLE` is not set for either chip, so using it would mean leaving the Arduino IDE for a custom ESP-IDF build. Wi-Fi and Bluetooth are never initialized.

## Reliability

Storage faults no longer end a recording. A short or failed SD write is retried (`SD_WRITE_RETRY_LIMIT`), then escalates to reopening the file, then to remounting the card (`SD_RECOVERY_LIMIT`), and only stops if recovery itself keeps failing. A genuinely full card stops cleanly with a clear message instead of retrying forever. Retries and recoveries are counted in the metrics line as `sd-retry` and `sd-recover` — both should stay at zero on healthy hardware.

Still true, and worth knowing before an unattended deployment:

- **No timestamps.** Files are numbered, not dated; there is no RTC and no network time. You cannot tell what time a given recording happened.
- **Dropped audio is silent.** If the card stalls longer than the buffer reserve (~680 ms), samples are dropped and the recording jump-cuts with no marker in the file. The `dropped` counter is the only evidence, and it is not written to the card.

## Other modes

`config.h` has a few other `LOGGER_MODE` options (SD-only write test, mic-only capture test, idle power baseline) meant for isolating a wiring problem to "SD" vs "mic" vs "board" — not needed for normal use. Leave `LOGGER_MODE` on `MODE_RECORD_RAW_PCM` unless you're debugging.
