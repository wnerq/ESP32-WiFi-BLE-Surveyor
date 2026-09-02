// ESP32 Wireless Surveyor firmware.
// Provides Wi-Fi/BLE surveying, a browser interface, serial controls, session checkpointing, and developer diagnostics.
//
// Improve mobile survey resilience, freshness, and AP identity reuse
//
// - reclaim the least-recently-seen unreferenced AP-table identity safely
// - default hidden-network history capture off while preserving live RF analysis
// - expose newest-observation and per-scan new/known/logged/drop diagnostics
// - recover asynchronous Wi-Fi scans that remain pending for 60 seconds
// - invoke saved-infrastructure reconnect detection after completed surveys
// - add explicit mobile-friendly Apply controls
//
// Dependency: NimBLE-Arduino 2.5.0 (install with Arduino Library Manager).
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <FS.h>
#include <SPIFFS.h>
#include <ESPmDNS.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_arduino_version.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

// ============================================================
// Firmware identity
// ============================================================

const char* FIRMWARE_FILE = "WifiConnect38e_mobile_survey_resilience.ino";
const char* FIRMWARE_VERSION = "38e";


Preferences preferences;
WebServer server(80);

const unsigned long WIFI_TIMEOUT_MS = 15000;
const unsigned long WIFI_STARTUP_SETTLE_MS = 300;
const uint32_t INFRA_RECONNECT_BACKOFF_MS = 30000;
const uint32_t INFRA_RECONNECT_ATTEMPT_WINDOW_MS = WIFI_TIMEOUT_MS;

bool webServerStarted = false;

// Hardware and firmware properties that cannot change during one boot are
// captured once so web diagnostics do not repeatedly invoke low-level queries.
struct BootStaticSystemInfo {
  esp_reset_reason_t resetReason = ESP_RST_UNKNOWN;
  size_t appPartitionBytes = 0;
  size_t sketchBytes = 0;
  size_t unusedAppBytes = 0;
  size_t flashBytes = 0;
  uint8_t chipRevision = 0;
  uint8_t chipCores = 0;
  uint32_t cpuFreqMHz = 0;
  char chipModel[32] = "Unknown";
};

BootStaticSystemInfo bootStaticSystemInfo;

// ============================================================
// Local network identity / mDNS
// ============================================================

const char* DEFAULT_MDNS_HOSTNAME = "surveyor";
const size_t MAX_MDNS_HOSTNAME_LENGTH = 32;
String mdnsHostname = DEFAULT_MDNS_HOSTNAME;
bool mdnsHostnameUserConfigured = false;
bool mdnsStarted = false;
bool mdnsAttempted = false;
String mdnsStatusMessage = "Not started";


// ============================================================
// Access point configuration
// ============================================================

bool apEnabled = true;
bool apRunning = false;

String apSSID = "";
String apPassword = "";
String apStatusMessage = "";


// ============================================================
// Scan logger
// ============================================================

// Scan history is intentionally RAM-only. It is cleared on power cycle/reset.
// The same SSID/BSSID may appear in many records with different scan numbers
// and timestamps, which is useful for signal-strength trending.
const size_t MIN_SCAN_HISTORY_RECORDS = 50;
const size_t MAX_SCAN_HISTORY_RECORDS = 12000;
const size_t MIN_BLE_HISTORY_RECORDS = 50;
const size_t MAX_BLE_HISTORY_RECORDS = 12000;
const size_t BLE_ADDRESS_TABLE_TARGET = 128;
const size_t BLE_SCAN_METADATA_SLOTS = 256;
const size_t WIFI_ONLY_AP_TABLE_TARGET = 512;
const size_t WIFI_ONLY_SCAN_METADATA_SLOTS = 1024;
const size_t DUAL_RADIO_WIFI_AP_TABLE_TARGET = 64;
const size_t DUAL_RADIO_WIFI_SCAN_METADATA_SLOTS = 64;
// Dual-radio history sizing deliberately leaves additional heap unallocated.
// Web page generation, mDNS, radio activity, and transient String buffers need
// substantial headroom after the survey tables are created. This allocation-
// time reserve is intentionally higher than the desired steady-state runtime
// margin because web and network services consume additional heap afterward.
const size_t DUAL_RADIO_HEAP_RESERVE_BYTES = 88 * 1024;

// Wi-Fi-only mode can devote substantially more RAM to retained observations
// because the BLE stack and BLE history tables are absent.
const size_t HISTORY_HEAP_RESERVE_BYTES = 64 * 1024;

const unsigned long MIN_SCAN_INTERVAL_SECONDS = 5;
const unsigned long MAX_SCAN_INTERVAL_SECONDS = 3600;
const uint32_t BLE_SCAN_DURATION_SECONDS = 5;
const unsigned long INITIAL_WIFI_SCAN_DELAY_MS = 2500;
const unsigned long INITIAL_BLE_SCAN_DELAY_MS = 9000;
const size_t HEAP_WARN_BYTES = 48 * 1024;
// Minimum-free-heap is the more important unattended-runtime indicator.
// In dual-radio mode warn below 20 KB; Wi-Fi-only has ample margin.
const size_t DUAL_RADIO_MIN_HEAP_WARN_BYTES = 20 * 1024;

// Wi-Fi-first operating mode. Bluetooth is disabled by default and the
// preference is stored in NVS. Changing the setting from the web UI triggers
// a controlled restart so BLE is either fully initialized before history
// allocation or never initialized at all.
bool bleSurveyEnabled = false;

// Common ESP32 DEVKITV1 boards expose a controllable blue LED on GPIO2.
// The red LED found on many boards is a hard-wired power LED and cannot be
// controlled by firmware. Set STATUS_LED_AVAILABLE false if GPIO2 is not an
// onboard LED on the specific board being used.
const bool STATUS_LED_AVAILABLE = true;
bool statusLedEnabled = true;
bool statusLedSelfTestOverride = false;
bool webAutoRefreshEnabled = true;
const uint8_t STATUS_LED_PIN = 2;
const bool STATUS_LED_ACTIVE_HIGH = true;
const TickType_t WIFI_SCAN_LED_PERIOD_TICKS = pdMS_TO_TICKS(75);
const TickType_t BLE_SCAN_LED_PERIOD_TICKS = pdMS_TO_TICKS(125);


// ScanRecord remains a synthesized view used by the existing UI/CSV code.
// This expanded record is synthesized for presentation rather than stored in the compact history ring.
struct ScanRecord {
  uint32_t scanNumber;
  uint32_t uptimeMs;
  char ssid[33];
  char bssid[18];
  int16_t rssi;
  int16_t channel;
  uint8_t authMode;
  bool connected;
  bool hidden;
};

// Stable attributes are stored once per unique BSSID/AP.
struct WifiApEntry {
  char ssid[33];
  uint8_t bssid[6];
  uint8_t channel;
  uint8_t authMode;
  uint16_t lastSeenScanLow;
};

// Generic scan metadata shared by Wi-Fi and BLE. Each radio has its own
// metadata table/counter, but both use the same representation and lifecycle.
struct SurveyScanMetadata {
  uint32_t scanNumber;
  uint32_t uptimeMs;
};
using WifiScanMetadata = SurveyScanMetadata;

// Compact recurring measurement. 6 bytes on the classic ESP32 ABI versus
// 68 bytes for the expanded ScanRecord representation.
struct WifiObservation {
  uint16_t apIndex;
  uint16_t scanSlot;
  int8_t rssi;
  uint8_t reserved;
};

// Synthesized BLE view used by existing UI/CSV code. Compact history no longer stores
// this flat record in the recurring history ring.
struct BleScanRecord {
  uint32_t scanNumber;
  uint32_t uptimeMs;
  char name[48];
  char address[18];
  int16_t rssi;
  uint8_t addressType;
  bool named;
};

// Current properties are stored once per observed BLE address. A random BLE
// address is treated only as an observed address identity; the firmware does
// not claim that it permanently identifies one physical device.
struct BleAddressEntry {
  char name[48];
  uint8_t address[6];
  uint8_t addressType;
};

// Compact recurring BLE measurement. Meaningful payload is address reference,
// scan reference, and whole-dBm RSSI. Natural alignment makes this 6 bytes.
struct BleObservation {
  uint16_t addressIndex;
  uint16_t scanSlot;
  int8_t rssi;
};
static_assert(sizeof(BleObservation) == 6, "Unexpected BleObservation size");

struct SignalStats {
  uint32_t samples;
  int16_t latestRssi;
  int16_t minRssi;
  int16_t maxRssi;
  int32_t rssiTotal;
  uint32_t firstSeenMs;
  uint32_t lastSeenMs;
};

struct NetworkSummary {
  char ssid[33];
  char bssid[18];
  int16_t channel;
  uint8_t authMode;
  SignalStats signal;
  bool connected;
  bool hidden;
};

struct BLEDeviceSummary {
  char name[48];
  char address[18];
  uint8_t addressType;
  SignalStats signal;
  bool named;
};

struct BootHeapCheckpoint {
  const char* stage;
  uint32_t freeHeap;
  uint32_t minimumFreeHeap;
  uint32_t largestFreeBlock;
};

// Explicit prototypes for custom types avoid Arduino 1.8.x auto-prototype ordering issues.
const WifiObservation& compactHistoryRecord(size_t logicalIndex);
ScanRecord historyRecord(size_t logicalIndex);
void appendWifiObservation(const WifiObservation& observation);
const BleObservation& compactBleHistoryRecord(size_t logicalIndex);
BleScanRecord bleHistoryRecord(size_t logicalIndex);
void appendBleObservation(const BleObservation& observation);
void discardBleObservationsForScanSlot(uint16_t scanSlot);
void updateBleUsageHighWaterMarks();
int findOrCreateBleAddress(const uint8_t address[6], const String& name, uint8_t addressType);
int findWifiApByTextBssid(const String& bssid);
int findOrCreateWifiAp(const uint8_t bssid[6], const String& ssid, uint8_t channel, uint8_t authMode, bool* created = nullptr, bool* reclaimed = nullptr);
float rssiInterferenceWeight(int rssi);
float channelOverlapFactor(int distance);
bool buildNetworkSummaryByApIndex(uint16_t apIndex, NetworkSummary& summary);
void serviceLoggedWifiScan();
String jsonQuoted(const String& value);

const size_t MAX_BOOT_HEAP_CHECKPOINTS = 12;
BootHeapCheckpoint bootHeapCheckpoints[MAX_BOOT_HEAP_CHECKPOINTS] = {};
size_t bootHeapCheckpointCount = 0;

TimerHandle_t statusLedTimer = nullptr;
volatile bool statusLedState = false;

WifiObservation* scanHistory = nullptr;
size_t scanHistoryCapacity = 0;        // Physical observation capacity allocated at boot.
size_t scanHistoryRetentionLimit = 0;  // Logical user-selected retention limit.
size_t historyStart = 0;
size_t historyCount = 0;
String historyResizeMessage = "";
uint32_t scanCounter = 0;
uint32_t lastScanUptimeMs = 0;
unsigned long scanIntervalSeconds = 300;
unsigned long lastAutoScanMs = 0;
uint32_t wifiAutoScanStartCount = 0;
uint32_t wifiAutoScanCompletionCount = 0;
uint32_t wifiAutoScanStartFailureCount = 0;
uint32_t wifiAutoScanCompletionFailureCount = 0;
uint32_t lastWifiAutoScanStartMs = 0;
uint32_t lastWifiAutoScanCompletionMs = 0;
bool wifiCurrentScanAutomatic = false;
bool wifiScanCompletedSinceBoot = false;
uint32_t wifiCurrentScanStartMs = 0;
uint32_t wifiScanDurationCount = 0;
uint32_t wifiLastScanDurationMs = 0;
uint32_t wifiMinScanDurationMs = 0;
uint32_t wifiMaxScanDurationMs = 0;
uint64_t wifiTotalScanDurationMs = 0;
bool wifiAutoScanRetryPending = false;
uint32_t lastWifiAutoScanFailureMs = 0;

// Interaction-priority policy:
// - Never abort a Wi-Fi scan that has already started.
// - Explicit web actions arm a short defer only after their handler returns.
// - Background live-update/status requests do not arm the defer.
// - CSV export is serviced before any new automatic scan; scheduler timing is preserved.
const uint32_t USER_INTERACTION_DEFER_MS = 2000;
const uint32_t WIFI_AUTOSCAN_RETRY_BACKOFF_MS = 2000;
const uint32_t WIFI_AUTOSCAN_DIAG_GRACE_MS = USER_INTERACTION_DEFER_MS + 1000;
const uint32_t WIFI_SCAN_WATCHDOG_MS = 60000;
const size_t CSV_STREAM_BUFFER_BYTES = 2048;

bool explicitUserInteractionHandled = false;
bool userInteractionDeferArmed = false;
uint32_t userInteractionDeferStartedMs = 0;
bool csvExportInProgress = false;
// Declared with the other global survey state so checkpoint/test helpers
// defined earlier in this sketch can safely reference the active scan state.
bool wifiScanInProgress = false;

uint32_t wifiCsvExportCount = 0;
size_t wifiCsvLastRows = 0;
size_t wifiCsvLastBytes = 0;
uint32_t wifiCsvLastDurationMs = 0;
uint32_t bleCsvExportCount = 0;
size_t bleCsvLastRows = 0;
size_t bleCsvLastBytes = 0;
uint32_t bleCsvLastDurationMs = 0;

WifiApEntry* wifiApTable = nullptr;
size_t wifiApTableCapacity = 0;
size_t wifiApCount = 0;
WifiScanMetadata* wifiScanMetadata = nullptr;
size_t wifiScanMetadataCapacity = 0;
size_t wifiApTableFullDrops = 0;
size_t wifiApReclamationCount = 0;
bool captureHiddenNetworks = false;
uint16_t wifiLastScanFound = 0;
uint16_t wifiLastScanLogged = 0;
uint16_t wifiLastScanDropped = 0;
uint16_t wifiLastScanNewAps = 0;
uint16_t wifiLastScanPreviouslySeenAps = 0;
uint16_t wifiLastScanHiddenSkipped = 0;
uint16_t wifiLastScanReclaimedAps = 0;

// Latest-scan RF aggregates remain valid even when hidden-network history
// capture is disabled. This keeps channel analysis representative without
// spending AP-table or history capacity on hidden BSSIDs.
uint32_t wifiLatestAnalysisScan = 0;
uint16_t wifiLatestChannelApCount[12] = {};
int16_t wifiLatestChannelStrongestRssi[12] = {};
float wifiLatestCoChannelScore[12] = {};
float wifiLatestAdjacentScore[12] = {};

// Session checkpoint / restore and test-tool diagnostics.
const char* SESSION_CHECKPOINT_PATH = "/survey_session.bin";
const uint32_t SESSION_CHECKPOINT_MAGIC = 0x53565233; // "SVR3"
const uint16_t SESSION_CHECKPOINT_VERSION = 3;
bool spiffsMounted = false;
bool sessionRestoredThisBoot = false;
String sessionCheckpointStatus = "Filesystem not initialized";
// History uses a continued survey-session timebase across checkpoint restores.
// Scheduling and boot uptime continue to use raw millis().
uint32_t sessionUptimeOffsetMs = 0;
uint32_t syntheticPrefillRuns = 0;
size_t syntheticPrefillLastTarget = 0;

// Native station reconnect is preferred over issuing competing WiFi.begin()
// calls from the survey scheduler. These counters observe link transitions.
bool nativeReconnectStateInitialized = false;
bool nativeReconnectSawDisconnect = false;
bool nativeReconnectWasConnected = false;
uint32_t nativeReconnectObservedCount = 0;

BleObservation* bleHistory = nullptr;
size_t bleHistoryCapacity = 0;        // Physical observation capacity allocated at boot.
size_t bleHistoryRetentionLimit = 0;  // Logical user-selected retention limit.
size_t bleHistoryStart = 0;
size_t bleHistoryCount = 0;
BleAddressEntry* bleAddressTable = nullptr;
size_t bleAddressTableCapacity = 0;
size_t bleAddressCount = 0;
size_t bleAddressPeakReferenced = 0;
size_t bleAddressTableFullDrops = 0;
SurveyScanMetadata* bleScanMetadata = nullptr;
size_t bleScanMetadataCapacity = 0;
size_t bleScanMetadataPeakUsed = 0;
String bleHistoryResizeMessage = "";
String bleStatusMessage = "";
uint32_t bleScanCounter = 0;
uint32_t lastBleScanUptimeMs = 0;
bool autoBleScanEnabled = true;
unsigned long bleScanIntervalSeconds = 300;
unsigned long lastAutoBleScanMs = 0;
bool bleInitialized = false;
bool wifiSubsystemInitialized = false;
bool initialWifiScanPending = true;
bool initialBleScanPending = true;
unsigned long surveyServicesReadyMs = 0;

size_t bootWifiHistoryCapacity = 0;
size_t bootBleHistoryCapacity = 0;


// ============================================================
// Developer diagnostic streaming
// ============================================================

// Diagnostic streaming settings are persisted so a controlled restart (for
// example enabling BLE mode) can be captured from the earliest BLE init event.
// First-use defaults are selected for the BLE characterization campaign while
// the master switch remains OFF until explicitly enabled.
bool diagnosticStreamingEnabled = false;
bool diagnosticSurveyEvents = true;
bool diagnosticBleEvents = true;
bool diagnosticMemoryEvents = true;
bool diagnosticWebEvents = true;
bool diagnosticSchedulerEvents = false;
bool diagnosticCheckpointEvents = false;
uint32_t diagnosticSnapshotIntervalMs = 10000;
uint32_t diagnosticLastSnapshotMs = 0;

struct DiagnosticEventRecord {
  uint32_t uptimeMs = 0;
  char category[16] = "";
  char detail[80] = "";
};

const size_t DIAGNOSTIC_EVENT_CAPACITY = 32;
DiagnosticEventRecord diagnosticEvents[DIAGNOSTIC_EVENT_CAPACITY] = {};
size_t diagnosticEventStart = 0;
size_t diagnosticEventCount = 0;
size_t diagnosticExportEventLimit = 16;

// BLE timing separates scan-start API time from scan duration and the time
// spent normalizing captured advertisements into retained history.
bool bleDiagnosticScanActive = false;
uint32_t bleDiagnosticScanStartMs = 0;
volatile bool bleAsyncCompletionPending = false;
volatile uint32_t bleAsyncCompletionMs = 0;
char bleDiagnosticActiveTrigger[24] = "unknown";
char lastSerialCommand[96] = "(none)";
uint32_t bleDiagnosticLastApiDurationMs = 0;
uint32_t bleDiagnosticLastProcessingDurationMs = 0;
uint32_t bleDiagnosticLastTotalDurationMs = 0;
uint32_t bleDiagnosticDurationCount = 0;
uint64_t bleDiagnosticTotalDurationMs = 0;
uint32_t bleDiagnosticMinDurationMs = UINT32_MAX;
uint32_t bleDiagnosticMaxDurationMs = 0;
uint32_t bleDiagnosticLastResultCount = 0;
uint32_t bleDiagnosticLastScanEndMs = 0;
uint8_t bleDiagnosticSchedulerReason = 0;
uint32_t bleDiagnosticLastHeapBefore = 0;
uint32_t bleDiagnosticLastHeapAfterApi = 0;
uint32_t bleDiagnosticLastHeapAfterProcessing = 0;
uint32_t bleDiagnosticLastLargestBefore = 0;
uint32_t bleDiagnosticLastLargestAfterApi = 0;
uint32_t bleDiagnosticLastLargestAfterProcessing = 0;
uint32_t bleDiagnosticLastCaptureDrops = 0;
int bleDiagnosticLastCompletionReason = 0;

// NimBLE keeps no application scan-result list. Advertisements are copied into
// this bounded firmware-owned buffer and normalized into retained history after
// the scan-complete callback signals the main loop.
struct NimBleCapturedAdvertisement {
  uint8_t address[6];
  uint8_t addressType;
  int8_t rssi;
  char name[48];
};

const size_t BLE_SCAN_CAPTURE_CAPACITY = BLE_ADDRESS_TABLE_TARGET;
NimBleCapturedAdvertisement bleScanCapture[BLE_SCAN_CAPTURE_CAPACITY] = {};
volatile uint16_t bleScanCaptureCount = 0;
volatile uint32_t bleScanCaptureDrops = 0;
volatile int bleScanCompletionReason = 0;
portMUX_TYPE bleScanCaptureMux = portMUX_INITIALIZER_UNLOCKED;

void captureNimBleAdvertisement(const NimBLEAdvertisedDevice* advertisedDevice);

class SurveyNimBLEScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    captureNimBleAdvertisement(advertisedDevice);
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    (void)results;
    bleScanCompletionReason = reason;
    bleAsyncCompletionMs = millis();
    bleAsyncCompletionPending = true;
  }
};

SurveyNimBLEScanCallbacks surveyNimBLEScanCallbacks;

// Main-loop gap instrumentation identifies periods when loop(), web servicing,
// or survey scheduling is delayed beyond the normal execution cadence.
uint32_t diagnosticLastLoopEntryMs = 0;
uint32_t diagnosticLastLoopGapMs = 0;
uint32_t diagnosticMaxLoopGapMs = 0;
uint32_t diagnosticLoopGapOver100MsCount = 0;

// HTTP handler timing measures actual handler execution after WebServer begins
// servicing a request. It does not claim to know when a TCP request first
// arrived while loop() was blocked; loop-gap/BLE timestamps provide that side.
uint32_t diagnosticWebHandlerCount = 0;
uint32_t diagnosticWebSlowHandlerCount = 0;
uint32_t diagnosticWebLastDurationMs = 0;
uint32_t diagnosticWebMaxDurationMs = 0;
const uint32_t DIAGNOSTIC_WEB_SLOW_MS = 100;


// ============================================================
// Boot/resource diagnostics and status LED
// ============================================================

// Purpose: Records free/minimum/largest heap metrics at a named startup stage for later diagnostics.
void captureBootHeapCheckpoint(const char* stage) {
  if (bootHeapCheckpointCount >= MAX_BOOT_HEAP_CHECKPOINTS) return;

  BootHeapCheckpoint& cp = bootHeapCheckpoints[bootHeapCheckpointCount++];
  cp.stage = stage;
  cp.freeHeap = ESP.getFreeHeap();
  cp.minimumFreeHeap = ESP.getMinFreeHeap();
  cp.largestFreeBlock =
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

// Purpose: Drives the status LED to the requested logical state while honoring whether LED output is available.
void writeStatusLed(bool on) {
  if (!STATUS_LED_AVAILABLE) return;
  if (on && !statusLedEnabled && !statusLedSelfTestOverride) return;

  statusLedState = on;
  bool electricalHigh =
      STATUS_LED_ACTIVE_HIGH ? on : !on;
  digitalWrite(STATUS_LED_PIN, electricalHigh ? HIGH : LOW);
}

// Purpose: FreeRTOS timer callback used to toggle or advance status-LED scan indication.
void statusLedTimerCallback(TimerHandle_t) {
  writeStatusLed(!statusLedState);
}

// Purpose: Configures the status LED pin and creates the timer used for nonblocking indications.
void initializeStatusLed() {
  if (!STATUS_LED_AVAILABLE) return;

  pinMode(STATUS_LED_PIN, OUTPUT);
  writeStatusLed(false);

  statusLedTimer = xTimerCreate(
      "surveyLed",
      WIFI_SCAN_LED_PERIOD_TICKS,
      pdTRUE,
      nullptr,
      statusLedTimerCallback);
}

// Purpose: Starts the periodic LED indication used while a radio scan is active.
void startScanLed(TickType_t periodTicks) {
  if (!STATUS_LED_AVAILABLE || !statusLedEnabled || statusLedTimer == nullptr) return;

  writeStatusLed(true);
  xTimerStop(statusLedTimer, 0);
  xTimerChangePeriod(statusLedTimer, periodTicks, 0);
}

// Purpose: Stops scan indication and returns the status LED to its normal state.
void stopScanLed() {
  if (!STATUS_LED_AVAILABLE) return;

  if (statusLedTimer != nullptr) {
    xTimerStop(statusLedTimer, 0);
  }

  writeStatusLed(false);
}

void statusLedPulse(
  unsigned long onMs,
  unsigned long offMs
) {
  if (!STATUS_LED_AVAILABLE || (!statusLedEnabled && !statusLedSelfTestOverride)) return;

  writeStatusLed(true);
  delay(onMs);
  writeStatusLed(false);
  delay(offMs);
}

// Purpose: Shows the early-boot LED pattern before the web interface is ready.
void indicateBootStarted() {
  if (!STATUS_LED_AVAILABLE || (!statusLedEnabled && !statusLedSelfTestOverride)) return;

  statusLedPulse(90, 90);
  statusLedPulse(90, 0);
}

// Purpose: Shows the final startup LED state based on detected failure or warning conditions.
void indicateStartupStatus(bool failed, bool warning) {
  if (!STATUS_LED_AVAILABLE || (!statusLedEnabled && !statusLedSelfTestOverride)) return;

  if (failed) {
    for (int i = 0; i < 3; i++) statusLedPulse(300, 220);
    return;
  }

  if (warning) {
    for (int i = 0; i < 2; i++) statusLedPulse(250, 220);
    return;
  }

  statusLedPulse(650, 0);
}

// Purpose: Runs a short visible LED pattern so the user can verify the configured indicator hardware.
void runStatusLedSelfTest() {
  if (!STATUS_LED_AVAILABLE) return;

  if (statusLedTimer != nullptr) xTimerStop(statusLedTimer, 0);
  statusLedSelfTestOverride = true;
  writeStatusLed(false);
  delay(120);
  for (int i = 0; i < 3; i++) statusLedPulse(180, 180);
  writeStatusLed(true);
  delay(650);
  writeStatusLed(false);
  statusLedSelfTestOverride = false;
}


// ============================================================
// Serial helpers
// ============================================================

// Purpose: Reads one complete command line from Serial without changing the command parser itself.
String readSerialLine() {
  while (!Serial.available()) {
    if (webServerStarted) {
      server.handleClient();
    }
    delay(10);
  }

  String input = Serial.readStringUntil('\n');
  input.trim();
  return input;
}


// ============================================================
// Wi-Fi helpers
// ============================================================

// Purpose: Builds the short MAC-derived suffix used in generated default network names.
String macSuffix() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");

  if (mac.length() >= 6) {
    return mac.substring(mac.length() - 6);
  }

  return "ESP32";
}

// Purpose: Returns the generated default SSID for the ESP32-hosted Device AP.
String defaultApSSID() {
  return "ESP32-Surveyor-" + macSuffix();
}

// Purpose: Returns the default password used when no custom Device AP password is stored.
String defaultApPassword() {
  return "survey-" + macSuffix();
}

// Purpose: Loads Device AP settings from NVS and applies validated defaults when values are missing or invalid.
void loadAccessPointSettings() {
  preferences.begin("ap", true);

  bool hasEnabled = preferences.isKey("enabled");
  bool hasSSID = preferences.isKey("ssid");
  bool hasPassword = preferences.isKey("password");

  apEnabled =
      hasEnabled
        ? preferences.getBool("enabled", true)
        : true;

  apSSID =
      hasSSID
        ? preferences.getString("ssid", "")
        : "";

  apPassword =
      hasPassword
        ? preferences.getString("password", "")
        : "";

  preferences.end();

  if (apSSID.length() == 0) {
    apSSID = defaultApSSID();
  }

  if (apPassword.length() < 8) {
    apPassword = defaultApPassword();
  }
}

void saveAccessPointSettings(
  bool enabled,
  const String& ssid,
  const String& password
) {
  preferences.begin("ap", false);

  preferences.putBool("enabled", enabled);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);

  preferences.end();
}

// Purpose: Validates a proposed hostname against the firmware rules for length and allowed characters.
bool isValidMdnsHostname(const String& value) {
  if (value.length() < 1 || value.length() > MAX_MDNS_HOSTNAME_LENGTH) return false;
  if (value[0] == '-' || value[value.length() - 1] == '-') return false;

  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    bool digit = (c >= '0' && c <= '9');
    if (!alpha && !digit && c != '-') return false;
  }
  return true;
}

// Purpose: Normalizes user-entered hostname text before validation and storage.
String normalizedMdnsHostname(String value) {
  value.trim();
  value.toLowerCase();
  return value;
}

// Purpose: Builds the friendly HTTP address derived from the configured device hostname.
String mdnsWebAddress() {
  return "http://" + mdnsHostname + ".local/";
}

// Purpose: Loads persisted survey-mode, scan-interval, interface, and related runtime settings from NVS.
void loadSurveyModeSettings() {
  preferences.begin("survey", true);
  bleSurveyEnabled = preferences.getBool("bleEnabled", false);
  statusLedEnabled = preferences.getBool("ledEnabled", true);
  webAutoRefreshEnabled = preferences.getBool("webRefresh", true);
  captureHiddenNetworks = preferences.getBool("captureHidden", false);
  scanIntervalSeconds = preferences.getULong("wifiInterval", scanIntervalSeconds);
  bleScanIntervalSeconds = preferences.getULong("bleInterval", bleScanIntervalSeconds);
  if (scanIntervalSeconds < MIN_SCAN_INTERVAL_SECONDS) scanIntervalSeconds = MIN_SCAN_INTERVAL_SECONDS;
  if (scanIntervalSeconds > MAX_SCAN_INTERVAL_SECONDS) scanIntervalSeconds = MAX_SCAN_INTERVAL_SECONDS;
  if (bleScanIntervalSeconds < MIN_SCAN_INTERVAL_SECONDS) bleScanIntervalSeconds = MIN_SCAN_INTERVAL_SECONDS;
  if (bleScanIntervalSeconds > MAX_SCAN_INTERVAL_SECONDS) bleScanIntervalSeconds = MAX_SCAN_INTERVAL_SECONDS;
  mdnsHostnameUserConfigured = preferences.isKey("hostname");
  mdnsHostname = normalizedMdnsHostname(preferences.getString("hostname", DEFAULT_MDNS_HOSTNAME));
  preferences.end();

  if (!isValidMdnsHostname(mdnsHostname)) {
    mdnsHostname = DEFAULT_MDNS_HOSTNAME;
    mdnsHostnameUserConfigured = false;
  }
  autoBleScanEnabled = bleSurveyEnabled;
}

void saveCaptureHiddenNetworks(bool enabled) {
  captureHiddenNetworks = enabled;
  preferences.begin("survey", false);
  preferences.putBool("captureHidden", captureHiddenNetworks);
  preferences.end();
}

// Purpose: Builds a unique default hostname from the ESP32 identity.
String generatedDefaultMdnsHostname() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String("surveyor-") + suffix;
}

// Purpose: Applies a generated hostname when no valid saved hostname is available.
void applyGeneratedDefaultMdnsHostname() {
  if (mdnsHostnameUserConfigured) return;
  mdnsHostname = normalizedMdnsHostname(generatedDefaultMdnsHostname());
}

// Purpose: Stores the validated device hostname in NVS and updates the in-memory copy.
void saveMdnsHostname(const String& hostname) {
  mdnsHostname = normalizedMdnsHostname(hostname);
  mdnsHostnameUserConfigured = true;
  preferences.begin("survey", false);
  preferences.putString("hostname", mdnsHostname);
  preferences.end();
}

// Purpose: Starts mDNS advertising for the configured device hostname and records diagnostic status.
bool startMdnsService() {
  mdnsAttempted = true;

  if (WiFi.status() != WL_CONNECTED && !apRunning) {
    mdnsStarted = false;
    mdnsStatusMessage = "No active network interface";
    return false;
  }

  if (!isValidMdnsHostname(mdnsHostname)) {
    mdnsStarted = false;
    mdnsStatusMessage = "Invalid hostname";
    return false;
  }

  if (!MDNS.begin(mdnsHostname.c_str())) {
    mdnsStarted = false;
    mdnsStatusMessage = "mDNS responder failed to start";
    return false;
  }

  MDNS.addService("http", "tcp", 80);
  mdnsStarted = true;
  mdnsStatusMessage = "Advertising " + mdnsHostname + ".local";
  return true;
}

// Purpose: Persists whether Bluetooth Survey should be initialized on the next boot.
void saveBleSurveyEnabled(bool enabled) {
  preferences.begin("survey", false);
  preferences.putBool("bleEnabled", enabled);
  preferences.end();
}

// Purpose: Persists interface preferences that are stored on the ESP32 rather than in the browser.
void saveInterfaceSettings(bool ledEnabled, bool autoRefreshEnabled) {
  statusLedEnabled = ledEnabled;
  webAutoRefreshEnabled = autoRefreshEnabled;
  preferences.begin("survey", false);
  preferences.putBool("ledEnabled", statusLedEnabled);
  preferences.putBool("webRefresh", webAutoRefreshEnabled);
  preferences.end();
}

// Purpose: Persists the global Live Updates preference.
void saveWebLiveUpdates(bool enabled) {
  webAutoRefreshEnabled = enabled;
  preferences.begin("survey", false);
  preferences.putBool("webRefresh", webAutoRefreshEnabled);
  preferences.end();
}

// Purpose: Ensures the ESP32 Wi-Fi driver includes station capability without unnecessarily disrupting the Device AP.
void ensureWiFiStationMode() {
  wifi_mode_t currentMode = WiFi.getMode();

  if (apRunning || apEnabled) {
    if (currentMode != WIFI_AP_STA) {
      WiFi.mode(WIFI_AP_STA);
      delay(WIFI_STARTUP_SETTLE_MS);
    }
  } else {
    if (currentMode != WIFI_STA) {
      WiFi.mode(WIFI_STA);
      delay(WIFI_STARTUP_SETTLE_MS);
    }
  }
}

// Purpose: Starts the ESP32-hosted Device AP using the currently configured SSID and password.
bool startAccessPoint() {
  if (!apEnabled) {
    apRunning = false;
    return false;
  }

  WiFi.mode(WIFI_AP_STA);
  delay(WIFI_STARTUP_SETTLE_MS);

  bool started =
      WiFi.softAP(
        apSSID.c_str(),
        apPassword.c_str()
      );

  apRunning = started;

  Serial.println();

  if (started) {
    Serial.println("Access point started.");

    Serial.print("AP SSID:     ");
    Serial.println(apSSID);

    Serial.print("AP Password: ");
    Serial.println(apPassword);

    Serial.print("AP IP:       ");
    Serial.println(WiFi.softAPIP());

    Serial.println(
      "Connect directly to the AP to use the web interface "
      "without infrastructure Wi-Fi."
    );
  } else {
    Serial.println("Failed to start access point.");
  }

  return started;
}

// Purpose: Converts an ESP32 Wi-Fi authentication enum into a human-readable security label.
String securityLabel(wifi_auth_mode_t authMode) {
  switch (authMode) {
    case WIFI_AUTH_OPEN:
      return "OPEN";

    case WIFI_AUTH_WEP:
      return "WEP";

    case WIFI_AUTH_WPA_PSK:
      return "WPA-PSK";

    case WIFI_AUTH_WPA2_PSK:
      return "WPA2-PSK";

    case WIFI_AUTH_WPA_WPA2_PSK:
      return "WPA/WPA2-PSK";

    case WIFI_AUTH_WPA2_ENTERPRISE:
      return "WPA2-ENTERPRISE";

    case WIFI_AUTH_WPA3_PSK:
      return "WPA3-PSK";

    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "WPA2/WPA3-PSK";

    default:
      return "UNKNOWN";
  }
}


// ============================================================
// Shared survey/statistics helpers
// ============================================================

// Purpose: Initializes an RSSI statistics accumulator before observations are added.
void resetSignalStats(SignalStats& stats) {
  stats.samples = 0;
  stats.latestRssi = 0;
  stats.minRssi = 0;
  stats.maxRssi = 0;
  stats.rssiTotal = 0;
  stats.firstSeenMs = 0;
  stats.lastSeenMs = 0;
}

void addSignalObservation(
  SignalStats& stats,
  int16_t rssi,
  uint32_t uptimeMs
) {
  if (stats.samples == 0) {
    stats.samples = 1;
    stats.latestRssi = rssi;
    stats.minRssi = rssi;
    stats.maxRssi = rssi;
    stats.rssiTotal = rssi;
    stats.firstSeenMs = uptimeMs;
    stats.lastSeenMs = uptimeMs;
    return;
  }

  stats.samples++;
  stats.latestRssi = rssi;

  if (rssi < stats.minRssi) {
    stats.minRssi = rssi;
  }

  if (rssi > stats.maxRssi) {
    stats.maxRssi = rssi;
  }

  stats.rssiTotal += rssi;
  stats.lastSeenMs = uptimeMs;
}

// Purpose: Calculates mean RSSI from an accumulated SignalStats structure.
float averageSignal(const SignalStats& stats) {
  if (stats.samples == 0) {
    return 0.0f;
  }

  return
    (float)stats.rssiTotal /
    (float)stats.samples;
}


// ============================================================
// Session checkpoint / restore
// ============================================================

// Purpose: Returns the logical survey-session timebase, which can continue across checkpoint restores.
uint32_t surveySessionUptimeMs() {
  return millis() + sessionUptimeOffsetMs;
}

struct SessionCheckpointHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t headerBytes;
  uint32_t payloadBytes;
  uint32_t crc32;
  uint32_t savedUptimeMs;
  uint32_t scanCounter;
  uint32_t lastScanUptimeMs;
  uint32_t wifiObservationCount;
  uint32_t wifiApCount;
  uint32_t wifiMetadataCount;
  uint32_t bleObservationCount;
  uint32_t bleAddressCount;
  uint32_t bleMetadataCount;
};

// Purpose: Updates the CRC32 used to protect serialized session-checkpoint data.
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t length) {
  crc = ~crc;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++)
      crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)-(int32_t)(crc & 1));
  }
  return ~crc;
}

// Purpose: Writes one checkpoint payload chunk while simultaneously folding it into the running CRC.
bool checkpointWriteChunk(File& file, uint32_t& crc, const void* data, size_t bytes) {
  if (bytes == 0) return true;
  if (file.write((const uint8_t*)data, bytes) != bytes) return false;
  crc = crc32Update(crc, (const uint8_t*)data, bytes);
  return true;
}

// Purpose: Serializes retained survey state to SPIFFS with header/version/length/CRC protection.
bool saveSurveySessionCheckpoint(String& detail) {
  if (!spiffsMounted) {
    detail = "SPIFFS is not mounted.";
    return false;
  }
  if (wifiScanInProgress || csvExportInProgress) {
    detail = "Wait for the active Wi-Fi scan or CSV export to finish.";
    return false;
  }

  SessionCheckpointHeader h = {};
  h.magic = SESSION_CHECKPOINT_MAGIC;
  h.version = SESSION_CHECKPOINT_VERSION;
  h.headerBytes = sizeof(SessionCheckpointHeader);
  h.savedUptimeMs = surveySessionUptimeMs();
  h.scanCounter = scanCounter;
  h.lastScanUptimeMs = lastScanUptimeMs;
  h.wifiObservationCount = historyCount;
  h.wifiApCount = wifiApCount;
  h.wifiMetadataCount = wifiScanMetadataCapacity;
  h.bleObservationCount = bleSurveyEnabled ? bleHistoryCount : 0;
  h.bleAddressCount = bleSurveyEnabled ? bleAddressCount : 0;
  h.bleMetadataCount = bleSurveyEnabled ? bleScanMetadataCapacity : 0;
  h.payloadBytes =
      h.wifiObservationCount * sizeof(WifiObservation) +
      h.wifiApCount * sizeof(WifiApEntry) +
      h.wifiMetadataCount * sizeof(WifiScanMetadata) +
      h.bleObservationCount * sizeof(BleObservation) +
      h.bleAddressCount * sizeof(BleAddressEntry) +
      h.bleMetadataCount * sizeof(SurveyScanMetadata);

  File f = SPIFFS.open(SESSION_CHECKPOINT_PATH, FILE_WRITE);
  if (!f) { detail = "Unable to create checkpoint file."; return false; }
  h.crc32 = 0;
  if (f.write((const uint8_t*)&h, sizeof(h)) != sizeof(h)) {
    f.close(); SPIFFS.remove(SESSION_CHECKPOINT_PATH);
    detail = "Unable to write checkpoint header."; return false;
  }

  uint32_t crc = 0;
  bool ok = true;
  for (size_t i = 0; ok && i < historyCount; i++) {
    const WifiObservation& o = compactHistoryRecord(i);
    ok = checkpointWriteChunk(f, crc, &o, sizeof(o));
  }
  if (ok) ok = checkpointWriteChunk(f, crc, wifiApTable, h.wifiApCount * sizeof(WifiApEntry));
  if (ok) ok = checkpointWriteChunk(f, crc, wifiScanMetadata, h.wifiMetadataCount * sizeof(WifiScanMetadata));
  if (bleSurveyEnabled) {
    for (size_t i = 0; ok && i < bleHistoryCount; i++) {
      const BleObservation& o = compactBleHistoryRecord(i);
      ok = checkpointWriteChunk(f, crc, &o, sizeof(o));
    }
    if (ok) ok = checkpointWriteChunk(f, crc, bleAddressTable, h.bleAddressCount * sizeof(BleAddressEntry));
    if (ok) ok = checkpointWriteChunk(f, crc, bleScanMetadata, h.bleMetadataCount * sizeof(SurveyScanMetadata));
  }
  if (!ok) { f.close(); SPIFFS.remove(SESSION_CHECKPOINT_PATH); detail = "Checkpoint write failed."; return false; }
  h.crc32 = crc;
  if (!f.seek(0)) { f.close(); SPIFFS.remove(SESSION_CHECKPOINT_PATH); detail = "Unable to seek checkpoint header."; return false; }
  size_t rewritten = f.write((const uint8_t*)&h, sizeof(h));
  f.close();
  if (rewritten != sizeof(h)) { SPIFFS.remove(SESSION_CHECKPOINT_PATH); detail = "Unable to finalize checkpoint CRC."; return false; }

  detail = "Saved " + String(historyCount) + " Wi-Fi observation(s)";
  if (bleSurveyEnabled) detail += " and " + String(bleHistoryCount) + " BLE observation(s)";
  detail += ".";
  sessionCheckpointStatus = detail;
  if (diagnosticStreamingEnabled && diagnosticCheckpointEvents) recordDiagnosticEvent("CHECKPOINT", detail);
  return true;
}

// Purpose: Deletes the saved survey-session checkpoint from SPIFFS.
bool discardSurveySessionCheckpoint(String& detail) {
  if (!spiffsMounted) { detail = "SPIFFS is not mounted."; return false; }
  if (!SPIFFS.exists(SESSION_CHECKPOINT_PATH)) {
    detail = "No restart checkpoint exists.";
    sessionCheckpointStatus = detail;
    return true;
  }
  bool ok = SPIFFS.remove(SESSION_CHECKPOINT_PATH);
  detail = ok ? "Restart checkpoint discarded." : "Unable to remove restart checkpoint.";
  sessionCheckpointStatus = detail;
  if (diagnosticStreamingEnabled && diagnosticCheckpointEvents) recordDiagnosticEvent("CHECKPOINT", detail);
  return ok;
}

// Purpose: Validates and restores a saved checkpoint, including history tables and logical survey time.
bool restoreSurveySessionCheckpoint(String& detail) {
  if (!spiffsMounted || !SPIFFS.exists(SESSION_CHECKPOINT_PATH)) {
    detail = "No restart checkpoint found.";
    sessionCheckpointStatus = detail;
    return false;
  }
  File f = SPIFFS.open(SESSION_CHECKPOINT_PATH, FILE_READ);
  if (!f) { detail = "Unable to open restart checkpoint."; return false; }
  SessionCheckpointHeader h = {};
  if (f.read((uint8_t*)&h, sizeof(h)) != sizeof(h) ||
      h.magic != SESSION_CHECKPOINT_MAGIC ||
      h.version != SESSION_CHECKPOINT_VERSION ||
      h.headerBytes != sizeof(SessionCheckpointHeader)) {
    f.close(); detail = "Restart checkpoint header is invalid or incompatible."; sessionCheckpointStatus = detail; return false;
  }
  if (h.wifiApCount > wifiApTableCapacity || h.wifiMetadataCount > wifiScanMetadataCapacity ||
      (bleSurveyEnabled && (h.bleAddressCount > bleAddressTableCapacity || h.bleMetadataCount > bleScanMetadataCapacity))) {
    f.close(); detail = "Restart checkpoint tables do not fit the current survey mode."; sessionCheckpointStatus = detail; return false;
  }
  uint32_t expectedPayload =
      h.wifiObservationCount * sizeof(WifiObservation) + h.wifiApCount * sizeof(WifiApEntry) +
      h.wifiMetadataCount * sizeof(WifiScanMetadata) + h.bleObservationCount * sizeof(BleObservation) +
      h.bleAddressCount * sizeof(BleAddressEntry) + h.bleMetadataCount * sizeof(SurveyScanMetadata);
  if (expectedPayload != h.payloadBytes || f.size() != (size_t)sizeof(h) + h.payloadBytes) {
    f.close(); detail = "Restart checkpoint length check failed."; sessionCheckpointStatus = detail; return false;
  }

  uint32_t crc = 0;
  uint8_t buf[256];
  size_t remaining = h.payloadBytes;
  while (remaining) {
    size_t n = remaining > sizeof(buf) ? sizeof(buf) : remaining;
    int got = f.read(buf, n);
    if (got != (int)n) { f.close(); detail = "Restart checkpoint read failed."; sessionCheckpointStatus = detail; return false; }
    crc = crc32Update(crc, buf, n);
    remaining -= n;
  }
  if (crc != h.crc32) { f.close(); detail = "Restart checkpoint CRC check failed."; sessionCheckpointStatus = detail; return false; }
  f.seek(sizeof(h));

  clearScanHistory();
  size_t skipWifi = h.wifiObservationCount > scanHistoryRetentionLimit
      ? h.wifiObservationCount - scanHistoryRetentionLimit : 0;
  WifiObservation o = {};
  for (size_t i = 0; i < h.wifiObservationCount; i++) {
    if (f.read((uint8_t*)&o, sizeof(o)) != sizeof(o)) { f.close(); detail = "Wi-Fi observation restore failed."; return false; }
    if (i >= skipWifi) appendWifiObservation(o);
  }
  if (f.read((uint8_t*)wifiApTable, h.wifiApCount * sizeof(WifiApEntry)) != (int)(h.wifiApCount * sizeof(WifiApEntry))) { f.close(); detail = "Wi-Fi AP table restore failed."; return false; }
  wifiApCount = h.wifiApCount;
  memset(wifiScanMetadata, 0, wifiScanMetadataCapacity * sizeof(WifiScanMetadata));
  if (f.read((uint8_t*)wifiScanMetadata, h.wifiMetadataCount * sizeof(WifiScanMetadata)) != (int)(h.wifiMetadataCount * sizeof(WifiScanMetadata))) { f.close(); detail = "Wi-Fi metadata restore failed."; return false; }
  // Preserve saved history timestamps and continue the survey-session clock
  // from the checkpoint. Raw millis() still starts at zero after reboot and
  // remains the scheduler timebase. This avoids unsigned underflow when a
  // restored observation predates the current boot uptime.
  uint32_t restoreNow = millis();
  sessionUptimeOffsetMs = h.savedUptimeMs - restoreNow;
  scanCounter = h.scanCounter;
  lastScanUptimeMs = h.lastScanUptimeMs;

  if (h.bleObservationCount || h.bleAddressCount || h.bleMetadataCount) {
    if (bleSurveyEnabled && bleHistory && bleAddressTable && bleScanMetadata) {
      clearBleHistory();
      size_t skipBle = h.bleObservationCount > bleHistoryRetentionLimit
          ? h.bleObservationCount - bleHistoryRetentionLimit : 0;
      BleObservation bo = {};
      for (size_t i = 0; i < h.bleObservationCount; i++) {
        if (f.read((uint8_t*)&bo, sizeof(bo)) != sizeof(bo)) { f.close(); detail = "BLE observation restore failed."; return false; }
        if (i >= skipBle) appendBleObservation(bo);
      }
      if (f.read((uint8_t*)bleAddressTable, h.bleAddressCount * sizeof(BleAddressEntry)) != (int)(h.bleAddressCount * sizeof(BleAddressEntry))) { f.close(); detail = "BLE address restore failed."; return false; }
      bleAddressCount = h.bleAddressCount;
      memset(bleScanMetadata, 0, bleScanMetadataCapacity * sizeof(SurveyScanMetadata));
      if (f.read((uint8_t*)bleScanMetadata, h.bleMetadataCount * sizeof(SurveyScanMetadata)) != (int)(h.bleMetadataCount * sizeof(SurveyScanMetadata))) { f.close(); detail = "BLE metadata restore failed."; return false; }
      // BLE metadata uses the same continued survey-session timebase as Wi-Fi.
      // Saved timestamps remain unchanged; sessionUptimeOffsetMs was set above.
    } else {
      size_t skipBytes = h.bleObservationCount * sizeof(BleObservation) +
          h.bleAddressCount * sizeof(BleAddressEntry) + h.bleMetadataCount * sizeof(SurveyScanMetadata);
      if (!f.seek(f.position() + skipBytes)) { f.close(); detail = "Unable to skip saved BLE payload."; return false; }
    }
  }
  f.close();
  sessionRestoredThisBoot = true;
  bool consumed = SPIFFS.remove(SESSION_CHECKPOINT_PATH);
  detail = "Restored " + String(historyCount) + " Wi-Fi observation(s) from restart checkpoint";
  if (bleSurveyEnabled && bleHistoryCount) detail += " and " + String(bleHistoryCount) + " BLE observation(s)";
  detail += consumed ? "; checkpoint consumed." : "; warning: checkpoint could not be consumed.";
  sessionCheckpointStatus = detail;
  if (diagnosticStreamingEnabled && diagnosticCheckpointEvents) recordDiagnosticEvent("CHECKPOINT", detail);
  return true;
}

// Purpose: Mounts SPIFFS, performs first-use formatting when needed, and attempts automatic checkpoint restore.
void initializeSessionStorageAndRestore() {
  // First try a non-destructive mount so an existing checkpoint is preserved.
  // On an unformatted SPIFFS partition, retry with format-on-failure enabled.
  bool formattedAfterMountFailure = false;
  spiffsMounted = SPIFFS.begin(false);
  if (!spiffsMounted) {
    spiffsMounted = SPIFFS.begin(true);
    formattedAfterMountFailure = spiffsMounted;
  }
  if (!spiffsMounted) {
    sessionCheckpointStatus = "SPIFFS mount/format failed; checkpoint features unavailable.";
    return;
  }

  String detail;
  if (SPIFFS.exists(SESSION_CHECKPOINT_PATH)) {
    restoreSurveySessionCheckpoint(detail);
  } else {
    sessionCheckpointStatus = formattedAfterMountFailure
      ? "SPIFFS mounted after first-use format; no restart checkpoint."
      : "No restart checkpoint.";
  }
}

// Purpose: Waits briefly for active work to finish, then saves a checkpoint before an intentional reboot.
bool checkpointBeforeControlledRestart() {
  if (historyCount == 0 && (!bleSurveyEnabled || bleHistoryCount == 0)) return true;
  uint32_t waitStarted = millis();
  while (wifiScanInProgress && (uint32_t)(millis() - waitStarted) < 10000) {
    serviceLoggedWifiScan();
    delay(10);
  }
  String detail;
  return saveSurveySessionCheckpoint(detail);
}

// ============================================================
// Synthetic history test tools
// ============================================================

// Purpose: Adds deterministic synthetic Wi-Fi observations until the requested percentage of history capacity is filled.
bool prefillWifiHistoryToPercent(uint8_t percent, String& detail) {
  if (percent != 50 && percent != 75 && percent != 95) { detail = "Unsupported target."; return false; }
  if (!scanHistory || !wifiApTable || !wifiScanMetadata || wifiScanInProgress || csvExportInProgress) {
    detail = "History storage is unavailable or busy."; return false;
  }
  size_t target = (scanHistoryRetentionLimit * percent) / 100;
  if (historyCount >= target) { detail = "History is already at or above the requested target."; return true; }

  const uint8_t syntheticApCount = 16;
  uint16_t apIndexes[syntheticApCount];
  for (uint8_t a = 0; a < syntheticApCount; a++) {
    uint8_t bssid[6] = {0x02, 0x53, 0x59, 0x4E, 0x00, a};
    String ssid = "TEST-PREFILL-" + String(a + 1);
    int idx = findOrCreateWifiAp(bssid, ssid, (uint8_t)(1 + (a % 11)), WIFI_AUTH_OPEN);
    if (idx < 0) { detail = "AP table has no room for synthetic test identities."; return false; }
    apIndexes[a] = (uint16_t)idx;
  }

  while (historyCount < target) {
    scanCounter++;
    uint16_t slot = (uint16_t)((scanCounter - 1) % wifiScanMetadataCapacity);
    if (wifiScanMetadata[slot].scanNumber != 0 && wifiScanMetadata[slot].scanNumber != scanCounter)
      discardObservationsForScanSlot(slot);
    wifiScanMetadata[slot].scanNumber = scanCounter;
    wifiScanMetadata[slot].uptimeMs = surveySessionUptimeMs();
    lastScanUptimeMs = wifiScanMetadata[slot].uptimeMs;
    for (uint8_t a = 0; a < syntheticApCount && historyCount < target; a++) {
      WifiObservation test = {};
      test.apIndex = apIndexes[a];
      test.scanSlot = slot;
      test.rssi = (int8_t)(-35 - ((scanCounter + a * 7) % 55));
      test.reserved = 0xA5; // explicit synthetic marker for future diagnostics/export use
      appendWifiObservation(test);
    }
  }
  syntheticPrefillRuns++;
  syntheticPrefillLastTarget = target;
  detail = "Synthetic test data filled history to " + String(percent) + "% (" + String(historyCount) + " observations).";
  return true;
}

// Purpose: Adds deterministic synthetic BLE observations until the requested percentage of history capacity is filled.
bool prefillBleHistoryToPercent(uint8_t percent, String& detail) {
  if (percent != 50 && percent != 75 && percent != 95) { detail = "Unsupported target."; return false; }
  if (!bleSurveyEnabled || !bleHistory || !bleAddressTable || !bleScanMetadata ||
      bleHistoryRetentionLimit == 0 || bleScanMetadataCapacity == 0 ||
      bleDiagnosticScanActive || csvExportInProgress) {
    detail = "Bluetooth history storage is unavailable, disabled, or busy.";
    return false;
  }

  size_t target = (bleHistoryRetentionLimit * percent) / 100;
  if (bleHistoryCount >= target) { detail = "Bluetooth history is already at or above the requested target."; return true; }

  const uint8_t syntheticAddressCount = 16;
  uint16_t addressIndexes[syntheticAddressCount];
  for (uint8_t a = 0; a < syntheticAddressCount; a++) {
    uint8_t address[6] = {0xC2, 0x42, 0x4C, 0x45, 0x00, a};
    String name = "TEST-PREFILL-BLE-" + String(a + 1);
    int idx = findOrCreateBleAddress(address, name, 1);
    if (idx < 0) { detail = "BLE address table has no room for synthetic test identities."; return false; }
    addressIndexes[a] = (uint16_t)idx;
  }

  while (bleHistoryCount < target) {
    bleScanCounter++;
    uint16_t slot = (uint16_t)((bleScanCounter - 1) % bleScanMetadataCapacity);
    if (bleScanMetadata[slot].scanNumber != 0 && bleScanMetadata[slot].scanNumber != bleScanCounter)
      discardBleObservationsForScanSlot(slot);
    bleScanMetadata[slot].scanNumber = bleScanCounter;
    bleScanMetadata[slot].uptimeMs = surveySessionUptimeMs();
    lastBleScanUptimeMs = bleScanMetadata[slot].uptimeMs;

    for (uint8_t a = 0; a < syntheticAddressCount && bleHistoryCount < target; a++) {
      BleObservation test = {};
      test.addressIndex = addressIndexes[a];
      test.scanSlot = slot;
      test.rssi = (int8_t)(-38 - ((bleScanCounter + a * 9) % 52));
      appendBleObservation(test);
    }
  }

  updateBleUsageHighWaterMarks();
  detail = "Synthetic Bluetooth test data filled history to " + String(percent) + "% (" + String(bleHistoryCount) + " observations).";
  return true;
}

// Purpose: HTTP handler for the Developer synthetic-history prefill controls.
void handleHistoryPrefill() {
  markExplicitUserInteraction();
  if (!server.hasArg("percent")) { server.send(400, "text/plain", "Missing target percent."); return; }
  int percent = server.arg("percent").toInt();
  String radio = server.hasArg("radio") ? server.arg("radio") : "wifi";
  String detail;
  bool ok = radio == "ble"
    ? prefillBleHistoryToPercent((uint8_t)percent, detail)
    : prefillWifiHistoryToPercent((uint8_t)percent, detail);
  server.sendHeader("Location", "/system");
  server.send(ok ? 303 : 409, "text/plain", detail);
}

// Purpose: HTTP handler for manually saving a survey-session checkpoint.
void handleSessionCheckpointSave() {
  markExplicitUserInteraction();
  uint32_t waitStarted = millis();
  while (wifiScanInProgress && (uint32_t)(millis() - waitStarted) < 10000) {
    serviceLoggedWifiScan();
    delay(10);
  }
  String detail; bool ok = saveSurveySessionCheckpoint(detail);
  server.sendHeader("Location", "/system"); server.send(ok ? 303 : 409, "text/plain", detail);
}

// Purpose: HTTP handler for deleting a saved survey-session checkpoint.
void handleSessionCheckpointDiscard() {
  markExplicitUserInteraction();
  String detail; bool ok = discardSurveySessionCheckpoint(detail);
  server.sendHeader("Location", "/system"); server.send(ok ? 303 : 500, "text/plain", detail);
}

// Purpose: Observes infrastructure Wi-Fi link transitions so native ESP32 auto-reconnect behavior can be diagnosed.
void serviceNativeReconnectDiagnostics() {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (!nativeReconnectStateInitialized) {
    nativeReconnectStateInitialized = true;
    nativeReconnectWasConnected = connected;
    return;
  }
  if (nativeReconnectWasConnected && !connected) nativeReconnectSawDisconnect = true;
  if (!nativeReconnectWasConnected && connected && nativeReconnectSawDisconnect) {
    nativeReconnectObservedCount++;
    nativeReconnectSawDisconnect = false;
  }
  nativeReconnectWasConnected = connected;
}

// ============================================================
// Developer diagnostic streaming helpers
// ============================================================

// Purpose: Loads persisted developer logging selections early enough to observe BLE initialization after restart.
void loadDiagnosticStreamingSettings() {
  preferences.begin("diag", true);
  diagnosticStreamingEnabled = preferences.getBool("enabled", false);
  diagnosticSurveyEvents = preferences.getBool("survey", true);
  diagnosticBleEvents = preferences.getBool("ble", true);
  diagnosticMemoryEvents = preferences.getBool("memory", true);
  diagnosticWebEvents = preferences.getBool("web", true);
  diagnosticSchedulerEvents = preferences.getBool("scheduler", false);
  diagnosticCheckpointEvents = preferences.getBool("checkpoint", false);
  diagnosticSnapshotIntervalMs = preferences.getULong("snapshotMs", 10000);
  diagnosticExportEventLimit = preferences.getUInt("eventLimit", 16);
  if (diagnosticExportEventLimit > DIAGNOSTIC_EVENT_CAPACITY) diagnosticExportEventLimit = DIAGNOSTIC_EVENT_CAPACITY;
  preferences.end();
}

// Purpose: Persists developer logging selections so diagnostics survive controlled BLE mode restarts.
void saveDiagnosticStreamingSettings() {
  preferences.begin("diag", false);
  preferences.putBool("enabled", diagnosticStreamingEnabled);
  preferences.putBool("survey", diagnosticSurveyEvents);
  preferences.putBool("ble", diagnosticBleEvents);
  preferences.putBool("memory", diagnosticMemoryEvents);
  preferences.putBool("web", diagnosticWebEvents);
  preferences.putBool("scheduler", diagnosticSchedulerEvents);
  preferences.putBool("checkpoint", diagnosticCheckpointEvents);
  preferences.putULong("snapshotMs", diagnosticSnapshotIntervalMs);
  preferences.putUInt("eventLimit", diagnosticExportEventLimit);
  preferences.end();
}

// Purpose: Returns the largest currently allocatable 8-bit heap block for fragmentation diagnostics.
uint32_t diagnosticLargestFreeBlock() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

// Purpose: Retains one compact diagnostic event in a bounded RAM ring for later export.
void recordDiagnosticEvent(const char* category, const String& detail) {
  size_t index;
  if (diagnosticEventCount < DIAGNOSTIC_EVENT_CAPACITY) {
    index = (diagnosticEventStart + diagnosticEventCount) % DIAGNOSTIC_EVENT_CAPACITY;
    diagnosticEventCount++;
  } else {
    index = diagnosticEventStart;
    diagnosticEventStart = (diagnosticEventStart + 1) % DIAGNOSTIC_EVENT_CAPACITY;
  }
  DiagnosticEventRecord& event = diagnosticEvents[index];
  event.uptimeMs = millis();
  snprintf(event.category, sizeof(event.category), "%s", category ? category : "EVENT");
  snprintf(event.detail, sizeof(event.detail), "%s", detail.c_str());
}

// Purpose: Returns one retained diagnostic event by chronological index.
const DiagnosticEventRecord& diagnosticEventAt(size_t logicalIndex) {
  return diagnosticEvents[(diagnosticEventStart + logicalIndex) % DIAGNOSTIC_EVENT_CAPACITY];
}

// Purpose: Prints the common timestamp/category prefix used by streamed diagnostic events.
void diagnosticPrefix(const char* category) {
  Serial.print("[");
  Serial.print(millis());
  Serial.print("] ");
  Serial.print(category);
  Serial.print(" ");
}

// Purpose: Prints a compact heap triplet that can be compared at event boundaries.
void diagnosticPrintHeapTriplet() {
  Serial.print("heap="); Serial.print(ESP.getFreeHeap());
  Serial.print(" min="); Serial.print(ESP.getMinFreeHeap());
  Serial.print(" largest="); Serial.print(diagnosticLargestFreeBlock());
}

// Purpose: Emits one compact state snapshot without requiring the web interface.
void printDiagnosticSnapshot() {
  diagnosticPrefix("STATUS");
  Serial.print("up="); Serial.print(millis());
  Serial.print(" wifiScan="); Serial.print(wifiScanInProgress ? 1 : 0);
  Serial.print(" bleScan="); Serial.print(bleDiagnosticScanActive ? 1 : 0);
  Serial.print(" wifiObs="); Serial.print(historyCount);
  Serial.print("/"); Serial.print(scanHistoryRetentionLimit);
  Serial.print(" bleObs="); Serial.print(bleHistoryCount);
  Serial.print("/"); Serial.print(bleHistoryRetentionLimit);
  Serial.print(" bleScans="); Serial.print(bleScanCounter);
  Serial.print(" sta="); Serial.print(WiFi.status() == WL_CONNECTED ? 1 : 0);
  Serial.print(" loopGapLast="); Serial.print(diagnosticLastLoopGapMs);
  Serial.print(" loopGapMax="); Serial.print(diagnosticMaxLoopGapMs);
  Serial.print(" ");
  diagnosticPrintHeapTriplet();
  Serial.println();
  if (diagnosticStreamingEnabled) {
    recordDiagnosticEvent("STATUS", "wifiObs=" + String(historyCount) + "/" + String(scanHistoryRetentionLimit) + " bleObs=" + String(bleHistoryCount) + "/" + String(bleHistoryRetentionLimit) + " free=" + String(ESP.getFreeHeap()));
  }
}

// Purpose: Emits periodic diagnostic snapshots at the configured runtime interval.
void serviceDiagnosticSnapshot() {
  if (!diagnosticStreamingEnabled || diagnosticSnapshotIntervalMs == 0) return;
  uint32_t now = millis();
  if ((uint32_t)(now - diagnosticLastSnapshotMs) < diagnosticSnapshotIntervalMs) return;
  diagnosticLastSnapshotMs = now;
  printDiagnosticSnapshot();
}

// Purpose: Records long main-loop service gaps, including gaps caused by synchronous BLE scans.
void serviceLoopGapDiagnostics() {
  uint32_t now = millis();
  if (diagnosticLastLoopEntryMs != 0) {
    uint32_t gap = (uint32_t)(now - diagnosticLastLoopEntryMs);
    diagnosticLastLoopGapMs = gap;
    if (gap > diagnosticMaxLoopGapMs) diagnosticMaxLoopGapMs = gap;
    if (gap >= 100) {
      diagnosticLoopGapOver100MsCount++;
      if (diagnosticStreamingEnabled && diagnosticSchedulerEvents) {
        diagnosticPrefix("LOOP GAP");
        Serial.print("duration="); Serial.print(gap); Serial.print("ms");
        Serial.print(" bleActive="); Serial.print(bleDiagnosticScanActive ? 1 : 0);
        Serial.print(" afterBle="); Serial.print(bleDiagnosticLastScanEndMs != 0 && (uint32_t)(now - bleDiagnosticLastScanEndMs) < 100 ? 1 : 0);
        Serial.print(" wifiActive="); Serial.print(wifiScanInProgress ? 1 : 0);
        Serial.println();
        recordDiagnosticEvent("LOOP GAP", "duration=" + String(gap) + "ms bleActive=" + String(bleDiagnosticScanActive ? 1 : 0) + " wifiActive=" + String(wifiScanInProgress ? 1 : 0));
      }
    }
  }
  diagnosticLastLoopEntryMs = now;
}

// Purpose: Runs a registered HTTP handler while measuring page/API generation time for developer diagnostics.
void runDiagnosticWebHandler(const char* route, void (*handler)()) {
  uint32_t startMs = millis();
  if (diagnosticStreamingEnabled && diagnosticWebEvents) {
    diagnosticPrefix("HTTP START");
    Serial.print("route="); Serial.print(route);
    Serial.print(" bleScan="); Serial.print(bleDiagnosticScanActive ? 1 : 0);
    Serial.print(" "); diagnosticPrintHeapTriplet(); Serial.println();
  }

  handler();

  uint32_t durationMs = (uint32_t)(millis() - startMs);
  diagnosticWebHandlerCount++;
  diagnosticWebLastDurationMs = durationMs;
  if (durationMs > diagnosticWebMaxDurationMs) diagnosticWebMaxDurationMs = durationMs;
  if (durationMs >= DIAGNOSTIC_WEB_SLOW_MS) diagnosticWebSlowHandlerCount++;
  if (diagnosticStreamingEnabled && diagnosticWebEvents) {
    diagnosticPrefix("HTTP END");
    Serial.print("route="); Serial.print(route);
    Serial.print(" duration="); Serial.print(durationMs); Serial.print("ms");
    if (durationMs >= DIAGNOSTIC_WEB_SLOW_MS) Serial.print(" SLOW");
    Serial.print(" "); diagnosticPrintHeapTriplet(); Serial.println();
    recordDiagnosticEvent("HTTP", String(route) + " " + String(durationMs) + "ms" + (durationMs >= DIAGNOSTIC_WEB_SLOW_MS ? " SLOW" : ""));
  }
}

struct WebResponseProfile {
  bool active = false;
  const char* route = "";
  uint32_t startMs = 0;
  uint32_t phaseStartMs = 0;
  uint32_t sendCalls = 0;
  size_t sendBytes = 0;
  uint32_t sendTimeMs = 0;
  uint32_t maxSendMs = 0;
  size_t maxSendBytes = 0;
  uint32_t slowSendCount = 0;
  uint32_t phaseMaxSendMs = 0;
  size_t phaseMaxSendBytes = 0;
  uint32_t phaseSlowSendCount = 0;
  uint32_t phaseSendCalls = 0;
  size_t phaseSendBytes = 0;
  uint32_t phaseSendTimeMs = 0;
};

WebResponseProfile webResponseProfile;

struct WebWorkTiming {
  const char* label = "";
  uint32_t durationMs = 0;
};

const size_t WEB_WORK_TIMING_CAPACITY = 24;
WebWorkTiming webWorkTimings[WEB_WORK_TIMING_CAPACITY];
size_t webWorkTimingCount = 0;

// Purpose: Records a measured handler operation without printing inside the timed page phase.
void recordWebWorkTiming(const char* label, uint32_t startMs) {
  if (!webResponseProfile.active || webWorkTimingCount >= WEB_WORK_TIMING_CAPACITY) return;
  webWorkTimings[webWorkTimingCount].label = label;
  webWorkTimings[webWorkTimingCount].durationMs = (uint32_t)(millis() - startMs);
  webWorkTimingCount++;
}

const size_t WEB_RESPONSE_BUFFER_FLUSH_BYTES = 4096;
String webResponseBuffer;
bool webResponseBuffering = false;

// Purpose: Accounts for one actual network write while preserving page-response timing diagnostics.
void sendProfiledContentNow(const String& content) {
  uint32_t startMs = millis();
  server.sendContent(content);
  uint32_t durationMs = (uint32_t)(millis() - startMs);
  if (!webResponseProfile.active) return;

  webResponseProfile.sendCalls++;
  webResponseProfile.sendBytes += content.length();
  webResponseProfile.sendTimeMs += durationMs;
  if (durationMs > webResponseProfile.maxSendMs) {
    webResponseProfile.maxSendMs = durationMs;
    webResponseProfile.maxSendBytes = content.length();
  }
  if (durationMs > webResponseProfile.phaseMaxSendMs) {
    webResponseProfile.phaseMaxSendMs = durationMs;
    webResponseProfile.phaseMaxSendBytes = content.length();
  }
  if (durationMs >= DIAGNOSTIC_WEB_SLOW_MS) {
    webResponseProfile.slowSendCount++;
    webResponseProfile.phaseSlowSendCount++;
  }
}

// Purpose: Flushes accumulated response text as one moderate synchronous network write.
void flushDiagnosticWebResponseBuffer() {
  if (!webResponseBuffering || webResponseBuffer.length() == 0) return;
  sendProfiledContentNow(webResponseBuffer);
  webResponseBuffer.remove(0);
}

// Purpose: Appends bytes while keeping individual network writes near the configured response-chunk size.
void appendDiagnosticWebResponseBytes(const char* data, size_t length) {
  if (!webResponseBuffering) {
    if (length == 0) return;
    String direct;
    direct.reserve(length + 1);
    direct.concat(data, length);
    server.sendContent(direct);
    return;
  }

  while (length > 0) {
    size_t used = webResponseBuffer.length();
    size_t room = used < WEB_RESPONSE_BUFFER_FLUSH_BYTES
      ? WEB_RESPONSE_BUFFER_FLUSH_BYTES - used
      : 0;
    if (room == 0) {
      flushDiagnosticWebResponseBuffer();
      room = WEB_RESPONSE_BUFFER_FLUSH_BYTES;
    }

    size_t take = length < room ? length : room;
    webResponseBuffer.concat(data, take);
    data += take;
    length -= take;

    if (webResponseBuffer.length() >= WEB_RESPONSE_BUFFER_FLUSH_BYTES)
      flushDiagnosticWebResponseBuffer();
  }
}

// Purpose: Starts buffered response output and detailed timing for a major browser response.
void beginWebResponseProfile(const char* route) {
  webResponseProfile = WebResponseProfile();
  webResponseProfile.active = diagnosticStreamingEnabled && diagnosticWebEvents;
  webResponseProfile.route = route;
  webWorkTimingCount = 0;
  webResponseBuffer.remove(0);
  webResponseBuffer.reserve(WEB_RESPONSE_BUFFER_FLUSH_BYTES + 128);
  webResponseBuffering = true;

  if (!webResponseProfile.active) return;
  webResponseProfile.startMs = millis();
  diagnosticPrefix("PAGE START");
  Serial.print("route="); Serial.print(route);
  Serial.print(" "); diagnosticPrintHeapTriplet(); Serial.println();
  webResponseProfile.startMs = millis();
  webResponseProfile.phaseStartMs = webResponseProfile.startMs;
}

// Purpose: Buffers response text so small logical fragments become moderate network writes.
void diagnosticSendContent(const String& content) {
  appendDiagnosticWebResponseBytes(content.c_str(), content.length());
}

// Purpose: Buffers literal response text without creating a temporary String for each call.
void diagnosticSendContent(const char* content) {
  appendDiagnosticWebResponseBytes(content, strlen(content));
}

// Purpose: Records elapsed work and send activity for one logical response-generation phase.
void markWebResponsePhase(const char* phase) {
  if (webResponseProfile.active)
    flushDiagnosticWebResponseBuffer();
  if (!webResponseProfile.active) return;
  uint32_t now = millis();
  uint32_t elapsedMs = (uint32_t)(now - webResponseProfile.phaseStartMs);
  uint32_t sendTimeMs = webResponseProfile.sendTimeMs - webResponseProfile.phaseSendTimeMs;
  uint32_t otherMs = elapsedMs >= sendTimeMs ? elapsedMs - sendTimeMs : 0;
  uint32_t sendCalls = webResponseProfile.sendCalls - webResponseProfile.phaseSendCalls;
  size_t sendBytes = webResponseProfile.sendBytes - webResponseProfile.phaseSendBytes;
  diagnosticPrefix("PAGE PHASE");
  Serial.print("route="); Serial.print(webResponseProfile.route);
  Serial.print(" phase="); Serial.print(phase);
  Serial.print(" elapsed="); Serial.print(elapsedMs); Serial.print("ms");
  Serial.print(" sendTime="); Serial.print(sendTimeMs); Serial.print("ms");
  Serial.print(" other="); Serial.print(otherMs); Serial.print("ms");
  Serial.print(" sends="); Serial.print(sendCalls);
  Serial.print(" bytes="); Serial.print(sendBytes);
  Serial.print(" maxSend="); Serial.print(webResponseProfile.phaseMaxSendMs); Serial.print("ms");
  Serial.print(" maxBytes="); Serial.print(webResponseProfile.phaseMaxSendBytes);
  Serial.print(" slowSends="); Serial.print(webResponseProfile.phaseSlowSendCount);
  Serial.print(" "); diagnosticPrintHeapTriplet(); Serial.println();
  for (size_t i = 0; i < webWorkTimingCount; i++) {
    diagnosticPrefix("PAGE WORK");
    Serial.print("route="); Serial.print(webResponseProfile.route);
    Serial.print(" phase="); Serial.print(phase);
    Serial.print(" item="); Serial.print(webWorkTimings[i].label);
    Serial.print(" elapsed="); Serial.print(webWorkTimings[i].durationMs); Serial.println("ms");
  }
  webWorkTimingCount = 0;
  webResponseProfile.phaseStartMs = millis();
  webResponseProfile.phaseMaxSendMs = 0;
  webResponseProfile.phaseMaxSendBytes = 0;
  webResponseProfile.phaseSlowSendCount = 0;
  webResponseProfile.phaseSendCalls = webResponseProfile.sendCalls;
  webResponseProfile.phaseSendBytes = webResponseProfile.sendBytes;
  webResponseProfile.phaseSendTimeMs = webResponseProfile.sendTimeMs;
}

// Purpose: Finishes detailed response profiling and prints aggregate construction-versus-send timing.
void endWebResponseProfile() {
  if (!webResponseProfile.active) {
    flushDiagnosticWebResponseBuffer();
    webResponseBuffering = false;
    webResponseBuffer.remove(0);
    return;
  }

  markWebResponsePhase("finish");
  uint32_t totalMs = (uint32_t)(millis() - webResponseProfile.startMs);
  uint32_t otherMs = totalMs >= webResponseProfile.sendTimeMs ? totalMs - webResponseProfile.sendTimeMs : 0;
  diagnosticPrefix("PAGE SUMMARY");
  Serial.print("route="); Serial.print(webResponseProfile.route);
  Serial.print(" total="); Serial.print(totalMs); Serial.print("ms");
  Serial.print(" sendTime="); Serial.print(webResponseProfile.sendTimeMs); Serial.print("ms");
  Serial.print(" other="); Serial.print(otherMs); Serial.print("ms");
  Serial.print(" sends="); Serial.print(webResponseProfile.sendCalls);
  Serial.print(" bytes="); Serial.print(webResponseProfile.sendBytes);
  Serial.print(" maxSend="); Serial.print(webResponseProfile.maxSendMs); Serial.print("ms");
  Serial.print(" maxBytes="); Serial.print(webResponseProfile.maxSendBytes);
  Serial.print(" slowSends="); Serial.print(webResponseProfile.slowSendCount);
  Serial.print(" "); diagnosticPrintHeapTriplet(); Serial.println();
  recordDiagnosticEvent("PAGE SUMMARY", String(webResponseProfile.route) + " total=" + String(totalMs) + "ms send=" + String(webResponseProfile.sendTimeMs) + "ms other=" + String(otherMs) + "ms maxSend=" + String(webResponseProfile.maxSendMs) + "ms");
  webResponseProfile.active = false;
  webResponseBuffering = false;
  webResponseBuffer.remove(0);
}

// Purpose: Prints accumulated BLE timing, loop-gap, HTTP, and heap diagnostics as one serial report.
void printDeveloperDiagnosticSummary() {
  Serial.println();
  Serial.println("============================================================");
  Serial.println(" Developer Diagnostics");
  Serial.println("============================================================");
  Serial.print("Diagnostic streaming:  "); Serial.println(diagnosticStreamingEnabled ? "ON" : "OFF");
  Serial.print("Periodic snapshot:     ");
  if (diagnosticSnapshotIntervalMs == 0) Serial.println("OFF");
  else { Serial.print(diagnosticSnapshotIntervalMs / 1000); Serial.println(" s"); }
  Serial.print("Recent events retained:"); Serial.print(" "); Serial.print(diagnosticEventCount); Serial.print(" / "); Serial.println(DIAGNOSTIC_EVENT_CAPACITY);
  Serial.print("Events in export:      "); Serial.println(diagnosticExportEventLimit);
  Serial.print("Last serial RX:         "); Serial.println(lastSerialCommand);
  Serial.print("Categories:             Survey="); Serial.print(diagnosticSurveyEvents ? "ON" : "OFF");
  Serial.print(" BLE="); Serial.print(diagnosticBleEvents ? "ON" : "OFF");
  Serial.print(" Memory="); Serial.print(diagnosticMemoryEvents ? "ON" : "OFF");
  Serial.print(" Web="); Serial.print(diagnosticWebEvents ? "ON" : "OFF");
  Serial.print(" Scheduler="); Serial.print(diagnosticSchedulerEvents ? "ON" : "OFF");
  Serial.print(" Checkpoint="); Serial.println(diagnosticCheckpointEvents ? "ON" : "OFF");

  Serial.println();
  Serial.println("BLE timing:");
  Serial.print("  Completed timed scans: "); Serial.println(bleDiagnosticDurationCount);
  Serial.print("  Last scan-start API:   "); Serial.print(bleDiagnosticLastApiDurationMs); Serial.println(" ms");
  Serial.print("  Last result processing:"); Serial.print(" "); Serial.print(bleDiagnosticLastProcessingDurationMs); Serial.println(" ms");
  Serial.print("  Last total:            "); Serial.print(bleDiagnosticLastTotalDurationMs); Serial.println(" ms");
  Serial.print("  Min / Avg / Max total: ");
  if (bleDiagnosticDurationCount == 0) Serial.println("n/a");
  else {
    Serial.print(bleDiagnosticMinDurationMs); Serial.print(" / ");
    Serial.print((uint32_t)(bleDiagnosticTotalDurationMs / bleDiagnosticDurationCount)); Serial.print(" / ");
    Serial.print(bleDiagnosticMaxDurationMs); Serial.println(" ms");
  }
  Serial.print("  Last result count:     "); Serial.println(bleDiagnosticLastResultCount);
  Serial.print("  Last capture overflow: "); Serial.println(bleDiagnosticLastCaptureDrops);
  Serial.print("  Last completion reason:"); Serial.print(" "); Serial.println(bleDiagnosticLastCompletionReason);

  Serial.println();
  Serial.println("Last BLE heap boundaries (bytes):");
  Serial.print("  Before scan:           free="); Serial.print(bleDiagnosticLastHeapBefore); Serial.print(" largest="); Serial.println(bleDiagnosticLastLargestBefore);
  Serial.print("  At scan completion:    free="); Serial.print(bleDiagnosticLastHeapAfterApi); Serial.print(" largest="); Serial.println(bleDiagnosticLastLargestAfterApi);
  Serial.print("  After processing:      free="); Serial.print(bleDiagnosticLastHeapAfterProcessing); Serial.print(" largest="); Serial.println(bleDiagnosticLastLargestAfterProcessing);
  Serial.print("  Current minimum heap:  "); Serial.println(ESP.getMinFreeHeap());

  Serial.println();
  Serial.println("Loop/Web timing:");
  Serial.print("  Last / max loop gap:   "); Serial.print(diagnosticLastLoopGapMs); Serial.print(" / "); Serial.print(diagnosticMaxLoopGapMs); Serial.println(" ms");
  Serial.print("  Loop gaps >=100 ms:    "); Serial.println(diagnosticLoopGapOver100MsCount);
  Serial.print("  HTTP handlers timed:   "); Serial.println(diagnosticWebHandlerCount);
  Serial.print("  Last / max HTTP time:  "); Serial.print(diagnosticWebLastDurationMs); Serial.print(" / "); Serial.print(diagnosticWebMaxDurationMs); Serial.println(" ms");
  Serial.print("  HTTP handlers >=100 ms:"); Serial.print(" "); Serial.println(diagnosticWebSlowHandlerCount);

  Serial.println();
  Serial.println("Developer commands:");
  Serial.println("  diag on|off            - Master diagnostic streaming switch");
  Serial.println("  diag snapshot          - Print one state/heap snapshot now");
  Serial.println("  diag summary           - Print accumulated timing diagnostics");
  Serial.println("  diag interval <sec>    - Periodic snapshot interval; 0 disables");
  Serial.println("  diag survey on|off     - Survey lifecycle events");
  Serial.println("  diag ble on|off        - BLE scan/init/timing events");
  Serial.println("  diag memory on|off     - Heap boundaries in event logs");
  Serial.println("  diag web on|off        - HTTP handler timing");
  Serial.println("  diag scheduler on|off  - Scheduler/loop-gap events");
  Serial.println("  diag checkpoint on|off - Restart-checkpoint events");
  Serial.println("  diag reset             - Reset accumulated timing counters");
  Serial.println("  0/back                 - Main menu");
  Serial.println();
  Serial.print("> ");
}

// Purpose: Clears accumulated timing counters without changing logging category selections.
void resetDeveloperDiagnostics() {
  bleDiagnosticLastApiDurationMs = 0;
  bleDiagnosticLastProcessingDurationMs = 0;
  bleDiagnosticLastTotalDurationMs = 0;
  bleDiagnosticDurationCount = 0;
  bleDiagnosticTotalDurationMs = 0;
  bleDiagnosticMinDurationMs = UINT32_MAX;
  bleDiagnosticMaxDurationMs = 0;
  bleDiagnosticLastResultCount = 0;
  bleDiagnosticLastHeapBefore = 0;
  bleDiagnosticLastHeapAfterApi = 0;
  bleDiagnosticLastCaptureDrops = 0;
  bleDiagnosticLastCompletionReason = 0;
  bleDiagnosticLastHeapAfterProcessing = 0;
  bleDiagnosticLastLargestBefore = 0;
  bleDiagnosticLastLargestAfterApi = 0;
  bleDiagnosticLastLargestAfterProcessing = 0;
  diagnosticLastLoopGapMs = 0;
  diagnosticMaxLoopGapMs = 0;
  diagnosticLoopGapOver100MsCount = 0;
  diagnosticWebHandlerCount = 0;
  diagnosticWebSlowHandlerCount = 0;
  diagnosticWebLastDurationMs = 0;
  diagnosticWebMaxDurationMs = 0;
  diagnosticEventStart = 0;
  diagnosticEventCount = 0;
}

// Purpose: Logs BLE scheduler state only when the blocking/defer reason changes, avoiding per-loop serial spam.
void diagnosticBleSchedulerState(uint8_t reason, const char* label) {
  if (reason == bleDiagnosticSchedulerReason) return;
  bleDiagnosticSchedulerReason = reason;
  if (!diagnosticStreamingEnabled || !diagnosticSchedulerEvents) return;
  diagnosticPrefix("BLE SCHED");
  Serial.print("state="); Serial.print(label);
  Serial.print(" sinceLast="); Serial.print((uint32_t)(millis() - lastAutoBleScanMs)); Serial.print("ms");
  Serial.print(" interval="); Serial.print(bleScanIntervalSeconds * 1000UL); Serial.println("ms");
  recordDiagnosticEvent("BLE SCHED", String(label) + " sinceLast=" + String((uint32_t)(millis() - lastAutoBleScanMs)) + "ms");
}

// ============================================================
// BLE helpers
// ============================================================

// Purpose: Converts the compact BLE address-type code into a readable Public/Random label.
String bleAddressTypeLabel(uint8_t addressType) {
  switch (addressType) {
    case 0: return "Public";
    case 1: return "Random";
    case 2: return "Public ID";
    case 3: return "Random ID";
    default: return "Unknown";
  }
}

// Purpose: Initializes the BLE stack and scanner only when Bluetooth Survey is enabled at boot.
void initializeBLEScanner() {
  if (!bleSurveyEnabled || bleInitialized) return;

  uint32_t startMs = millis();
  uint32_t heapBefore = ESP.getFreeHeap();
  uint32_t largestBefore = diagnosticLargestFreeBlock();
  if (diagnosticStreamingEnabled && diagnosticBleEvents) {
    diagnosticPrefix("BLE INIT START");
    Serial.print("stack=NimBLE ");
    Serial.print("heap="); Serial.print(heapBefore);
    Serial.print(" largest="); Serial.println(largestBefore);
  }

  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan != nullptr) {
    scan->setScanCallbacks(&surveyNimBLEScanCallbacks, false);
    scan->setActiveScan(true);
    scan->setMaxResults(0);
  }
  bleInitialized = scan != nullptr;

  if (diagnosticStreamingEnabled && diagnosticBleEvents) {
    diagnosticPrefix("BLE INIT END");
    Serial.print("stack=NimBLE ");
    Serial.print("duration="); Serial.print((uint32_t)(millis() - startMs)); Serial.print("ms ");
    diagnosticPrintHeapTriplet();
    Serial.println();
  }
}


// ============================================================
// Wi-Fi compact normalized history
// ============================================================

// Purpose: Returns the total RAM reserved for Wi-Fi compact observations, AP identity table, and scan metadata.
size_t wifiHistoryAllocatedBytes() {
  return
    scanHistoryCapacity * sizeof(WifiObservation) +
    wifiApTableCapacity * sizeof(WifiApEntry) +
    wifiScanMetadataCapacity * sizeof(WifiScanMetadata);
}

// Purpose: Returns a retained compact Wi-Fi observation by logical ring-buffer index.
const WifiObservation& compactHistoryRecord(size_t logicalIndex) {
  size_t physicalIndex =
      (historyStart + logicalIndex) % scanHistoryCapacity;
  return scanHistory[physicalIndex];
}

// Purpose: Formats a six-byte Wi-Fi BSSID as standard colon-separated hexadecimal text.
void formatBssid(const uint8_t bssid[6], char output[18]) {
  snprintf(
    output, 18,
    "%02X:%02X:%02X:%02X:%02X:%02X",
    bssid[0], bssid[1], bssid[2],
    bssid[3], bssid[4], bssid[5]
  );
}

// Purpose: Synthesizes a full Wi-Fi ScanRecord view from the compact observation and lookup tables.
ScanRecord historyRecord(size_t logicalIndex) {
  ScanRecord record = {};
  if (
    scanHistory == nullptr ||
    logicalIndex >= historyCount ||
    wifiApTable == nullptr ||
    wifiScanMetadata == nullptr
  ) return record;

  const WifiObservation& observation = compactHistoryRecord(logicalIndex);
  if (
    observation.apIndex >= wifiApCount ||
    observation.scanSlot >= wifiScanMetadataCapacity
  ) return record;

  const WifiApEntry& ap = wifiApTable[observation.apIndex];
  const WifiScanMetadata& scan = wifiScanMetadata[observation.scanSlot];

  record.scanNumber = scan.scanNumber;
  record.uptimeMs = scan.uptimeMs;
  memcpy(record.ssid, ap.ssid, sizeof(record.ssid));
  formatBssid(ap.bssid, record.bssid);
  record.rssi = observation.rssi;
  record.channel = ap.channel;
  record.authMode = ap.authMode;
  record.hidden = (ap.ssid[0] == '\0');
  record.connected = false;
  return record;
}

// Purpose: Counts distinct retained Wi-Fi scan numbers directly from compact scan metadata.
size_t countRetainedScanGroups() {
  static size_t cachedHistoryStart = SIZE_MAX;
  static size_t cachedHistoryCount = SIZE_MAX;
  static uint32_t cachedScanCounter = UINT32_MAX;
  static size_t cachedGroups = 0;

  if (
    cachedHistoryStart == historyStart &&
    cachedHistoryCount == historyCount &&
    cachedScanCounter == scanCounter
  ) {
    return cachedGroups;
  }

  size_t groups = 0;
  uint32_t previousScan = 0;
  bool havePrevious = false;

  if (scanHistory != nullptr && wifiScanMetadata != nullptr) {
    for (size_t i = 0; i < historyCount; i++) {
      const WifiObservation& observation = compactHistoryRecord(i);
      if (observation.scanSlot >= wifiScanMetadataCapacity) continue;
      uint32_t currentScan = wifiScanMetadata[observation.scanSlot].scanNumber;
      if (!havePrevious || currentScan != previousScan) {
        groups++;
        previousScan = currentScan;
        havePrevious = true;
      }
    }
  }

  cachedHistoryStart = historyStart;
  cachedHistoryCount = historyCount;
  cachedScanCounter = scanCounter;
  cachedGroups = groups;
  return cachedGroups;
}

// Purpose: Evicts the oldest Wi-Fi observation from the ring buffer.
void discardOldestWifiObservation() {
  if (historyCount == 0 || scanHistoryCapacity == 0) return;
  historyStart = (historyStart + 1) % scanHistoryCapacity;
  historyCount--;
}

// Purpose: Removes every retained Wi-Fi observation that references a metadata slot before that slot is recycled.
void discardObservationsForScanSlot(uint16_t scanSlot) {
  if (!scanHistory || historyCount == 0) return;
  size_t kept = 0;
  const size_t originalCount = historyCount;
  for (size_t i = 0; i < originalCount; i++) {
    WifiObservation observation = compactHistoryRecord(i);
    if (observation.scanSlot == scanSlot) continue;
    size_t writeIndex = (historyStart + kept) % scanHistoryCapacity;
    scanHistory[writeIndex] = observation;
    kept++;
  }
  historyCount = kept;
}

// Purpose: Counts retained Wi-Fi observations whose metadata references violate chronological or slot-mapping invariants.
size_t wifiHistoryIntegrityAnomalies() {
  static uint32_t cachedScanCounter = UINT32_MAX;
  static size_t cachedHistoryCount = (size_t)-1;
  static size_t cachedAnomalies = 0;
  if (cachedScanCounter == scanCounter && cachedHistoryCount == historyCount) return cachedAnomalies;
  cachedScanCounter = scanCounter;
  cachedHistoryCount = historyCount;
  if (!wifiScanMetadata || wifiScanMetadataCapacity == 0) { cachedAnomalies = historyCount; return cachedAnomalies; }
  size_t anomalies = 0;
  uint32_t previousUptime = 0;
  uint32_t previousScan = 0;
  bool havePrevious = false;
  for (size_t i = 0; i < historyCount; i++) {
    const WifiObservation& observation = compactHistoryRecord(i);
    if (observation.scanSlot >= wifiScanMetadataCapacity) { anomalies++; continue; }
    const WifiScanMetadata& metadata = wifiScanMetadata[observation.scanSlot];
    if (metadata.scanNumber == 0 || ((metadata.scanNumber - 1) % wifiScanMetadataCapacity) != observation.scanSlot) anomalies++;
    if (havePrevious && (metadata.uptimeMs < previousUptime || metadata.scanNumber < previousScan)) anomalies++;
    previousUptime = metadata.uptimeMs;
    previousScan = metadata.scanNumber;
    havePrevious = true;
  }
  cachedAnomalies = anomalies;
  return cachedAnomalies;
}

// Purpose: Changes the logical Wi-Fi history-retention limit without reallocating the physical buffers.
bool setWifiRetentionLimit(size_t requestedLimit) {
  if (scanHistory == nullptr || scanHistoryCapacity == 0) {
    historyResizeMessage = "Wi-Fi history storage is not allocated.";
    return false;
  }

  if (requestedLimit < MIN_SCAN_HISTORY_RECORDS)
    requestedLimit = MIN_SCAN_HISTORY_RECORDS;
  if (requestedLimit > scanHistoryCapacity)
    requestedLimit = scanHistoryCapacity;

  scanHistoryRetentionLimit = requestedLimit;
  while (historyCount > scanHistoryRetentionLimit)
    discardOldestWifiObservation();

  historyResizeMessage =
    "Retention limit set to " +
    String(scanHistoryRetentionLimit) +
    " observations. Physical capacity remains " +
    String(scanHistoryCapacity) + ".";
  return true;
}

// Compatibility wrapper: the compact history implementation does not reallocate the history buffer at
// runtime. Web changes alter only the logical retention limit.
// Purpose: Compatibility wrapper for changing the Wi-Fi retention limit under the compact fixed-allocation design.
bool resizeScanHistory(size_t requestedCapacity, bool preserveRecords = true) {
  (void)preserveRecords;
  return setWifiRetentionLimit(requestedCapacity);
}

// Purpose: Clears Wi-Fi history and then applies a requested logical retention limit.
bool clearAndResizeScanHistory(size_t requestedCapacity) {
  historyStart = 0;
  historyCount = 0;
  scanCounter = 0;
  lastScanUptimeMs = 0;
  wifiApCount = 0;
  wifiApTableFullDrops = 0;
  if (wifiScanMetadata && wifiScanMetadataCapacity)
    memset(wifiScanMetadata, 0, wifiScanMetadataCapacity * sizeof(WifiScanMetadata));
  return setWifiRetentionLimit(requestedCapacity);
}

// Purpose: Appends one compact Wi-Fi observation, evicting old data as required by the ring-buffer limit.
void appendWifiObservation(const WifiObservation& observation) {
  if (
    scanHistory == nullptr ||
    scanHistoryCapacity == 0 ||
    scanHistoryRetentionLimit == 0
  ) return;

  while (historyCount >= scanHistoryRetentionLimit)
    discardOldestWifiObservation();

  size_t writeIndex =
      (historyStart + historyCount) % scanHistoryCapacity;
  scanHistory[writeIndex] = observation;
  historyCount++;
}

// Purpose: Clears retained Wi-Fi observations and associated scan-history state.
void clearScanHistory() {
  historyStart = 0;
  historyCount = 0;
  scanCounter = 0;
  lastScanUptimeMs = 0;
  wifiApCount = 0;
  wifiApTableFullDrops = 0;
  wifiApReclamationCount = 0;
  if (wifiScanMetadata && wifiScanMetadataCapacity)
    memset(wifiScanMetadata, 0, wifiScanMetadataCapacity * sizeof(WifiScanMetadata));
}

// Purpose: Finds an existing Wi-Fi AP-table entry whose binary BSSID matches the supplied address.
int findWifiApByBssid(const uint8_t bssid[6]) {
  for (size_t i = 0; i < wifiApCount; i++) {
    if (memcmp(wifiApTable[i].bssid, bssid, 6) == 0)
      return (int)i;
  }
  return -1;
}

// Purpose: Checks whether any retained Wi-Fi observation still refers to a given AP-table slot.
bool wifiApIndexIsReferenced(size_t apIndex) {
  for (size_t i = 0; i < historyCount; i++) {
    if (compactHistoryRecord(i).apIndex == apIndex) return true;
  }
  return false;
}

int findOrCreateWifiAp(
  const uint8_t bssid[6],
  const String& ssid,
  uint8_t channel,
  uint8_t authMode,
  bool* created,
  bool* reclaimed
) {
  if (created) *created = false;
  if (reclaimed) *reclaimed = false;
  int existing = findWifiApByBssid(bssid);

  if (existing >= 0) {
    WifiApEntry& ap = wifiApTable[existing];

    if (ap.channel != 0 && ap.channel != channel) {
      char textBssid[18];
      formatBssid(bssid, textBssid);
      Serial.print("Wi-Fi AP channel changed: ");
      Serial.print(textBssid);
      Serial.print(" ");
      Serial.print(ap.channel);
      Serial.print(" -> ");
      Serial.println(channel);
    }

    ssid.toCharArray(ap.ssid, sizeof(ap.ssid));
    ap.channel = channel;
    ap.authMode = authMode;
    ap.lastSeenScanLow = (uint16_t)scanCounter;
    return existing;
  }

  if (wifiApTable == nullptr) {
    wifiApTableFullDrops++;
    return -1;
  }

  size_t targetIndex = wifiApCount;
  if (wifiApCount >= wifiApTableCapacity) {
    targetIndex = wifiApTableCapacity;
    uint16_t oldestAge = 0;
    for (size_t i = 0; i < wifiApTableCapacity; i++) {
      if (!wifiApIndexIsReferenced(i)) {
        uint16_t age = (uint16_t)((uint16_t)scanCounter - wifiApTable[i].lastSeenScanLow);
        if (targetIndex >= wifiApTableCapacity || age > oldestAge) {
          oldestAge = age;
          targetIndex = i;
        }
      }
    }
    if (targetIndex >= wifiApTableCapacity) {
      wifiApTableFullDrops++;
      return -1;
    }
    wifiApReclamationCount++;
    if (reclaimed) *reclaimed = true;
  } else {
    wifiApCount++;
  }

  WifiApEntry& ap = wifiApTable[targetIndex];
  memset(&ap, 0, sizeof(ap));
  ssid.toCharArray(ap.ssid, sizeof(ap.ssid));
  memcpy(ap.bssid, bssid, 6);
  ap.channel = channel;
  ap.authMode = authMode;
  ap.lastSeenScanLow = (uint16_t)scanCounter;
  if (created) *created = true;
  return (int)targetIndex;
}

// Purpose: Allocates the compact Wi-Fi observation ring, AP table, and scan-metadata table within the chosen RAM budget.
bool initializeCompactWifiHistory(size_t budgetBytes, size_t apCapacity, size_t scanCapacity) {

  size_t metadataBytes =
      apCapacity * sizeof(WifiApEntry) +
      scanCapacity * sizeof(WifiScanMetadata);

  size_t observationCapacity = MIN_SCAN_HISTORY_RECORDS;
  if (budgetBytes > metadataBytes) {
    observationCapacity =
        (budgetBytes - metadataBytes) / sizeof(WifiObservation);
    if (observationCapacity < MIN_SCAN_HISTORY_RECORDS)
      observationCapacity = MIN_SCAN_HISTORY_RECORDS;
  }
  if (observationCapacity > MAX_SCAN_HISTORY_RECORDS)
    observationCapacity = MAX_SCAN_HISTORY_RECORDS;

  wifiApTable = (WifiApEntry*)calloc(apCapacity, sizeof(WifiApEntry));
  wifiScanMetadata =
      (WifiScanMetadata*)calloc(scanCapacity, sizeof(WifiScanMetadata));
  scanHistory =
      (WifiObservation*)malloc(observationCapacity * sizeof(WifiObservation));

  if (!wifiApTable || !wifiScanMetadata || !scanHistory) {
    if (wifiApTable) free(wifiApTable);
    if (wifiScanMetadata) free(wifiScanMetadata);
    if (scanHistory) free(scanHistory);
    wifiApTable = nullptr;
    wifiScanMetadata = nullptr;
    scanHistory = nullptr;
    wifiApTableCapacity = 0;
    wifiScanMetadataCapacity = 0;
    scanHistoryCapacity = 0;
    scanHistoryRetentionLimit = 0;
    return false;
  }

  wifiApTableCapacity = apCapacity;
  wifiScanMetadataCapacity = scanCapacity;
  scanHistoryCapacity = observationCapacity;
  scanHistoryRetentionLimit = observationCapacity;
  historyStart = 0;
  historyCount = 0;
  wifiApCount = 0;
  return true;
}

bool wifiInitialScanCheckpointPending = false;
String wifiScanStatusMessage = "Idle";

// Saved infrastructure Wi-Fi reconnects opportunistically from survey results.
// No extra scan is started just for connectivity recovery.
bool infrastructureReconnectPending = false;
bool infrastructureReconnectAttemptActive = false;
bool infrastructureReconnectAttempted = false;
uint32_t infrastructureReconnectAttemptStartedMs = 0;
uint32_t lastInfrastructureReconnectAttemptMs = 0;
uint32_t infrastructureReconnectAttemptCount = 0;
uint32_t infrastructureReconnectSuccessCount = 0;

bool loadCredentials(String& ssid, String& password);
void considerInfrastructureReconnectAfterScan(int networkCount);
void serviceNativeReconnectDiagnostics();

// Purpose: Converts a completed ESP32 Wi-Fi scan into compact retained observations and updates scan state.
int processCompletedWifiScan(int networkCount) {
  scanCounter++;
  lastScanUptimeMs = surveySessionUptimeMs();
  wifiScanCompletedSinceBoot = true;

  if (wifiScanMetadata == nullptr || wifiScanMetadataCapacity == 0)
    return networkCount;

  uint16_t scanSlot =
      (uint16_t)((scanCounter - 1) % wifiScanMetadataCapacity);

  if (
    wifiScanMetadata[scanSlot].scanNumber != 0 &&
    wifiScanMetadata[scanSlot].scanNumber != scanCounter
  ) {
    discardObservationsForScanSlot(scanSlot);
  }

  wifiScanMetadata[scanSlot].scanNumber = scanCounter;
  wifiScanMetadata[scanSlot].uptimeMs = lastScanUptimeMs;

  wifiLastScanFound = networkCount > 0 ? (uint16_t)networkCount : 0;
  wifiLastScanLogged = 0;
  wifiLastScanDropped = 0;
  wifiLastScanNewAps = 0;
  wifiLastScanPreviouslySeenAps = 0;
  wifiLastScanHiddenSkipped = 0;
  wifiLastScanReclaimedAps = 0;
  wifiLatestAnalysisScan = scanCounter;
  memset(wifiLatestChannelApCount, 0, sizeof(wifiLatestChannelApCount));
  memset(wifiLatestCoChannelScore, 0, sizeof(wifiLatestCoChannelScore));
  memset(wifiLatestAdjacentScore, 0, sizeof(wifiLatestAdjacentScore));
  for (int ch = 0; ch < 12; ch++) wifiLatestChannelStrongestRssi[ch] = -127;

  if (networkCount <= 0) return networkCount;

  for (int i = 0; i < networkCount; i++) {
    const uint8_t* rawBssid = WiFi.BSSID(i);
    if (rawBssid == nullptr) continue;

    String ssid = WiFi.SSID(i);
    int channel = WiFi.channel(i);
    int rssi = WiFi.RSSI(i);
    if (channel >= 1 && channel <= 11) {
      wifiLatestChannelApCount[channel]++;
      if (rssi > wifiLatestChannelStrongestRssi[channel]) wifiLatestChannelStrongestRssi[channel] = rssi;
      float w = rssiInterferenceWeight(rssi);
      for (int candidate = 1; candidate <= 11; candidate++) {
        int d = abs(candidate - channel);
        float overlap = channelOverlapFactor(d);
        if (d == 0) wifiLatestCoChannelScore[candidate] += w;
        else if (overlap > 0.0f) wifiLatestAdjacentScore[candidate] += w * overlap;
      }
    }

    if (ssid.length() == 0 && !captureHiddenNetworks) {
      wifiLastScanHiddenSkipped++;
      continue;
    }

    // Make the ring slot available before AP allocation. This can release the
    // final reference to an old AP identity, allowing that identity slot to be
    // reclaimed for the incoming BSSID without ever retargeting live history.
    while (historyCount >= scanHistoryRetentionLimit)
      discardOldestWifiObservation();

    bool created = false;
    bool reclaimed = false;
    int apIndex = findOrCreateWifiAp(
      rawBssid,
      ssid,
      (uint8_t)channel,
      (uint8_t)WiFi.encryptionType(i),
      &created,
      &reclaimed
    );

    if (apIndex < 0) { wifiLastScanDropped++; continue; }
    if (created) wifiLastScanNewAps++;
    else wifiLastScanPreviouslySeenAps++;
    if (reclaimed) wifiLastScanReclaimedAps++;

    if (rssi < -128) rssi = -128;
    if (rssi > 127) rssi = 127;

    WifiObservation observation = {};
    observation.apIndex = (uint16_t)apIndex;
    observation.scanSlot = scanSlot;
    observation.rssi = (int8_t)rssi;
    appendWifiObservation(observation);
    wifiLastScanLogged++;
  }

  return networkCount;
}

// Purpose: Adds one successful Wi-Fi scan duration to the last/min/max/average statistics.
void recordWifiScanDuration(uint32_t durationMs) {
  wifiLastScanDurationMs = durationMs;
  wifiTotalScanDurationMs += durationMs;
  wifiScanDurationCount++;

  if (wifiScanDurationCount == 1 || durationMs < wifiMinScanDurationMs)
    wifiMinScanDurationMs = durationMs;
  if (durationMs > wifiMaxScanDurationMs)
    wifiMaxScanDurationMs = durationMs;
}

// Purpose: Returns the arithmetic mean duration of successful Wi-Fi scans recorded this session.
uint32_t wifiAverageScanDurationMs() {
  if (wifiScanDurationCount == 0) return 0;
  return (uint32_t)(wifiTotalScanDurationMs / wifiScanDurationCount);
}

// Purpose: Records an automatic Wi-Fi scan-start failure and arms the retry-backoff state.
void noteAutomaticScanFailure() {
  wifiAutoScanRetryPending = true;
  lastWifiAutoScanFailureMs = millis();
}

// Purpose: Legacy synchronous wrapper that starts a logged Wi-Fi scan and waits for its completion.
int performLoggedScan() {
  ensureWiFiStationMode();

  startScanLed(WIFI_SCAN_LED_PERIOD_TICKS);
  uint32_t scanStartMs = millis();
  int networkCount = WiFi.scanNetworks(false, true);
  uint32_t scanDurationMs = millis() - scanStartMs;
  stopScanLed();

  int result = processCompletedWifiScan(networkCount);
  if (networkCount >= 0) recordWifiScanDuration(scanDurationMs);
  lastAutoScanMs = millis();
  wifiAutoScanRetryPending = false;
  wifiScanStatusMessage = "Complete";
  return result;
}

// Purpose: Starts an asynchronous Wi-Fi scan when scheduling and resource conditions permit it.
bool beginLoggedWifiScan(bool initialCheckpoint, bool automaticTrigger) {
  if (wifiScanInProgress) return false;

  ensureWiFiStationMode();
  WiFi.scanDelete();

  int result = WiFi.scanNetworks(true, true);
  if (result == WIFI_SCAN_FAILED) {
    wifiScanStatusMessage = "Failed to start";
    if (automaticTrigger) {
      wifiAutoScanStartFailureCount++;
      noteAutomaticScanFailure();
    }
    return false;
  }

  wifiCurrentScanAutomatic = automaticTrigger;
  wifiCurrentScanStartMs = millis();
  if (automaticTrigger) {
    wifiAutoScanStartCount++;
    lastWifiAutoScanStartMs = wifiCurrentScanStartMs;
    wifiAutoScanRetryPending = false;
  }

  wifiScanInProgress = true;
  wifiInitialScanCheckpointPending =
      wifiInitialScanCheckpointPending || initialCheckpoint;
  wifiScanStatusMessage = "Scanning";
  startScanLed(WIFI_SCAN_LED_PERIOD_TICKS);
  return true;
}

// Purpose: Polls an in-progress asynchronous Wi-Fi scan and processes it when the ESP32 reports completion.
void serviceLoggedWifiScan() {
  if (!wifiScanInProgress) return;

  int result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) {
    uint32_t elapsedMs = millis() - wifiCurrentScanStartMs;
    if (elapsedMs < WIFI_SCAN_WATCHDOG_MS) return;
    stopScanLed();
    WiFi.scanDelete();
    wifiScanInProgress = false;
    wifiScanStatusMessage = "Timed out; recovery scheduled";
    wifiInitialScanCheckpointPending = false;
    recordDiagnosticEvent("WIFI TIMEOUT", "elapsed=" + String(elapsedMs) + "ms auto=" + String(wifiCurrentScanAutomatic ? "yes" : "no"));
    if (wifiCurrentScanAutomatic) {
      wifiAutoScanCompletionFailureCount++;
      noteAutomaticScanFailure();
    }
    wifiCurrentScanAutomatic = false;
    return;
  }

  uint32_t scanDurationMs = millis() - wifiCurrentScanStartMs;
  stopScanLed();
  wifiScanInProgress = false;

  if (result == WIFI_SCAN_FAILED) {
    wifiScanStatusMessage = "Scan failed";
    WiFi.scanDelete();
    wifiInitialScanCheckpointPending = false;
    if (wifiCurrentScanAutomatic) {
      wifiAutoScanCompletionFailureCount++;
      noteAutomaticScanFailure();
    }
    wifiCurrentScanAutomatic = false;
    return;
  }

  processCompletedWifiScan(result);
  considerInfrastructureReconnectAfterScan(result);
  recordWifiScanDuration(scanDurationMs);
  if (wifiCurrentScanAutomatic) {
    wifiAutoScanCompletionCount++;
    lastWifiAutoScanCompletionMs = millis();
  }
  wifiScanStatusMessage = "Complete";
  if (scanDurationMs >= 15000 || (diagnosticStreamingEnabled && diagnosticSurveyEvents))
    recordDiagnosticEvent("WIFI SCAN", "seq=" + String(scanCounter) + " dur=" + String(scanDurationMs) + "ms found=" + String(wifiLastScanFound) + " log=" + String(wifiLastScanLogged) + " drop=" + String(wifiLastScanDropped));
  WiFi.scanDelete();
  lastAutoScanMs = millis();
  wifiAutoScanRetryPending = false;

  if (wifiInitialScanCheckpointPending) {
    captureBootHeapCheckpoint("Initial Wi-Fi scan");
    wifiInitialScanCheckpointPending = false;
  }

  wifiCurrentScanAutomatic = false;
}

// ============================================================
// BLE compact normalized history
// ============================================================

// Purpose: Returns the total RAM reserved for BLE observations, address identities, and scan metadata.
size_t bleHistoryAllocatedBytes() {
  return
    bleHistoryCapacity * sizeof(BleObservation) +
    bleAddressTableCapacity * sizeof(BleAddressEntry) +
    bleScanMetadataCapacity * sizeof(SurveyScanMetadata);
}

// Purpose: Returns a retained compact BLE observation by logical ring-buffer index.
const BleObservation& compactBleHistoryRecord(size_t logicalIndex) {
  size_t physicalIndex =
      (bleHistoryStart + logicalIndex) % bleHistoryCapacity;
  return bleHistory[physicalIndex];
}

// Purpose: Parses colon-separated BLE address text into its six-byte binary representation.
bool parseBleAddress(const String& text, uint8_t output[6]) {
  unsigned int b[6];
  if (sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x",
      &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
  for (int i = 0; i < 6; i++) output[i] = (uint8_t)b[i];
  return true;
}


// Purpose: Copies one NimBLE advertisement into the bounded scan buffer without retaining NimBLE result objects.
void captureNimBleAdvertisement(const NimBLEAdvertisedDevice* advertisedDevice) {
  if (advertisedDevice == nullptr || !bleDiagnosticScanActive) return;

  std::string addressStd = advertisedDevice->getAddress().toString();
  String addressText(addressStd.c_str());
  uint8_t rawAddress[6];
  if (!parseBleAddress(addressText, rawAddress)) return;

  NimBleCapturedAdvertisement captured = {};
  memcpy(captured.address, rawAddress, sizeof(captured.address));
  captured.addressType = advertisedDevice->getAddressType();
  captured.rssi = advertisedDevice->getRSSI();
  if (advertisedDevice->haveName()) {
    std::string nameStd = advertisedDevice->getName();
    strncpy(captured.name, nameStd.c_str(), sizeof(captured.name) - 1);
    captured.name[sizeof(captured.name) - 1] = '\0';
  }

  portENTER_CRITICAL(&bleScanCaptureMux);
  for (uint16_t i = 0; i < bleScanCaptureCount; i++) {
    if (bleScanCapture[i].addressType == captured.addressType &&
        memcmp(bleScanCapture[i].address, captured.address, sizeof(captured.address)) == 0) {
      bleScanCapture[i].rssi = captured.rssi;
      if (captured.name[0] != '\0') {
        strncpy(bleScanCapture[i].name, captured.name, sizeof(bleScanCapture[i].name) - 1);
        bleScanCapture[i].name[sizeof(bleScanCapture[i].name) - 1] = '\0';
      }
      portEXIT_CRITICAL(&bleScanCaptureMux);
      return;
    }
  }

  if (bleScanCaptureCount < BLE_SCAN_CAPTURE_CAPACITY) {
    bleScanCapture[bleScanCaptureCount] = captured;
    bleScanCaptureCount++;
  } else {
    bleScanCaptureDrops++;
  }
  portEXIT_CRITICAL(&bleScanCaptureMux);
}

// Purpose: Formats a six-byte BLE address as colon-separated hexadecimal text.
void formatBleAddress(const uint8_t address[6], char output[18]) {
  snprintf(output, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
    address[0], address[1], address[2], address[3], address[4], address[5]);
}

// Purpose: Synthesizes a full BLE ScanRecord view from compact observation and lookup-table data.
BleScanRecord bleHistoryRecord(size_t logicalIndex) {
  BleScanRecord record = {};
  if (
    bleHistory == nullptr || logicalIndex >= bleHistoryCount ||
    bleAddressTable == nullptr || bleScanMetadata == nullptr
  ) return record;

  const BleObservation& observation = compactBleHistoryRecord(logicalIndex);
  if (
    observation.addressIndex >= bleAddressTableCapacity ||
    observation.scanSlot >= bleScanMetadataCapacity
  ) return record;

  const BleAddressEntry& entry = bleAddressTable[observation.addressIndex];
  const SurveyScanMetadata& scan = bleScanMetadata[observation.scanSlot];
  record.scanNumber = scan.scanNumber;
  record.uptimeMs = scan.uptimeMs;
  memcpy(record.name, entry.name, sizeof(record.name));
  formatBleAddress(entry.address, record.address);
  record.rssi = observation.rssi;
  record.addressType = entry.addressType;
  record.named = entry.name[0] != '\0';
  return record;
}

// Purpose: Checks whether any retained BLE observation still references a given address-table slot.
bool bleAddressIndexIsReferenced(size_t addressIndex) {
  for (size_t i = 0; i < bleHistoryCount; i++) {
    if (compactBleHistoryRecord(i).addressIndex == addressIndex) return true;
  }
  return false;
}

// Purpose: Counts BLE address-table slots that are still referenced by retained observations.
size_t countReferencedBleAddresses() {
  if (!bleAddressTable || bleAddressTableCapacity == 0) return 0;
  size_t count = 0;
  for (size_t i = 0; i < bleAddressTableCapacity; i++)
    if (bleAddressIndexIsReferenced(i)) count++;
  return count;
}

// Purpose: Counts BLE scan-metadata slots that are still referenced by retained observations.
size_t countReferencedBleScanSlots() {
  if (!bleScanMetadata || bleScanMetadataCapacity == 0) return 0;
  size_t count = 0;
  for (size_t slot = 0; slot < bleScanMetadataCapacity; slot++) {
    bool referenced = false;
    for (size_t i = 0; i < bleHistoryCount; i++) {
      if (compactBleHistoryRecord(i).scanSlot == slot) { referenced = true; break; }
    }
    if (referenced) count++;
  }
  return count;
}

// Purpose: Updates peak BLE address-table and scan-metadata utilization diagnostics.
void updateBleUsageHighWaterMarks() {
  size_t addresses = countReferencedBleAddresses();
  if (addresses > bleAddressPeakReferenced) bleAddressPeakReferenced = addresses;
  size_t scans = countReferencedBleScanSlots();
  if (scans > bleScanMetadataPeakUsed) bleScanMetadataPeakUsed = scans;
}

// Purpose: Evicts the oldest BLE observation from the ring buffer.
void discardOldestBleObservation() {
  if (bleHistoryCount == 0 || bleHistoryCapacity == 0) return;
  bleHistoryStart = (bleHistoryStart + 1) % bleHistoryCapacity;
  bleHistoryCount--;
}

// Purpose: Removes BLE observations tied to a scan-metadata slot that must be recycled.
void discardBleObservationsForScanSlot(uint16_t scanSlot) {
  if (bleHistoryCount == 0) return;
  size_t original = bleHistoryCount;
  for (size_t i = 0; i < original; i++) {
    BleObservation observation = compactBleHistoryRecord(0);
    discardOldestBleObservation();
    if (observation.scanSlot != scanSlot) appendBleObservation(observation);
  }
}

// Purpose: Changes the logical BLE history-retention limit without reallocating boot-time buffers.
bool setBleRetentionLimit(size_t requestedCapacity) {
  if (requestedCapacity < MIN_BLE_HISTORY_RECORDS)
    requestedCapacity = MIN_BLE_HISTORY_RECORDS;
  if (requestedCapacity > bleHistoryCapacity)
    requestedCapacity = bleHistoryCapacity;
  bleHistoryRetentionLimit = requestedCapacity;
  while (bleHistoryCount > bleHistoryRetentionLimit)
    discardOldestBleObservation();
  bleHistoryResizeMessage =
    "BLE retention limit set to " + String(bleHistoryRetentionLimit) +
    " observations (history capacity " + String(bleHistoryCapacity) + ").";
  return true;
}

// Purpose: Appends one compact BLE observation while enforcing the configured retention limit.
void appendBleObservation(const BleObservation& observation) {
  if (!bleHistory || bleHistoryCapacity == 0 || bleHistoryRetentionLimit == 0)
    return;
  while (bleHistoryCount >= bleHistoryRetentionLimit)
    discardOldestBleObservation();
  size_t writeIndex = (bleHistoryStart + bleHistoryCount) % bleHistoryCapacity;
  bleHistory[writeIndex] = observation;
  bleHistoryCount++;
}

// Purpose: Clears retained BLE observations and resets related BLE history bookkeeping.
void clearBleHistory() {
  bleHistoryStart = 0;
  bleHistoryCount = 0;
  bleScanCounter = 0;
  lastBleScanUptimeMs = 0;
  bleAddressCount = 0;
  bleAddressPeakReferenced = 0;
  bleAddressTableFullDrops = 0;
  bleScanMetadataPeakUsed = 0;
  if (bleAddressTable && bleAddressTableCapacity)
    memset(bleAddressTable, 0, bleAddressTableCapacity * sizeof(BleAddressEntry));
  if (bleScanMetadata && bleScanMetadataCapacity)
    memset(bleScanMetadata, 0, bleScanMetadataCapacity * sizeof(SurveyScanMetadata));
}

// Purpose: Finds or prepares the BLE address-table lookup used to associate observations with compact identities.
int findBleAddress(const uint8_t address[6]) {
  for (size_t i = 0; i < bleAddressCount; i++) {
    if (memcmp(bleAddressTable[i].address, address, 6) == 0) return (int)i;
  }
  return -1;
}

int findOrCreateBleAddress(
  const uint8_t address[6],
  const String& name,
  uint8_t addressType
) {
  int existing = findBleAddress(address);
  if (existing >= 0) {
    BleAddressEntry& entry = bleAddressTable[existing];
    if (name.length() > 0 && !name.equals(String(entry.name)))
      name.toCharArray(entry.name, sizeof(entry.name));
    entry.addressType = addressType;
    return existing;
  }

  if (!bleAddressTable) { bleAddressTableFullDrops++; return -1; }

  size_t target = bleAddressCount;
  if (bleAddressCount >= bleAddressTableCapacity) {
    target = bleAddressTableCapacity;
    for (size_t i = 0; i < bleAddressTableCapacity; i++) {
      if (!bleAddressIndexIsReferenced(i)) { target = i; break; }
    }
    if (target >= bleAddressTableCapacity) {
      bleAddressTableFullDrops++;
      return -1;
    }
  } else {
    bleAddressCount++;
  }

  BleAddressEntry& entry = bleAddressTable[target];
  memset(&entry, 0, sizeof(entry));
  memcpy(entry.address, address, 6);
  if (name.length() > 0) name.toCharArray(entry.name, sizeof(entry.name));
  entry.addressType = addressType;
  return (int)target;
}

// Purpose: Allocates compact BLE observation, address, and metadata tables within the dual-radio RAM budget.
bool initializeCompactBleHistory(size_t budgetBytes) {
  const size_t addressCapacity = BLE_ADDRESS_TABLE_TARGET;
  const size_t scanCapacity = BLE_SCAN_METADATA_SLOTS;
  size_t metadataBytes =
      addressCapacity * sizeof(BleAddressEntry) +
      scanCapacity * sizeof(SurveyScanMetadata);
  size_t observationCapacity = MIN_BLE_HISTORY_RECORDS;
  if (budgetBytes > metadataBytes) {
    observationCapacity =
      (budgetBytes - metadataBytes) / sizeof(BleObservation);
    if (observationCapacity < MIN_BLE_HISTORY_RECORDS)
      observationCapacity = MIN_BLE_HISTORY_RECORDS;
  }
  if (observationCapacity > MAX_BLE_HISTORY_RECORDS)
    observationCapacity = MAX_BLE_HISTORY_RECORDS;

  bleAddressTable = (BleAddressEntry*)calloc(addressCapacity, sizeof(BleAddressEntry));
  bleScanMetadata =
      (SurveyScanMetadata*)calloc(scanCapacity, sizeof(SurveyScanMetadata));
  bleHistory =
      (BleObservation*)malloc(observationCapacity * sizeof(BleObservation));

  if (!bleAddressTable || !bleScanMetadata || !bleHistory) {
    if (bleAddressTable) free(bleAddressTable);
    if (bleScanMetadata) free(bleScanMetadata);
    if (bleHistory) free(bleHistory);
    bleAddressTable = nullptr;
    bleScanMetadata = nullptr;
    bleHistory = nullptr;
    bleAddressTableCapacity = 0;
    bleScanMetadataCapacity = 0;
    bleHistoryCapacity = 0;
    bleHistoryRetentionLimit = 0;
    return false;
  }

  bleAddressTableCapacity = addressCapacity;
  bleScanMetadataCapacity = scanCapacity;
  bleHistoryCapacity = observationCapacity;
  bleHistoryRetentionLimit = observationCapacity;
  bleHistoryStart = 0;
  bleHistoryCount = 0;
  bleAddressCount = 0;
  bleAddressPeakReferenced = 0;
  bleScanMetadataPeakUsed = 0;
  bleAddressTableFullDrops = 0;
  return true;
}

// Purpose: Counts distinct BLE scans still represented in retained history.
size_t countRetainedBleScanGroups() {
  if (bleHistoryCount == 0 || bleHistory == nullptr) return 0;
  size_t groups = 0;
  uint32_t previousScan = 0;
  bool havePrevious = false;
  for (size_t i = 0; i < bleHistoryCount; i++) {
    uint32_t currentScan = bleHistoryRecord(i).scanNumber;
    if (!havePrevious || currentScan != previousScan) {
      groups++;
      previousScan = currentScan;
      havePrevious = true;
    }
  }
  return groups;
}

// Purpose: Chooses Wi-Fi/BLE RAM budgets after radio mode is known and allocates the compact history tables.
void initializeAutoSizedHistories() {
  if (scanHistory != nullptr || bleHistory != nullptr) return;

  size_t freeHeap = ESP.getFreeHeap();

  if (!bleSurveyEnabled) {
    const size_t wifiMetadata =
        WIFI_ONLY_AP_TABLE_TARGET * sizeof(WifiApEntry) +
        WIFI_ONLY_SCAN_METADATA_SLOTS * sizeof(WifiScanMetadata);
    const size_t minimumWifiObs =
        MIN_SCAN_HISTORY_RECORDS * sizeof(WifiObservation);
    const size_t minimumCompactBytes = wifiMetadata + minimumWifiObs;
    size_t available = freeHeap > HISTORY_HEAP_RESERVE_BYTES
      ? freeHeap - HISTORY_HEAP_RESERVE_BYTES : 0;
    if (available < minimumCompactBytes) available = minimumCompactBytes;
    if (!initializeCompactWifiHistory(available, WIFI_ONLY_AP_TABLE_TARGET, WIFI_ONLY_SCAN_METADATA_SLOTS))
      initializeCompactWifiHistory(minimumCompactBytes, WIFI_ONLY_AP_TABLE_TARGET, WIFI_ONLY_SCAN_METADATA_SLOTS);
  } else {
    // Keep a deliberate allocation-time reserve so later web, mDNS, scan, and
    // transient presentation allocations do not have to compete with oversized
    // history rings. If the fixed tables plus minimum observation rings exceed
    // the remaining budget, the minimum rings take priority over expansion.
    size_t available = freeHeap > DUAL_RADIO_HEAP_RESERVE_BYTES
      ? freeHeap - DUAL_RADIO_HEAP_RESERVE_BYTES : 0;

    const size_t wifiMetadata =
        DUAL_RADIO_WIFI_AP_TABLE_TARGET * sizeof(WifiApEntry) +
        DUAL_RADIO_WIFI_SCAN_METADATA_SLOTS * sizeof(WifiScanMetadata);
    const size_t bleMetadata =
        BLE_ADDRESS_TABLE_TARGET * sizeof(BleAddressEntry) +
        BLE_SCAN_METADATA_SLOTS * sizeof(SurveyScanMetadata);
    const size_t minimumWifiObs =
        MIN_SCAN_HISTORY_RECORDS * sizeof(WifiObservation);
    const size_t minimumBleObs =
        MIN_BLE_HISTORY_RECORDS * sizeof(BleObservation);
    const size_t minimumTotal =
        wifiMetadata + bleMetadata + minimumWifiObs + minimumBleObs;

    size_t extra = available > minimumTotal ? available - minimumTotal : 0;
    size_t wifiExtra = extra / 2;
    size_t bleExtra = extra - wifiExtra;
    size_t wifiBudget = wifiMetadata + minimumWifiObs + wifiExtra;
    size_t bleBudget = bleMetadata + minimumBleObs + bleExtra;

    if (!initializeCompactWifiHistory(wifiBudget, DUAL_RADIO_WIFI_AP_TABLE_TARGET, DUAL_RADIO_WIFI_SCAN_METADATA_SLOTS))
      initializeCompactWifiHistory(wifiMetadata + minimumWifiObs, DUAL_RADIO_WIFI_AP_TABLE_TARGET, DUAL_RADIO_WIFI_SCAN_METADATA_SLOTS);
    if (!initializeCompactBleHistory(bleBudget))
      initializeCompactBleHistory(bleMetadata + minimumBleObs);
  }

  bootWifiHistoryCapacity = scanHistoryCapacity;
  bootBleHistoryCapacity = bleHistoryCapacity;
  historyResizeMessage = "";
  bleHistoryResizeMessage = "";
}

// Purpose: Tests whether the requested BLE address currently exists in retained history.
bool bleHistoryContainsAddress(const String& address) {
  uint8_t parsed[6];
  if (!parseBleAddress(address, parsed)) return false;
  int index = findBleAddress(parsed);
  return index >= 0 && bleAddressIndexIsReferenced((size_t)index);
}

// Purpose: Returns the most recently retained advertised name for a BLE address, when available.
String latestBleNameForAddress(const String& address) {
  uint8_t parsed[6];
  if (!parseBleAddress(address, parsed)) return "(unnamed)";
  int index = findBleAddress(parsed);
  if (index < 0 || !bleAddressIndexIsReferenced((size_t)index)) return "(unnamed)";
  return bleAddressTable[index].name[0] ? String(bleAddressTable[index].name) : String("(unnamed)");
}

// Purpose: Checks whether a newer retained observation exists for the same BLE address.
bool hasNewerBleObservationForAddress(size_t logicalIndex, const String& address) {
  for (size_t i = logicalIndex + 1; i < bleHistoryCount; i++) {
    if (String(bleHistoryRecord(i).address).equalsIgnoreCase(address)) return true;
  }
  return false;
}

// Purpose: Aggregates retained observations for one BLE address into RSSI and identity summary statistics.
bool buildBleDeviceSummary(const String& address, BLEDeviceSummary& summary) {
  summary = {};
  resetSignalStats(summary.signal);
  bool found = false;
  for (size_t i = 0; i < bleHistoryCount; i++) {
    BleScanRecord record = bleHistoryRecord(i);
    if (!String(record.address).equalsIgnoreCase(address)) continue;
    found = true;
    addSignalObservation(summary.signal, record.rssi, record.uptimeMs);
    summary.addressType = record.addressType;
  }
  if (!found) return false;
  address.toCharArray(summary.address, sizeof(summary.address));
  String currentName = latestBleNameForAddress(address);
  if (currentName != "(unnamed)") {
    currentName.toCharArray(summary.name, sizeof(summary.name));
    summary.named = true;
  }
  return true;
}

// Purpose: Starts one asynchronous NimBLE scan and records start-call timing, heap, and trigger diagnostics.
int performLoggedBLEScanWithTrigger(const char* trigger) {
  if (!bleSurveyEnabled) {
    bleStatusMessage = "Bluetooth Survey is disabled; BLE stack is not initialized.";
    return -1;
  }
  if (bleDiagnosticScanActive) {
    bleStatusMessage = "BLE scan already in progress.";
    return -2;
  }
  if (wifiScanInProgress) {
    bleStatusMessage = "BLE scan deferred because a Wi-Fi scan is active.";
    return -5;
  }

  initializeBLEScanner();
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan == nullptr) {
    bleStatusMessage = "BLE scanner is unavailable.";
    return -3;
  }

  portENTER_CRITICAL(&bleScanCaptureMux);
  bleScanCaptureCount = 0;
  bleScanCaptureDrops = 0;
  portEXIT_CRITICAL(&bleScanCaptureMux);
  bleScanCompletionReason = 0;
  bleStatusMessage = "BLE scan in progress...";
  bleDiagnosticScanActive = true;
  bleAsyncCompletionPending = false;
  bleDiagnosticScanStartMs = millis();
  strncpy(bleDiagnosticActiveTrigger, trigger ? trigger : "unknown", sizeof(bleDiagnosticActiveTrigger) - 1);
  bleDiagnosticActiveTrigger[sizeof(bleDiagnosticActiveTrigger) - 1] = '\0';
  bleDiagnosticLastHeapBefore = ESP.getFreeHeap();
  bleDiagnosticLastLargestBefore = diagnosticLargestFreeBlock();

  if (diagnosticStreamingEnabled && (diagnosticBleEvents || diagnosticSurveyEvents)) {
    diagnosticPrefix("BLE SCAN START");
    Serial.print("stack=NimBLE");
    Serial.print(" trigger="); Serial.print(bleDiagnosticActiveTrigger);
    Serial.print(" nextSeq="); Serial.print(bleScanCounter + 1);
    Serial.print(" durationCfg="); Serial.print(BLE_SCAN_DURATION_SECONDS); Serial.print("s");
    if (diagnosticMemoryEvents) {
      Serial.print(" heap="); Serial.print(bleDiagnosticLastHeapBefore);
      Serial.print(" min="); Serial.print(ESP.getMinFreeHeap());
      Serial.print(" largest="); Serial.print(bleDiagnosticLastLargestBefore);
    }
    Serial.println();
  }

  startScanLed(BLE_SCAN_LED_PERIOD_TICKS);
  uint32_t apiStartMs = millis();
  bool started = scan->start(BLE_SCAN_DURATION_SECONDS * 1000UL, false, true);
  uint32_t apiEndMs = millis();
  bleDiagnosticLastApiDurationMs = (uint32_t)(apiEndMs - apiStartMs);

  if (!started) {
    stopScanLed();
    bleDiagnosticScanActive = false;
    bleDiagnosticLastScanEndMs = millis();
    bleStatusMessage = "BLE scan failed to start.";
    if (diagnosticStreamingEnabled && diagnosticBleEvents) {
      diagnosticPrefix("BLE SCAN START FAILED");
      Serial.print("apiCall="); Serial.print(bleDiagnosticLastApiDurationMs); Serial.println("ms");
    }
    return -4;
  }

  if (diagnosticStreamingEnabled && diagnosticBleEvents) {
    diagnosticPrefix("BLE SCAN STARTED");
    Serial.print("apiCall="); Serial.print(bleDiagnosticLastApiDurationMs); Serial.print("ms");
    if (diagnosticMemoryEvents) {
      Serial.print(" heap="); Serial.print(ESP.getFreeHeap());
      Serial.print(" min="); Serial.print(ESP.getMinFreeHeap());
      Serial.print(" largest="); Serial.print(diagnosticLargestFreeBlock());
    }
    Serial.println();
  }
  return 0;
}

// Purpose: Processes advertisements from a completed NimBLE scan in the main loop and updates retained history and diagnostics.
void serviceCompletedBLEScan() {
  if (!bleAsyncCompletionPending) return;
  bleAsyncCompletionPending = false;

  uint32_t completionMs = bleAsyncCompletionMs;
  stopScanLed();

  bleDiagnosticLastHeapAfterApi = ESP.getFreeHeap();
  bleDiagnosticLastLargestAfterApi = diagnosticLargestFreeBlock();

  uint16_t capturedCount = 0;
  uint32_t captureDrops = 0;
  portENTER_CRITICAL(&bleScanCaptureMux);
  capturedCount = bleScanCaptureCount;
  captureDrops = bleScanCaptureDrops;
  portEXIT_CRITICAL(&bleScanCaptureMux);
  bleDiagnosticLastResultCount = capturedCount;
  bleDiagnosticLastCaptureDrops = captureDrops;
  bleDiagnosticLastCompletionReason = bleScanCompletionReason;

  if (diagnosticStreamingEnabled && diagnosticBleEvents) {
    diagnosticPrefix("BLE SCAN COMPLETE CALLBACK");
    Serial.print("elapsed="); Serial.print((uint32_t)(completionMs - bleDiagnosticScanStartMs)); Serial.print("ms");
    Serial.print(" results="); Serial.print(capturedCount);
    Serial.print(" captureDrops="); Serial.print(captureDrops);
    Serial.print(" reason="); Serial.print(bleDiagnosticLastCompletionReason);
    if (diagnosticMemoryEvents) {
      Serial.print(" heap="); Serial.print(bleDiagnosticLastHeapAfterApi);
      Serial.print(" min="); Serial.print(ESP.getMinFreeHeap());
      Serial.print(" largest="); Serial.print(bleDiagnosticLastLargestAfterApi);
    }
    Serial.println();
  }

  uint32_t processingStartMs = millis();
  bleScanCounter++;
  lastBleScanUptimeMs = surveySessionUptimeMs();

  if (bleScanMetadata && bleScanMetadataCapacity > 0) {
    uint16_t scanSlot = (uint16_t)((bleScanCounter - 1) % bleScanMetadataCapacity);
    if (bleScanMetadata[scanSlot].scanNumber != 0 && bleScanMetadata[scanSlot].scanNumber != bleScanCounter)
      discardBleObservationsForScanSlot(scanSlot);
    bleScanMetadata[scanSlot].scanNumber = bleScanCounter;
    bleScanMetadata[scanSlot].uptimeMs = lastBleScanUptimeMs;

    size_t historyBefore = bleHistoryCount;
    size_t dropsBefore = bleAddressTableFullDrops;
    for (uint16_t i = 0; i < capturedCount; i++) {
      NimBleCapturedAdvertisement captured = {};
      portENTER_CRITICAL(&bleScanCaptureMux);
      captured = bleScanCapture[i];
      portEXIT_CRITICAL(&bleScanCaptureMux);

      String name(captured.name);
      int addressIndex = findOrCreateBleAddress(captured.address, name, captured.addressType);
      if (addressIndex < 0) continue;
      BleObservation observation = {};
      observation.addressIndex = (uint16_t)addressIndex;
      observation.scanSlot = scanSlot;
      observation.rssi = captured.rssi;
      appendBleObservation(observation);
    }

    updateBleUsageHighWaterMarks();

    bleDiagnosticLastProcessingDurationMs = (uint32_t)(millis() - processingStartMs);
    bleDiagnosticLastTotalDurationMs = (uint32_t)(millis() - bleDiagnosticScanStartMs);
    bleDiagnosticLastHeapAfterProcessing = ESP.getFreeHeap();
    bleDiagnosticLastLargestAfterProcessing = diagnosticLargestFreeBlock();
    bleDiagnosticDurationCount++;
    bleDiagnosticTotalDurationMs += bleDiagnosticLastTotalDurationMs;
    if (bleDiagnosticLastTotalDurationMs < bleDiagnosticMinDurationMs) bleDiagnosticMinDurationMs = bleDiagnosticLastTotalDurationMs;
    if (bleDiagnosticLastTotalDurationMs > bleDiagnosticMaxDurationMs) bleDiagnosticMaxDurationMs = bleDiagnosticLastTotalDurationMs;
    bleDiagnosticScanActive = false;
    bleDiagnosticLastScanEndMs = millis();

    bleStatusMessage = "BLE scan #" + String(bleScanCounter) + " complete: " + String(capturedCount) + " address(es) observed.";
    if (captureDrops > 0)
      bleStatusMessage += " WARN: " + String(captureDrops) + " advertisement(s) exceeded the temporary BLE scan capture capacity.";
    if (bleAddressTableFullDrops > 0)
      bleStatusMessage += " WARN: " + String(bleAddressTableFullDrops) + " observation(s) dropped because the BLE Address Table was full.";

    if (diagnosticStreamingEnabled && (diagnosticBleEvents || diagnosticSurveyEvents))
      recordDiagnosticEvent("BLE SCAN", "seq=" + String(bleScanCounter) + " elapsed=" + String(bleDiagnosticLastTotalDurationMs) + "ms results=" + String(capturedCount) + " drops=" + String(captureDrops));

    if (diagnosticStreamingEnabled && (diagnosticBleEvents || diagnosticSurveyEvents)) {
      diagnosticPrefix("BLE SCAN END");
      Serial.print("seq="); Serial.print(bleScanCounter);
      Serial.print(" trigger="); Serial.print(bleDiagnosticActiveTrigger);
      Serial.print(" startApi="); Serial.print(bleDiagnosticLastApiDurationMs); Serial.print("ms");
      Serial.print(" elapsed="); Serial.print(bleDiagnosticLastTotalDurationMs); Serial.print("ms");
      Serial.print(" serviceDelay="); Serial.print((uint32_t)(processingStartMs - completionMs)); Serial.print("ms");
      Serial.print(" process="); Serial.print(bleDiagnosticLastProcessingDurationMs); Serial.print("ms");
      Serial.print(" results="); Serial.print(capturedCount);
      Serial.print(" captureDrops="); Serial.print(captureDrops);
      Serial.print(" reason="); Serial.print(bleDiagnosticLastCompletionReason);
      Serial.print(" obsAdded="); Serial.print(bleHistoryCount >= historyBefore ? bleHistoryCount - historyBefore : 0);
      Serial.print(" dropsDelta="); Serial.print(bleAddressTableFullDrops - dropsBefore);
      Serial.print(" history="); Serial.print(bleHistoryCount); Serial.print("/"); Serial.print(bleHistoryRetentionLimit);
      Serial.print(" addr="); Serial.print(countReferencedBleAddresses()); Serial.print("/"); Serial.print(bleAddressTableCapacity);
      if (diagnosticMemoryEvents) {
        Serial.print(" heap="); Serial.print(bleDiagnosticLastHeapAfterProcessing);
        Serial.print(" min="); Serial.print(ESP.getMinFreeHeap());
        Serial.print(" largest="); Serial.print(bleDiagnosticLastLargestAfterProcessing);
      }
      Serial.println();
    }
    if (strcmp(bleDiagnosticActiveTrigger, "initial") == 0) captureBootHeapCheckpoint("Initial BLE scan");
  } else {
    bleDiagnosticLastProcessingDurationMs = (uint32_t)(millis() - processingStartMs);
    bleDiagnosticLastTotalDurationMs = (uint32_t)(millis() - bleDiagnosticScanStartMs);
    bleDiagnosticScanActive = false;
    bleDiagnosticLastScanEndMs = millis();
  }
}

// Purpose: Compatibility wrapper for BLE scan callers that do not need a specific trigger label.
int performLoggedBLEScan() {
  return performLoggedBLEScanWithTrigger("direct");
}

// Purpose: Prints a string to Serial and pads it to a fixed column width for readable text tables.
void printPadded(const String& value, int width) {
  String output = value;

  if (output.length() > width) {
    if (width > 3) output = output.substring(0, width - 3) + "...";
    else output = output.substring(0, width);
  }

  Serial.print(output);
  for (int i = output.length(); i < width; i++) Serial.print(' ');
}


// ============================================================
// Wi-Fi credential storage// ============================================================
// Wi-Fi credential storage
// ============================================================

// Purpose: Stores validated infrastructure Wi-Fi credentials in NVS.
void saveCredentials(const String& ssid, const String& password) {
  preferences.begin("wifi", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();

  Serial.println("Wi-Fi credentials saved.");
}

// Purpose: Loads saved infrastructure Wi-Fi credentials from NVS when present.
bool loadCredentials(String& ssid, String& password) {
  preferences.begin("wifi", true);

  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");

  preferences.end();

  return ssid.length() > 0;
}

// Purpose: Deletes saved infrastructure Wi-Fi credentials from NVS.
void eraseCredentials() {
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();

  Serial.println("Saved Wi-Fi credentials erased.");
}


// ============================================================
// Uptime
// ============================================================

// Purpose: Formats a millisecond duration as a compact human-readable uptime string.
String formatUptime(uint32_t uptimeMs) {
  unsigned long totalSeconds = uptimeMs / 1000;

  unsigned long days = totalSeconds / 86400;
  totalSeconds %= 86400;

  unsigned long hours = totalSeconds / 3600;
  totalSeconds %= 3600;

  unsigned long minutes = totalSeconds / 60;
  unsigned long seconds = totalSeconds % 60;

  String result = "";

  if (days > 0) {
    result += String(days);
    result += "d ";
  }

  if (hours > 0 || days > 0) {
    result += String(hours);
    result += "h ";
  }

  result += String(minutes);
  result += "m ";

  result += String(seconds);
  result += "s";

  return result;
}

// Purpose: Returns the current raw boot uptime as a human-readable string.
String getUptimeString() {
  return formatUptime(millis());
}

String retainedWindowLabel(
  uint32_t oldestMs,
  uint32_t newestMs
) {
  if (newestMs < oldestMs) return "counter wrapped";
  return formatUptime(newestMs - oldestMs);
}

// Purpose: Converts a survey-session timestamp into a relative age such as seconds, minutes, or hours ago.
String observationAgeLabel(uint32_t observationMs) {
  uint32_t now = surveySessionUptimeMs();
  if (now < observationMs) return "counter wrapped";
  return formatUptime(now - observationMs) + " ago";
}

// Purpose: Converts a raw boot-runtime timestamp into a relative age for scheduler diagnostics.
String runtimeAgeLabel(uint32_t runtimeMs) {
  uint32_t now = millis();
  if (now < runtimeMs) return "counter wrapped";
  return formatUptime(now - runtimeMs) + " ago";
}

// Purpose: Marks the current web request as an explicit user action so automatic scans can briefly yield afterward.
void markExplicitUserInteraction() {
  explicitUserInteractionHandled = true;
}

// Purpose: Starts the post-request defer window used to prioritize immediate user interactions over new automatic scans.
void armUserInteractionDeferAfterWebService() {
  if (!explicitUserInteractionHandled) return;
  explicitUserInteractionHandled = false;
  userInteractionDeferStartedMs = millis();
  userInteractionDeferArmed = true;
}

// Purpose: Returns whether the short user-interaction defer window is currently active.
bool userInteractionDeferActive() {
  if (!userInteractionDeferArmed) return false;
  return (uint32_t)(millis() - userInteractionDeferStartedMs) < USER_INTERACTION_DEFER_MS;
}

// Purpose: Formats a duration in milliseconds using an appropriate human-readable unit.
String millisecondsLabel(uint32_t durationMs) {
  if (durationMs < 1000) return String(durationMs) + " ms";
  return String(durationMs / 1000.0f, 2) + " s";
}

// Purpose: Builds the last/average/min/max successful Wi-Fi scan-duration summary.
String wifiScanDurationSummaryLabel() {
  if (wifiScanDurationCount == 0) return "No completed scan timing yet";
  return "last " + millisecondsLabel(wifiLastScanDurationMs) +
         "; avg " + millisecondsLabel(wifiAverageScanDurationMs()) +
         "; min " + millisecondsLabel(wifiMinScanDurationMs) +
         "; max " + millisecondsLabel(wifiMaxScanDurationMs);
}

// Purpose: Calculates and formats CSV streaming throughput from bytes and elapsed time.
String csvThroughputLabel(size_t bytes, uint32_t durationMs) {
  if (durationMs == 0) return "n/a";
  float kibPerSecond = ((float)bytes / 1024.0f) / ((float)durationMs / 1000.0f);
  return String(kibPerSecond, 1) + " KB/s";
}

// Purpose: Builds the row/size/duration/throughput summary for the most recent CSV export.
String csvExportSummaryLabel(size_t rows, size_t bytes, uint32_t durationMs) {
  return String(rows) + " rows; " +
         String(bytes / 1024.0f, 1) + " KB; " +
         millisecondsLabel(durationMs) + "; " +
         csvThroughputLabel(bytes, durationMs);
}

// Purpose: Determines whether automatic Wi-Fi scanning is later than expected after allowing for scan and interaction timing.
bool wifiAutoScanCadenceOverdue() {
  uint32_t now = millis();
  uint32_t intervalMs = scanIntervalSeconds * 1000UL;

  if (csvExportInProgress || userInteractionDeferActive() || wifiScanInProgress)
    return false;

  if (!wifiScanCompletedSinceBoot)
    return !initialWifiScanPending && !wifiScanInProgress;

  if (wifiAutoScanRetryPending) return true;

  uint32_t schedulerAgeMs = now - lastAutoScanMs;
  return schedulerAgeMs > intervalMs + WIFI_AUTOSCAN_DIAG_GRACE_MS;
}

// Purpose: Builds the human-readable automatic Wi-Fi scan health/status message.
String wifiAutoScanDiagnosticLabel() {
  uint32_t now = millis();
  uint32_t intervalMs = scanIntervalSeconds * 1000UL;

  if (csvExportInProgress)
    return "PAUSED - CSV export in progress";

  if (userInteractionDeferActive())
    return "DEFERRED - user interaction priority";

  if (wifiScanInProgress && wifiCurrentScanAutomatic)
    return "OK - automatic scan in progress";

  if (wifiScanInProgress)
    return "OK - manual scan in progress";

  if (!wifiScanCompletedSinceBoot) {
    if (initialWifiScanPending)
      return "STARTING - waiting for first completed scan";
    if (wifiAutoScanRetryPending)
      return "WARN - automatic scan retry pending";
    return "WARN - no completed Wi-Fi scan";
  }

  if (wifiAutoScanRetryPending)
    return "WARN - automatic scan retry pending";

  uint32_t schedulerAgeMs = now - lastAutoScanMs;
  if (schedulerAgeMs <= intervalMs)
    return "OK - last scan completed within interval";

  if (schedulerAgeMs <= intervalMs + WIFI_AUTOSCAN_DIAG_GRACE_MS)
    return "DUE - automatic scan awaiting scheduler";

  return "WARN - scan overdue; automatic scan not in progress";
}

// Purpose: Formats the relative runtime age of the last automatic Wi-Fi scan start.
String wifiAutoScanLastStartLabel() {
  if (wifiAutoScanStartCount == 0) return "Never";
  return runtimeAgeLabel(lastWifiAutoScanStartMs);
}

// Purpose: Formats the relative runtime age of the last automatic Wi-Fi scan completion.
String wifiAutoScanLastCompletionLabel() {
  if (wifiAutoScanCompletionCount == 0) return "Never";
  return runtimeAgeLabel(lastWifiAutoScanCompletionMs);
}

// ============================================================
// Wi-Fi connection
// ============================================================

// Purpose: Attempts infrastructure Wi-Fi connection with supplied credentials while preserving survey operation.
bool connectToWiFi(const String& ssid, const String& password) {
  Serial.println();
  Serial.print("Connecting to ");
  Serial.print(ssid);
  Serial.println("...");

  ensureWiFiStationMode();

  WiFi.disconnect();
  delay(100);

  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long startTime = millis();

  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startTime >= WIFI_TIMEOUT_MS) {
      Serial.println();
      Serial.println("Connection timed out.");

      WiFi.disconnect();
      return false;
    }

    Serial.print(".");
    delay(500);
  }

  Serial.println();
  Serial.println("Connected successfully.");

  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  Serial.print("Signal strength: ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");

  return true;
}

// Purpose: Loads and attempts connection using saved infrastructure Wi-Fi credentials.
bool connectUsingSavedCredentials() {
  String ssid;
  String password;

  if (!loadCredentials(ssid, password)) {
    Serial.println("No saved Wi-Fi credentials.");
    return false;
  }

  Serial.print("Saved Wi-Fi network: ");
  Serial.println(ssid);

  return connectToWiFi(ssid, password);
}


// Purpose: Legacy reconnect decision hook; native ESP32 auto-reconnect is preferred in the current design.
void considerInfrastructureReconnectAfterScan(int networkCount) {
  if (WiFi.status() == WL_CONNECTED || networkCount <= 0) {
    if (WiFi.status() == WL_CONNECTED) infrastructureReconnectPending = false;
    return;
  }

  String savedSsid;
  String savedPassword;
  if (!loadCredentials(savedSsid, savedPassword) || savedSsid.length() == 0) {
    infrastructureReconnectPending = false;
    return;
  }

  bool seen = false;
  for (int i = 0; i < networkCount; i++) {
    if (WiFi.SSID(i) == savedSsid) {
      seen = true;
      break;
    }
  }

  if (!seen) {
    infrastructureReconnectPending = false;
    return;
  }

  // Presence in the most recently completed survey scan is the trigger. The
  // actual WiFi.begin() call is
  // deferred until after scan result cleanup so reconnect does not add a scan
  // or block the completed-scan processing path.
  infrastructureReconnectPending = true;
}


// Purpose: Services application-side infrastructure reconnect state when such a retry has been explicitly armed.
void serviceInfrastructureReconnect() {
  if (WiFi.status() == WL_CONNECTED) {
    infrastructureReconnectPending = false;
    if (infrastructureReconnectAttemptActive) {
      infrastructureReconnectAttemptActive = false;
      infrastructureReconnectSuccessCount++;
      Serial.print("Infrastructure Wi-Fi reconnected automatically: ");
      Serial.println(WiFi.SSID());
    }
    return;
  }

  uint32_t now = millis();

  if (
    infrastructureReconnectAttemptActive &&
    (uint32_t)(now - infrastructureReconnectAttemptStartedMs) >=
      INFRA_RECONNECT_ATTEMPT_WINDOW_MS
  ) {
    infrastructureReconnectAttemptActive = false;
  }

  if (
    !infrastructureReconnectPending ||
    wifiScanInProgress ||
    csvExportInProgress ||
    userInteractionDeferActive()
  ) {
    return;
  }

  if (
    infrastructureReconnectAttempted &&
    (uint32_t)(now - lastInfrastructureReconnectAttemptMs) <
      INFRA_RECONNECT_BACKOFF_MS
  ) {
    return;
  }

  String savedSsid;
  String savedPassword;
  if (!loadCredentials(savedSsid, savedPassword) || savedSsid.length() == 0) {
    infrastructureReconnectPending = false;
    return;
  }

  infrastructureReconnectPending = false;
  infrastructureReconnectAttemptActive = true;
  infrastructureReconnectAttempted = true;
  infrastructureReconnectAttemptStartedMs = now;
  lastInfrastructureReconnectAttemptMs = now;
  infrastructureReconnectAttemptCount++;

  Serial.print("Saved infrastructure network observed; reconnect attempt #");
  Serial.print(infrastructureReconnectAttemptCount);
  Serial.print(" to ");
  Serial.println(savedSsid);

  // WiFi.begin() is asynchronous here. Survey scanning remains the primary
  // activity; no blocking wait loop and no extra reconnect-specific scan.
  ensureWiFiStationMode();
  WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
}


// ============================================================
// Informational Wi-Fi scan
// ============================================================

// Purpose: Runs the interactive/serial Wi-Fi scan presentation path.
void scanNetworks() {
  Serial.println();
  Serial.println("Scanning for Wi-Fi networks...");
  Serial.println();

  int networkCount = performLoggedScan();

  if (networkCount <= 0) {
    Serial.println("No networks found.");
    WiFi.scanDelete();
    return;
  }

  Serial.print(networkCount);
  Serial.println(" network(s) found:");
  Serial.println();

  Serial.println("SSID                              SIGNAL      CH   SECURITY");
  Serial.println("--------------------------------  ----------  ---  ----------------");

  for (int i = 0; i < networkCount; i++) {
    String ssid = WiFi.SSID(i);
    String signal = String(WiFi.RSSI(i)) + " dBm";
    String channel = String(WiFi.channel(i));
    String security = securityLabel(WiFi.encryptionType(i));

    printPadded(ssid, 34);
    printPadded(signal, 12);
    printPadded(channel, 5);
    Serial.println(security);
  }

  Serial.println();
  Serial.print("Logged as scan #");
  Serial.print(scanCounter);
  Serial.print(". History: ");
  Serial.print(historyCount);
  Serial.print(" / ");
  Serial.print(scanHistoryCapacity);
  Serial.println(" records.");

  Serial.println("Scan complete.");
  Serial.println("Use 'wifi-config' to configure an infrastructure network.");

  WiFi.scanDelete();
}


// ============================================================
// Interactive Wi-Fi configuration
// ============================================================

// Purpose: Initializes station/AP Wi-Fi operation and attempts optional infrastructure connectivity.
void configureWiFi() {
  while (true) {
    Serial.println();
    Serial.println("Scanning for Wi-Fi networks...");
    Serial.println();

    int networkCount = performLoggedScan();

    if (networkCount == 0) {
      Serial.println("No Wi-Fi networks found.");
      WiFi.scanDelete();

      Serial.println();
      Serial.println("Press Enter to scan again.");
      Serial.println("Enter 'q' to cancel.");
      Serial.print("> ");

      String input = readSerialLine();

      if (input.equalsIgnoreCase("q")) {
        Serial.println();
        Serial.println("Wi-Fi configuration cancelled.");
        return;
      }

      continue;
    }

    Serial.println("#   SSID                              SIGNAL      CH   SECURITY");
    Serial.println("--  --------------------------------  ----------  ---  ----------------");

    for (int i = 0; i < networkCount; i++) {
      String number = String(i + 1);
      String ssid = WiFi.SSID(i);
      String signal = String(WiFi.RSSI(i)) + " dBm";
      String channel = String(WiFi.channel(i));
      String security = securityLabel(WiFi.encryptionType(i));

      printPadded(number, 4);
      printPadded(ssid, 34);
      printPadded(signal, 12);
      printPadded(channel, 5);
      Serial.println(security);
    }

    Serial.println();
    Serial.println("Enter network number.");
    Serial.println("Enter 'q' to cancel.");
    Serial.print("> ");

    String selectionText = readSerialLine();

    if (selectionText.equalsIgnoreCase("q")) {
      WiFi.scanDelete();

      Serial.println();
      Serial.println("Wi-Fi configuration cancelled.");
      return;
    }

    int selection = selectionText.toInt();

    if (selection < 1 || selection > networkCount) {
      Serial.println("Invalid selection.");
      WiFi.scanDelete();
      continue;
    }

    int networkIndex = selection - 1;

    String selectedSSID = WiFi.SSID(networkIndex);
    wifi_auth_mode_t encryption = WiFi.encryptionType(networkIndex);

    WiFi.scanDelete();

    Serial.println();
    Serial.print("Selected network: ");
    Serial.println(selectedSSID);

    String password = "";

    if (encryption != WIFI_AUTH_OPEN) {
      Serial.print("Enter Wi-Fi passphrase: ");
      password = readSerialLine();
    }

    if (connectToWiFi(selectedSSID, password)) {
      saveCredentials(selectedSSID, password);

      Serial.println();
      Serial.println("Wi-Fi setup complete.");

      return;
    }

    Serial.println();
    Serial.println("Unable to connect using those credentials.");
    Serial.println("Try again.");
  }
}


// ============================================================
// Status display
// ============================================================

// Purpose: Prints current infrastructure and Device AP Wi-Fi state to Serial.
void printWiFiStatus() {
  ensureWiFiStationMode();

  Serial.println();
  Serial.println("================================");
  Serial.println(" Wi-Fi Status");
  Serial.println("================================");

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Status:    Connected");

    Serial.print("SSID:      ");
    Serial.println(WiFi.SSID());

    Serial.print("IP:        ");
    Serial.println(WiFi.localIP());

    Serial.print("MAC:       ");
    Serial.println(WiFi.macAddress());

    Serial.print("Hostname:  ");
    Serial.println(WiFi.getHostname());

    Serial.print("Gateway:   ");
    Serial.println(WiFi.gatewayIP());

    Serial.print("Subnet:    ");
    Serial.println(WiFi.subnetMask());

    Serial.print("RSSI:      ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");

    Serial.print("Uptime:    ");
    Serial.println(getUptimeString());
  } else {
    Serial.println("Status:    Not connected");

    Serial.print("MAC:       ");
    Serial.println(WiFi.macAddress());

    Serial.print("Hostname:  ");
    Serial.println(WiFi.getHostname());

    Serial.print("Uptime:    ");
    Serial.println(getUptimeString());
  }

  Serial.println();
  Serial.println("Access Point:");

  Serial.print("Status:    ");
  Serial.println(apRunning ? "Running" : "Disabled");

  if (apRunning) {
    Serial.print("SSID:      ");
    Serial.println(apSSID);

    Serial.print("IP:        ");
    Serial.println(WiFi.softAPIP());

    Serial.print("Clients:   ");
    Serial.println(WiFi.softAPgetStationNum());
  }

  Serial.println();
}

// Purpose: Prints the ESP32 Wi-Fi MAC identities to Serial.
void printMacAddress() {
  ensureWiFiStationMode();

  Serial.println();
  Serial.print("Wi-Fi MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println();
}


// ============================================================
// Firmware information
// ============================================================

// Purpose: Returns the compile date/time string for the running firmware build.
String firmwareBuildTimestamp() {
  return String(__DATE__) + " " + String(__TIME__);
}

// Purpose: Prints firmware identity, core/IDF, chip, memory, and related build information to Serial.
void printFirmwareInfo() {
  Serial.println();
  Serial.println("================================");
  Serial.println(" Firmware Information");
  Serial.println("================================");
  Serial.print("File:       ");
  Serial.println(FIRMWARE_FILE);
  Serial.print("Version:    ");
  Serial.println(FIRMWARE_VERSION);
  Serial.print("Built:      ");
  Serial.println(firmwareBuildTimestamp());
  Serial.println();
}


// ============================================================
// HTML helpers
// ============================================================

// ============================================================

// Purpose: Escapes text before inserting it into HTML so data cannot accidentally break page markup.
String htmlEscape(const String& input) {
  String output = input;
  output.replace("&", "&amp;");
  output.replace("<", "&lt;");
  output.replace(">", "&gt;");
  output.replace("\"", "&quot;");
  output.replace("'", "&#39;");
  return output;
}

// Purpose: Percent-encodes text before placing it in URL query parameters.
String urlEncode(const String& input) {
  const char* hex = "0123456789ABCDEF";
  String output;

  output.reserve(input.length() * 3);

  for (size_t i = 0; i < input.length(); i++) {
    unsigned char c = input.charAt(i);

    if (
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' ||
      c == '_' ||
      c == '.' ||
      c == '~'
    ) {
      output += (char)c;
    } else {
      output += '%';
      output += hex[(c >> 4) & 0x0F];
      output += hex[c & 0x0F];
    }
  }

  return output;
}

// Purpose: Returns the latest retained SSID associated with a specific BSSID.
String latestSSIDForBSSID(const String& bssid) {
  if (bssid.length() == 0 || wifiApTable == nullptr || scanHistory == nullptr) return "";

  int apIndex = findWifiApByTextBssid(bssid);
  if (apIndex < 0) return "";

  for (size_t offset = 0; offset < historyCount; offset++) {
    size_t logicalIndex = historyCount - 1 - offset;
    const WifiObservation& observation = compactHistoryRecord(logicalIndex);
    if (observation.apIndex != (uint16_t)apIndex) continue;

    const WifiApEntry& ap = wifiApTable[apIndex];
    return ap.ssid[0] == '\0' ? String("(hidden)") : String(ap.ssid);
  }

  return "";
}

// Purpose: Checks whether a BSSID is represented anywhere in retained Wi-Fi history.
bool historyContainsBSSID(const String& bssid) {
  if (bssid.length() == 0 || scanHistory == nullptr) return false;

  int apIndex = findWifiApByTextBssid(bssid);
  if (apIndex < 0) return false;

  for (size_t i = 0; i < historyCount; i++) {
    if (compactHistoryRecord(i).apIndex == (uint16_t)apIndex) return true;
  }

  return false;
}

// Purpose: Returns the shared CSS used by all web-interface pages, including progressive view-depth visibility rules.
String pageStyles() {
  return R"rawliteral(
<style>
  :root {
    color-scheme: light;
    --page-bg: #f4f4f4;
    --card-bg: #ffffff;
    --text: #111111;
    --muted: #666666;
    --border: #dddddd;
    --header-bg: #f7f7f7;
    --button-bg: #333333;
    --button-text: #ffffff;
    --input-bg: #ffffff;
    --input-border: #bbbbbb;
    --link: #0000ee;
    --current-row: #eef7ee;
    --shadow: rgba(0,0,0,0.15);
    --plot-bg: #ffffff;
    --plot-grid: #dddddd;
    --plot-border: #bbbbbb;
    --plot-line: #333333;
    --plot-text: #222222;
  }

  html[data-theme="dark"] {
    color-scheme: dark;
    --page-bg: #151515;
    --card-bg: #242424;
    --text: #eeeeee;
    --muted: #b7b7b7;
    --border: #4a4a4a;
    --header-bg: #303030;
    --button-bg: #4a4a4a;
    --button-text: #ffffff;
    --input-bg: #2b2b2b;
    --input-border: #666666;
    --link: #8ab4f8;
    --current-row: #263b2b;
    --shadow: rgba(0,0,0,0.35);
    --plot-bg: #1d1d1d;
    --plot-grid: #444444;
    --plot-border: #666666;
    --plot-line: #8ab4f8;
    --plot-text: #dddddd;
  }

  * {
    box-sizing: border-box;
  }

  body {
    font-family: Arial, Helvetica, sans-serif;
    background: var(--page-bg);
    color: var(--text);
    margin: 0;
    padding: 20px;
  }

  .container {
    max-width: 760px;
    margin: auto;
  }

  h1, h2 {
    text-align: center;
  }

  .card {
    background: var(--card-bg);
    border-radius: 10px;
    padding: 20px;
    margin-top: 20px;
    box-shadow: 0 2px 8px var(--shadow);
  }

  .row {
    display: flex;
    justify-content: space-between;
    border-bottom: 1px solid var(--border);
    padding: 12px 0;
    gap: 16px;
  }

  .row:last-child {
    border-bottom: none;
  }

  .label {
    font-weight: bold;
  }

  .value {
    text-align: right;
    overflow-wrap: anywhere;
  }

  .button {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    box-sizing: border-box;
    min-height: 44px;
    margin: 20px 6px 0 6px;
    padding: 12px 20px;
    border: 0;
    background: var(--button-bg);
    color: var(--button-text);
    text-decoration: none;
    border-radius: 6px;
    font: inherit;
    line-height: 1.2;
    cursor: pointer;
    vertical-align: middle;
  }

  .button:disabled {
    opacity: 0.65;
    cursor: default;
  }

  .buttons {
    text-align: center;
  }

  .test-tool-group {
    margin-top: 18px;
  }

  .test-tool-group h3 {
    margin: 0 0 8px 0;
    font-size: 1em;
  }

  .test-tool-actions {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 12px;
  }

  .test-tool-actions form {
    margin: 0;
  }

  .test-tool-actions button {
    min-height: 44px;
    padding: 10px 18px;
  }

  .survey-control-row {
    display: flex;
    flex-wrap: wrap;
    align-items: end;
    gap: 12px;
    margin-top: 14px;
  }

  .survey-control-row .control {
    margin: 0;
  }

  .save-state {
    min-width: 52px;
    color: var(--muted);
    font-size: 0.92em;
    padding-bottom: 10px;
  }

  .table-scroll {
    width: 100%;
    max-width: 100%;
    overflow-x: auto;
    -webkit-overflow-scrolling: touch;
  }

  .table-scroll table {
    width: 100%;
    min-width: 820px;
  }

  table {
    width: 100%;
    border-collapse: collapse;
    margin-top: 10px;
  }

  th, td {
    padding: 10px 8px;
    border-bottom: 1px solid var(--border);
    text-align: left;
  }

  th {
    background: var(--header-bg);
    white-space: nowrap;
  }

  td.address {
    white-space: nowrap;
  }

  td.signal, th.signal {
    text-align: right;
    white-space: nowrap;
  }

  td.security, th.security {
    text-align: center;
    white-space: nowrap;
  }

  tr.current {
    font-weight: bold;
    background: var(--current-row);
  }

  .note {
    color: var(--muted);
    font-size: 0.9em;
    margin-top: 14px;
  }

  .scan-group {
    margin-top: 26px;
  }

  .scan-heading {
    font-weight: bold;
    margin: 0 0 8px 0;
  }

  th.sortable {
    cursor: pointer;
    user-select: none;
  }

  th.sortable:hover {
    text-decoration: underline;
  }

  .plot-wrap {
    width: 100%;
    overflow-x: auto;
  }

  .plot-wrap svg {
    width: 100%;
    min-width: 520px;
    height: auto;
  }

  .plot-bg {
    fill: var(--plot-bg);
    stroke: var(--plot-border);
  }

  .plot-grid {
    stroke: var(--plot-grid);
  }

  .plot-line {
    fill: none;
    stroke: var(--plot-line);
    stroke-width: 2;
  }

  .plot-point {
    fill: var(--plot-line);
  }

  .plot-text {
    fill: var(--plot-text);
  }

  .controls {
    display: flex;
    flex-wrap: wrap;
    align-items: end;
    gap: 12px;
    margin-top: 12px;
  }

  .settings-row {
    display: flex;
    flex-wrap: wrap;
    align-items: flex-end;
    gap: 14px;
    margin-top: 12px;
  }

  .checkbox-stack {
    display: flex;
    flex-direction: column;
    justify-content: center;
    gap: 8px;
    padding-bottom: 2px;
  }

  .checkbox-stack label {
    white-space: nowrap;
  }

  .control {
    display: flex;
    flex-direction: column;
    gap: 5px;
  }

  input[type="number"] {
    width: 100px;
    padding: 9px;
    border: 1px solid var(--input-border);
    border-radius: 5px;
  }

  input[type="text"], input[type="password"] {
    min-width: 220px;
    padding: 9px;
    border: 1px solid var(--input-border);
    border-radius: 5px;
    background: var(--input-bg);
    color: var(--text);
  }

  button {
    padding: 10px 16px;
    border: 0;
    border-radius: 6px;
    background: var(--button-bg);
    color: var(--button-text);
    cursor: pointer;
  }

  .danger {
    background: #7a2d2d;
  }

  .theme-control {
    display: flex;
    justify-content: flex-end;
    align-items: center;
    gap: 8px;
    margin-bottom: 10px;
    color: var(--muted);
    font-size: 0.9em;
  }

  .sticky-interface-card {
    position: sticky;
    top: 0;
    z-index: 1000;
    margin-bottom: 14px;
    padding: 10px 12px;
    border: 1px solid var(--border);
    border-radius: 8px;
    background: var(--card-bg);
  }

  .site-header {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    margin-bottom: 10px;
  }

  #rssi-plot {
    scroll-margin-top: 150px;
  }

  .site-title {
    font-weight: bold;
    font-size: 1.05em;
  }

  .header-actions {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 12px;
  }

  .nav {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
  }

  .live-control {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    white-space: nowrap;
    color: var(--text);
    font-size: 0.92em;
  }

  .live-control input {
    width: auto;
    margin: 0;
  }

  .scan-state {
    display: inline-block;
    margin-left: 8px;
    color: var(--muted);
    font-size: 0.92em;
  }

  .scan-state.active {
    font-weight: 600;
  }

  .nav a {
    padding: 8px 11px;
    border-radius: 6px;
    text-decoration: none;
    color: var(--text);
    border: 1px solid var(--border);
    background: var(--card-bg);
  }

  .nav a.active {
    background: var(--button-bg);
    color: var(--button-text);
  }

  .badge {
    display: inline-block;
    padding: 3px 7px;
    border: 1px solid var(--border);
    border-radius: 999px;
    font-size: 0.62em;
    font-weight: normal;
    vertical-align: middle;
    color: var(--muted);
  }

  .status-pass { font-weight: bold; }
  .status-warn { font-weight: bold; }
  .status-fail { font-weight: bold; }

  .interface-controls {
    display: flex;
    flex-wrap: wrap;
    justify-content: flex-end;
    align-items: center;
    gap: 14px;
    margin-bottom: 10px;
  }

  .view-control, .theme-control {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-bottom: 0;
    color: var(--muted);
    font-size: 0.9em;
  }

  .diagnostic-details {
    margin-top: 14px;
    border-top: 1px solid var(--border);
    padding-top: 10px;
  }

  .diagnostic-details > summary {
    cursor: pointer;
    font-weight: 600;
    color: var(--text);
    list-style-position: outside;
  }

  .diagnostic-details > summary .diagnostic-summary {
    margin-left: 8px;
    color: var(--muted);
    font-weight: normal;
    font-size: 0.9em;
  }

  .diagnostic-details[open] > summary {
    margin-bottom: 8px;
  }

  .diagnostic-body {
    margin-top: 4px;
  }

  .diagnostic-warning {
    margin-top: 10px;
    padding: 9px 10px;
    border: 1px solid var(--border);
    border-radius: 6px;
    font-weight: 600;
  }

  .theme-control select, .view-control select {
    min-width: 110px;
    padding: 7px;
    border: 1px solid var(--input-border);
    border-radius: 5px;
    background: var(--input-bg);
    color: var(--text);
  }

  .footer {
    text-align: center;
    font-size: 0.8em;
    margin-top: 25px;
    color: #777;
  }

  /* View-depth model: Standard < Advanced < Developer. */
  .advanced-only, .developer-only { display: none !important; }
  html[data-view="advanced"] div.advanced-only,
  html[data-view="developer"] div.advanced-only,
  html[data-view="developer"] div.developer-only { display: block !important; }
  html[data-view="advanced"] .row.advanced-only,
  html[data-view="developer"] .row.advanced-only,
  html[data-view="developer"] .row.developer-only { display: flex !important; }
  html[data-view="advanced"] form.advanced-only,
  html[data-view="developer"] form.advanced-only,
  html[data-view="developer"] form.developer-only { display: flex !important; }
  html[data-view="advanced"] tr.advanced-only,
  html[data-view="developer"] tr.advanced-only,
  html[data-view="developer"] tr.developer-only { display: table-row !important; }
  html[data-view="advanced"] th.advanced-only, html[data-view="advanced"] td.advanced-only,
  html[data-view="developer"] th.advanced-only, html[data-view="developer"] td.advanced-only,
  html[data-view="developer"] th.developer-only, html[data-view="developer"] td.developer-only { display: table-cell !important; }
  .status-neutral { font-weight: bold; }

  @media (max-width: 600px) {
    table {
      font-size: 0.9em;
    }

    th, td {
      padding: 8px 5px;
    }
  }
</style>
)rawliteral";
}


// ============================================================
// Web Wi-Fi scan page and logger controls
// ============================================================

// Purpose: Quotes and escapes a value for safe CSV output.
String csvEscape(const String& input) {
  String output = input;
  output.replace("\"", "\"\"");
  return "\"" + output + "\"";
}

// Purpose: Appends CSV text to a streaming buffer and flushes when the buffer reaches its target size.
void appendCsvBuffered(String& buffer, const String& text, size_t& bytesSent) {
  if (buffer.length() > 0 && buffer.length() + text.length() > CSV_STREAM_BUFFER_BYTES) {
    size_t pendingBytes = buffer.length();
    diagnosticSendContent(buffer);
    bytesSent += pendingBytes;
    buffer.remove(0);
  }

  if (text.length() > CSV_STREAM_BUFFER_BYTES) {
    diagnosticSendContent(text);
    bytesSent += text.length();
    return;
  }

  buffer += text;
}

// Purpose: Sends any pending CSV buffer content to the HTTP client and resets the buffer.
void flushCsvBuffer(String& buffer, size_t& bytesSent) {
  if (buffer.length() == 0) return;
  size_t pendingBytes = buffer.length();
  diagnosticSendContent(buffer);
  bytesSent += pendingBytes;
  buffer.remove(0);
}

// Purpose: Escapes dynamic text before embedding it inside generated JavaScript string literals.
String jsEscape(String input) {
  input.replace("\\", "\\\\");
  input.replace("'", "\\'");
  input.replace("\r", "");
  input.replace("\n", "\\n");
  return input;
}

// Purpose: Sends an HTTP redirect back to the Wi-Fi Survey page.
void redirectToScanPage() {
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

// Purpose: Returns current Wi-Fi survey, history, network, and diagnostic values for lightweight browser live updates.
void handleWifiScanStatus() {
  bool connected = WiFi.status() == WL_CONNECTED;
  String oldestLabel = "Never";
  String newestLabel = "Never";
  String windowLabel = "-";
  if (historyCount > 0) {
    const ScanRecord& oldest = historyRecord(0);
    const ScanRecord& newest = historyRecord(historyCount - 1);
    oldestLabel = observationAgeLabel(oldest.uptimeMs);
    newestLabel = observationAgeLabel(newest.uptimeMs);
    windowLabel = retainedWindowLabel(oldest.uptimeMs, newest.uptimeMs);
  }
  String json = "{";
  json += "\"scan\":" + String(scanCounter);
  json += ",\"records\":" + String(historyCount);
  json += ",\"capacity\":" + String(scanHistoryCapacity);
  json += ",\"retainedScans\":" + String(countRetainedScanGroups());
  json += ",\"lastScan\":" + jsonQuoted(scanCounter ? observationAgeLabel(lastScanUptimeMs) : String("Never"));
  json += ",\"oldestData\":" + jsonQuoted(oldestLabel);
  json += ",\"newestData\":" + jsonQuoted(newestLabel);
  json += ",\"retainedWindow\":" + jsonQuoted(windowLabel);
  json += ",\"interval\":" + String(scanIntervalSeconds);
  json += ",\"scanning\":" + String(wifiScanInProgress ? "true" : "false");
  json += ",\"scanStatus\":" + jsonQuoted(wifiScanStatusMessage);
  json += ",\"automaticScan\":" + String(wifiCurrentScanAutomatic ? "true" : "false");
  json += ",\"autoDiagnostic\":" + jsonQuoted(wifiAutoScanDiagnosticLabel());
  json += ",\"autoStarts\":" + String(wifiAutoScanStartCount);
  json += ",\"autoCompletions\":" + String(wifiAutoScanCompletionCount);
  json += ",\"autoStartFailures\":" + String(wifiAutoScanStartFailureCount);
  json += ",\"autoCompletionFailures\":" + String(wifiAutoScanCompletionFailureCount);
  json += ",\"lastAutoStart\":" + jsonQuoted(wifiAutoScanLastStartLabel());
  json += ",\"lastAutoCompletion\":" + jsonQuoted(wifiAutoScanLastCompletionLabel());
  json += ",\"scanDuration\":" + jsonQuoted(wifiScanDurationSummaryLabel());
  json += ",\"autoRetryPending\":" + String(wifiAutoScanRetryPending ? "true" : "false");
  json += ",\"interactionDeferred\":" + String(userInteractionDeferActive() ? "true" : "false");
  json += ",\"csvExportInProgress\":" + String(csvExportInProgress ? "true" : "false");
  json += ",\"csvExports\":" + String(wifiCsvExportCount);
  json += ",\"lastCsv\":" + jsonQuoted(wifiCsvExportCount ? csvExportSummaryLabel(wifiCsvLastRows, wifiCsvLastBytes, wifiCsvLastDurationMs) : String("Never"));
  json += ",\"apCount\":" + String(wifiApCount);
  json += ",\"apCapacity\":" + String(wifiApTableCapacity);
  json += ",\"apDrops\":" + String(wifiApTableFullDrops);
  json += ",\"apReclaims\":" + String(wifiApReclamationCount);
  json += ",\"lastFound\":" + String(wifiLastScanFound);
  json += ",\"lastLogged\":" + String(wifiLastScanLogged);
  json += ",\"lastDropped\":" + String(wifiLastScanDropped);
  json += ",\"lastNewAps\":" + String(wifiLastScanNewAps);
  json += ",\"lastSeenAps\":" + String(wifiLastScanPreviouslySeenAps);
  json += ",\"lastHiddenSkipped\":" + String(wifiLastScanHiddenSkipped);
  json += ",\"lastReclaimedAps\":" + String(wifiLastScanReclaimedAps);
  json += ",\"historyIntegrityAnomalies\":" + String(wifiHistoryIntegrityAnomalies());
  json += ",\"freeHeap\":" + String(ESP.getFreeHeap());
  json += ",\"largestBlock\":" + String(diagnosticLargestFreeBlock());
  json += ",\"connected\":" + String(connected ? "true" : "false");
  json += ",\"stationSSID\":" + jsonQuoted(connected ? WiFi.SSID() : String(""));
  json += ",\"stationBSSID\":" + jsonQuoted(connected ? WiFi.BSSIDstr() : String(""));
  json += ",\"stationRssi\":" + String(connected ? WiFi.RSSI() : 0);
  json += ",\"stationChannel\":" + String(connected ? WiFi.channel() : 0);
  json += ",\"live\":" + String(webAutoRefreshEnabled ? "true" : "false");
  json += "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

// Purpose: HTTP endpoint that requests an immediate asynchronous Wi-Fi scan.
void handleWebScanNow() {
  markExplicitUserInteraction();
  bool started = beginLoggedWifiScan(false, false);
  if (!started) {
    wifiScanStatusMessage = wifiScanInProgress ? "Scan already in progress" : "Unable to start scan";
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(started || wifiScanInProgress ? 202 : 503, "application/json",
    String("{\"started\":") + (started ? "true" : "false") +
    ",\"scanning\":" + (wifiScanInProgress ? "true" : "false") +
    ",\"message\":\"" + jsEscape(wifiScanStatusMessage) + "\"}");
}

// Purpose: HTTP endpoint that persists the global Live Updates checkbox state.
void handleLiveUpdatesSetting() {
  markExplicitUserInteraction();
  if (!server.hasArg("enabled")) {
    server.send(400, "text/plain", "Missing enabled value.");
    return;
  }
  bool enabled = server.arg("enabled") == "1";
  saveWebLiveUpdates(enabled);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json",
    String("{\"enabled\":") + (enabled ? "true" : "false") + "}");
}

// Purpose: Validates, applies, and persists a Wi-Fi scan interval supplied by an HTTP request.
bool applyWifiScanIntervalFromRequest() {
  if (!server.hasArg("interval")) return false;

  long requested = server.arg("interval").toInt();
  if (requested < (long)MIN_SCAN_INTERVAL_SECONDS)
    requested = MIN_SCAN_INTERVAL_SECONDS;
  if (requested > (long)MAX_SCAN_INTERVAL_SECONDS)
    requested = MAX_SCAN_INTERVAL_SECONDS;

  scanIntervalSeconds = (unsigned long)requested;
  lastAutoScanMs = millis();

  preferences.begin("survey", false);
  preferences.putULong("wifiInterval", scanIntervalSeconds);
  preferences.end();

  return true;
}

// Purpose: Legacy Wi-Fi settings endpoint that applies the interval and redirects to the survey page.
void handleScanSettings() {
  markExplicitUserInteraction();
  applyWifiScanIntervalFromRequest();
  redirectToScanPage();
}

// Purpose: JSON endpoint used by the Wi-Fi page to save scan interval without reloading the page.
void handleWifiIntervalSetting() {
  markExplicitUserInteraction();
  bool accepted = applyWifiScanIntervalFromRequest();

  server.sendHeader("Cache-Control", "no-store");
  if (!accepted) {
    server.send(400, "application/json", "{\"saved\":false,\"message\":\"Missing interval\"}");
    return;
  }

  server.send(
    200,
    "application/json",
    String("{\"saved\":true,\"interval\":") + String(scanIntervalSeconds) + "}"
  );
}

// Purpose: Clears Wi-Fi history and redirects back to the Wi-Fi Survey page.
void handleClearScanHistory() {
  markExplicitUserInteraction();
  clearScanHistory();
  redirectToScanPage();
}

// Purpose: Streams retained Wi-Fi observations as CSV while pausing new automatic scan starts during export.
void handleScanCsv() {
  markExplicitUserInteraction();
  csvExportInProgress = true;

  const size_t exportCount = historyCount;
  const uint32_t exportStartMs = millis();
  size_t bytesSent = 0;

  server.sendHeader(
    "Content-Disposition",
    "attachment; filename=\"wifi_scan_log.csv\""
  );

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");

  String buffer;
  buffer.reserve(CSV_STREAM_BUFFER_BYTES + 256);
  appendCsvBuffered(
    buffer,
    "scan,uptime_ms,uptime,ssid,bssid,channel,rssi_dbm,security,connected,hidden\r\n",
    bytesSent
  );

  int connectedApIndex = -1;
  if (WiFi.status() == WL_CONNECTED) {
    connectedApIndex = findWifiApByTextBssid(WiFi.BSSIDstr());
  }

  for (size_t i = 0; i < exportCount; i++) {
    const ScanRecord& record = historyRecord(i);
    const WifiObservation& observation = compactHistoryRecord(i);

    String line;
    line.reserve(180);

    line += String(record.scanNumber);
    line += ",";
    line += String(record.uptimeMs);
    line += ",";
    line += csvEscape(formatUptime(record.uptimeMs));
    line += ",";
    line += csvEscape(String(record.ssid));
    line += ",";
    line += csvEscape(String(record.bssid));
    line += ",";
    line += String(record.channel);
    line += ",";
    line += String(record.rssi);
    line += ",";
    line += csvEscape(
      securityLabel((wifi_auth_mode_t)record.authMode)
    );
    line += ",";
    line += (connectedApIndex >= 0 && observation.apIndex == (uint16_t)connectedApIndex) ? "YES" : "NO";
    line += ",";
    line += record.hidden ? "YES" : "NO";
    line += "\r\n";

    appendCsvBuffered(buffer, line, bytesSent);
  }

  flushCsvBuffer(buffer, bytesSent);
  diagnosticSendContent("");

  wifiCsvExportCount++;
  wifiCsvLastRows = exportCount;
  wifiCsvLastBytes = bytesSent;
  wifiCsvLastDurationMs = millis() - exportStartMs;
  csvExportInProgress = false;
}

// Purpose: Generates and streams the SVG RSSI history plot for one selected Wi-Fi BSSID.
void sendRssiHistoryPlot(const String& selectedBssid) {
  const int SVG_WIDTH = 720;
  const int SVG_HEIGHT = 280;

  const int LEFT = 58;
  const int RIGHT = 20;
  const int TOP = 20;
  const int BOTTOM = 45;

  const int plotWidth = SVG_WIDTH - LEFT - RIGHT;
  const int plotHeight = SVG_HEIGHT - TOP - BOTTOM;

  // Fixed RSSI range keeps plots comparable between locations.
  const int RSSI_TOP = -30;
  const int RSSI_BOTTOM = -100;

  int selectedApIndex = findWifiApByTextBssid(selectedBssid);
  if (selectedApIndex < 0) {
    diagnosticSendContent(
      "<p>No logged RSSI samples are available for the selected BSSID.</p>"
    );
    return;
  }

  uint32_t firstMs = 0;
  uint32_t lastMs = 0;
  size_t pointCount = 0;

  for (size_t i = 0; i < historyCount; i++) {
    const WifiObservation& observation = compactHistoryRecord(i);
    if (observation.apIndex != (uint16_t)selectedApIndex) continue;
    if (observation.scanSlot >= wifiScanMetadataCapacity) continue;

    const WifiScanMetadata& scan = wifiScanMetadata[observation.scanSlot];
    if (scan.scanNumber == 0) continue;

    if (pointCount == 0) firstMs = scan.uptimeMs;
    lastMs = scan.uptimeMs;
    pointCount++;
  }

  if (pointCount == 0) {
    diagnosticSendContent(
      "<p>No logged RSSI samples are available for the selected BSSID.</p>"
    );
    return;
  }

  if (lastMs <= firstMs) {
    lastMs = firstMs + 1;
  }

  diagnosticSendContent(
    "<div class=\"plot-wrap\">"
    "<svg viewBox=\"0 0 720 280\" role=\"img\" "
    "aria-label=\"RSSI history for selected access point\">"
  );

  // Background and horizontal grid lines.
  diagnosticSendContent(
    "<rect class=\"plot-bg\" x=\"58\" y=\"20\" width=\"642\" height=\"215\"/>"
  );

  for (int rssi = -100; rssi <= -30; rssi += 10) {
    int y = TOP +
      ((RSSI_TOP - rssi) * plotHeight) /
      (RSSI_TOP - RSSI_BOTTOM);

    String grid;
    grid.reserve(180);

    grid += "<line x1=\"";
    grid += String(LEFT);
    grid += "\" y1=\"";
    grid += String(y);
    grid += "\" x2=\"";
    grid += String(SVG_WIDTH - RIGHT);
    grid += "\" y2=\"";
    grid += String(y);
    grid += "\" class=\"plot-grid\" stroke-width=\"1\"/>";

    grid += "<text x=\"";
    grid += String(LEFT - 8);
    grid += "\" y=\"";
    grid += String(y + 4);
    grid += "\" class=\"plot-text\" text-anchor=\"end\" font-size=\"11\">";
    grid += String(rssi);
    grid += "</text>";

    diagnosticSendContent(grid);
  }

  // Polyline points.
  String points;
  points.reserve(pointCount * 14);

  for (size_t i = 0; i < historyCount; i++) {
    const WifiObservation& observation = compactHistoryRecord(i);
    if (observation.apIndex != (uint16_t)selectedApIndex) continue;
    if (observation.scanSlot >= wifiScanMetadataCapacity) continue;

    const WifiScanMetadata& scan = wifiScanMetadata[observation.scanSlot];
    if (scan.scanNumber == 0) continue;

    int x = LEFT +
      (uint64_t)(scan.uptimeMs - firstMs) * plotWidth /
      (lastMs - firstMs);

    int clippedRssi = observation.rssi;

    if (clippedRssi > RSSI_TOP) {
      clippedRssi = RSSI_TOP;
    }

    if (clippedRssi < RSSI_BOTTOM) {
      clippedRssi = RSSI_BOTTOM;
    }

    int y = TOP +
      ((RSSI_TOP - clippedRssi) * plotHeight) /
      (RSSI_TOP - RSSI_BOTTOM);

    if (points.length() > 0) {
      points += " ";
    }

    points += String(x);
    points += ",";
    points += String(y);
  }

  String polyline;
  polyline.reserve(points.length() + 120);
  polyline =
    "<polyline class=\"plot-line\" points=\"";
  polyline += points;
  polyline += "\"/>";

  diagnosticSendContent(polyline);

  // Individual samples. Buffer several SVG circles per send so a dense plot
  // does not turn into hundreds of tiny TCP writes.
  String dotBuffer;
  dotBuffer.reserve(CSV_STREAM_BUFFER_BYTES + 256);

  for (size_t i = 0; i < historyCount; i++) {
    const WifiObservation& observation = compactHistoryRecord(i);
    if (observation.apIndex != (uint16_t)selectedApIndex) continue;
    if (observation.scanSlot >= wifiScanMetadataCapacity) continue;

    const WifiScanMetadata& scan = wifiScanMetadata[observation.scanSlot];
    if (scan.scanNumber == 0) continue;

    int x = LEFT +
      (uint64_t)(scan.uptimeMs - firstMs) * plotWidth /
      (lastMs - firstMs);

    int clippedRssi = observation.rssi;

    if (clippedRssi > RSSI_TOP) {
      clippedRssi = RSSI_TOP;
    }

    if (clippedRssi < RSSI_BOTTOM) {
      clippedRssi = RSSI_BOTTOM;
    }

    int y = TOP +
      ((RSSI_TOP - clippedRssi) * plotHeight) /
      (RSSI_TOP - RSSI_BOTTOM);

    String dot;
    dot.reserve(220);

    dot += "<circle cx=\"";
    dot += String(x);
    dot += "\" cy=\"";
    dot += String(y);
    dot += "\" r=\"4\" class=\"plot-point\">";
    dot += "<title>Scan #";
    dot += String(scan.scanNumber);
    dot += " | ";
    dot += htmlEscape(formatUptime(scan.uptimeMs));
    dot += " | ";
    dot += String(observation.rssi);
    dot += " dBm</title></circle>";

    if (
      dotBuffer.length() > 0 &&
      dotBuffer.length() + dot.length() > CSV_STREAM_BUFFER_BYTES
    ) {
      diagnosticSendContent(dotBuffer);
      dotBuffer = "";
    }

    dotBuffer += dot;
  }

  if (dotBuffer.length() > 0) diagnosticSendContent(dotBuffer);

  String labels;
  labels.reserve(400);

  labels += "<text x=\"";
  labels += String(LEFT);
  labels += "\" y=\"";
  labels += String(SVG_HEIGHT - 18);
  labels += "\" class=\"plot-text\" text-anchor=\"start\" font-size=\"11\">";
  labels += htmlEscape(formatUptime(firstMs));
  labels += "</text>";

  labels += "<text x=\"";
  labels += String(SVG_WIDTH - RIGHT);
  labels += "\" y=\"";
  labels += String(SVG_HEIGHT - 18);
  labels += "\" class=\"plot-text\" text-anchor=\"end\" font-size=\"11\">";
  labels += htmlEscape(formatUptime(lastMs));
  labels += "</text>";

  labels +=
    "<text x=\"15\" y=\"128\" transform=\"rotate(-90 15 128)\" "
    "text-anchor=\"middle\" font-size=\"12\">RSSI (dBm)</text>";

  diagnosticSendContent(labels);
  diagnosticSendContent("</svg></div>");
}


// Purpose: Finds an AP-table slot by matching a textual BSSID.
int findWifiApByTextBssid(const String& bssid) {
  char textBssid[18];

  for (size_t i = 0; i < wifiApCount; i++) {
    formatBssid(wifiApTable[i].bssid, textBssid);
    if (bssid.equalsIgnoreCase(textBssid)) return (int)i;
  }

  return -1;
}


bool buildNetworkSummaryByApIndex(
  uint16_t apIndex,
  NetworkSummary& summary
) {
  summary = {};
  resetSignalStats(summary.signal);

  if (
    wifiApTable == nullptr ||
    wifiScanMetadata == nullptr ||
    scanHistory == nullptr ||
    apIndex >= wifiApCount
  ) {
    return false;
  }

  bool found = false;

  // Compact observations already contain the AP-table index. Walking them
  // directly avoids synthesizing ScanRecord/String objects for every history
  // comparison, which was the dominant full-history Wi-Fi page cost.
  for (size_t i = 0; i < historyCount; i++) {
    const WifiObservation& observation = compactHistoryRecord(i);
    if (observation.apIndex != apIndex) continue;
    if (observation.scanSlot >= wifiScanMetadataCapacity) continue;

    const WifiScanMetadata& scan = wifiScanMetadata[observation.scanSlot];
    if (scan.scanNumber == 0) continue;

    found = true;
    addSignalObservation(
      summary.signal,
      observation.rssi,
      scan.uptimeMs
    );
  }

  if (!found) return false;

  const WifiApEntry& ap = wifiApTable[apIndex];

  memcpy(summary.ssid, ap.ssid, sizeof(summary.ssid));
  formatBssid(ap.bssid, summary.bssid);
  summary.channel = ap.channel;
  summary.authMode = ap.authMode;
  summary.hidden = (ap.ssid[0] == '\0');

  summary.connected = false;
  if (WiFi.status() == WL_CONNECTED) {
    const uint8_t* connectedBssid = WiFi.BSSID();
    summary.connected =
      connectedBssid != nullptr &&
      memcmp(connectedBssid, ap.bssid, 6) == 0;
  }

  return true;
}


bool buildNetworkSummary(
  const String& bssid,
  NetworkSummary& summary
) {
  int apIndex = findWifiApByTextBssid(bssid);
  if (apIndex < 0) return false;
  return buildNetworkSummaryByApIndex((uint16_t)apIndex, summary);
}

// Purpose: Streams the sortable Observed Networks table with columns progressively exposed by view depth.
void sendNetworkSummaryTable() {
  if (historyCount == 0) {
    diagnosticSendContent("<p>No networks have been observed yet.</p>");
    return;
  }

  diagnosticSendContent(
    "<div class=\"table-scroll\"><table id=\"network-summary\">"
    "<thead><tr>"
    "<th class=\"sortable\" onclick=\"sortTable('network-summary',0,'text')\">SSID</th>"
    "<th class=\"sortable signal\" onclick=\"sortTable('network-summary',1,'number')\">CH</th>"
    "<th class=\"sortable signal\" onclick=\"sortTable('network-summary',2,'number')\">Last</th>"
    "<th class=\"sortable signal\" onclick=\"sortTable('network-summary',3,'number')\">Avg</th>"
    "<th class=\"sortable signal\" onclick=\"sortTable('network-summary',4,'number')\">Count</th>"
    "<th class=\"sortable\" onclick=\"sortTable('network-summary',5,'number')\">Last Seen</th>"
    "<th class=\"sortable signal advanced-only\" onclick=\"sortTable('network-summary',6,'number')\">Min</th>"
    "<th class=\"sortable signal advanced-only\" onclick=\"sortTable('network-summary',7,'number')\">Max</th>"
    "<th class=\"sortable developer-only\" onclick=\"sortTable('network-summary',8,'text')\">BSSID</th>"
    "<th class=\"sortable developer-only\" onclick=\"sortTable('network-summary',9,'text')\">Security</th>"
    "<th class=\"sortable developer-only\" onclick=\"sortTable('network-summary',10,'number')\">First Seen</th>"
    "</tr></thead><tbody>"
  );

  // Walk newest-to-oldest and emit each AP once. The compact observation's
  // AP index is the deduplication key, preserving linear-time summary generation.
  bool emittedAp[512] = {};
  String tableBuffer;
  tableBuffer.reserve(CSV_STREAM_BUFFER_BYTES + 256);

  for (size_t offset = 0; offset < historyCount; offset++) {
    size_t logicalIndex = historyCount - 1 - offset;
    const WifiObservation& latestObservation = compactHistoryRecord(logicalIndex);
    uint16_t apIndex = latestObservation.apIndex;

    if (apIndex >= wifiApCount || apIndex >= 512) continue;
    if (emittedAp[apIndex]) continue;
    emittedAp[apIndex] = true;

    NetworkSummary summary = {};
    if (!buildNetworkSummaryByApIndex(apIndex, summary)) continue;

    String plotUrl = "/scan?plot=" + urlEncode(String(summary.bssid)) + "#rssi-plot";
    String displaySSID = summary.hidden ? "(hidden)" : String(summary.ssid);
    float avgRssi = averageSignal(summary.signal);
    uint32_t nowMs = surveySessionUptimeMs();
    uint32_t firstAgeMs = nowMs >= summary.signal.firstSeenMs ? nowMs - summary.signal.firstSeenMs : 0;
    uint32_t lastAgeMs = nowMs >= summary.signal.lastSeenMs ? nowMs - summary.signal.lastSeenMs : 0;

    String row;
    row.reserve(950);
    row += summary.connected ? "<tr class=\"current\">" : "<tr>";
    row += "<td class=\"address\"><a href=\"" + plotUrl + "\">" + htmlEscape(displaySSID);
    if (summary.connected) row += " (connected)";
    row += "</a></td>";
    row += "<td class=\"signal\" data-sort=\"" + String(summary.channel) + "\">" + String(summary.channel) + "</td>";
    row += "<td class=\"signal\" data-sort=\"" + String(summary.signal.latestRssi) + "\">" + String(summary.signal.latestRssi) + " dBm</td>";
    row += "<td class=\"signal\" data-sort=\"" + String(avgRssi, 1) + "\">" + String(avgRssi, 1) + " dBm</td>";
    row += "<td class=\"signal\" data-sort=\"" + String(summary.signal.samples) + "\">" + String(summary.signal.samples) + "</td>";
    row += "<td data-sort=\"" + String(lastAgeMs) + "\">" + htmlEscape(observationAgeLabel(summary.signal.lastSeenMs)) + "</td>";
    row += "<td class=\"signal advanced-only\" data-sort=\"" + String(summary.signal.minRssi) + "\">" + String(summary.signal.minRssi) + " dBm</td>";
    row += "<td class=\"signal advanced-only\" data-sort=\"" + String(summary.signal.maxRssi) + "\">" + String(summary.signal.maxRssi) + " dBm</td>";
    row += "<td class=\"developer-only\"><a href=\"" + plotUrl + "\">" + htmlEscape(String(summary.bssid)) + "</a></td>";
    row += "<td class=\"developer-only\">" + htmlEscape(securityLabel((wifi_auth_mode_t)summary.authMode)) + "</td>";
    row += "<td class=\"developer-only\" data-sort=\"" + String(firstAgeMs) + "\">" + htmlEscape(observationAgeLabel(summary.signal.firstSeenMs)) + "</td>";
    row += "</tr>";

    if (tableBuffer.length() > 0 && tableBuffer.length() + row.length() > CSV_STREAM_BUFFER_BYTES) {
      diagnosticSendContent(tableBuffer);
      tableBuffer = "";
    }
    tableBuffer += row;
  }

  if (tableBuffer.length() > 0) diagnosticSendContent(tableBuffer);
  diagnosticSendContent("</tbody></table></div>");
}

void sendThemeControl();

// Purpose: Returns early JavaScript that applies the saved theme/view before the page renders, reducing visual flash.
String themeBootstrapScript() {
  return
    "<script>"
    "(function(){"
      "try{"
        "var v=localStorage.getItem('esp32-theme')||'system';"
        "var d=(v==='system')"
          "?(window.matchMedia&&window.matchMedia('(prefers-color-scheme: dark)').matches)"
          ":(v==='dark');"
        "document.documentElement.dataset.theme=d?'dark':'light';"
        "var w=localStorage.getItem('esp32-view')||'standard';"
        "w=(w==='developer'||w==='advanced')?w:'standard';"
        "document.documentElement.dataset.view=w;"
      "}catch(e){}"
    "})();"
    "</script>";
}

// Purpose: Streams the theme/view bootstrap JavaScript into the current page.
void sendThemeBootstrapScript() {
  diagnosticSendContent(themeBootstrapScript());
}

// Purpose: Returns the HTML class attribute used to highlight the active top-level navigation item.
String activeNavClass(const String& active, const char* item) {
  return active == item ? " class=\"active\"" : "";
}

// Purpose: Streams the common site header, navigation links, Live Updates control, and view/theme controls.
void sendSiteNavigation(const String& active) {
  String nav;
  nav.reserve(1400);
  nav += "<div class=\"sticky-interface-card\"><div class=\"site-header\"><div class=\"site-title\">ESP32 Wireless Surveyor</div><div class=\"header-actions\"><nav class=\"nav\">";
  nav += "<a href=\"/\"" + activeNavClass(active, "wifi") + ">Wi-Fi</a>";
  nav += "<a href=\"/ble\"" + activeNavClass(active, "ble") + ">Bluetooth</a>";
  nav += "<a href=\"/system\"" + activeNavClass(active, "system") + ">System</a>";
  nav += "<a href=\"/settings\"" + activeNavClass(active, "settings") + ">Settings</a>";
  nav += "</nav><label class=\"live-control\"><input id=\"live-updates-toggle\" type=\"checkbox\"";
  if (webAutoRefreshEnabled) nav += " checked";
  nav += "> Live updates</label></div></div>";
  diagnosticSendContent(nav);
  sendThemeControl();
  diagnosticSendContent("</div>");
  diagnosticSendContent(
    "<script>(function(){"
    "const c=document.getElementById('live-updates-toggle');if(!c)return;"
    "c.addEventListener('change',function(){"
      "const v=c.checked?'1':'0';"
      "fetch('/api/live-updates',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'enabled='+v,cache:'no-store'})"
      ".catch(function(){c.checked=!c.checked;});"
    "});"
    "})();</script>"
  );
}

// Purpose: Streams the Standard/Advanced/Developer view selector and System/Light/Dark theme selector.
void sendThemeControl() {
  diagnosticSendContent(
    "<div class=\"interface-controls\">"
    "<div class=\"view-control\">"
    "<label for=\"view-select\">View</label>"
    "<select id=\"view-select\" class=\"view-select\" onchange=\"setViewMode(this.value)\">"
    "<option value=\"standard\">Standard</option>"
    "<option value=\"advanced\">Advanced</option>"
    "<option value=\"developer\">Developer</option>"
    "</select></div>"
    "<div class=\"theme-control\">"
    "<label for=\"theme-select\">Theme</label>"
    "<select id=\"theme-select\" class=\"theme-select\" onchange=\"setTheme(this.value)\">"
    "<option value=\"system\">System</option>"
    "<option value=\"light\">Light</option>"
    "<option value=\"dark\">Dark</option>"
    "</select></div></div>"
  );
}

// Purpose: Streams browser-side JavaScript for changing and persisting theme/view selections.
void sendThemeScript() {
  diagnosticSendContent(
    "<script>"
    "function applyTheme(v){"
      "const r=document.documentElement;"
      "if(v==='system'){"
        "const d=window.matchMedia&&window.matchMedia('(prefers-color-scheme: dark)').matches;"
        "r.dataset.theme=d?'dark':'light';"
      "}else{r.dataset.theme=v;}"
    "}"
    "function setTheme(v){"
      "localStorage.setItem('esp32-theme',v);"
      "applyTheme(v);"
      "document.querySelectorAll('.theme-select').forEach(s=>s.value=v);"
    "}"
    "function applyViewMode(v){"
      "v=(v==='developer'||v==='advanced')?v:'standard';"
      "document.documentElement.dataset.view=v;"
      "document.querySelectorAll('.view-select').forEach(s=>s.value=v);"
    "}"
    "function setViewMode(v){"
      "v=(v==='developer'||v==='advanced')?v:'standard';"
      "localStorage.setItem('esp32-view',v);"
      "applyViewMode(v);"
    "}"
    "document.addEventListener('DOMContentLoaded',()=>{"
      "const v=localStorage.getItem('esp32-theme')||'system';"
      "document.querySelectorAll('.theme-select').forEach(s=>s.value=v);"
      "const w=localStorage.getItem('esp32-view')||'standard';"
      "applyViewMode(w);"
      "if(window.matchMedia){"
        "window.matchMedia('(prefers-color-scheme: dark)').addEventListener?.('change',()=>{"
          "if((localStorage.getItem('esp32-theme')||'system')==='system')applyTheme('system');"
        "});"
      "}"
    "});"
    "</script>"
  );
}

// Purpose: Streams the generic client-side table sorting function used by Wi-Fi and BLE result tables.
void sendSortableTableScript() {
  diagnosticSendContent(
    "<script>"
    "const tableSortState={};"
    "function sortTable(tableId,column,type){"
      "const table=document.getElementById(tableId);"
      "if(!table)return;"
      "const body=table.tBodies[0];"
      "const rows=Array.from(body.rows);"
      "const previous=tableSortState[tableId]||{column:-1,ascending:true};"
      "const ascending=(previous.column===column)?!previous.ascending:true;"
      "rows.sort((a,b)=>{"
        "let av=a.cells[column].dataset.sort??a.cells[column].innerText.trim();"
        "let bv=b.cells[column].dataset.sort??b.cells[column].innerText.trim();"
        "if(type==='number'){"
          "av=parseFloat(av);bv=parseFloat(bv);"
          "if(Number.isNaN(av))av=0;"
          "if(Number.isNaN(bv))bv=0;"
          "return ascending?av-bv:bv-av;"
        "}"
        "av=av.toLowerCase();bv=bv.toLowerCase();"
        "return ascending?av.localeCompare(bv):bv.localeCompare(av);"
      "});"
      "rows.forEach(row=>body.appendChild(row));"
      "tableSortState[tableId]={column:column,ascending:ascending};"
    "}"
    "</script>"
  );
}

struct ChannelAnalysis {
  int apCount[12];
  int strongestRssi[12];
  float coChannelScore[12];
  float adjacentScore[12];
  float totalScore[12];
  int suggestedChannel;
  uint32_t scanNumber;
  bool valid;
};

// Explicit prototype prevents Arduino 1.8.x prototype generation from
// emitting this custom return type before ChannelAnalysis is declared.
ChannelAnalysis analyzeLatestWifiScan();

// Purpose: Converts RSSI into a relative interference weight for channel scoring.
float rssiInterferenceWeight(int rssi) {
  if (rssi <= -100) return 0.0f;
  if (rssi >= -30) return 100.0f;
  return (float)(rssi + 100) * (100.0f / 70.0f);
}

// Purpose: Returns the weighting applied to interference from an AP a given number of channels away.
float channelOverlapFactor(int distance) {
  if (distance == 0) return 1.0f;
  if (distance == 1) return 0.75f;
  if (distance == 2) return 0.50f;
  if (distance == 3) return 0.25f;
  if (distance == 4) return 0.10f;
  return 0.0f;
}

// Purpose: Analyzes the latest retained Wi-Fi scan and computes per-channel observed-interference scores.
ChannelAnalysis analyzeLatestWifiScan() {
  static uint32_t cachedScanCounter = UINT32_MAX;
  static ChannelAnalysis cached = {};
  if (cachedScanCounter == scanCounter) return cached;

  ChannelAnalysis a = {};
  a.suggestedChannel = 0;
  for (int ch = 1; ch <= 11; ch++) a.strongestRssi[ch] = -127;
  if (wifiLatestAnalysisScan == scanCounter && scanCounter > 0) {
    a.scanNumber = scanCounter;
    for (int ch = 1; ch <= 11; ch++) {
      a.apCount[ch] = wifiLatestChannelApCount[ch];
      a.strongestRssi[ch] = wifiLatestChannelStrongestRssi[ch];
      a.coChannelScore[ch] = wifiLatestCoChannelScore[ch];
      a.adjacentScore[ch] = wifiLatestAdjacentScore[ch];
      a.totalScore[ch] = a.coChannelScore[ch] + a.adjacentScore[ch];
      if (a.apCount[ch]) a.valid = true;
    }
    if (a.valid) {
      const int preferred[3] = {1, 6, 11};
      int best = preferred[0];
      for (int i = 1; i < 3; i++) if (a.totalScore[preferred[i]] < a.totalScore[best]) best = preferred[i];
      a.suggestedChannel = best;
    }
    cached = a;
    cachedScanCounter = scanCounter;
    return cached;
  }

  if (historyCount == 0 || scanHistory == nullptr || wifiScanMetadata == nullptr || wifiApTable == nullptr) {
    cached = a;
    cachedScanCounter = scanCounter;
    return cached;
  }

  const WifiObservation& latestObservation = compactHistoryRecord(historyCount - 1);
  if (latestObservation.scanSlot >= wifiScanMetadataCapacity) {
    cached = a;
    cachedScanCounter = scanCounter;
    return cached;
  }

  uint32_t latestScan = wifiScanMetadata[latestObservation.scanSlot].scanNumber;
  a.scanNumber = latestScan;
  for (size_t i = 0; i < historyCount; i++) {
    const WifiObservation& observation = compactHistoryRecord(i);
    if (
      observation.scanSlot >= wifiScanMetadataCapacity ||
      observation.apIndex >= wifiApCount
    ) continue;

    const WifiScanMetadata& metadata = wifiScanMetadata[observation.scanSlot];
    if (metadata.scanNumber != latestScan) continue;

    const WifiApEntry& ap = wifiApTable[observation.apIndex];
    if (ap.channel < 1 || ap.channel > 11) continue;

    a.valid = true;
    a.apCount[ap.channel]++;
    if (observation.rssi > a.strongestRssi[ap.channel]) a.strongestRssi[ap.channel] = observation.rssi;
    float w = rssiInterferenceWeight(observation.rssi);
    for (int candidate = 1; candidate <= 11; candidate++) {
      int d = abs(candidate - ap.channel);
      float overlap = channelOverlapFactor(d);
      if (overlap <= 0.0f) continue;
      if (d == 0) a.coChannelScore[candidate] += w;
      else a.adjacentScore[candidate] += w * overlap;
    }
  }
  if (!a.valid) { cached = a; cachedScanCounter = scanCounter; return cached; }
  for (int ch = 1; ch <= 11; ch++) a.totalScore[ch] = a.coChannelScore[ch] + a.adjacentScore[ch];
  const int preferred[3] = {1, 6, 11};
  int best = preferred[0];
  for (int i = 1; i < 3; i++) if (a.totalScore[preferred[i]] < a.totalScore[best]) best = preferred[i];
  a.suggestedChannel = best;
  cached = a;
  cachedScanCounter = scanCounter;
  return cached;
}

// Purpose: Streams the channel recommendation and, in deeper views, the detailed interference table.
void sendWifiChannelAnalysis() {
  ChannelAnalysis a = analyzeLatestWifiScan();
  diagnosticSendContent("<div class=\"card\"><h2>Observed Channel Interference</h2>");
  if (!a.valid) {
    diagnosticSendContent("<p>No retained Wi-Fi scan is available for channel analysis yet.</p></div>");
    return;
  }
  diagnosticSendContent("<div class=\"row\"><span class=\"label\">Suggested Channel</span><span class=\"value\"><strong>" + String(a.suggestedChannel) + "</strong></span></div>"
    "<div class=\"note\">Suggestion is based on currently observed 2.4 GHz Wi-Fi interference.</div>");
  String detail = "<div class=\"advanced-only\"><div class=\"row\"><span class=\"label\">Based On</span><span class=\"value\">Latest scan #" + String(a.scanNumber) + "</span></div>"
    "<div class=\"note\">This estimate compares visible AP strength plus co-channel and adjacent-channel overlap. Lower score is preferred. It does not measure airtime utilization, traffic load, noise floor, retransmissions, or non-Wi-Fi interference.</div>"
    "<div class=\"table-scroll\"><table><thead><tr><th>Channel</th><th>APs</th><th>Strongest AP</th><th>Co-channel</th><th>Adjacent</th><th>Total score</th></tr></thead><tbody>";
  diagnosticSendContent(detail);
  for (int ch=1; ch<=11; ch++) {
    String row = "<tr";
    if (ch == a.suggestedChannel) row += " class=\"current\"";
    row += "><td>" + String(ch) + "</td><td>" + String(a.apCount[ch]) + "</td><td>";
    row += a.strongestRssi[ch] == -127 ? "-" : String(a.strongestRssi[ch]) + " dBm";
    row += "</td><td>" + String(a.coChannelScore[ch],1) + "</td><td>" + String(a.adjacentScore[ch],1) + "</td><td>" + String(a.totalScore[ch],1) + "</td></tr>";
    diagnosticSendContent(row);
  }
  diagnosticSendContent("</tbody></table></div></div></div>");
}

// Purpose: Builds the Wi-Fi Survey page with consistent survey, analysis, network-context, and diagnostic ordering.
void handleWebScan() {
  beginWebResponseProfile("/");
  markExplicitUserInteraction();
  ensureWiFiStationMode();

  bool connected = WiFi.status() == WL_CONNECTED;
  String connectedSSID = connected ? WiFi.SSID() : "";
  String connectedBSSID = connected ? WiFi.BSSIDstr() : "";
  int connectedRSSI = connected ? WiFi.RSSI() : 0;
  int connectedChannel = connected ? WiFi.channel() : 0;
  String selectedBSSID = "";

  if (server.hasArg("plot")) {
    String requestedBSSID = server.arg("plot");
    requestedBSSID.trim();
    if (historyContainsBSSID(requestedBSSID)) selectedBSSID = requestedBSSID;
  }
  if (selectedBSSID.length() == 0 && connected && historyContainsBSSID(connectedBSSID)) selectedBSSID = connectedBSSID;
  if (selectedBSSID.length() == 0 && historyCount > 0) selectedBSSID = String(historyRecord(historyCount - 1).bssid);

  String selectedSSID = latestSSIDForBSSID(selectedBSSID);
  String oldestLabel = "Never";
  String newestLabel = "Never";
  String windowLabel = "-";
  if (historyCount > 0) {
    const ScanRecord& oldest = historyRecord(0);
    const ScanRecord& newest = historyRecord(historyCount - 1);
    oldestLabel = observationAgeLabel(oldest.uptimeMs);
    newestLabel = observationAgeLabel(newest.uptimeMs);
    windowLabel = retainedWindowLabel(oldest.uptimeMs, newest.uptimeMs);
  }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  diagnosticSendContent("<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>ESP32 Wi-Fi Survey</title>");
  sendThemeBootstrapScript();
  diagnosticSendContent(pageStyles());
  diagnosticSendContent("</head><body><div class=\"container\">");
  sendSiteNavigation("wifi");
  diagnosticSendContent("<h1>Wi-Fi Survey</h1>");
  markWebResponsePhase("header");

  String card;
  card.reserve(3600);
  card += "<div class=\"card\"><h2>Survey Status &amp; Controls</h2>"
    "<div class=\"row\"><span class=\"label\">Surveying</span><span class=\"value\">Always On</span></div>"
    "<div class=\"row\"><span class=\"label\">Scans This Session</span><span id=\"wifi-scans-session\" class=\"value\">" + String(scanCounter) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Last Scan</span><span id=\"wifi-last-scan\" class=\"value\">" + (scanCounter ? htmlEscape(observationAgeLabel(lastScanUptimeMs)) : String("Never")) + "</span></div>"
    "<div class=\"survey-control-row\"><div class=\"control\"><label for=\"interval\">Scan Interval (seconds)</label>"
    "<input id=\"interval\" type=\"number\" min=\"5\" max=\"3600\" value=\"" + String(scanIntervalSeconds) + "\"></div>"
    "<button class=\"button\" type=\"button\" id=\"wifi-interval-apply\">Apply Interval</button><span id=\"interval-save-state\" class=\"save-state\"></span></div>"
    "<div class=\"buttons\"><button class=\"button\" type=\"button\" id=\"wifi-scan-now\">Scan Now</button>"
    "<a class=\"button\" href=\"/\">Refresh Page</a></div><div id=\"wifi-scan-state\" class=\"scan-state\"></div>"
    "<div class=\"note\">Surveying runs automatically whenever the device is operating. Use Apply Interval to save a change.</div>"
    "<div id=\"wifi-auto-warning\" class=\"diagnostic-warning\"" + String(wifiAutoScanDiagnosticLabel().startsWith("WARN") ? "" : " style=\"display:none\"") + ">" + htmlEscape(wifiAutoScanDiagnosticLabel()) + "</div>"
    "<div id=\"wifi-ap-drop-warning\" class=\"diagnostic-warning\"" + String(wifiApTableFullDrops ? "" : " style=\"display:none\"") + ">" + (wifiApTableFullDrops ? String(wifiApTableFullDrops) + " Wi-Fi observation(s) were not logged because no AP table slot was available." : String("")) + "</div></div>";

  card += "<div class=\"card\"><h2>History</h2>"
    "<div class=\"row\"><span class=\"label\">Stored Observations</span><span id=\"wifi-history-count\" class=\"value\">" + String(historyCount) + " / " + String(scanHistoryCapacity) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Retained Scans</span><span id=\"wifi-retained-scans\" class=\"value\">" + String(countRetainedScanGroups()) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Oldest Observation</span><span id=\"wifi-oldest-data\" class=\"value\">" + htmlEscape(oldestLabel) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Newest Observation</span><span id=\"wifi-newest-data\" class=\"value\">" + htmlEscape(newestLabel) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Retained Time Window</span><span id=\"wifi-retained-window\" class=\"value\">" + htmlEscape(windowLabel) + "</span></div>"
    "<div class=\"buttons\"><a class=\"button\" href=\"/scanlog.csv\">Download CSV</a><a class=\"button\" href=\"/scan-clear\">Clear History</a></div>"
    "<div class=\"note\">History is retained in RAM during normal operation. Restart checkpoints preserve the current working history through controlled restarts and are consumed after successful restore.</div></div>";
  diagnosticSendContent(card);
  markWebResponsePhase("status-history");

  String plotHeading = selectedSSID.length() ? selectedSSID : String("");
  diagnosticSendContent("<div class=\"card\" id=\"rssi-plot\"><h2>RSSI History" + (plotHeading.length() ? String(" &mdash; ") + htmlEscape(plotHeading) : String("")) + "</h2>");
  if (selectedBSSID.length() > 0) {
    diagnosticSendContent("<div class=\"row developer-only\"><span class=\"label\">BSSID</span><span class=\"value\">" + htmlEscape(selectedBSSID) + "</span></div>");
    sendRssiHistoryPlot(selectedBSSID);
    diagnosticSendContent("<div class=\"note\">Click a network below to plot that access point's retained RSSI history.</div>");
  } else diagnosticSendContent("<p>No logged networks are available to plot yet.</p>");
  diagnosticSendContent("</div>");
  markWebResponsePhase("rssi");

  diagnosticSendContent("<div class=\"card\" id=\"wifi-observed-card\"><h2>Observed Networks</h2><div class=\"note\">One row per retained BSSID. Click a column header to sort; click a network to redraw the RSSI plot.</div>");
  sendNetworkSummaryTable();
  diagnosticSendContent("</div>");
  markWebResponsePhase("observed-networks");

  diagnosticSendContent("<div id=\"wifi-channel-region\">");
  sendWifiChannelAnalysis();
  diagnosticSendContent("</div>");
  markWebResponsePhase("channel-analysis");

  diagnosticSendContent("<div class=\"card\"><h2>Infrastructure Wi-Fi</h2>"
    "<div class=\"row\"><span class=\"label\">Status</span><span id=\"wifi-infra-status\" class=\"value\">" + String(connected ? "Connected" : "Not connected") + "</span></div>"
    "<div class=\"row\"><span class=\"label\">SSID</span><span id=\"wifi-infra-ssid\" class=\"value\">" + htmlEscape(connected ? connectedSSID : String("-")) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Signal</span><span id=\"wifi-infra-rssi\" class=\"value\">" + String(connected ? String(connectedRSSI) + " dBm" : String("-")) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Channel</span><span id=\"wifi-infra-channel\" class=\"value\">" + String(connected ? String(connectedChannel) : String("-")) + "</span></div>"
    "<div class=\"row advanced-only\"><span class=\"label\">BSSID</span><span id=\"wifi-infra-bssid\" class=\"value\">" + htmlEscape(connected ? connectedBSSID : String("-")) + "</span></div></div>");
  markWebResponsePhase("network-context");

  String health;
  health.reserve(2300);
  health += "<div class=\"card advanced-only\"><h2>Survey Health</h2>"
    "<div class=\"row\"><span class=\"label\">Last Scan Results</span><span id=\"wifi-last-scan-results\" class=\"value\">" + String(wifiLastScanFound) + " found; " + String(wifiLastScanLogged) + " logged; " + String(wifiLastScanDropped) + " dropped</span></div>"
    "<div class=\"row\"><span class=\"label\">APs in Last Scan</span><span id=\"wifi-last-ap-results\" class=\"value\">" + String(wifiLastScanNewAps) + " new; " + String(wifiLastScanPreviouslySeenAps) + " previously seen</span></div>"
    "<div class=\"row\"><span class=\"label\">Hidden / Reclaimed</span><span id=\"wifi-last-filter-results\" class=\"value\">" + String(wifiLastScanHiddenSkipped) + " hidden skipped; " + String(wifiLastScanReclaimedAps) + " slots reclaimed</span></div>"
    "<div class=\"row\"><span class=\"label\">Auto-Scan Diagnostic</span><span id=\"wifi-health-auto\" class=\"value\">" + htmlEscape(wifiAutoScanDiagnosticLabel()) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Automatic Scan Starts</span><span id=\"wifi-health-starts\" class=\"value\">" + String(wifiAutoScanStartCount) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Automatic Scan Completions</span><span id=\"wifi-health-completions\" class=\"value\">" + String(wifiAutoScanCompletionCount) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Automatic Start Failures</span><span id=\"wifi-health-start-failures\" class=\"value\">" + String(wifiAutoScanStartFailureCount) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Automatic Completion Failures</span><span id=\"wifi-health-completion-failures\" class=\"value\">" + String(wifiAutoScanCompletionFailureCount) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Last Automatic Start</span><span id=\"wifi-health-last-start\" class=\"value\">" + htmlEscape(wifiAutoScanLastStartLabel()) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Last Automatic Completion</span><span id=\"wifi-health-last-completion\" class=\"value\">" + htmlEscape(wifiAutoScanLastCompletionLabel()) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Wi-Fi Scan Duration</span><span id=\"wifi-health-duration\" class=\"value\">" + htmlEscape(wifiScanDurationSummaryLabel()) + "</span></div></div>";
  diagnosticSendContent(health);

  String dev;
  dev.reserve(2600);
  dev += "<div class=\"card developer-only\"><h2>Survey Scheduler Diagnostics</h2>"
    "<div class=\"row\"><span class=\"label\">Automatic Retry Backoff</span><span id=\"wifi-retry-state\" class=\"value\">" + String(WIFI_AUTOSCAN_RETRY_BACKOFF_MS / 1000.0f, 1) + " s; " + String(wifiAutoScanRetryPending ? "retry pending" : "idle") + "</span></div>"
    "<div class=\"row\"><span class=\"label\">User Interaction Defer</span><span class=\"value\">" + String(USER_INTERACTION_DEFER_MS / 1000.0f, 1) + " s after explicit web requests</span></div></div>";
  dev += "<div class=\"card developer-only\"><h2>CSV Diagnostics</h2>"
    "<div class=\"row\"><span class=\"label\">CSV Exports Served</span><span id=\"wifi-csv-count\" class=\"value\">" + String(wifiCsvExportCount) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Last CSV Export</span><span id=\"wifi-csv-last\" class=\"value\">" + (wifiCsvExportCount ? htmlEscape(csvExportSummaryLabel(wifiCsvLastRows, wifiCsvLastBytes, wifiCsvLastDurationMs)) : String("Never")) + "</span></div></div>";
  dev += "<div class=\"card developer-only\"><h2>Wi-Fi Memory Diagnostics</h2>"
    "<div class=\"row\"><span class=\"label\">History RAM</span><span class=\"value\">" + String(wifiHistoryAllocatedBytes()/1024.0,1) + " KB total</span></div>"
    "<div class=\"row\"><span class=\"label\">Observation Storage</span><span class=\"value\">" + String((scanHistoryCapacity*sizeof(WifiObservation))/1024.0,1) + " KB; " + String(sizeof(WifiObservation)) + " bytes/observation</span></div>"
    "<div class=\"row\"><span class=\"label\">AP Table</span><span id=\"wifi-ap-table\" class=\"value\">" + String(wifiApCount) + " / " + String(wifiApTableCapacity) + "; " + String((wifiApTableCapacity*sizeof(WifiApEntry))/1024.0,1) + " KB</span></div>"
    "<div class=\"row\"><span class=\"label\">Scan Metadata</span><span class=\"value\">" + String(wifiScanMetadataCapacity) + " slots; " + String((wifiScanMetadataCapacity*sizeof(WifiScanMetadata))/1024.0,1) + " KB</span></div>"
    "<div class=\"row\"><span class=\"label\">History Integrity</span><span id=\"wifi-history-integrity\" class=\"value\">" + String(wifiHistoryIntegrityAnomalies() == 0 ? "PASS" : String("WARN - ") + String(wifiHistoryIntegrityAnomalies()) + " anomaly(s)") + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Free Heap</span><span id=\"wifi-free-heap\" class=\"value\">" + String(ESP.getFreeHeap()/1024.0,1) + " KB</span></div>"
    "<div class=\"row\"><span class=\"label\">Largest Free Block</span><span id=\"wifi-largest-block\" class=\"value\">" + String(diagnosticLargestFreeBlock()/1024.0,1) + " KB</span></div></div>";
  diagnosticSendContent(dev);
  markWebResponsePhase("health-diagnostics");

  diagnosticSendContent("<div class=\"footer\">ESP32 Web Interface</div>");
  sendSortableTableScript();
  sendThemeScript();
  {
    String refreshScript =
      "<script>(function(){"
      "let scan=" + String(scanCounter) + ";"
      "const toggle=document.getElementById('live-updates-toggle');"
      "const plotBssid='" + jsEscape(selectedBSSID) + "';"
      "let updating=false;"
      "const scanButton=document.getElementById('wifi-scan-now');"
      "const scanState=document.getElementById('wifi-scan-state');"
      "const intervalInput=document.getElementById('interval');"
      "const intervalApply=document.getElementById('wifi-interval-apply');"
      "const intervalState=document.getElementById('interval-save-state');"
      "function text(id,v){const e=document.getElementById(id);if(e)e.textContent=v;}"
      "function show(id,on,msg){const e=document.getElementById(id);if(!e)return;e.style.display=on?'':'none';e.textContent=on?msg:'';}"
      "function showScanState(active,msg){if(scanState){scanState.textContent=msg||'';scanState.classList.toggle('active',!!active);}if(scanButton)scanButton.disabled=!!active;}"
      "function applyStatus(s){text('wifi-scans-session',s.scan);text('wifi-last-scan',s.lastScan);text('wifi-history-count',s.records+' / '+s.capacity);text('wifi-retained-scans',s.retainedScans);text('wifi-oldest-data',s.oldestData);text('wifi-retained-window',s.retainedWindow);if(intervalInput&&document.activeElement!==intervalInput)intervalInput.value=s.interval;text('wifi-health-auto',s.autoDiagnostic);text('wifi-health-starts',s.autoStarts);text('wifi-health-completions',s.autoCompletions);text('wifi-health-start-failures',s.autoStartFailures);text('wifi-health-completion-failures',s.autoCompletionFailures);text('wifi-health-last-start',s.lastAutoStart);text('wifi-health-last-completion',s.lastAutoCompletion);text('wifi-health-duration',s.scanDuration);text('wifi-retry-state'," + String(WIFI_AUTOSCAN_RETRY_BACKOFF_MS / 1000.0f, 1) + "+' s; '+(s.autoRetryPending?'retry pending':'idle'));text('wifi-csv-count',s.csvExports);text('wifi-csv-last',s.lastCsv);text('wifi-ap-table',s.apCount+' / '+s.apCapacity+'; " + String((wifiApTableCapacity*sizeof(WifiApEntry))/1024.0,1) + " KB');text('wifi-history-integrity',s.historyIntegrityAnomalies===0?'PASS':'WARN - '+s.historyIntegrityAnomalies+' anomaly(s)');text('wifi-free-heap',(s.freeHeap/1024).toFixed(1)+' KB');text('wifi-largest-block',(s.largestBlock/1024).toFixed(1)+' KB');text('wifi-infra-status',s.connected?'Connected':'Not connected');text('wifi-infra-ssid',s.connected?s.stationSSID:'-');text('wifi-infra-rssi',s.connected?s.stationRssi+' dBm':'-');text('wifi-infra-channel',s.connected?s.stationChannel:'-');text('wifi-infra-bssid',s.connected?s.stationBSSID:'-');show('wifi-auto-warning',String(s.autoDiagnostic).startsWith('WARN'),s.autoDiagnostic);show('wifi-ap-drop-warning',s.apDrops>0,s.apDrops+' Wi-Fi observation(s) were not logged because no AP table slot was available.');showScanState(!!s.scanning,s.scanning?'Scanning…':'');}"
      "function saveInterval(){if(!intervalInput)return;let v=parseInt(intervalInput.value,10);if(!Number.isFinite(v))return;v=Math.max(5,Math.min(3600,v));intervalInput.value=v;if(intervalState)intervalState.textContent='Saving…';fetch('/api/wifi/interval?interval='+encodeURIComponent(v),{method:'POST',cache:'no-store'}).then(r=>{if(!r.ok)throw new Error();return r.json();}).then(s=>{intervalInput.value=s.interval;if(intervalState){intervalState.textContent='Saved';setTimeout(()=>{intervalState.textContent='';},1400);}}).catch(()=>{if(intervalState)intervalState.textContent='Save failed';});}"
      "if(intervalApply)intervalApply.addEventListener('click',saveInterval);if(intervalInput){intervalInput.addEventListener('keydown',function(e){if(e.key==='Enter'){e.preventDefault();saveInterval();intervalInput.blur();}});}"
      "if(scanButton){scanButton.addEventListener('click',function(){showScanState(true,'Scanning…');fetch('/scan-now',{cache:'no-store'}).then(function(r){if(!r.ok&&r.status!==202)throw new Error();return r.json();}).then(function(s){showScanState(!!s.scanning,s.scanning?'Scanning…':(s.message||''));}).catch(function(){showScanState(false,'Unable to start scan');});});}"
      "async function repaint(){if(updating)return;updating=true;try{const jobs=[fetch('/api/wifi/observed',{cache:'no-store'}).then(r=>r.text()).then(h=>{const e=document.getElementById('wifi-observed-card');if(e)e.innerHTML=h;}),fetch('/api/wifi/channel',{cache:'no-store'}).then(r=>r.text()).then(h=>{const e=document.getElementById('wifi-channel-region');if(e)e.innerHTML=h;})];if(plotBssid){jobs.push(fetch('/api/wifi/plot?bssid='+encodeURIComponent(plotBssid),{cache:'no-store'}).then(r=>r.text()).then(h=>{const e=document.getElementById('rssi-plot');if(e)e.innerHTML=h;}));}await Promise.all(jobs);}catch(e){}finally{updating=false;}}"
      "setInterval(function(){fetch('/api/wifi/status',{cache:'no-store'}).then(r=>r.json()).then(function(s){if(toggle&&toggle.checked)applyStatus(s);else showScanState(!!s.scanning,s.scanning?'Scanning…':'');if(s.scan!==scan){scan=s.scan;if(toggle&&toggle.checked)repaint();}}).catch(function(){});},2000);"
      "})();</script>";
    diagnosticSendContent(refreshScript);
  }
  diagnosticSendContent("</div></body></html>");
  diagnosticSendContent("");
  markWebResponsePhase("footer-scripts");
  endWebResponseProfile();
}


// Purpose: Returns only the Observed Networks card content for live-update repainting.
void handleWifiObservedFragment() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", "");
  diagnosticSendContent(
    "<h2>Observed Networks</h2><div class=\"note\">"
    "One row is shown for each BSSID observed during this session. "
    "Click any column header to sort the table. Click an SSID or BSSID "
    "to redraw the RSSI plot for that access point.</div>"
  );
  sendNetworkSummaryTable();
  diagnosticSendContent("");
}

// Purpose: Returns only the selected Wi-Fi RSSI plot fragment for live-update repainting.
void handleWifiPlotFragment() {
  String selectedBSSID = server.hasArg("bssid") ? server.arg("bssid") : "";
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", "");

  if (selectedBSSID.length() > 0) {
    NetworkSummary summary = {};
    if (buildNetworkSummary(selectedBSSID, summary)) {
      String displaySSID = summary.hidden ? "(hidden)" : String(summary.ssid);
      diagnosticSendContent("<h2>RSSI History &mdash; " + htmlEscape(displaySSID) + "</h2>");
      diagnosticSendContent("<div class=\"row developer-only\"><span class=\"label\">BSSID</span><span class=\"value\">" + htmlEscape(selectedBSSID) + "</span></div>");
      sendRssiHistoryPlot(selectedBSSID);
    } else {
      diagnosticSendContent("<h2>RSSI History</h2><p>The selected network is no longer retained.</p>");
    }
  } else {
    diagnosticSendContent("<h2>RSSI History</h2><p>Select a network below to display RSSI history.</p>");
  }
  diagnosticSendContent("");
}


// Purpose: Returns the current Wi-Fi channel-analysis card for live-update repainting.
void handleWifiChannelFragment() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", "");
  sendWifiChannelAnalysis();
  diagnosticSendContent("");
}

// ============================================================
// BLE web survey
// ============================================================

// Purpose: Sends an HTTP redirect back to the Bluetooth Survey page.
void redirectToBLEPage() {
  server.sendHeader("Location", "/ble");
  server.send(303, "text/plain", "");
}

// Purpose: Applies Bluetooth scan interval settings and keeps automatic surveying active whenever Bluetooth Survey is enabled.
bool applyBleScanIntervalFromRequest() {
  if (!bleSurveyEnabled || !server.hasArg("interval")) return false;
  long requested = server.arg("interval").toInt();
  if (requested < (long)MIN_SCAN_INTERVAL_SECONDS) requested = MIN_SCAN_INTERVAL_SECONDS;
  if (requested > (long)MAX_SCAN_INTERVAL_SECONDS) requested = MAX_SCAN_INTERVAL_SECONDS;
  bleScanIntervalSeconds = (unsigned long)requested;
  autoBleScanEnabled = true;
  lastAutoBleScanMs = millis();
  preferences.begin("survey", false);
  preferences.putULong("bleInterval", bleScanIntervalSeconds);
  preferences.end();
  return true;
}

// Purpose: Legacy Bluetooth settings endpoint that applies the interval and redirects to the survey page.
void handleBLESettings() {
  markExplicitUserInteraction();
  if (!bleSurveyEnabled) {
    bleStatusMessage = "BLE settings are unavailable because BLE is disabled at boot.";
    redirectToBLEPage();
    return;
  }
  applyBleScanIntervalFromRequest();
  autoBleScanEnabled = true;
  redirectToBLEPage();
}

// Purpose: JSON endpoint used by the Bluetooth page to save scan interval without reloading the page.
void handleBleIntervalSetting() {
  markExplicitUserInteraction();
  bool accepted = applyBleScanIntervalFromRequest();
  server.sendHeader("Cache-Control", "no-store");
  if (!accepted) {
    server.send(400, "application/json", "{\"saved\":false,\"message\":\"Bluetooth Survey disabled or interval missing\"}");
    return;
  }
  server.send(200, "application/json", String("{\"saved\":true,\"interval\":") + String(bleScanIntervalSeconds) + "}");
}

// Purpose: Clears BLE history and redirects back to the Bluetooth Survey page.
void handleClearBLEHistory() {
  markExplicitUserInteraction();
  clearBleHistory();
  redirectToBLEPage();
}

// Purpose: Streams retained BLE observations as CSV and records export-performance diagnostics.
void handleBLEScanCsv() {
  markExplicitUserInteraction();
  csvExportInProgress = true;

  const size_t exportCount = bleHistoryCount;
  const uint32_t exportStartMs = millis();
  size_t bytesSent = 0;

  server.sendHeader("Content-Disposition", "attachment; filename=\"ble_scan_log.csv\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");

  String buffer;
  buffer.reserve(CSV_STREAM_BUFFER_BYTES + 256);
  appendCsvBuffered(
    buffer,
    "scan,uptime_ms,uptime,name,address,address_type,rssi_dbm\r\n",
    bytesSent
  );

  for (size_t i = 0; i < exportCount; i++) {
    const BleScanRecord& record = bleHistoryRecord(i);

    String line;
    line.reserve(180);
    line += String(record.scanNumber) + ",";
    line += String(record.uptimeMs) + ",";
    line += csvEscape(formatUptime(record.uptimeMs)) + ",";
    line += csvEscape(record.named ? String(record.name) : String("")) + ",";
    line += csvEscape(String(record.address)) + ",";
    line += csvEscape(bleAddressTypeLabel(record.addressType)) + ",";
    line += String(record.rssi) + "\r\n";
    appendCsvBuffered(buffer, line, bytesSent);
  }

  flushCsvBuffer(buffer, bytesSent);
  diagnosticSendContent("");

  bleCsvExportCount++;
  bleCsvLastRows = exportCount;
  bleCsvLastBytes = bytesSent;
  bleCsvLastDurationMs = millis() - exportStartMs;
  csvExportInProgress = false;
}

// Purpose: Generates and streams the SVG RSSI history plot for one selected BLE address.
void sendBleRssiHistoryPlot(const String& selectedAddress) {
  const int SVG_WIDTH = 720, SVG_HEIGHT = 280;
  const int LEFT = 58, RIGHT = 20, TOP = 20, BOTTOM = 45;
  const int plotWidth = SVG_WIDTH - LEFT - RIGHT;
  const int plotHeight = SVG_HEIGHT - TOP - BOTTOM;
  const int RSSI_TOP = -30, RSSI_BOTTOM = -100;

  uint32_t firstMs = 0, lastMs = 0;
  size_t pointCount = 0;

  for (size_t i = 0; i < bleHistoryCount; i++) {
    const BleScanRecord& record = bleHistoryRecord(i);
    if (!String(record.address).equalsIgnoreCase(selectedAddress)) continue;
    if (pointCount == 0) firstMs = record.uptimeMs;
    lastMs = record.uptimeMs;
    pointCount++;
  }

  if (pointCount == 0) {
    diagnosticSendContent("<p>No retained samples for this BLE device.</p>");
    return;
  }

  if (lastMs <= firstMs) lastMs = firstMs + 1;

  diagnosticSendContent("<div class=\"plot-wrap\"><svg viewBox=\"0 0 720 280\" role=\"img\">"
    "<rect class=\"plot-bg\" x=\"58\" y=\"20\" width=\"642\" height=\"215\"/>");

  for (int rssi = -100; rssi <= -30; rssi += 10) {
    int y = TOP + ((RSSI_TOP - rssi) * plotHeight) / (RSSI_TOP - RSSI_BOTTOM);
    String grid = "<line x1=\"" + String(LEFT) + "\" y1=\"" + String(y) +
      "\" x2=\"" + String(SVG_WIDTH - RIGHT) + "\" y2=\"" + String(y) +
      "\" class=\"plot-grid\"/><text class=\"plot-text\" x=\"" + String(LEFT - 8) + "\" y=\"" +
      String(y + 4) + "\" class=\"plot-text\" text-anchor=\"end\" font-size=\"11\">" +
      String(rssi) + "</text>";
    diagnosticSendContent(grid);
  }

  String points;
  points.reserve(pointCount * 14);

  for (size_t i = 0; i < bleHistoryCount; i++) {
    const BleScanRecord& record = bleHistoryRecord(i);
    if (!String(record.address).equalsIgnoreCase(selectedAddress)) continue;

    int x = LEFT + (uint64_t)(record.uptimeMs - firstMs) * plotWidth / (lastMs - firstMs);
    int clipped = record.rssi;
    if (clipped > RSSI_TOP) clipped = RSSI_TOP;
    if (clipped < RSSI_BOTTOM) clipped = RSSI_BOTTOM;
    int y = TOP + ((RSSI_TOP - clipped) * plotHeight) / (RSSI_TOP - RSSI_BOTTOM);

    if (points.length()) points += " ";
    points += String(x) + "," + String(y);
  }

  diagnosticSendContent("<polyline class=\"plot-line\" points=\"" + points + "\"/>");

  for (size_t i = 0; i < bleHistoryCount; i++) {
    const BleScanRecord& record = bleHistoryRecord(i);
    if (!String(record.address).equalsIgnoreCase(selectedAddress)) continue;

    int x = LEFT + (uint64_t)(record.uptimeMs - firstMs) * plotWidth / (lastMs - firstMs);
    int clipped = record.rssi;
    if (clipped > RSSI_TOP) clipped = RSSI_TOP;
    if (clipped < RSSI_BOTTOM) clipped = RSSI_BOTTOM;
    int y = TOP + ((RSSI_TOP - clipped) * plotHeight) / (RSSI_TOP - RSSI_BOTTOM);

    String dot = "<circle cx=\"" + String(x) + "\" cy=\"" + String(y) +
      "\" r=\"4\" class=\"plot-point\"><title>Scan #" + String(record.scanNumber) +
      " | " + htmlEscape(formatUptime(record.uptimeMs)) + " | " +
      String(record.rssi) + " dBm</title></circle>";
    diagnosticSendContent(dot);
  }

  diagnosticSendContent("</svg></div>");
}

// Purpose: Streams the sortable Observed BLE Devices table with columns progressively exposed by view depth.
void sendBleSummaryTable() {
  if (bleHistoryCount == 0) {
    diagnosticSendContent("<p>No BLE devices have been observed yet.</p>");
    return;
  }

  diagnosticSendContent("<div class=\"table-scroll\"><table id=\"ble-summary\"><thead><tr>"
    "<th class=\"sortable\" onclick=\"sortTable('ble-summary',0,'text')\">Name</th>"
    "<th class=\"sortable\" onclick=\"sortTable('ble-summary',1,'text')\">Address</th>"
    "<th class=\"sortable signal\" onclick=\"sortTable('ble-summary',2,'number')\">Last</th>"
    "<th class=\"sortable signal\" onclick=\"sortTable('ble-summary',3,'number')\">Avg</th>"
    "<th class=\"sortable signal\" onclick=\"sortTable('ble-summary',4,'number')\">Count</th>"
    "<th class=\"sortable\" onclick=\"sortTable('ble-summary',5,'number')\">Last Seen</th>"
    "<th class=\"sortable signal advanced-only\" onclick=\"sortTable('ble-summary',6,'number')\">Min</th>"
    "<th class=\"sortable signal advanced-only\" onclick=\"sortTable('ble-summary',7,'number')\">Max</th>"
    "<th class=\"sortable developer-only\" onclick=\"sortTable('ble-summary',8,'text')\">Address Type</th>"
    "<th class=\"sortable developer-only\" onclick=\"sortTable('ble-summary',9,'number')\">First Seen</th>"
    "</tr></thead><tbody>");

  for (size_t offset = 0; offset < bleHistoryCount; offset++) {
    size_t logicalIndex = bleHistoryCount - 1 - offset;
    const BleScanRecord& newest = bleHistoryRecord(logicalIndex);
    String address = String(newest.address);
    if (hasNewerBleObservationForAddress(logicalIndex, address)) continue;

    BLEDeviceSummary summary = {};
    if (!buildBleDeviceSummary(address, summary)) continue;

    String plotUrl = "/ble?plot=" + urlEncode(address) + "#rssi-plot";
    String displayName = summary.named ? String(summary.name) : "(unnamed)";
    float avgRssi = averageSignal(summary.signal);
    uint32_t nowMs = surveySessionUptimeMs();
    uint32_t firstAgeMs = nowMs >= summary.signal.firstSeenMs ? nowMs - summary.signal.firstSeenMs : 0;
    uint32_t lastAgeMs = nowMs >= summary.signal.lastSeenMs ? nowMs - summary.signal.lastSeenMs : 0;

    String row;
    row.reserve(900);
    row += "<tr>";
    row += "<td><a href=\"" + plotUrl + "\">" + htmlEscape(displayName) + "</a></td>";
    row += "<td class=\"address\"><a href=\"" + plotUrl + "\">" + htmlEscape(address) + "</a></td>";
    row += "<td class=\"signal\" data-sort=\"" + String(summary.signal.latestRssi) + "\">" + String(summary.signal.latestRssi) + " dBm</td>";
    row += "<td class=\"signal\" data-sort=\"" + String(avgRssi, 1) + "\">" + String(avgRssi, 1) + " dBm</td>";
    row += "<td class=\"signal\" data-sort=\"" + String(summary.signal.samples) + "\">" + String(summary.signal.samples) + "</td>";
    row += "<td data-sort=\"" + String(lastAgeMs) + "\">" + htmlEscape(observationAgeLabel(summary.signal.lastSeenMs)) + "</td>";
    row += "<td class=\"signal advanced-only\" data-sort=\"" + String(summary.signal.minRssi) + "\">" + String(summary.signal.minRssi) + " dBm</td>";
    row += "<td class=\"signal advanced-only\" data-sort=\"" + String(summary.signal.maxRssi) + "\">" + String(summary.signal.maxRssi) + " dBm</td>";
    row += "<td class=\"developer-only\">" + htmlEscape(bleAddressTypeLabel(summary.addressType)) + "</td>";
    row += "<td class=\"developer-only\" data-sort=\"" + String(firstAgeMs) + "\">" + htmlEscape(observationAgeLabel(summary.signal.firstSeenMs)) + "</td>";
    row += "</tr>";
    diagnosticSendContent(row);
  }

  diagnosticSendContent("</tbody></table></div>");
}

// Purpose: Returns current Bluetooth survey, history, network, and diagnostic values for lightweight browser live updates.
void handleBleScanStatus() {
  bool connected = WiFi.status() == WL_CONNECTED;
  String json = "{";
  json += "\"scan\":" + String(bleScanCounter);
  json += ",\"records\":" + String(bleHistoryCount);
  json += ",\"capacity\":" + String(bleHistoryRetentionLimit);
  json += ",\"retainedScans\":" + String(countRetainedBleScanGroups());
  json += ",\"lastScan\":" + jsonQuoted(bleScanCounter ? observationAgeLabel(lastBleScanUptimeMs) : String("Never"));
  json += ",\"interval\":" + String(bleScanIntervalSeconds);
  json += ",\"scanning\":" + String(bleDiagnosticScanActive ? "true" : "false");
  json += ",\"scanStatus\":" + jsonQuoted(bleStatusMessage);
  json += ",\"addressDrops\":" + String(bleAddressTableFullDrops);
  json += ",\"addressReferenced\":" + String(countReferencedBleAddresses());
  json += ",\"addressCapacity\":" + String(bleAddressTableCapacity);
  json += ",\"metadataReferenced\":" + String(countReferencedBleScanSlots());
  json += ",\"metadataCapacity\":" + String(bleScanMetadataCapacity);
  json += ",\"csvExports\":" + String(bleCsvExportCount);
  json += ",\"lastCsv\":" + jsonQuoted(bleCsvExportCount ? csvExportSummaryLabel(bleCsvLastRows, bleCsvLastBytes, bleCsvLastDurationMs) : String("Never"));
  json += ",\"freeHeap\":" + String(ESP.getFreeHeap());
  json += ",\"largestBlock\":" + String(diagnosticLargestFreeBlock());
  json += ",\"connected\":" + String(connected ? "true" : "false");
  json += ",\"stationSSID\":" + jsonQuoted(connected ? WiFi.SSID() : String(""));
  json += ",\"stationBSSID\":" + jsonQuoted(connected ? WiFi.BSSIDstr() : String(""));
  json += ",\"stationRssi\":" + String(connected ? WiFi.RSSI() : 0);
  json += ",\"stationChannel\":" + String(connected ? WiFi.channel() : 0);
  json += "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

// Purpose: Builds the Bluetooth Survey page with automatic surveying, immediate interval control, and consistent page ordering.
void handleBLESurvey() {
  beginWebResponseProfile("/ble");
  markExplicitUserInteraction();
  String selectedAddress = "";
  if (server.hasArg("plot")) {
    String requested = server.arg("plot");
    requested.trim();
    if (bleHistoryContainsAddress(requested)) selectedAddress = requested;
  }
  if (selectedAddress.length() == 0 && bleHistoryCount > 0) selectedAddress = String(bleHistoryRecord(bleHistoryCount - 1).address);

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  diagnosticSendContent("<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>ESP32 Bluetooth Survey</title>");
  sendThemeBootstrapScript();
  diagnosticSendContent(pageStyles());
  diagnosticSendContent("</head><body><div class=\"container\">");
  sendSiteNavigation("ble");
  diagnosticSendContent("<h1>Bluetooth Survey</h1>");
  markWebResponsePhase("header");

  if (!bleSurveyEnabled) {
    diagnosticSendContent("<div class=\"card\"><h2>Surveying: Disabled</h2>"
      "<div class=\"note\"><strong>Bluetooth Survey is disabled.</strong> Enabling Bluetooth allows Bluetooth surveying, but significantly reduces Wi-Fi history capacity because both surveys share the ESP32's available memory. Enabling Bluetooth requires a restart.</div>"
      "<div class=\"row advanced-only\"><span class=\"label\">BLE Stack</span><span class=\"value\">Not initialized</span></div>"
      "<div class=\"row developer-only\"><span class=\"label\">BLE History RAM</span><span class=\"value\">0 KB</span></div>"
      "<form class=\"controls\" action=\"/ble-mode\" method=\"post\"><input type=\"hidden\" name=\"enabled\" value=\"1\"><button type=\"submit\">Enable Bluetooth Survey</button></form></div>"
      "<div class=\"footer\">ESP32 Web Interface</div>");
    sendThemeScript();
    diagnosticSendContent("</div></body></html>");
    diagnosticSendContent("");
    markWebResponsePhase("disabled-content");
    endWebResponseProfile();
    return;
  }

  autoBleScanEnabled = true;
  String status;
  status.reserve(3000);
  status += "<div class=\"card\"><h2>Survey Status &amp; Controls</h2>"
    "<div class=\"row\"><span class=\"label\">Surveying</span><span class=\"value\">Automatic while enabled</span></div>"
    "<div class=\"row\"><span class=\"label\">Scans This Session</span><span id=\"ble-scans-session\" class=\"value\">" + String(bleScanCounter) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Last Scan</span><span id=\"ble-last-scan\" class=\"value\">" + (bleScanCounter ? htmlEscape(observationAgeLabel(lastBleScanUptimeMs)) : String("Never")) + "</span></div>"
    "<div class=\"survey-control-row\"><div class=\"control\"><label for=\"ble-interval\">Scan Interval (seconds)</label>"
    "<input id=\"ble-interval\" type=\"number\" min=\"5\" max=\"3600\" value=\"" + String(bleScanIntervalSeconds) + "\"></div>"
    "<span id=\"ble-interval-save-state\" class=\"save-state\"></span></div>"
    "<div class=\"buttons\"><a class=\"button\" href=\"/ble-scan\">Scan Now</a><a class=\"button\" href=\"/ble\">Refresh Page</a></div>"
    "<form class=\"controls\" action=\"/ble-mode\" method=\"post\"><input type=\"hidden\" name=\"enabled\" value=\"0\"><button type=\"submit\">Disable Bluetooth Survey</button></form>"
    "<div id=\"ble-scan-state\" class=\"scan-state\"></div>"
    "<div class=\"note\">Bluetooth surveying runs automatically whenever Bluetooth Survey is enabled. Changing the interval saves immediately. Bluetooth surveying significantly reduces available Wi-Fi history capacity.</div>"
    "<div id=\"ble-status-note\" class=\"note\">" + htmlEscape(bleStatusMessage) + "</div></div>";

  status += "<div class=\"card\"><h2>History</h2>"
    "<div class=\"row\"><span class=\"label\">Stored Observations</span><span id=\"ble-history-count\" class=\"value\">" + String(bleHistoryCount) + " / " + String(bleHistoryRetentionLimit) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Retained Scans</span><span id=\"ble-retained-scans\" class=\"value\">" + String(countRetainedBleScanGroups()) + "</span></div>"
    "<div class=\"buttons\"><a class=\"button\" href=\"/blelog.csv\">Download CSV</a><a class=\"button\" href=\"/ble-clear\">Clear History</a></div></div>";
  if (bleHistoryResizeMessage.length()) status += "<div class=\"note\"><strong>" + htmlEscape(bleHistoryResizeMessage) + "</strong></div>";
  diagnosticSendContent(status);
  markWebResponsePhase("status-history");

  String identity;
  if (selectedAddress.length()) {
    String selectedName = latestBleNameForAddress(selectedAddress);
    identity = (selectedName.length() && selectedName != "(unnamed)") ? selectedName : selectedAddress;
  }
  diagnosticSendContent("<div class=\"card\" id=\"rssi-plot\"><h2>RSSI History" + (identity.length() ? String(" &mdash; ") + htmlEscape(identity) : String("")) + "</h2>");
  if (selectedAddress.length()) {
    diagnosticSendContent("<div class=\"row developer-only\"><span class=\"label\">BLE Address</span><span class=\"value\">" + htmlEscape(selectedAddress) + "</span></div>");
    sendBleRssiHistoryPlot(selectedAddress);
    diagnosticSendContent("<div class=\"note\">Click a device below to redraw this plot.</div>");
  } else diagnosticSendContent("<p>No logged BLE addresses are available to plot yet.</p>");
  diagnosticSendContent("</div>");
  markWebResponsePhase("rssi");

  diagnosticSendContent("<div class=\"card\" id=\"ble-observed-card\"><h2>Observed BLE Devices</h2><div class=\"note\">One row per retained BLE address. Click a column header to sort.</div>");
  sendBleSummaryTable();
  diagnosticSendContent("</div>");
  markWebResponsePhase("observed-devices");

  bool connected = WiFi.status() == WL_CONNECTED;
  diagnosticSendContent("<div class=\"card\"><h2>Infrastructure Wi-Fi</h2>"
    "<div class=\"row\"><span class=\"label\">Status</span><span id=\"ble-infra-status\" class=\"value\">" + String(connected ? "Connected" : "Not connected") + "</span></div>"
    "<div class=\"row\"><span class=\"label\">SSID</span><span id=\"ble-infra-ssid\" class=\"value\">" + htmlEscape(connected ? WiFi.SSID() : String("-")) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Signal</span><span id=\"ble-infra-rssi\" class=\"value\">" + String(connected ? String(WiFi.RSSI()) + " dBm" : String("-")) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Channel</span><span id=\"ble-infra-channel\" class=\"value\">" + String(connected ? String(WiFi.channel()) : String("-")) + "</span></div>"
    "<div class=\"row advanced-only\"><span class=\"label\">BSSID</span><span id=\"ble-infra-bssid\" class=\"value\">" + htmlEscape(connected ? WiFi.BSSIDstr() : String("-")) + "</span></div></div>");
  markWebResponsePhase("network-context");

  diagnosticSendContent("<div class=\"card advanced-only\"><h2>Survey Health</h2>"
    "<div class=\"row\"><span class=\"label\">Dropped BLE Observations</span><span id=\"ble-dropped-observations\" class=\"value\">" + String(bleAddressTableFullDrops) + "</span></div></div>");

  String dev;
  dev.reserve(1900);
  dev += "<div class=\"card developer-only\"><h2>BLE Implementation Diagnostics</h2>"
    "<div class=\"row\"><span class=\"label\">History RAM</span><span class=\"value\">" + String(bleHistoryAllocatedBytes()/1024.0,1) + " KB total</span></div>"
    "<div class=\"row\"><span class=\"label\">BLE Observation Size</span><span class=\"value\">" + String(sizeof(BleObservation)) + " bytes</span></div>"
    "<div class=\"row\"><span class=\"label\">BLE Address Table</span><span id=\"ble-address-table\" class=\"value\">" + String(countReferencedBleAddresses()) + " / " + String(bleAddressTableCapacity) + " referenced; peak " + String(bleAddressPeakReferenced) + "; " + String(bleAddressTableCapacity*sizeof(BleAddressEntry)/1024.0,1) + " KB</span></div>"
    "<div class=\"row\"><span class=\"label\">BLE Scan Metadata</span><span id=\"ble-metadata-table\" class=\"value\">" + String(countReferencedBleScanSlots()) + " / " + String(bleScanMetadataCapacity) + " referenced; peak " + String(bleScanMetadataPeakUsed) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">CSV Exports Served</span><span id=\"ble-csv-count\" class=\"value\">" + String(bleCsvExportCount) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Last CSV Export</span><span id=\"ble-csv-last\" class=\"value\">" + (bleCsvExportCount ? htmlEscape(csvExportSummaryLabel(bleCsvLastRows,bleCsvLastBytes,bleCsvLastDurationMs)) : String("Never")) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Free Heap</span><span id=\"ble-free-heap\" class=\"value\">" + String(ESP.getFreeHeap()/1024.0,1) + " KB</span></div>"
    "<div class=\"row\"><span class=\"label\">Largest Free Block</span><span id=\"ble-largest-block\" class=\"value\">" + String(diagnosticLargestFreeBlock()/1024.0,1) + " KB</span></div>"
    "<div class=\"note\">BLE scanning uses NimBLE callbacks and a bounded firmware capture buffer so scan acquisition does not block normal web servicing.</div></div>";
  diagnosticSendContent(dev);
  markWebResponsePhase("health-diagnostics");

  diagnosticSendContent("<div class=\"footer\">ESP32 Web Interface</div>");
  sendSortableTableScript();
  sendThemeScript();
  {
    String refreshScript =
      "<script>(function(){let scan=" + String(bleScanCounter) + ";const toggle=document.getElementById('live-updates-toggle');const address='" + jsEscape(selectedAddress) + "';let updating=false;const intervalInput=document.getElementById('ble-interval');const intervalState=document.getElementById('ble-interval-save-state');"
      "function text(id,v){const e=document.getElementById(id);if(e)e.textContent=v;}"
      "function applyStatus(s){text('ble-scans-session',s.scan);text('ble-last-scan',s.lastScan);text('ble-history-count',s.records+' / '+s.capacity);text('ble-retained-scans',s.retainedScans);if(intervalInput&&document.activeElement!==intervalInput)intervalInput.value=s.interval;text('ble-scan-state',s.scanning?'Scanning…':'');text('ble-status-note',s.scanStatus||'');text('ble-dropped-observations',s.addressDrops);text('ble-address-table',s.addressReferenced+' / '+s.addressCapacity+' referenced; peak " + String(bleAddressPeakReferenced) + "; " + String(bleAddressTableCapacity*sizeof(BleAddressEntry)/1024.0,1) + " KB');text('ble-metadata-table',s.metadataReferenced+' / '+s.metadataCapacity+' referenced; peak " + String(bleScanMetadataPeakUsed) + "');text('ble-csv-count',s.csvExports);text('ble-csv-last',s.lastCsv);text('ble-free-heap',(s.freeHeap/1024).toFixed(1)+' KB');text('ble-largest-block',(s.largestBlock/1024).toFixed(1)+' KB');text('ble-infra-status',s.connected?'Connected':'Not connected');text('ble-infra-ssid',s.connected?s.stationSSID:'-');text('ble-infra-rssi',s.connected?s.stationRssi+' dBm':'-');text('ble-infra-channel',s.connected?s.stationChannel:'-');text('ble-infra-bssid',s.connected?s.stationBSSID:'-');}"
      "function saveInterval(){if(!intervalInput)return;let v=parseInt(intervalInput.value,10);if(!Number.isFinite(v))return;v=Math.max(5,Math.min(3600,v));intervalInput.value=v;if(intervalState)intervalState.textContent='Saving…';fetch('/api/ble/interval?interval='+encodeURIComponent(v),{method:'POST',cache:'no-store'}).then(r=>{if(!r.ok)throw new Error();return r.json();}).then(s=>{intervalInput.value=s.interval;if(intervalState){intervalState.textContent='Saved';setTimeout(()=>{intervalState.textContent='';},1400);}}).catch(()=>{if(intervalState)intervalState.textContent='Save failed';});}"
      "if(intervalInput){intervalInput.addEventListener('change',saveInterval);intervalInput.addEventListener('keydown',function(e){if(e.key==='Enter'){e.preventDefault();saveInterval();intervalInput.blur();}});}"
      "async function repaint(){if(updating)return;updating=true;try{const jobs=[fetch('/api/ble/observed',{cache:'no-store'}).then(r=>r.text()).then(h=>{const e=document.getElementById('ble-observed-card');if(e)e.innerHTML=h;})];if(address){jobs.push(fetch('/api/ble/plot?address='+encodeURIComponent(address),{cache:'no-store'}).then(r=>r.text()).then(h=>{const e=document.getElementById('rssi-plot');if(e)e.innerHTML=h;}));}await Promise.all(jobs);}catch(e){}finally{updating=false;}}"
      "setInterval(function(){fetch('/api/ble/status',{cache:'no-store'}).then(r=>r.json()).then(function(s){if(toggle&&toggle.checked)applyStatus(s);if(s.scan!==scan){scan=s.scan;if(toggle&&toggle.checked)repaint();}}).catch(function(){});},2000);})();</script>";
    diagnosticSendContent(refreshScript);
  }
  diagnosticSendContent("</div></body></html>");
  diagnosticSendContent("");
  markWebResponsePhase("footer-scripts");
  endWebResponseProfile();
}


// Purpose: Returns only the Observed BLE Devices card content for live-update repainting.
void handleBleObservedFragment() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", "");
  diagnosticSendContent("<h2>Observed BLE Devices</h2><div class=\"note\">One row per retained BLE address. Click a column header to sort.</div>");
  sendBleSummaryTable();
  diagnosticSendContent("");
}

// Purpose: Returns only the selected BLE RSSI plot fragment for live-update repainting.
void handleBlePlotFragment() {
  String selectedAddress = server.hasArg("address") ? server.arg("address") : "";
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", "");
  if (selectedAddress.length()) {
    String selectedName = latestBleNameForAddress(selectedAddress);
    String identity = (selectedName.length() && selectedName != "(unnamed)") ? selectedName : selectedAddress;
    diagnosticSendContent("<h2>RSSI History &mdash; " + htmlEscape(identity) + "</h2>");
    diagnosticSendContent("<div class=\"row developer-only\"><span class=\"label\">BLE Address</span><span class=\"value\">" + htmlEscape(selectedAddress) + "</span></div>");
    sendBleRssiHistoryPlot(selectedAddress);
  } else {
    diagnosticSendContent("<h2>RSSI History</h2><p>Select a BLE address below to display RSSI history.</p>");
  }
  diagnosticSendContent("");
}

// Purpose: Runs the user-requested immediate BLE scan endpoint.
void handleBLEScanNow() {
  markExplicitUserInteraction();
  if (!bleSurveyEnabled) {
    bleStatusMessage =
      "BLE scan not started because Bluetooth Survey is disabled in Settings.";
    redirectToBLEPage();
    return;
  }

  performLoggedBLEScanWithTrigger("web-manual");
  redirectToBLEPage();
}


// ============================================================
// System status, diagnostics, and settings
// ============================================================

// Purpose: Captures hardware and firmware properties that remain constant until restart.
void captureBootStaticSystemInfo() {
  bootStaticSystemInfo.resetReason = esp_reset_reason();
  const esp_partition_t* runningPartition = esp_ota_get_running_partition();
  bootStaticSystemInfo.appPartitionBytes = runningPartition ? runningPartition->size : 0;
  bootStaticSystemInfo.sketchBytes = ESP.getSketchSize();
  bootStaticSystemInfo.unusedAppBytes =
      bootStaticSystemInfo.appPartitionBytes > bootStaticSystemInfo.sketchBytes
        ? bootStaticSystemInfo.appPartitionBytes - bootStaticSystemInfo.sketchBytes
        : 0;
  bootStaticSystemInfo.flashBytes = ESP.getFlashChipSize();
  bootStaticSystemInfo.chipRevision = ESP.getChipRevision();
  bootStaticSystemInfo.chipCores = ESP.getChipCores();
  bootStaticSystemInfo.cpuFreqMHz = ESP.getCpuFreqMHz();
  snprintf(bootStaticSystemInfo.chipModel, sizeof(bootStaticSystemInfo.chipModel), "%s", ESP.getChipModel());
}

// Purpose: Converts the ESP-IDF reset-reason enum into a readable diagnostic label.
String resetReasonLabel(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "Power-on";
    case ESP_RST_EXT: return "External reset";
    case ESP_RST_SW: return "Software restart";
    case ESP_RST_PANIC: return "Panic / exception";
    case ESP_RST_INT_WDT: return "Interrupt watchdog";
    case ESP_RST_TASK_WDT: return "Task watchdog";
    case ESP_RST_WDT: return "Other watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep-sleep wake";
    case ESP_RST_BROWNOUT: return "Brownout";
    case ESP_RST_SDIO: return "SDIO";
    default: return "Unknown";
  }
}

// Purpose: Converts the ESP32 Wi-Fi mode enum into a readable mode label.
String wifiModeLabel(wifi_mode_t mode) {
  switch (mode) {
    case WIFI_MODE_NULL: return "Off";
    case WIFI_MODE_STA: return "STA";
    case WIFI_MODE_AP: return "AP";
    case WIFI_MODE_APSTA: return "AP+STA";
    default: return "Unknown";
  }
}

// Purpose: Builds one consistent HTML status row for System Health component checks.
String selfTestRow(const String& name, const String& state, const String& detail) {
  String css = state == "PASS" ? "status-pass" : (state == "WARN" ? "status-warn" : "status-fail");
  return "<div class=\"row\"><span class=\"label\">" + htmlEscape(name) + "</span><span class=\"value \"" + css + "\">" + state + " - " + htmlEscape(detail) + "</span></div>";
}

// Purpose: Builds the System page: Device, health, memory, network, diagnostics export, restart continuity, and developer test tools.
void handleSystemStatus() {
  beginWebResponseProfile("/system");
  uint32_t workStartMs = millis();
  markExplicitUserInteraction();
  recordWebWorkTiming("mark-user-interaction", workStartMs);
  uint8_t primaryChannel = 0;
  wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
  workStartMs = millis();
  esp_err_t channelResult = esp_wifi_get_channel(&primaryChannel, &secondary);
  recordWebWorkTiming("wifi-channel-query", workStartMs);
  workStartMs = millis();
  size_t freeHeap = ESP.getFreeHeap();
  size_t minFreeHeap = ESP.getMinFreeHeap();
  size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  float largestPct = freeHeap ? (100.0f * largestBlock / freeHeap) : 0.0f;
  esp_reset_reason_t rr = bootStaticSystemInfo.resetReason;
  size_t appPartitionBytes = bootStaticSystemInfo.appPartitionBytes;
  size_t unusedAppBytes = bootStaticSystemInfo.unusedAppBytes;
  recordWebWorkTiming("heap-static-system-info", workStartMs);

  workStartMs = millis();
  bool wh = scanHistory && wifiApTable && wifiScanMetadata && scanHistoryCapacity>=MIN_SCAN_HISTORY_RECORDS && scanHistoryRetentionLimit>=MIN_SCAN_HISTORY_RECORDS && scanHistoryRetentionLimit<=scanHistoryCapacity && historyCount<=scanHistoryRetentionLimit;
  size_t wifiIntegrityAnomalies = wifiHistoryIntegrityAnomalies();
  bool wifiIntegrityOk = wifiIntegrityAnomalies == 0;
  bool bh = !bleSurveyEnabled || (bleHistory && bleAddressTable && bleScanMetadata && bleHistoryCapacity>=MIN_BLE_HISTORY_RECORDS && bleHistoryRetentionLimit>=MIN_BLE_HISTORY_RECORDS && bleHistoryRetentionLimit<=bleHistoryCapacity && bleHistoryCount<=bleHistoryRetentionLimit);
  bool cfg = scanIntervalSeconds>=MIN_SCAN_INTERVAL_SECONDS && scanIntervalSeconds<=MAX_SCAN_INTERVAL_SECONDS && (!bleSurveyEnabled || (bleScanIntervalSeconds>=MIN_SCAN_INTERVAL_SECONDS && bleScanIntervalSeconds<=MAX_SCAN_INTERVAL_SECONDS));
  bool initialDone = !initialWifiScanPending && (!bleSurveyEnabled || !initialBleScanPending);
  bool autos = !bleSurveyEnabled || autoBleScanEnabled;
  bool wifiCadenceOverdue = wifiAutoScanCadenceOverdue();
  bool currentHeapOk = bleSurveyEnabled ? freeHeap >= 24*1024 : freeHeap >= HEAP_WARN_BYTES;
  bool lowWaterOk = bleSurveyEnabled ? minFreeHeap >= DUAL_RADIO_MIN_HEAP_WARN_BYTES : minFreeHeap >= HEAP_WARN_BYTES;
  bool memoryOk = currentHeapOk && lowWaterOk;
  bool resetWarn = rr==ESP_RST_PANIC || rr==ESP_RST_INT_WDT || rr==ESP_RST_TASK_WDT || rr==ESP_RST_WDT || rr==ESP_RST_BROWNOUT;
  bool overallFail = !wifiSubsystemInitialized || (bleSurveyEnabled && !bleInitialized) || !wh || !bh || !cfg;
  bool overallWarn = !overallFail && (!initialDone || !autos || wifiCadenceOverdue || !wifiIntegrityOk || !memoryOk || !spiffsMounted || !mdnsStarted || resetWarn);
  recordWebWorkTiming("health-state-calculation", workStartMs);

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  workStartMs = millis();
  server.send(200, "text/html", "");
  recordWebWorkTiming("response-start", workStartMs);
  diagnosticSendContent("<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>ESP32 System</title>");
  workStartMs = millis();
  sendThemeBootstrapScript();
  recordWebWorkTiming("theme-bootstrap", workStartMs);
  workStartMs = millis();
  diagnosticSendContent(pageStyles());
  recordWebWorkTiming("page-styles", workStartMs);
  diagnosticSendContent("</head><body><div class=\"container\">");
  workStartMs = millis();
  sendSiteNavigation("system");
  recordWebWorkTiming("site-navigation", workStartMs);
  diagnosticSendContent("<h1>System</h1>");
  markWebResponsePhase("header");

  workStartMs = millis();
  String s; s.reserve(5200);
  s += "<div class=\"card\"><h2>Device</h2>"
    "<div class=\"row\"><span class=\"label\">Firmware Version</span><span class=\"value\">" + htmlEscape(FIRMWARE_VERSION) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Uptime</span><span class=\"value\">" + htmlEscape(getUptimeString()) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Last Reset</span><span class=\"value\">" + htmlEscape(resetReasonLabel(rr)) + "</span></div>"
    "<div class=\"row advanced-only\"><span class=\"label\">Build</span><span class=\"value\">" + htmlEscape(firmwareBuildTimestamp()) + "</span></div>"
    "<div class=\"row developer-only\"><span class=\"label\">Firmware File</span><span class=\"value\">" + htmlEscape(FIRMWARE_FILE) + "</span></div>"
    "<div class=\"row developer-only\"><span class=\"label\">Arduino ESP32 Core</span><span class=\"value\">" + String(ESP_ARDUINO_VERSION_STR) + "</span></div>"
    "<div class=\"row developer-only\"><span class=\"label\">ESP-IDF</span><span class=\"value\">" + String(esp_get_idf_version()) + "</span></div>"
    "<div class=\"row developer-only\"><span class=\"label\">Chip</span><span class=\"value\">" + String(bootStaticSystemInfo.chipModel) + ", rev " + String(bootStaticSystemInfo.chipRevision) + "</span></div>"
    "<div class=\"row developer-only\"><span class=\"label\">CPU</span><span class=\"value\">" + String(bootStaticSystemInfo.cpuFreqMHz) + " MHz, " + String(bootStaticSystemInfo.chipCores) + " cores</span></div></div>";

  String overall = overallFail ? "FAIL" : (overallWarn ? "WARN" : "PASS");
  String bleHealth = bleSurveyEnabled ? (bleInitialized ? "PASS" : "FAIL") : "Disabled";
  s += "<div class=\"card\"><h2>System Health</h2>"
    "<div class=\"row\"><span class=\"label\">Wi-Fi Survey</span><span class=\"value\">" + String(wifiSubsystemInitialized ? "PASS" : "FAIL") + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Bluetooth Survey</span><span class=\"value\">" + bleHealth + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Automatic Surveying</span><span class=\"value\">" + String(autos && !wifiCadenceOverdue ? "PASS" : "WARN") + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Memory</span><span class=\"value\">" + String(memoryOk ? "PASS" : "WARN") + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Overall</span><span class=\"value\"><strong>" + overall + "</strong></span></div>";
  s += "<div class=\"advanced-only\">";
  s += selfTestRow("Wi-Fi history buffer", wh ? "PASS" : "FAIL", wh ? "allocated and sane" : "allocation/capacity invalid");
  s += selfTestRow("Wi-Fi history integrity", wifiIntegrityOk ? "PASS" : "WARN", wifiIntegrityOk ? "metadata references and ordering are consistent" : String(wifiIntegrityAnomalies) + " retained-history anomaly(s) detected");
  s += selfTestRow("BLE history buffer", bh ? "PASS" : "FAIL", !bleSurveyEnabled ? "not allocated by design" : (bh ? "allocated and sane" : "allocation/capacity invalid"));
  s += selfTestRow("Scan configuration", cfg ? "PASS" : "FAIL", cfg ? "survey intervals within valid range" : "one or more values out of range");
  s += selfTestRow("Initial boot scans", initialDone ? "PASS" : "WARN", initialDone ? "required initial scans completed" : "one or more initial scans pending");
  s += selfTestRow("Wi-Fi auto-scan cadence", wifiCadenceOverdue ? "WARN" : "PASS", wifiAutoScanDiagnosticLabel());
  s += selfTestRow("Wi-Fi scan timing", wifiScanDurationCount ? "PASS" : "WARN", wifiScanDurationSummaryLabel());
  s += selfTestRow("Restart checkpoint storage", spiffsMounted ? "PASS" : "WARN", spiffsMounted ? sessionCheckpointStatus : "SPIFFS unavailable");
  s += selfTestRow("mDNS hostname", mdnsStarted ? "PASS" : "WARN", mdnsStarted ? mdnsWebAddress() : mdnsStatusMessage);
  s += selfTestRow("Heap reserve", memoryOk ? "PASS" : "WARN", String(freeHeap/1024) + " KB free; " + String(minFreeHeap/1024) + " KB minimum");
  s += selfTestRow("Boot/reset diagnostic", resetWarn ? "WARN" : "PASS", resetReasonLabel(rr));
  s += "</div><div class=\"developer-only\">" + selfTestRow("Application space", unusedAppBytes>64*1024 ? "PASS" : "WARN", String(unusedAppBytes/1024) + " KB unused in running app partition") + "</div></div>";

  s += "<div class=\"card advanced-only\"><h2>Memory</h2>"
    "<div class=\"row\"><span class=\"label\">Free Heap</span><span class=\"value\">" + String(freeHeap/1024.0,1) + " KB</span></div>"
    "<div class=\"row\"><span class=\"label\">Minimum Free Heap</span><span class=\"value\">" + String(minFreeHeap/1024.0,1) + " KB</span></div>"
    "<div class=\"row\"><span class=\"label\">Survey Memory Mode</span><span class=\"value\">" + String(bleSurveyEnabled ? "Wi-Fi + Bluetooth" : "Wi-Fi only") + "</span></div>"
    "<div class=\"row developer-only\"><span class=\"label\">Largest Free Block</span><span class=\"value\">" + String(largestBlock/1024.0,1) + " KB (" + String(largestPct,1) + "%)</span></div>"
    "<div class=\"row developer-only\"><span class=\"label\">Flash Size</span><span class=\"value\">" + String(bootStaticSystemInfo.flashBytes/1024.0/1024.0,2) + " MB</span></div>"
    "<div class=\"row developer-only\"><span class=\"label\">Sketch Size</span><span class=\"value\">" + String(bootStaticSystemInfo.sketchBytes/1024.0,1) + " KB</span></div>"
    "<div class=\"row developer-only\"><span class=\"label\">App Partition Size</span><span class=\"value\">" + String(appPartitionBytes/1024.0,1) + " KB</span></div>"
    "<div class=\"row developer-only\"><span class=\"label\">Unused App Partition</span><span class=\"value\">" + String(unusedAppBytes/1024.0,1) + " KB</span></div>"
    "<div class=\"row developer-only\"><span class=\"label\">Target Heap Reserve</span><span class=\"value\">" + String((bleSurveyEnabled?DUAL_RADIO_HEAP_RESERVE_BYTES:HISTORY_HEAP_RESERVE_BYTES)/1024) + " KB at history allocation</span></div></div>";

  s += "<div class=\"card\"><h2>Network</h2>"
    "<div class=\"row\"><span class=\"label\">Infrastructure Wi-Fi</span><span class=\"value\">" + String(WiFi.status()==WL_CONNECTED ? "Connected" : "Disconnected") + "</span></div>";
  if (WiFi.status()==WL_CONNECTED) s += "<div class=\"row\"><span class=\"label\">Network / Signal</span><span class=\"value\">" + htmlEscape(WiFi.SSID()) + " / " + String(WiFi.RSSI()) + " dBm</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Device AP</span><span class=\"value\">" + String(apRunning ? "Running" : "Disabled") + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Broadcast SSID</span><span class=\"value\">" + htmlEscape(apSSID) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Friendly Web Address</span><span class=\"value\"><a href=\"" + htmlEscape(mdnsWebAddress()) + "\">" + htmlEscape(mdnsWebAddress()) + "</a></span></div>"
    "<div class=\"row advanced-only\"><span class=\"label\">Infrastructure Auto-Reconnect</span><span class=\"value\">Native enabled; " + String(nativeReconnectObservedCount) + " transition(s) observed</span></div>"
    "<div class=\"row advanced-only\"><span class=\"label\">STA MAC</span><span class=\"value\">" + WiFi.macAddress() + "</span></div>"
    "<div class=\"row advanced-only\"><span class=\"label\">AP MAC</span><span class=\"value\">" + WiFi.softAPmacAddress() + "</span></div>"
    "<div class=\"row advanced-only\"><span class=\"label\">Radio Channel</span><span class=\"value\">" + String(channelResult==ESP_OK ? String(primaryChannel) : String("Unavailable")) + "</span></div>"
    "<div class=\"row advanced-only\"><span class=\"label\">mDNS Status</span><span class=\"value\">" + htmlEscape(mdnsStatusMessage) + "</span></div>"
    "<div class=\"row developer-only\"><span class=\"label\">Wi-Fi Mode</span><span class=\"value\">" + wifiModeLabel(WiFi.getMode()) + "</span></div></div>";
  recordWebWorkTiming("device-health-network-build", workStartMs);
  diagnosticSendContent(s);
  markWebResponsePhase("device-health-memory-network");

  diagnosticSendContent("<div class=\"card advanced-only\"><h2>Diagnostics Export</h2>"
    "<div class=\"buttons\"><a class=\"button\" href=\"/status.json\">Download Diagnostics</a></div>"
    "<div class=\"note\">The export contains a current diagnostic snapshot plus the configured number of recent RAM-buffered diagnostic events. Retained Wi-Fi and BLE observation rows remain in their survey CSV exports.</div>"
    "<div class=\"developer-only\"><div class=\"survey-control-row\"><div class=\"control\"><label for=\"diag-event-limit\">Recent diagnostic events</label>"
    "<input id=\"diag-event-limit\" type=\"number\" min=\"0\" max=\"" + String(DIAGNOSTIC_EVENT_CAPACITY) + "\" value=\"" + String(diagnosticExportEventLimit) + "\"></div>"
    "<span id=\"diag-event-limit-state\" class=\"save-state\"></span></div>"
    "<div class=\"row\"><span class=\"label\">Events currently retained</span><span class=\"value\">" + String(diagnosticEventCount) + " / " + String(DIAGNOSTIC_EVENT_CAPACITY) + "</span></div>"
    "<div class=\"note\">Recent diagnostic history is bounded in RAM and is not continuously written to flash.</div></div></div>"
    "<script>(function(){const i=document.getElementById('diag-event-limit');const st=document.getElementById('diag-event-limit-state');if(!i)return;async function save(){let v=parseInt(i.value,10);if(!Number.isFinite(v))return;v=Math.max(0,Math.min(" + String(DIAGNOSTIC_EVENT_CAPACITY) + ",v));i.value=v;if(st)st.textContent='Saving…';try{const r=await fetch('/api/diag/event-limit?events='+encodeURIComponent(v),{method:'POST',cache:'no-store'});if(!r.ok)throw new Error();const j=await r.json();i.value=j.events;if(st){st.textContent='Saved';setTimeout(()=>{st.textContent='';},1400);}}catch(e){if(st)st.textContent='Save failed';}}i.addEventListener('change',save);i.addEventListener('keydown',function(e){if(e.key==='Enter'){e.preventDefault();save();i.blur();}});})();</script>");
  markWebResponsePhase("diagnostics-export");

  diagnosticSendContent("<div class=\"card developer-only\"><h2>Boot Heap Checkpoints</h2><div class=\"note\">Startup instrumentation showing where heap is consumed.</div><div class=\"table-scroll\"><table><thead><tr><th>Stage</th><th>Free Heap</th><th>Min Free</th><th>Largest Block</th></tr></thead><tbody>");
  for (size_t i=0;i<bootHeapCheckpointCount;i++) {
    const BootHeapCheckpoint& cp=bootHeapCheckpoints[i];
    diagnosticSendContent("<tr><td>"+htmlEscape(String(cp.stage))+"</td><td class=\"signal\">"+String(cp.freeHeap/1024.0,1)+" KB</td><td class=\"signal\">"+String(cp.minimumFreeHeap/1024.0,1)+" KB</td><td class=\"signal\">"+String(cp.largestFreeBlock/1024.0,1)+" KB</td></tr>");
  }
  diagnosticSendContent("</tbody></table></div></div>");
  markWebResponsePhase("boot-checkpoints");

  diagnosticSendContent("<div class=\"card advanced-only\"><h2>Session</h2><div class=\"row\"><span class=\"label\">Restart Checkpoint</span><span class=\"value\">" + htmlEscape(sessionCheckpointStatus) + "</span></div><div class=\"row\"><span class=\"label\">Restored This Boot</span><span class=\"value\">" + String(sessionRestoredThisBoot ? "Yes" : "No") + "</span></div>"
    "<div class=\"developer-only\"><div class=\"test-tool-actions\"><form action=\"/session-save\" method=\"post\"><button type=\"submit\">Save Restart Checkpoint</button></form><form action=\"/session-discard\" method=\"post\"><button type=\"submit\">Discard Restart Checkpoint</button></form></div><div class=\"note\">Restart checkpoints preserve the current RAM history through controlled reboots and are consumed after successful restore. Persistent logging is a separate future feature.</div></div></div>");
  String historyTestTools = "<div class=\"card developer-only\"><h2>History Test Tools</h2>"
    "<div class=\"test-tool-group\"><h3>Wi-Fi History</h3><div class=\"test-tool-actions\">"
    "<form action=\"/history-prefill\" method=\"post\"><input type=\"hidden\" name=\"radio\" value=\"wifi\"><input type=\"hidden\" name=\"percent\" value=\"50\"><button type=\"submit\">Fill to 50%</button></form>"
    "<form action=\"/history-prefill\" method=\"post\"><input type=\"hidden\" name=\"radio\" value=\"wifi\"><input type=\"hidden\" name=\"percent\" value=\"75\"><button type=\"submit\">Fill to 75%</button></form>"
    "<form action=\"/history-prefill\" method=\"post\"><input type=\"hidden\" name=\"radio\" value=\"wifi\"><input type=\"hidden\" name=\"percent\" value=\"95\"><button type=\"submit\">Fill to 95%</button></form>"
    "</div></div>";
  if (bleSurveyEnabled && bleHistory && bleAddressTable && bleScanMetadata) {
    historyTestTools += "<div class=\"test-tool-group\"><h3>Bluetooth History</h3><div class=\"test-tool-actions\">"
      "<form action=\"/history-prefill\" method=\"post\"><input type=\"hidden\" name=\"radio\" value=\"ble\"><input type=\"hidden\" name=\"percent\" value=\"50\"><button type=\"submit\">Fill to 50%</button></form>"
      "<form action=\"/history-prefill\" method=\"post\"><input type=\"hidden\" name=\"radio\" value=\"ble\"><input type=\"hidden\" name=\"percent\" value=\"75\"><button type=\"submit\">Fill to 75%</button></form>"
      "<form action=\"/history-prefill\" method=\"post\"><input type=\"hidden\" name=\"radio\" value=\"ble\"><input type=\"hidden\" name=\"percent\" value=\"95\"><button type=\"submit\">Fill to 95%</button></form>"
      "</div></div>";
  } else {
    historyTestTools += "<div class=\"test-tool-group\"><h3>Bluetooth History</h3><div class=\"note\">Enable Bluetooth Survey to use Bluetooth history prefill.</div></div>";
  }
  historyTestTools += "<div class=\"note\"><strong>TEST FEATURE:</strong> inserts synthetic TEST-PREFILL observations directly into the real compact Wi-Fi or Bluetooth history. Synthetic data is not a substitute for radio/endurance testing and remains in UI/CSV output until cleared or rolled out.</div></div>";
  diagnosticSendContent(historyTestTools);
  diagnosticSendContent("<div class=\"footer\">ESP32 Web Interface</div>");
  sendThemeScript(); diagnosticSendContent("</div></body></html>"); diagnosticSendContent("");
  markWebResponsePhase("session-tools-footer");
  endWebResponseProfile();
}


// ============================================================
// Status export and configuration backup / restore
// ============================================================

const uint32_t STATUS_SCHEMA_VERSION = 2;
const uint32_t CONFIG_SCHEMA_VERSION = 1;
const size_t MAX_CONFIG_IMPORT_BYTES = 4096;

// Purpose: Escapes a string for safe inclusion in generated JSON.
String jsonEscape(String input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((uint8_t)c < 0x20) {
          char escaped[7];
          snprintf(escaped, sizeof(escaped), "\\u%04X", (unsigned int)(uint8_t)c);
          out += escaped;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// Purpose: Returns a JSON string literal including surrounding quotes.
String jsonQuoted(const String& value) {
  return String("\"") + jsonEscape(value) + "\"";
}

struct PortableConfig {
  unsigned long wifiScanIntervalSeconds;
  bool bluetoothSurveyEnabled;
  bool statusLedEnabled;
  bool liveUpdatesEnabled;
  bool mdnsHostnameAutomatic;
  String mdnsHostname;
  bool accessPointEnabled;
  bool accessPointSSIDAutomatic;
  String accessPointSSID;
};

// Explicit prototypes avoid Arduino 1.8.x auto-prototype placement issues
// with custom types in .ino sketches.
PortableConfig readPersistedPortableConfig();
String portableConfigJson(const PortableConfig& c);

// Purpose: Reads the non-secret persisted configuration values that are eligible for export/import.
PortableConfig readPersistedPortableConfig() {
  PortableConfig c = {};

  preferences.begin("survey", true);
  c.wifiScanIntervalSeconds =
      preferences.getULong("wifiInterval", scanIntervalSeconds);
  c.bluetoothSurveyEnabled =
      preferences.getBool("bleEnabled", bleSurveyEnabled);
  c.statusLedEnabled =
      preferences.getBool("ledEnabled", statusLedEnabled);
  c.liveUpdatesEnabled =
      preferences.getBool("webRefresh", webAutoRefreshEnabled);
  c.mdnsHostnameAutomatic = !preferences.isKey("hostname");
  c.mdnsHostname = c.mdnsHostnameAutomatic
      ? String("")
      : normalizedMdnsHostname(
          preferences.getString("hostname", "")
        );
  preferences.end();

  if (c.wifiScanIntervalSeconds < MIN_SCAN_INTERVAL_SECONDS)
    c.wifiScanIntervalSeconds = MIN_SCAN_INTERVAL_SECONDS;
  if (c.wifiScanIntervalSeconds > MAX_SCAN_INTERVAL_SECONDS)
    c.wifiScanIntervalSeconds = MAX_SCAN_INTERVAL_SECONDS;

  preferences.begin("ap", true);
  c.accessPointEnabled = preferences.getBool("enabled", true);
  c.accessPointSSIDAutomatic = !preferences.isKey("ssid");
  c.accessPointSSID = c.accessPointSSIDAutomatic
      ? String("")
      : preferences.getString("ssid", "");
  preferences.end();

  return c;
}

// Purpose: Serializes portable non-secret configuration into the configuration-export JSON schema.
String portableConfigJson(const PortableConfig& c) {
  String json;
  json.reserve(700);
  json += "{\n";
  json += "  \"configVersion\":" + String(CONFIG_SCHEMA_VERSION) + ",\n";
  json += "  \"wifiScanIntervalSeconds\":" + String(c.wifiScanIntervalSeconds) + ",\n";
  json += "  \"bluetoothSurveyEnabled\":" + String(c.bluetoothSurveyEnabled ? "true" : "false") + ",\n";
  json += "  \"statusLedEnabled\":" + String(c.statusLedEnabled ? "true" : "false") + ",\n";
  json += "  \"liveUpdatesEnabled\":" + String(c.liveUpdatesEnabled ? "true" : "false") + ",\n";
  json += "  \"mdnsHostnameAutomatic\":" + String(c.mdnsHostnameAutomatic ? "true" : "false") + ",\n";
  json += "  \"mdnsHostname\":" + jsonQuoted(c.mdnsHostname) + ",\n";
  json += "  \"accessPointEnabled\":" + String(c.accessPointEnabled ? "true" : "false") + ",\n";
  json += "  \"accessPointSSIDAutomatic\":" + String(c.accessPointSSIDAutomatic ? "true" : "false") + ",\n";
  json += "  \"accessPointSSID\":" + jsonQuoted(c.accessPointSSID) + ",\n";
  json += "  \"credentialsIncluded\":false\n";
  json += "}\n";
  return json;
}

// Purpose: Streams the portable non-secret configuration JSON download.
void handleConfigExport() {
  markExplicitUserInteraction();
  PortableConfig c = readPersistedPortableConfig();
  server.sendHeader(
    "Content-Disposition",
    "attachment; filename=\"wireless_surveyor_config.json\""
  );
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", portableConfigJson(c));
}

class FlatConfigJsonParser {
public:
  FlatConfigJsonParser(const String& source)
      : s(source), pos(0), errorMessage("") {}

  bool parse(PortableConfig& out) {
    bool seenVersion = false;
    bool seenInterval = false;
    bool seenBle = false;
    bool seenLed = false;
    bool seenLive = false;
    bool seenHostnameAuto = false;
    bool seenHostname = false;
    bool seenApEnabled = false;
    bool seenApSsidAuto = false;
    bool seenApSsid = false;
    bool seenCredentials = false;

    skipWs();
    if (!consume('{')) return fail("Configuration must be a JSON object.");
    skipWs();
    if (peek('}')) {
      consume('}');
      return fail("Configuration object is empty.");
    }

    while (true) {
      String key;
      if (!parseString(key)) return fail("Expected a quoted configuration key.");
      skipWs();
      if (!consume(':')) return fail("Expected ':' after configuration key.");
      skipWs();

      if (key == "configVersion") {
        if (seenVersion) return fail("Duplicate configVersion.");
        seenVersion = true;
        unsigned long value = 0;
        if (!parseUnsigned(value)) return fail("configVersion must be an integer.");
        if (value != CONFIG_SCHEMA_VERSION)
          return fail("Unsupported configVersion.");
      } else if (key == "wifiScanIntervalSeconds") {
        if (seenInterval) return fail("Duplicate wifiScanIntervalSeconds.");
        seenInterval = true;
        unsigned long value = 0;
        if (!parseUnsigned(value))
          return fail("wifiScanIntervalSeconds must be an integer.");
        out.wifiScanIntervalSeconds = value;
      } else if (key == "bluetoothSurveyEnabled") {
        if (seenBle) return fail("Duplicate bluetoothSurveyEnabled.");
        seenBle = true;
        if (!parseBool(out.bluetoothSurveyEnabled))
          return fail("bluetoothSurveyEnabled must be true or false.");
      } else if (key == "statusLedEnabled") {
        if (seenLed) return fail("Duplicate statusLedEnabled.");
        seenLed = true;
        if (!parseBool(out.statusLedEnabled))
          return fail("statusLedEnabled must be true or false.");
      } else if (key == "liveUpdatesEnabled") {
        if (seenLive) return fail("Duplicate liveUpdatesEnabled.");
        seenLive = true;
        if (!parseBool(out.liveUpdatesEnabled))
          return fail("liveUpdatesEnabled must be true or false.");
      } else if (key == "mdnsHostnameAutomatic") {
        if (seenHostnameAuto) return fail("Duplicate mdnsHostnameAutomatic.");
        seenHostnameAuto = true;
        if (!parseBool(out.mdnsHostnameAutomatic))
          return fail("mdnsHostnameAutomatic must be true or false.");
      } else if (key == "mdnsHostname") {
        if (seenHostname) return fail("Duplicate mdnsHostname.");
        seenHostname = true;
        if (!parseString(out.mdnsHostname))
          return fail("mdnsHostname must be a string.");
      } else if (key == "accessPointEnabled") {
        if (seenApEnabled) return fail("Duplicate accessPointEnabled.");
        seenApEnabled = true;
        if (!parseBool(out.accessPointEnabled))
          return fail("accessPointEnabled must be true or false.");
      } else if (key == "accessPointSSIDAutomatic") {
        if (seenApSsidAuto) return fail("Duplicate accessPointSSIDAutomatic.");
        seenApSsidAuto = true;
        if (!parseBool(out.accessPointSSIDAutomatic))
          return fail("accessPointSSIDAutomatic must be true or false.");
      } else if (key == "accessPointSSID") {
        if (seenApSsid) return fail("Duplicate accessPointSSID.");
        seenApSsid = true;
        if (!parseString(out.accessPointSSID))
          return fail("accessPointSSID must be a string.");
      } else if (key == "credentialsIncluded") {
        if (seenCredentials) return fail("Duplicate credentialsIncluded.");
        seenCredentials = true;
        bool included = false;
        if (!parseBool(included))
          return fail("credentialsIncluded must be true or false.");
        if (included)
          return fail("Credential-bearing configuration imports are not supported.");
      } else {
        return fail("Unsupported configuration key: " + key);
      }

      skipWs();
      if (consume('}')) break;
      if (!consume(','))
        return fail("Expected ',' or '}' after configuration value.");
      skipWs();
    }

    skipWs();
    if (pos != s.length())
      return fail("Unexpected content after the JSON object.");

    if (!seenVersion || !seenInterval || !seenBle || !seenLed ||
        !seenLive || !seenHostnameAuto || !seenHostname ||
        !seenApEnabled || !seenApSsidAuto || !seenApSsid ||
        !seenCredentials)
      return fail("Configuration is missing one or more required fields.");

    out.mdnsHostname = normalizedMdnsHostname(out.mdnsHostname);
    out.accessPointSSID.trim();

    if (out.wifiScanIntervalSeconds < MIN_SCAN_INTERVAL_SECONDS ||
        out.wifiScanIntervalSeconds > MAX_SCAN_INTERVAL_SECONDS)
      return fail(
        "wifiScanIntervalSeconds must be between " +
        String(MIN_SCAN_INTERVAL_SECONDS) + " and " +
        String(MAX_SCAN_INTERVAL_SECONDS) + "."
      );

    if (!out.mdnsHostnameAutomatic &&
        !isValidMdnsHostname(out.mdnsHostname))
      return fail("mdnsHostname is invalid.");

    if (!out.accessPointSSIDAutomatic) {
      if (out.accessPointSSID.length() < 1 ||
          out.accessPointSSID.length() > 32)
        return fail("accessPointSSID must contain 1 to 32 characters.");
      for (size_t i = 0; i < out.accessPointSSID.length(); i++) {
        if ((uint8_t)out.accessPointSSID[i] < 0x20)
          return fail("accessPointSSID contains a control character.");
      }
    }

    return true;
  }

  String error() const { return errorMessage; }

private:
  const String& s;
  size_t pos;
  String errorMessage;

  void skipWs() {
    while (pos < s.length()) {
      char c = s[pos];
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
      pos++;
    }
  }

  bool peek(char c) const {
    return pos < s.length() && s[pos] == c;
  }

  bool consume(char c) {
    skipWs();
    if (!peek(c)) return false;
    pos++;
    return true;
  }

  bool fail(const String& message) {
    errorMessage = message;
    return false;
  }

  bool parseBool(bool& value) {
    skipWs();
    if (s.substring(pos, pos + 4) == "true") {
      pos += 4;
      value = true;
      return true;
    }
    if (s.substring(pos, pos + 5) == "false") {
      pos += 5;
      value = false;
      return true;
    }
    return false;
  }

  bool parseUnsigned(unsigned long& value) {
    skipWs();
    if (pos >= s.length() || s[pos] < '0' || s[pos] > '9') return false;
    unsigned long result = 0;
    while (pos < s.length() && s[pos] >= '0' && s[pos] <= '9') {
      unsigned long digit = (unsigned long)(s[pos] - '0');
      if (result > (0xFFFFFFFFUL - digit) / 10UL) return false;
      result = result * 10UL + digit;
      pos++;
    }
    value = result;
    return true;
  }

  bool parseString(String& value) {
    skipWs();
    if (pos >= s.length() || s[pos] != '"') return false;
    pos++;
    value = "";
    while (pos < s.length()) {
      char c = s[pos++];
      if (c == '"') return true;
      if ((uint8_t)c < 0x20) return false;
      if (c != '\\') {
        value += c;
        continue;
      }

      if (pos >= s.length()) return false;
      char e = s[pos++];
      switch (e) {
        case '"': value += '"'; break;
        case '\\': value += '\\'; break;
        case '/': value += '/'; break;
        case 'b': value += '\b'; break;
        case 'f': value += '\f'; break;
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        default:
          // Unicode escapes are rejected because the exported configuration
          // configuration is ASCII field names plus UTF-8 string content.
          return false;
      }
    }
    return false;
  }
};

String configImportResultJson(
  bool ok,
  const String& message,
  uint32_t appliedCount,
  bool restartRequired,
  const String& restartReason
) {
  String json;
  json.reserve(500);
  json += "{\"ok\":";
  json += ok ? "true" : "false";
  json += ",\"message\":" + jsonQuoted(message);
  json += ",\"applied\":" + String(appliedCount);
  json += ",\"restartRequired\":";
  json += restartRequired ? "true" : "false";
  json += ",\"restartReason\":" + jsonQuoted(restartReason);
  json += "}";
  return json;
}

// Purpose: Validates an uploaded configuration JSON document and applies supported settings atomically.
void handleConfigImport() {
  markExplicitUserInteraction();

  if (!server.hasArg("plain")) {
    server.send(
      400,
      "application/json",
      configImportResultJson(
        false, "Missing JSON request body.", 0, false, ""
      )
    );
    return;
  }

  String body = server.arg("plain");
  if (body.length() == 0 || body.length() > MAX_CONFIG_IMPORT_BYTES) {
    server.send(
      413,
      "application/json",
      configImportResultJson(
        false,
        "Configuration must be between 1 and " +
          String(MAX_CONFIG_IMPORT_BYTES) + " bytes.",
        0, false, ""
      )
    );
    return;
  }

  PortableConfig requested = {};
  FlatConfigJsonParser parser(body);
  if (!parser.parse(requested)) {
    server.send(
      400,
      "application/json",
      configImportResultJson(
        false, parser.error(), 0, false, ""
      )
    );
    return;
  }

  // Validation is complete before this point. No NVS writes occur until the
  // entire supported configuration has been parsed and range-checked.
  PortableConfig previous = readPersistedPortableConfig();
  uint32_t appliedCount = 0;
  bool restartRequired = false;
  String restartReason;

  if (requested.wifiScanIntervalSeconds != previous.wifiScanIntervalSeconds) {
    preferences.begin("survey", false);
    preferences.putULong("wifiInterval", requested.wifiScanIntervalSeconds);
    preferences.end();
    scanIntervalSeconds = requested.wifiScanIntervalSeconds;
    lastAutoScanMs = millis();
    appliedCount++;
  }

  if (requested.statusLedEnabled != previous.statusLedEnabled) {
    preferences.begin("survey", false);
    preferences.putBool("ledEnabled", requested.statusLedEnabled);
    preferences.end();
    statusLedEnabled = requested.statusLedEnabled;
    if (!statusLedEnabled) stopScanLed();
    appliedCount++;
  }

  if (requested.liveUpdatesEnabled != previous.liveUpdatesEnabled) {
    saveWebLiveUpdates(requested.liveUpdatesEnabled);
    appliedCount++;
  }

  if (requested.bluetoothSurveyEnabled != previous.bluetoothSurveyEnabled) {
    saveBleSurveyEnabled(requested.bluetoothSurveyEnabled);
    appliedCount++;
    restartRequired = true;
    restartReason += "Bluetooth survey mode";
  }

  bool hostnameChanged =
      requested.mdnsHostnameAutomatic != previous.mdnsHostnameAutomatic ||
      (!requested.mdnsHostnameAutomatic &&
       requested.mdnsHostname != previous.mdnsHostname);
  if (hostnameChanged) {
    preferences.begin("survey", false);
    if (requested.mdnsHostnameAutomatic)
      preferences.remove("hostname");
    else
      preferences.putString("hostname", requested.mdnsHostname);
    preferences.end();
    appliedCount++;
    restartRequired = true;
    if (restartReason.length()) restartReason += ", ";
    restartReason += "mDNS hostname";
  }

  bool apChanged =
      requested.accessPointEnabled != previous.accessPointEnabled ||
      requested.accessPointSSIDAutomatic != previous.accessPointSSIDAutomatic ||
      (!requested.accessPointSSIDAutomatic &&
       requested.accessPointSSID != previous.accessPointSSID);
  if (apChanged) {
    preferences.begin("ap", false);
    preferences.putBool("enabled", requested.accessPointEnabled);
    if (requested.accessPointSSIDAutomatic)
      preferences.remove("ssid");
    else
      preferences.putString("ssid", requested.accessPointSSID);
    // The existing AP password is intentionally left untouched.
    preferences.end();
    appliedCount++;
    restartRequired = true;
    if (restartReason.length()) restartReason += ", ";
    restartReason += "access point";
  }

  if (restartRequired) {
    String checkpointDetail;
    saveSurveySessionCheckpoint(checkpointDetail);
  }

  server.sendHeader("Cache-Control", "no-store");
  server.send(
    200,
    "application/json",
    configImportResultJson(
      true,
      appliedCount == 0
        ? String("Configuration already matched this device.")
        : String("Configuration validated and applied."),
      appliedCount,
      restartRequired,
      restartReason
    )
  );
}

// Purpose: Saves the number of recent RAM-buffered diagnostic events included in diagnostics export.
void handleDiagnosticEventLimit() {
  markExplicitUserInteraction();
  if (!server.hasArg("events")) {
    server.send(400, "application/json", "{\"saved\":false,\"message\":\"Missing events value\"}");
    return;
  }
  long requested = server.arg("events").toInt();
  if (requested < 0) requested = 0;
  if (requested > (long)DIAGNOSTIC_EVENT_CAPACITY) requested = DIAGNOSTIC_EVENT_CAPACITY;
  diagnosticExportEventLimit = (size_t)requested;
  saveDiagnosticStreamingSettings();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", String("{\"saved\":true,\"events\":") + String(diagnosticExportEventLimit) + "}");
}

// Purpose: Streams the comprehensive diagnostic status snapshot as JSON.
void handleStatusJsonExport() {
  beginWebResponseProfile("/status.json");
  uint32_t workStartMs = millis();
  markExplicitUserInteraction();
  recordWebWorkTiming("mark-user-interaction", workStartMs);
  workStartMs = millis();
  ChannelAnalysis channel = analyzeLatestWifiScan();
  recordWebWorkTiming("channel-analysis-snapshot", workStartMs);
  workStartMs = millis();
  esp_reset_reason_t resetReason = bootStaticSystemInfo.resetReason;
  size_t largestBlock =
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  size_t appPartitionBytes = bootStaticSystemInfo.appPartitionBytes;
  size_t unusedAppBytes = bootStaticSystemInfo.unusedAppBytes;
  recordWebWorkTiming("cached-reset-heap-partition-info", workStartMs);
  workStartMs = millis();
  PortableConfig persisted = readPersistedPortableConfig();
  recordWebWorkTiming("read-persisted-config", workStartMs);

  server.sendHeader(
    "Content-Disposition",
    "attachment; filename=\"wireless_surveyor_status.json\""
  );
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  workStartMs = millis();
  server.send(200, "application/json", "");
  recordWebWorkTiming("response-start", workStartMs);

  diagnosticSendContent("{\n");
  diagnosticSendContent(
    "  \"statusSchemaVersion\":" + String(STATUS_SCHEMA_VERSION) + ",\n"
  );

  workStartMs = millis();
  diagnosticSendContent("  \"firmware\":{");
  diagnosticSendContent("\"file\":" + jsonQuoted(FIRMWARE_FILE));
  diagnosticSendContent(",\"version\":" + jsonQuoted(FIRMWARE_VERSION));
  diagnosticSendContent(",\"arduinoEsp32\":" + jsonQuoted(ESP_ARDUINO_VERSION_STR));
  diagnosticSendContent(",\"espIdf\":" + jsonQuoted(String(esp_get_idf_version())));
  diagnosticSendContent("},\n");
  recordWebWorkTiming("firmware-json-build", workStartMs);
  markWebResponsePhase("firmware");

  diagnosticSendContent("  \"runtime\":{");
  diagnosticSendContent("\"uptimeMs\":" + String(millis()));
  diagnosticSendContent(",\"uptime\":" + jsonQuoted(formatUptime(millis())));
  diagnosticSendContent(",\"lastReset\":" + jsonQuoted(resetReasonLabel(resetReason)));
  diagnosticSendContent("},\n");
  markWebResponsePhase("runtime");

  workStartMs = millis();
  diagnosticSendContent("  \"hardware\":{");
  diagnosticSendContent("\"chip\":" + jsonQuoted(String(bootStaticSystemInfo.chipModel)));
  diagnosticSendContent(",\"revision\":" + String(bootStaticSystemInfo.chipRevision));
  diagnosticSendContent(",\"cores\":" + String(bootStaticSystemInfo.chipCores));
  diagnosticSendContent(",\"cpuMHz\":" + String(bootStaticSystemInfo.cpuFreqMHz));
  diagnosticSendContent(",\"flashBytes\":" + String(bootStaticSystemInfo.flashBytes));
  diagnosticSendContent(",\"sketchBytes\":" + String(bootStaticSystemInfo.sketchBytes));
  diagnosticSendContent(",\"appPartitionBytes\":" + String(appPartitionBytes));
  diagnosticSendContent(",\"unusedAppBytes\":" + String(unusedAppBytes));
  diagnosticSendContent("},\n");
  recordWebWorkTiming("hardware-json-build", workStartMs);

  markWebResponsePhase("hardware");
  diagnosticSendContent("  \"memory\":{");
  diagnosticSendContent("\"heapBytes\":" + String(ESP.getHeapSize()));
  diagnosticSendContent(",\"freeHeapBytes\":" + String(ESP.getFreeHeap()));
  diagnosticSendContent(",\"minimumFreeHeapBytes\":" + String(ESP.getMinFreeHeap()));
  diagnosticSendContent(",\"largestFreeBlockBytes\":" + String(largestBlock));
  diagnosticSendContent("},\n");

  markWebResponsePhase("memory");
  diagnosticSendContent("  \"network\":{");
  diagnosticSendContent("\"stationConnected\":");
  diagnosticSendContent(WiFi.status() == WL_CONNECTED ? "true" : "false");
  diagnosticSendContent(",\"stationMac\":" + jsonQuoted(WiFi.macAddress()));
  diagnosticSendContent(",\"stationSSID\":" +
    jsonQuoted(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("")));
  diagnosticSendContent(",\"stationIP\":" +
    jsonQuoted(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("")));
  diagnosticSendContent(",\"stationRssiDbm\":" +
    String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0));
  diagnosticSendContent(",\"stationChannel\":" +
    String(WiFi.status() == WL_CONNECTED ? WiFi.channel() : 0));
  diagnosticSendContent(",\"autoReconnectPending\":");
  diagnosticSendContent(infrastructureReconnectPending ? "true" : "false");
  diagnosticSendContent(",\"autoReconnectAttemptActive\":");
  diagnosticSendContent(infrastructureReconnectAttemptActive ? "true" : "false");
  diagnosticSendContent(",\"autoReconnectAttempts\":" +
    String(infrastructureReconnectAttemptCount));
  diagnosticSendContent(",\"autoReconnectSuccesses\":" +
    String(infrastructureReconnectSuccessCount));
  diagnosticSendContent(",\"nativeAutoReconnectEnabled\":true");
  diagnosticSendContent(",\"nativeReconnectTransitions\":" + String(nativeReconnectObservedCount));
  diagnosticSendContent(",\"accessPointRunning\":");
  diagnosticSendContent(apRunning ? "true" : "false");
  diagnosticSendContent(",\"accessPointSSID\":" + jsonQuoted(apSSID));
  diagnosticSendContent(",\"accessPointIP\":" +
    jsonQuoted(apRunning ? WiFi.softAPIP().toString() : String("")));
  diagnosticSendContent(",\"accessPointClients\":" +
    String(apRunning ? WiFi.softAPgetStationNum() : 0));
  diagnosticSendContent(",\"mdnsHostname\":" + jsonQuoted(mdnsHostname));
  diagnosticSendContent(",\"mdnsStarted\":");
  diagnosticSendContent(mdnsStarted ? "true" : "false");
  diagnosticSendContent("},\n");

  markWebResponsePhase("network");
  diagnosticSendContent("  \"wifiSurvey\":{");
  diagnosticSendContent("\"scanIntervalSeconds\":" + String(scanIntervalSeconds));
  diagnosticSendContent(",\"scanCounter\":" + String(scanCounter));
  diagnosticSendContent(",\"scanInProgress\":");
  diagnosticSendContent(wifiScanInProgress ? "true" : "false");
  diagnosticSendContent(",\"scanStatus\":" + jsonQuoted(wifiScanStatusMessage));
  diagnosticSendContent(",\"observationsRetained\":" + String(historyCount));
  diagnosticSendContent(",\"observationCapacity\":" + String(scanHistoryCapacity));
  diagnosticSendContent(",\"scanGroupsRetained\":" + String(countRetainedScanGroups()));
  diagnosticSendContent(",\"historyAllocatedBytes\":" + String(wifiHistoryAllocatedBytes()));
  diagnosticSendContent(",\"observationRecordBytes\":" + String(sizeof(WifiObservation)));
  diagnosticSendContent(",\"scanMetadataSlots\":" + String(wifiScanMetadataCapacity));
  if (historyCount > 0) {
    const ScanRecord& oldest = historyRecord(0);
    const ScanRecord& newest = historyRecord(historyCount - 1);
    diagnosticSendContent(",\"oldestRecordUptimeMs\":" + String(oldest.uptimeMs));
    diagnosticSendContent(",\"newestRecordUptimeMs\":" + String(newest.uptimeMs));
    diagnosticSendContent(",\"retainedTimeWindowMs\":" +
      String((uint32_t)(newest.uptimeMs - oldest.uptimeMs)));
  }
  diagnosticSendContent(",\"apTableUsed\":" + String(wifiApCount));
  diagnosticSendContent(",\"apTableCapacity\":" + String(wifiApTableCapacity));
  diagnosticSendContent(",\"apTableDrops\":" + String(wifiApTableFullDrops));
  diagnosticSendContent(",\"historyIntegrityAnomalies\":" + String(wifiHistoryIntegrityAnomalies()));
  diagnosticSendContent(",\"autoDiagnostic\":" + jsonQuoted(wifiAutoScanDiagnosticLabel()));
  diagnosticSendContent(",\"automaticStarts\":" + String(wifiAutoScanStartCount));
  diagnosticSendContent(",\"automaticCompletions\":" + String(wifiAutoScanCompletionCount));
  diagnosticSendContent(",\"automaticStartFailures\":" + String(wifiAutoScanStartFailureCount));
  diagnosticSendContent(",\"automaticCompletionFailures\":" + String(wifiAutoScanCompletionFailureCount));
  diagnosticSendContent(",\"automaticRetryPending\":");
  diagnosticSendContent(wifiAutoScanRetryPending ? "true" : "false");
  diagnosticSendContent(",\"interactionDeferred\":");
  diagnosticSendContent(userInteractionDeferActive() ? "true" : "false");
  diagnosticSendContent(",\"csvExportInProgress\":");
  diagnosticSendContent(csvExportInProgress ? "true" : "false");
  diagnosticSendContent(",\"scanDurationLastMs\":" + String(wifiLastScanDurationMs));
  diagnosticSendContent(",\"scanDurationAverageMs\":" + String(wifiAverageScanDurationMs()));
  diagnosticSendContent(",\"scanDurationMinimumMs\":" + String(wifiMinScanDurationMs));
  diagnosticSendContent(",\"scanDurationMaximumMs\":" + String(wifiMaxScanDurationMs));
  diagnosticSendContent(",\"csvExports\":" + String(wifiCsvExportCount));
  diagnosticSendContent(",\"lastCsvRows\":" + String(wifiCsvLastRows));
  diagnosticSendContent(",\"lastCsvBytes\":" + String(wifiCsvLastBytes));
  diagnosticSendContent(",\"lastCsvDurationMs\":" + String(wifiCsvLastDurationMs));
  diagnosticSendContent(",\"syntheticPrefillRuns\":" + String(syntheticPrefillRuns));
  diagnosticSendContent(",\"sessionRestoredThisBoot\":");
  diagnosticSendContent(sessionRestoredThisBoot ? "true" : "false");
  diagnosticSendContent("},\n");

  markWebResponsePhase("wifi-survey");
  diagnosticSendContent("  \"bluetoothSurvey\":{");
  diagnosticSendContent("\"enabled\":");
  diagnosticSendContent(bleSurveyEnabled ? "true" : "false");
  diagnosticSendContent(",\"initialized\":");
  diagnosticSendContent(bleInitialized ? "true" : "false");
  diagnosticSendContent(",\"automaticScanningEnabled\":");
  diagnosticSendContent(autoBleScanEnabled ? "true" : "false");
  diagnosticSendContent(",\"scanIntervalSeconds\":" + String(bleScanIntervalSeconds));
  diagnosticSendContent(",\"scanCounter\":" + String(bleScanCounter));
  diagnosticSendContent(",\"scanStatus\":" + jsonQuoted(bleStatusMessage));
  diagnosticSendContent(",\"lastScanUptimeMs\":" + String(lastBleScanUptimeMs));
  diagnosticSendContent(",\"observationsRetained\":" + String(bleHistoryCount));
  diagnosticSendContent(",\"observationCapacity\":" + String(bleHistoryCapacity));
  diagnosticSendContent(",\"historyAllocatedBytes\":" + String(bleHistoryAllocatedBytes()));
  diagnosticSendContent(",\"observationRecordBytes\":" + String(sizeof(BleObservation)));
  diagnosticSendContent(",\"scanMetadataSlots\":" + String(bleScanMetadataCapacity));
  diagnosticSendContent(",\"addressTableUsed\":" + String(bleAddressCount));
  diagnosticSendContent(",\"addressTableCapacity\":" + String(bleAddressTableCapacity));
  diagnosticSendContent(",\"addressTableDrops\":" + String(bleAddressTableFullDrops));
  diagnosticSendContent(",\"csvExports\":" + String(bleCsvExportCount));
  diagnosticSendContent(",\"lastCsvRows\":" + String(bleCsvLastRows));
  diagnosticSendContent(",\"lastCsvBytes\":" + String(bleCsvLastBytes));
  diagnosticSendContent(",\"lastCsvDurationMs\":" + String(bleCsvLastDurationMs));
  diagnosticSendContent("},\n");

  markWebResponsePhase("bluetooth-survey");
  diagnosticSendContent("  \"bootHeapCheckpoints\":[");
  for (size_t i = 0; i < bootHeapCheckpointCount; i++) {
    const BootHeapCheckpoint& cp = bootHeapCheckpoints[i];
    if (i) diagnosticSendContent(",");
    diagnosticSendContent("{\"stage\":" + jsonQuoted(String(cp.stage)));
    diagnosticSendContent(",\"freeHeapBytes\":" + String(cp.freeHeap));
    diagnosticSendContent(",\"minimumFreeHeapBytes\":" + String(cp.minimumFreeHeap));
    diagnosticSendContent(",\"largestFreeBlockBytes\":" + String(cp.largestFreeBlock));
    diagnosticSendContent("}");
  }
  diagnosticSendContent("],\n");

  markWebResponsePhase("boot-checkpoints");
  diagnosticSendContent("  \"channelAnalysis\":{");
  diagnosticSendContent("\"valid\":");
  diagnosticSendContent(channel.valid ? "true" : "false");
  diagnosticSendContent(",\"sourceScan\":" + String(channel.scanNumber));
  diagnosticSendContent(",\"suggestedChannel\":" + String(channel.suggestedChannel));
  diagnosticSendContent(",\"channel1Score\":" + String(channel.totalScore[1], 2));
  diagnosticSendContent(",\"channel6Score\":" + String(channel.totalScore[6], 2));
  diagnosticSendContent(",\"channel11Score\":" + String(channel.totalScore[11], 2));
  diagnosticSendContent("},\n");

  markWebResponsePhase("channel-analysis");

  size_t eventsToInclude = diagnosticExportEventLimit < diagnosticEventCount ? diagnosticExportEventLimit : diagnosticEventCount;
  size_t firstEvent = diagnosticEventCount - eventsToInclude;
  diagnosticSendContent("  \"recentDiagnosticEvents\":{");
  diagnosticSendContent("\"available\":" + String(diagnosticEventCount));
  diagnosticSendContent(",\"included\":" + String(eventsToInclude));
  diagnosticSendContent(",\"capacity\":" + String(DIAGNOSTIC_EVENT_CAPACITY));
  diagnosticSendContent(",\"events\":[");
  for (size_t i = firstEvent; i < diagnosticEventCount; i++) {
    if (i > firstEvent) diagnosticSendContent(",");
    const DiagnosticEventRecord& event = diagnosticEventAt(i);
    diagnosticSendContent("{\"uptimeMs\":" + String(event.uptimeMs));
    diagnosticSendContent(",\"category\":" + jsonQuoted(String(event.category)));
    diagnosticSendContent(",\"detail\":" + jsonQuoted(String(event.detail)) + "}");
  }
  diagnosticSendContent("]},\n");
  markWebResponsePhase("diagnostic-events");

  diagnosticSendContent("  \"configuration\":");
  String compactConfig = portableConfigJson(persisted);
  compactConfig.trim();
  diagnosticSendContent(compactConfig);
  diagnosticSendContent("\n}\n");
  diagnosticSendContent("");
  markWebResponsePhase("configuration");
  endWebResponseProfile();
}

// Purpose: Builds the Settings page with ordinary configuration in Standard and maintenance detail in deeper views.
void handleSettingsPage() {
  beginWebResponseProfile("/settings");
  markExplicitUserInteraction();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  diagnosticSendContent("<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>ESP32 Settings</title>");
  sendThemeBootstrapScript(); diagnosticSendContent(pageStyles());
  diagnosticSendContent("</head><body><div class=\"container\">"); sendSiteNavigation("settings"); diagnosticSendContent("<h1>Settings</h1>");
  markWebResponsePhase("header");

  String s; s.reserve(1200);
  s += "<div class=\"card\"><h2>Infrastructure Wi-Fi</h2><div class=\"row\"><span class=\"label\">Status</span><span class=\"value\">" + String(WiFi.status()==WL_CONNECTED ? "Connected" : "Disconnected") + "</span></div>";
  if (WiFi.status()==WL_CONNECTED) {
    s += "<div class=\"row\"><span class=\"label\">SSID</span><span class=\"value\">" + htmlEscape(WiFi.SSID()) + "</span></div>";
    s += "<div class=\"row advanced-only\"><span class=\"label\">IP Address</span><span class=\"value\">" + WiFi.localIP().toString() + "</span></div>";
  }
  s += "<form class=\"controls\" action=\"/wifi-save\" method=\"post\"><div class=\"control\"><label for=\"sta-ssid\">SSID</label><input id=\"sta-ssid\" name=\"ssid\" type=\"text\" maxlength=\"32\" required></div><div class=\"control\"><label for=\"sta-password\">Password</label><input id=\"sta-password\" name=\"password\" type=\"password\" maxlength=\"63\"></div><button type=\"submit\">Connect &amp; Save</button></form><form class=\"controls\" action=\"/wifi-clear\" method=\"post\"><button class=\"danger\" type=\"submit\">Clear Saved Wi-Fi</button></form>"
    "<div class=\"note\">Infrastructure Wi-Fi is optional and does not affect automatic surveying. New credentials are saved only after a successful connection; stored passwords are never displayed.</div></div>";
  diagnosticSendContent(s); s.remove(0);
  markWebResponsePhase("infrastructure-wifi");

  s += "<div class=\"card\"><h2>Device Hostname</h2><div class=\"row\"><span class=\"label\">Friendly Web Address</span><span class=\"value\">" + htmlEscape(mdnsWebAddress()) + "</span></div>"
    "<div class=\"row advanced-only\"><span class=\"label\">mDNS Status</span><span class=\"value\">" + htmlEscape(mdnsStatusMessage) + "</span></div>"
    "<form class=\"controls\" action=\"/hostname-save\" method=\"post\"><div class=\"control\"><label for=\"mdns-hostname\">Hostname</label><input id=\"mdns-hostname\" name=\"hostname\" type=\"text\" maxlength=\"32\" value=\"" + htmlEscape(mdnsHostname) + "\" required></div><button type=\"submit\">Save Hostname &amp; Restart</button></form>"
    "<div class=\"note\">Use letters, numbers, and hyphens only; the name cannot begin or end with a hyphen.</div><div class=\"note advanced-only\">The friendly address is &lt;hostname&gt;.local. mDNS support can vary by client, so IP addresses remain the fallback.</div></div>";
  diagnosticSendContent(s); s.remove(0);
  markWebResponsePhase("hostname");

  s += "<div class=\"card\"><h2>Device AP</h2><div class=\"row\"><span class=\"label\">Status</span><span class=\"value\">" + String(apRunning ? "Running" : "Off") + "</span></div>";
  if (apRunning) {
    s += "<div class=\"row\"><span class=\"label\">Broadcast SSID</span><span class=\"value\">" + htmlEscape(apSSID) + "</span></div>";
  }
  s += "<div class=\"advanced-only\"><div class=\"row\"><span class=\"label\">AP IP</span><span class=\"value\">" + (apRunning ? WiFi.softAPIP().toString() : String("-")) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Connected Clients</span><span class=\"value\">" + String(apRunning ? WiFi.softAPgetStationNum() : 0) + "</span></div>"
    "<form class=\"controls\" action=\"/ap-save\" method=\"post\"><div class=\"control\"><label><input type=\"checkbox\" name=\"enabled\" value=\"1\" " + String(apEnabled ? "checked" : "") + "> Enable Device AP</label></div>"
    "<div class=\"control\"><label for=\"apssid\">Broadcast SSID</label><input id=\"apssid\" name=\"ssid\" type=\"text\" maxlength=\"32\" value=\"" + htmlEscape(apSSID) + "\"></div>"
    "<div class=\"control\"><label for=\"appassword\">New AP password</label><input id=\"appassword\" name=\"password\" type=\"password\" minlength=\"8\" maxlength=\"63\" placeholder=\"Leave blank to keep current\"></div>"
    "<button type=\"submit\">Save Device AP Settings &amp; Restart</button></form>"
    "<div class=\"note\">The Device AP password must be 8 to 63 characters. Leaving it blank keeps the current password. Changes are stored and applied after restart. Disabling the Device AP can make the web interface unreachable unless infrastructure Wi-Fi is available.</div></div></div>";
  diagnosticSendContent(s); s.remove(0);
  markWebResponsePhase("device-ap");

  s += "<div class=\"card\"><h2>Survey Mode</h2><div class=\"row\"><span class=\"label\">Bluetooth Survey</span><span class=\"value\">" + String(bleSurveyEnabled ? "Enabled" : "Disabled") + "</span></div><form class=\"controls\" action=\"/ble-mode\" method=\"post\"><input type=\"hidden\" name=\"enabled\" value=\"" + String(bleSurveyEnabled ? "0" : "1") + "\"><button type=\"submit\">" + String(bleSurveyEnabled ? "Disable Bluetooth Survey" : "Enable Bluetooth Survey") + "</button></form>"
    "<div class=\"note\">Enabling Bluetooth allows simultaneous Wi-Fi and Bluetooth surveying, but significantly reduces Wi-Fi history capacity. Changing this mode requires a restart.</div>"
    "<div class=\"note developer-only\">The selection is stored in NVS. BLE creates a persistent heap allocation at boot, so survey histories are sized after the selected radio mode is initialized.</div></div>";
  diagnosticSendContent(s); s.remove(0);
  markWebResponsePhase("survey-mode");

  s += "<div class=\"card advanced-only\"><h2>Wi-Fi Capture</h2><form class=\"controls\" action=\"/wifi-capture-settings\" method=\"post\">"
    "<div class=\"control\"><label><input type=\"checkbox\" name=\"captureHidden\" value=\"1\" " + String(captureHiddenNetworks ? "checked" : "") + "> Capture Hidden Networks</label></div>"
    "<button type=\"submit\">Apply Wi-Fi Capture Settings</button></form>"
    "<div class=\"note\">When disabled, networks without an advertised SSID still contribute to current RF/channel analysis but do not consume retained-history or AP-table capacity. Existing hidden observations age out normally.</div></div>";
  diagnosticSendContent(s); s.remove(0);
  markWebResponsePhase("wifi-capture");

  s += "<div class=\"card\"><h2>Interface &amp; Indicators</h2><div class=\"control\"><label><input id=\"status-led-enabled\" type=\"checkbox\" " + String(statusLedEnabled ? "checked" : "") + " onchange=\"setStatusLed(this)\"> Enable status LED indicators</label><span id=\"status-led-save-state\" class=\"save-state\"></span></div>"
    "<div class=\"row\"><span class=\"label\">Status LED</span><span id=\"status-led-state\" class=\"value\">" + String(STATUS_LED_AVAILABLE ? (statusLedEnabled ? "Enabled" : "Disabled") : "Not available") + "</span></div>"
    "<form class=\"controls advanced-only\" action=\"/led-test\" method=\"post\"><button type=\"submit\">Test Status LED</button></form>"
    "<div class=\"row developer-only\"><span class=\"label\">Status LED GPIO</span><span class=\"value\">" + String(STATUS_LED_PIN) + "</span></div>"
    "<div class=\"note advanced-only\">Theme and Standard/Advanced/Developer view selections are stored in this browser, not on the ESP32.</div></div>";
  diagnosticSendContent(s); s.remove(0);
  markWebResponsePhase("interface-indicators");

  diagnosticSendContent("<div class=\"card advanced-only\"><h2>Configuration Backup &amp; Restore</h2><div class=\"buttons\"><a class=\"button\" href=\"/config.json\">Download Configuration</a></div><div class=\"note\">Configuration export contains supported non-secret settings; infrastructure and Device AP passwords are excluded.</div><div class=\"control\"><label for=\"config-import-file\">Restore configuration</label><input id=\"config-import-file\" type=\"file\" accept=\"application/json,.json\"></div><div class=\"buttons\"><button id=\"config-import-button\" type=\"button\" onclick=\"importConfiguration()\">Validate &amp; Apply Configuration</button></div><div id=\"config-import-result\" class=\"note\">Import validates the complete supported schema before writing settings. Restart-required changes are saved but are not silently restarted.</div></div>");
  diagnosticSendContent("<script>async function setStatusLed(box){const state=document.getElementById('status-led-state');const save=document.getElementById('status-led-save-state');const enabled=!!box.checked;if(save)save.textContent='Saving…';try{const r=await fetch('/interface-settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ledEnabled='+(enabled?'1':'0'),cache:'no-store'});if(!r.ok)throw new Error();const j=await r.json();box.checked=!!j.enabled;if(state)state.textContent=j.enabled?'Enabled':'Disabled';if(save){save.textContent='Saved';setTimeout(()=>{save.textContent='';},1400);}}catch(e){box.checked=!enabled;if(state)state.textContent=box.checked?'Enabled':'Disabled';if(save)save.textContent='Save failed';}} async function importConfiguration(){const f=document.getElementById('config-import-file');const o=document.getElementById('config-import-result');if(!f||!o)return;if(!f.files||!f.files.length){o.textContent='Choose a configuration JSON file first.';return;}const file=f.files[0];if(file.size>4096){o.textContent='Configuration file is larger than the 4096-byte import limit.';return;}o.textContent='Validating configuration...';try{const body=await file.text();const r=await fetch('/config/import',{method:'POST',headers:{'Content-Type':'application/json'},body:body,cache:'no-store'});const j=await r.json();if(!j.ok){o.textContent='Import rejected: '+j.message;return;}let msg=j.message+' '+j.applied+' setting group(s) changed.';if(j.restartRequired){msg+=' Restart required for: '+j.restartReason+'.';}else{msg+=' No restart required.';}o.textContent=msg;}catch(e){o.textContent='Configuration import failed: '+e;}}</script>");
  markWebResponsePhase("configuration-backup");
  diagnosticSendContent("<div class=\"footer\">ESP32 Web Interface</div>"); sendThemeScript(); diagnosticSendContent("</div></body></html>"); diagnosticSendContent("");
  markWebResponsePhase("footer-scripts");
  endWebResponseProfile();
}

// Purpose: Validates and saves a new device hostname, checkpoints the session, and performs a controlled restart.
void handleHostnameSave() {
  markExplicitUserInteraction();
  if (!server.hasArg("hostname")) {
    server.send(400, "text/plain", "Hostname is required.");
    return;
  }

  String requested = normalizedMdnsHostname(server.arg("hostname"));
  if (!isValidMdnsHostname(requested)) {
    server.send(400, "text/plain", "Invalid hostname. Use 1-32 letters, numbers, or hyphens; do not begin or end with a hyphen.");
    return;
  }

  if (requested == mdnsHostname) {
    server.sendHeader("Location", "/settings");
    server.send(303, "text/plain", "Hostname unchanged.");
    return;
  }

  saveMdnsHostname(requested);
  server.send(200, "text/html",
    String("<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">") +
    themeBootstrapScript() + pageStyles() +
    "</head><body><div class=\"container\"><div class=\"card\"><h1>Hostname Updated</h1><p>The ESP32 will advertise as <strong>" +
    htmlEscape(mdnsHostname) + ".local</strong> after restart.</p><p>The ESP32 is restarting now.</p></div></div></body></html>");
  checkpointBeforeControlledRestart();
  delay(750);
  ESP.restart();
}

// Purpose: Saves ESP32-resident interface settings such as the status LED preference.
void handleInterfaceSettings() {
  markExplicitUserInteraction();
  if (!server.hasArg("ledEnabled")) {
    server.send(400, "application/json", "{\"saved\":false,\"message\":\"Missing status LED value.\"}");
    return;
  }
  bool requestedLed = server.arg("ledEnabled") == "1";
  saveInterfaceSettings(requestedLed, webAutoRefreshEnabled);
  if (!statusLedEnabled) stopScanLed();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", String("{\"saved\":true,\"enabled\":") + (statusLedEnabled ? "true" : "false") + "}");
}

void handleWifiCaptureSettings() {
  markExplicitUserInteraction();
  saveCaptureHiddenNetworks(server.hasArg("captureHidden"));
  server.sendHeader("Location", "/settings");
  server.send(303, "text/plain", "Wi-Fi capture settings saved.");
}

// Purpose: Runs the status LED self-test requested from the Settings page.
void handleLedSelfTest() {
  markExplicitUserInteraction();
  runStatusLedSelfTest();
  server.sendHeader("Location", "/settings");
  server.send(303, "text/plain", "Status LED self-test complete.");
}

// Purpose: Validates infrastructure Wi-Fi credentials, connects, and saves them only after connection succeeds.
void handleSaveStationSettings() {
  markExplicitUserInteraction();
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain", "SSID is required.");
    return;
  }
  String ssid = server.arg("ssid");
  String password = server.hasArg("password") ? server.arg("password") : "";
  ssid.trim();
  if (ssid.length() == 0 || ssid.length() > 32 || password.length() > 63) {
    server.send(400, "text/plain", "Invalid SSID or password length.");
    return;
  }
  bool connected = connectToWiFi(ssid, password);
  if (connected) saveCredentials(ssid, password);
  server.sendHeader("Location", "/settings");
  server.send(303, "text/plain", connected ? "Connected and saved." : "Connection failed; previous saved credentials were retained.");
}

// Purpose: Persists the requested Bluetooth survey mode, attempts a checkpoint, and performs the required controlled restart.
void handleBleModeChange() {
  markExplicitUserInteraction();
  if (!server.hasArg("enabled")) {
    server.send(400, "text/plain", "Missing Bluetooth mode value.");
    return;
  }

  bool requested = server.arg("enabled") == "1";
  if (requested == bleSurveyEnabled) {
    server.sendHeader("Location", requested ? "/ble" : "/settings");
    server.send(303, "text/plain", "Bluetooth survey mode unchanged.");
    return;
  }

  saveBleSurveyEnabled(requested);

  server.send(200, "text/html",
    String("<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">") +
    themeBootstrapScript() + pageStyles() +
    "</head><body><div class=\"container\"><div class=\"card\"><h1>Bluetooth Survey Mode Updated</h1><p>Bluetooth Survey will be " +
    String(requested ? "enabled" : "disabled") +
    " after restart.</p><p>The ESP32 is restarting now. This page will reconnect automatically.</p>"
    "<p id=\"reconnect-status\">Waiting for the surveyor...</p><p><a class=\"button\" href=\"/ble\">Return to Bluetooth Survey</a></p></div></div>"
    "<script>(function(){setTimeout(function retry(){fetch('/ble',{cache:'no-store'}).then(function(r){"
    "if(r.ok){location.replace('/ble');return;}setTimeout(retry,1000);"
    "}).catch(function(){setTimeout(retry,1000);});},2500);})();</script></body></html>");

  checkpointBeforeControlledRestart();
  delay(750);
  ESP.restart();
}

// Purpose: Deletes saved infrastructure Wi-Fi credentials and returns the radio to a usable station/AP mode.
void handleClearStationSettings() {
  markExplicitUserInteraction();
  eraseCredentials();
  WiFi.disconnect(false);
  ensureWiFiStationMode();
  server.sendHeader("Location", "/settings");
  server.send(303, "text/plain", "Saved infrastructure Wi-Fi credentials cleared.");
}

// ============================================================
// Access point web configuration
// ============================================================

// Purpose: Redirects legacy Device AP configuration links to the integrated Settings page.
void handleAccessPointSettingsPage() {
  markExplicitUserInteraction();
  server.sendHeader("Location", "/settings");
  server.send(303, "text/plain", "Device AP settings are available in Advanced view on Settings.");
}

// Purpose: Validates and saves Device AP settings, then applies the restart/reconfiguration behavior required by those changes.
void handleSaveAccessPointSettings() {
  markExplicitUserInteraction();
  bool requestedEnabled = server.hasArg("enabled");

  String requestedSSID =
      server.hasArg("ssid")
        ? server.arg("ssid")
        : apSSID;

  requestedSSID.trim();

  if (requestedSSID.length() == 0) {
    requestedSSID = defaultApSSID();
  }

  if (requestedSSID.length() > 32) {
    requestedSSID = requestedSSID.substring(0, 32);
  }

  String requestedPassword = apPassword;

  if (
    server.hasArg("password") &&
    server.arg("password").length() > 0
  ) {
    requestedPassword = server.arg("password");

    if (
      requestedPassword.length() < 8 ||
      requestedPassword.length() > 63
    ) {
      server.send(
        400,
        "text/plain",
        "AP password must be 8 to 63 characters."
      );
      return;
    }
  }

  saveAccessPointSettings(
    requestedEnabled,
    requestedSSID,
    requestedPassword
  );

  server.send(
    200,
    "text/html",
    "<!DOCTYPE html><html><head>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>AP Settings Saved</title></head><body>"
    "<h2>Access point settings saved.</h2>"
    "<p>The ESP32 is restarting now.</p>"
    "<p>If the AP SSID or password changed, reconnect using the new settings.</p>"
    "</body></html>"
  );

  checkpointBeforeControlledRestart();
  delay(750);
  ESP.restart();
}


// ============================================================
// Web server
// ============================================================

// Purpose: Registers all HTTP routes and starts the ESP32 web server.
void startWebServer() {
  if (webServerStarted) {
    return;
  }

  server.on("/", []() { runDiagnosticWebHandler("/", handleWebScan); });
  server.on("/scan", []() { runDiagnosticWebHandler("/scan", handleWebScan); });
  server.on("/system", HTTP_GET, []() { runDiagnosticWebHandler("/system", handleSystemStatus); });
  server.on("/settings", HTTP_GET, []() { runDiagnosticWebHandler("/settings", handleSettingsPage); });
  server.on("/status.json", HTTP_GET, []() { runDiagnosticWebHandler("/status.json", handleStatusJsonExport); });
  server.on("/api/diag/event-limit", HTTP_POST, []() { runDiagnosticWebHandler("/api/diag/event-limit", handleDiagnosticEventLimit); });
  server.on("/config.json", HTTP_GET, []() { runDiagnosticWebHandler("/config.json", handleConfigExport); });
  server.on("/config/import", HTTP_POST, []() { runDiagnosticWebHandler("/config/import", handleConfigImport); });
  server.on("/wifi-save", HTTP_POST, []() { runDiagnosticWebHandler("/wifi-save", handleSaveStationSettings); });
  server.on("/wifi-clear", HTTP_POST, []() { runDiagnosticWebHandler("/wifi-clear", handleClearStationSettings); });
  server.on("/hostname-save", HTTP_POST, []() { runDiagnosticWebHandler("/hostname-save", handleHostnameSave); });
  server.on("/interface-settings", HTTP_POST, []() { runDiagnosticWebHandler("/interface-settings", handleInterfaceSettings); });
  server.on("/wifi-capture-settings", HTTP_POST, []() { runDiagnosticWebHandler("/wifi-capture-settings", handleWifiCaptureSettings); });
  server.on("/led-test", HTTP_POST, []() { runDiagnosticWebHandler("/led-test", handleLedSelfTest); });
  server.on("/scan-now", []() { runDiagnosticWebHandler("/scan-now", handleWebScanNow); });
  server.on("/api/wifi/status", HTTP_GET, []() { runDiagnosticWebHandler("/api/wifi/status", handleWifiScanStatus); });
  server.on("/api/wifi/observed", HTTP_GET, []() { runDiagnosticWebHandler("/api/wifi/observed", handleWifiObservedFragment); });
  server.on("/api/wifi/plot", HTTP_GET, []() { runDiagnosticWebHandler("/api/wifi/plot", handleWifiPlotFragment); });
  server.on("/api/wifi/channel", HTTP_GET, []() { runDiagnosticWebHandler("/api/wifi/channel", handleWifiChannelFragment); });
  server.on("/api/live-updates", HTTP_POST, []() { runDiagnosticWebHandler("/api/live-updates", handleLiveUpdatesSetting); });
  server.on("/api/wifi/interval", HTTP_POST, []() { runDiagnosticWebHandler("/api/wifi/interval", handleWifiIntervalSetting); });
  server.on("/scan-settings", []() { runDiagnosticWebHandler("/scan-settings", handleScanSettings); });
  server.on("/scan-clear", []() { runDiagnosticWebHandler("/scan-clear", handleClearScanHistory); });
  server.on("/scanlog.csv", []() { runDiagnosticWebHandler("/scanlog.csv", handleScanCsv); });
  server.on("/history-prefill", HTTP_POST, []() { runDiagnosticWebHandler("/history-prefill", handleHistoryPrefill); });
  server.on("/session-save", HTTP_POST, []() { runDiagnosticWebHandler("/session-save", handleSessionCheckpointSave); });
  server.on("/session-discard", HTTP_POST, []() { runDiagnosticWebHandler("/session-discard", handleSessionCheckpointDiscard); });
  server.on("/ble", HTTP_GET, []() { runDiagnosticWebHandler("/ble", handleBLESurvey); });
  server.on("/api/ble/status", HTTP_GET, []() { runDiagnosticWebHandler("/api/ble/status", handleBleScanStatus); });
  server.on("/api/ble/observed", HTTP_GET, []() { runDiagnosticWebHandler("/api/ble/observed", handleBleObservedFragment); });
  server.on("/api/ble/plot", HTTP_GET, []() { runDiagnosticWebHandler("/api/ble/plot", handleBlePlotFragment); });
  server.on("/api/ble/interval", HTTP_POST, []() { runDiagnosticWebHandler("/api/ble/interval", handleBleIntervalSetting); });
  server.on("/ble-mode", HTTP_POST, []() { runDiagnosticWebHandler("/ble-mode", handleBleModeChange); });
  server.on("/ble-scan", HTTP_GET, []() { runDiagnosticWebHandler("/ble-scan", handleBLEScanNow); });
  server.on("/ble-settings", HTTP_GET, []() { runDiagnosticWebHandler("/ble-settings", handleBLESettings); });
  server.on("/ble-clear", HTTP_GET, []() { runDiagnosticWebHandler("/ble-clear", handleClearBLEHistory); });
  server.on("/blelog.csv", HTTP_GET, []() { runDiagnosticWebHandler("/blelog.csv", handleBLEScanCsv); });
  server.on("/ap", HTTP_GET, []() { runDiagnosticWebHandler("/ap", handleAccessPointSettingsPage); });
  server.on("/ap-save", HTTP_POST, []() { runDiagnosticWebHandler("/ap-save", handleSaveAccessPointSettings); });

  server.onNotFound([]() {
    server.send(
      404,
      "text/plain",
      "404 - Page not found"
    );
  });

  server.begin();
  webServerStarted = true;

  Serial.println();
  Serial.println("Web server started.");

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("LAN web UI: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/");
  }

  if (apRunning) {
    Serial.print("AP web UI:  http://");
    Serial.print(WiFi.softAPIP());
    Serial.println("/");
  }
}


// ============================================================
// Serial interface - text-oriented parity with the web UI
// ============================================================

// Purpose: Prints the top-level serial command menu.
void printSerialMainMenu() {
  Serial.println();
  Serial.println("============================================================");
  Serial.print(" ESP32 Wireless Surveyor V");
  Serial.println(FIRMWARE_VERSION);
  Serial.println("============================================================");
  Serial.println("1 - Wi-Fi Survey");
  Serial.println("2 - Bluetooth Survey");
  Serial.println("3 - System");
  Serial.println("4 - Settings");
  Serial.println("5 - Developer");
  Serial.println();
  Serial.println("h/help - Show this menu");
  Serial.println("restart - Restart ESP32");
  Serial.println();
  Serial.print("> ");
}

// Purpose: Prints Wi-Fi survey status and retained-network summaries to Serial.
void printWifiSurveySerial() {
  Serial.println();
  Serial.println("============================================================");
  Serial.println(" Wi-Fi Survey");
  Serial.println("============================================================");
  Serial.println("Automatic scanning:  Always enabled");
  Serial.print("Scan interval:       ");
  Serial.print(scanIntervalSeconds);
  Serial.println(" s");
  Serial.print("Auto-scan diagnostic: ");
  Serial.println(wifiAutoScanDiagnosticLabel());
  Serial.print("Auto starts/completes: ");
  Serial.print(wifiAutoScanStartCount);
  Serial.print(" / ");
  Serial.println(wifiAutoScanCompletionCount);
  Serial.print("Auto start failures:  ");
  Serial.println(wifiAutoScanStartFailureCount);
  Serial.print("Auto completion fails:");
  Serial.print(" ");
  Serial.println(wifiAutoScanCompletionFailureCount);
  Serial.print("Auto retry pending:    ");
  Serial.println(wifiAutoScanRetryPending ? "Yes" : "No");
  Serial.print("Wi-Fi scan duration:   ");
  Serial.println(wifiScanDurationSummaryLabel());
  Serial.print("CSV exports served:    ");
  Serial.println(wifiCsvExportCount);
  Serial.print("Last CSV export:       ");
  Serial.println(wifiCsvExportCount == 0 ? String("Never") : csvExportSummaryLabel(wifiCsvLastRows, wifiCsvLastBytes, wifiCsvLastDurationMs));
  Serial.print("Last auto start:       ");
  Serial.println(wifiAutoScanLastStartLabel());
  Serial.print("Last auto completion:  ");
  Serial.println(wifiAutoScanLastCompletionLabel());
  Serial.print("Observations:        ");
  Serial.print(historyCount);
  Serial.print(" / ");
  Serial.print(scanHistoryRetentionLimit);
  Serial.print(" retained; ");
  Serial.print(scanHistoryCapacity);
  Serial.println(" history capacity");
  Serial.print("Retained scans:      ");
  Serial.println(countRetainedScanGroups());
  Serial.print("Unique AP slots used:");
  Serial.print(" ");
  Serial.print(wifiApCount);
  Serial.print(" / ");
  Serial.println(wifiApTableCapacity);
  Serial.println();

  if (historyCount == 0) {
    Serial.println("No Wi-Fi observations retained yet.");
  } else {
    Serial.println("SSID                 BSSID              CH  Latest  Min  Max  Avg    N   First Seen    Last Seen");
    Serial.println("-------------------  -----------------  --  ------  ---  ---  -----  ---  ------------  ------------");
    for (size_t apIndex = 0; apIndex < wifiApCount; apIndex++) {
      if (!wifiApIndexIsReferenced(apIndex)) continue;
      char bssidText[18];
      formatBssid(wifiApTable[apIndex].bssid, bssidText);
      NetworkSummary summary;
      if (!buildNetworkSummary(String(bssidText), summary)) continue;

      String ssid = summary.hidden ? "(hidden)" : String(summary.ssid);
      if (summary.connected) ssid += "*";
      if (ssid.length() > 19) ssid = ssid.substring(0, 19);
      printPadded(ssid, 21);
      printPadded(String(summary.bssid), 19);
      printPadded(String(summary.channel), 4);
      printPadded(String(summary.signal.latestRssi), 8);
      printPadded(String(summary.signal.minRssi), 5);
      printPadded(String(summary.signal.maxRssi), 5);
      String avg = summary.signal.samples ? String((float)summary.signal.rssiTotal / summary.signal.samples, 1) : "-";
      printPadded(avg, 7);
      printPadded(String(summary.signal.samples), 5);
      printPadded(observationAgeLabel(summary.signal.firstSeenMs), 14);
      Serial.println(observationAgeLabel(summary.signal.lastSeenMs));
    }
    Serial.println("* = currently connected infrastructure AP");
  }

  ChannelAnalysis a = analyzeLatestWifiScan();
  if (a.valid) {
    Serial.println();
    Serial.print("Suggested 2.4 GHz channel: ");
    Serial.println(a.suggestedChannel);
    Serial.print("Scores 1/6/11:       ");
    Serial.print(a.totalScore[1], 1); Serial.print(" / ");
    Serial.print(a.totalScore[6], 1); Serial.print(" / ");
    Serial.println(a.totalScore[11], 1);
  }

  Serial.println();
  Serial.println("Wi-Fi commands:");
  Serial.println("  scan                 - Scan now");
  Serial.println("  wclear               - Clear Wi-Fi history");
  Serial.println("  interval <seconds>   - Set automatic scan interval");
  Serial.println("  wifi-config          - Configure infrastructure Wi-Fi");
  Serial.println("  wifi-clear           - Clear saved infrastructure credentials");
  Serial.println("  0/back               - Main menu");
  Serial.println();
  Serial.print("> ");
}

// Purpose: Prints Bluetooth survey status and retained-device summaries to Serial.
void printBluetoothSurveySerial() {
  Serial.println();
  Serial.println("============================================================");
  Serial.println(" Bluetooth Survey");
  Serial.println("============================================================");
  Serial.print("Bluetooth Survey:     ");
  Serial.println(bleSurveyEnabled ? "Enabled" : "Disabled");
  if (bleSurveyEnabled) {
    Serial.print("BLE initialized:      ");
    Serial.println(bleInitialized ? "Yes" : "No");
    Serial.print("Automatic scanning:  ");
    Serial.println("Always on while Bluetooth Survey is enabled");
    Serial.print("Scan interval:       ");
    Serial.print(bleScanIntervalSeconds);
    Serial.println(" s");
    Serial.print("Last BLE start API: "); Serial.print(bleDiagnosticLastApiDurationMs); Serial.println(" ms");
    Serial.print("Last BLE processing:"); Serial.print(" "); Serial.print(bleDiagnosticLastProcessingDurationMs); Serial.println(" ms");
    Serial.print("Last BLE total:     "); Serial.print(bleDiagnosticLastTotalDurationMs); Serial.println(" ms");
    Serial.print("BLE total min/avg/max: ");
    if (bleDiagnosticDurationCount == 0) Serial.println("n/a");
    else { Serial.print(bleDiagnosticMinDurationMs); Serial.print(" / "); Serial.print((uint32_t)(bleDiagnosticTotalDurationMs/bleDiagnosticDurationCount)); Serial.print(" / "); Serial.print(bleDiagnosticMaxDurationMs); Serial.println(" ms"); }
    Serial.print("History:             ");
    Serial.print(bleHistoryCount); Serial.print(" / "); Serial.print(bleHistoryRetentionLimit);
    Serial.print(" retained; "); Serial.print(bleHistoryCapacity); Serial.println(" physical");
    Serial.print("History RAM:         "); Serial.print(bleHistoryAllocatedBytes()/1024.0,1); Serial.println(" KB total");
    Serial.print("Observation size:    "); Serial.print(sizeof(BleObservation)); Serial.println(" bytes");
    Serial.print("Address table:       "); Serial.print(countReferencedBleAddresses()); Serial.print(" / "); Serial.print(bleAddressTableCapacity); Serial.print(" referenced; peak "); Serial.println(bleAddressPeakReferenced);
    Serial.print("Scan metadata:       "); Serial.print(countReferencedBleScanSlots()); Serial.print(" / "); Serial.print(bleScanMetadataCapacity); Serial.print(" referenced; peak "); Serial.println(bleScanMetadataPeakUsed);
    Serial.print("Address-table drops: "); Serial.println(bleAddressTableFullDrops);
    Serial.println();
    Serial.println("BLE commands:");
    Serial.println("  blescan              - Scan now");
    Serial.println("  bleclear             - Clear BLE history");
    Serial.println("  bleinterval <sec>    - Set BLE automatic scan interval");
    Serial.println("  ble off              - Disable BLE Survey and restart");
  } else {
    Serial.println();
    Serial.println("BLE is not initialized; its heap remains available to Wi-Fi surveying.");
    Serial.println("  ble on               - Enable BLE Survey and restart");
  }
  Serial.println("  0/back               - Main menu");
  Serial.println();
  Serial.print("> ");
}

// Purpose: Prints system/device/network/memory diagnostics to Serial.
void printSystemSerial() {
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t minHeap = ESP.getMinFreeHeap();
  uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  size_t appBytes = bootStaticSystemInfo.appPartitionBytes;
  size_t unused = bootStaticSystemInfo.unusedAppBytes;

  Serial.println();
  Serial.println("============================================================");
  Serial.println(" System");
  Serial.println("============================================================");
  Serial.print("Firmware:             "); Serial.print(FIRMWARE_FILE); Serial.print(" (V"); Serial.print(FIRMWARE_VERSION); Serial.println(")");
  Serial.print("Built:                "); Serial.println(firmwareBuildTimestamp());
  Serial.print("Arduino ESP32 core:   "); Serial.println(ESP_ARDUINO_VERSION_STR);
  Serial.print("ESP-IDF:              "); Serial.println(esp_get_idf_version());
  Serial.print("Uptime:               "); Serial.println(getUptimeString());
  Serial.print("Last reset:           "); Serial.println(resetReasonLabel(bootStaticSystemInfo.resetReason));
  Serial.print("Chip:                 "); Serial.print(bootStaticSystemInfo.chipModel); Serial.print(" rev "); Serial.println(bootStaticSystemInfo.chipRevision);
  Serial.print("CPU / cores:          "); Serial.print(bootStaticSystemInfo.cpuFreqMHz); Serial.print(" MHz / "); Serial.println(bootStaticSystemInfo.chipCores);
  Serial.print("Flash size:           "); Serial.print(bootStaticSystemInfo.flashBytes/1024.0/1024.0, 2); Serial.println(" MB");
  Serial.print("Sketch size:          "); Serial.print(bootStaticSystemInfo.sketchBytes/1024.0, 1); Serial.println(" KB");
  Serial.print("App partition:        "); Serial.print(appBytes/1024.0, 1); Serial.println(" KB");
  Serial.print("Unused app partition: "); Serial.print(unused/1024.0, 1); Serial.println(" KB");
  Serial.print("Free heap:            "); Serial.print(freeHeap/1024.0, 1); Serial.println(" KB");
  Serial.print("Minimum free heap:    "); Serial.print(minHeap/1024.0, 1); Serial.println(" KB");
  Serial.print("Largest free block:   "); Serial.print(largest/1024.0, 1); Serial.println(" KB");
  Serial.print("Survey memory mode:   "); Serial.println(bleSurveyEnabled ? "Wi-Fi + BLE" : "Wi-Fi only");
  Serial.print("History reserve target:"); Serial.print(" "); Serial.print((bleSurveyEnabled ? DUAL_RADIO_HEAP_RESERVE_BYTES : HISTORY_HEAP_RESERVE_BYTES)/1024); Serial.println(" KB at allocation");
  if (bleSurveyEnabled) {
    Serial.print("Low-water WARN below: "); Serial.print(DUAL_RADIO_MIN_HEAP_WARN_BYTES/1024); Serial.println(" KB minimum free heap");
  }
  Serial.print("STA MAC:              "); Serial.println(WiFi.macAddress());
  Serial.print("AP MAC:               "); Serial.println(WiFi.softAPmacAddress());
  Serial.print("Wi-Fi history:        "); Serial.print(historyCount); Serial.print(" / "); Serial.print(scanHistoryRetentionLimit); Serial.print(" retained; "); Serial.print(scanHistoryCapacity); Serial.println(" physical");
  Serial.print("Wi-Fi history RAM:    "); Serial.print(wifiHistoryAllocatedBytes()/1024.0, 1); Serial.println(" KB");
  Serial.print("BLE history:          ");
  if (bleSurveyEnabled) { Serial.print(bleHistoryCount); Serial.print(" / "); Serial.print(bleHistoryRetentionLimit); Serial.print(" retained; "); Serial.print(bleHistoryCapacity); Serial.println(" physical"); }
  else Serial.println("Disabled at boot");
  Serial.print("Status LED:           "); Serial.println(STATUS_LED_AVAILABLE ? (statusLedEnabled ? "Enabled" : "Disabled by setting") : "Not available");
  Serial.print("Web live updates:     "); Serial.println(webAutoRefreshEnabled ? "Enabled" : "Disabled");
  Serial.print("mDNS hostname:        "); Serial.print(mdnsHostname); Serial.println(".local");
  Serial.print("Friendly web address:"); Serial.print(" "); Serial.println(mdnsWebAddress());
  Serial.print("mDNS status:          "); Serial.println(mdnsStatusMessage);

  Serial.println();
  Serial.println("Boot Heap Checkpoints:");
  Serial.println("Stage                         Free KB   Min KB   Largest KB");
  Serial.println("----------------------------  --------  -------  ----------");
  for (size_t i=0; i<bootHeapCheckpointCount; i++) {
    printPadded(String(bootHeapCheckpoints[i].stage), 30);
    printPadded(String(bootHeapCheckpoints[i].freeHeap/1024.0,1), 10);
    printPadded(String(bootHeapCheckpoints[i].minimumFreeHeap/1024.0,1), 9);
    Serial.println(String(bootHeapCheckpoints[i].largestFreeBlock/1024.0,1));
  }

  Serial.println();
  Serial.println("System commands:");
  Serial.println("  selftest              - Run/report software self-tests");
  Serial.println("  led test              - Run status LED self-test");
  Serial.println("  version               - Firmware information");
  Serial.println("  0/back                - Main menu");
  Serial.println();
  Serial.print("> ");
}

// Purpose: Prints the firmware self-test results to Serial.
void printSoftwareSelfTestsSerial() {
  bool wifiHistOk = scanHistory && wifiApTable && wifiScanMetadata &&
                    scanHistoryCapacity >= MIN_SCAN_HISTORY_RECORDS;
  bool bleOk = !bleSurveyEnabled ||
      (bleInitialized && bleHistory != nullptr && bleAddressTable != nullptr && bleScanMetadata != nullptr);
  bool intervalsOk = scanIntervalSeconds >= MIN_SCAN_INTERVAL_SECONDS &&
                     scanIntervalSeconds <= MAX_SCAN_INTERVAL_SECONDS &&
                     (!bleSurveyEnabled || (bleScanIntervalSeconds >= MIN_SCAN_INTERVAL_SECONDS && bleScanIntervalSeconds <= MAX_SCAN_INTERVAL_SECONDS));
  bool initialDone = !initialWifiScanPending && (!bleSurveyEnabled || !initialBleScanPending);
  bool autoOk = !bleSurveyEnabled || autoBleScanEnabled;
  uint32_t selfTestFreeHeap = ESP.getFreeHeap();
  uint32_t selfTestMinHeap = ESP.getMinFreeHeap();
  bool heapOk = bleSurveyEnabled
    ? (selfTestFreeHeap >= 24 * 1024 &&
       selfTestMinHeap >= DUAL_RADIO_MIN_HEAP_WARN_BYTES)
    : (selfTestFreeHeap >= HEAP_WARN_BYTES &&
       selfTestMinHeap >= HEAP_WARN_BYTES);

  Serial.println();
  Serial.println("Software Self-Tests");
  Serial.println("-------------------");
  Serial.print("Wi-Fi subsystem:            "); Serial.println(wifiSubsystemInitialized ? "PASS" : "FAIL");
  Serial.print("BLE subsystem/mode:         "); Serial.println(bleOk ? "PASS" : "FAIL");
  Serial.print("Wi-Fi history allocation:  "); Serial.println(wifiHistOk ? "PASS" : "FAIL");
  Serial.print("Configuration ranges:      "); Serial.println(intervalsOk ? "PASS" : "WARN");
  Serial.print("Initial boot scan(s):      "); Serial.println(initialDone ? "PASS" : "WARN");
  Serial.print("Headless automatic survey: "); Serial.println(autoOk ? "PASS" : "WARN");
  Serial.print("mDNS hostname:             "); Serial.println(mdnsStarted ? "PASS" : "WARN");
  Serial.print("Heap reserve:              "); Serial.print(heapOk ? "PASS" : "WARN");
  Serial.print(" - "); Serial.print(selfTestFreeHeap/1024); Serial.print(" KB free, ");
  Serial.print(selfTestMinHeap/1024); Serial.println(" KB minimum");
  Serial.print("Boot/reset diagnostic:     "); Serial.println(resetReasonLabel(bootStaticSystemInfo.resetReason));
  Serial.println();
}

// Purpose: Prints current configurable settings and relevant state to Serial.
void printSettingsSerial() {
  Serial.println();
  Serial.println("============================================================");
  Serial.println(" Settings");
  Serial.println("============================================================");
  Serial.print("Infrastructure Wi-Fi: "); Serial.println(WiFi.status()==WL_CONNECTED ? "Connected" : "Disconnected");
  if (WiFi.status()==WL_CONNECTED) { Serial.print("  SSID:              "); Serial.println(WiFi.SSID()); }
  Serial.print("Device AP:             "); Serial.println(apRunning ? "Running" : "Disabled");
  Serial.print("  Broadcast SSID:      "); Serial.println(apSSID);
  Serial.print("Bluetooth Survey:      "); Serial.println(bleSurveyEnabled ? "Enabled" : "Disabled");
  Serial.print("Status LED:            "); Serial.println(statusLedEnabled ? "Enabled" : "Disabled");
  Serial.print("Web live updates:      "); Serial.println(webAutoRefreshEnabled ? "Enabled" : "Disabled");
  Serial.print("mDNS hostname:         "); Serial.print(mdnsHostname); Serial.println(".local");
  Serial.print("Friendly web address: "); Serial.println(mdnsWebAddress());
  Serial.println();
  Serial.println("Settings commands:");
  Serial.println("  wifi-config           - Configure infrastructure Wi-Fi");
  Serial.println("  wifi-clear            - Clear saved infrastructure Wi-Fi");
  Serial.println("  ble on|off            - Change BLE mode and restart");
  Serial.println("  led on|off|test       - Status LED control/self-test");
  Serial.println("  refresh on|off        - Survey web-page live updates");
  Serial.println("  hostname <name>       - Set mDNS hostname and restart");
  Serial.println("  ap on|off             - Enable/disable Device AP and restart");
  Serial.println("  apssid <name>         - Change Device AP broadcast SSID and restart");
  Serial.println("  appass <password>     - Change Device AP password and restart");
  Serial.println("  0/back                - Main menu");
  Serial.println();
  Serial.print("> ");
}

// Purpose: Compatibility alias that prints the current serial main menu.
void printMenu() { printSerialMainMenu(); }

// Purpose: Extracts and trims the argument portion of a serial command with a known prefix.
String commandArgument(const String& command, const String& prefix) {
  if (command.length() <= prefix.length()) return "";
  String value = command.substring(prefix.length());
  value.trim();
  return value;
}

// Purpose: Parses one serial command and dispatches it to the appropriate survey, settings, diagnostic, or test action.
void handleSerialCommand() {
  if (!Serial.available()) return;

  String command = Serial.readStringUntil('\n');
  command.trim();
  if (command.length() == 0) { printSerialMainMenu(); return; }

  // Echo a sanitized receive line so captured logs preserve command context without exposing passwords.
  String echoedCommand = command;
  if (command.startsWith("appass ")) echoedCommand = "appass ********";
  echoedCommand.toCharArray(lastSerialCommand, sizeof(lastSerialCommand));
  Serial.print("[RX] ");
  Serial.println(lastSerialCommand);

  if (command == "1" || command.equalsIgnoreCase("wifi-survey")) { printWifiSurveySerial(); return; }
  if (command == "2" || command.equalsIgnoreCase("bluetooth") || command.equalsIgnoreCase("ble-survey")) { printBluetoothSurveySerial(); return; }
  if (command == "3" || command.equalsIgnoreCase("system")) { printSystemSerial(); return; }
  if (command == "4" || command.equalsIgnoreCase("settings")) { printSettingsSerial(); return; }
  if (command == "5" || command.equalsIgnoreCase("developer") || command.equalsIgnoreCase("dev")) { printDeveloperDiagnosticSummary(); return; }
  if (command == "0" || command.equalsIgnoreCase("back") || command.equalsIgnoreCase("h") || command.equalsIgnoreCase("help")) { printSerialMainMenu(); return; }

  if (command.equalsIgnoreCase("diag on") || command.equalsIgnoreCase("diag off")) {
    diagnosticStreamingEnabled = command.substring(command.length()-2).equalsIgnoreCase("on");
    saveDiagnosticStreamingSettings();
    diagnosticLastSnapshotMs = millis();
    Serial.print("Diagnostic streaming "); Serial.println(diagnosticStreamingEnabled ? "enabled." : "disabled.");
    if (diagnosticStreamingEnabled) printDiagnosticSnapshot();
    printDeveloperDiagnosticSummary(); return;
  }
  if (command.equalsIgnoreCase("diag snapshot")) { printDiagnosticSnapshot(); Serial.print("> "); return; }
  if (command.equalsIgnoreCase("diag summary")) { printDeveloperDiagnosticSummary(); return; }
  if (command.equalsIgnoreCase("diag reset")) { resetDeveloperDiagnostics(); Serial.println("Diagnostic timing counters reset."); printDeveloperDiagnosticSummary(); return; }
  if (command.startsWith("diag interval ")) {
    long seconds = commandArgument(command, "diag interval").toInt();
    if (seconds < 0 || seconds > 3600) Serial.println("Diagnostic snapshot interval must be 0-3600 seconds.");
    else { diagnosticSnapshotIntervalMs = (uint32_t)seconds * 1000UL; saveDiagnosticStreamingSettings(); diagnosticLastSnapshotMs = millis(); Serial.println(seconds == 0 ? "Periodic diagnostic snapshots disabled." : "Periodic diagnostic snapshot interval updated."); }
    printDeveloperDiagnosticSummary(); return;
  }
  if (command.startsWith("diag ")) {
    int lastSpace = command.lastIndexOf(' ');
    String category = command.substring(5, lastSpace); category.trim();
    String state = command.substring(lastSpace + 1); state.trim();
    if (!state.equalsIgnoreCase("on") && !state.equalsIgnoreCase("off")) { Serial.println("Use on or off for a diagnostic category."); printDeveloperDiagnosticSummary(); return; }
    bool enabled = state.equalsIgnoreCase("on");
    bool matched = true;
    if (category.equalsIgnoreCase("survey")) diagnosticSurveyEvents = enabled;
    else if (category.equalsIgnoreCase("ble")) diagnosticBleEvents = enabled;
    else if (category.equalsIgnoreCase("memory")) diagnosticMemoryEvents = enabled;
    else if (category.equalsIgnoreCase("web")) diagnosticWebEvents = enabled;
    else if (category.equalsIgnoreCase("scheduler")) diagnosticSchedulerEvents = enabled;
    else if (category.equalsIgnoreCase("checkpoint")) diagnosticCheckpointEvents = enabled;
    else matched = false;
    if (!matched) Serial.println("Unknown diagnostic category.");
    else { saveDiagnosticStreamingSettings(); Serial.print("Diagnostic category '"); Serial.print(category); Serial.print("' "); Serial.println(enabled ? "enabled." : "disabled."); }
    printDeveloperDiagnosticSummary(); return;
  }

  if (command.equalsIgnoreCase("scan")) { performLoggedScan(); WiFi.scanDelete(); printWifiSurveySerial(); return; }
  if (command.equalsIgnoreCase("wclear")) { clearScanHistory(); Serial.println("Wi-Fi history cleared."); printWifiSurveySerial(); return; }

  if (command.startsWith("interval ")) {
    long n=commandArgument(command,"interval").toInt();
    if (n < (long)MIN_SCAN_INTERVAL_SECONDS || n > (long)MAX_SCAN_INTERVAL_SECONDS) Serial.println("Invalid Wi-Fi interval.");
    else {
      scanIntervalSeconds=(unsigned long)n;
      lastAutoScanMs=millis();
      preferences.begin("survey", false);
      preferences.putULong("wifiInterval", scanIntervalSeconds);
      preferences.end();
      Serial.println("Wi-Fi scan interval updated and saved.");
    }
    printWifiSurveySerial(); return;
  }

  if (command.equalsIgnoreCase("blescan")) {
    if (!bleSurveyEnabled) {
      Serial.println("Bluetooth Survey is disabled. Use 'ble on' to enable it and restart.");
    } else {
      int result = performLoggedBLEScanWithTrigger("serial-manual");
      if (result == -5) Serial.println("BLE scan not started: Wi-Fi scan is active. Retry when wifiScan=0.");
      else if (result < 0) { Serial.print("BLE scan not started: "); Serial.println(bleStatusMessage); }
      else Serial.println("BLE scan started asynchronously.");
    }
    printBluetoothSurveySerial(); return;
  }
  if (command.equalsIgnoreCase("bleclear")) { if (bleSurveyEnabled) clearBleHistory(); Serial.println("BLE history cleared."); printBluetoothSurveySerial(); return; }

  if (command.startsWith("bleinterval ")) {
    long n=commandArgument(command,"bleinterval").toInt();
    if (!bleSurveyEnabled) Serial.println("Bluetooth Survey is disabled.");
    else if (n < (long)MIN_SCAN_INTERVAL_SECONDS || n > (long)MAX_SCAN_INTERVAL_SECONDS) Serial.println("Invalid BLE interval.");
    else { bleScanIntervalSeconds=(unsigned long)n; autoBleScanEnabled=true; lastAutoBleScanMs=millis(); preferences.begin("survey", false); preferences.putULong("bleInterval", bleScanIntervalSeconds); preferences.end(); Serial.println("BLE scan interval updated and saved."); }
    printBluetoothSurveySerial(); return;
  }
  if (command.equalsIgnoreCase("bleauto on") || command.equalsIgnoreCase("bleauto off")) {
    if (bleSurveyEnabled) { autoBleScanEnabled=true; lastAutoBleScanMs=millis(); }
    Serial.println("Automatic Bluetooth surveying is always active while Bluetooth Survey is enabled.");
    printBluetoothSurveySerial(); return;
  }
  if (command.equalsIgnoreCase("ble on") || command.equalsIgnoreCase("ble off")) {
    bool requested=command.endsWith("on");
    if (requested==bleSurveyEnabled) { Serial.println("Bluetooth Survey mode already set."); printSettingsSerial(); return; }
    saveBleSurveyEnabled(requested);
    Serial.print("Bluetooth Survey will be "); Serial.print(requested?"enabled":"disabled"); Serial.println(" after restart. Restarting...");
    checkpointBeforeControlledRestart(); delay(500); ESP.restart(); return;
  }

  if (command.equalsIgnoreCase("selftest")) { printSoftwareSelfTestsSerial(); Serial.print("> "); return; }
  if (command.equalsIgnoreCase("led test")) { Serial.println("Running status LED self-test..."); runStatusLedSelfTest(); Serial.println("LED self-test complete."); Serial.print("> "); return; }
  if (command.equalsIgnoreCase("led on") || command.equalsIgnoreCase("led off")) {
    bool requested=command.endsWith("on"); saveInterfaceSettings(requested,webAutoRefreshEnabled); if(!requested) stopScanLed();
    Serial.print("Status LED "); Serial.println(requested?"enabled.":"disabled."); printSettingsSerial(); return;
  }
  if (command.equalsIgnoreCase("refresh on") || command.equalsIgnoreCase("refresh off")) {
    bool requested=command.endsWith("on"); saveInterfaceSettings(statusLedEnabled,requested);
    Serial.print("Web live updates "); Serial.println(requested?"enabled.":"disabled."); printSettingsSerial(); return;
  }

  if (command.equalsIgnoreCase("wifi-config")) { configureWiFi(); printSettingsSerial(); return; }
  if (command.equalsIgnoreCase("wifi-clear")) { eraseCredentials(); WiFi.disconnect(false); ensureWiFiStationMode(); Serial.println("Saved infrastructure Wi-Fi credentials cleared."); printSettingsSerial(); return; }

  if (command.startsWith("hostname ")) {
    String requested = normalizedMdnsHostname(commandArgument(command, "hostname"));
    if (!isValidMdnsHostname(requested)) {
      Serial.println("Hostname must be 1-32 letters, numbers, or hyphens and cannot begin/end with a hyphen.");
      printSettingsSerial(); return;
    }
    if (requested == mdnsHostname) {
      Serial.println("mDNS hostname is already set to that value.");
      printSettingsSerial(); return;
    }
    saveMdnsHostname(requested);
    Serial.print("mDNS hostname saved as "); Serial.print(mdnsHostname); Serial.println(".local. Restarting...");
    checkpointBeforeControlledRestart(); delay(500); ESP.restart(); return;
  }

  if (command.equalsIgnoreCase("ap on") || command.equalsIgnoreCase("ap off")) {
    bool requested=command.endsWith("on"); saveAccessPointSettings(requested,apSSID,apPassword);
    Serial.println("Device AP setting saved. Restarting..."); checkpointBeforeControlledRestart(); delay(500); ESP.restart(); return;
  }
  if (command.startsWith("apssid ")) {
    String requested=commandArgument(command,"apssid"); requested.trim();
    if (requested.length()==0 || requested.length()>32) { Serial.println("AP SSID must be 1-32 characters."); printSettingsSerial(); return; }
    saveAccessPointSettings(apEnabled,requested,apPassword); Serial.println("AP SSID saved. Restarting..."); checkpointBeforeControlledRestart(); delay(500); ESP.restart(); return;
  }
  if (command.startsWith("appass ")) {
    String requested=commandArgument(command,"appass");
    if (requested.length()<8 || requested.length()>63) { Serial.println("AP password must be 8-63 characters."); printSettingsSerial(); return; }
    saveAccessPointSettings(apEnabled,apSSID,requested); Serial.println("AP password saved. Restarting..."); checkpointBeforeControlledRestart(); delay(500); ESP.restart(); return;
  }

  if (command.equalsIgnoreCase("version") || command.equalsIgnoreCase("v")) { printFirmwareInfo(); Serial.print("> "); return; }
  if (command.equalsIgnoreCase("mac")) { printMacAddress(); Serial.print("> "); return; }
  if (command.equalsIgnoreCase("restart")) { Serial.println("Restarting ESP32..."); checkpointBeforeControlledRestart(); delay(500); ESP.restart(); return; }

  Serial.print("Unknown command: "); Serial.println(command);
  Serial.println("Enter 'h' for the main menu.");
  Serial.print("> ");
}

// ============================================================
// Setup
// ============================================================

// Purpose: Arduino entry point that initializes hardware, settings, radios, histories, checkpoint restore, web services, and initial survey scheduling.
void setup() {
  Serial.begin(115200);
  delay(1500);

  captureBootHeapCheckpoint("Startup");
  loadSurveyModeSettings();
  loadDiagnosticStreamingSettings();
  captureBootHeapCheckpoint("Settings loaded");
  initializeStatusLed();
  captureBootHeapCheckpoint("Status LED initialized");
  indicateBootStarted();
  captureBootStaticSystemInfo();

  Serial.println();
  Serial.println();
  Serial.println("================================");
  Serial.println(" ESP32 Starting");
  Serial.println("================================");
  Serial.print("Firmware: ");
  Serial.println(FIRMWARE_FILE);
  Serial.print("Version:  ");
  Serial.println(FIRMWARE_VERSION);
  Serial.print("Built:    ");
  Serial.println(firmwareBuildTimestamp());
  Serial.println();
  if (diagnosticStreamingEnabled) {
    diagnosticPrefix("BOOT");
    Serial.print("firmware=V"); Serial.print(FIRMWARE_VERSION);
    Serial.print(" bleMode="); Serial.print(bleSurveyEnabled ? 1 : 0);
    Serial.print(" snapshot="); Serial.print(diagnosticSnapshotIntervalMs / 1000); Serial.print("s ");
    diagnosticPrintHeapTriplet();
    Serial.println();
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  delay(WIFI_STARTUP_SETTLE_MS);
  wifiSubsystemInitialized = (WiFi.getMode() != WIFI_MODE_NULL);
  applyGeneratedDefaultMdnsHostname();
  captureBootHeapCheckpoint("Wi-Fi initialized");

  loadAccessPointSettings();
  Serial.print("Bluetooth Survey mode: ");
  Serial.println(bleSurveyEnabled ? "enabled" : "disabled");
  Serial.print("Diagnostic streaming: ");
  Serial.println(diagnosticStreamingEnabled ? "enabled (persisted)" : "disabled");
  if (apEnabled) startAccessPoint();
  captureBootHeapCheckpoint("AP setup complete");

  bool connected = connectUsingSavedCredentials();
  captureBootHeapCheckpoint("STA attempt complete");

  if (!connected) {
    Serial.println();
    Serial.println("Infrastructure Wi-Fi is not configured or unavailable.");
    if (apRunning) {
      Serial.println("The ESP32 access point remains available for surveying and web configuration.");
    } else {
      Serial.println("Use the serial 'wifi-config' command to configure infrastructure Wi-Fi.");
    }
  }

  // Initialize BLE only when the persisted survey mode enables it. When disabled,
  // the BLE stack remains completely uninitialized so its RAM stays available
  // to Wi-Fi history.
  if (bleSurveyEnabled) {
    initializeBLEScanner();
    captureBootHeapCheckpoint("BLE initialized");
  } else {
    autoBleScanEnabled = false;
    initialBleScanPending = false;
    captureBootHeapCheckpoint("BLE disabled by setting");
  }

  initializeAutoSizedHistories();
  captureBootHeapCheckpoint("Histories allocated");
  initializeSessionStorageAndRestore();

  Serial.print("Auto-sized Wi-Fi history: ");
  Serial.print(scanHistoryCapacity);
  Serial.println(" compact observation slots");
  Serial.print("Auto-sized BLE history:   ");
  if (bleSurveyEnabled) {
    Serial.print(bleHistoryCapacity);
    Serial.println(" compact observation slots");
  } else {
    Serial.println("disabled");
  }
  Serial.print("History allocation reserve target: ");
  Serial.print((bleSurveyEnabled ? DUAL_RADIO_HEAP_RESERVE_BYTES : HISTORY_HEAP_RESERVE_BYTES) / 1024);
  Serial.println(" KB");
  Serial.print("Heap after history allocation:      ");
  Serial.print(ESP.getFreeHeap() / 1024.0, 1);
  Serial.print(" KB free; largest block ");
  Serial.print(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024.0, 1);
  Serial.println(" KB");

  if (WiFi.status() == WL_CONNECTED || apRunning) startWebServer();
  captureBootHeapCheckpoint("Web server ready");

  if (webServerStarted) {
    startMdnsService();
    if (mdnsStarted) {
      Serial.print("Friendly web UI: ");
      Serial.println(mdnsWebAddress());
    } else {
      Serial.print("mDNS warning: ");
      Serial.println(mdnsStatusMessage);
    }
  }
  captureBootHeapCheckpoint("mDNS ready");

  bool startupFailed =
      !wifiSubsystemInitialized ||
      scanHistory == nullptr ||
      wifiApTable == nullptr ||
      wifiScanMetadata == nullptr ||
      (bleSurveyEnabled && (!bleInitialized || bleHistory == nullptr ||
        bleAddressTable == nullptr || bleScanMetadata == nullptr));

  bool startupWarning =
      !startupFailed &&
      (ESP.getFreeHeap() < HEAP_WARN_BYTES ||
       (mdnsAttempted && !mdnsStarted) ||
       scanHistoryCapacity == MIN_SCAN_HISTORY_RECORDS ||
       (bleSurveyEnabled &&
        (bleHistoryCapacity == MIN_BLE_HISTORY_RECORDS || bleAddressTableFullDrops > 0)));

  indicateStartupStatus(startupFailed, startupWarning);

  surveyServicesReadyMs = millis();
  lastAutoScanMs = surveyServicesReadyMs;
  lastAutoBleScanMs = surveyServicesReadyMs;
  initialWifiScanPending = true;
  initialBleScanPending = bleSurveyEnabled;

  printMenu();
}


// ============================================================
// Automatic scan service


// ============================================================
// Automatic scan service
// ============================================================

// Purpose: Runs the required first Wi-Fi/BLE scans shortly after boot without blocking overall startup longer than necessary.
void serviceInitialSurveyScans() {
  unsigned long elapsed = millis() - surveyServicesReadyMs;

  if (
    initialWifiScanPending &&
    elapsed >= INITIAL_WIFI_SCAN_DELAY_MS &&
    !wifiScanInProgress &&
    !csvExportInProgress &&
    !userInteractionDeferActive()
  ) {
    initialWifiScanPending = false;
    Serial.println("Initial headless Wi-Fi survey scan...");
    if (!beginLoggedWifiScan(true, true)) {
      Serial.println("Unable to start initial asynchronous Wi-Fi scan.");
    }
  }

  if (
    bleSurveyEnabled &&
    initialBleScanPending &&
    elapsed >= INITIAL_BLE_SCAN_DELAY_MS &&
    !wifiScanInProgress &&
    !csvExportInProgress &&
    !userInteractionDeferActive()
  ) {
    initialBleScanPending = false;
    Serial.println("Initial headless BLE survey scan...");
    performLoggedBLEScanWithTrigger("initial");
    lastAutoBleScanMs = millis();
  }
}

// Purpose: Schedules automatic Wi-Fi scans while honoring active work, interaction defer, retry backoff, and configured interval.
void serviceAutomaticScan() {
  if (wifiScanInProgress || bleDiagnosticScanActive || csvExportInProgress || userInteractionDeferActive()) return;

  uint32_t now = millis();

  if (wifiAutoScanRetryPending) {
    if ((uint32_t)(now - lastWifiAutoScanFailureMs) < WIFI_AUTOSCAN_RETRY_BACKOFF_MS) return;
    wifiAutoScanRetryPending = false;
    beginLoggedWifiScan(false, true);
    return;
  }

  unsigned long intervalMs = scanIntervalSeconds * 1000UL;
  if ((uint32_t)(now - lastAutoScanMs) < intervalMs) return;

  beginLoggedWifiScan(false, true);
}

// Purpose: Schedules periodic BLE scans when Bluetooth Survey and automatic BLE surveying are enabled.
void serviceAutomaticBLEScan() {
  if (!bleSurveyEnabled) { diagnosticBleSchedulerState(1, "disabled"); return; }
  if (!autoBleScanEnabled) { diagnosticBleSchedulerState(2, "automatic-off"); return; }
  if (bleDiagnosticScanActive) { diagnosticBleSchedulerState(8, "scan-active"); return; }

  unsigned long intervalMs = bleScanIntervalSeconds * 1000UL;
  if ((uint32_t)(millis() - lastAutoBleScanMs) < intervalMs) { diagnosticBleSchedulerState(3, "waiting-interval"); return; }
  if (wifiScanInProgress) { diagnosticBleSchedulerState(4, "deferred-wifi-scan"); return; }
  if (csvExportInProgress) { diagnosticBleSchedulerState(5, "deferred-csv"); return; }
  if (userInteractionDeferActive()) { diagnosticBleSchedulerState(6, "deferred-user-interaction"); return; }

  diagnosticBleSchedulerState(7, "starting");
  lastAutoBleScanMs = millis();
  performLoggedBLEScanWithTrigger("automatic");
  diagnosticBleSchedulerState(3, "waiting-interval");
}


// ============================================================
// Main loop
// ============================================================

// Purpose: Arduino main loop that continuously services web requests, serial commands, scan completion, reconnect diagnostics, and automatic survey scheduling.
void loop() {
  serviceLoopGapDiagnostics();

  if (webServerStarted) {
    server.handleClient();
    armUserInteractionDeferAfterWebService();
  }

  serviceDiagnosticSnapshot();
  serviceCompletedBLEScan();
  serviceLoggedWifiScan();
  serviceNativeReconnectDiagnostics();
  serviceInitialSurveyScans();
  serviceAutomaticScan();
  serviceAutomaticBLEScan();
  handleSerialCommand();

  delay(5);
}