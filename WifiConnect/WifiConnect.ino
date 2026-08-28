// WifiConnect21 - compact normalized Wi-Fi history, logical retention limit,
// relative first/last-seen ages, scan-completion page refresh, optional BLE mode
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_arduino_version.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

// ============================================================
// Firmware identity
// ============================================================

const char* FIRMWARE_FILE = "WifiConnect21_compact_wifi_history.ino";
const char* FIRMWARE_VERSION = "21";


Preferences preferences;
WebServer server(80);

const unsigned long WIFI_TIMEOUT_MS = 15000;
const unsigned long WIFI_STARTUP_SETTLE_MS = 300;

bool webServerStarted = false;


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
const size_t MAX_BLE_HISTORY_RECORDS = 2000;

const size_t HISTORY_HEAP_RESERVE_BYTES = 96 * 1024;

const unsigned long MIN_SCAN_INTERVAL_SECONDS = 5;
const unsigned long MAX_SCAN_INTERVAL_SECONDS = 3600;
const uint32_t BLE_SCAN_DURATION_SECONDS = 5;
const unsigned long INITIAL_WIFI_SCAN_DELAY_MS = 2500;
const unsigned long INITIAL_BLE_SCAN_DELAY_MS = 9000;
const size_t HEAP_WARN_BYTES = 48 * 1024;

// V20 Wi-Fi-first operating mode. Bluetooth is disabled by default and the
// preference is stored in NVS. Changing the setting from the web UI triggers
// a controlled restart so BLE is either fully initialized before history
// allocation or never initialized at all.
bool bleSurveyEnabled = false;

// Common ESP32 DEVKITV1 boards expose a controllable blue LED on GPIO2.
// The red LED found on many boards is a hard-wired power LED and cannot be
// controlled by firmware. Set STATUS_LED_ENABLED false if GPIO2 is not an
// onboard LED on the specific board being used.
const bool STATUS_LED_ENABLED = true;
const uint8_t STATUS_LED_PIN = 2;
const bool STATUS_LED_ACTIVE_HIGH = true;
const TickType_t WIFI_SCAN_LED_PERIOD_TICKS = pdMS_TO_TICKS(75);
const TickType_t BLE_SCAN_LED_PERIOD_TICKS = pdMS_TO_TICKS(125);


// ScanRecord remains a synthesized view used by the existing UI/CSV code.
// V21 no longer stores this 68-byte structure in the history ring.
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
};

// Scan number and timestamp are stored once per scan. Observations reference
// one of these slots. Slots are recycled only after observations using the
// old slot have been discarded.
struct WifiScanMetadata {
  uint32_t scanNumber;
  uint32_t uptimeMs;
};

// Compact recurring measurement. 6 bytes on the classic ESP32 ABI versus
// 68 bytes for the old flat ScanRecord.
struct WifiObservation {
  uint16_t apIndex;
  uint16_t scanSlot;
  int8_t rssi;
  uint8_t reserved;
};

struct BleScanRecord {
  uint32_t scanNumber;
  uint32_t uptimeMs;
  char name[48];
  char address[18];
  int16_t rssi;
  uint8_t addressType;
  bool named;
};

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

// Explicit prototypes for V21 custom types to avoid Arduino 1.8.x auto-prototype ordering issues.
const WifiObservation& compactHistoryRecord(size_t logicalIndex);
ScanRecord historyRecord(size_t logicalIndex);
void appendWifiObservation(const WifiObservation& observation);

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
bool autoScanEnabled = true;
unsigned long scanIntervalSeconds = 300;
unsigned long lastAutoScanMs = 0;

WifiApEntry* wifiApTable = nullptr;
size_t wifiApTableCapacity = 0;
size_t wifiApCount = 0;
WifiScanMetadata* wifiScanMetadata = nullptr;
size_t wifiScanMetadataCapacity = 0;
size_t wifiApTableFullDrops = 0;

BleScanRecord* bleHistory = nullptr;
size_t bleHistoryCapacity = 0;
size_t bleHistoryStart = 0;
size_t bleHistoryCount = 0;
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
// V19/V20 boot/resource diagnostics and status LED
// ============================================================

void captureBootHeapCheckpoint(const char* stage) {
  if (bootHeapCheckpointCount >= MAX_BOOT_HEAP_CHECKPOINTS) return;

  BootHeapCheckpoint& cp = bootHeapCheckpoints[bootHeapCheckpointCount++];
  cp.stage = stage;
  cp.freeHeap = ESP.getFreeHeap();
  cp.minimumFreeHeap = ESP.getMinFreeHeap();
  cp.largestFreeBlock =
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

void writeStatusLed(bool on) {
  if (!STATUS_LED_ENABLED) return;

  statusLedState = on;
  bool electricalHigh =
      STATUS_LED_ACTIVE_HIGH ? on : !on;
  digitalWrite(STATUS_LED_PIN, electricalHigh ? HIGH : LOW);
}

void statusLedTimerCallback(TimerHandle_t) {
  writeStatusLed(!statusLedState);
}

void initializeStatusLed() {
  if (!STATUS_LED_ENABLED) return;

  pinMode(STATUS_LED_PIN, OUTPUT);
  writeStatusLed(false);

  statusLedTimer = xTimerCreate(
      "surveyLed",
      WIFI_SCAN_LED_PERIOD_TICKS,
      pdTRUE,
      nullptr,
      statusLedTimerCallback);
}

void startScanLed(TickType_t periodTicks) {
  if (!STATUS_LED_ENABLED || statusLedTimer == nullptr) return;

  writeStatusLed(true);
  xTimerStop(statusLedTimer, 0);
  xTimerChangePeriod(statusLedTimer, periodTicks, 0);
}

void stopScanLed() {
  if (!STATUS_LED_ENABLED) return;

  if (statusLedTimer != nullptr) {
    xTimerStop(statusLedTimer, 0);
  }

  writeStatusLed(false);
}

void statusLedPulse(
  unsigned long onMs,
  unsigned long offMs
) {
  if (!STATUS_LED_ENABLED) return;

  writeStatusLed(true);
  delay(onMs);
  writeStatusLed(false);
  delay(offMs);
}

void indicateBootStarted() {
  if (!STATUS_LED_ENABLED) return;

  statusLedPulse(90, 90);
  statusLedPulse(90, 0);
}

void indicateStartupStatus(bool failed, bool warning) {
  if (!STATUS_LED_ENABLED) return;

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


// ============================================================
// Serial helpers
// ============================================================

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

String macSuffix() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");

  if (mac.length() >= 6) {
    return mac.substring(mac.length() - 6);
  }

  return "ESP32";
}

String defaultApSSID() {
  return "ESP32-Surveyor-" + macSuffix();
}

String defaultApPassword() {
  return "survey-" + macSuffix();
}

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

void loadSurveyModeSettings() {
  preferences.begin("survey", true);
  bleSurveyEnabled = preferences.getBool("bleEnabled", false);
  preferences.end();
}

void saveBleSurveyEnabled(bool enabled) {
  preferences.begin("survey", false);
  preferences.putBool("bleEnabled", enabled);
  preferences.end();
}

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

float averageSignal(const SignalStats& stats) {
  if (stats.samples == 0) {
    return 0.0f;
  }

  return
    (float)stats.rssiTotal /
    (float)stats.samples;
}


// ============================================================
// BLE helpers
// ============================================================

String bleAddressTypeLabel(uint8_t addressType) {
  switch (addressType) {
    case 0: return "Public";
    case 1: return "Random";
    case 2: return "Public ID";
    case 3: return "Random ID";
    default: return "Unknown";
  }
}

void initializeBLEScanner() {
  if (!bleSurveyEnabled || bleInitialized) return;

  BLEDevice::init("");
  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  bleInitialized = true;
}


// ============================================================
// Wi-Fi compact normalized history (V21)
// ============================================================

size_t wifiHistoryAllocatedBytes() {
  return
    scanHistoryCapacity * sizeof(WifiObservation) +
    wifiApTableCapacity * sizeof(WifiApEntry) +
    wifiScanMetadataCapacity * sizeof(WifiScanMetadata);
}

const WifiObservation& compactHistoryRecord(size_t logicalIndex) {
  size_t physicalIndex =
      (historyStart + logicalIndex) % scanHistoryCapacity;
  return scanHistory[physicalIndex];
}

void formatBssid(const uint8_t bssid[6], char output[18]) {
  snprintf(
    output, 18,
    "%02X:%02X:%02X:%02X:%02X:%02X",
    bssid[0], bssid[1], bssid[2],
    bssid[3], bssid[4], bssid[5]
  );
}

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
  record.connected =
      WiFi.status() == WL_CONNECTED &&
      WiFi.BSSIDstr().equalsIgnoreCase(String(record.bssid));
  return record;
}

size_t countRetainedScanGroups() {
  if (historyCount == 0 || scanHistory == nullptr) return 0;

  size_t groups = 0;
  uint32_t previousScan = 0;
  bool havePrevious = false;

  for (size_t i = 0; i < historyCount; i++) {
    ScanRecord current = historyRecord(i);
    if (!havePrevious || current.scanNumber != previousScan) {
      groups++;
      previousScan = current.scanNumber;
      havePrevious = true;
    }
  }
  return groups;
}

void discardOldestWifiObservation() {
  if (historyCount == 0 || scanHistoryCapacity == 0) return;
  historyStart = (historyStart + 1) % scanHistoryCapacity;
  historyCount--;
}

void discardObservationsForScanSlot(uint16_t scanSlot) {
  // Observations are chronological. Any observation using a metadata slot that
  // is about to be recycled belongs to the oldest retained scan using it.
  while (historyCount > 0) {
    const WifiObservation& oldest = compactHistoryRecord(0);
    if (oldest.scanSlot != scanSlot) break;
    discardOldestWifiObservation();
  }
}

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

// Compatibility wrapper: V21 no longer reallocates the history buffer at
// runtime. Web changes alter only the logical retention limit.
bool resizeScanHistory(size_t requestedCapacity, bool preserveRecords = true) {
  (void)preserveRecords;
  return setWifiRetentionLimit(requestedCapacity);
}

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

void clearScanHistory() {
  historyStart = 0;
  historyCount = 0;
  scanCounter = 0;
  lastScanUptimeMs = 0;
  wifiApCount = 0;
  wifiApTableFullDrops = 0;
  if (wifiScanMetadata && wifiScanMetadataCapacity)
    memset(wifiScanMetadata, 0, wifiScanMetadataCapacity * sizeof(WifiScanMetadata));
}

int findWifiApByBssid(const uint8_t bssid[6]) {
  for (size_t i = 0; i < wifiApCount; i++) {
    if (memcmp(wifiApTable[i].bssid, bssid, 6) == 0)
      return (int)i;
  }
  return -1;
}

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
  uint8_t authMode
) {
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
    return existing;
  }

  if (wifiApTable == nullptr) {
    wifiApTableFullDrops++;
    return -1;
  }

  size_t targetIndex = wifiApCount;
  if (wifiApCount >= wifiApTableCapacity) {
    targetIndex = wifiApTableCapacity;
    for (size_t i = 0; i < wifiApTableCapacity; i++) {
      if (!wifiApIndexIsReferenced(i)) {
        targetIndex = i;
        break;
      }
    }
    if (targetIndex >= wifiApTableCapacity) {
      wifiApTableFullDrops++;
      return -1;
    }
  } else {
    wifiApCount++;
  }

  WifiApEntry& ap = wifiApTable[targetIndex];
  memset(&ap, 0, sizeof(ap));
  ssid.toCharArray(ap.ssid, sizeof(ap.ssid));
  memcpy(ap.bssid, bssid, 6);
  ap.channel = channel;
  ap.authMode = authMode;
  return (int)targetIndex;
}

