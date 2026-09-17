# Release notes

## v0.1.0

- Targets Arduino-ESP32 3.3.11 and the generic classic `ESP32 Dev Module` board profile.
- Includes an idle mode for the USB-powered DevKit power baseline.
- Implements the 30-minute, 96,000-byte/s SD write-only qualification test.
- Implements a 48 kHz, 32-bit I2S transport capture-and-discard test for the INMP441.
- Implements queued 48 kHz, 16-bit mono raw PCM recording.
- Implements segmented PCM WAV recording with clean header finalization.
- Reports sample, drop, queue, SD latency, short-read, and short-write metrics.
- Keeps Wi-Fi and Bluetooth uninitialized.
