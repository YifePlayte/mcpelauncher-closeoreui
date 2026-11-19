#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace coui {
namespace {

#if defined(__ANDROID__)
constexpr const char* kTag = "CloseOreUI";
#endif

bool envFlag(const char* name) {
    const char* v = getenv(name);
    if (v == nullptr) {
        return false;
    }
    return strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "on") == 0;
}

void emit(int priority, const char* fmt, va_list args) {
#if defined(__ANDROID__)
    __android_log_vprint(priority, kTag, fmt, args);
#else
    (void) priority;
    fputs("[CloseOreUI] ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
#endif
}

}  // namespace

bool debugEnabled() {
    static const bool value = envFlag("CLOSEOREUI_DEBUG");
    return value;
}

void logInfo(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
#if defined(__ANDROID__)
    emit(ANDROID_LOG_INFO, fmt, args);
#else
    emit(0, fmt, args);
#endif
    va_end(args);
}

void logWarn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
#if defined(__ANDROID__)
    emit(ANDROID_LOG_WARN, fmt, args);
#else
    emit(0, fmt, args);
#endif
    va_end(args);
}

void logError(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
#if defined(__ANDROID__)
    emit(ANDROID_LOG_ERROR, fmt, args);
#else
    emit(0, fmt, args);
#endif
    va_end(args);
}

void logDebug(const char* fmt, ...) {
    if (!debugEnabled()) {
        return;
    }
    va_list args;
    va_start(args, fmt);
#if defined(__ANDROID__)
    emit(ANDROID_LOG_DEBUG, fmt, args);
#else
    emit(0, fmt, args);
#endif
    va_end(args);
}

}  // namespace coui