bool initializeCompactWifiHistory(size_t budgetBytes) {
  // Scale metadata overhead down in BLE mode, where the available Wi-Fi
  // history budget is intentionally small. Wi-Fi-only mode uses larger tables.
  size_t apCapacity = budgetBytes >= 24 * 1024 ? 256 : 64;
  size_t scanCapacity = budgetBytes >= 24 * 1024 ? 1024 : 64;

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

int performLoggedScan() {
  ensureWiFiStationMode();

  startScanLed(WIFI_SCAN_LED_PERIOD_TICKS);
  int networkCount = WiFi.scanNetworks();
  stopScanLed();

  scanCounter++;
  lastScanUptimeMs = millis();

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

  if (networkCount <= 0) return networkCount;

  for (int i = 0; i < networkCount; i++) {
    const uint8_t* rawBssid = WiFi.BSSID(i);
    if (rawBssid == nullptr) continue;

    String ssid = WiFi.SSID(i);
    int apIndex = findOrCreateWifiAp(
      rawBssid,
      ssid,
      (uint8_t)WiFi.channel(i),
      (uint8_t)WiFi.encryptionType(i)
    );

    if (apIndex < 0) continue;

    int rssi = WiFi.RSSI(i);
    if (rssi < -128) rssi = -128;
    if (rssi > 127) rssi = 127;

    WifiObservation observation = {};
    observation.apIndex = (uint16_t)apIndex;
    observation.scanSlot = scanSlot;
    observation.rssi = (int8_t)rssi;
    appendWifiObservation(observation);
  }

  return networkCount;
}

// ============================================================
// BLE history
// ============================================================

const BleScanRecord& bleHistoryRecord(size_t logicalIndex) {
  size_t physicalIndex =
      (bleHistoryStart + logicalIndex) % bleHistoryCapacity;
  return bleHistory[physicalIndex];
}

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

bool resizeBleHistory(size_t requestedCapacity, bool preserveRecords = true) {
  if (requestedCapacity < MIN_BLE_HISTORY_RECORDS)
    requestedCapacity = MIN_BLE_HISTORY_RECORDS;
  if (requestedCapacity > MAX_BLE_HISTORY_RECORDS)
    requestedCapacity = MAX_BLE_HISTORY_RECORDS;

  if (bleHistory != nullptr && requestedCapacity == bleHistoryCapacity) {
    bleHistoryResizeMessage =
      "BLE history limit unchanged at " +
      String(bleHistoryCapacity) + " records.";
    return true;
  }

  BleScanRecord* newHistory =
      (BleScanRecord*)malloc(requestedCapacity * sizeof(BleScanRecord));

  if (newHistory == nullptr) {
    bleHistoryResizeMessage =
      "Unable to allocate " +
      String(requestedCapacity) +
      " BLE records; previous limit retained.";
    return false;
  }

  size_t recordsToKeep = 0;

  if (preserveRecords && bleHistory != nullptr && bleHistoryCount > 0) {
    recordsToKeep =
      bleHistoryCount < requestedCapacity ? bleHistoryCount : requestedCapacity;
    size_t first = bleHistoryCount - recordsToKeep;

    for (size_t i = 0; i < recordsToKeep; i++)
      newHistory[i] = bleHistoryRecord(first + i);
  }

  if (bleHistory != nullptr) free(bleHistory);

  bleHistory = newHistory;
  bleHistoryCapacity = requestedCapacity;
  bleHistoryStart = 0;
  bleHistoryCount = recordsToKeep;

  bleHistoryResizeMessage =
    "BLE history limit set to " +
    String(bleHistoryCapacity) + " records.";

  return true;
}

bool clearAndResizeBleHistory(size_t requestedCapacity) {
  if (requestedCapacity < MIN_BLE_HISTORY_RECORDS)
    requestedCapacity = MIN_BLE_HISTORY_RECORDS;
  if (requestedCapacity > MAX_BLE_HISTORY_RECORDS)
    requestedCapacity = MAX_BLE_HISTORY_RECORDS;

  size_t previousCapacity = bleHistoryCapacity;

  if (bleHistory != nullptr) {
    free(bleHistory);
    bleHistory = nullptr;
  }

  bleHistoryCapacity = 0;
  bleHistoryStart = 0;
  bleHistoryCount = 0;
  bleScanCounter = 0;
  lastBleScanUptimeMs = 0;

  BleScanRecord* newHistory =
      (BleScanRecord*)malloc(requestedCapacity * sizeof(BleScanRecord));

  if (newHistory != nullptr) {
    bleHistory = newHistory;
    bleHistoryCapacity = requestedCapacity;
    bleHistoryResizeMessage =
      "BLE history cleared; limit set to " +
      String(bleHistoryCapacity) + " records.";
    return true;
  }

  size_t fallback =
      previousCapacity > 0 ? previousCapacity : MIN_BLE_HISTORY_RECORDS;

  newHistory =
      (BleScanRecord*)malloc(fallback * sizeof(BleScanRecord));

  if (newHistory != nullptr) {
    bleHistory = newHistory;
    bleHistoryCapacity = fallback;
    bleHistoryResizeMessage =
      "BLE history cleared, requested resize failed; restored empty " +
      String(bleHistoryCapacity) + "-record buffer.";
    return false;
  }

  bleHistoryResizeMessage =
    "BLE history cleared, but no BLE history buffer could be allocated.";
  return false;
}

void appendBleScanRecord(const BleScanRecord& record) {
  if (bleHistory == nullptr || bleHistoryCapacity == 0) return;

  size_t writeIndex;

  if (bleHistoryCount < bleHistoryCapacity) {
    writeIndex =
      (bleHistoryStart + bleHistoryCount) % bleHistoryCapacity;
    bleHistoryCount++;
  } else {
    writeIndex = bleHistoryStart;
    bleHistoryStart = (bleHistoryStart + 1) % bleHistoryCapacity;
  }

  bleHistory[writeIndex] = record;
}

void clearBleHistory() {
  bleHistoryStart = 0;
  bleHistoryCount = 0;
  bleScanCounter = 0;
  lastBleScanUptimeMs = 0;
}

size_t capacityForBudget(
  size_t budgetBytes,
  size_t recordSize,
  size_t minimumRecords,
  size_t maximumRecords
) {
  if (recordSize == 0) return minimumRecords;

  size_t capacity = budgetBytes / recordSize;
  if (capacity < minimumRecords) capacity = minimumRecords;
  if (capacity > maximumRecords) capacity = maximumRecords;
  return capacity;
}

void initializeAutoSizedHistories() {
  if (scanHistory != nullptr || bleHistory != nullptr) return;

  size_t freeHeap = ESP.getFreeHeap();
  size_t available =
    freeHeap > HISTORY_HEAP_RESERVE_BYTES
      ? freeHeap - HISTORY_HEAP_RESERVE_BYTES
      : 0;

  size_t wifiBudget = bleSurveyEnabled ? available / 2 : available;
  size_t bleBudget = bleSurveyEnabled ? available - wifiBudget : 0;

  // When BLE leaves no budget above the preferred reserve, still provide a
  // small functional Wi-Fi history footprint comparable to V20's 50 records.
  size_t minimumCompactBytes =
      64 * sizeof(WifiApEntry) +
      64 * sizeof(WifiScanMetadata) +
      MIN_SCAN_HISTORY_RECORDS * sizeof(WifiObservation);
  if (wifiBudget < minimumCompactBytes)
    wifiBudget = minimumCompactBytes;

  if (!initializeCompactWifiHistory(wifiBudget)) {
    // Last-resort compact allocation with the minimum practical tables.
    size_t fallbackBytes = minimumCompactBytes;
    initializeCompactWifiHistory(fallbackBytes);
  }

  if (bleSurveyEnabled) {
    size_t bleTarget = capacityForBudget(
      bleBudget,
      sizeof(BleScanRecord),
      MIN_BLE_HISTORY_RECORDS,
      MAX_BLE_HISTORY_RECORDS
    );

    if (!resizeBleHistory(bleTarget, false))
      resizeBleHistory(MIN_BLE_HISTORY_RECORDS, false);
  }

  bootWifiHistoryCapacity = scanHistoryCapacity;
  bootBleHistoryCapacity = bleHistoryCapacity;

  historyResizeMessage = "";
  bleHistoryResizeMessage = "";
}

bool bleHistoryContainsAddress(const String& address) {
  for (size_t i = 0; i < bleHistoryCount; i++) {
    if (String(bleHistoryRecord(i).address).equalsIgnoreCase(address))
      return true;
  }
  return false;
}

String latestBleNameForAddress(const String& address) {
  for (size_t offset = 0; offset < bleHistoryCount; offset++) {
    size_t i = bleHistoryCount - 1 - offset;
    const BleScanRecord& record = bleHistoryRecord(i);

    if (
      String(record.address).equalsIgnoreCase(address) &&
      record.named
    ) {
      return String(record.name);
    }
  }
  return "(unnamed)";
}

bool hasNewerBleObservationForAddress(
  size_t logicalIndex,
  const String& address
) {
  for (size_t i = logicalIndex + 1; i < bleHistoryCount; i++) {
    if (String(bleHistoryRecord(i).address).equalsIgnoreCase(address))
      return true;
  }
  return false;
}

bool buildBleDeviceSummary(
  const String& address,
  BLEDeviceSummary& summary
) {
  summary = {};
  resetSignalStats(summary.signal);

  bool found = false;
  String newestName = "";

  for (size_t i = 0; i < bleHistoryCount; i++) {
    const BleScanRecord& record = bleHistoryRecord(i);
    if (!String(record.address).equalsIgnoreCase(address)) continue;

    found = true;
    addSignalObservation(summary.signal, record.rssi, record.uptimeMs);
    summary.addressType = record.addressType;

    if (record.named && String(record.name).length() > 0)
      newestName = String(record.name);
  }

  if (!found) return false;

  address.toCharArray(summary.address, sizeof(summary.address));

  if (newestName.length() > 0) {
    newestName.toCharArray(summary.name, sizeof(summary.name));
    summary.named = true;
  }

  return true;
}

int performLoggedBLEScan() {
  if (!bleSurveyEnabled) {
    bleStatusMessage =
      "Bluetooth Survey is disabled; BLE stack is not initialized.";
    return -1;
  }

  initializeBLEScanner();

  BLEScan* scan = BLEDevice::getScan();
  bleStatusMessage = "BLE scan in progress...";

  startScanLed(BLE_SCAN_LED_PERIOD_TICKS);
  BLEScanResults* results =
      scan->start(BLE_SCAN_DURATION_SECONDS, false);
  stopScanLed();

  bleScanCounter++;
  lastBleScanUptimeMs = millis();

  int resultCount =
      results != nullptr ? results->getCount() : 0;

  if (results != nullptr) {
    for (int i = 0; i < resultCount; i++) {
      BLEAdvertisedDevice device = results->getDevice(i);

      BleScanRecord record = {};
      record.scanNumber = bleScanCounter;
      record.uptimeMs = lastBleScanUptimeMs;
      record.rssi = device.getRSSI();
      record.addressType = device.getAddressType();

      String address = device.getAddress().toString();
      address.toCharArray(record.address, sizeof(record.address));

      if (device.haveName()) {
        String name = device.getName();
        if (name.length() > 0) {
          name.toCharArray(record.name, sizeof(record.name));
          record.named = true;
        }
      }

      appendBleScanRecord(record);
    }
  }

  scan->clearResults();

  bleStatusMessage =
    "BLE scan #" + String(bleScanCounter) +
    " complete: " + String(resultCount) +
    " device(s) observed.";

  return resultCount;
}

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

void saveCredentials(const String& ssid, const String& password) {
  preferences.begin("wifi", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();

  Serial.println("Wi-Fi credentials saved.");
}

bool loadCredentials(String& ssid, String& password) {
  preferences.begin("wifi", true);

  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");

  preferences.end();

  return ssid.length() > 0;
}

void eraseCredentials() {
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();

  Serial.println("Saved Wi-Fi credentials erased.");
}


// ============================================================
// Uptime
// ============================================================

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

String observationAgeLabel(uint32_t observationMs) {
  uint32_t now = millis();
  if (now < observationMs) return "counter wrapped";
  return formatUptime(now - observationMs) + " ago";
}


// ============================================================
// Wi-Fi connection
// ============================================================

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


// ============================================================
// Informational Wi-Fi scan
// ============================================================

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
  Serial.println("Use '3' or 'wifi' to configure a network.");

  WiFi.scanDelete();
}


// ============================================================
// Interactive Wi-Fi configuration
// ============================================================

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

String firmwareBuildTimestamp() {
  return String(__DATE__) + " " + String(__TIME__);
}

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

String htmlEscape(const String& input) {
  String output = input;
  output.replace("&", "&amp;");
  output.replace("<", "&lt;");
  output.replace(">", "&gt;");
  output.replace("\"", "&quot;");
  output.replace("'", "&#39;");
  return output;
}

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

String latestSSIDForBSSID(const String& bssid) {
  for (size_t offset = 0; offset < historyCount; offset++) {
    size_t logicalIndex = historyCount - 1 - offset;
    const ScanRecord& record = historyRecord(logicalIndex);

    if (String(record.bssid).equalsIgnoreCase(bssid)) {
      if (record.hidden) {
        return "(hidden)";
      }

      return String(record.ssid);
    }
  }

  return "";
}

bool historyContainsBSSID(const String& bssid) {
  if (bssid.length() == 0) {
    return false;
  }

  for (size_t i = 0; i < historyCount; i++) {
    if (String(historyRecord(i).bssid).equalsIgnoreCase(bssid)) {
      return true;
    }
  }

  return false;
}

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
    display: inline-block;
    margin: 20px 6px 0 6px;
    padding: 12px 20px;
    background: var(--button-bg);
    color: var(--button-text);
    text-decoration: none;
    border-radius: 6px;
  }

  .buttons {
    text-align: center;
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

  .site-header {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    margin-bottom: 14px;
  }

  .site-title {
    font-weight: bold;
    font-size: 1.05em;
  }

  .nav {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
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

  .status-pass { font-weight: bold; }
  .status-warn { font-weight: bold; }
  .status-fail { font-weight: bold; }

  .theme-control select {
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

String csvEscape(const String& input) {
  String output = input;
  output.replace("\"", "\"\"");
  return "\"" + output + "\"";
}

void redirectToScanPage() {
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

void handleWifiScanStatus() {
  String json = "{\"scan\":" + String(scanCounter) +
                ",\"records\":" + String(historyCount) + "}";
  server.send(200, "application/json", json);
}

void handleWebScanNow() {
  performLoggedScan();
  WiFi.scanDelete();
  redirectToScanPage();
}

void handleScanSettings() {
  if (server.hasArg("interval")) {
    long requested = server.arg("interval").toInt();

    if (requested < (long)MIN_SCAN_INTERVAL_SECONDS) {
      requested = MIN_SCAN_INTERVAL_SECONDS;
    }

    if (requested > (long)MAX_SCAN_INTERVAL_SECONDS) {
      requested = MAX_SCAN_INTERVAL_SECONDS;
    }

    scanIntervalSeconds = (unsigned long)requested;
  }

  if (server.hasArg("history")) {
    long requestedHistory = server.arg("history").toInt();
    if (requestedHistory < (long)MIN_SCAN_HISTORY_RECORDS)
      requestedHistory = MIN_SCAN_HISTORY_RECORDS;
    if (requestedHistory > (long)scanHistoryCapacity)
      requestedHistory = (long)scanHistoryCapacity;
    setWifiRetentionLimit((size_t)requestedHistory);
  }

  autoScanEnabled = server.hasArg("auto");
  lastAutoScanMs = millis();

  redirectToScanPage();
}

void handleClearScanHistory() {
  clearScanHistory();
  redirectToScanPage();
}

void handleScanCsv() {
  server.sendHeader(
    "Content-Disposition",
    "attachment; filename=\"wifi_scan_log.csv\""
  );

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");

  server.sendContent(
    "scan,uptime_ms,uptime,ssid,bssid,channel,rssi_dbm,security,connected,hidden\r\n"
  );

  for (size_t i = 0; i < historyCount; i++) {
    const ScanRecord& record = historyRecord(i);

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
    line += record.connected ? "YES" : "NO";
    line += ",";
    line += record.hidden ? "YES" : "NO";
    line += "\r\n";

    server.sendContent(line);
  }

  server.sendContent("");
}

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

  uint32_t firstMs = 0;
  uint32_t lastMs = 0;
  size_t pointCount = 0;

  for (size_t i = 0; i < historyCount; i++) {
    const ScanRecord& record = historyRecord(i);

    if (!String(record.bssid).equalsIgnoreCase(selectedBssid)) {
      continue;
    }

    if (pointCount == 0) {
      firstMs = record.uptimeMs;
    }

    lastMs = record.uptimeMs;
    pointCount++;
  }

  if (pointCount == 0) {
    server.sendContent(
      "<p>No logged RSSI samples are available for the selected BSSID.</p>"
    );
    return;
  }

  if (lastMs <= firstMs) {
    lastMs = firstMs + 1;
  }

  server.sendContent(
    "<div class=\"plot-wrap\">"
    "<svg viewBox=\"0 0 720 280\" role=\"img\" "
    "aria-label=\"RSSI history for selected access point\">"
  );

  // Background and horizontal grid lines.
  server.sendContent(
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

    server.sendContent(grid);
  }

  // Polyline points.
  String points;
  points.reserve(pointCount * 14);

  for (size_t i = 0; i < historyCount; i++) {
    const ScanRecord& record = historyRecord(i);

    if (!String(record.bssid).equalsIgnoreCase(selectedBssid)) {
      continue;
    }

    int x = LEFT +
      (uint64_t)(record.uptimeMs - firstMs) * plotWidth /
      (lastMs - firstMs);

    int clippedRssi = record.rssi;

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

  server.sendContent(polyline);

  // Individual samples.
  for (size_t i = 0; i < historyCount; i++) {
    const ScanRecord& record = historyRecord(i);

    if (!String(record.bssid).equalsIgnoreCase(selectedBssid)) {
      continue;
    }

    int x = LEFT +
      (uint64_t)(record.uptimeMs - firstMs) * plotWidth /
      (lastMs - firstMs);

    int clippedRssi = record.rssi;

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
    dot += String(record.scanNumber);
    dot += " | ";
    dot += htmlEscape(formatUptime(record.uptimeMs));
    dot += " | ";
    dot += String(record.rssi);
    dot += " dBm</title></circle>";

    server.sendContent(dot);
  }

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

  server.sendContent(labels);
  server.sendContent("</svg></div>");
}


bool hasNewerObservationForBSSID(
  size_t logicalIndex,
  const String& bssid
) {
  for (
    size_t i = logicalIndex + 1;
    i < historyCount;
    i++
  ) {
    if (
      String(historyRecord(i).bssid)
        .equalsIgnoreCase(bssid)
    ) {
      return true;
    }
  }

  return false;
}

bool buildNetworkSummary(
  const String& bssid,
  NetworkSummary& summary
) {
  summary = {};
  resetSignalStats(summary.signal);

  bool found = false;
  String newestKnownSSID = "";

  // Walk oldest -> newest so SignalStats.latestRssi and lastSeenMs naturally
  // become the newest retained values.
  for (size_t i = 0; i < historyCount; i++) {
    const ScanRecord& record = historyRecord(i);

    if (
      !String(record.bssid)
        .equalsIgnoreCase(bssid)
    ) {
      continue;
    }

    found = true;

    addSignalObservation(
      summary.signal,
      record.rssi,
      record.uptimeMs
    );

    // Channel and security use the newest retained observation.
    summary.channel = record.channel;
    summary.authMode = record.authMode;

    // Preserve the newest known non-hidden SSID for this BSSID.
    if (
      !record.hidden &&
      String(record.ssid).length() > 0
    ) {
      newestKnownSSID = String(record.ssid);
    }
  }

  if (!found) {
    return false;
  }

  bssid.toCharArray(
    summary.bssid,
    sizeof(summary.bssid)
  );

  if (newestKnownSSID.length() > 0) {
    newestKnownSSID.toCharArray(
      summary.ssid,
      sizeof(summary.ssid)
    );

    summary.hidden = false;
  } else {
    summary.ssid[0] = '\0';
    summary.hidden = true;
  }

  // "Connected" is a live state, not a historical property. This avoids
  // highlighting an AP simply because it was connected during an older scan.
  summary.connected =
      WiFi.status() == WL_CONNECTED &&
      WiFi.BSSIDstr().equalsIgnoreCase(bssid);

  return true;
}

void sendNetworkSummaryTable() {
  if (historyCount == 0) {
    server.sendContent(
      "<p>No networks have been observed yet.</p>"
    );
    return;
  }

  server.sendContent(
    "<div class=\"table-scroll\"><table id=\"network-summary\">"
    "<thead><tr>"
    "<th class=\"sortable\" onclick=\"sortTable('network-summary',0,'text')\">SSID</th>"
    "<th class=\"sortable\" onclick=\"sortTable('network-summary',1,'text')\">BSSID</th>"
    "<th class=\"sortable signal\" onclick=\"sortTable('network-summary',2,'number')\">CH</th>"
    "<th class=\"sortable signal\" onclick=\"sortTable('network-summary',3,'number')\">Latest</th>"
    "<th class=\"sortable signal\" onclick=\"sortTable('network-summary',4,'number')\">Min</th>"
    "<th class=\"sortable signal\" onclick=\"sortTable('network-summary',5,'number')\">Max</th>"
    "<th class=\"sortable signal\" onclick=\"sortTable('network-summary',6,'number')\">Avg</th>"
    "<th class=\"sortable signal\" onclick=\"sortTable('network-summary',7,'number')\">Samples</th>"
    "<th class=\"sortable\" onclick=\"sortTable('network-summary',8,'text')\">Security</th>"
    "<th class=\"sortable\" onclick=\"sortTable('network-summary',9,'number')\">First Seen</th>"
    "<th class=\"sortable\" onclick=\"sortTable('network-summary',10,'number')\">Last Seen</th>"
    "</tr></thead><tbody>"
  );

  // Iterate newest -> oldest. Only the newest retained observation for each
  // BSSID emits a row. This eliminates the large temporary NetworkSummary
  // array used previously and keeps page generation memory-bounded.
  for (
    size_t offset = 0;
    offset < historyCount;
    offset++
  ) {
    size_t logicalIndex =
        historyCount - 1 - offset;

    const ScanRecord& latestRecord =
        historyRecord(logicalIndex);

    String bssid =
        String(latestRecord.bssid);

    if (
      hasNewerObservationForBSSID(
        logicalIndex,
        bssid
      )
    ) {
      continue;
    }

    NetworkSummary summary = {};

    if (!buildNetworkSummary(bssid, summary)) {
      continue;
    }

    String plotUrl =
        "/scan?plot=" +
        urlEncode(String(summary.bssid)) +
        "#rssi-plot";

    String displaySSID =
        summary.hidden
          ? "(hidden)"
          : String(summary.ssid);

    float avgRssi =
        averageSignal(summary.signal);

    String row;
    row.reserve(900);

    if (summary.connected) {
      row += "<tr class=\"current\">";
    } else {
      row += "<tr>";
    }

    row += "<td class=\"address\"><a href=\"";
    row += plotUrl;
    row += "\">";
    row += htmlEscape(displaySSID);

    if (summary.connected) {
      row += " (connected)";
    }

    row += "</a></td>";

    row += "<td><a href=\"";
    row += plotUrl;
    row += "\">";
    row += htmlEscape(String(summary.bssid));
    row += "</a></td>";

    row += "<td class=\"signal\" data-sort=\"";
    row += String(summary.channel);
    row += "\">";
    row += String(summary.channel);
    row += "</td>";

    row += "<td class=\"signal\" data-sort=\"";
    row += String(summary.signal.latestRssi);
    row += "\">";
    row += String(summary.signal.latestRssi);
    row += " dBm</td>";

    row += "<td class=\"signal\" data-sort=\"";
    row += String(summary.signal.minRssi);
    row += "\">";
    row += String(summary.signal.minRssi);
    row += " dBm</td>";

    row += "<td class=\"signal\" data-sort=\"";
    row += String(summary.signal.maxRssi);
    row += "\">";
    row += String(summary.signal.maxRssi);
    row += " dBm</td>";

    row += "<td class=\"signal\" data-sort=\"";
    row += String(avgRssi, 1);
    row += "\">";
    row += String(avgRssi, 1);
    row += " dBm</td>";

    row += "<td class=\"signal\" data-sort=\"";
    row += String(summary.signal.samples);
    row += "\">";
    row += String(summary.signal.samples);
    row += "</td>";

    row += "<td>";
    row += htmlEscape(
      securityLabel(
        (wifi_auth_mode_t)summary.authMode
      )
    );
    row += "</td>";

    // Sort by age (smaller = more recent) while displaying relative time.
    uint32_t nowMs = millis();
    uint32_t firstAgeMs = nowMs >= summary.signal.firstSeenMs
        ? nowMs - summary.signal.firstSeenMs : 0;
    uint32_t lastAgeMs = nowMs >= summary.signal.lastSeenMs
        ? nowMs - summary.signal.lastSeenMs : 0;

    row += "<td data-sort=\"";
    row += String(firstAgeMs);
    row += "\">";
    row += htmlEscape(observationAgeLabel(summary.signal.firstSeenMs));
    row += "</td>";

    row += "<td data-sort=\"";
    row += String(lastAgeMs);
    row += "\">";
    row += htmlEscape(observationAgeLabel(summary.signal.lastSeenMs));
    row += "</td>";

    row += "</tr>";

    server.sendContent(row);
  }

  server.sendContent("</tbody></table></div>");
}

void sendThemeControl();

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
      "}catch(e){}"
    "})();"
    "</script>";
}

void sendThemeBootstrapScript() {
  server.sendContent(themeBootstrapScript());
}

String activeNavClass(const String& active, const char* item) {
  return active == item ? " class=\"active\"" : "";
}

void sendSiteNavigation(const String& active) {
  String nav;
  nav.reserve(900);
  nav += "<div class=\"site-header\"><div class=\"site-title\">ESP32 Wireless Surveyor</div><nav class=\"nav\">";
  nav += "<a href=\"/\"" + activeNavClass(active, "wifi") + ">Wi-Fi</a>";
  nav += "<a href=\"/ble\"" + activeNavClass(active, "ble") + ">Bluetooth</a>";
  nav += "<a href=\"/system\"" + activeNavClass(active, "system") + ">System</a>";
  nav += "<a href=\"/settings\"" + activeNavClass(active, "settings") + ">Settings</a>";
  nav += "</nav></div>";
  server.sendContent(nav);
  sendThemeControl();
}

void sendThemeControl() {
  server.sendContent(
    "<div class=\"theme-control\">"
    "<label for=\"theme-select\">Theme</label>"
    "<select id=\"theme-select\" class=\"theme-select\" onchange=\"setTheme(this.value)\">"
    "<option value=\"system\">System</option>"
    "<option value=\"light\">Light</option>"
    "<option value=\"dark\">Dark</option>"
    "</select></div>"
  );
}

void sendThemeScript() {
  server.sendContent(
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
    "document.addEventListener('DOMContentLoaded',()=>{"
      "const v=localStorage.getItem('esp32-theme')||'system';"
      "document.querySelectorAll('.theme-select').forEach(s=>s.value=v);"
      "if(window.matchMedia){"
        "window.matchMedia('(prefers-color-scheme: dark)').addEventListener?.('change',()=>{"
          "if((localStorage.getItem('esp32-theme')||'system')==='system')applyTheme('system');"
        "});"
      "}"
    "});"
    "</script>"
  );
}

void sendSortableTableScript() {
  server.sendContent(
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

float rssiInterferenceWeight(int rssi) {
  if (rssi <= -100) return 0.0f;
  if (rssi >= -30) return 100.0f;
  return (float)(rssi + 100) * (100.0f / 70.0f);
}

float channelOverlapFactor(int distance) {
  if (distance == 0) return 1.0f;
  if (distance == 1) return 0.75f;
  if (distance == 2) return 0.50f;
  if (distance == 3) return 0.25f;
  if (distance == 4) return 0.10f;
  return 0.0f;
}

ChannelAnalysis analyzeLatestWifiScan() {
  ChannelAnalysis a = {};
  a.suggestedChannel = 0;
  for (int ch = 1; ch <= 11; ch++) a.strongestRssi[ch] = -127;
  if (historyCount == 0 || scanHistory == nullptr) return a;

  uint32_t latestScan = historyRecord(historyCount - 1).scanNumber;
  a.scanNumber = latestScan;
  for (size_t i = 0; i < historyCount; i++) {
    const ScanRecord& r = historyRecord(i);
    if (r.scanNumber != latestScan || r.channel < 1 || r.channel > 11) continue;
    a.valid = true;
    a.apCount[r.channel]++;
    if (r.rssi > a.strongestRssi[r.channel]) a.strongestRssi[r.channel] = r.rssi;
    float w = rssiInterferenceWeight(r.rssi);
    for (int candidate = 1; candidate <= 11; candidate++) {
      int d = abs(candidate - r.channel);
      float overlap = channelOverlapFactor(d);
      if (overlap <= 0.0f) continue;
      if (d == 0) a.coChannelScore[candidate] += w;
      else a.adjacentScore[candidate] += w * overlap;
    }
  }
  if (!a.valid) return a;
  for (int ch = 1; ch <= 11; ch++) a.totalScore[ch] = a.coChannelScore[ch] + a.adjacentScore[ch];
  const int preferred[3] = {1, 6, 11};
  int best = preferred[0];
  for (int i = 1; i < 3; i++) if (a.totalScore[preferred[i]] < a.totalScore[best]) best = preferred[i];
  a.suggestedChannel = best;
  return a;
}

void sendWifiChannelAnalysis() {
  ChannelAnalysis a = analyzeLatestWifiScan();
  server.sendContent("<div class=\"card\"><h2>2.4 GHz Channel Analysis</h2>");
  if (!a.valid) {
    server.sendContent("<p>No scan data is available yet.</p><div class=\"note\">This is an advisory interference estimate, not measured airtime utilization.</div></div>");
    return;
  }
  String s;
  s.reserve(2600);
  s += "<div class=\"row\"><span class=\"label\">Based On</span><span class=\"value\">Latest scan #" + String(a.scanNumber) + "</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Suggested channel</span><span class=\"value\"><strong>" + String(a.suggestedChannel) + "</strong></span></div>";
  s += "<div class=\"note\">Suggestion compares channels 1, 6, and 11 using visible AP strength plus co-channel and adjacent-channel overlap. Lower score is preferred. This does not measure actual airtime utilization or non-Wi-Fi interference.</div>";
  s += "<div class=\"table-scroll\"><table><thead><tr><th>Channel</th><th>APs</th><th>Strongest AP</th><th>Co-channel</th><th>Adjacent</th><th>Total score</th></tr></thead><tbody>";
  for (int ch = 1; ch <= 11; ch++) {
    s += "<tr";
    if (ch == a.suggestedChannel) s += " class=\"current\"";
    s += "><td>" + String(ch) + "</td><td>" + String(a.apCount[ch]) + "</td><td>";
    s += a.strongestRssi[ch] == -127 ? "-" : String(a.strongestRssi[ch]) + " dBm";
    s += "</td><td>" + String(a.coChannelScore[ch], 1) + "</td><td>" + String(a.adjacentScore[ch], 1) + "</td><td>" + String(a.totalScore[ch], 1) + "</td></tr>";
  }
  s += "</tbody></table></div></div>";
  server.sendContent(s);
}

void handleWebScan() {
  ensureWiFiStationMode();

  bool connected = WiFi.status() == WL_CONNECTED;
  String connectedSSID = connected ? WiFi.SSID() : "";
  String connectedBSSID = connected ? WiFi.BSSIDstr() : "";
  int connectedRSSI = connected ? WiFi.RSSI() : 0;

  String selectedBSSID = "";

  if (server.hasArg("plot")) {
    String requestedBSSID = server.arg("plot");
    requestedBSSID.trim();

    if (historyContainsBSSID(requestedBSSID)) {
      selectedBSSID = requestedBSSID;
    }
  }

  // Default to the currently connected infrastructure AP when its BSSID
  // is present in scan history.
  if (
    selectedBSSID.length() == 0 &&
    connected &&
    historyContainsBSSID(connectedBSSID)
  ) {
    selectedBSSID = connectedBSSID;
  }

  // In AP-only operation, or before the connected AP has appeared in a
  // logged scan, default to the newest retained network observation.
  if (
    selectedBSSID.length() == 0 &&
    historyCount > 0
  ) {
    selectedBSSID =
        String(historyRecord(historyCount - 1).bssid);
  }

  String selectedSSID =
      latestSSIDForBSSID(selectedBSSID);

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  server.sendContent(
    "<!DOCTYPE html><html><head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>ESP32 Wi-Fi Survey</title>"
  );

  sendThemeBootstrapScript();
  server.sendContent(pageStyles());

  server.sendContent(
    "</head><body><div class=\"container\">"
  );

  sendSiteNavigation("wifi");

  server.sendContent(
    "<h1>Wi-Fi Survey</h1>"
  );

  if (connected) {
    String connectedCard;
    connectedCard.reserve(550);

    connectedCard +=
      "<div class=\"card\">"
      "<div class=\"row\"><span class=\"label\">Connected SSID</span>"
      "<span class=\"value\">";
    connectedCard += htmlEscape(connectedSSID);
    connectedCard +=
      "</span></div>"
      "<div class=\"row\"><span class=\"label\">Connected BSSID</span>"
      "<span class=\"value\">";
    connectedCard += htmlEscape(connectedBSSID);
    connectedCard +=
      "</span></div>"
      "<div class=\"row\"><span class=\"label\">Current Signal</span>"
      "<span class=\"value\">";
    connectedCard += String(connectedRSSI);
    connectedCard +=
      " dBm</span></div>"
      "</div>";

    server.sendContent(connectedCard);
  }

  String loggingCard;
  loggingCard.reserve(2200);

  loggingCard +=
    "<div class=\"card\"><h2>Scan Logging</h2>"
    "<div class=\"row\"><span class=\"label\">Automatic Scanning</span>"
    "<span class=\"value\">";
  loggingCard += autoScanEnabled ? "ON" : "OFF";
  loggingCard +=
    "</span></div>"
    "<div class=\"row\"><span class=\"label\">Scan Interval</span>"
    "<span class=\"value\">";
  loggingCard += String(scanIntervalSeconds);
  loggingCard +=
    " seconds</span></div>"
    "<div class=\"row\"><span class=\"label\">Scans This Session</span>"
    "<span class=\"value\">";
  loggingCard += String(scanCounter);
  loggingCard +=
    "</span></div>"
    "<div class=\"row\"><span class=\"label\">Stored Observations</span>"
    "<span class=\"value\">";
  loggingCard += String(historyCount);
  loggingCard += " / ";
  loggingCard += String(scanHistoryRetentionLimit);
  loggingCard += " retained / ";
  loggingCard += String(scanHistoryCapacity);
  loggingCard += " allocated capacity";
  loggingCard +=
    "</span></div>"
    "<div class=\"row\"><span class=\"label\">Scan Groups Retained</span>"
    "<span class=\"value\">";
  loggingCard += String(countRetainedScanGroups());
  loggingCard += "</span></div>";

  if (historyCount > 0) {
    const ScanRecord& oldestRetained = historyRecord(0);
    const ScanRecord& newestRetained = historyRecord(historyCount - 1);
    loggingCard +=
      "<div class=\"row\"><span class=\"label\">Oldest Record Age</span>"
      "<span class=\"value\">" +
      htmlEscape(observationAgeLabel(oldestRetained.uptimeMs)) +
      "</span></div>"
      "<div class=\"row\"><span class=\"label\">Retained Time Window</span>"
      "<span class=\"value\">" +
      htmlEscape(retainedWindowLabel(oldestRetained.uptimeMs, newestRetained.uptimeMs)) +
      "</span></div>";
  }

  loggingCard +=
    "<div class=\"row\"><span class=\"label\">History RAM</span>"
    "<span class=\"value\">";
  loggingCard += String(wifiHistoryAllocatedBytes() / 1024.0, 1);
  loggingCard +=
    " KB total</span></div>"
    "<div class=\"row\"><span class=\"label\">Observation Storage</span><span class=\"value\">";
  loggingCard += String((scanHistoryCapacity * sizeof(WifiObservation)) / 1024.0, 1);
  loggingCard += " KB; ";
  loggingCard += String(sizeof(WifiObservation));
  loggingCard += " bytes/observation</span></div>"
    "<div class=\"row\"><span class=\"label\">AP Table</span><span class=\"value\">";
  loggingCard += String(wifiApCount);
  loggingCard += " / ";
  loggingCard += String(wifiApTableCapacity);
  loggingCard += " APs; ";
  loggingCard += String((wifiApTableCapacity * sizeof(WifiApEntry)) / 1024.0, 1);
  loggingCard += " KB allocated</span></div>"
    "<div class=\"row\"><span class=\"label\">Scan Metadata</span><span class=\"value\">";
  loggingCard += String(wifiScanMetadataCapacity);
  loggingCard += " slots; ";
  loggingCard += String((wifiScanMetadataCapacity * sizeof(WifiScanMetadata)) / 1024.0, 1);
  loggingCard += " KB allocated</span></div>"
    "<div class=\"row\"><span class=\"label\">Free Heap</span>"
    "<span class=\"value\">";
  loggingCard += String(ESP.getFreeHeap() / 1024.0, 1);
  loggingCard +=
    " KB</span></div>"
    "<div class=\"row\"><span class=\"label\">Largest Free Block</span>"
    "<span class=\"value\">";
  loggingCard += String(
    heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024.0,
    1
  );
  loggingCard +=
    " KB</span></div>"
    "<div class=\"row\"><span class=\"label\">Last Scan</span>"
    "<span class=\"value\">";

  if (scanCounter == 0) {
    loggingCard += "Never";
  } else {
    loggingCard += htmlEscape(formatUptime(lastScanUptimeMs));
    loggingCard += " uptime";
  }

  loggingCard +=
    "</span></div>"
    "<form class=\"settings-row\" action=\"/scan-settings\" method=\"get\">"
    "<div class=\"control\"><label for=\"interval\">Interval (seconds)</label>"
    "<input id=\"interval\" name=\"interval\" type=\"number\" min=\"5\" max=\"3600\" value=\"";
  loggingCard += String(scanIntervalSeconds);
  loggingCard +=
    "\"></div>"
    "<div class=\"control\"><label for=\"history\">Retention limit (observations)</label>"
    "<input id=\"history\" name=\"history\" type=\"number\" min=\"50\" max=\"";
  loggingCard += String(scanHistoryCapacity);
  loggingCard += "\" value=\"";
  loggingCard += String(scanHistoryRetentionLimit);
  loggingCard +=
    "\"></div>"
    "<div class=\"checkbox-stack\">"
    "<label><input type=\"checkbox\" name=\"auto\" value=\"1\"";
  if (autoScanEnabled) loggingCard += " checked";
  loggingCard +=
    "> Automatic scanning</label></div>"
    "<button type=\"submit\">Apply</button></form>"
    "<div class=\"buttons\">"
    "<a class=\"button\" href=\"/scan-now\">Scan Now</a>"
    "<a class=\"button\" href=\"/scanlog.csv\">Download CSV</a>"
    "<a class=\"button\" href=\"/scan-clear\">Clear History</a>"
    "<a class=\"button\" href=\"/\">Refresh Page</a>"
    "</div>"
    "<div class=\"note\">"
    "Scan history is kept in RAM only and is cleared by reset or power cycle. "
    "V21 allocates the maximum safe compact Wi-Fi history once at boot. "
    "Changing the retention limit does not reallocate RAM; it only changes how many "
    "of the allocated observation slots may be retained. When the logical limit is "
    "full, the oldest observations are discarded."
    "</div>";

  if (wifiApTableFullDrops > 0) {
    loggingCard += "<div class=\"note\"><strong>Warning:</strong> ";
    loggingCard += String(wifiApTableFullDrops);
    loggingCard += " observation(s) were not logged because the unique AP table was full.</div>";
  }

  if (historyResizeMessage.length() > 0) {
    loggingCard += "<div class=\"note\"><strong>";
    loggingCard += htmlEscape(historyResizeMessage);
    loggingCard += "</strong></div>";
  }

  loggingCard += "</div>";

  server.sendContent(loggingCard);
  sendWifiChannelAnalysis();

  server.sendContent(
    "<div class=\"card\" id=\"rssi-plot\"><h2>RSSI History</h2>"
  );

  if (selectedBSSID.length() > 0) {
    String plotInfo;
    plotInfo.reserve(500);

    plotInfo +=
      "<div class=\"row\"><span class=\"label\">Selected Network</span>"
      "<span class=\"value\">";
    plotInfo += htmlEscape(selectedSSID);
    plotInfo +=
      "</span></div>"
      "<div class=\"row\"><span class=\"label\">Selected BSSID</span>"
      "<span class=\"value\">";
    plotInfo += htmlEscape(selectedBSSID);
    plotInfo += "</span></div>";

    if (
      connected &&
      selectedBSSID.equalsIgnoreCase(connectedBSSID)
    ) {
      plotInfo +=
        "<div class=\"row\"><span class=\"label\">Current Connection</span>"
        "<span class=\"value\">Yes</span></div>";
    }

    server.sendContent(plotInfo);

    sendRssiHistoryPlot(selectedBSSID);

    server.sendContent(
      "<div class=\"note\">"
      "Click any SSID or BSSID in the scan-history tables below to redraw "
      "this plot using that access point's logged data. The default selection "
      "is the currently connected infrastructure AP when available. "
      "Each point is one logged scan observation; hover a point for scan "
      "number, uptime, and RSSI."
      "</div>"
    );
  } else {
    server.sendContent(
      "<p>No logged networks are available to plot yet.</p>"
    );
  }

  server.sendContent("</div>");

  server.sendContent(
    "<div class=\"card\"><h2>Observed Networks</h2>"
    "<div class=\"note\">"
    "One row is shown for each BSSID observed during this session. "
    "Click any column header to sort the table. Click an SSID or BSSID "
    "to redraw the RSSI plot for that access point."
    "</div>"
  );

  sendNetworkSummaryTable();

  server.sendContent("</div>");

  server.sendContent(
    "<div class=\"footer\">ESP32 Web Interface</div>"
  );

  sendSortableTableScript();
  sendThemeScript();

  // Poll only for scan sequence changes. Suppress reload while the user is
  // interacting with a form or after a form value has been edited.
  String refreshScript =
    "<script>(function(){"
    "let scan=" + String(scanCounter) + ";"
    "let dirty=false;"
    "document.querySelectorAll('.settings-row input,.settings-row select,.settings-row textarea').forEach(function(e){"
      "e.addEventListener('input',function(){dirty=true;});"
      "e.addEventListener('change',function(){dirty=true;});"
    "});"
    "setInterval(function(){"
      "fetch('/api/wifi/status',{cache:'no-store'}).then(r=>r.json()).then(function(s){"
        "if(s.scan!==scan){"
          "scan=s.scan;"
          "var a=document.activeElement;"
          "var editing=a&&(a.tagName==='INPUT'||a.tagName==='SELECT'||a.tagName==='TEXTAREA');"
          "if(!dirty&&!editing)location.reload();"
        "}"
      "}).catch(function(){});"
    "},2500);"
    "})();</script>";
  server.sendContent(refreshScript);

  server.sendContent(
    "</div></body></html>"
  );

  server.sendContent("");
}


// ============================================================
// BLE web survey
// ============================================================

void redirectToBLEPage() {
  server.sendHeader("Location", "/ble");
  server.send(303, "text/plain", "");
}

void handleBLESettings() {
  if (!bleSurveyEnabled) {
    bleStatusMessage =
      "BLE settings are unavailable because BLE is disabled at boot.";
    redirectToBLEPage();
    return;
  }

  if (server.hasArg("interval")) {
    long requested = server.arg("interval").toInt();
    if (requested < (long)MIN_SCAN_INTERVAL_SECONDS) requested = MIN_SCAN_INTERVAL_SECONDS;
    if (requested > (long)MAX_SCAN_INTERVAL_SECONDS) requested = MAX_SCAN_INTERVAL_SECONDS;
    bleScanIntervalSeconds = (unsigned long)requested;
  }

  if (server.hasArg("history")) {
    long requested = server.arg("history").toInt();
    if (requested < (long)MIN_BLE_HISTORY_RECORDS) requested = MIN_BLE_HISTORY_RECORDS;
    if (requested > (long)MAX_BLE_HISTORY_RECORDS) requested = MAX_BLE_HISTORY_RECORDS;

    bool clearBeforeResize = server.hasArg("clear_resize");

    if (clearBeforeResize && (size_t)requested != bleHistoryCapacity)
      clearAndResizeBleHistory((size_t)requested);
    else
      resizeBleHistory((size_t)requested, true);
  }

  autoBleScanEnabled = server.hasArg("auto");
  lastAutoBleScanMs = millis();
  redirectToBLEPage();
}

void handleClearBLEHistory() {
  clearBleHistory();
  redirectToBLEPage();
}

void handleBLEScanCsv() {
  server.sendHeader("Content-Disposition", "attachment; filename=\"ble_scan_log.csv\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  server.sendContent("scan,uptime_ms,uptime,name,address,address_type,rssi_dbm\r\n");

  for (size_t i = 0; i < bleHistoryCount; i++) {
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
    server.sendContent(line);
  }

  server.sendContent("");
}

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
    server.sendContent("<p>No retained samples for this BLE device.</p>");
    return;
  }

  if (lastMs <= firstMs) lastMs = firstMs + 1;

  server.sendContent("<div class=\"plot-wrap\"><svg viewBox=\"0 0 720 280\" role=\"img\">"
    "<rect class=\"plot-bg\" x=\"58\" y=\"20\" width=\"642\" height=\"215\"/>");

  for (int rssi = -100; rssi <= -30; rssi += 10) {
    int y = TOP + ((RSSI_TOP - rssi) * plotHeight) / (RSSI_TOP - RSSI_BOTTOM);
    String grid = "<line x1=\"" + String(LEFT) + "\" y1=\"" + String(y) +
      "\" x2=\"" + String(SVG_WIDTH - RIGHT) + "\" y2=\"" + String(y) +
      "\" class=\"plot-grid\"/><text class=\"plot-text\" x=\"" + String(LEFT - 8) + "\" y=\"" +
      String(y + 4) + "\" class=\"plot-text\" text-anchor=\"end\" font-size=\"11\">" +
      String(rssi) + "</text>";
    server.sendContent(grid);
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

  server.sendContent("<polyline class=\"plot-line\" points=\"" + points + "\"/>");

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
    server.sendContent(dot);
  }

  server.sendContent("</svg></div>");
}

void sendBleSummaryTable() {
  if (bleHistoryCount == 0) {
    server.sendContent("<p>No BLE devices have been observed yet.</p>");
    return;
  }

  server.sendContent("<div class=\"table-scroll\"><table id=\"ble-summary\"><thead><tr>"
    "<th class=\"sortable\" onclick=\"sortTable('ble-summary',0,'text')\">Name</th>"
    "<th class=\"sortable\" onclick=\"sortTable('ble-summary',1,'text')\">Address</th>"
    "<th class=\"sortable\" onclick=\"sortTable('ble-summary',2,'text')\">Address Type</th>"
    "<th class=\"sortable signal\" onclick=\"sortTable('ble-summary',3,'number')\">Latest</th>"
    "<th class=\"sortable signal\" onclick=\"sortTable('ble-summary',4,'number')\">Min</th>"
    "<th class=\"sortable signal\" onclick=\"sortTable('ble-summary',5,'number')\">Max</th>"
    "<th class=\"sortable signal\" onclick=\"sortTable('ble-summary',6,'number')\">Avg</th>"
    "<th class=\"sortable signal\" onclick=\"sortTable('ble-summary',7,'number')\">Samples</th>"
    "<th class=\"sortable\" onclick=\"sortTable('ble-summary',8,'number')\">First Seen</th>"
    "<th class=\"sortable\" onclick=\"sortTable('ble-summary',9,'number')\">Last Seen</th>"
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

    String row;
    row.reserve(850);
    row += "<tr>";
    row += "<td><a href=\"" + plotUrl + "\">" + htmlEscape(displayName) + "</a></td>";
    row += "<td class=\"address\"><a href=\"" + plotUrl + "\">" + htmlEscape(address) + "</a></td>";
    row += "<td>" + htmlEscape(bleAddressTypeLabel(summary.addressType)) + "</td>";
    row += "<td class=\"signal\" data-sort=\"" + String(summary.signal.latestRssi) + "\">" + String(summary.signal.latestRssi) + " dBm</td>";
    row += "<td class=\"signal\" data-sort=\"" + String(summary.signal.minRssi) + "\">" + String(summary.signal.minRssi) + " dBm</td>";
    row += "<td class=\"signal\" data-sort=\"" + String(summary.signal.maxRssi) + "\">" + String(summary.signal.maxRssi) + " dBm</td>";
    row += "<td class=\"signal\" data-sort=\"" + String(avgRssi, 1) + "\">" + String(avgRssi, 1) + " dBm</td>";
    row += "<td class=\"signal\" data-sort=\"" + String(summary.signal.samples) + "\">" + String(summary.signal.samples) + "</td>";
    uint32_t bleNowMs = millis();
    uint32_t bleFirstAgeMs = bleNowMs >= summary.signal.firstSeenMs ? bleNowMs - summary.signal.firstSeenMs : 0;
    uint32_t bleLastAgeMs = bleNowMs >= summary.signal.lastSeenMs ? bleNowMs - summary.signal.lastSeenMs : 0;
    row += "<td data-sort=\"" + String(bleFirstAgeMs) + "\">" + htmlEscape(observationAgeLabel(summary.signal.firstSeenMs)) + "</td>";
    row += "<td data-sort=\"" + String(bleLastAgeMs) + "\">" + htmlEscape(observationAgeLabel(summary.signal.lastSeenMs)) + "</td>";
    row += "</tr>";
    server.sendContent(row);
  }

  server.sendContent("</tbody></table></div>");
}

void handleBLESurvey() {
  String selectedAddress = "";

  if (server.hasArg("plot")) {
    String requested = server.arg("plot");
    requested.trim();
    if (bleHistoryContainsAddress(requested)) selectedAddress = requested;
  }

  if (selectedAddress.length() == 0 && bleHistoryCount > 0)
    selectedAddress = String(bleHistoryRecord(bleHistoryCount - 1).address);

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent("<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>ESP32 Bluetooth Survey</title>");
  sendThemeBootstrapScript();
  server.sendContent(pageStyles());
  server.sendContent("</head><body><div class=\"container\">");
  sendSiteNavigation("ble");
  server.sendContent("<h1>Bluetooth Survey</h1>"
    "<div class=\"card\"><h2>Scan Logging</h2>");

  if (!bleSurveyEnabled) {
    server.sendContent(
      "<div class=\"note\"><strong>Bluetooth Survey is disabled.</strong> "
      "The BLE stack is not initialized, leaving substantially more RAM available for Wi-Fi survey history. "
      "Enabling Bluetooth saves the setting and restarts the ESP32 so memory can be allocated safely at boot.</div>"
      "<div class=\"row\"><span class=\"label\">BLE Stack</span><span class=\"value\">Not initialized</span></div>"
      "<div class=\"row\"><span class=\"label\">BLE History RAM</span><span class=\"value\">0 KB</span></div>"
      "<form class=\"controls\" action=\"/ble-mode\" method=\"post\">"
      "<input type=\"hidden\" name=\"enabled\" value=\"1\">"
      "<button type=\"submit\">Enable Bluetooth Survey</button></form>"
      "</div><div class=\"footer\">ESP32 Web Interface</div>");
    sendThemeScript();
    server.sendContent("</div></body></html>");
    server.sendContent("");
    return;
  }

  String status;
  status.reserve(2600);
  status += "<div class=\"row\"><span class=\"label\">Automatic Scanning</span><span class=\"value\">" +
    String(autoBleScanEnabled ? "ON" : "OFF") + "</span></div>";
  status += "<div class=\"row\"><span class=\"label\">Scan Interval</span><span class=\"value\">" +
    String(bleScanIntervalSeconds) + " seconds</span></div>";
  status += "<div class=\"row\"><span class=\"label\">Scans This Session</span><span class=\"value\">" +
    String(bleScanCounter) + "</span></div>";
  status += "<div class=\"row\"><span class=\"label\">Stored Records</span><span class=\"value\">" +
    String(bleHistoryCount) + " / " + String(bleHistoryCapacity) + "</span></div>";
  status += "<div class=\"row\"><span class=\"label\">History RAM</span><span class=\"value\">" +
    String((bleHistoryCapacity * sizeof(BleScanRecord)) / 1024.0, 1) + " KB</span></div>";
  status += "<div class=\"row\"><span class=\"label\">Last Scan</span><span class=\"value\">";
  status += bleScanCounter == 0 ? "Never" : formatUptime(lastBleScanUptimeMs) + " uptime";
  status += "</span></div>";

  status += "<form class=\"settings-row\" action=\"/ble-settings\" method=\"get\">"
    "<div class=\"control\"><label for=\"ble-interval\">Interval (seconds)</label>"
    "<input id=\"ble-interval\" name=\"interval\" type=\"number\" min=\"5\" max=\"3600\" value=\"" +
    String(bleScanIntervalSeconds) + "\"></div>"
    "<div class=\"control\"><label for=\"ble-history\">History limit (records)</label>"
    "<input id=\"ble-history\" name=\"history\" type=\"number\" min=\"50\" max=\"2000\" value=\"" +
    String(bleHistoryCapacity) + "\"></div>"
    "<div class=\"checkbox-stack\">"
    "<label><input type=\"checkbox\" name=\"clear_resize\" value=\"1\"> Clear history before resizing</label>"
    "<label><input type=\"checkbox\" name=\"auto\" value=\"1\"";

  if (autoBleScanEnabled) status += " checked";

  status += "> Automatic scanning</label></div><button type=\"submit\">Apply</button></form>"
    "<div class=\"buttons\"><a class=\"button\" href=\"/ble-scan\">Scan Now</a>"
    "<a class=\"button\" href=\"/blelog.csv\">Download CSV</a>"
    "<a class=\"button\" href=\"/ble-clear\">Clear History</a>"
    "<a class=\"button\" href=\"/ble\">Refresh Page</a></div>"
    "<div class=\"note\">Automatic BLE scanning defaults to 300 seconds. "
    "History capacity is auto-sized at boot from available heap.</div>";

  if (bleHistoryResizeMessage.length()) status += "<div class=\"note\"><strong>" + htmlEscape(bleHistoryResizeMessage) + "</strong></div>";
  if (bleStatusMessage.length()) status += "<div class=\"note\"><strong>" + htmlEscape(bleStatusMessage) + "</strong></div>";

  status += "</div>";
  server.sendContent(status);

  server.sendContent("<div class=\"card\" id=\"rssi-plot\"><h2>RSSI History</h2>");

  if (selectedAddress.length()) {
    String selectedName = latestBleNameForAddress(selectedAddress);
    server.sendContent("<div class=\"row\"><span class=\"label\">Selected Device</span><span class=\"value\">" +
      htmlEscape(selectedName) + "</span></div><div class=\"row\"><span class=\"label\">BLE Address</span><span class=\"value\">" +
      htmlEscape(selectedAddress) + "</span></div>");
    sendBleRssiHistoryPlot(selectedAddress);
    server.sendContent("<div class=\"note\">Click any device name or address below to redraw this plot.</div>");
  } else {
    server.sendContent("<p>No logged BLE devices are available to plot yet.</p>");
  }

  server.sendContent("</div><div class=\"card\"><h2>Observed BLE Devices</h2>"
    "<div class=\"note\">One row per retained BLE address. Click a column header to sort.</div>");
  sendBleSummaryTable();
  server.sendContent("</div><div class=\"footer\">ESP32 Web Interface</div>");
  sendSortableTableScript();
  sendThemeScript();
  server.sendContent("</div></body></html>");
  server.sendContent("");
}

void handleBLEScanNow() {
  if (!bleSurveyEnabled) {
    bleStatusMessage =
      "BLE scan not started because Bluetooth Survey is disabled in Settings.";
    redirectToBLEPage();
    return;
  }

  performLoggedBLEScan();
  redirectToBLEPage();
}


// ============================================================
// System status, diagnostics, and settings
// ============================================================

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

String wifiModeLabel(wifi_mode_t mode) {
  switch (mode) {
    case WIFI_MODE_NULL: return "Off";
    case WIFI_MODE_STA: return "STA";
    case WIFI_MODE_AP: return "AP";
    case WIFI_MODE_APSTA: return "AP+STA";
    default: return "Unknown";
  }
}

String selfTestRow(const String& name, const String& state, const String& detail) {
  String css = state == "PASS" ? "status-pass" : (state == "WARN" ? "status-warn" : "status-fail");
  return "<div class=\"row\"><span class=\"label\">" + htmlEscape(name) + "</span><span class=\"value \"" + css + "\">" + state + " - " + htmlEscape(detail) + "</span></div>";
}

void handleSystemStatus() {
  uint8_t primaryChannel = 0;
  wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
  esp_err_t channelResult = esp_wifi_get_channel(&primaryChannel, &secondary);
  size_t freeHeap = ESP.getFreeHeap();
  size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  float largestPct = freeHeap ? (100.0f * largestBlock / freeHeap) : 0.0f;
  esp_reset_reason_t rr = esp_reset_reason();

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent("<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>ESP32 System</title>");
  sendThemeBootstrapScript();
  server.sendContent(pageStyles());
  server.sendContent("</head><body><div class=\"container\">");
  sendSiteNavigation("system");
  server.sendContent("<h1>System</h1><div class=\"card\"><h2>Firmware & Hardware</h2>");

  String s;
  s.reserve(5200);
  s += "<div class=\"row\"><span class=\"label\">Firmware File</span><span class=\"value\">" + htmlEscape(FIRMWARE_FILE) + "</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Firmware Version</span><span class=\"value\">" + htmlEscape(FIRMWARE_VERSION) + "</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Build</span><span class=\"value\">" + htmlEscape(firmwareBuildTimestamp()) + "</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Arduino ESP32 Core</span><span class=\"value\">" + String(ESP_ARDUINO_VERSION_STR) + "</span></div>";
  s += "<div class=\"row\"><span class=\"label\">ESP-IDF</span><span class=\"value\">" + String(esp_get_idf_version()) + "</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Uptime</span><span class=\"value\">" + htmlEscape(getUptimeString()) + "</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Last Reset</span><span class=\"value\">" + htmlEscape(resetReasonLabel(rr)) + "</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Chip</span><span class=\"value\">" + String(ESP.getChipModel()) + ", rev " + String(ESP.getChipRevision()) + "</span></div>";
  s += "<div class=\"row\"><span class=\"label\">CPU</span><span class=\"value\">" + String(ESP.getCpuFreqMHz()) + " MHz, " + String(ESP.getChipCores()) + " cores</span></div></div>";
  s += "<div class=\"card\"><h2>Flash & Memory</h2>";
  s += "<div class=\"row\"><span class=\"label\">Flash Size</span><span class=\"value\">" + String(ESP.getFlashChipSize()/1024.0/1024.0, 2) + " MB</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Sketch Size</span><span class=\"value\">" + String(ESP.getSketchSize()/1024.0, 1) + " KB</span></div>";
  const esp_partition_t* runningPartition = esp_ota_get_running_partition();
  size_t appPartitionBytes = runningPartition ? runningPartition->size : 0;
  size_t unusedAppBytes = appPartitionBytes > ESP.getSketchSize() ? appPartitionBytes - ESP.getSketchSize() : 0;
  s += "<div class=\"row\"><span class=\"label\">App Partition Size</span><span class=\"value\">" + String(appPartitionBytes/1024.0, 1) + " KB</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Unused App Partition</span><span class=\"value\">" + String(unusedAppBytes/1024.0, 1) + " KB</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Heap Size</span><span class=\"value\">" + String(ESP.getHeapSize()/1024.0, 1) + " KB</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Free Heap</span><span class=\"value\">" + String(freeHeap/1024.0, 1) + " KB</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Minimum Free Heap</span><span class=\"value\">" + String(ESP.getMinFreeHeap()/1024.0, 1) + " KB</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Largest Free Block</span><span class=\"value\">" + String(largestBlock/1024.0, 1) + " KB (" + String(largestPct,1) + "% of free heap)</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Wi-Fi History</span><span class=\"value\">" + String(historyCount) + " / " + String(scanHistoryRetentionLimit) + " retained; " + String(scanHistoryCapacity) + " physical capacity; " + String(wifiHistoryAllocatedBytes()/1024.0,1) + " KB total</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Wi-Fi Observation Size</span><span class=\"value\">" + String(sizeof(WifiObservation)) + " bytes (V20 flat record was " + String(sizeof(ScanRecord)) + " bytes)</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Wi-Fi AP Table</span><span class=\"value\">" + String(wifiApCount) + " / " + String(wifiApTableCapacity) + " entries; " + String(wifiApTableCapacity*sizeof(WifiApEntry)/1024.0,1) + " KB allocated</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Wi-Fi Scan Metadata</span><span class=\"value\">" + String(wifiScanMetadataCapacity) + " slots; " + String(wifiScanMetadataCapacity*sizeof(WifiScanMetadata)/1024.0,1) + " KB allocated</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Wi-Fi Retained Scans</span><span class=\"value\">" + String(countRetainedScanGroups()) + "</span></div>";
  if (historyCount > 0) {
    const ScanRecord& oldestWifi = historyRecord(0);
    const ScanRecord& newestWifi = historyRecord(historyCount - 1);
    s += "<div class=\"row\"><span class=\"label\">Wi-Fi Oldest Record Age</span><span class=\"value\">" + htmlEscape(observationAgeLabel(oldestWifi.uptimeMs)) + "</span></div>";
    s += "<div class=\"row\"><span class=\"label\">Wi-Fi Retained Time Window</span><span class=\"value\">" + htmlEscape(retainedWindowLabel(oldestWifi.uptimeMs, newestWifi.uptimeMs)) + "</span></div>";
  }
  if (bleSurveyEnabled) {
    s += "<div class=\"row\"><span class=\"label\">BLE History</span><span class=\"value\">" + String(bleHistoryCount) + " / " + String(bleHistoryCapacity) + " records, " + String(bleHistoryCapacity*sizeof(BleScanRecord)/1024.0,1) + " KB allocated</span></div>";
    s += "<div class=\"row\"><span class=\"label\">BLE Retained Scans</span><span class=\"value\">" + String(countRetainedBleScanGroups()) + "</span></div>";
    if (bleHistoryCount > 0) {
      const BleScanRecord& oldestBle = bleHistoryRecord(0);
      const BleScanRecord& newestBle = bleHistoryRecord(bleHistoryCount - 1);
      s += "<div class=\"row\"><span class=\"label\">BLE Oldest Record Age</span><span class=\"value\">" + htmlEscape(observationAgeLabel(oldestBle.uptimeMs)) + "</span></div>";
      s += "<div class=\"row\"><span class=\"label\">BLE Retained Time Window</span><span class=\"value\">" + htmlEscape(retainedWindowLabel(oldestBle.uptimeMs, newestBle.uptimeMs)) + "</span></div>";
    }
  } else {
    s += "<div class=\"row\"><span class=\"label\">BLE History</span><span class=\"value\">Disabled at boot; 0 KB history allocated</span></div>";
  }
  s += "</div>";
  s += "<div class=\"card\"><h2>Radio Status</h2>";
  s += "<div class=\"row\"><span class=\"label\">Wi-Fi Mode</span><span class=\"value\">" + wifiModeLabel(WiFi.getMode()) + "</span></div>";
  s += "<div class=\"row\"><span class=\"label\">STA MAC</span><span class=\"value\">" + WiFi.macAddress() + "</span></div>";
  s += "<div class=\"row\"><span class=\"label\">AP MAC</span><span class=\"value\">" + WiFi.softAPmacAddress() + "</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Infrastructure Wi-Fi</span><span class=\"value\">" + String(WiFi.status()==WL_CONNECTED ? "Connected" : "Disconnected") + "</span></div>";
  if (WiFi.status()==WL_CONNECTED) s += "<div class=\"row\"><span class=\"label\">STA SSID / RSSI</span><span class=\"value\">" + htmlEscape(WiFi.SSID()) + " / " + String(WiFi.RSSI()) + " dBm</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Radio Channel</span><span class=\"value\">" + String(channelResult==ESP_OK ? String(primaryChannel) : String("Unavailable")) + "</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Survey AP</span><span class=\"value\">" + String(apRunning ? "Running" : "Disabled") + (apRunning ? " - " + htmlEscape(apSSID) : "") + "</span></div>";
  s += "<div class=\"row\"><span class=\"label\">BLE Boot Mode</span><span class=\"value\">" + String(bleSurveyEnabled ? "Enabled" : "Disabled (maximum Wi-Fi history)") + "</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Status LED</span><span class=\"value\">" + (STATUS_LED_ENABLED ? String("GPIO ") + String(STATUS_LED_PIN) : String("Disabled")) + "</span></div></div>";
  server.sendContent(s);

  server.sendContent("<div class=\"card\"><h2>Boot Heap Checkpoints</h2>"
    "<div class=\"note\">Captured during startup so Wi-Fi, BLE, history, web-server, and initial-scan RAM costs can be compared directly.</div>"
    "<div class=\"table-scroll\"><table><thead><tr><th>Stage</th><th>Free Heap</th><th>Min Free</th><th>Largest Block</th></tr></thead><tbody>");
  for (size_t i = 0; i < bootHeapCheckpointCount; i++) {
    const BootHeapCheckpoint& cp = bootHeapCheckpoints[i];
    String row = "<tr><td>" + htmlEscape(String(cp.stage)) + "</td>"
      "<td class=\"signal\">" + String(cp.freeHeap / 1024.0, 1) + " KB</td>"
      "<td class=\"signal\">" + String(cp.minimumFreeHeap / 1024.0, 1) + " KB</td>"
      "<td class=\"signal\">" + String(cp.largestFreeBlock / 1024.0, 1) + " KB</td></tr>";
    server.sendContent(row);
  }
  server.sendContent("</tbody></table></div></div>");

  server.sendContent("<div class=\"card\"><h2>Software Self-Tests</h2>");
  server.sendContent(selfTestRow("Wi-Fi subsystem", wifiSubsystemInitialized ? "PASS" : "FAIL", wifiSubsystemInitialized ? "initialized" : "not initialized"));
  if (bleSurveyEnabled) {
    server.sendContent(selfTestRow("BLE subsystem", bleInitialized ? "PASS" : "FAIL", bleInitialized ? "initialized" : "not initialized"));
  } else {
    server.sendContent(selfTestRow("BLE subsystem", "PASS", "disabled by configured survey mode; BLE stack not initialized"));
  }
  bool wh = scanHistory && wifiApTable && wifiScanMetadata &&
      scanHistoryCapacity>=MIN_SCAN_HISTORY_RECORDS &&
      scanHistoryRetentionLimit>=MIN_SCAN_HISTORY_RECORDS &&
      scanHistoryRetentionLimit<=scanHistoryCapacity &&
      historyCount<=scanHistoryRetentionLimit;
  bool bh = !bleSurveyEnabled ||
      (bleHistory && bleHistoryCapacity>=MIN_BLE_HISTORY_RECORDS && bleHistoryCount<=bleHistoryCapacity);
  server.sendContent(selfTestRow("Wi-Fi history buffer", wh ? "PASS" : "FAIL", wh ? "allocated and sane" : "allocation/capacity invalid"));
  server.sendContent(selfTestRow("BLE history buffer", bh ? "PASS" : "FAIL",
    !bleSurveyEnabled ? "not allocated by design" : (bh ? "allocated and sane" : "allocation/capacity invalid")));
  bool cfg = scanIntervalSeconds>=MIN_SCAN_INTERVAL_SECONDS && scanIntervalSeconds<=MAX_SCAN_INTERVAL_SECONDS &&
      (!bleSurveyEnabled || (bleScanIntervalSeconds>=MIN_SCAN_INTERVAL_SECONDS && bleScanIntervalSeconds<=MAX_SCAN_INTERVAL_SECONDS));
  server.sendContent(selfTestRow("Scan configuration", cfg ? "PASS" : "FAIL", cfg ? "enabled survey intervals within valid range" : "one or more values out of range"));
  bool initialDone = !initialWifiScanPending &&
      (!bleSurveyEnabled || !initialBleScanPending);
  server.sendContent(selfTestRow("Initial boot scans", initialDone ? "PASS" : "WARN",
    initialDone ? (bleSurveyEnabled ? "Wi-Fi and BLE initial scans completed" : "Wi-Fi initial scan completed; Bluetooth Survey disabled")
                : "one or more required initial scans are still pending"));
  bool autos = autoScanEnabled &&
      (!bleSurveyEnabled || autoBleScanEnabled);
  server.sendContent(selfTestRow("Headless automatic surveying", autos ? "PASS" : "WARN",
    autos ? (bleSurveyEnabled ? "Wi-Fi and BLE periodic scanning enabled" : "Wi-Fi periodic scanning enabled; Bluetooth Survey disabled")
          : "one or more enabled automatic scan services are disabled at runtime"));
  server.sendContent(selfTestRow("Heap reserve", freeHeap>=HEAP_WARN_BYTES ? "PASS" : "WARN", String(freeHeap/1024) + " KB free"));
  server.sendContent(selfTestRow("Application space", unusedAppBytes>64*1024 ? "PASS" : "WARN", String(unusedAppBytes/1024) + " KB unused in running app partition"));
  bool resetWarn = rr==ESP_RST_PANIC || rr==ESP_RST_INT_WDT || rr==ESP_RST_TASK_WDT || rr==ESP_RST_WDT || rr==ESP_RST_BROWNOUT;
  server.sendContent(selfTestRow("Boot/reset diagnostic", resetWarn ? "WARN" : "PASS", resetReasonLabel(rr)));
  server.sendContent("<div class=\"note\">WARN indicates a nonfatal condition worth reviewing. No filesystem self-test is included because persistent logging/filesystem support is intentionally not part of V21.</div></div>");
  server.sendContent("<div class=\"footer\">ESP32 Web Interface</div>");
  sendThemeScript();
  server.sendContent("</div></body></html>");
  server.sendContent("");
}

void handleSettingsPage() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent("<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>ESP32 Settings</title>");
  sendThemeBootstrapScript();
  server.sendContent(pageStyles());
  server.sendContent("</head><body><div class=\"container\">");
  sendSiteNavigation("settings");
  server.sendContent("<h1>Settings</h1><div class=\"card\"><h2>Infrastructure Wi-Fi</h2>");
  String s;
  s.reserve(3000);
  s += "<div class=\"row\"><span class=\"label\">Status</span><span class=\"value\">" + String(WiFi.status()==WL_CONNECTED ? "Connected" : "Disconnected") + "</span></div>";
  if (WiFi.status()==WL_CONNECTED) {
    s += "<div class=\"row\"><span class=\"label\">SSID</span><span class=\"value\">" + htmlEscape(WiFi.SSID()) + "</span></div>";
    s += "<div class=\"row\"><span class=\"label\">IP Address</span><span class=\"value\">" + WiFi.localIP().toString() + "</span></div>";
  }
  s += "<form class=\"controls\" action=\"/wifi-save\" method=\"post\"><div class=\"control\"><label for=\"sta-ssid\">SSID</label><input id=\"sta-ssid\" name=\"ssid\" type=\"text\" maxlength=\"32\" required></div><div class=\"control\"><label for=\"sta-password\">Password</label><input id=\"sta-password\" name=\"password\" type=\"password\" maxlength=\"63\"></div><button type=\"submit\">Connect &amp; Save</button></form>";
  s += "<form class=\"controls\" action=\"/wifi-clear\" method=\"post\"><button class=\"danger\" type=\"submit\">Clear Saved Wi-Fi</button></form>";
  s += "<div class=\"note\">The stored infrastructure passphrase is never displayed. A connection must succeed before new credentials are saved. Infrastructure Wi-Fi is optional; failed or absent infrastructure connectivity does not stop surveying.</div></div>";
  s += "<div class=\"card\"><h2>Survey Access Point</h2><div class=\"row\"><span class=\"label\">Status</span><span class=\"value\">" + String(apRunning ? "Running" : "Disabled") + "</span></div><div class=\"row\"><span class=\"label\">SSID</span><span class=\"value\">" + htmlEscape(apSSID) + "</span></div><div class=\"buttons\"><a class=\"button\" href=\"/ap\">Access Point Settings</a></div></div>";
  s += "<div class=\"card\"><h2>Survey Mode</h2>"
    "<div class=\"row\"><span class=\"label\">Bluetooth Survey</span><span class=\"value\">" +
    String(bleSurveyEnabled ? "Enabled" : "Disabled - maximum Wi-Fi history") + "</span></div>"
    "<form class=\"controls\" action=\"/ble-mode\" method=\"post\">"
    "<input type=\"hidden\" name=\"enabled\" value=\"" + String(bleSurveyEnabled ? "0" : "1") + "\">"
    "<button type=\"submit\">" + String(bleSurveyEnabled ? "Disable Bluetooth Survey" : "Enable Bluetooth Survey") + "</button></form>"
    "<div class=\"note\">Bluetooth is disabled by default to maximize Wi-Fi history depth. Changing this setting is stored in NVS and restarts the ESP32. "
    "The restart is intentional: BLE uses a large persistent heap allocation, so survey histories must be sized after the selected radio mode is established.</div>"
    "<div class=\"row\"><span class=\"label\">Status LED</span><span class=\"value\">" +
    String(STATUS_LED_ENABLED ? "GPIO 2 enabled" : "Disabled") + "</span></div></div>";
  s += "<div class=\"card\"><h2>Interface</h2><div class=\"note\">Theme selection is browser-local and is applied in the page head before CSS to avoid a light-theme flash during refresh. Survey interval and history settings remain session-only and are configured on their respective survey pages.</div></div>";
  server.sendContent(s);
  server.sendContent("<div class=\"footer\">ESP32 Web Interface</div>");
  sendThemeScript();
  server.sendContent("</div></body></html>");
  server.sendContent("");
}

void handleSaveStationSettings() {
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

void handleBleModeChange() {
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
    " after restart.</p><p>The ESP32 is restarting now so radio and history memory can be allocated safely.</p></div></div></body></html>");

  delay(750);
  ESP.restart();
}

void handleClearStationSettings() {
  eraseCredentials();
  WiFi.disconnect(false);
  ensureWiFiStationMode();
  server.sendHeader("Location", "/settings");
  server.send(303, "text/plain", "Saved infrastructure Wi-Fi credentials cleared.");
}

// ============================================================
// Access point web configuration// ============================================================
// Access point web configuration
// ============================================================

void handleAccessPointSettingsPage() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>

<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Access Point Settings</title>
)rawliteral";

  html += themeBootstrapScript();
  html += pageStyles();

  html += R"rawliteral(
</head>

<body>
  <div class="container">

    <div class="theme-control">
      <label for="theme-select">Theme</label>
      <select id="theme-select" class="theme-select" onchange="setTheme(this.value)">
        <option value="system">System</option>
        <option value="light">Light</option>
        <option value="dark">Dark</option>
      </select>
    </div>

    <div class="site-header"><div class="site-title">ESP32 Wireless Surveyor</div><nav class="nav"><a href="/">Wi-Fi</a><a href="/ble">Bluetooth</a><a href="/system">System</a><a class="active" href="/settings">Settings</a></nav></div>
    <h1>Access Point Settings</h1>

    <div class="card">

      <div class="row">
        <span class="label">Current Status</span>
        <span class="value">%AP_STATUS%</span>
      </div>

      <div class="row">
        <span class="label">Current AP IP</span>
        <span class="value">%AP_IP%</span>
      </div>

      <div class="row">
        <span class="label">Connected Clients</span>
        <span class="value">%AP_CLIENTS%</span>
      </div>

      <form class="controls" action="/ap-save" method="post">

        <div class="control">
          <label>
            <input
              type="checkbox"
              name="enabled"
              value="1"
              %AP_CHECKED%
            >
            Enable access point
          </label>
        </div>

        <div class="control">
          <label for="apssid">AP SSID</label>
          <input
            id="apssid"
            name="ssid"
            type="text"
            maxlength="32"
            value="%AP_SSID%"
          >
        </div>

        <div class="control">
          <label for="appassword">New AP password</label>
          <input
            id="appassword"
            name="password"
            type="password"
            minlength="8"
            maxlength="63"
            placeholder="Leave blank to keep current"
          >
        </div>

        <button type="submit">Save AP Settings &amp; Restart</button>

      </form>

      <div class="note">
        The AP password must be 8 to 63 characters.
        Leaving the password field blank keeps the current password.
        Changes are saved in NVS and applied after the ESP32 restarts.
        Disabling the AP may make this page unreachable unless the ESP32
        is also connected to an infrastructure Wi-Fi network.
      </div>

    </div>

    <div class="buttons">
      <a class="button" href="/settings">Back to Settings</a>
    </div>

    <div class="footer">
      ESP32 Web Interface
    </div>

  </div>
    <script>
      function applyTheme(v) {
        const r = document.documentElement;
        if (v === 'system') {
          const d = window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches;
          r.dataset.theme = d ? 'dark' : 'light';
        } else {
          r.dataset.theme = v;
        }
      }
      function setTheme(v) {
        localStorage.setItem('esp32-theme', v);
        applyTheme(v);
        document.querySelectorAll('.theme-select').forEach(s => s.value = v);
      }
      document.addEventListener('DOMContentLoaded', () => {
        const v = localStorage.getItem('esp32-theme') || 'system';
        applyTheme(v);
        document.querySelectorAll('.theme-select').forEach(s => s.value = v);
      });
    </script>
</body>
</html>
)rawliteral";

  html.replace(
    "%AP_STATUS%",
    apRunning ? "Running" : "Disabled"
  );

  html.replace(
    "%AP_IP%",
    apRunning ? WiFi.softAPIP().toString() : "-"
  );

  html.replace(
    "%AP_CLIENTS%",
    apRunning ? String(WiFi.softAPgetStationNum()) : "0"
  );

  html.replace("%AP_SSID%", htmlEscape(apSSID));

  html.replace(
    "%AP_CHECKED%",
    apEnabled ? "checked" : ""
  );

  server.send(200, "text/html", html);
}

