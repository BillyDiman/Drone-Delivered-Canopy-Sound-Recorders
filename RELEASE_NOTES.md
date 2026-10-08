# Release notes

## v0.2.1

- Measured on hardware: I2S capture returns no samples at all below 80 MHz. `POWER_FULL` and `POWER_NORMAL` record correctly; `POWER_LOW` and `POWER_SUPER_LOW` silently produce empty recordings (every I2S read times out after 1 s). The failure tracks the CPU clock source changing from PLL to crystal; the mechanism was not identified.
- `config.h` now fails the build when a microphone mode is combined with a sub-80 MHz profile, rather than letting it fail silently at runtime. The low profiles still build for `MODE_IDLE_BASELINE` and `MODE_SD_WRITE_TEST`.
- Corrected documentation that previously claimed I2S timing was unaffected by CPU frequency because it is PLL-sourced. The clock source is PLL, but that did not make low profiles safe.

## v0.2.0

- Storage faults are now recoverable instead of fatal. A short or failed SD write retries, then reopens the file, then remounts the card, and only stops if recovery itself keeps failing. New `sd-retry` and `sd-recover` metrics report it. A retry now resends only the bytes that did not land, rather than duplicating audio already written.
- A full SD card is detected and stops the recording cleanly with a clear message, instead of presenting as an unexplained write failure.
- File rotation no longer rescans filenames from `REC0001` every segment; it resumes from the last index, so rotations stay fast as recordings accumulate.
- Audio blocks enlarged from 1024 to 4096 samples (8 KB writes, ~682 ms of queue reserve, up from ~171 ms). Fewer, larger SD transactions cut filesystem overhead and card active time, and leave far more slack for a slow card.
- Corrected the `POWER_SUPER_LOW` value for the ESP32-C3: it is 20 MHz, not 10 MHz. Only the classic ESP32 supports crystal/4.
- SD SPI clock is clamped at runtime to what APB can produce (APB follows the CPU clock below 80 MHz on both chips) and reported at startup.
- Guarded against a livelock where a mic returning no samples would spin at the highest task priority and starve the writer on single-core chips.
- Added `SERIAL_DIAGNOSTICS` to silence the recurring metrics line for deployment.
- Added Seeed XIAO ESP32-C3 support alongside the classic ESP32. Pin mapping is now per-board in `config.h`, selected automatically at compile time; an unmapped board fails the build instead of silently using wrong pins.
- Fixed a crash-on-start on single-core boards (ESP32-C3): the capture/writer tasks were hard-pinned to cores 0 and 1, which doesn't exist on a single-core chip. Task core assignment is now conditional on core count.
- Added a `CPU_POWER_PROFILE` setting in `config.h` (`POWER_SUPER_LOW`/`POWER_LOW`/`POWER_NORMAL`/`POWER_FULL`, default `POWER_NORMAL` = 80MHz) that resolves to the right MHz value per board; `setup()` now warns over Serial if the chip rejects the requested frequency.

## v0.1.0

- Targets Arduino-ESP32 3.3.11 and the generic classic `ESP32 Dev Module` board profile.
- Includes an idle mode for the USB-powered DevKit power baseline.
- Implements the 30-minute, 96,000-byte/s SD write-only qualification test.
- Implements a 48 kHz, 32-bit I2S transport capture-and-discard test for the INMP441.
- Implements queued 48 kHz, 16-bit mono raw PCM recording.
- Implements segmented PCM WAV recording with clean header finalization.
- Reports sample, drop, queue, SD latency, short-read, and short-write metrics.
- Keeps Wi-Fi and Bluetooth uninitialized.
