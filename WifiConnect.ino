#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

Preferences preferences;
WebServer server(80);

const unsigned long WIFI_TIMEOUT_MS = 15000;
const unsigned long WIFI_STARTUP_SETTLE_MS = 300;

bool webServerStarted = false;


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

String getUptimeString() {
  unsigned long totalSeconds = millis() / 1000;

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


// ============================================================
// Wi-Fi connection
// ============================================================

bool connectToWiFi(const String& ssid, const String& password) {
  Serial.println();
  Serial.print("Connecting to ");
  Serial.print(ssid);
  Serial.println("...");

  ensureWiFiStationMode();

  // We are intentionally changing networks here, so disconnect first.
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

  ensureWiFiStationMode();

  // Do NOT disconnect an active connection just to scan.
  int networkCount = WiFi.scanNetworks();

  if (networkCount == 0) {
    Serial.println("No networks found.");
    WiFi.scanDelete();
    return;
  }

  Serial.print(networkCount);
  Serial.println(" network(s) found:");
  Serial.println();

  // Intentionally not numbered. This command is informational only.
  for (int i = 0; i < networkCount; i++) {
    Serial.print("  ");
    Serial.print(WiFi.SSID(i));

    Serial.print("  [");
    Serial.print(WiFi.RSSI(i));
    Serial.print(" dBm]");

    if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) {
      Serial.print("  OPEN");
    } else {
      Serial.print("  SECURED");
    }

    Serial.println();
  }

  Serial.println();
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

    ensureWiFiStationMode();

    int networkCount = WiFi.scanNetworks();

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

    for (int i = 0; i < networkCount; i++) {
      Serial.print(i + 1);
      Serial.print(" - ");
      Serial.print(WiFi.SSID(i));

      Serial.print("  [");
      Serial.print(WiFi.RSSI(i));
      Serial.print(" dBm]");

      if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) {
        Serial.print("  OPEN");
      } else {
        Serial.print("  SECURED");
      }

      Serial.println();
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

  // Match the web page field order as closely as possible.
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
// Web page
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

  <style>
    body {
      font-family: Arial, Helvetica, sans-serif;
      background: #f4f4f4;
      margin: 0;
      padding: 20px;
    }

    .container {
      max-width: 600px;
      margin: auto;
    }

    h1 {
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
      margin-top: 20px;
      padding: 12px 20px;
      background: #333;
      color: white;
      text-decoration: none;
      border-radius: 6px;
    }

    .footer {
      text-align: center;
      font-size: 0.8em;
      margin-top: 25px;
      color: #777;
    }
  </style>
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

    <div style="text-align:center">
      <a class="button" href="/">Refresh</a>
    </div>

    <div class="footer">
      ESP32 Web Interface
    </div>

  </div>

</body>

</html>
)rawliteral";

  html.replace("%STATUS%", statusText);
  html.replace("%SSID%", ssidText);
  html.replace("%IP%", ipText);
  html.replace("%MAC%", macText);
  html.replace("%HOSTNAME%", hostnameText);
  html.replace("%GATEWAY%", gatewayText);
  html.replace("%SUBNET%", subnetText);
  html.replace("%RSSI%", rssiText);
  html.replace("%UPTIME%", getUptimeString());

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
// Main loop
// ============================================================

void loop() {
  if (webServerStarted) {
    server.handleClient();
  }

  handleSerialCommand();

  delay(5);
}
