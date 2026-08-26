// WifiConnect7 - automatic scanning enabled by default
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

Preferences preferences;
WebServer server(80);

const unsigned long WIFI_TIMEOUT_MS = 15000;
const unsigned long WIFI_STARTUP_SETTLE_MS = 300;

bool webServerStarted = false;


// ============================================================
// Scan logger
// ============================================================

// Scan history is intentionally RAM-only. It is cleared on power cycle/reset.
// The same SSID/BSSID may appear in many records with different scan numbers
// and timestamps, which is useful for signal-strength trending.
const size_t DEFAULT_SCAN_HISTORY_RECORDS = 300;
const size_t MIN_SCAN_HISTORY_RECORDS = 50;
const size_t MAX_SCAN_HISTORY_RECORDS = 2000;

const unsigned long MIN_SCAN_INTERVAL_SECONDS = 5;
const unsigned long MAX_SCAN_INTERVAL_SECONDS = 3600;

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

ScanRecord* scanHistory = nullptr;
size_t scanHistoryCapacity = 0;

size_t historyStart = 0;
size_t historyCount = 0;

String historyResizeMessage = "";

uint32_t scanCounter = 0;
uint32_t lastScanUptimeMs = 0;

bool autoScanEnabled = true;
unsigned long scanIntervalSeconds = 300;
unsigned long lastAutoScanMs = 0;



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

void ensureWiFiStationMode() {
  if (WiFi.getMode() == WIFI_OFF) {
    WiFi.mode(WIFI_STA);
    delay(WIFI_STARTUP_SETTLE_MS);
  }
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

const ScanRecord& historyRecord(size_t logicalIndex) {
  size_t physicalIndex =
      (historyStart + logicalIndex) % scanHistoryCapacity;

  return scanHistory[physicalIndex];
}

size_t countRetainedScanGroups() {
  if (historyCount == 0 || scanHistory == nullptr) {
    return 0;
  }

  size_t groups = 0;
  uint32_t previousScan = 0;
  bool havePrevious = false;

  for (size_t i = 0; i < historyCount; i++) {
    uint32_t currentScan = historyRecord(i).scanNumber;

    if (!havePrevious || currentScan != previousScan) {
      groups++;
      previousScan = currentScan;
      havePrevious = true;
    }
  }

  return groups;
}

bool resizeScanHistory(size_t requestedCapacity, bool preserveRecords = true) {
  if (requestedCapacity < MIN_SCAN_HISTORY_RECORDS) {
    requestedCapacity = MIN_SCAN_HISTORY_RECORDS;
  }

  if (requestedCapacity > MAX_SCAN_HISTORY_RECORDS) {
    requestedCapacity = MAX_SCAN_HISTORY_RECORDS;
  }

  if (scanHistory != nullptr && requestedCapacity == scanHistoryCapacity) {
    historyResizeMessage =
      "History limit unchanged at " +
      String(scanHistoryCapacity) +
      " records.";
    return true;
  }

  ScanRecord* newHistory =
      (ScanRecord*)malloc(requestedCapacity * sizeof(ScanRecord));

  if (newHistory == nullptr) {
    historyResizeMessage =
      "Unable to allocate " +
      String(requestedCapacity) +
      " records; previous limit retained.";
    return false;
  }

  size_t recordsToKeep = 0;

  if (preserveRecords && scanHistory != nullptr && historyCount > 0) {
    recordsToKeep =
      historyCount < requestedCapacity ? historyCount : requestedCapacity;

    size_t firstLogicalRecord = historyCount - recordsToKeep;

    for (size_t i = 0; i < recordsToKeep; i++) {
      newHistory[i] = historyRecord(firstLogicalRecord + i);
    }
  }

  if (scanHistory != nullptr) {
    free(scanHistory);
  }

  scanHistory = newHistory;
  scanHistoryCapacity = requestedCapacity;
  historyStart = 0;
  historyCount = recordsToKeep;

  historyResizeMessage =
    "History limit set to " +
    String(scanHistoryCapacity) +
    " records.";

  return true;
}

void initializeScanHistory() {
  if (scanHistory != nullptr) {
    return;
  }

  if (!resizeScanHistory(DEFAULT_SCAN_HISTORY_RECORDS, false)) {
    resizeScanHistory(MIN_SCAN_HISTORY_RECORDS, false);
  }

  historyResizeMessage = "";
}

void appendScanRecord(const ScanRecord& record) {
  if (scanHistory == nullptr || scanHistoryCapacity == 0) {
    return;
  }

  size_t writeIndex;

  if (historyCount < scanHistoryCapacity) {
    writeIndex =
      (historyStart + historyCount) % scanHistoryCapacity;
    historyCount++;
  } else {
    writeIndex = historyStart;
    historyStart =
      (historyStart + 1) % scanHistoryCapacity;
  }

  scanHistory[writeIndex] = record;
}

void clearScanHistory() {
  historyStart = 0;
  historyCount = 0;
  scanCounter = 0;
  lastScanUptimeMs = 0;
}

int performLoggedScan() {
  ensureWiFiStationMode();

  bool connectedNow = WiFi.status() == WL_CONNECTED;
  String connectedBSSID = connectedNow ? WiFi.BSSIDstr() : "";

  int networkCount = WiFi.scanNetworks();

  scanCounter++;
  lastScanUptimeMs = millis();

  if (networkCount <= 0) {
    return networkCount;
  }

  for (int i = 0; i < networkCount; i++) {
    ScanRecord record = {};

    record.scanNumber = scanCounter;
    record.uptimeMs = lastScanUptimeMs;

    String ssid = WiFi.SSID(i);
    String bssid = WiFi.BSSIDstr(i);

    ssid.toCharArray(record.ssid, sizeof(record.ssid));
    bssid.toCharArray(record.bssid, sizeof(record.bssid));

    record.rssi = WiFi.RSSI(i);
    record.channel = WiFi.channel(i);
    record.authMode = (uint8_t)WiFi.encryptionType(i);
    record.connected =
        connectedNow &&
        connectedBSSID.length() > 0 &&
        bssid.equalsIgnoreCase(connectedBSSID);

    record.hidden = (ssid.length() == 0);

    appendScanRecord(record);
  }

  return networkCount;
}

void printPadded(const String& value, int width) {
  String output = value;

  if (output.length() > width) {
    if (width > 3) {
      output = output.substring(0, width - 3) + "...";
    } else {
      output = output.substring(0, width);
    }
  }

  Serial.print(output);

  for (int i = output.length(); i < width; i++) {
    Serial.print(' ');
  }
}


// ============================================================
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
}

