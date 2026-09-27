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

  SdDataFormat format() const;
  SdWriteMode writeMode() const;

  void setFormat(SdDataFormat format, bool persist = true);
  void setWriteMode(SdWriteMode mode, bool persist = true);

  const char* formatName() const;
  const char* writeModeName() const;

  bool automaticWritesEnabled() const;

private:
  SdDataFormat dataFormat = SdDataFormat::CSV;
  SdWriteMode dataWriteMode = SdWriteMode::AUTOMATIC;
};

extern SdDataManager sdDataManager;