// WifiConnect29 - Wi-Fi auto-scan diagnostics and scheduler cleanup
// memory allocation, web/serial interface parity, LED controls, and mDNS
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_mac.h>
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

const char* FIRMWARE_FILE = "WifiConnect29_autoscan_diagnostics.ino";
const char* FIRMWARE_VERSION = "29";


Preferences preferences;
WebServer server(80);

const unsigned long WIFI_TIMEOUT_MS = 15000;
const unsigned long WIFI_STARTUP_SETTLE_MS = 300;

bool webServerStarted = false;

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
// Dual-radio mode has a much tighter heap envelope because the BLE stack
// remains resident and Wi-Fi scans have a large transient allocation. V24's
// 48 KB allocation-time target yielded an ~11 KB observed low-water mark.
// V25 intentionally targets 60 KB here; if fixed compact metadata + minimum
// observation rings exceed the remaining budget, only those minimum rings are
// allocated rather than consuming additional heap.
const size_t DUAL_RADIO_HEAP_RESERVE_BYTES = 60 * 1024;

const size_t HISTORY_HEAP_RESERVE_BYTES = 96 * 1024;

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

// Generic scan metadata shared by Wi-Fi and BLE. Each radio has its own
// metadata table/counter, but both use the same representation and lifecycle.
struct SurveyScanMetadata {
  uint32_t scanNumber;
  uint32_t uptimeMs;
};
using WifiScanMetadata = SurveyScanMetadata;

// Compact recurring measurement. 6 bytes on the classic ESP32 ABI versus
// 68 bytes for the old flat ScanRecord.
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

// Explicit prototypes for V21 custom types to avoid Arduino 1.8.x auto-prototype ordering issues.
const WifiObservation& compactHistoryRecord(size_t logicalIndex);
ScanRecord historyRecord(size_t logicalIndex);
void appendWifiObservation(const WifiObservation& observation);
const BleObservation& compactBleHistoryRecord(size_t logicalIndex);
BleScanRecord bleHistoryRecord(size_t logicalIndex);
void appendBleObservation(const BleObservation& observation);

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
uint32_t lastWifiAutoScanStartMs = 0;
uint32_t lastWifiAutoScanCompletionMs = 0;
bool wifiCurrentScanAutomatic = false;

WifiApEntry* wifiApTable = nullptr;
size_t wifiApTableCapacity = 0;
size_t wifiApCount = 0;
WifiScanMetadata* wifiScanMetadata = nullptr;
size_t wifiScanMetadataCapacity = 0;
size_t wifiApTableFullDrops = 0;

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
// Boot/resource diagnostics and status LED
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
  if (!STATUS_LED_AVAILABLE) return;
  if (on && !statusLedEnabled && !statusLedSelfTestOverride) return;

  statusLedState = on;
  bool electricalHigh =
      STATUS_LED_ACTIVE_HIGH ? on : !on;
  digitalWrite(STATUS_LED_PIN, electricalHigh ? HIGH : LOW);
}

void statusLedTimerCallback(TimerHandle_t) {
  writeStatusLed(!statusLedState);
}

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

void startScanLed(TickType_t periodTicks) {
  if (!STATUS_LED_AVAILABLE || !statusLedEnabled || statusLedTimer == nullptr) return;

  writeStatusLed(true);
  xTimerStop(statusLedTimer, 0);
  xTimerChangePeriod(statusLedTimer, periodTicks, 0);
}

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

void indicateBootStarted() {
  if (!STATUS_LED_AVAILABLE || (!statusLedEnabled && !statusLedSelfTestOverride)) return;

  statusLedPulse(90, 90);
  statusLedPulse(90, 0);
}

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

String normalizedMdnsHostname(String value) {
  value.trim();
  value.toLowerCase();
  return value;
}

String mdnsWebAddress() {
  return "http://" + mdnsHostname + ".local/";
}

void loadSurveyModeSettings() {
  preferences.begin("survey", true);
  bleSurveyEnabled = preferences.getBool("bleEnabled", false);
  statusLedEnabled = preferences.getBool("ledEnabled", true);
  webAutoRefreshEnabled = preferences.getBool("webRefresh", true);
  scanIntervalSeconds = preferences.getULong("wifiInterval", scanIntervalSeconds);
  if (scanIntervalSeconds < MIN_SCAN_INTERVAL_SECONDS) scanIntervalSeconds = MIN_SCAN_INTERVAL_SECONDS;
  if (scanIntervalSeconds > MAX_SCAN_INTERVAL_SECONDS) scanIntervalSeconds = MAX_SCAN_INTERVAL_SECONDS;
  mdnsHostnameUserConfigured = preferences.isKey("hostname");
  mdnsHostname = normalizedMdnsHostname(preferences.getString("hostname", DEFAULT_MDNS_HOSTNAME));
  preferences.end();

  if (!isValidMdnsHostname(mdnsHostname)) {
    mdnsHostname = DEFAULT_MDNS_HOSTNAME;
    mdnsHostnameUserConfigured = false;
  }
}

String generatedDefaultMdnsHostname() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String("surveyor-") + suffix;
}

void applyGeneratedDefaultMdnsHostname() {
  if (mdnsHostnameUserConfigured) return;
  mdnsHostname = normalizedMdnsHostname(generatedDefaultMdnsHostname());
}

void saveMdnsHostname(const String& hostname) {
  mdnsHostname = normalizedMdnsHostname(hostname);
  mdnsHostnameUserConfigured = true;
  preferences.begin("survey", false);
  preferences.putString("hostname", mdnsHostname);
  preferences.end();
}

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

void saveBleSurveyEnabled(bool enabled) {
  preferences.begin("survey", false);
  preferences.putBool("bleEnabled", enabled);
  preferences.end();
}

void saveInterfaceSettings(bool ledEnabled, bool autoRefreshEnabled) {
  statusLedEnabled = ledEnabled;
  webAutoRefreshEnabled = autoRefreshEnabled;
  preferences.begin("survey", false);
  preferences.putBool("ledEnabled", statusLedEnabled);
  preferences.putBool("webRefresh", webAutoRefreshEnabled);
  preferences.end();
}

