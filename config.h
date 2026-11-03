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

constexpr LoggerMode LOGGER_MODE = MODE_RECORD_RAW_PCM;
constexpr char FIRMWARE_VERSION[] = "0.6.0";

// Serial diagnostics. Set false for a deployed logger: the startup banner
// still prints, but the periodic metrics line stops, which removes the
// only recurring CPU/UART work in the steady state.
constexpr bool SERIAL_DIAGNOSTICS = true;
constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t METRICS_INTERVAL_MS = 5000;

// Target stored audio format
constexpr uint32_t SAMPLE_RATE_HZ = 48000;
constexpr uint16_t STORED_BITS_PER_SAMPLE = 16;
constexpr uint16_t STORED_CHANNELS = 1;
constexpr uint32_t STORED_BYTE_RATE =
    SAMPLE_RATE_HZ * STORED_CHANNELS * (STORED_BITS_PER_SAMPLE / 8);

// SPI microSD and INMP441 wiring. VCC/VDD are physical power connections,
// not GPIOs. Pin numbers are board-specific, so supporting another board
// means adding a block here, not changing any other file.
#if CONFIG_IDF_TARGET_ESP32
// Classic ESP32 Dev Module / DOIT ESP32 DEVKIT V1.
constexpr int SD_PIN_SCK = 18;
constexpr int SD_PIN_MISO = 19;
constexpr int SD_PIN_MOSI = 23;
constexpr int SD_PIN_CS = 27;

constexpr int MIC_PIN_BCLK = 26;
constexpr int MIC_PIN_WS = 25;
constexpr int MIC_PIN_DATA = 33;

#else
#error "This firmware targets the classic ESP32 only. Select ESP32 Dev " \
       "Module or DOIT ESP32 DEVKIT V1, or add a pin block above for the " \
       "board you are using instead of guessing pin numbers."
#endif

// Requested SD SPI clock. The SPI peripheral is clocked from APB, and APB
// follows the CPU clock once the CPU drops below 80 MHz, so this is clamped at runtime to what APB can actually produce.
// Audio needs only 96 kB/s (768 kbit/s), so even a heavily clamped clock
// has bandwidth to spare. Drop to 4000000 if breadboard wiring is marginal.
constexpr uint32_t SD_SPI_FREQUENCY_HZ = 10000000;

// CPU clock profile.
//
// MEASURED, not theorized: I2S capture stops working entirely below 80 MHz.
// POWER_FULL and POWER_NORMAL record correctly; POWER_LOW and
// POWER_SUPER_LOW produce no samples at all -- every I2S read sits out its
// full 1 s timeout and returns nothing, so the recording is silently empty.
//
// The boundary lines up exactly with the CPU clock source: 80 MHz and above
// run the CPU from the PLL, 40 MHz and below run it from the crystal, and
// crossing that line kills the capture stream. The precise mechanism was
// not pinned down; what is certain is the behavior. The static_assert below
// turns that into a build error rather than an empty SD card.
//
// Lower profiles are still useful for the modes that never touch the
// microphone (MODE_IDLE_BASELINE, MODE_SD_WRITE_TEST), which is exactly
// what you want for measuring a power floor.
//
// Also note the SD SPI clock is derived from APB, and APB follows the CPU
// clock below 80 MHz, so the requested SPI rate is clamped at runtime.
// Audio needs only 96 kB/s, so that clamp costs nothing in practice.
enum PowerProfile : uint8_t {
  POWER_SUPER_LOW = 0,
  POWER_LOW = 1,
  POWER_NORMAL = 2,
  POWER_FULL = 3,
};

constexpr PowerProfile CPU_POWER_PROFILE = POWER_NORMAL;

