#pragma once

#include <Arduino.h>

enum class SdDataFormat : uint8_t {
  CSV = 0,
  JSON = 1
};

enum class SdWriteMode : uint8_t {
  AUTOMATIC = 0,
  MANUAL = 1
};

class SdDataManager {
public:
  void begin();

  // Existing future SD settings
  SdDataFormat format() const;
  SdWriteMode writeMode() const;

  void setFormat(SdDataFormat format, bool persist = true);
  void setWriteMode(SdWriteMode mode, bool persist = true);

  const char* formatName() const;
  const char* writeModeName() const;

  bool automaticWritesEnabled() const;

  // Existing SD state
  bool available() const;
  uint32_t loggingFileNumber() const;

  const String& wifiLogPath() const;
  const String& bleLogPath() const;

  uint32_t wifiRowsLogged() const;
  uint32_t bleRowsLogged() const;
  uint32_t writeFailures() const;
  uint32_t wifiBatchFlushes() const;
  uint32_t bleBatchFlushes() const;

  void setAvailable(bool value);
  void setLoggingFileNumber(uint32_t value);
  void setWifiLogPath(const String& value);
  void setBleLogPath(const String& value);

  void incrementWifiRowsLogged(uint32_t amount = 1);
  void incrementBleRowsLogged(uint32_t amount = 1);
  void incrementWriteFailures(uint32_t amount = 1);
  void incrementWifiBatchFlushes(uint32_t amount = 1);
  void incrementBleBatchFlushes(uint32_t amount = 1);

  void resetCounters();

private:
  SdDataFormat dataFormat = SdDataFormat::CSV;
  SdWriteMode dataWriteMode = SdWriteMode::AUTOMATIC;

  bool sdLoggingAvailable = false;
  uint32_t sdLoggingFileNumber = 0;

  String sdWifiLogPath;
  String sdBleLogPath;

  uint32_t sdWifiRowsLogged = 0;
  uint32_t sdBleRowsLogged = 0;
  uint32_t sdWriteFailures = 0;
  uint32_t sdWifiBatchFlushes = 0;
  uint32_t sdBleBatchFlushes = 0;
};

extern SdDataManager sdDataManager;