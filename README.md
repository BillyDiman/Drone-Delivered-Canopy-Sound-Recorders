# Continuous Audio Logger

ESP32 + I2S microphone + SD card. Records continuous mono 16-bit 48kHz PCM audio to the SD card, rotating into a new file every 30 minutes. No Wi-Fi or Bluetooth involved.

## Parts

- ESP32 DOIT DevKit V1 (classic ESP32)
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

All grounds must be tied together, and the mic must run on 3.3V, never 5V. Unplug USB before changing any wiring.

## Flash it

1. In Arduino IDE, install board package **esp32 by Espressif Systems**, version **3.3.11 or newer** (Boards Manager). Verified on 3.3.11 and 3.3.12.
2. **Tools > Board > esp32 >** select **DOIT ESP32 DEVKIT V1** (or **ESP32 Dev Module**).
3. **Tools > Port** — select your board.
4. Open `continuous_audio_logger.ino`. Confirm `config.h` appears as its own tab in the same window — it has to be in the same folder as the `.ino` or the sketch won't compile.
5. Click **Upload**.

Selecting a different chip family fails to compile with a clear error rather than silently using the wrong pins.

## Check that it's recording

1. **Tools > Serial Monitor**, 115200 baud.
2. Within a couple seconds you should see `Opened /REC00001.PCM`.
3. Let it run a bit, then just unplug it — no shutdown sequence needed.
4. Put the SD card in your computer. Import `REC00001.PCM` into Audacity (or ffmpeg) as raw audio: **signed 16-bit PCM, little-endian, mono, 48000 Hz**.

That's the whole loop. It starts recording the moment it powers up and keeps going, rotating to `REC00002.PCM`, `REC00003.PCM`, etc. every 30 minutes, until you pull the power.

## If it doesn't work

- **Wrong board selected** — make sure **Tools > Board** is a classic ESP32 target. Anything else fails to build with an explicit message.
- **`SD.begin() failed` in Serial Monitor** — check the SD wiring for loose contacts, and try a different USB cable/port. A card's power-up spike can brown out the board on a weak USB connection.
- **Compiler says it can't find `setup()`** — `config.h` isn't sitting next to the `.ino` in the same folder.

## Power

`CPU_POWER_PROFILE` in `config.h` picks the CPU clock. Only frequencies the chip actually accepts are listed:

| Profile | Clock | Microphone modes |
| --- | --- | --- |
| `POWER_SUPER_LOW` | 10 MHz | Broken — build refused |
| `POWER_LOW` | 40 MHz | Broken — build refused |
| `POWER_NORMAL` (default) | 80 MHz | Works |
| `POWER_FULL` | 240 MHz | Works |

**`POWER_NORMAL` is the floor for anything that uses the microphone.** This was measured, not predicted: at `POWER_LOW` and `POWER_SUPER_LOW` the I2S reads time out and return nothing, every metrics line reports `samples=0`, and the recording comes out empty with no error raised anywhere. `POWER_FULL` and `POWER_NORMAL` record correctly.

The boundary lines up exactly with the CPU clock source — 80 MHz and up run the CPU from the PLL, 40 MHz and below run it from the crystal, and crossing that line kills capture. The precise mechanism was never pinned down; the behavior is reproducible. `config.h` **fails the build** if you combine a microphone mode with a sub-80 MHz profile, so this cannot silently produce an empty card again.

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

1. **Remove the power LEDs** on both the dev board and the SD breakout.
2. **Feed 3.3 V directly** (bypassing the onboard LDO and USB-serial chip) from an efficient buck converter rather than 5 V USB.
3. In Arduino IDE, **Tools > Flash Frequency > 40 MHz** trims a little more.

Deeper tricks are not available here: automatic light sleep is fundamentally incompatible with continuous I2S capture (the DMA stops), and dynamic frequency scaling (`esp_pm_configure`) is compiled out of the stock Arduino ESP32 libraries — `CONFIG_PM_ENABLE` is not set for either chip, so using it would mean leaving the Arduino IDE for a custom ESP-IDF build. Wi-Fi and Bluetooth are never initialized.

## Card capacity

At 48 kHz 16-bit mono the logger writes **96 kB/s = 8.3 GB/day**. Each 30-minute file is 172.8 MB.