// Only frequencies the chip actually accepts are listed. The classic ESP32
// runs a 40 MHz crystal and supports crystal/1, /2 and /4 alongside the
// PLL-derived steps. Requesting anything else makes setCpuFrequencyMhz()
// fail and leaves the clock alone, which setup() reports over serial.
constexpr uint32_t CPU_FREQUENCY_MHZ = CPU_POWER_PROFILE == POWER_SUPER_LOW ? 10
                                       : CPU_POWER_PROFILE == POWER_LOW     ? 40
                                       : CPU_POWER_PROFILE == POWER_NORMAL  ? 80
                                                                            : 240;

// With INMP441 L/R tied to GND, select the left I2S slot.
// Set false only if L/R is instead tied to 3.3 V.
constexpr bool MIC_USE_LEFT_SLOT = true;

// INMP441 transports a 24-bit sample in a 32-bit I2S slot. The default keeps
// the most significant 16 bits. Change only after inspecting Stage 2 metrics.
constexpr uint8_t MIC_TO_PCM_RIGHT_SHIFT = 16;

// Test durations. A value of 0 means run until reset/power removal.
constexpr uint32_t SD_WRITE_TEST_SECONDS = 30 * 60;
constexpr uint32_t I2S_DISCARD_TEST_SECONDS = 15 * 60;
constexpr uint32_t RECORD_TEST_SECONDS = 0;

// Long recordings are segmented so a reset risks only the active file.
// The flush interval is also the worst-case data-loss window on sudden
// power loss: audio written since the last flush may not be reachable
// because the directory entry still carries the old file size.
constexpr uint32_t FILE_SEGMENT_SECONDS = 30 * 60;
constexpr uint32_t FILE_FLUSH_INTERVAL_MS = 5000;

// Storage fault handling. A transient short/failed write used to be fatal
// and stopped the whole recording until a power cycle. Instead the writer
// now retries, then reopens the file, then remounts the card, and only
// gives up if recovery itself keeps failing.
// Mounting the card is retried with a progressively slower SPI clock, since
// a first attempt can fail on marginal wiring or the card's power-up surge.
constexpr uint8_t SD_MOUNT_RETRY_LIMIT = 4;
constexpr uint32_t SD_SPI_MIN_FREQUENCY_HZ = 1000000;

// Production behavior. When true, losing the card (pulled out, gone bad, or
// full) pauses recording instead of ending the run: the board stays awake,
// discards audio while it has nowhere to put it, and resumes into a new file
// as soon as a working card is present again. Set false only if you would
// rather the firmware stop dead on a storage fault.
constexpr bool PAUSE_WHEN_STORAGE_ABSENT = true;

// Polling for a returned card starts brisk so a card swap resumes quickly,
// then backs off. Each poll mounts and unmounts the card, so retrying every
// few seconds for days on end would be a lot of pointless card activity.
constexpr uint32_t STORAGE_RETRY_INTERVAL_MS = 3000;
constexpr uint32_t STORAGE_RETRY_SLOW_INTERVAL_MS = 15000;
constexpr uint8_t STORAGE_RETRY_FAST_ATTEMPTS = 20;

// Appends one line per notable event to the card, so an unattended run can
// be explained afterwards without having watched the serial output. This is
// what tells you whether a short file was a storage fault or a reset, and
// for a reset, what caused it.
constexpr bool EVENT_LOG_ENABLED = true;
constexpr char EVENT_LOG_PATH[] = "/EVENTS.CSV";

constexpr uint8_t SD_WRITE_RETRY_LIMIT = 3;
constexpr uint8_t SD_RECOVERY_LIMIT = 5;
constexpr uint32_t SD_RETRY_DELAY_MS = 20;
constexpr uint32_t SD_RECOVERY_DELAY_MS = 250;

// Remounting during recovery and while polling for a returned card uses
// fewer attempts than a cold boot, because recovery already loops around
// this several times and the full ladder made a removed card take ten
// seconds to notice.
constexpr uint8_t SD_REMOUNT_RETRY_LIMIT = 2;

// Recording filenames are REC00001..REC99999, which is still 8.3-safe. At
// the default 30-minute segment that ceiling is several years of files --
// far past the point where any card fills up, so it stops being a limit.
constexpr uint32_t RECORDING_INDEX_MAX = 99999;

