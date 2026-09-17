#pragma once

#include <Arduino.h>

// Select exactly one staged test before compiling.
enum LoggerMode : uint8_t {
  MODE_IDLE_BASELINE = 0,
  MODE_SD_WRITE_TEST = 1,
  MODE_I2S_DISCARD_TEST = 2,
  MODE_RECORD_RAW_PCM = 3,
  MODE_RECORD_WAV = 4,
};

constexpr LoggerMode LOGGER_MODE = MODE_RECORD_WAV;
constexpr char FIRMWARE_VERSION[] = "0.1.0";

// Serial diagnostics
constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t METRICS_INTERVAL_MS = 5000;

// Target stored audio format
constexpr uint32_t SAMPLE_RATE_HZ = 48000;
constexpr uint16_t STORED_BITS_PER_SAMPLE = 16;
constexpr uint16_t STORED_CHANNELS = 1;
constexpr uint32_t STORED_BYTE_RATE =
    SAMPLE_RATE_HZ * STORED_CHANNELS * (STORED_BITS_PER_SAMPLE / 8);

// SPI microSD wiring. VCC is a physical power connection, not a GPIO.
constexpr int SD_PIN_SCK = 18;
constexpr int SD_PIN_MISO = 19;
constexpr int SD_PIN_MOSI = 23;
constexpr int SD_PIN_CS = 27;
constexpr uint32_t SD_SPI_FREQUENCY_HZ = 10000000;

// INMP441 wiring. The microphone must use 3.3 V, never 5 V.
constexpr int MIC_PIN_BCLK = 26;
constexpr int MIC_PIN_WS = 25;
constexpr int MIC_PIN_DATA = 33;

// With INMP441 L/R tied to GND, select the left I2S slot.
// Set false only if L/R is instead tied to 3.3 V.
constexpr bool MIC_USE_LEFT_SLOT = true;

// INMP441 transports a 24-bit sample in a 32-bit I2S slot. The default keeps
// the most significant 16 bits. Change only after inspecting Stage 2 metrics.
constexpr uint8_t MIC_TO_PCM_RIGHT_SHIFT = 16;

// Test durations. A value of 0 means run until reset/power removal.
constexpr uint32_t SD_WRITE_TEST_SECONDS = 30 * 60;
constexpr uint32_t I2S_DISCARD_TEST_SECONDS = 15 * 60;
constexpr uint32_t RECORD_TEST_SECONDS = 30;

// Long recordings are segmented so a reset risks only the active file.
constexpr uint32_t FILE_SEGMENT_SECONDS = 30 * 60;
constexpr uint32_t FILE_FLUSH_INTERVAL_MS = 5000;

// Buffers: 1024 mono samples = 2048 stored bytes = about 21.3 ms at 48 kHz.
constexpr size_t AUDIO_SAMPLES_PER_BLOCK = 1024;
constexpr size_t AUDIO_BLOCK_COUNT = 8;
constexpr size_t SD_STRESS_WRITE_BYTES = 16 * 1024;

static_assert(STORED_BITS_PER_SAMPLE == 16, "This prototype stores 16-bit PCM");
static_assert(STORED_CHANNELS == 1, "This prototype stores mono PCM");
static_assert(MIC_TO_PCM_RIGHT_SHIFT <= 31, "Invalid microphone shift");
static_assert(AUDIO_BLOCK_COUNT >= 2, "At least two audio buffers are required");