void saveWebLiveUpdates(bool enabled) {
  webAutoRefreshEnabled = enabled;
  preferences.begin("survey", false);
  preferences.putBool("webRefresh", webAutoRefreshEnabled);
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

bool wifiScanInProgress = false;
bool wifiInitialScanCheckpointPending = false;
String wifiScanStatusMessage = "Idle";

int processCompletedWifiScan(int networkCount) {
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

int performLoggedScan() {
  ensureWiFiStationMode();

  startScanLed(WIFI_SCAN_LED_PERIOD_TICKS);
  int networkCount = WiFi.scanNetworks();
  stopScanLed();

  int result = processCompletedWifiScan(networkCount);
  wifiScanStatusMessage = "Complete";
  return result;
}

bool beginLoggedWifiScan(bool initialCheckpoint, bool automaticTrigger) {
  if (wifiScanInProgress) return false;

  ensureWiFiStationMode();
  WiFi.scanDelete();

  int result = WiFi.scanNetworks(true);
  if (result == WIFI_SCAN_FAILED) {
    wifiScanStatusMessage = "Failed to start";
    if (automaticTrigger) wifiAutoScanStartFailureCount++;
    return false;
  }

  wifiCurrentScanAutomatic = automaticTrigger;
  if (automaticTrigger) {
    wifiAutoScanStartCount++;
    lastWifiAutoScanStartMs = millis();
  }

  wifiScanInProgress = true;
  wifiInitialScanCheckpointPending =
      wifiInitialScanCheckpointPending || initialCheckpoint;
  wifiScanStatusMessage = "Scanning";
  startScanLed(WIFI_SCAN_LED_PERIOD_TICKS);
  return true;
}

void serviceLoggedWifiScan() {
  if (!wifiScanInProgress) return;

  int result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) return;

  stopScanLed();
  wifiScanInProgress = false;

  if (result == WIFI_SCAN_FAILED) {
    wifiScanStatusMessage = "Scan failed";
    WiFi.scanDelete();
    wifiInitialScanCheckpointPending = false;
    wifiCurrentScanAutomatic = false;
    return;
  }

  processCompletedWifiScan(result);
  if (wifiCurrentScanAutomatic) {
    wifiAutoScanCompletionCount++;
    lastWifiAutoScanCompletionMs = lastScanUptimeMs;
  }
  wifiScanStatusMessage = "Complete";
  WiFi.scanDelete();
  lastAutoScanMs = millis();

  if (wifiInitialScanCheckpointPending) {
    captureBootHeapCheckpoint("Initial Wi-Fi scan");
    wifiInitialScanCheckpointPending = false;
  }

  wifiCurrentScanAutomatic = false;
}

// ============================================================
// BLE compact normalized history
// ============================================================

size_t bleHistoryAllocatedBytes() {
  return
    bleHistoryCapacity * sizeof(BleObservation) +
    bleAddressTableCapacity * sizeof(BleAddressEntry) +
    bleScanMetadataCapacity * sizeof(SurveyScanMetadata);
}

const BleObservation& compactBleHistoryRecord(size_t logicalIndex) {
  size_t physicalIndex =
      (bleHistoryStart + logicalIndex) % bleHistoryCapacity;
  return bleHistory[physicalIndex];
}

bool parseBleAddress(const String& text, uint8_t output[6]) {
  unsigned int b[6];
  if (sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x",
      &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
  for (int i = 0; i < 6; i++) output[i] = (uint8_t)b[i];
  return true;
}

void formatBleAddress(const uint8_t address[6], char output[18]) {
  snprintf(output, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
    address[0], address[1], address[2], address[3], address[4], address[5]);
}

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

bool bleAddressIndexIsReferenced(size_t addressIndex) {
  for (size_t i = 0; i < bleHistoryCount; i++) {
    if (compactBleHistoryRecord(i).addressIndex == addressIndex) return true;
  }
  return false;
}

size_t countReferencedBleAddresses() {
  if (!bleAddressTable || bleAddressTableCapacity == 0) return 0;
  size_t count = 0;
  for (size_t i = 0; i < bleAddressTableCapacity; i++)
    if (bleAddressIndexIsReferenced(i)) count++;
  return count;
}

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

void updateBleUsageHighWaterMarks() {
  size_t addresses = countReferencedBleAddresses();
  if (addresses > bleAddressPeakReferenced) bleAddressPeakReferenced = addresses;
  size_t scans = countReferencedBleScanSlots();
  if (scans > bleScanMetadataPeakUsed) bleScanMetadataPeakUsed = scans;
}

void discardOldestBleObservation() {
  if (bleHistoryCount == 0 || bleHistoryCapacity == 0) return;
  bleHistoryStart = (bleHistoryStart + 1) % bleHistoryCapacity;
  bleHistoryCount--;
}

void discardBleObservationsForScanSlot(uint16_t scanSlot) {
  if (bleHistoryCount == 0) return;
  size_t original = bleHistoryCount;
  for (size_t i = 0; i < original; i++) {
    BleObservation observation = compactBleHistoryRecord(0);
    discardOldestBleObservation();
    if (observation.scanSlot != scanSlot) appendBleObservation(observation);
  }
}

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

void appendBleObservation(const BleObservation& observation) {
  if (!bleHistory || bleHistoryCapacity == 0 || bleHistoryRetentionLimit == 0)
    return;
  while (bleHistoryCount >= bleHistoryRetentionLimit)
    discardOldestBleObservation();
  size_t writeIndex = (bleHistoryStart + bleHistoryCount) % bleHistoryCapacity;
  bleHistory[writeIndex] = observation;
  bleHistoryCount++;
}

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

void initializeAutoSizedHistories() {
  if (scanHistory != nullptr || bleHistory != nullptr) return;

  size_t freeHeap = ESP.getFreeHeap();

  if (!bleSurveyEnabled) {
    size_t available = freeHeap > HISTORY_HEAP_RESERVE_BYTES
      ? freeHeap - HISTORY_HEAP_RESERVE_BYTES : 0;
    size_t minimumCompactBytes =
        64 * sizeof(WifiApEntry) +
        64 * sizeof(WifiScanMetadata) +
        MIN_SCAN_HISTORY_RECORDS * sizeof(WifiObservation);
    if (available < minimumCompactBytes) available = minimumCompactBytes;
    if (!initializeCompactWifiHistory(available))
      initializeCompactWifiHistory(minimumCompactBytes);
  } else {
    // BLE initialization itself consumes substantial heap. V24 showed that a
    // 48 KB allocation-time target resulted in only ~11 KB minimum free heap
    // during the initial radio scans after WebServer + mDNS startup. V25 uses
    // a more conservative 60 KB allocation-time target. If the agreed fixed
    // tables plus minimum observation rings exceed that budget, the minimum
    // rings win and no additional 50/50 observation expansion is attempted.
    size_t available = freeHeap > DUAL_RADIO_HEAP_RESERVE_BYTES
      ? freeHeap - DUAL_RADIO_HEAP_RESERVE_BYTES : 0;

    const size_t wifiMetadata =
        64 * sizeof(WifiApEntry) + 64 * sizeof(WifiScanMetadata);
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

    if (!initializeCompactWifiHistory(wifiBudget))
      initializeCompactWifiHistory(wifiMetadata + minimumWifiObs);
    if (!initializeCompactBleHistory(bleBudget))
      initializeCompactBleHistory(bleMetadata + minimumBleObs);
  }

  bootWifiHistoryCapacity = scanHistoryCapacity;
  bootBleHistoryCapacity = bleHistoryCapacity;
  historyResizeMessage = "";
  bleHistoryResizeMessage = "";
}

bool bleHistoryContainsAddress(const String& address) {
  uint8_t parsed[6];
  if (!parseBleAddress(address, parsed)) return false;
  int index = findBleAddress(parsed);
  return index >= 0 && bleAddressIndexIsReferenced((size_t)index);
}

String latestBleNameForAddress(const String& address) {
  uint8_t parsed[6];
  if (!parseBleAddress(address, parsed)) return "(unnamed)";
  int index = findBleAddress(parsed);
  if (index < 0 || !bleAddressIndexIsReferenced((size_t)index)) return "(unnamed)";
  return bleAddressTable[index].name[0] ? String(bleAddressTable[index].name) : String("(unnamed)");
}

bool hasNewerBleObservationForAddress(size_t logicalIndex, const String& address) {
  for (size_t i = logicalIndex + 1; i < bleHistoryCount; i++) {
    if (String(bleHistoryRecord(i).address).equalsIgnoreCase(address)) return true;
  }
  return false;
}

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

int performLoggedBLEScan() {
  if (!bleSurveyEnabled) {
    bleStatusMessage = "Bluetooth Survey is disabled; BLE stack is not initialized.";
    return -1;
  }

  initializeBLEScanner();
  BLEScan* scan = BLEDevice::getScan();
  bleStatusMessage = "BLE scan in progress...";
  startScanLed(BLE_SCAN_LED_PERIOD_TICKS);
  BLEScanResults* results = scan->start(BLE_SCAN_DURATION_SECONDS, false);
  stopScanLed();

  bleScanCounter++;
  lastBleScanUptimeMs = millis();
  int resultCount = results != nullptr ? results->getCount() : 0;

  if (!bleScanMetadata || bleScanMetadataCapacity == 0) {
    if (results) scan->clearResults();
    return resultCount;
  }

  uint16_t scanSlot =
      (uint16_t)((bleScanCounter - 1) % bleScanMetadataCapacity);
  if (
    bleScanMetadata[scanSlot].scanNumber != 0 &&
    bleScanMetadata[scanSlot].scanNumber != bleScanCounter
  ) discardBleObservationsForScanSlot(scanSlot);
  bleScanMetadata[scanSlot].scanNumber = bleScanCounter;
  bleScanMetadata[scanSlot].uptimeMs = lastBleScanUptimeMs;

  if (results != nullptr) {
    for (int i = 0; i < resultCount; i++) {
      BLEAdvertisedDevice device = results->getDevice(i);
      String addressText = device.getAddress().toString();
      uint8_t rawAddress[6];
      if (!parseBleAddress(addressText, rawAddress)) continue;
      String name = "";
      if (device.haveName()) name = device.getName();
      int addressIndex = findOrCreateBleAddress(
        rawAddress, name, (uint8_t)device.getAddressType());
      if (addressIndex < 0) continue;
      int rssi = device.getRSSI();
      if (rssi < -128) rssi = -128;
      if (rssi > 127) rssi = 127;
      BleObservation observation = {};
      observation.addressIndex = (uint16_t)addressIndex;
      observation.scanSlot = scanSlot;
      observation.rssi = (int8_t)rssi;
      appendBleObservation(observation);
    }
  }

  scan->clearResults();
  updateBleUsageHighWaterMarks();
  bleStatusMessage =
    "BLE scan #" + String(bleScanCounter) + " complete: " +
    String(resultCount) + " address(es) observed.";
  if (bleAddressTableFullDrops > 0)
    bleStatusMessage += " WARN: " + String(bleAddressTableFullDrops) +
      " observation(s) dropped because the BLE Address Table was full.";
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

bool wifiAutoScanCadenceOverdue() {
  uint32_t now = millis();
  uint32_t intervalMs = scanIntervalSeconds * 1000UL;

  if (wifiScanInProgress) return false;
  if (scanCounter == 0)
    return !initialWifiScanPending;

  return (uint32_t)(now - lastScanUptimeMs) > intervalMs;
}

String wifiAutoScanDiagnosticLabel() {
  uint32_t now = millis();
  uint32_t intervalMs = scanIntervalSeconds * 1000UL;

  if (wifiScanInProgress && wifiCurrentScanAutomatic)
    return "OK - automatic scan in progress";

  if (scanCounter == 0) {
    if (initialWifiScanPending || wifiScanInProgress)
      return "STARTING - waiting for first completed scan";
    return "WARN - no completed Wi-Fi scan";
  }

  uint32_t ageMs = now - lastScanUptimeMs;
  if (ageMs <= intervalMs)
    return "OK - last scan completed within interval";

  if (wifiScanInProgress)
    return "OK - scan in progress after interval elapsed";

  return "WARN - scan overdue; automatic scan not in progress";
}

String wifiAutoScanLastStartLabel() {
  if (wifiAutoScanStartCount == 0) return "Never";
  return observationAgeLabel(lastWifiAutoScanStartMs);
}

String wifiAutoScanLastCompletionLabel() {
  if (wifiAutoScanCompletionCount == 0) return "Never";
  return observationAgeLabel(lastWifiAutoScanCompletionMs);
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
  Serial.println("Use 'wifi-config' to configure an infrastructure network.");

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

String jsEscape(String input) {
  input.replace("\\", "\\\\");
  input.replace("'", "\\'");
  input.replace("\r", "");
  input.replace("\n", "\\n");
  return input;
}

void redirectToScanPage() {
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

void handleWifiScanStatus() {
  String json = "{\"scan\":" + String(scanCounter) +
                ",\"records\":" + String(historyCount) +
                ",\"scanning\":" + String(wifiScanInProgress ? "true" : "false") +
                ",\"automaticScan\":" + String(wifiCurrentScanAutomatic ? "true" : "false") +
                ",\"autoStarts\":" + String(wifiAutoScanStartCount) +
                ",\"autoCompletions\":" + String(wifiAutoScanCompletionCount) +
                ",\"autoStartFailures\":" + String(wifiAutoScanStartFailureCount) +
                ",\"live\":" + String(webAutoRefreshEnabled ? "true" : "false") + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleWebScanNow() {
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

void handleLiveUpdatesSetting() {
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

void handleScanSettings() {
  applyWifiScanIntervalFromRequest();
  redirectToScanPage();
}

void handleWifiIntervalSetting() {
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
  nav.reserve(1300);
  nav += "<div class=\"site-header\"><div class=\"site-title\">ESP32 Wireless Surveyor</div><div class=\"header-actions\"><nav class=\"nav\">";
  nav += "<a href=\"/\"" + activeNavClass(active, "wifi") + ">Wi-Fi</a>";
  nav += "<a href=\"/ble\"" + activeNavClass(active, "ble") + ">Bluetooth</a>";
  nav += "<a href=\"/system\"" + activeNavClass(active, "system") + ">System</a>";
  nav += "<a href=\"/settings\"" + activeNavClass(active, "settings") + ">Settings</a>";
  nav += "</nav><label class=\"live-control\"><input id=\"live-updates-toggle\" type=\"checkbox\"";
  if (webAutoRefreshEnabled) nav += " checked";
  nav += "> Live updates</label></div></div>";
  server.sendContent(nav);
  sendThemeControl();
  server.sendContent(
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
  server.sendContent("<div class=\"card\"><h2>2.4 GHz Observed Channel Interference</h2>");
  if (!a.valid) {
    server.sendContent("<p>No scan data is available yet.</p><div class=\"note\">This is an estimate based on observed AP presence and RSSI, not measured airtime utilization or traffic load.</div></div>");
    return;
  }
  String s;
  s.reserve(2600);
  s += "<div class=\"row\"><span class=\"label\">Based On</span><span class=\"value\">Latest scan #" + String(a.scanNumber) + "</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Suggested Channel Based on Observed APs</span><span class=\"value\"><strong>" + String(a.suggestedChannel) + "</strong></span></div>";
  s += "<div class=\"note\">This estimate compares channels 1, 6, and 11 using visible AP strength plus co-channel and adjacent-channel overlap. Lower score is preferred. It is based only on observed AP presence and RSSI; it does not measure airtime utilization, traffic load, noise floor, retransmissions, or non-Wi-Fi interference.</div>";
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
  loggingCard.reserve(1800);

  loggingCard +=
    "<div class=\"card\"><h2>Survey Controls</h2>"
    "<div class=\"row\"><span class=\"label\">Surveying</span>"
    "<span class=\"value\">Always on</span></div>"
    "<div class=\"row\"><span class=\"label\">Scans This Session</span>"
    "<span class=\"value\">";
  loggingCard += String(scanCounter);
  loggingCard +=
    "</span></div>"
    "<div class=\"row\"><span class=\"label\">Stored Observations</span>"
    "<span class=\"value\">";
  loggingCard += String(historyCount);
  loggingCard += " / ";
  loggingCard += String(scanHistoryCapacity);
  loggingCard +=
    " capacity</span></div>"
    "<div class=\"row\"><span class=\"label\">Last Scan</span>"
    "<span class=\"value\">";

  if (scanCounter == 0) {
    loggingCard += "Never";
  } else {
    loggingCard += htmlEscape(observationAgeLabel(lastScanUptimeMs));
  }

  loggingCard +=
    "</span></div>"
    "<div class=\"survey-control-row\">"
    "<div class=\"control\"><label for=\"interval\">Scan interval (seconds)</label>"
    "<input id=\"interval\" type=\"number\" min=\"5\" max=\"3600\" value=\"";
  loggingCard += String(scanIntervalSeconds);
  loggingCard +=
    "\"></div>"
    "<span id=\"interval-save-state\" class=\"save-state\"></span>"
    "</div>"
    "<div class=\"buttons\">"
    "<button class=\"button\" type=\"button\" id=\"wifi-scan-now\">Scan Now</button>"
    "<a class=\"button\" href=\"/scanlog.csv\">Download CSV</a>"
    "<a class=\"button\" href=\"/scan-clear\">Clear History</a>"
    "<a class=\"button\" href=\"/\">Refresh Page</a>"
    "</div>"
    "<div id=\"wifi-scan-state\" class=\"scan-state\"></div>"
    "<div class=\"note\">"
    "Surveying runs automatically whenever the device is operating. "
    "Changing the interval saves immediately. Scan history is RAM-only and is cleared by reset or power cycle."
    "</div>";

  if (wifiApTableFullDrops > 0) {
    loggingCard += "<div class=\"note\"><strong>Warning:</strong> ";
    loggingCard += String(wifiApTableFullDrops);
    loggingCard += " observation(s) were not logged because the unique AP table was full.</div>";
  }

  loggingCard += "</div>";
  server.sendContent(loggingCard);
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
    "<div class=\"card\" id=\"wifi-observed-card\"><h2>Observed Networks</h2>"
    "<div class=\"note\">"
    "One row is shown for each BSSID observed during this session. "
    "Click any column header to sort the table. Click an SSID or BSSID "
    "to redraw the RSSI plot for that access point."
    "</div>"
  );

  sendNetworkSummaryTable();

  server.sendContent("</div>");

  String surveyDetails;
  surveyDetails.reserve(1800);
  surveyDetails +=
    "<div class=\"card\"><h2>Survey Details</h2>"
    "<div class=\"row\"><span class=\"label\">History Capacity</span><span class=\"value\">" +
    String(scanHistoryCapacity) + " observations (automatic maximum)</span></div>"
    "<div class=\"row\"><span class=\"label\">Scan Groups Retained</span><span class=\"value\">" +
    String(countRetainedScanGroups()) + "</span></div>";

  if (historyCount > 0) {
    const ScanRecord& oldestRetained = historyRecord(0);
    const ScanRecord& newestRetained = historyRecord(historyCount - 1);
    surveyDetails +=
      "<div class=\"row\"><span class=\"label\">Oldest Record Age</span><span class=\"value\">" +
      htmlEscape(observationAgeLabel(oldestRetained.uptimeMs)) + "</span></div>"
      "<div class=\"row\"><span class=\"label\">Retained Time Window</span><span class=\"value\">" +
      htmlEscape(retainedWindowLabel(oldestRetained.uptimeMs, newestRetained.uptimeMs)) + "</span></div>";
  }

  surveyDetails +=
    "<div class=\"row\"><span class=\"label\">Auto-Scan Diagnostic</span><span class=\"value\">" +
    htmlEscape(wifiAutoScanDiagnosticLabel()) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Automatic Scan Starts</span><span class=\"value\">" +
    String(wifiAutoScanStartCount) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Automatic Scan Completions</span><span class=\"value\">" +
    String(wifiAutoScanCompletionCount) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Automatic Start Failures</span><span class=\"value\">" +
    String(wifiAutoScanStartFailureCount) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Last Automatic Start</span><span class=\"value\">" +
    htmlEscape(wifiAutoScanLastStartLabel()) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Last Automatic Completion</span><span class=\"value\">" +
    htmlEscape(wifiAutoScanLastCompletionLabel()) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">History RAM</span><span class=\"value\">" +
    String(wifiHistoryAllocatedBytes() / 1024.0, 1) + " KB total</span></div>"
    "<div class=\"row\"><span class=\"label\">Observation Storage</span><span class=\"value\">" +
    String((scanHistoryCapacity * sizeof(WifiObservation)) / 1024.0, 1) + " KB; " +
    String(sizeof(WifiObservation)) + " bytes/observation</span></div>"
    "<div class=\"row\"><span class=\"label\">AP Table</span><span class=\"value\">" +
    String(wifiApCount) + " / " + String(wifiApTableCapacity) + " APs; " +
    String((wifiApTableCapacity * sizeof(WifiApEntry)) / 1024.0, 1) + " KB allocated</span></div>"
    "<div class=\"row\"><span class=\"label\">Scan Metadata</span><span class=\"value\">" +
    String(wifiScanMetadataCapacity) + " slots; " +
    String((wifiScanMetadataCapacity * sizeof(WifiScanMetadata)) / 1024.0, 1) + " KB allocated</span></div>"
    "<div class=\"row\"><span class=\"label\">Free Heap</span><span class=\"value\">" +
    String(ESP.getFreeHeap() / 1024.0, 1) + " KB</span></div>"
    "<div class=\"row\"><span class=\"label\">Largest Free Block</span><span class=\"value\">" +
    String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024.0, 1) + " KB</span></div>"
    "</div>";
  server.sendContent(surveyDetails);

  sendWifiChannelAnalysis();

  server.sendContent(
    "<div class=\"footer\">ESP32 Web Interface</div>"
  );

  sendSortableTableScript();
  sendThemeScript();

  // Live updates repaint only changing survey fragments; the document,
  // scroll position, forms, and plot selection remain intact.
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
      "const intervalState=document.getElementById('interval-save-state');"
      "function showScanState(active,msg){if(scanState){scanState.textContent=msg||'';scanState.classList.toggle('active',!!active);}if(scanButton)scanButton.disabled=!!active;}"
      "function saveInterval(){if(!intervalInput)return;let v=parseInt(intervalInput.value,10);if(!Number.isFinite(v))return;v=Math.max(5,Math.min(3600,v));intervalInput.value=v;if(intervalState)intervalState.textContent='Saving…';fetch('/api/wifi/interval?interval='+encodeURIComponent(v),{method:'POST',cache:'no-store'}).then(r=>{if(!r.ok)throw new Error();return r.json();}).then(s=>{intervalInput.value=s.interval;if(intervalState){intervalState.textContent='Saved';setTimeout(()=>{intervalState.textContent='';},1400);}}).catch(()=>{if(intervalState)intervalState.textContent='Save failed';});}"
      "if(intervalInput){intervalInput.addEventListener('change',saveInterval);intervalInput.addEventListener('keydown',function(e){if(e.key==='Enter'){e.preventDefault();saveInterval();intervalInput.blur();}});}"
      "if(scanButton){scanButton.addEventListener('click',function(){showScanState(true,'Scanning…');fetch('/scan-now',{cache:'no-store'}).then(function(r){if(!r.ok&&r.status!==202)throw new Error();return r.json();}).then(function(s){showScanState(!!s.scanning,s.scanning?'Scanning…':(s.message||''));}).catch(function(){showScanState(false,'Unable to start scan');});});}"
      "async function repaint(){"
        "if(updating)return;updating=true;"
        "try{"
          "const jobs=[fetch('/api/wifi/observed',{cache:'no-store'}).then(r=>r.text()).then(h=>{const e=document.getElementById('wifi-observed-card');if(e)e.innerHTML=h;})];"
          "if(plotBssid){jobs.push(fetch('/api/wifi/plot?bssid='+encodeURIComponent(plotBssid),{cache:'no-store'}).then(r=>r.text()).then(h=>{const e=document.getElementById('rssi-plot');if(e)e.innerHTML=h;}));}"
          "await Promise.all(jobs);"
        "}catch(e){}finally{updating=false;}"
      "}"
      "setInterval(function(){"
        "fetch('/api/wifi/status',{cache:'no-store'}).then(r=>r.json()).then(function(s){"
          "showScanState(!!s.scanning,s.scanning?'Scanning…':'');"
          "if(s.scan!==scan){scan=s.scan;if(toggle&&toggle.checked)repaint();}"
        "}).catch(function(){});"
      "},2000);"
      "})();</script>";
    server.sendContent(refreshScript);
  }

  server.sendContent(
    "</div></body></html>"
  );

  server.sendContent("");
}


void handleWifiObservedFragment() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", "");
  server.sendContent(
    "<h2>Observed Networks</h2><div class=\"note\">"
    "One row is shown for each BSSID observed during this session. "
    "Click any column header to sort the table. Click an SSID or BSSID "
    "to redraw the RSSI plot for that access point.</div>"
  );
  sendNetworkSummaryTable();
  server.sendContent("");
}

void handleWifiPlotFragment() {
  String selectedBSSID = server.hasArg("bssid") ? server.arg("bssid") : "";
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", "");
  server.sendContent("<h2>RSSI History</h2>");

  if (selectedBSSID.length() > 0) {
    NetworkSummary summary = {};
    if (buildNetworkSummary(selectedBSSID, summary)) {
      server.sendContent(
        "<div class=\"row\"><span class=\"label\">Selected Network</span><span class=\"value\">" +
        htmlEscape(String(summary.ssid)) +
        "</span></div><div class=\"row\"><span class=\"label\">Selected BSSID</span><span class=\"value\">" +
        htmlEscape(selectedBSSID) + "</span></div>"
      );
      sendRssiHistoryPlot(selectedBSSID);
    } else {
      server.sendContent("<p>The selected network is no longer retained.</p>");
    }
  } else {
    server.sendContent("<p>Select an SSID or BSSID below to display RSSI history.</p>");
  }
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

void handleBleScanStatus() {
  String json = "{\"scan\":" + String(bleScanCounter) +
                ",\"records\":" + String(bleHistoryCount) + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
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
  server.sendContent("<h1>Bluetooth Survey <span class=\"badge\">Limited Mode</span></h1>"
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
    String(bleHistoryCount) + " / " + String(bleHistoryRetentionLimit) +
    " retained; " + String(bleHistoryCapacity) + " physical</span></div>";
  status += "<div class=\"row\"><span class=\"label\">History RAM</span><span class=\"value\">" +
    String(bleHistoryAllocatedBytes() / 1024.0, 1) + " KB total</span></div>";
  status += "<div class=\"row\"><span class=\"label\">BLE Observation Size</span><span class=\"value\">" +
    String(sizeof(BleObservation)) + " bytes (previous flat record was " + String(sizeof(BleScanRecord)) + " bytes)</span></div>";
  status += "<div class=\"row\"><span class=\"label\">BLE Address Table</span><span class=\"value\">" +
    String(countReferencedBleAddresses()) + " / " + String(bleAddressTableCapacity) + " referenced; peak " +
    String(bleAddressPeakReferenced) + "; " + String(bleAddressTableCapacity*sizeof(BleAddressEntry)/1024.0,1) + " KB allocated</span></div>";
  status += "<div class=\"row\"><span class=\"label\">BLE Scan Metadata</span><span class=\"value\">" +
    String(countReferencedBleScanSlots()) + " / " + String(bleScanMetadataCapacity) + " referenced; peak " +
    String(bleScanMetadataPeakUsed) + "; " + String(bleScanMetadataCapacity*sizeof(SurveyScanMetadata)/1024.0,1) + " KB allocated</span></div>";
  status += "<div class=\"row\"><span class=\"label\">Dropped BLE Observations</span><span class=\"value\">" +
    String(bleAddressTableFullDrops) + " (address table full)</span></div>";
  status += "<div class=\"row\"><span class=\"label\">Last Scan</span><span class=\"value\">";
  status += bleScanCounter == 0 ? "Never" : formatUptime(lastBleScanUptimeMs) + " uptime";
  status += "</span></div>";

  status += "<form class=\"settings-row\" action=\"/ble-settings\" method=\"get\">"
    "<div class=\"control\"><label for=\"ble-interval\">Interval (seconds)</label>"
    "<input id=\"ble-interval\" name=\"interval\" type=\"number\" min=\"5\" max=\"3600\" value=\"" +
    String(bleScanIntervalSeconds) + "\"></div>"
    "<div class=\"control\"><label>History capacity</label><div class=\"value\">" +
    String(bleHistoryCapacity) + " observations (automatic maximum)</div></div>"
    "<div class=\"checkbox-stack\">"
    "<label><input type=\"checkbox\" name=\"auto\" value=\"1\"";

  if (autoBleScanEnabled) status += " checked";

  status += "> Automatic scanning</label></div><button type=\"submit\">Apply</button></form>"
    "<div class=\"buttons\"><a class=\"button\" href=\"/ble-scan\">Scan Now</a>"
    "<a class=\"button\" href=\"/blelog.csv\">Download CSV</a>"
    "<a class=\"button\" href=\"/ble-clear\">Clear History</a>"
    "<a class=\"button\" href=\"/ble\">Refresh Page</a></div>"
    "<div class=\"note\">Automatic BLE scanning defaults to 300 seconds. "
    "Physical capacity is allocated once at boot; the retention limit changes immediately without reallocating RAM. ""Combined BLE mode is intentionally a limited option on this ESP32 because the BLE stack substantially reduces heap and retained-history capacity. ""BLE scans still use the synchronous Arduino BLE API and may briefly pause web servicing while a BLE scan is active.</div>";

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

  server.sendContent("</div><div class=\"card\" id=\"ble-observed-card\"><h2>Observed BLE Devices</h2>"
    "<div class=\"note\">One row per retained BLE address. Click a column header to sort.</div>");
  sendBleSummaryTable();
  server.sendContent("</div><div class=\"footer\">ESP32 Web Interface</div>");
  sendSortableTableScript();
  sendThemeScript();
  {
    String refreshScript =
      "<script>(function(){"
      "let scan=" + String(bleScanCounter) + ";"
      "const toggle=document.getElementById('live-updates-toggle');"
      "const address='" + jsEscape(selectedAddress) + "';"
      "let updating=false;"
      "async function repaint(){if(updating)return;updating=true;try{"
        "const jobs=[fetch('/api/ble/observed',{cache:'no-store'}).then(r=>r.text()).then(h=>{const e=document.getElementById('ble-observed-card');if(e)e.innerHTML=h;})];"
        "if(address){jobs.push(fetch('/api/ble/plot?address='+encodeURIComponent(address),{cache:'no-store'}).then(r=>r.text()).then(h=>{const e=document.getElementById('rssi-plot');if(e)e.innerHTML=h;}));}"
        "await Promise.all(jobs);"
      "}catch(e){}finally{updating=false;}}"
      "setInterval(function(){if(!toggle||!toggle.checked)return;"
        "fetch('/api/ble/status',{cache:'no-store'}).then(r=>r.json()).then(function(s){if(s.scan!==scan){scan=s.scan;repaint();}}).catch(function(){});"
      "},2000);"
      "})();</script>";
    server.sendContent(refreshScript);
  }
  server.sendContent("</div></body></html>");
  server.sendContent("");
}


void handleBleObservedFragment() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", "");
  server.sendContent("<h2>Observed BLE Devices</h2><div class=\"note\">One row per retained BLE address. Click a column header to sort.</div>");
  sendBleSummaryTable();
  server.sendContent("");
}

void handleBlePlotFragment() {
  String selectedAddress = server.hasArg("address") ? server.arg("address") : "";
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", "");
  server.sendContent("<h2>RSSI History</h2>");
  if (selectedAddress.length()) {
    String selectedName = latestBleNameForAddress(selectedAddress);
    server.sendContent("<div class=\"row\"><span class=\"label\">Selected Device</span><span class=\"value\">" +
      htmlEscape(selectedName) + "</span></div><div class=\"row\"><span class=\"label\">BLE Address</span><span class=\"value\">" +
      htmlEscape(selectedAddress) + "</span></div>");
    sendBleRssiHistoryPlot(selectedAddress);
  } else {
    server.sendContent("<p>Select a BLE address below to display RSSI history.</p>");
  }
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
  s += "<div class=\"row\"><span class=\"label\">Survey Memory Mode</span><span class=\"value\">" + String(bleSurveyEnabled ? "Wi-Fi + BLE (limited)" : "Wi-Fi only") + "</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Target Heap Reserve</span><span class=\"value\">" + String((bleSurveyEnabled ? DUAL_RADIO_HEAP_RESERVE_BYTES : HISTORY_HEAP_RESERVE_BYTES)/1024) + " KB at history allocation</span></div>";
  if (bleSurveyEnabled) {
    s += "<div class=\"row\"><span class=\"label\">Dual-Radio Low-Water Warning</span><span class=\"value\">" + String(DUAL_RADIO_MIN_HEAP_WARN_BYTES/1024) + " KB minimum free heap</span></div>";
  }
  s += "<div class=\"row\"><span class=\"label\">Wi-Fi History</span><span class=\"value\">" + String(historyCount) + " / " + String(scanHistoryRetentionLimit) + " retained; " + String(scanHistoryCapacity) + " history capacity; " + String(wifiHistoryAllocatedBytes()/1024.0,1) + " KB total</span></div>";
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
    s += "<div class=\"row\"><span class=\"label\">BLE History</span><span class=\"value\">" + String(bleHistoryCount) + " / " + String(bleHistoryRetentionLimit) + " retained; " + String(bleHistoryCapacity) + " capacity; " + String(bleHistoryAllocatedBytes()/1024.0,1) + " KB total</span></div>";
    s += "<div class=\"row\"><span class=\"label\">BLE Observation Size</span><span class=\"value\">" + String(sizeof(BleObservation)) + " bytes (previous flat record was " + String(sizeof(BleScanRecord)) + " bytes)</span></div>";
    s += "<div class=\"row\"><span class=\"label\">BLE Address Table</span><span class=\"value\">" + String(countReferencedBleAddresses()) + " / " + String(bleAddressTableCapacity) + " referenced; peak " + String(bleAddressPeakReferenced) + "; drops " + String(bleAddressTableFullDrops) + "</span></div>";
    s += "<div class=\"row\"><span class=\"label\">BLE Scan Metadata</span><span class=\"value\">" + String(countReferencedBleScanSlots()) + " / " + String(bleScanMetadataCapacity) + " referenced; peak " + String(bleScanMetadataPeakUsed) + "</span></div>";
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
  s += "<div class=\"row\"><span class=\"label\">mDNS Hostname</span><span class=\"value\">" + htmlEscape(mdnsHostname) + ".local</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Friendly Web Address</span><span class=\"value\"><a href=\"" + htmlEscape(mdnsWebAddress()) + "\">" + htmlEscape(mdnsWebAddress()) + "</a></span></div>";
  s += "<div class=\"row\"><span class=\"label\">mDNS Status</span><span class=\"value\">" + htmlEscape(mdnsStatusMessage) + "</span></div>";
  s += "<div class=\"row\"><span class=\"label\">BLE Boot Mode</span><span class=\"value\">" + String(bleSurveyEnabled ? "Enabled" : "Disabled (maximum Wi-Fi history)") + "</span></div>";
  s += "<div class=\"row\"><span class=\"label\">Status LED</span><span class=\"value\">" + (STATUS_LED_AVAILABLE ? (statusLedEnabled ? String("GPIO ") + String(STATUS_LED_PIN) + String(" enabled") : String("Disabled by setting")) : String("Not available")) + "</span></div></div>";
  server.sendContent(s);

  server.sendContent("<div class=\"card\"><h2>Boot Heap Checkpoints</h2>"
    "<div class=\"note\">Captured during startup so Wi-Fi, BLE, history, web-server, mDNS, and initial-scan RAM costs can be compared directly.</div>"
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
      (bleHistory && bleAddressTable && bleScanMetadata &&
       bleHistoryCapacity>=MIN_BLE_HISTORY_RECORDS &&
       bleHistoryRetentionLimit>=MIN_BLE_HISTORY_RECORDS &&
       bleHistoryRetentionLimit<=bleHistoryCapacity &&
       bleHistoryCount<=bleHistoryRetentionLimit);
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
  bool autos = !bleSurveyEnabled || autoBleScanEnabled;
  server.sendContent(selfTestRow("Headless automatic surveying", autos ? "PASS" : "WARN",
    autos ? (bleSurveyEnabled ? "Wi-Fi and BLE periodic scanning enabled" : "Wi-Fi periodic scanning enabled; Bluetooth Survey disabled")
          : "BLE automatic scanning is disabled at runtime"));
  bool wifiCadenceOverdue = wifiAutoScanCadenceOverdue();
  server.sendContent(selfTestRow("Wi-Fi auto-scan cadence", wifiCadenceOverdue ? "WARN" : "PASS",
    wifiAutoScanDiagnosticLabel() + "; starts " + String(wifiAutoScanStartCount) +
    ", completions " + String(wifiAutoScanCompletionCount) +
    ", start failures " + String(wifiAutoScanStartFailureCount)));
  server.sendContent(selfTestRow("mDNS hostname", mdnsStarted ? "PASS" : "WARN",
    mdnsStarted ? mdnsWebAddress() : mdnsStatusMessage));
  {
    uint32_t minFreeHeap = ESP.getMinFreeHeap();
    bool currentHeapOk = bleSurveyEnabled
      ? freeHeap >= 24 * 1024
      : freeHeap >= HEAP_WARN_BYTES;
    bool lowWaterOk = bleSurveyEnabled
      ? minFreeHeap >= DUAL_RADIO_MIN_HEAP_WARN_BYTES
      : minFreeHeap >= HEAP_WARN_BYTES;
    String heapDetail =
      String(freeHeap/1024) + " KB free; " +
      String(minFreeHeap/1024) + " KB minimum";
    server.sendContent(
      selfTestRow(
        "Heap reserve",
        (currentHeapOk && lowWaterOk) ? "PASS" : "WARN",
        heapDetail
      )
    );
  }
  server.sendContent(selfTestRow("Application space", unusedAppBytes>64*1024 ? "PASS" : "WARN", String(unusedAppBytes/1024) + " KB unused in running app partition"));
  bool resetWarn = rr==ESP_RST_PANIC || rr==ESP_RST_INT_WDT || rr==ESP_RST_TASK_WDT || rr==ESP_RST_WDT || rr==ESP_RST_BROWNOUT;
  server.sendContent(selfTestRow("Boot/reset diagnostic", resetWarn ? "WARN" : "PASS", resetReasonLabel(rr)));
  server.sendContent("<div class=\"note\">WARN indicates a nonfatal condition worth reviewing. No filesystem self-test is included because persistent logging/filesystem support is intentionally not part of this firmware. In dual-radio mode the BLE stack and fixed compact tables may intentionally leave less than the Wi-Fi-only heap margin.</div></div>");
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
  s += "<div class=\"card\"><h2>Network Identity</h2>"
    "<div class=\"row\"><span class=\"label\">mDNS Hostname</span><span class=\"value\">" + htmlEscape(mdnsHostname) + ".local</span></div>"
    "<div class=\"row\"><span class=\"label\">Friendly Web Address</span><span class=\"value\">" + htmlEscape(mdnsWebAddress()) + "</span></div>"
    "<div class=\"row\"><span class=\"label\">mDNS Status</span><span class=\"value\">" + htmlEscape(mdnsStatusMessage) + "</span></div>"
    "<form class=\"controls\" action=\"/hostname-save\" method=\"post\">"
    "<div class=\"control\"><label for=\"mdns-hostname\">Hostname</label><input id=\"mdns-hostname\" name=\"hostname\" type=\"text\" maxlength=\"32\" value=\"" + htmlEscape(mdnsHostname) + "\" required></div>"
    "<button type=\"submit\">Save Hostname &amp; Restart</button></form>"
    "<div class=\"note\">Use letters, numbers, and hyphens only. The name cannot begin or end with a hyphen. The normal friendly address is &lt;hostname&gt;.local. mDNS support on a directly connected ESP32 access point can vary by client, so the displayed IP addresses remain the fallback.</div></div>";
  s += "<div class=\"card\"><h2>Survey Access Point</h2><div class=\"row\"><span class=\"label\">Status</span><span class=\"value\">" + String(apRunning ? "Running" : "Disabled") + "</span></div><div class=\"row\"><span class=\"label\">SSID</span><span class=\"value\">" + htmlEscape(apSSID) + "</span></div><div class=\"buttons\"><a class=\"button\" href=\"/ap\">Access Point Settings</a></div></div>";
  s += "<div class=\"card\"><h2>Survey Mode</h2>"
    "<div class=\"row\"><span class=\"label\">Bluetooth Survey</span><span class=\"value\">" +
    String(bleSurveyEnabled ? "Enabled" : "Disabled - maximum Wi-Fi history") + "</span></div>"
    "<form class=\"controls\" action=\"/ble-mode\" method=\"post\">"
    "<input type=\"hidden\" name=\"enabled\" value=\"" + String(bleSurveyEnabled ? "0" : "1") + "\">"
    "<button type=\"submit\">" + String(bleSurveyEnabled ? "Disable Bluetooth Survey" : "Enable Bluetooth Survey") + "</button></form>"
    "<div class=\"note\">Bluetooth is disabled by default to maximize Wi-Fi history depth. Combined BLE surveying is a limited mode on this hardware because the BLE stack substantially reduces heap and retained-history capacity. Changing this setting is stored in NVS and restarts the ESP32. "
    "The restart is intentional: BLE uses a large persistent heap allocation, so survey histories must be sized after the selected radio mode is established.</div></div>";
  s += "<div class=\"card\"><h2>Interface &amp; Indicators</h2>"
    "<form class=\"controls\" action=\"/interface-settings\" method=\"post\">"
    "<div class=\"control\"><label><input type=\"checkbox\" name=\"ledEnabled\" value=\"1\" " + String(statusLedEnabled ? "checked" : "") + "> Enable status LED indicators</label></div>"
    "<button type=\"submit\">Save Interface Settings</button></form>"
    "<form class=\"controls\" action=\"/led-test\" method=\"post\"><button type=\"submit\">Test Status LED</button></form>"
    "<div class=\"row\"><span class=\"label\">Status LED</span><span class=\"value\">" + String(STATUS_LED_AVAILABLE ? (statusLedEnabled ? "Enabled on GPIO 2" : "Disabled by setting") : "Not available") + "</span></div>"
    "<div class=\"row\"><span class=\"label\">Survey Page Live Updates</span><span class=\"value\">" + String(webAutoRefreshEnabled ? "Enabled" : "Disabled") + "</span></div>"
    "<div class=\"note\">The LED setting is saved here. Live Updates are controlled immediately from the checkbox in the page header and are also stored in NVS. Theme selection remains browser-local.</div></div>";
  server.sendContent(s);
  server.sendContent("<div class=\"footer\">ESP32 Web Interface</div>");
  sendThemeScript();
  server.sendContent("</div></body></html>");
  server.sendContent("");
}

void handleHostnameSave() {
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
  delay(750);
  ESP.restart();
}

void handleInterfaceSettings() {
  // Live Updates are managed by the persistent header control. Saving the LED
  // setting must not silently overwrite that independently managed preference.
  bool requestedLed = server.hasArg("ledEnabled");
  saveInterfaceSettings(requestedLed, webAutoRefreshEnabled);
  if (!statusLedEnabled) stopScanLed();
  server.sendHeader("Location", "/settings");
  server.send(303, "text/plain", "Interface settings saved.");
}

void handleLedSelfTest() {
  runStatusLedSelfTest();
  server.sendHeader("Location", "/settings");
  server.send(303, "text/plain", "Status LED self-test complete.");
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
    " after restart.</p><p>The ESP32 is restarting now. This page will reconnect automatically.</p>"
    "<p id=\"reconnect-status\">Waiting for the surveyor...</p></div></div>"
    "<script>(function(){setTimeout(function retry(){fetch('/ble',{cache:'no-store'}).then(function(r){"
    "if(r.ok){location.replace('/ble');return;}setTimeout(retry,1000);"
    "}).catch(function(){setTimeout(retry,1000);});},2500);})();</script></body></html>");

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
  server.on("/hostname-save", HTTP_POST, handleHostnameSave);
  server.on("/interface-settings", HTTP_POST, handleInterfaceSettings);
  server.on("/led-test", HTTP_POST, handleLedSelfTest);
  server.on("/scan-now", handleWebScanNow);
  server.on("/api/wifi/status", HTTP_GET, handleWifiScanStatus);
  server.on("/api/wifi/observed", HTTP_GET, handleWifiObservedFragment);
  server.on("/api/wifi/plot", HTTP_GET, handleWifiPlotFragment);
  server.on("/api/live-updates", HTTP_POST, handleLiveUpdatesSetting);
  server.on("/api/wifi/interval", HTTP_POST, handleWifiIntervalSetting);
  server.on("/scan-settings", handleScanSettings);
  server.on("/scan-clear", handleClearScanHistory);
  server.on("/scanlog.csv", handleScanCsv);
  server.on("/ble", HTTP_GET, handleBLESurvey);
  server.on("/api/ble/status", HTTP_GET, handleBleScanStatus);
  server.on("/api/ble/observed", HTTP_GET, handleBleObservedFragment);
  server.on("/api/ble/plot", HTTP_GET, handleBlePlotFragment);
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
// Serial interface - text-oriented parity with the web UI
// ============================================================

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
  Serial.println();
  Serial.println("h/help - Show this menu");
  Serial.println("restart - Restart ESP32");
  Serial.println();
  Serial.print("> ");
}

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

void printBluetoothSurveySerial() {
  Serial.println();
  Serial.println("============================================================");
  Serial.println(" Bluetooth Survey");
  Serial.println("============================================================");
  Serial.print("Bluetooth Survey:     ");
  Serial.println(bleSurveyEnabled ? "Enabled" : "Disabled (Wi-Fi-first mode)");
  if (bleSurveyEnabled) {
    Serial.print("BLE initialized:      ");
    Serial.println(bleInitialized ? "Yes" : "No");
    Serial.print("Automatic scanning:  ");
    Serial.println(autoBleScanEnabled ? "Enabled" : "Disabled");
    Serial.print("Scan interval:       ");
    Serial.print(bleScanIntervalSeconds);
    Serial.println(" s");
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
    Serial.println("  bleauto on|off       - Enable/disable automatic BLE scans");
    Serial.println("  ble off              - Disable BLE Survey and restart");
  } else {
    Serial.println();
    Serial.println("BLE is not initialized, preserving approximately 83 KB of heap for Wi-Fi surveying.");
    Serial.println("  ble on               - Enable BLE Survey and restart");
  }
  Serial.println("  0/back               - Main menu");
  Serial.println();
  Serial.print("> ");
}

void printSystemSerial() {
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t minHeap = ESP.getMinFreeHeap();
  uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const esp_partition_t* running = esp_ota_get_running_partition();
  size_t appBytes = running ? running->size : 0;
  size_t unused = appBytes > ESP.getSketchSize() ? appBytes - ESP.getSketchSize() : 0;

  Serial.println();
  Serial.println("============================================================");
  Serial.println(" System");
  Serial.println("============================================================");
  Serial.print("Firmware:             "); Serial.print(FIRMWARE_FILE); Serial.print(" (V"); Serial.print(FIRMWARE_VERSION); Serial.println(")");
  Serial.print("Built:                "); Serial.println(firmwareBuildTimestamp());
  Serial.print("Arduino ESP32 core:   "); Serial.println(ESP_ARDUINO_VERSION_STR);
  Serial.print("ESP-IDF:              "); Serial.println(esp_get_idf_version());
  Serial.print("Uptime:               "); Serial.println(getUptimeString());
  Serial.print("Last reset:           "); Serial.println(resetReasonLabel(esp_reset_reason()));
  Serial.print("Chip:                 "); Serial.print(ESP.getChipModel()); Serial.print(" rev "); Serial.println(ESP.getChipRevision());
  Serial.print("CPU / cores:          "); Serial.print(ESP.getCpuFreqMHz()); Serial.print(" MHz / "); Serial.println(ESP.getChipCores());
  Serial.print("Flash size:           "); Serial.print(ESP.getFlashChipSize()/1024.0/1024.0, 2); Serial.println(" MB");
  Serial.print("Sketch size:          "); Serial.print(ESP.getSketchSize()/1024.0, 1); Serial.println(" KB");
  Serial.print("App partition:        "); Serial.print(appBytes/1024.0, 1); Serial.println(" KB");
  Serial.print("Unused app partition: "); Serial.print(unused/1024.0, 1); Serial.println(" KB");
  Serial.print("Free heap:            "); Serial.print(freeHeap/1024.0, 1); Serial.println(" KB");
  Serial.print("Minimum free heap:    "); Serial.print(minHeap/1024.0, 1); Serial.println(" KB");
  Serial.print("Largest free block:   "); Serial.print(largest/1024.0, 1); Serial.println(" KB");
  Serial.print("Survey memory mode:   "); Serial.println(bleSurveyEnabled ? "Wi-Fi + BLE (limited)" : "Wi-Fi only");
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
  Serial.print("Boot/reset diagnostic:     "); Serial.println(resetReasonLabel(esp_reset_reason()));
  Serial.println();
}

void printSettingsSerial() {
  Serial.println();
  Serial.println("============================================================");
  Serial.println(" Settings");
  Serial.println("============================================================");
  Serial.print("Infrastructure Wi-Fi: "); Serial.println(WiFi.status()==WL_CONNECTED ? "Connected" : "Disconnected");
  if (WiFi.status()==WL_CONNECTED) { Serial.print("  SSID:              "); Serial.println(WiFi.SSID()); }
  Serial.print("Survey AP:             "); Serial.println(apRunning ? "Running" : "Disabled");
  Serial.print("  SSID:                "); Serial.println(apSSID);
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
  Serial.println("  ap on|off             - Enable/disable Survey AP and restart");
  Serial.println("  apssid <name>         - Change Survey AP SSID and restart");
  Serial.println("  appass <password>     - Change Survey AP password and restart");
  Serial.println("  0/back                - Main menu");
  Serial.println();
  Serial.print("> ");
}

void printMenu() { printSerialMainMenu(); }

String commandArgument(const String& command, const String& prefix) {
  if (command.length() <= prefix.length()) return "";
  String value = command.substring(prefix.length());
  value.trim();
  return value;
}

void handleSerialCommand() {
  if (!Serial.available()) return;

  String command = Serial.readStringUntil('\n');
  command.trim();
  if (command.length() == 0) { printSerialMainMenu(); return; }

  if (command == "1" || command.equalsIgnoreCase("wifi-survey")) { printWifiSurveySerial(); return; }
  if (command == "2" || command.equalsIgnoreCase("bluetooth") || command.equalsIgnoreCase("ble-survey")) { printBluetoothSurveySerial(); return; }
  if (command == "3" || command.equalsIgnoreCase("system")) { printSystemSerial(); return; }
  if (command == "4" || command.equalsIgnoreCase("settings")) { printSettingsSerial(); return; }
  if (command == "0" || command.equalsIgnoreCase("back") || command.equalsIgnoreCase("h") || command.equalsIgnoreCase("help")) { printSerialMainMenu(); return; }

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
    if (!bleSurveyEnabled) Serial.println("Bluetooth Survey is disabled. Use 'ble on' to enable it and restart.");
    else performLoggedBLEScan();
    printBluetoothSurveySerial(); return;
  }
  if (command.equalsIgnoreCase("bleclear")) { if (bleSurveyEnabled) clearBleHistory(); Serial.println("BLE history cleared."); printBluetoothSurveySerial(); return; }

  if (command.startsWith("bleinterval ")) {
    long n=commandArgument(command,"bleinterval").toInt();
    if (!bleSurveyEnabled) Serial.println("Bluetooth Survey is disabled.");
    else if (n < (long)MIN_SCAN_INTERVAL_SECONDS || n > (long)MAX_SCAN_INTERVAL_SECONDS) Serial.println("Invalid BLE interval.");
    else { bleScanIntervalSeconds=(unsigned long)n; lastAutoBleScanMs=millis(); Serial.println("BLE scan interval updated."); }
    printBluetoothSurveySerial(); return;
  }
  if (command.equalsIgnoreCase("bleauto on")) { if (bleSurveyEnabled) { autoBleScanEnabled=true; lastAutoBleScanMs=millis(); } printBluetoothSurveySerial(); return; }
  if (command.equalsIgnoreCase("bleauto off")) { if (bleSurveyEnabled) autoBleScanEnabled=false; printBluetoothSurveySerial(); return; }
  if (command.equalsIgnoreCase("ble on") || command.equalsIgnoreCase("ble off")) {
    bool requested=command.endsWith("on");
    if (requested==bleSurveyEnabled) { Serial.println("Bluetooth Survey mode already set."); printSettingsSerial(); return; }
    saveBleSurveyEnabled(requested);
    Serial.print("Bluetooth Survey will be "); Serial.print(requested?"enabled":"disabled"); Serial.println(" after restart. Restarting...");
    delay(500); ESP.restart(); return;
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
    delay(500); ESP.restart(); return;
  }

  if (command.equalsIgnoreCase("ap on") || command.equalsIgnoreCase("ap off")) {
    bool requested=command.endsWith("on"); saveAccessPointSettings(requested,apSSID,apPassword);
    Serial.println("Survey AP setting saved. Restarting..."); delay(500); ESP.restart(); return;
  }
  if (command.startsWith("apssid ")) {
    String requested=commandArgument(command,"apssid"); requested.trim();
    if (requested.length()==0 || requested.length()>32) { Serial.println("AP SSID must be 1-32 characters."); printSettingsSerial(); return; }
    saveAccessPointSettings(apEnabled,requested,apPassword); Serial.println("AP SSID saved. Restarting..."); delay(500); ESP.restart(); return;
  }
  if (command.startsWith("appass ")) {
    String requested=commandArgument(command,"appass");
    if (requested.length()<8 || requested.length()>63) { Serial.println("AP password must be 8-63 characters."); printSettingsSerial(); return; }
    saveAccessPointSettings(apEnabled,apSSID,requested); Serial.println("AP password saved. Restarting..."); delay(500); ESP.restart(); return;
  }

  if (command.equalsIgnoreCase("version") || command.equalsIgnoreCase("v")) { printFirmwareInfo(); Serial.print("> "); return; }
  if (command.equalsIgnoreCase("mac")) { printMacAddress(); Serial.print("> "); return; }
  if (command.equalsIgnoreCase("restart")) { Serial.println("Restarting ESP32..."); delay(500); ESP.restart(); return; }

  Serial.print("Unknown command: "); Serial.println(command);
  Serial.println("Enter 'h' for the main menu.");
  Serial.print("> ");
}

// ============================================================
// Setup
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(1500);

  captureBootHeapCheckpoint("Startup");
  loadSurveyModeSettings();
  captureBootHeapCheckpoint("Settings loaded");
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
  applyGeneratedDefaultMdnsHostname();
  captureBootHeapCheckpoint("Wi-Fi initialized");

  loadAccessPointSettings();
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

void serviceInitialSurveyScans() {
  unsigned long elapsed = millis() - surveyServicesReadyMs;

  if (
    initialWifiScanPending &&
    elapsed >= INITIAL_WIFI_SCAN_DELAY_MS &&
    !wifiScanInProgress
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
    !wifiScanInProgress
  ) {
    initialBleScanPending = false;
    Serial.println("Initial headless BLE survey scan (limited mode)...");
    performLoggedBLEScan();
    lastAutoBleScanMs = millis();
    captureBootHeapCheckpoint("Initial BLE scan");
  }
}

void serviceAutomaticScan() {
  if (wifiScanInProgress) return;

  unsigned long intervalMs = scanIntervalSeconds * 1000UL;
  if (millis() - lastAutoScanMs < intervalMs) return;

  beginLoggedWifiScan(false, true);
}

void serviceAutomaticBLEScan() {
  if (!bleSurveyEnabled || !autoBleScanEnabled || wifiScanInProgress) return;

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

  serviceLoggedWifiScan();
  serviceInitialSurveyScans();
  serviceAutomaticScan();
  serviceAutomaticBLEScan();
  handleSerialCommand();

  delay(5);
}
