// WifiConnect4 - Wi-Fi survey/logger revision
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
const size_t MAX_SCAN_RECORDS = 300;
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

ScanRecord scanHistory[MAX_SCAN_RECORDS];

size_t historyStart = 0;
size_t historyCount = 0;

uint32_t scanCounter = 0;
uint32_t lastScanUptimeMs = 0;

bool autoScanEnabled = false;
unsigned long scanIntervalSeconds = 10;
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

void appendScanRecord(const ScanRecord& record) {
  size_t writeIndex;

  if (historyCount < MAX_SCAN_RECORDS) {
    writeIndex = (historyStart + historyCount) % MAX_SCAN_RECORDS;
    historyCount++;
  } else {
    // Ring buffer full: overwrite the oldest record.
    writeIndex = historyStart;
    historyStart = (historyStart + 1) % MAX_SCAN_RECORDS;
  }

  scanHistory[writeIndex] = record;
}

const ScanRecord& historyRecord(size_t logicalIndex) {
  size_t physicalIndex =
      (historyStart + logicalIndex) % MAX_SCAN_RECORDS;

  return scanHistory[physicalIndex];
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
  Serial.print(MAX_SCAN_RECORDS);
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

void handleWebScan() {
  ensureWiFiStationMode();

  bool connected = WiFi.status() == WL_CONNECTED;
  String connectedSSID = connected ? WiFi.SSID() : "";
  int connectedRSSI = connected ? WiFi.RSSI() : 0;

  String html = R"rawliteral(
<!DOCTYPE html>
<html>

<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Wi-Fi Survey</title>
)rawliteral";

  html += pageStyles();

  html += R"rawliteral(
</head>

<body>
  <div class="container">

    <h1>Wi-Fi Survey</h1>
)rawliteral";

  if (connected) {
    html += R"rawliteral(
    <div class="card">
      <div class="row">
        <span class="label">Connected SSID</span>
        <span class="value">)rawliteral";
    html += htmlEscape(connectedSSID);
    html += R"rawliteral(</span>
      </div>

      <div class="row">
        <span class="label">Current Signal</span>
        <span class="value">)rawliteral";
    html += String(connectedRSSI);
    html += R"rawliteral( dBm</span>
      </div>
    </div>
)rawliteral";
  }

  html += R"rawliteral(
    <div class="card">
      <h2>Scan Logging</h2>

      <div class="row">
        <span class="label">Automatic Scanning</span>
        <span class="value">)rawliteral";
  html += autoScanEnabled ? "ON" : "OFF";
  html += R"rawliteral(</span>
      </div>

      <div class="row">
        <span class="label">Scan Interval</span>
        <span class="value">)rawliteral";
  html += String(scanIntervalSeconds);
  html += R"rawliteral( seconds</span>
      </div>

      <div class="row">
        <span class="label">Scans This Session</span>
        <span class="value">)rawliteral";
  html += String(scanCounter);
  html += R"rawliteral(</span>
      </div>

      <div class="row">
        <span class="label">Stored Records</span>
        <span class="value">)rawliteral";
  html += String(historyCount);
  html += " / ";
  html += String(MAX_SCAN_RECORDS);
  html += R"rawliteral(</span>
      </div>

      <div class="row">
        <span class="label">Last Scan</span>
        <span class="value">)rawliteral";

  if (scanCounter == 0) {
    html += "Never";
  } else {
    html += htmlEscape(formatUptime(lastScanUptimeMs));
    html += " uptime";
  }

  html += R"rawliteral(</span>
      </div>

      <form class="controls" action="/scan-settings" method="get">
        <div class="control">
          <label for="interval">Interval (seconds)</label>
          <input
            id="interval"
            name="interval"
            type="number"
            min="5"
            max="3600"
            value=")rawliteral";
  html += String(scanIntervalSeconds);
  html += R"rawliteral("
          >
        </div>

        <div class="control">
          <label>
            <input
              type="checkbox"
              name="auto"
              value="1"
)rawliteral";

  if (autoScanEnabled) {
    html += " checked";
  }

  html += R"rawliteral(
            >
            Automatic scanning
          </label>
        </div>

        <button type="submit">Apply</button>
      </form>

      <div class="buttons">
        <a class="button" href="/scan-now">Scan Now</a>
        <a class="button" href="/scanlog.csv">Download CSV</a>
        <a class="button" href="/scan-clear">Clear History</a>
      </div>

      <div class="note">
        Scan history is kept in RAM only and is cleared by reset or power cycle.
        When the record buffer fills, the oldest records are overwritten.
      </div>
    </div>

    <div class="card">
      <h2>Latest Scan</h2>
)rawliteral";

  if (scanCounter == 0 || historyCount == 0) {
    html += R"rawliteral(
      <p>No scan has been logged yet.</p>
)rawliteral";
  } else {
    html += R"rawliteral(
      <table>
        <thead>
          <tr>
            <th>SSID</th>
            <th>BSSID</th>
            <th class="signal">CH</th>
            <th class="signal">Signal</th>
            <th class="security">Security</th>
          </tr>
        </thead>
        <tbody>
)rawliteral";

    for (size_t i = 0; i < historyCount; i++) {
      const ScanRecord& record = historyRecord(i);

      if (record.scanNumber != scanCounter) {
        continue;
      }

      if (record.connected) {
        html += "<tr class=\"current\">";
      } else {
        html += "<tr>";
      }

      String displaySSID =
          record.hidden ? "(hidden)" : String(record.ssid);

      html += "<td>";
      html += htmlEscape(displaySSID);

      if (record.connected) {
        html += " (connected)";
      }

      html += "</td>";

      html += "<td>";
      html += htmlEscape(String(record.bssid));
      html += "</td>";

      html += "<td class=\"signal\">";
      html += String(record.channel);
      html += "</td>";

      html += "<td class=\"signal\">";
      html += String(record.rssi);
      html += " dBm</td>";

      html += "<td class=\"security\">";
      html += htmlEscape(
        securityLabel((wifi_auth_mode_t)record.authMode)
      );
      html += "</td>";

      html += "</tr>";
    }

    html += R"rawliteral(
        </tbody>
      </table>
)rawliteral";
  }

  html += R"rawliteral(
      <div class="note">
        Signal values closer to 0 dBm are stronger. BSSID identifies an
        individual access point/radio, so the same SSID can appear more than once.
      </div>
    </div>

    <div class="buttons">
      <a class="button" href="/">Back to Status</a>
      <a class="button" href="/scan">Refresh Page</a>
    </div>

    <div class="footer">
      ESP32 Web Interface
    </div>

  </div>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
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
