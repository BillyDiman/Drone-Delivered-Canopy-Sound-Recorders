# Continuous Audio Logger Prototype

Arduino firmware for the staged ESP32 prototype defined in **Continuous Audio Logger Prototype Framework v0.1**.

The code targets:

- Classic ESP32 / ESP-32U 38-pin development board
- Arduino IDE with `esp32 by Espressif Systems` version 3.3.11
- INMP441 I2S microphone
- SPI microSD module with onboard regulator and VHC125A logic buffer
- 48 kHz, 16-bit, mono stored PCM at 96,000 bytes per second

No third-party Arduino libraries are required.

## Electrical rules

- Select **ESP32 Dev Module**, not ESP32C3 Dev Module.
- Power the verified SD breakout through its `VCC` pin from the ESP32 board's `5V` pin.
- The SD module's SPI signals connect only to 3.3 V ESP32 GPIOs.
- Power the INMP441 from `3V3`, never `5V`.
- Disconnect USB before changing wiring.
- All modules must share ground.

## Wiring

### SPI microSD

| SD module | ESP32 |
| --- | --- |
| `VCC` | `5V` |
| `GND` | `GND` |
| `MOSI` | `GPIO23` |
| `MISO` | `GPIO19` |
| `SCK` | `GPIO18` |
| `CS` | `GPIO27` |

### INMP441

| INMP441 | ESP32 |
| --- | --- |
| `VDD` | `3V3` |
| `GND` | `GND` |
| `SCK` | `GPIO26` |
| `WS` | `GPIO25` |
| `SD` | `GPIO33` |
| `L/R` | `GND` for the default left-slot configuration |

Do not confuse the microphone's `SD` data pin with the microSD storage module.

## Arduino IDE settings

1. Install `esp32 by Espressif Systems` 3.3.11.
2. Select **Tools > Board > esp32 > ESP32 Dev Module**.
3. Select the board's serial port.
4. Leave flash size at the normal 4 MB default unless the upload log identifies something different.
5. Open `continuous_audio_logger.ino` from this folder.
6. Open Serial Monitor at 115200 baud after uploading.

The firmware prints the detected chip model, revision, core count, CPU speed, and flash size at startup.

## Running the stages

Select a stage by changing `LOGGER_MODE` near the top of `config.h`, then compile and upload.

### Power baseline: ESP32 idle

Use:

```cpp
constexpr LoggerMode LOGGER_MODE = MODE_IDLE_BASELINE;
```

This initializes neither SD nor I2S. Use it to measure the USB-powered DevKit baseline with the same board and Serial Monitor settings. Wi-Fi and Bluetooth remain uninitialized.

### Stage 1 SD write-only test

Use:

```cpp
constexpr LoggerMode LOGGER_MODE = MODE_SD_WRITE_TEST;
```

The default run is 30 minutes. It writes generated data at the target 96,000-byte/s audio rate in large blocks, reports maximum SD-write latency and short writes, flushes periodically, and closes the file cleanly.

Expected 30-minute file size:

```text
172,800,000 bytes
```

Pass only if initialization succeeds, the firmware reports no short writes, and macOS reports the exact expected size.

### Stage 2 I2S capture-and-discard test

Use:

```cpp
constexpr LoggerMode LOGGER_MODE = MODE_I2S_DISCARD_TEST;
```

The default run is 15 minutes. The ESP32 receives 32-bit I2S transport words, measures their range, counts samples and short reads, then discards the data.

Pass only if:

- the captured-sample count closely tracks 48,000 samples per elapsed second;
- `i2s-short` remains zero;
- the raw minimum and maximum change with real sound;
- the stream is not entirely zero or fixed.

If data is absent, verify the `L/R` connection matches `MIC_USE_LEFT_SLOT` in `config.h`.

### Stage 3 combined raw recording

Use:

```cpp
constexpr LoggerMode LOGGER_MODE = MODE_RECORD_RAW_PCM;
```

The capture task reads I2S/DMA blocks and converts them to 16-bit PCM. A separate writer task drains a bounded eight-block queue to the SD card. The default run is 30 minutes and creates files named `REC0001.PCM`, `REC0002.PCM`, and so on without overwriting existing files.

Import a `.PCM` file into an audio editor as:

- signed 16-bit PCM;
- little-endian;
- mono;
- 48,000 Hz.

The expected 30-minute payload is 172,800,000 bytes.

### Stage 4 WAV recording

Use:

```cpp
constexpr LoggerMode LOGGER_MODE = MODE_RECORD_WAV;
```

This uses the same real-time path but adds and finalizes a standard 44-byte PCM WAV header. The expected 30-minute file size is 172,800,044 bytes.

The header is finalized only during a clean close. Unexpected power loss can leave the active WAV header with a zero or stale length; earlier closed segments remain usable.

## Buffering and file segmentation

- I2S is read in blocks of 1,024 transport samples.
- Each stored audio block is 2,048 bytes, about 21.3 ms of audio.
- Eight PCM blocks provide about 171 ms of queue reserve.
- Recording and writing run in separate FreeRTOS tasks.
- Files rotate every 30 minutes by default.
- A value of `0` for a test duration runs until reset or power removal.

For long tests, keep file segmentation enabled. Do not create a single multi-day WAV file: standard RIFF/WAV uses 32-bit length fields.

## Metrics

Every five seconds the firmware reports:

- captured and expected samples;
- queued and dropped samples;
- stored and expected bytes;
- maximum observed SD-write time;
- queue high-water mark;
- raw I2S sample range;
- zero and clipped sample counts;
- I2S short reads and SD short writes.

Any nonzero dropped-sample or short-write count fails a qualification recording.

## Troubleshooting

- **Upload identifies ESP32-C3:** the wrong Arduino board target is selected. Choose `ESP32 Dev Module`.
- **SD initialization fails:** disconnect power and recheck `VCC`, common ground, card seating, pin order, and FAT32 formatting. Never solve detection by applying 5 V to a GPIO.
- **Unstable SD writes:** shorten jumpers, secure contacts, and reduce `SD_SPI_FREQUENCY_HZ` from 10 MHz to 4 MHz.
- **No microphone data:** verify `VDD=3V3`, common ground, pin labels, and the `L/R` slot selection.
- **Audio is extremely quiet or clipped:** do not guess immediately. Run Stage 2, record the raw range, and then adjust `MIC_TO_PCM_RIGHT_SHIFT` by one or two bits if evidence supports it.
- **Dropped blocks:** inspect `max-write` and `queue-high`. Short SD stalls should fit within the queue; persistent stalls or wiring errors must be fixed before increasing buffer memory.

## Power measurements

Run the same firmware build and test duration while measuring at the ESP32 USB input. Serial diagnostics are intentionally rate-limited, but the DevKit's USB interface, regulator, and LEDs remain part of the measurement.

The code never starts Wi-Fi or Bluetooth.