void printMacAddress() {
  ensureWiFiStationMode();

  Serial.println();
  Serial.print("Wi-Fi MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println();
}


// ============================================================
// HTML helpers
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

String pageStyles() {
  return R"rawliteral(
<style>
  body {
    font-family: Arial, Helvetica, sans-serif;
    background: #f4f4f4;
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
    background: white;
    border-radius: 10px;
    padding: 20px;
    margin-top: 20px;
    box-shadow: 0 2px 8px rgba(0,0,0,0.15);
  }

  .row {
    display: flex;
    justify-content: space-between;
    border-bottom: 1px solid #ddd;
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
    background: #333;
    color: white;
    text-decoration: none;
    border-radius: 6px;
  }

  .buttons {
    text-align: center;
  }

  table {
    width: 100%;
    border-collapse: collapse;
    margin-top: 10px;
  }

  th, td {
    padding: 10px 8px;
    border-bottom: 1px solid #ddd;
    text-align: left;
  }

  th {
    background: #f7f7f7;
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
    background: #eef7ee;
  }

  .note {
    color: #666;
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

  .plot-wrap {
    width: 100%;
    overflow-x: auto;
  }

  .plot-wrap svg {
    width: 100%;
    min-width: 520px;
    height: auto;
  }

  .controls {
    display: flex;
    flex-wrap: wrap;
    align-items: end;
    gap: 12px;
    margin-top: 12px;
  }

  .control {
    display: flex;
    flex-direction: column;
    gap: 5px;
  }

  input[type="number"] {
    width: 100px;
    padding: 9px;
    border: 1px solid #bbb;
    border-radius: 5px;
  }

  button {
    padding: 10px 16px;
    border: 0;
    border-radius: 6px;
    background: #333;
    color: white;
    cursor: pointer;
  }

  .danger {
    background: #7a2d2d;
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
// Web status page
// ============================================================

void handleRoot() {
  ensureWiFiStationMode();

  bool connected = WiFi.status() == WL_CONNECTED;

  String statusText;
  String ssidText;
  String ipText;
  String rssiText;

  String macText = WiFi.macAddress();
  String hostnameText = WiFi.getHostname();
  String gatewayText = WiFi.gatewayIP().toString();
  String subnetText = WiFi.subnetMask().toString();

  if (connected) {
    statusText = "Connected";
    ssidText = WiFi.SSID();
    ipText = WiFi.localIP().toString();

    rssiText = String(WiFi.RSSI());
    rssiText += " dBm";
  } else {
    statusText = "Disconnected";
    ssidText = "-";
    ipText = "-";
    gatewayText = "-";
    subnetText = "-";
    rssiText = "-";
  }

  String html = R"rawliteral(
<!DOCTYPE html>
<html>

<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Status</title>
)rawliteral";

  html += pageStyles();

  html += R"rawliteral(
</head>

<body>
  <div class="container">

    <h1>ESP32 Status</h1>

    <div class="card">

      <div class="row">
        <span class="label">Wi-Fi Status</span>
        <span class="value">%STATUS%</span>
      </div>

      <div class="row">
        <span class="label">SSID</span>
        <span class="value">%SSID%</span>
      </div>

      <div class="row">
        <span class="label">IP Address</span>
        <span class="value">%IP%</span>
      </div>

      <div class="row">
        <span class="label">MAC Address</span>
        <span class="value">%MAC%</span>
      </div>

      <div class="row">
        <span class="label">Hostname</span>
        <span class="value">%HOSTNAME%</span>
      </div>

      <div class="row">
        <span class="label">Gateway</span>
        <span class="value">%GATEWAY%</span>
      </div>

      <div class="row">
        <span class="label">Subnet</span>
        <span class="value">%SUBNET%</span>
      </div>

      <div class="row">
        <span class="label">Signal</span>
        <span class="value">%RSSI%</span>
      </div>

      <div class="row">
        <span class="label">Uptime</span>
        <span class="value">%UPTIME%</span>
      </div>

    </div>

    <div class="buttons">
      <a class="button" href="/">Refresh Status</a>
      <a class="button" href="/scan-now">Scan Wi-Fi</a>
    </div>

    <div class="footer">
      ESP32 Web Interface
    </div>

  </div>
</body>
</html>
)rawliteral";

  html.replace("%STATUS%", htmlEscape(statusText));
  html.replace("%SSID%", htmlEscape(ssidText));
  html.replace("%IP%", htmlEscape(ipText));
  html.replace("%MAC%", htmlEscape(macText));
  html.replace("%HOSTNAME%", htmlEscape(hostnameText));
  html.replace("%GATEWAY%", htmlEscape(gatewayText));
  html.replace("%SUBNET%", htmlEscape(subnetText));
  html.replace("%RSSI%", htmlEscape(rssiText));
  html.replace("%UPTIME%", htmlEscape(getUptimeString()));

  server.send(200, "text/html", html);
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
  server.sendHeader("Location", "/scan");
  server.send(303, "text/plain", "");
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

    if (requestedHistory < (long)MIN_SCAN_HISTORY_RECORDS) {
      requestedHistory = MIN_SCAN_HISTORY_RECORDS;
    }

    if (requestedHistory > (long)MAX_SCAN_HISTORY_RECORDS) {
      requestedHistory = MAX_SCAN_HISTORY_RECORDS;
    }

    resizeScanHistory((size_t)requestedHistory, true);
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

void sendRssiHistoryPlot(const String& currentBssid) {
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

    if (!String(record.bssid).equalsIgnoreCase(currentBssid)) {
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
      "<p>No logged RSSI samples are available yet for the currently connected BSSID.</p>"
    );
    return;
  }

  if (lastMs <= firstMs) {
    lastMs = firstMs + 1;
  }

  server.sendContent(
    "<div class=\"plot-wrap\">"
    "<svg viewBox=\"0 0 720 280\" role=\"img\" "
    "aria-label=\"RSSI history for the currently connected access point\">"
  );

  // Background and horizontal grid lines.
  server.sendContent(
    "<rect x=\"58\" y=\"20\" width=\"642\" height=\"215\" "
    "fill=\"white\" stroke=\"#bbb\"/>"
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
    grid += "\" stroke=\"#ddd\" stroke-width=\"1\"/>";

    grid += "<text x=\"";
    grid += String(LEFT - 8);
    grid += "\" y=\"";
    grid += String(y + 4);
    grid += "\" text-anchor=\"end\" font-size=\"11\">";
    grid += String(rssi);
    grid += "</text>";

    server.sendContent(grid);
  }

  // Polyline points.
  String points;
  points.reserve(pointCount * 14);

  for (size_t i = 0; i < historyCount; i++) {
    const ScanRecord& record = historyRecord(i);

    if (!String(record.bssid).equalsIgnoreCase(currentBssid)) {
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
    "<polyline fill=\"none\" stroke=\"#333\" stroke-width=\"2\" points=\"";
  polyline += points;
  polyline += "\"/>";

  server.sendContent(polyline);

  // Individual samples.
  for (size_t i = 0; i < historyCount; i++) {
    const ScanRecord& record = historyRecord(i);

    if (!String(record.bssid).equalsIgnoreCase(currentBssid)) {
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
    dot += "\" r=\"4\" fill=\"#333\">";
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
  labels += "\" text-anchor=\"start\" font-size=\"11\">";
  labels += htmlEscape(formatUptime(firstMs));
  labels += "</text>";

  labels += "<text x=\"";
  labels += String(SVG_WIDTH - RIGHT);
  labels += "\" y=\"";
  labels += String(SVG_HEIGHT - 18);
  labels += "\" text-anchor=\"end\" font-size=\"11\">";
  labels += htmlEscape(formatUptime(lastMs));
  labels += "</text>";

  labels +=
    "<text x=\"15\" y=\"128\" transform=\"rotate(-90 15 128)\" "
    "text-anchor=\"middle\" font-size=\"12\">RSSI (dBm)</text>";

  server.sendContent(labels);
  server.sendContent("</svg></div>");
}

bool scanNumberIsRetained(uint32_t scanNumber) {
  for (size_t i = 0; i < historyCount; i++) {
    if (historyRecord(i).scanNumber == scanNumber) {
      return true;
    }
  }

  return false;
}

size_t retainedRecordCountForScan(uint32_t scanNumber) {
  size_t count = 0;

  for (size_t i = 0; i < historyCount; i++) {
    if (historyRecord(i).scanNumber == scanNumber) {
      count++;
    }
  }

  return count;
}

uint32_t retainedUptimeForScan(uint32_t scanNumber) {
  for (size_t i = 0; i < historyCount; i++) {
    if (historyRecord(i).scanNumber == scanNumber) {
      return historyRecord(i).uptimeMs;
    }
  }

  return 0;
}

void sendScanHistoryTables() {
  if (historyCount == 0) {
    server.sendContent("<p>No scans have been logged yet.</p>");
    return;
  }

  // Newest scan first.
  for (uint32_t scanNumber = scanCounter; scanNumber > 0; scanNumber--) {
    if (!scanNumberIsRetained(scanNumber)) {
      continue;
    }

    size_t recordCount = retainedRecordCountForScan(scanNumber);
    uint32_t scanUptime = retainedUptimeForScan(scanNumber);

    String heading;
    heading.reserve(220);

    heading += "<div class=\"scan-group\">";
    heading += "<div class=\"scan-heading\">Scan #";
    heading += String(scanNumber);
    heading += " &mdash; ";
    heading += htmlEscape(formatUptime(scanUptime));
    heading += " uptime &mdash; ";
    heading += String(recordCount);
    heading += " network";

    if (recordCount != 1) {
      heading += "s";
    }

    heading += "</div>";

    heading +=
      "<table><thead><tr>"
      "<th>SSID</th>"
      "<th>BSSID</th>"
      "<th class=\"signal\">CH</th>"
      "<th class=\"signal\">Signal</th>"
      "<th class=\"security\">Security</th>"
      "</tr></thead><tbody>";

    server.sendContent(heading);

    for (size_t i = 0; i < historyCount; i++) {
      const ScanRecord& record = historyRecord(i);

      if (record.scanNumber != scanNumber) {
        continue;
      }

      String row;
      row.reserve(420);

      if (record.connected) {
        row += "<tr class=\"current\">";
      } else {
        row += "<tr>";
      }

      String displaySSID =
        record.hidden ? "(hidden)" : String(record.ssid);

      row += "<td>";
      row += htmlEscape(displaySSID);

      if (record.connected) {
        row += " (connected)";
      }

      row += "</td>";

      row += "<td>";
      row += htmlEscape(String(record.bssid));
      row += "</td>";

      row += "<td class=\"signal\">";
      row += String(record.channel);
      row += "</td>";

      row += "<td class=\"signal\">";
      row += String(record.rssi);
      row += " dBm</td>";

      row += "<td class=\"security\">";
      row += htmlEscape(
        securityLabel((wifi_auth_mode_t)record.authMode)
      );
      row += "</td>";

      row += "</tr>";

      server.sendContent(row);
    }

    server.sendContent("</tbody></table></div>");
  }
}

void handleWebScan() {
  ensureWiFiStationMode();

  bool connected = WiFi.status() == WL_CONNECTED;
  String connectedSSID = connected ? WiFi.SSID() : "";
  String connectedBSSID = connected ? WiFi.BSSIDstr() : "";
  int connectedRSSI = connected ? WiFi.RSSI() : 0;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  server.sendContent(
    "<!DOCTYPE html><html><head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>ESP32 Wi-Fi Survey</title>"
  );

  server.sendContent(pageStyles());

  server.sendContent(
    "</head><body><div class=\"container\">"
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
    "<div class=\"row\"><span class=\"label\">Stored Records</span>"
    "<span class=\"value\">";
  loggingCard += String(historyCount);
  loggingCard += " / ";
  loggingCard += String(scanHistoryCapacity);
  loggingCard +=
    "</span></div>"
    "<div class=\"row\"><span class=\"label\">Scan Groups Retained</span>"
    "<span class=\"value\">";
  loggingCard += String(countRetainedScanGroups());
  loggingCard +=
    "</span></div>"
    "<div class=\"row\"><span class=\"label\">History RAM</span>"
    "<span class=\"value\">";
  loggingCard += String(
    (scanHistoryCapacity * sizeof(ScanRecord)) / 1024.0,
    1
  );
  loggingCard +=
    " KB</span></div>"
    "<div class=\"row\"><span class=\"label\">Free Heap</span>"
    "<span class=\"value\">";
  loggingCard += String(ESP.getFreeHeap() / 1024.0, 1);
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
    "<form class=\"controls\" action=\"/scan-settings\" method=\"get\">"
    "<div class=\"control\">"
    "<label for=\"interval\">Interval (seconds)</label>"
    "<input id=\"interval\" name=\"interval\" type=\"number\" "
    "min=\"5\" max=\"3600\" value=\"";
  loggingCard += String(scanIntervalSeconds);
  loggingCard +=
    "\"></div>"
    "<div class=\"control\">"
    "<label for=\"history\">History limit (records)</label>"
    "<input id=\"history\" name=\"history\" type=\"number\" "
    "min=\"50\" max=\"2000\" value=\"";
  loggingCard += String(scanHistoryCapacity);
  loggingCard +=
    "\"></div>"
    "<div class=\"control\"><label>"
    "<input type=\"checkbox\" name=\"auto\" value=\"1\"";

  if (autoScanEnabled) {
    loggingCard += " checked";
  }

  loggingCard +=
    "> Automatic scanning</label></div>"
    "<button type=\"submit\">Apply</button></form>"
    "<div class=\"buttons\">"
    "<a class=\"button\" href=\"/scan-now\">Scan Now</a>"
    "<a class=\"button\" href=\"/scanlog.csv\">Download CSV</a>"
    "<a class=\"button\" href=\"/scan-clear\">Clear History</a>"
    "</div>"
    "<div class=\"note\">"
    "Scan history is kept in RAM only and is cleared by reset or power cycle. "
    "The limit can be changed from 50 to 2000 network observations. "
    "Changing the limit preserves the newest records when possible. "
    "When full, the oldest records are overwritten."
    "</div>";

  if (historyResizeMessage.length() > 0) {
    loggingCard += "<div class=\"note\"><strong>";
    loggingCard += htmlEscape(historyResizeMessage);
    loggingCard += "</strong></div>";
  }

  loggingCard += "</div>";

  server.sendContent(loggingCard);

  server.sendContent(
    "<div class=\"card\"><h2>Connected AP RSSI History</h2>"
  );

  if (connected) {
    sendRssiHistoryPlot(connectedBSSID);

    server.sendContent(
      "<div class=\"note\">"
      "This plot follows the BSSID of the access point currently serving the ESP32. "
      "Each point is one logged scan observation. Hover a point for scan number, "
      "uptime, and RSSI."
      "</div>"
    );
  } else {
    server.sendContent(
      "<p>The ESP32 is not currently connected to an access point.</p>"
    );
  }

  server.sendContent("</div>");

  server.sendContent(
    "<div class=\"card\"><h2>Scan History</h2>"
    "<div class=\"note\">Newest scans are shown first. "
    "Each section is one complete logged scan.</div>"
  );

  sendScanHistoryTables();

  server.sendContent("</div>");

  server.sendContent(
    "<div class=\"buttons\">"
    "<a class=\"button\" href=\"/\">Back to Status</a>"
    "<a class=\"button\" href=\"/scan\">Refresh Page</a>"
    "</div>"
    "<div class=\"footer\">ESP32 Web Interface</div>"
    "</div></body></html>"
  );

  server.sendContent("");
}


// ============================================================
// Web server
// ============================================================

void startWebServer() {
  if (webServerStarted) {
    return;
  }

  server.on("/", handleRoot);
  server.on("/scan", handleWebScan);
  server.on("/scan-now", handleWebScanNow);
  server.on("/scan-settings", handleScanSettings);
  server.on("/scan-clear", handleClearScanHistory);
  server.on("/scanlog.csv", handleScanCsv);

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
    Serial.print("Open: http://");
    Serial.print(WiFi.localIP());
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

  Serial.println();
  Serial.println("1 - Wi-Fi status");
  Serial.println("2 - Scan Wi-Fi networks");
  Serial.println("3 - Configure Wi-Fi");
  Serial.println("4 - Clear saved Wi-Fi credentials");
  Serial.println("5 - Restart ESP32");
  Serial.println();
  Serial.println("mac - Show Wi-Fi MAC address");
  Serial.println("h   - Show this menu");
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

      WiFi.disconnect(true);
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

  Serial.println();
  Serial.println();
  Serial.println("================================");
  Serial.println(" ESP32 Starting");
  Serial.println("================================");
  Serial.println();

  // Allocate the RAM-only scan history buffer.
  initializeScanHistory();

  // Initialize STA mode immediately so MAC and hostname are valid
  // even before the board has connected to a network.
  WiFi.mode(WIFI_STA);
  delay(WIFI_STARTUP_SETTLE_MS);

  bool connected = connectUsingSavedCredentials();

  if (!connected) {
    Serial.println();
    Serial.println("Wi-Fi is not configured.");
    Serial.println(
      "Use menu option 3 to configure Wi-Fi."
    );
  }

  if (WiFi.status() == WL_CONNECTED) {
    startWebServer();
  }

  printMenu();
}


// ============================================================
// Automatic scan service
// ============================================================

void serviceAutomaticScan() {
  if (!autoScanEnabled) {
    return;
  }

  unsigned long intervalMs = scanIntervalSeconds * 1000UL;

  if (millis() - lastAutoScanMs < intervalMs) {
    return;
  }

  lastAutoScanMs = millis();

  performLoggedScan();
  WiFi.scanDelete();
}


// ============================================================
// Main loop
// ============================================================

void loop() {
  if (webServerStarted) {
    server.handleClient();
  }

  serviceAutomaticScan();
  handleSerialCommand();

  delay(5);
}