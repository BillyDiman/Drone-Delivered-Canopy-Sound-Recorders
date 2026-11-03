#include <Arduino.h>
#include <ESP_I2S.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <inttypes.h>
#include <limits.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>

#include "config.h"

I2SClass AudioI2S;

struct Metrics {
  uint64_t transportBytesRead = 0;
  uint64_t samplesCaptured = 0;
  uint64_t samplesQueued = 0;
  uint64_t samplesDropped = 0;
  uint64_t storedBytesWritten = 0;
  uint64_t zeroSamples = 0;
  uint64_t clippedSamples = 0;
  uint32_t i2sShortReads = 0;
  uint32_t sdShortWrites = 0;
  uint32_t sdWriteFailures = 0;
  uint32_t sdWriteRetries = 0;
  uint32_t sdRecoveries = 0;
  uint32_t maxSdWriteUs = 0;
  uint32_t queueHighWater = 0;
  int32_t rawMinimum = INT32_MAX;
  int32_t rawMaximum = INT32_MIN;
};

struct AudioBlock {
  size_t bytes = 0;
  int16_t pcm[AUDIO_SAMPLES_PER_BLOCK];
};

portMUX_TYPE metricsMux = portMUX_INITIALIZER_UNLOCKED;
Metrics metrics;

SPIClass &sdSpi = SPI;
File activeFile;

AudioBlock audioBlocks[AUDIO_BLOCK_COUNT];
int32_t transportBuffer[AUDIO_SAMPLES_PER_BLOCK];
uint8_t sdStressBuffer[SD_STRESS_WRITE_BYTES];

QueueHandle_t freeBlockQueue = nullptr;
QueueHandle_t filledBlockQueue = nullptr;
TaskHandle_t captureTaskHandle = nullptr;
TaskHandle_t writerTaskHandle = nullptr;

volatile bool captureDone = false;
volatile bool writerDone = false;
volatile bool fatalError = false;
bool modeFinished = false;

uint64_t modeStartUs = 0;
uint64_t lastMetricsMs = 0;
uint64_t lastFlushMs = 0;
uint64_t currentSegmentPayloadBytes = 0;
uint32_t generatedBlockSequence = 0;
uint64_t totalPausedMs = 0;
char activePath[32] = {0};