| Card | Recording time | Files |
| --- | --- | --- |
| 16 GB | ~1.9 days | ~92 |
| 128 GB | ~15.3 days | ~734 |
| 256 GB | ~30.7 days | ~1,475 |

**Two weeks needs ~116 GB**, so a 16 GB card is nowhere near enough — use 128 GB or larger. Cards over 32 GB must be formatted FAT32 (macOS Disk Utility does this; Windows needs a third-party tool). exFAT is not supported.

The filename ceiling is `REC99999`, which at 30-minute files is years of recording — far past the point where any card fills, so it is no longer a practical limit.

## Safe ejection

Pulling the card while a file is open is an unclean unmount and can damage the filesystem over time. To remove a card safely, trigger an eject first: send `e` in the Serial Monitor, or wire a button to ground on a GPIO and set `EJECT_BUTTON_PIN` in `config.h`. Either closes the file, unmounts the card, and parks in the paused state. Reinsert and it resumes into a new file on its own.

Pulling without ejecting still works — it is handled as a storage fault — but each unclean pull carries some risk of filesystem damage.

## Running unattended

It records from power-up until power is cut. Files rotate every 30 minutes, so **a 30-minute file is a normal rotation and anything shorter means the recording was interrupted.**

Pulling the SD card pauses recording rather than ending the run: the board stays awake, discards audio while it has nowhere to put it, and resumes into a new file as soon as a working card is back. Swapping in a different card works too, including swapping out a full one. Controlled by `PAUSE_WHEN_STORAGE_ABSENT` in `config.h`.

Audio recorded while the card is out is lost — there is no buffering beyond the ~680 ms queue — but the run continues.

### EVENTS.CSV

Each run appends to `/EVENTS.CSV` on the card (`millis_since_boot,event,detail`), so an unattended run explains itself:

| Event | Meaning |
| --- | --- |
| `BOOT` | Board started. The detail is the reset reason — `power-on` is a normal start, `brownout` means the supply sagged, `panic`/`task-watchdog` mean a crash. |
| `OPEN` / `CLOSE` | A recording file was opened or closed. |
| `RECOVER` | A write failed and recovery ran; the current file was cut short. |
| `STORAGE_LOST` | Card removed, failed, or full. Recording paused here. |
| `STORAGE_BACK` | A working card was detected and recording resumed. |

To explain a short file, find its `CLOSE` and look at what follows. A `BOOT` right after means the board reset (check the reason); a `RECOVER` or `STORAGE_LOST` means a storage fault.

## Reliability

Storage faults no longer end a recording. A short or failed SD write is retried (`SD_WRITE_RETRY_LIMIT`), then escalates to reopening the file, then to remounting the card (`SD_RECOVERY_LIMIT`), and only stops if recovery itself keeps failing. A genuinely full card stops cleanly with a clear message instead of retrying forever. Retries and recoveries are counted in the metrics line as `sd-retry` and `sd-recover` — both should stay at zero on healthy hardware.

### Self-recovery

Two mechanisms keep an unattended run from dying silently:

- **Watchdog** (`WATCHDOG_ENABLED`, 60 s). If the capture or writer task stops making progress — a deadlock in the SD library, say — the board reboots and starts a new file. Costs one file instead of the rest of the deployment. The timeout is far longer than any legitimate SD stall.
- **Restart on fatal error** (`RESTART_ON_FATAL_ERROR`). Errors that are not storage faults — I2S failing to initialize, task creation failing — reboot after 10 s rather than leaving the board sitting dead. Each restart is recorded in `EVENTS.CSV` with its reset reason, so a boot loop is visible rather than invisible.

Still true, and worth knowing before an unattended deployment:

- **No timestamps.** Files are numbered, not dated; there is no RTC and no network time. You cannot tell what time a given recording happened.
- **Dropped audio is silent.** If the card stalls longer than the buffer reserve (~680 ms), samples are dropped and the recording jump-cuts with no marker in the file. The `dropped` counter is the only evidence, and it is not written to the card.

## Other modes

`config.h` has a few other `LOGGER_MODE` options (SD-only write test, mic-only capture test, idle power baseline) meant for isolating a wiring problem to "SD" vs "mic" vs "board" — not needed for normal use. Leave `LOGGER_MODE` on `MODE_RECORD_RAW_PCM` unless you're debugging.
