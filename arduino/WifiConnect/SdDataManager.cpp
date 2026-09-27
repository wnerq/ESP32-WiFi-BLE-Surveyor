#include "SdDataManager.h"
#include <Preferences.h>
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

namespace {

constexpr const char* SD_SETTINGS_NAMESPACE = "sddata";
constexpr const char* SD_FORMAT_KEY = "format";
constexpr const char* SD_WRITE_MODE_KEY = "writeMode";

}

SdDataManager sdDataManager;

void SdDataManager::begin() {
  Preferences prefs;

  if (!prefs.begin(SD_SETTINGS_NAMESPACE, true)) {
    dataFormat = SdDataFormat::CSV;
    dataWriteMode = SdWriteMode::AUTOMATIC;
    return;
  }

  uint8_t storedFormat =
    prefs.getUChar(
      SD_FORMAT_KEY,
      static_cast<uint8_t>(SdDataFormat::CSV)
    );

  uint8_t storedWriteMode =
    prefs.getUChar(
      SD_WRITE_MODE_KEY,
      static_cast<uint8_t>(SdWriteMode::AUTOMATIC)
    );

  prefs.end();

  dataFormat =
    storedFormat == static_cast<uint8_t>(SdDataFormat::JSON)
      ? SdDataFormat::JSON
      : SdDataFormat::CSV;

  dataWriteMode =
    storedWriteMode == static_cast<uint8_t>(SdWriteMode::MANUAL)
      ? SdWriteMode::MANUAL
      : SdWriteMode::AUTOMATIC;
}

SdDataFormat SdDataManager::format() const {
  return dataFormat;
}

SdWriteMode SdDataManager::writeMode() const {
  return dataWriteMode;
}

void SdDataManager::setFormat(
  SdDataFormat format,
  bool persist
) {
  dataFormat =
    format == SdDataFormat::JSON
      ? SdDataFormat::JSON
      : SdDataFormat::CSV;

  if (!persist) {
    return;
  }

  Preferences prefs;

  if (!prefs.begin(SD_SETTINGS_NAMESPACE, false)) {
    return;
  }

  prefs.putUChar(
    SD_FORMAT_KEY,
    static_cast<uint8_t>(dataFormat)
  );

  prefs.end();
}

void SdDataManager::setWriteMode(
  SdWriteMode mode,
  bool persist
) {
  dataWriteMode =
    mode == SdWriteMode::MANUAL
      ? SdWriteMode::MANUAL
      : SdWriteMode::AUTOMATIC;

  if (!persist) {
    return;
  }

  Preferences prefs;

  if (!prefs.begin(SD_SETTINGS_NAMESPACE, false)) {
    return;
  }

  prefs.putUChar(
    SD_WRITE_MODE_KEY,
    static_cast<uint8_t>(dataWriteMode)
  );

  prefs.end();
}

const char* SdDataManager::formatName() const {
  return dataFormat == SdDataFormat::JSON
    ? "JSON"
    : "CSV";
}

const char* SdDataManager::writeModeName() const {
  return dataWriteMode == SdWriteMode::MANUAL
    ? "Manual"
    : "Automatic";
}

bool SdDataManager::automaticWritesEnabled() const {
  return dataWriteMode == SdWriteMode::AUTOMATIC;
}

bool SdDataManager::initialize() {
  setAvailable(false);

  setLoggingFileNumber(0);
  setWifiLogPath("");
  setBleLogPath("");

  pinMode(SURVEY_SD_CS_PIN_VALUE, OUTPUT);
  digitalWrite(SURVEY_SD_CS_PIN_VALUE, HIGH);

  SPI.begin(18, 19, 23, SURVEY_SD_CS_PIN_VALUE);

  if (!SD.begin(SURVEY_SD_CS_PIN_VALUE, SPI, 10000000)) {
    Serial.println(
      "SD logging: card not detected or initialization failed; "
      "continuing without SD logging."
    );

    return false;
  }

  setAvailable(true);

  return true;
}

bool SdDataManager::appendText(const String& path, const String& text) {
  if (!available() || path.length() == 0) return false;

  File file = SD.open(path.c_str(), FILE_APPEND);

  if (!file) {
    incrementWriteFailures();
    return false;
  }

  size_t written = file.print(text);
  file.close();

  if (written != text.length()) {
    incrementWriteFailures();
    return false;
  }

  return true;
}

bool SdDataManager::beginAppend(const String& path) {
  if (!available() || path.length() == 0) {
    return false;
  }

  if (activeAppendOpen) {
    return false;
  }

  activeAppendFile = SD.open(path.c_str(), FILE_APPEND);

  if (!activeAppendFile) {
    incrementWriteFailures();
    return false;
  }

  activeAppendOpen = true;
  return true;
}

bool SdDataManager::writeText(const String& text) {
  if (!activeAppendOpen) {
    return false;
  }

  size_t written = activeAppendFile.print(text);

  if (written != text.length()) {
    return false;
  }

  return true;
}

void SdDataManager::endAppend() {
  if (!activeAppendOpen) {
    return;
  }

  activeAppendFile.close();
  activeAppendOpen = false;
}