// millis() wraps every 49.7 days, which a long deployment reaches. This is
// microseconds since boot in a 64-bit counter, so it never wraps in any
// practical lifetime.
uint64_t nowMs() {
  return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

void copyMetrics(Metrics &snapshot) {
  portENTER_CRITICAL(&metricsMux);
  snapshot = metrics;
  portEXIT_CRITICAL(&metricsMux);
}

void resetMetrics() {
  portENTER_CRITICAL(&metricsMux);
  metrics = Metrics{};
  portEXIT_CRITICAL(&metricsMux);
}

void updateSignalMetrics(const int32_t *samples, size_t count) {
  int32_t localMin = INT32_MAX;
  int32_t localMax = INT32_MIN;
  uint64_t localZeros = 0;

  for (size_t i = 0; i < count; ++i) {
    const int32_t value = samples[i];
    if (value < localMin) localMin = value;
    if (value > localMax) localMax = value;
    if (value == 0) ++localZeros;
  }

  portENTER_CRITICAL(&metricsMux);
  if (localMin < metrics.rawMinimum) metrics.rawMinimum = localMin;
  if (localMax > metrics.rawMaximum) metrics.rawMaximum = localMax;
  metrics.zeroSamples += localZeros;
  portEXIT_CRITICAL(&metricsMux);
}

uint64_t elapsedUs() {
  return static_cast<uint64_t>(esp_timer_get_time()) - modeStartUs;
}

const char *modeName() {
  switch (LOGGER_MODE) {
    case MODE_IDLE_BASELINE:
      return "idle power baseline";
    case MODE_SD_WRITE_TEST:
      return "SD write-only test";
    case MODE_I2S_DISCARD_TEST:
      return "I2S capture-and-discard test";
    case MODE_RECORD_RAW_PCM:
      return "microphone-to-SD raw PCM recording";
    case MODE_RECORD_WAV:
      return "microphone-to-SD WAV recording";
    default:
      return "invalid mode";
  }
}

void stopWithError(const char *message) {
  fatalError = true;
  Serial.printf("\nFATAL: %s\n", message);
}

// Runs after the peripherals are initialized so their clock configuration is
// already settled when the CPU clock changes.
//
// Note this ordering is hygiene, not a fix for the sub-80 MHz I2S failure:
// that was measured to happen either way, which is why config.h refuses to
// build a microphone mode below POWER_NORMAL.
void applyPowerProfile() {
  if (getCpuFrequencyMhz() == CPU_FREQUENCY_MHZ) return;

  if (!setCpuFrequencyMhz(CPU_FREQUENCY_MHZ)) {
    Serial.printf("WARNING: chip rejected %" PRIu32 " MHz; running at %" PRIu32
                  " MHz instead.\n",
                  CPU_FREQUENCY_MHZ, static_cast<uint32_t>(getCpuFrequencyMhz()));
    return;
  }

  Serial.printf("CPU clock set to %" PRIu32 " MHz (APB %" PRIu32 " Hz)\n",
                static_cast<uint32_t>(getCpuFrequencyMhz()), getApbFrequency());
  if (CPU_POWER_PROFILE < POWER_NORMAL) {
    Serial.println("NOTE: below 80 MHz. Confirm samples/dropped/i2s-short "
                   "look right over a long run before trusting this.");
  }
}

void printBoardIdentity() {
  Serial.println("\nContinuous Audio Logger Prototype");
  Serial.printf("Firmware: %s\n", FIRMWARE_VERSION);
  Serial.printf("Mode: %s\n", modeName());
  Serial.printf("Chip: %s, revision %u, cores %u\n", ESP.getChipModel(),
                static_cast<unsigned>(ESP.getChipRevision()),
                static_cast<unsigned>(ESP.getChipCores()));
  Serial.printf("CPU: %u MHz (APB %u Hz), flash: %u bytes\n",
                static_cast<unsigned>(ESP.getCpuFreqMHz()),
                static_cast<unsigned>(getApbFrequency()),
                static_cast<unsigned>(ESP.getFlashChipSize()));
  Serial.printf("Stored format: %u Hz, %u-bit, mono (%u bytes/s)\n",
                static_cast<unsigned>(SAMPLE_RATE_HZ),
                static_cast<unsigned>(STORED_BITS_PER_SAMPLE),
                static_cast<unsigned>(STORED_BYTE_RATE));
  Serial.printf("Buffers: %u blocks x %u samples = %lu ms of reserve\n",
                static_cast<unsigned>(AUDIO_BLOCK_COUNT),
                static_cast<unsigned>(AUDIO_SAMPLES_PER_BLOCK),
                static_cast<unsigned long>((AUDIO_BLOCK_COUNT *
                                            AUDIO_SAMPLES_PER_BLOCK * 1000ULL) /
                                           SAMPLE_RATE_HZ));
  Serial.println("Wi-Fi and Bluetooth are not initialized by this firmware.");
}

const char *cardTypeName(uint8_t type) {
  switch (type) {
    case CARD_MMC:
      return "MMC";
    case CARD_SD:
      return "SDSC";
    case CARD_SDHC:
      return "SDHC/SDXC";
    case CARD_NONE:
      return "none";
    default:
      return "unknown";
  }
}

// SPI is clocked from APB, and APB follows the CPU clock below 80 MHz, so
// the configured rate is not always achievable. Audio needs 96 kB/s, so a
// clamped clock still has plenty of bandwidth; this just keeps the request
// honest instead of silently landing somewhere unintended.
uint32_t effectiveSdSpiHz() {
  const uint32_t apb = getApbFrequency();
  const uint32_t ceiling = apb / 2;
  return SD_SPI_FREQUENCY_HZ > ceiling ? ceiling : SD_SPI_FREQUENCY_HZ;
}

// Free space is sampled once per mount and then tracked by subtracting what
// we write. Querying the filesystem for free space walks the allocation
// table, which is far too slow to do on every file rotation.
uint64_t freeBytesAtMount = 0;
uint64_t bytesWrittenSinceMount = 0;

uint64_t estimatedFreeBytes() {
  return bytesWrittenSinceMount >= freeBytesAtMount
             ? 0
             : freeBytesAtMount - bytesWrittenSinceMount;
}

bool initializeSd(uint8_t maxAttempts = SD_MOUNT_RETRY_LIMIT, bool quiet = false) {
  pinMode(SD_PIN_CS, OUTPUT);
  digitalWrite(SD_PIN_CS, HIGH);
  sdSpi.begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);

  uint32_t spiHz = effectiveSdSpiHz();
  if (!quiet) {
    Serial.printf("Initializing SD: SCK=%d MISO=%d MOSI=%d CS=%d at %" PRIu32
                  " Hz (APB %" PRIu32 " Hz)\n",
                  SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS, spiHz,
                  getApbFrequency());
    if (spiHz != SD_SPI_FREQUENCY_HZ) {
      Serial.printf("Note: SD SPI clamped from %" PRIu32 " Hz to fit APB.\n",
                    SD_SPI_FREQUENCY_HZ);
    }
  }

  // Mounting is retried, halving the clock each time, because marginal
  // breadboard wiring and the card's power-up surge make a first attempt
  // fail intermittently. Giving up on one attempt turns that into a dead
  // logger that has to be noticed and power-cycled by hand.
  bool mounted = false;
  for (uint8_t attempt = 1; attempt <= maxAttempts; ++attempt) {
    if (SD.begin(SD_PIN_CS, sdSpi, spiHz)) {
      mounted = true;
      if (attempt > 1 && !quiet) {
        Serial.printf("SD mounted on attempt %u at %" PRIu32 " Hz.\n", attempt,
                      spiHz);
      }
      break;
    }
    if (!quiet) {
      Serial.printf("SD.begin() failed at %" PRIu32 " Hz (attempt %u of %u).\n",
                    spiHz, attempt, maxAttempts);
    }
    SD.end();
    delay(SD_RECOVERY_DELAY_MS);
    if (spiHz / 2 >= SD_SPI_MIN_FREQUENCY_HZ) spiHz /= 2;
  }

  if (!mounted) {
    if (!quiet) {
      Serial.println("SD would not mount. Check wiring and power, and verify "
                     "the card still mounts on a computer -- abrupt power loss "
                     "during a write can corrupt the filesystem.");
    }
    return false;
  }

  const uint8_t type = SD.cardType();
  if (type == CARD_NONE) {
    if (!quiet) Serial.println("No SD card detected.");
    SD.end();
    return false;
  }

  const uint64_t total = SD.totalBytes();
  const uint64_t used = SD.usedBytes();
  freeBytesAtMount = total > used ? total - used : 0;
  bytesWrittenSinceMount = 0;

  if (!quiet) {
    Serial.printf("SD type: %s\n", cardTypeName(type));
    Serial.printf("SD capacity: %.2f GB; free: %.2f GB (about %.1f hours)\n",
                  SD.cardSize() / 1000000000.0, freeBytesAtMount / 1000000000.0,
                  freeBytesAtMount / static_cast<double>(STORED_BYTE_RATE) / 3600.0);
  }
  return true;
}

// Resumes scanning where the last successful search finished. Restarting
// from 1 on every segment rotation made each rotation slower as recordings
// accumulated, and that stall competes with real-time audio writes.
uint32_t nextPathIndex = 1;

