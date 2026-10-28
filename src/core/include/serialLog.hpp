#pragma once
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "config/defines.h"


#ifndef HUB75_VERBOSE_LOGS
#define HUB75_VERBOSE_LOGS 0
#endif
#if HUB75_VERBOSE_LOGS
#define HUB75_DEBUG(...) std::printf(__VA_ARGS__)
#else
#define HUB75_DEBUG(...) ((void)0)
#endif

namespace SerialLog {
/**
    Limits repeated serial errors without hiding new failure conditions.

    The most recent message/result pair and its report time are retained in bounded state. Callers
    can continue showing a changed error immediately while suppressing an unchanged fault until
    the reporting interval expires. The gate decides whether to report; it does not print messages
    or change recovery state.
*/
struct RepeatGate {
    const char* previous = nullptr;
    int code = 0;
    uint32_t reportedMs = 0;

    // Allows a repeated log message once its quiet interval has elapsed.
    bool Allow(const char* message, int result, uint32_t nowMs);

};

// Identifies expected storage errors that do not need repeated logging.
bool RoutineStorageError(const char* message, int code);

struct Status {
    uint32_t uptimeSeconds = 0;
    uint32_t displayTenths = 0;
    uint32_t videoTenths = 0;
    uint32_t targetNumerator = 0;
    uint32_t targetDenominator = 1;
    uint32_t sdRetryMs = 0;
    uint32_t hotspotClients = 0;
    bool video = false;
    const char* mode = "builtin";
    const char* homeNetwork = "no-wifi";
    const char* homeIp = "-";
    const char* hotspotNetwork = "unavailable";
    const char* hotspotIp = "-";
    const char* storage = "unavailable";
    const char* job = "idle";
};

// Formats one concise display, playback, network, and storage status line.
bool FormatStatus(char* output, size_t capacity, const Status& status);

} // namespace SerialLog
