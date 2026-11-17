#pragma once

// Small platform abstraction so the same sources can be built for Android
// (the real target) and for the host (used by the self-test harness).

#include <cstdint>
#include <cstddef>

namespace coui {

void logInfo(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logWarn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logError(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Verbose diagnostics, only emitted when CLOSEOREUI_DEBUG is set.
void logDebug(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
bool debugEnabled();

}  // namespace coui
