#include "DiagnosticManager.h"

DiagnosticManager diagnosticManager;

void DiagnosticManager::begin() {
  eventStart = 0;
  eventCount = 0;

  memset(events, 0, sizeof(events));
}

void DiagnosticManager::record(const char* category, const String& detail) {
  size_t index;

  if (eventCount < DIAGNOSTIC_EVENT_CAPACITY) {
    index = (eventStart + eventCount) % DIAGNOSTIC_EVENT_CAPACITY;
    eventCount++;
  } else {
    // Ring is full. Replace the oldest event.
    index = eventStart;
    eventStart = (eventStart + 1) % DIAGNOSTIC_EVENT_CAPACITY;
  }

  DiagnosticEventRecord& event = events[index];

  event.uptimeMs = millis();

  snprintf(
    event.category,
    sizeof(event.category),
    "%s",
    category ? category : "EVENT"
  );

  snprintf(
    event.detail,
    sizeof(event.detail),
    "%s",
    detail.c_str()
  );
}

size_t DiagnosticManager::count() const {
  return eventCount;
}

size_t DiagnosticManager::capacity() const {
  return DIAGNOSTIC_EVENT_CAPACITY;
}

const DiagnosticEventRecord& DiagnosticManager::at(size_t logicalIndex) const {
  if (logicalIndex >= eventCount) {
    // This should never be used by normal callers.
    // Return the oldest event rather than introducing dynamic allocation.
    return events[eventStart];
  }

  return events[
    (eventStart + logicalIndex) % DIAGNOSTIC_EVENT_CAPACITY
  ];
}

void DiagnosticManager::clear() {
  eventStart = 0;
  eventCount = 0;
}