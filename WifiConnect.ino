#include <WiFi.h>
#include <Preferences.h>

Preferences preferences;

const unsigned long WIFI_TIMEOUT_MS = 15000;

String readSerialLine() {
  while (!Serial.available()) {
    delay(10);
  }

  String input = Serial.readStringUntil('\n');
  input.trim();

  return input;
}

bool connectToWiFi(const String& ssid, const String& password) {
  Serial.println();
  Serial.print("Connecting to ");
  Serial.print(ssid);
  Serial.println("...");

  WiFi.mode(WIFI_STA);
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
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("Signal strength: ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");

  return true;
}

bool trySavedCredentials() {
  preferences.begin("wifi", true);

  String savedSSID = preferences.getString("ssid", "");
  String savedPassword = preferences.getString("password", "");

  preferences.end();

  if (savedSSID.length() == 0) {
    Serial.println("No saved Wi-Fi credentials found.");
    return false;
  }

  Serial.print("Saved Wi-Fi network found: ");
  Serial.println(savedSSID);

  return connectToWiFi(savedSSID, savedPassword);
}

void saveCredentials(const String& ssid, const String& password) {
  preferences.begin("wifi", false);

  preferences.putString("ssid", ssid);
  preferences.putString("password", password);

  preferences.end();

  Serial.println("Wi-Fi credentials saved.");
}

void eraseCredentials() {
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();

  Serial.println("Saved Wi-Fi credentials erased.");
}

void configureWiFi() {
  while (true) {
    Serial.println();
    Serial.println("Scanning for Wi-Fi networks...");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    int networkCount = WiFi.scanNetworks();

    if (networkCount == 0) {
      Serial.println("No Wi-Fi networks found.");
      Serial.println("Press Enter to scan again.");
      readSerialLine();
      continue;
    }

    Serial.println();
    Serial.println("Available Wi-Fi networks:");
    Serial.println();

    for (int i = 0; i < networkCount; i++) {
      Serial.print(i + 1);
      Serial.print(": ");
      Serial.print(WiFi.SSID(i));

      Serial.print("  (");
      Serial.print(WiFi.RSSI(i));
      Serial.print(" dBm");

      if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) {
        Serial.print(", Open");
      } else {
        Serial.print(", Secured");
      }

      Serial.println(")");
    }

    Serial.println();
    Serial.print("Select network [1-");
    Serial.print(networkCount);
    Serial.println("]:");

    String selectionText = readSerialLine();
    int selection = selectionText.toInt();

    if (selection < 1 || selection > networkCount) {
      Serial.println("Invalid selection.");
      WiFi.scanDelete();
      continue;
    }

    String selectedSSID = WiFi.SSID(selection - 1);
    wifi_auth_mode_t encryption =
        WiFi.encryptionType(selection - 1);

    WiFi.scanDelete();

    Serial.println();
    Serial.print("Selected: ");
    Serial.println(selectedSSID);

    String password = "";

    if (encryption != WIFI_AUTH_OPEN) {
      Serial.println("Enter Wi-Fi passphrase:");
      password = readSerialLine();
    }

    if (connectToWiFi(selectedSSID, password)) {
      saveCredentials(selectedSSID, password);
      return;
    }

    Serial.println();
    Serial.println("Unable to connect.");
    Serial.println("Press Enter to scan again.");
    readSerialLine();
  }
}

void setup() {
  Serial.begin(115200);

  // Give Serial Monitor a moment to connect.
  delay(1500);

  Serial.println();
  Serial.println("==============================");
  Serial.println(" ESP32 Wi-Fi Setup");
  Serial.println("==============================");

  if (!trySavedCredentials()) {
    configureWiFi();
  }

  Serial.println();
  Serial.println("Wi-Fi setup complete.");
  Serial.println("Type 'status' for connection information.");
  Serial.println("Type 'erase' to erase saved Wi-Fi credentials.");
}

void loop() {
  if (Serial.available()) {
    String command = readSerialLine();

    if (command.equalsIgnoreCase("status")) {
      Serial.println();

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Wi-Fi status: Connected");
        Serial.print("SSID: ");
        Serial.println(WiFi.SSID());

        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());

        Serial.print("RSSI: ");
        Serial.print(WiFi.RSSI());
        Serial.println(" dBm");
      } else {
        Serial.println("Wi-Fi status: Not connected");
      }
    }

    else if (command.equalsIgnoreCase("erase")) {
      eraseCredentials();

      Serial.println("Restarting...");
      delay(1000);
      ESP.restart();
    }

    else if (command.length() > 0) {
      Serial.print("Unknown command: ");
      Serial.println(command);
    }
  }

  delay(10);
}