// Stop cleanly instead of thrashing once the card is nearly full. The
// reserve has to be at least one whole segment, otherwise the card fills
// partway through a file instead of stopping at a clean boundary.
constexpr uint64_t FILE_SEGMENT_BYTES =
    static_cast<uint64_t>(FILE_SEGMENT_SECONDS) * STORED_BYTE_RATE;
constexpr uint64_t SD_MIN_FREE_BYTES =
    (FILE_SEGMENT_BYTES > 0 ? FILE_SEGMENT_BYTES : 256ULL * 1024 * 1024) +
    16ULL * 1024 * 1024;

// Safe eject. Pulling a card while a file is open is an unclean unmount and
// can damage the filesystem over time. Either control closes the file,
// unmounts, and parks in the paused state so the card can be removed and
// returned safely.
//
// Set EJECT_BUTTON_PIN to a free GPIO to use a button (wired to ground;
// the internal pull-up is enabled). -1 disables it.
constexpr int EJECT_BUTTON_PIN = -1;
constexpr uint32_t EJECT_BUTTON_DEBOUNCE_MS = 50;
constexpr bool EJECT_VIA_SERIAL = true;  // send 'e' in the Serial Monitor

// A hung task would otherwise leave a silent dead logger in the field until
// somebody walks out to it. The watchdog reboots instead, costing one file
// rather than the rest of the deployment. The timeout is deliberately far
// longer than any legitimate SD stall.
constexpr bool WATCHDOG_ENABLED = true;
constexpr uint32_t WATCHDOG_TIMEOUT_MS = 60000;

// Same reasoning for errors that are fatal rather than recoverable (see the
// README): restarting retries the whole startup instead of sitting dead.
constexpr bool RESTART_ON_FATAL_ERROR = true;
constexpr uint32_t FATAL_RESTART_DELAY_MS = 10000;

// Buffers: 4096 mono samples = 8192 stored bytes = about 85.3 ms at 48 kHz.
// Eight of those is roughly 682 ms of queue reserve. Larger blocks mean
// proportionally fewer SD write transactions for the same audio, which cuts
// both per-write filesystem overhead and SD card active time (less power),
// and leaves far more slack for a slow card than small blocks did.
constexpr size_t AUDIO_SAMPLES_PER_BLOCK = 4096;
constexpr size_t AUDIO_BLOCK_COUNT = 8;
constexpr size_t SD_STRESS_WRITE_BYTES = 16 * 1024;

constexpr bool LOGGER_MODE_USES_MIC = LOGGER_MODE == MODE_I2S_DISCARD_TEST ||
                                      LOGGER_MODE == MODE_RECORD_RAW_PCM ||
                                      LOGGER_MODE == MODE_RECORD_WAV;

// Measured on hardware: below 80 MHz the I2S reads time out and return
// nothing, so recordings come out empty with no error anywhere. Failing the
// build is better than discovering that on a card after a deployment.
static_assert(!(LOGGER_MODE_USES_MIC && CPU_POWER_PROFILE < POWER_NORMAL),
              "I2S capture returns no samples below 80 MHz. Use POWER_NORMAL "
              "or POWER_FULL for any mode that reads the microphone. "
              "POWER_LOW and POWER_SUPER_LOW still work with "
              "MODE_IDLE_BASELINE and MODE_SD_WRITE_TEST for power floor "
              "measurements.");

static_assert(STORED_BITS_PER_SAMPLE == 16, "This prototype stores 16-bit PCM");
static_assert(STORED_CHANNELS == 1, "This prototype stores mono PCM");
static_assert(MIC_TO_PCM_RIGHT_SHIFT <= 31, "Invalid microphone shift");
static_assert(AUDIO_BLOCK_COUNT >= 2, "At least two audio buffers are required");
static_assert(SD_WRITE_RETRY_LIMIT >= 1, "At least one write attempt is required");
