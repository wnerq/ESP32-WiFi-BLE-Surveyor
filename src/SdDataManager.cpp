#include "SdDataManager.h"

#include <Preferences.h>

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