void handleSaveAccessPointSettings() {
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

  delay(750);
  ESP.restart();
}


// ============================================================
// Web server
// ============================================================

void startWebServer() {
  if (webServerStarted) {
    return;
  }

  server.on("/", handleWebScan);
  server.on("/scan", handleWebScan);
  server.on("/system", HTTP_GET, handleSystemStatus);
  server.on("/settings", HTTP_GET, handleSettingsPage);
  server.on("/wifi-save", HTTP_POST, handleSaveStationSettings);
  server.on("/wifi-clear", HTTP_POST, handleClearStationSettings);
  server.on("/scan-now", handleWebScanNow);
  server.on("/api/wifi/status", HTTP_GET, handleWifiScanStatus);
  server.on("/scan-settings", handleScanSettings);
  server.on("/scan-clear", handleClearScanHistory);
  server.on("/scanlog.csv", handleScanCsv);
  server.on("/ble", HTTP_GET, handleBLESurvey);
  server.on("/ble-mode", HTTP_POST, handleBleModeChange);
  server.on("/ble-scan", HTTP_GET, handleBLEScanNow);
  server.on("/ble-settings", HTTP_GET, handleBLESettings);
  server.on("/ble-clear", HTTP_GET, handleClearBLEHistory);
  server.on("/blelog.csv", HTTP_GET, handleBLEScanCsv);
  server.on("/ap", HTTP_GET, handleAccessPointSettingsPage);
  server.on("/ap-save", HTTP_POST, handleSaveAccessPointSettings);

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
// Serial menu
// ============================================================

void printMenu() {
  Serial.println();
  Serial.println("================================");
  Serial.println(" ESP32 Control Menu");
  Serial.println("================================");

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi: Connected to ");
    Serial.println(WiFi.SSID());

    Serial.print("IP:    ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi: Not connected");
  }

  if (apRunning) {
    Serial.print("AP:    ");
    Serial.print(apSSID);
    Serial.print(" @ ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("AP:    Disabled");
  }

  Serial.println();
  Serial.println("1 - Wi-Fi status");
  Serial.println("2 - Scan Wi-Fi networks");
  Serial.println("3 - Configure Wi-Fi");
  Serial.println("4 - Clear saved Wi-Fi credentials");
  Serial.println("5 - Restart ESP32");
  Serial.println();
  Serial.println("mac     - Show Wi-Fi MAC address");
  Serial.println("version - Show firmware information");
  Serial.println("h       - Show this menu");
  Serial.println();

  Serial.print("> ");
}


// ============================================================
// Serial command handling
// ============================================================

void handleSerialCommand() {
  if (!Serial.available()) {
    return;
  }

  String command = Serial.readStringUntil('\n');
  command.trim();

  if (command.length() == 0) {
    printMenu();
    return;
  }

  if (
    command == "1" ||
    command.equalsIgnoreCase("status")
  ) {
    printWiFiStatus();
  }

  else if (
    command == "2" ||
    command.equalsIgnoreCase("scan")
  ) {
    scanNetworks();
  }

  else if (
    command == "3" ||
    command.equalsIgnoreCase("wifi")
  ) {
    configureWiFi();

    if (
      WiFi.status() == WL_CONNECTED &&
      !webServerStarted
    ) {
      startWebServer();
    }

    Serial.println();
    Serial.println("Returning to main menu...");
    printMenu();
    return;
  }

  else if (
    command == "4" ||
    command.equalsIgnoreCase("erase")
  ) {
    Serial.println();
    Serial.println(
      "This will erase the saved Wi-Fi credentials."
    );

    Serial.println("Type YES to confirm:");
    Serial.print("> ");

    String confirmation = readSerialLine();

    if (confirmation.equalsIgnoreCase("YES")) {
      eraseCredentials();

      // Disconnect only the station interface; keep the survey AP running.
      WiFi.disconnect(false);
      delay(100);

      Serial.println();
      Serial.println("Credentials erased.");
      Serial.println("Wi-Fi disconnected.");
      Serial.println("Use option 3 to configure a network.");
    } else {
      Serial.println();
      Serial.println("Erase cancelled.");
    }
  }

  else if (
    command == "5" ||
    command.equalsIgnoreCase("restart")
  ) {
    Serial.println();
    Serial.println("Restarting ESP32...");

    delay(500);
    ESP.restart();
  }

  else if (
    command.equalsIgnoreCase("version") ||
    command.equalsIgnoreCase("v")
  ) {
    printFirmwareInfo();
  }

  else if (command.equalsIgnoreCase("mac")) {
    printMacAddress();
  }

  else if (
    command.equalsIgnoreCase("h") ||
    command.equalsIgnoreCase("help")
  ) {
    printMenu();
    return;
  }

  else {
    Serial.println();
    Serial.print("Unknown command: ");
    Serial.println(command);

    Serial.println("Enter 'h' for help.");
  }

  Serial.println();
  Serial.print("> ");
}


// ============================================================
// Setup
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(1500);

  captureBootHeapCheckpoint("Startup");
  initializeStatusLed();
  captureBootHeapCheckpoint("Status LED initialized");
  indicateBootStarted();

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

  WiFi.mode(WIFI_STA);
  delay(WIFI_STARTUP_SETTLE_MS);
  wifiSubsystemInitialized = (WiFi.getMode() != WIFI_MODE_NULL);
  captureBootHeapCheckpoint("Wi-Fi initialized");

  loadAccessPointSettings();
  loadSurveyModeSettings();
  Serial.print("Bluetooth Survey mode: ");
  Serial.println(bleSurveyEnabled ? "enabled" : "disabled (Wi-Fi-first)");
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
      Serial.println("Use serial menu option 3 to configure Wi-Fi.");
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

  Serial.print("Auto-sized Wi-Fi history: ");
  Serial.print(scanHistoryCapacity);
  Serial.println(" compact observation slots");
  Serial.print("Auto-sized BLE history:   ");
  if (bleSurveyEnabled) {
    Serial.print(bleHistoryCapacity);
    Serial.println(" records");
  } else {
    Serial.println("disabled");
  }

  if (WiFi.status() == WL_CONNECTED || apRunning) startWebServer();
  captureBootHeapCheckpoint("Web server ready");

  bool startupFailed =
      !wifiSubsystemInitialized ||
      scanHistory == nullptr ||
      wifiApTable == nullptr ||
      wifiScanMetadata == nullptr ||
      (bleSurveyEnabled && (!bleInitialized || bleHistory == nullptr));

  bool startupWarning =
      !startupFailed &&
      (ESP.getFreeHeap() < HEAP_WARN_BYTES ||
       scanHistoryCapacity == MIN_SCAN_HISTORY_RECORDS ||
       (bleSurveyEnabled &&
        bleHistoryCapacity == MIN_BLE_HISTORY_RECORDS));

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

void serviceInitialSurveyScans() {
  unsigned long elapsed = millis() - surveyServicesReadyMs;

  if (initialWifiScanPending && elapsed >= INITIAL_WIFI_SCAN_DELAY_MS) {
    initialWifiScanPending = false;
    Serial.println("Initial headless Wi-Fi survey scan...");
    performLoggedScan();
    WiFi.scanDelete();
    lastAutoScanMs = millis();
    captureBootHeapCheckpoint("Initial Wi-Fi scan");
  }

  if (
    bleSurveyEnabled &&
    initialBleScanPending &&
    elapsed >= INITIAL_BLE_SCAN_DELAY_MS
  ) {
    initialBleScanPending = false;
    Serial.println("Initial headless BLE survey scan...");
    performLoggedBLEScan();
    lastAutoBleScanMs = millis();
    captureBootHeapCheckpoint("Initial BLE scan");
  }
}

void serviceAutomaticScan() {
  if (!autoScanEnabled) return;

  unsigned long intervalMs = scanIntervalSeconds * 1000UL;
  if (millis() - lastAutoScanMs < intervalMs) return;

  lastAutoScanMs = millis();
  performLoggedScan();
  WiFi.scanDelete();
}

void serviceAutomaticBLEScan() {
  if (!bleSurveyEnabled || !autoBleScanEnabled) return;

  unsigned long intervalMs = bleScanIntervalSeconds * 1000UL;
  if (millis() - lastAutoBleScanMs < intervalMs) return;

  lastAutoBleScanMs = millis();
  performLoggedBLEScan();
}


// ============================================================
// Main loop
// ============================================================

void loop() {
  if (webServerStarted) {
    server.handleClient();
  }

  serviceInitialSurveyScans();
  serviceAutomaticScan();
  serviceAutomaticBLEScan();
  handleSerialCommand();

  delay(5);
}
