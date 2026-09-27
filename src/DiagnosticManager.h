#pragma once

#include <Arduino.h>

static constexpr size_t DIAGNOSTIC_EVENT_CAPACITY = 64;
static constexpr size_t DIAGNOSTIC_CATEGORY_LENGTH = 24;
static constexpr size_t DIAGNOSTIC_DETAIL_LENGTH = 160;

struct DiagnosticEventRecord {
  uint32_t uptimeMs = 0;
  char category[DIAGNOSTIC_CATEGORY_LENGTH] = {};
  char detail[DIAGNOSTIC_DETAIL_LENGTH] = {};
};

class DiagnosticManager {
public:
  void begin();

  void record(const char* category, const String& detail);

  size_t count() const;
  size_t capacity() const;

  const DiagnosticEventRecord& at(size_t logicalIndex) const;

  void clear();

private:
  DiagnosticEventRecord events[DIAGNOSTIC_EVENT_CAPACITY] = {};

  size_t eventStart = 0;
  size_t eventCount = 0;
};

extern DiagnosticManager diagnosticManager;