void buildIndexedPath(const char *prefix, uint32_t index, const char *extension,
                      char *output, size_t outputSize) {
  snprintf(output, outputSize, "/%s%05" PRIu32 "%s", prefix, index, extension);
}

bool indexedPathExists(const char *prefix, uint32_t index, const char *extension) {
  char path[32];
  buildIndexedPath(prefix, index, extension, path, sizeof(path));
  return SD.exists(path);
}

// Finds a free index without walking every file. A card holding a couple of
// thousand recordings made the old linear scan take seconds, and it ran at
// boot and on every card reinsertion. This doubles upward to bracket a free
// slot, then bisects, so a full card costs about two dozen lookups instead
// of thousands. Gaps from deleted files are fine: any unused index works.
uint32_t probeFreeIndex(const char *prefix, const char *extension) {
  if (!indexedPathExists(prefix, 1, extension)) return 1;

  uint32_t lo = 1;
  uint32_t hi = 2;
  while (hi <= RECORDING_INDEX_MAX && indexedPathExists(prefix, hi, extension)) {
    lo = hi;
    if (hi > RECORDING_INDEX_MAX / 2) {
      hi = RECORDING_INDEX_MAX + 1;
      break;
    }
    hi *= 2;
  }
  if (hi > RECORDING_INDEX_MAX) hi = RECORDING_INDEX_MAX + 1;

  // Invariant: lo is taken, hi is free (or just past the ceiling).
  while (hi - lo > 1) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (indexedPathExists(prefix, mid, extension)) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

bool findUnusedPath(const char *prefix, const char *extension,
                    char *output, size_t outputSize) {
  if (nextPathIndex <= 1) nextPathIndex = probeFreeIndex(prefix, extension);

  for (uint32_t index = nextPathIndex; index <= RECORDING_INDEX_MAX; ++index) {
    buildIndexedPath(prefix, index, extension, output, outputSize);
    if (!SD.exists(output)) {
      nextPathIndex = index + 1;
      return true;
    }
  }
  return false;
}

void putLe16(uint8_t *destination, uint16_t value) {
  destination[0] = static_cast<uint8_t>(value & 0xFF);
  destination[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void putLe32(uint8_t *destination, uint32_t value) {
  destination[0] = static_cast<uint8_t>(value & 0xFF);
  destination[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  destination[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  destination[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void buildWavHeader(uint8_t *header, uint32_t payloadBytes) {
  memset(header, 0, 44);
  memcpy(header + 0, "RIFF", 4);
  putLe32(header + 4, 36 + payloadBytes);
  memcpy(header + 8, "WAVE", 4);
  memcpy(header + 12, "fmt ", 4);
  putLe32(header + 16, 16);
  putLe16(header + 20, 1);
  putLe16(header + 22, STORED_CHANNELS);
  putLe32(header + 24, SAMPLE_RATE_HZ);
  putLe32(header + 28, STORED_BYTE_RATE);
  putLe16(header + 32, STORED_CHANNELS * (STORED_BITS_PER_SAMPLE / 8));
  putLe16(header + 34, STORED_BITS_PER_SAMPLE);
  memcpy(header + 36, "data", 4);
  putLe32(header + 40, payloadBytes);
}

// Reports failure without declaring a fatal error, so the writer can retry
// this as part of recovering from a transient card problem.
bool openNextRecordingFile() {
  const bool wav = LOGGER_MODE == MODE_RECORD_WAV;
  if (!findUnusedPath("REC", wav ? ".WAV" : ".PCM", activePath, sizeof(activePath))) {
    Serial.println("No unused recording filename remains.");
    return false;
  }

  activeFile = SD.open(activePath, FILE_WRITE);
  if (!activeFile) {
    Serial.printf("Could not create %s\n", activePath);
    return false;
  }

  currentSegmentPayloadBytes = 0;
  lastFlushMs = nowMs();

  if (wav) {
    uint8_t header[44];
    buildWavHeader(header, 0);
    if (activeFile.write(header, sizeof(header)) != sizeof(header)) {
      activeFile.close();
      Serial.println("Could not write WAV placeholder header.");
      return false;
    }
  }

  Serial.printf("Opened %s\n", activePath);
  logEvent("OPEN", activePath);
  return true;
}

bool closeRecordingFile() {
  if (!activeFile) return true;

  // Header problems leave the audio data intact and are not worth ending a
  // recording over, so they are reported and handed back to the caller to
  // recover from rather than treated as fatal.
  if (LOGGER_MODE == MODE_RECORD_WAV) {
    if (currentSegmentPayloadBytes > UINT32_MAX - 36) {
      Serial.println("WAV segment exceeded the 32-bit RIFF size limit.");
      activeFile.close();
      return false;
    }
    uint8_t header[44];
    buildWavHeader(header, static_cast<uint32_t>(currentSegmentPayloadBytes));
    if (!activeFile.seek(0) || activeFile.write(header, sizeof(header)) != sizeof(header)) {
      Serial.println("Could not finalize WAV header.");
      activeFile.close();
      return false;
    }
  }

  activeFile.flush();
  activeFile.close();
  Serial.printf("Closed %s with %llu payload bytes\n", activePath,
                static_cast<unsigned long long>(currentSegmentPayloadBytes));
  logEvent("CLOSE", activePath);
  return true;
}

bool initializeI2S() {
  const int8_t slotMask = static_cast<int8_t>(
      MIC_USE_LEFT_SLOT ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_RIGHT);
  AudioI2S.setPins(MIC_PIN_BCLK, MIC_PIN_WS, -1, MIC_PIN_DATA);

  Serial.printf("Initializing I2S: BCLK=%d WS=%d DATA=%d, %s slot\n",
                MIC_PIN_BCLK, MIC_PIN_WS, MIC_PIN_DATA,
                MIC_USE_LEFT_SLOT ? "left" : "right");

  if (!AudioI2S.begin(I2S_MODE_STD, SAMPLE_RATE_HZ,
                      I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO,
                      slotMask, I2S_ROLE_MASTER)) {
    Serial.printf("I2S begin failed; last error %d\n", AudioI2S.lastError());
    return false;
  }
  return true;
}

int16_t convertMicrophoneSample(int32_t transportSample, bool &clipped) {
  int64_t value = static_cast<int64_t>(transportSample) >> MIC_TO_PCM_RIGHT_SHIFT;
  clipped = false;
  if (value > INT16_MAX) {
    value = INT16_MAX;
    clipped = true;
  } else if (value < INT16_MIN) {
    value = INT16_MIN;
    clipped = true;
  }
  return static_cast<int16_t>(value);
}

void updateMaxWriteTime(uint32_t writeUs) {
  portENTER_CRITICAL(&metricsMux);
  if (writeUs > metrics.maxSdWriteUs) metrics.maxSdWriteUs = writeUs;
  portEXIT_CRITICAL(&metricsMux);
}

size_t writeMeasured(File &file, const uint8_t *data, size_t bytes) {
  const uint64_t start = esp_timer_get_time();
  const size_t written = file.write(data, bytes);
  const uint32_t duration = static_cast<uint32_t>(esp_timer_get_time() - start);
  updateMaxWriteTime(duration);

  portENTER_CRITICAL(&metricsMux);
  metrics.storedBytesWritten += written;
  if (written != bytes) {
    ++metrics.sdShortWrites;
    if (written == 0) ++metrics.sdWriteFailures;
  }
  portEXIT_CRITICAL(&metricsMux);
  return written;
}

void printMetrics(bool force = false) {
  if (!SERIAL_DIAGNOSTICS) return;
  const uint64_t now = nowMs();
  if (!force && now - lastMetricsMs < METRICS_INTERVAL_MS) return;
  lastMetricsMs = now;

  Metrics snapshot;
  copyMetrics(snapshot);
  const double seconds = elapsedUs() / 1000000.0;
  // Time spent paused is excluded, otherwise every pause leaves "written"
  // permanently behind "expected-bytes" and the comparison stops meaning
  // anything for the rest of the run.
  const double pausedSeconds = totalPausedMs / 1000.0;
  const double recordingSeconds = seconds > pausedSeconds ? seconds - pausedSeconds : 0.0;
  const uint64_t expectedSamples = static_cast<uint64_t>(seconds * SAMPLE_RATE_HZ);
  const uint64_t expectedStoredBytes = static_cast<uint64_t>(recordingSeconds * STORED_BYTE_RATE);

  Serial.printf(
      "t=%.1fs samples=%llu expected=%llu queued=%llu dropped=%llu "
      "written=%llu expected-bytes=%llu max-write=%uus queue-high=%u "
      "raw=[%ld,%ld] zeros=%llu clips=%llu i2s-short=%u sd-short=%u "
      "sd-retry=%u sd-recover=%u paused=%.1fs heap=%u\n",
      seconds,
      static_cast<unsigned long long>(snapshot.samplesCaptured),
      static_cast<unsigned long long>(expectedSamples),
      static_cast<unsigned long long>(snapshot.samplesQueued),
      static_cast<unsigned long long>(snapshot.samplesDropped),
      static_cast<unsigned long long>(snapshot.storedBytesWritten),
      static_cast<unsigned long long>(expectedStoredBytes),
      static_cast<unsigned>(snapshot.maxSdWriteUs),
      static_cast<unsigned>(snapshot.queueHighWater),
      static_cast<long>(snapshot.rawMinimum == INT32_MAX ? 0 : snapshot.rawMinimum),
      static_cast<long>(snapshot.rawMaximum == INT32_MIN ? 0 : snapshot.rawMaximum),
      static_cast<unsigned long long>(snapshot.zeroSamples),
      static_cast<unsigned long long>(snapshot.clippedSamples),
      static_cast<unsigned>(snapshot.i2sShortReads),
      static_cast<unsigned>(snapshot.sdShortWrites),
      static_cast<unsigned>(snapshot.sdWriteRetries),
      static_cast<unsigned>(snapshot.sdRecoveries),
      pausedSeconds,
      static_cast<unsigned>(ESP.getFreeHeap()));
}

void fillStressBuffer(uint32_t sequence) {
  uint32_t state = 0x9E3779B9u ^ sequence;
  for (size_t i = 0; i < sizeof(sdStressBuffer); ++i) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    sdStressBuffer[i] = static_cast<uint8_t>(state & 0xFF);
  }
  memcpy(sdStressBuffer, &sequence, sizeof(sequence));
}

bool startSdWriteTest() {
  if (!initializeSd()) return false;
  applyPowerProfile();
  if (!findUnusedPath("SDT", ".BIN", activePath, sizeof(activePath))) return false;
  activeFile = SD.open(activePath, FILE_WRITE);
  if (!activeFile) return false;
  Serial.printf("Opened %s\n", activePath);
  modeStartUs = esp_timer_get_time();
  lastFlushMs = nowMs();
  return true;
}

void serviceSdWriteTest() {
  if (modeFinished || fatalError) return;

  const uint32_t durationSeconds = SD_WRITE_TEST_SECONDS;
  const uint64_t elapsed = elapsedUs();
  const uint64_t finalTarget = durationSeconds == 0
                                   ? UINT64_MAX
                                   : static_cast<uint64_t>(durationSeconds) * STORED_BYTE_RATE;
  uint64_t scheduledBytes = (elapsed * STORED_BYTE_RATE) / 1000000ULL;
  if (scheduledBytes > finalTarget) scheduledBytes = finalTarget;

  Metrics snapshot;
  copyMetrics(snapshot);
  if (snapshot.storedBytesWritten < scheduledBytes) {
    const uint64_t deficit = scheduledBytes - snapshot.storedBytesWritten;
    const size_t bytesToWrite = static_cast<size_t>(
        deficit > sizeof(sdStressBuffer) ? sizeof(sdStressBuffer) : deficit);
    fillStressBuffer(generatedBlockSequence++);
    if (writeMeasured(activeFile, sdStressBuffer, bytesToWrite) != bytesToWrite) {
      stopWithError("SD stress-test write failed or was short");
      activeFile.close();
      return;
    }
  }

  if (nowMs() - lastFlushMs >= FILE_FLUSH_INTERVAL_MS) {
    activeFile.flush();
    lastFlushMs = nowMs();
  }

  printMetrics();

  copyMetrics(snapshot);
  if (durationSeconds != 0 && snapshot.storedBytesWritten >= finalTarget) {
    activeFile.flush();
    activeFile.close();
    modeFinished = true;
    printMetrics(true);
    Serial.printf("PASS CANDIDATE: wrote exactly %llu bytes to %s\n",
                  static_cast<unsigned long long>(finalTarget), activePath);
    Serial.println("Copy the file to macOS and confirm its exact size before declaring PASS.");
  }
}

bool startI2SDiscardTest() {
  if (!initializeI2S()) return false;
  applyPowerProfile();
  modeStartUs = esp_timer_get_time();
  return true;
}

void serviceI2SDiscardTest() {
  if (modeFinished || fatalError) return;

  Metrics beforeRead;
  copyMetrics(beforeRead);
  const uint64_t targetSamples = I2S_DISCARD_TEST_SECONDS == 0
                                     ? UINT64_MAX
                                     : static_cast<uint64_t>(I2S_DISCARD_TEST_SECONDS) * SAMPLE_RATE_HZ;
  if (beforeRead.samplesCaptured >= targetSamples) {
    AudioI2S.end();
    modeFinished = true;
    printMetrics(true);
    Serial.println("I2S discard test complete. Review sample count, raw range, and short-read count.");
    return;
  }

  const uint64_t remainingSamples = targetSamples - beforeRead.samplesCaptured;
  const size_t requestedSamples = remainingSamples < AUDIO_SAMPLES_PER_BLOCK
                                      ? static_cast<size_t>(remainingSamples)
                                      : AUDIO_SAMPLES_PER_BLOCK;
  const size_t requested = requestedSamples * sizeof(int32_t);
  const size_t bytesRead = AudioI2S.readBytes(
      reinterpret_cast<char *>(transportBuffer), requested);
  const size_t sampleCount = bytesRead / sizeof(int32_t);

  updateSignalMetrics(transportBuffer, sampleCount);
  portENTER_CRITICAL(&metricsMux);
  metrics.transportBytesRead += bytesRead;
  metrics.samplesCaptured += sampleCount;
  if (bytesRead != requested) ++metrics.i2sShortReads;
  portEXIT_CRITICAL(&metricsMux);

  printMetrics();

  Metrics afterRead;
  copyMetrics(afterRead);
  if (I2S_DISCARD_TEST_SECONDS != 0 && afterRead.samplesCaptured >= targetSamples) {
    AudioI2S.end();
    modeFinished = true;
    printMetrics(true);
    Serial.println("I2S discard test complete. Review sample count, raw range, and short-read count.");
  }
}

void captureTask(void *parameter) {
  (void)parameter;
  watchdogRegister();
  uint64_t capturedForRun = 0;
  const uint64_t targetSamples = RECORD_TEST_SECONDS == 0
                                     ? UINT64_MAX
                                     : static_cast<uint64_t>(RECORD_TEST_SECONDS) * SAMPLE_RATE_HZ;

  while (!fatalError && capturedForRun < targetSamples) {
    watchdogFeed();
    const uint64_t remainingSamples = targetSamples - capturedForRun;
    const size_t requestedSamples = remainingSamples < AUDIO_SAMPLES_PER_BLOCK
                                        ? static_cast<size_t>(remainingSamples)
                                        : AUDIO_SAMPLES_PER_BLOCK;
    const size_t requested = requestedSamples * sizeof(int32_t);
    const size_t bytesRead = AudioI2S.readBytes(
        reinterpret_cast<char *>(transportBuffer), requested);
    const size_t sampleCount = bytesRead / sizeof(int32_t);
    capturedForRun += sampleCount;

    updateSignalMetrics(transportBuffer, sampleCount);
    portENTER_CRITICAL(&metricsMux);
    metrics.transportBytesRead += bytesRead;
    metrics.samplesCaptured += sampleCount;
    if (bytesRead != requested) ++metrics.i2sShortReads;
    portEXIT_CRITICAL(&metricsMux);

    // Yield rather than spinning. This task runs at the highest priority,
    // so a mic that stops delivering would otherwise starve the writer and
    // the Arduino task outright on a single-core chip.
    if (sampleCount == 0) {
      vTaskDelay(1);
      continue;
    }

    AudioBlock *block = nullptr;
    if (xQueueReceive(freeBlockQueue, &block, 0) != pdTRUE) {
      portENTER_CRITICAL(&metricsMux);
      metrics.samplesDropped += sampleCount;
      portEXIT_CRITICAL(&metricsMux);
      continue;
    }

    uint64_t clipped = 0;
    for (size_t i = 0; i < sampleCount; ++i) {
      bool wasClipped = false;
      block->pcm[i] = convertMicrophoneSample(transportBuffer[i], wasClipped);
      if (wasClipped) ++clipped;
    }
    block->bytes = sampleCount * sizeof(int16_t);

    if (xQueueSend(filledBlockQueue, &block, 0) != pdTRUE) {
      portENTER_CRITICAL(&metricsMux);
      metrics.samplesDropped += sampleCount;
      portEXIT_CRITICAL(&metricsMux);
      xQueueSend(freeBlockQueue, &block, portMAX_DELAY);
      continue;
    }

    const uint32_t queued = uxQueueMessagesWaiting(filledBlockQueue);
    portENTER_CRITICAL(&metricsMux);
    metrics.samplesQueued += sampleCount;
    metrics.clippedSamples += clipped;
    if (queued > metrics.queueHighWater) metrics.queueHighWater = queued;
    portEXIT_CRITICAL(&metricsMux);
  }

  captureDone = true;
  vTaskDelete(nullptr);
}

uint8_t consecutiveRecoveries = 0;
volatile bool waitingForStorage = false;
extern bool awaitingCardRemoval;
extern uint32_t storageAttempts;
uint64_t lastStorageAttemptMs = 0;

const char *resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "external-pin";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt-watchdog";
    case ESP_RST_TASK_WDT: return "task-watchdog";
    case ESP_RST_WDT:      return "other-watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO:     return "sdio";
    default:               return "unknown";
  }
}

// Appends a line to the on-card log. Failures are ignored on purpose: the
// log is a diagnostic aid and must never be able to disturb recording.
void logEvent(const char *event, const char *detail) {
  if (!EVENT_LOG_ENABLED) return;
  File log = SD.open(EVENT_LOG_PATH, FILE_APPEND);
  if (!log) return;
  log.printf("%llu,%s,%s\n", static_cast<unsigned long long>(nowMs()), event,
             detail != nullptr ? detail : "");
  log.flush();
  log.close();
}

// The boot record needs a mounted card, which may not exist until one is
// inserted some time after startup. Written once, whenever that happens.
bool bootLogged = false;

void logBootOnce() {
  if (bootLogged) return;
  logEvent("BOOT", resetReasonName());
  bootLogged = true;
}

bool storageExhausted() {
  return estimatedFreeBytes() < SD_MIN_FREE_BYTES;
}

// Escalating recovery from a failed or short write: reopen the file, then
// remount the card. A transient card stall used to end the whole recording
// session permanently; now only a card that stays broken does.
// Stops writing without ending the run: the board stays awake and keeps
// checking for usable storage. This is what makes pulling the card a pause
// rather than the end of the deployment.
char storageLostReason[32] = {0};
uint64_t storageLostAtMs = 0;
bool fullCardReported = false;

void enterStorageWait(const char *reason) {
  if (!PAUSE_WHEN_STORAGE_ABSENT) {
    stopWithError(reason);
    return;
  }

  // Attempted, but it only lands when the card is still present (a full
  // card, say). If the card was pulled there is nothing to write to, which
  // is why the reason is also kept in RAM and recorded on the way back in.
  logEvent("STORAGE_LOST", reason);
  snprintf(storageLostReason, sizeof(storageLostReason), "%s", reason);
  storageLostAtMs = nowMs();

  Serial.printf("Storage unavailable (%s). Recording paused; will resume "
                "automatically when a working card is present.\n",
                reason);

  activeFile.close();
  SD.end();
  waitingForStorage = true;
  consecutiveRecoveries = 0;
  fullCardReported = false;
  storageAttempts = 0;
  lastStorageAttemptMs = nowMs();
}

// Polls for storage while paused. A reinserted or swapped card resumes into
// a fresh file; the filename scan restarts because a different card has
// different contents.
uint32_t storageAttempts = 0;
bool awaitingCardRemoval = false;

bool tryResumeStorage() {
  const uint32_t interval = storageAttempts < STORAGE_RETRY_FAST_ATTEMPTS
                                ? STORAGE_RETRY_INTERVAL_MS
                                : STORAGE_RETRY_SLOW_INTERVAL_MS;
  if (nowMs() - lastStorageAttemptMs < interval) return false;
  lastStorageAttemptMs = nowMs();
  ++storageAttempts;

  // Quiet and shallow: this runs every few seconds for as long as the card
  // is out, so the full noisy mount ladder would bury the log.
  const bool mounted = initializeSd(SD_REMOUNT_RETRY_LIMIT, true);

  // After a safe eject the card is still sitting in the slot. Remounting it
  // would resume recording within seconds and defeat the whole point, so
  // stay parked until the card has actually been taken out, and only then
  // treat a successful mount as a reinsertion.
  if (awaitingCardRemoval) {
    if (mounted) {
      SD.end();
      return false;
    }
    awaitingCardRemoval = false;
    Serial.println("Card removed. Waiting for one to be inserted.");
    return false;
  }

  if (!mounted) return false;

  if (storageExhausted()) {
    // Said once, not every poll. A full card stays full until somebody
    // swaps it, and repeating this forever was pure noise.
    if (!fullCardReported) {
      Serial.println("Card is full. Waiting for one with free space.");
      fullCardReported = true;
    }
    SD.end();
    return false;
  }
  fullCardReported = false;

  logBootOnce();

  nextPathIndex = 1;
  if (!openNextRecordingFile()) {
    SD.end();
    return false;
  }

  waitingForStorage = false;
  totalPausedMs += nowMs() - storageLostAtMs;

  // The matching STORAGE_LOST often could not be written, so this line
  // carries the reason and the outage length instead.
  char detail[96];
  snprintf(detail, sizeof(detail), "%s after %llu ms into %s",
           storageLostReason[0] != '\0' ? storageLostReason : "unknown",
           static_cast<unsigned long long>(nowMs() - storageLostAtMs),
           activePath);
  Serial.printf("Storage back; recording resumed into %s\n", activePath);
  logEvent("STORAGE_BACK", detail);
  return true;
}

bool recoverStorage() {
  portENTER_CRITICAL(&metricsMux);
  ++metrics.sdRecoveries;
  portEXIT_CRITICAL(&metricsMux);

  if (++consecutiveRecoveries > SD_RECOVERY_LIMIT) {
    enterStorageWait("recovery exhausted");
    return false;
  }

  Serial.printf("SD write trouble; recovery attempt %u of %u\n",
                consecutiveRecoveries, SD_RECOVERY_LIMIT);
  logEvent("RECOVER", activePath);

  activeFile.close();
  delay(SD_RECOVERY_DELAY_MS);

  // Only trust a full-card reading while the card still responds; an
  // unresponsive card reports zero capacity, identical to a full one.
  if (SD.cardType() != CARD_NONE && storageExhausted()) {
    enterStorageWait("card full");
    return false;
  }

  if (openNextRecordingFile()) return true;

  Serial.println("Reopen failed; remounting the card.");
  SD.end();
  delay(SD_RECOVERY_DELAY_MS);
  if (initializeSd(SD_REMOUNT_RETRY_LIMIT) && openNextRecordingFile()) return true;

  return false;
}

bool rotateSegment() {
  if (!closeRecordingFile()) return recoverStorage();

  // Checked here rather than waiting for a write to fail, so the card runs
  // out between files instead of partway through one.
  if (storageExhausted()) {
    enterStorageWait("card full");
    return false;
  }

  if (openNextRecordingFile()) return true;
  return recoverStorage();
}

// Closes the file and unmounts so the card can be pulled without leaving
// the filesystem half-written, then parks in the normal paused state.
void safeEject() {
  if (waitingForStorage) {
    Serial.println("Already paused; the card is safe to remove.");
    return;
  }
  Serial.println("Safe eject requested.");
  closeRecordingFile();
  enterStorageWait("ejected");
  awaitingCardRemoval = true;
  Serial.println("Card unmounted. Safe to remove. Recording resumes when a "
                 "card is inserted again.");
}

volatile bool ejectRequested = false;

void pollEjectControls() {
  if (EJECT_VIA_SERIAL && Serial.available() > 0) {
    const int key = Serial.read();
    if (key == 'e' || key == 'E') ejectRequested = true;
  }

  if (EJECT_BUTTON_PIN >= 0) {
    static uint64_t lastEdgeMs = 0;
    static int lastLevel = HIGH;
    const int level = digitalRead(EJECT_BUTTON_PIN);
    if (level != lastLevel && nowMs() - lastEdgeMs >= EJECT_BUTTON_DEBOUNCE_MS) {
      lastEdgeMs = nowMs();
      lastLevel = level;
      if (level == LOW) ejectRequested = true;
    }
  }
}

bool writeRecordingPayload(const uint8_t *data, size_t bytes) {
  const uint64_t segmentLimit = FILE_SEGMENT_SECONDS == 0
                                    ? UINT64_MAX
                                    : static_cast<uint64_t>(FILE_SEGMENT_SECONDS) * STORED_BYTE_RATE;
  size_t offset = 0;
  uint8_t attempt = 0;

  while (offset < bytes && !fatalError) {
    if (currentSegmentPayloadBytes >= segmentLimit) {
      if (!rotateSegment()) return false;
    }

    const uint64_t capacity = segmentLimit - currentSegmentPayloadBytes;
    const size_t remaining = bytes - offset;
    const size_t portion = capacity < remaining ? static_cast<size_t>(capacity) : remaining;

    // Advance by what actually landed, so a retry resends only the missing
    // tail rather than duplicating audio that already reached the card.
    const size_t written = writeMeasured(activeFile, data + offset, portion);
    currentSegmentPayloadBytes += written;
    bytesWrittenSinceMount += written;
    offset += written;

    if (written != portion) {
      portENTER_CRITICAL(&metricsMux);
      ++metrics.sdWriteRetries;
      portEXIT_CRITICAL(&metricsMux);

      if (++attempt > SD_WRITE_RETRY_LIMIT) {
        if (!recoverStorage()) return false;
        attempt = 0;
      } else {
        delay(SD_RETRY_DELAY_MS);
      }
      continue;
    }

    attempt = 0;
    consecutiveRecoveries = 0;

    if (nowMs() - lastFlushMs >= FILE_FLUSH_INTERVAL_MS) {
      activeFile.flush();
      lastFlushMs = nowMs();
    }
  }
  return offset == bytes;
}

void watchdogRegister() {
  if (!WATCHDOG_ENABLED) return;
  esp_task_wdt_add(nullptr);
}

void watchdogFeed() {
  if (!WATCHDOG_ENABLED) return;
  esp_task_wdt_reset();
}

void writerTask(void *parameter) {
  (void)parameter;
  watchdogRegister();

  while (!fatalError) {
    watchdogFeed();

    if (ejectRequested) {
      ejectRequested = false;
      safeEject();
    }

    // Paused: keep draining the queue so capture never blocks, but throw the
    // audio away because there is nowhere to put it, and keep looking for a
    // card. Capture stays running so I2S is still healthy when storage
    // returns.
    if (waitingForStorage) {
      AudioBlock *block = nullptr;
      while (xQueueReceive(filledBlockQueue, &block, 0) == pdTRUE) {
        portENTER_CRITICAL(&metricsMux);
        metrics.samplesDropped += block->bytes / sizeof(int16_t);
        portEXIT_CRITICAL(&metricsMux);
        xQueueSend(freeBlockQueue, &block, portMAX_DELAY);
      }

      tryResumeStorage();

      if (captureDone) break;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    AudioBlock *block = nullptr;
    if (xQueueReceive(filledBlockQueue, &block, pdMS_TO_TICKS(100)) == pdTRUE) {
      writeRecordingPayload(reinterpret_cast<const uint8_t *>(block->pcm), block->bytes);
      xQueueSend(freeBlockQueue, &block, portMAX_DELAY);
      continue;
    }

    if (captureDone && uxQueueMessagesWaiting(filledBlockQueue) == 0) break;
  }

  closeRecordingFile();
  writerDone = true;
  vTaskDelete(nullptr);
}

bool startRecording() {
  // A missing card at startup is the same condition as losing one mid-run,
  // so it pauses rather than refusing to start. This also means you can
  // flash the board with no card in, carry it to where it is going, and
  // have it begin a clean first file the moment a card goes in.
  const bool storageReady = initializeSd();
  if (!storageReady && !PAUSE_WHEN_STORAGE_ABSENT) return false;

  if (!initializeI2S()) return false;
  applyPowerProfile();

  freeBlockQueue = xQueueCreate(AUDIO_BLOCK_COUNT, sizeof(AudioBlock *));
  filledBlockQueue = xQueueCreate(AUDIO_BLOCK_COUNT, sizeof(AudioBlock *));
  if (freeBlockQueue == nullptr || filledBlockQueue == nullptr) {
    stopWithError("Could not allocate audio queues");
    return false;
  }

  for (size_t i = 0; i < AUDIO_BLOCK_COUNT; ++i) {
    AudioBlock *block = &audioBlocks[i];
    xQueueSend(freeBlockQueue, &block, portMAX_DELAY);
  }

  Serial.printf("Reset reason: %s\n", resetReasonName());

  if (storageReady) {
    logBootOnce();
    if (!openNextRecordingFile()) {
      stopWithError("Could not open the first recording file");
      return false;
    }
  } else {
    Serial.println("No usable card at startup. Waiting for one; recording "
                   "begins as soon as a card is inserted.");
    waitingForStorage = true;
    lastStorageAttemptMs = nowMs();
  }

  modeStartUs = esp_timer_get_time();
  captureDone = false;
  writerDone = false;

  // Separate cores, so a slow SD write can never delay I2S capture.
  constexpr BaseType_t writerCore = 1;
  constexpr BaseType_t captureCore = 0;

  if (xTaskCreatePinnedToCore(writerTask, "sd_writer", 8192, nullptr, 2,
                              &writerTaskHandle, writerCore) != pdPASS) {
    stopWithError("Could not create SD writer task");
    return false;
  }
  if (xTaskCreatePinnedToCore(captureTask, "i2s_capture", 8192, nullptr, 3,
                              &captureTaskHandle, captureCore) != pdPASS) {
    stopWithError("Could not create I2S capture task");
    return false;
  }
  return true;
}

void serviceRecording() {
  printMetrics();
  // Only meaningful for a bounded test run; in production the capture task
  // never finishes, so none of this executes.
  if (RECORD_TEST_SECONDS != 0 && writerDone && !modeFinished) {
    AudioI2S.end();
    modeFinished = true;
    printMetrics(true);
    Metrics snapshot;
    copyMetrics(snapshot);
    if (!fatalError && snapshot.samplesDropped == 0 &&
        snapshot.sdShortWrites == 0 && snapshot.i2sShortReads == 0 &&
        snapshot.sdRecoveries == 0) {
      Serial.println("PASS CANDIDATE: recording completed with zero reported drops or short writes.");
      Serial.println("Validate file size, duration, and audio on macOS before declaring PASS.");
    } else {
      Serial.println("FAIL: review the error and drop counters above.");
    }
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(1200);
  printBoardIdentity();
  resetMetrics();

  if (WATCHDOG_ENABLED) {
    esp_task_wdt_config_t wdt = {};
    wdt.timeout_ms = WATCHDOG_TIMEOUT_MS;
    wdt.idle_core_mask = 0;
    wdt.trigger_panic = true;
    // Already initialized by the core on some builds; reconfigure then.
    if (esp_task_wdt_init(&wdt) == ESP_ERR_INVALID_STATE) {
      esp_task_wdt_reconfigure(&wdt);
    }
  }

  if (EJECT_BUTTON_PIN >= 0) pinMode(EJECT_BUTTON_PIN, INPUT_PULLUP);

  bool started = false;
  switch (LOGGER_MODE) {
    case MODE_IDLE_BASELINE:
      modeStartUs = esp_timer_get_time();
      applyPowerProfile();
      started = true;
      Serial.println("Idle baseline running; no SD or I2S peripheral is initialized.");
      break;
    case MODE_SD_WRITE_TEST:
      started = startSdWriteTest();
      break;
    case MODE_I2S_DISCARD_TEST:
      started = startI2SDiscardTest();
      break;
    case MODE_RECORD_RAW_PCM:
    case MODE_RECORD_WAV:
      started = startRecording();
      break;
    default:
      stopWithError("Invalid LOGGER_MODE in config.h");
      break;
  }

  if (!started && !fatalError) stopWithError("Selected mode failed to start");
}

void loop() {
  if (fatalError) {
    printMetrics();
    if (RESTART_ON_FATAL_ERROR) {
      Serial.printf("Restarting in %" PRIu32 " ms to retry startup.\n",
                    FATAL_RESTART_DELAY_MS);
      Serial.flush();
      delay(FATAL_RESTART_DELAY_MS);
      ESP.restart();
    }
    delay(1000);
    return;
  }

  pollEjectControls();

  switch (LOGGER_MODE) {
    case MODE_IDLE_BASELINE:
      printMetrics();
      delay(100);
      break;
    case MODE_SD_WRITE_TEST:
      serviceSdWriteTest();
      break;
    case MODE_I2S_DISCARD_TEST:
      serviceI2SDiscardTest();
      break;
    case MODE_RECORD_RAW_PCM:
    case MODE_RECORD_WAV:
      serviceRecording();
      delay(10);
      break;
    default:
      delay(1000);
      break;
  }

  if (modeFinished) delay(1000);
}
