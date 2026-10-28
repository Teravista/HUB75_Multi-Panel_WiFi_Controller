#include "core/include/serialLog.hpp"

#define MILLISECONDS_ROUND_UP_BIAS 999
#define SERIAL_JOB_TEXT_BYTES 40
#define SERIAL_REPEAT_QUIET_MS 15000
#define SERIAL_RETRY_TEXT_BYTES 32
#define SERIAL_VIDEO_TEXT_BYTES 48

/**
    Allows a repeated log message once its quiet interval has elapsed.

    @param message Human-readable result or error message.
    @param result Result code returned by the SDK or filesystem operation.
    @param nowMs Current monotonic time since boot, in milliseconds.

    @return true - This message may be logged now.
    @return false - An equivalent message was logged within the quiet interval.
*/
bool
SerialLog::RepeatGate::Allow (
    const char* message,
    int result,
    uint32_t nowMs
    )
{
    if (previous && result == code && !std::strcmp(previous, message) &&
        nowMs - reportedMs < SERIAL_REPEAT_QUIET_MS) {
        return false;
    }
    previous = message;
    code = result;
    reportedMs = nowMs;
    return true;
} // Allow

/**
    Identifies expected storage errors that do not need repeated logging.

    @param message Human-readable result or error message.
    @param code Status or result code to classify or format.

    @return true - The message describes an expected busy or unavailable storage condition.
    @return false - The message or result code represents another condition.
*/
bool
SerialLog::RoutineStorageError (
    const char* message,
    int code
    )
{
    return !code && (std::strstr(message, "busy") || std::strstr(message, "not ready") ||
                     std::strstr(message, "unavailable") || std::strstr(message, "transaction"));
} // RoutineStorageError

/**
    Formats one concise display, playback, network, and storage status line.

    Builds one bounded status line from an already captured controller snapshot. Video mode
    includes measured and target frame rates; other modes omit video timing. Unavailable storage
    includes its remaining retry delay. Formatting only reads the supplied snapshot, so producing
    logs does not perform network or filesystem work.

    @param output Caller-owned destination for the complete serial status line.
    @param capacity Number of elements available in the destination buffer, including any text
                    terminator.
    @param status Status snapshot or protocol status code used by this operation.

    @return true - The complete status text fits in the output buffer.
    @return false - Formatting failed or the output would be truncated.
*/
bool
SerialLog::FormatStatus (
    char* output,
    size_t capacity,
    const Status& status
    )
{
    char video[SERIAL_VIDEO_TEXT_BYTES];
    char retry[SERIAL_RETRY_TEXT_BYTES];
    char job[SERIAL_JOB_TEXT_BYTES];
    uint32_t target;
    int size;

    std::memset(video, 0, sizeof(video));
    std::memcpy(video, "-", sizeof("-"));
    std::memset(retry, 0, sizeof(retry));
    std::memcpy(retry, "", sizeof(""));
    std::memset(job, 0, sizeof(job));
    std::memcpy(job, "", sizeof(""));
    if (status.video) {
        if (status.targetDenominator) {
            target =
                uint32_t(uint64_t(status.targetNumerator) * FPS_DECIMAL_SCALE / status.targetDenominator);
        } else {
            target = 0;
        }
        std::snprintf(
            video, sizeof(video), "%lu.%lu/%lu.%lu", (unsigned long)(status.videoTenths / FPS_DECIMAL_SCALE),
            (unsigned long)(status.videoTenths % FPS_DECIMAL_SCALE),
            (unsigned long)(target / FPS_DECIMAL_SCALE), (unsigned long)(target % FPS_DECIMAL_SCALE));
    }
    if (status.sdRetryMs) {
        std::snprintf(
            retry, sizeof(retry), " retry=%lus",
            (unsigned long)((status.sdRetryMs + MILLISECONDS_ROUND_UP_BIAS) / MILLISECONDS_PER_SECOND_U));
    }
    if (std::strcmp(status.job, "idle")) {
        std::snprintf(job, sizeof(job), " job=%s", status.job);
    }
    size = std::snprintf(
        output, capacity, "[status] t=%lus display_fps=%lu.%lu video_fps=%s mode=%s home_wifi=%s home_ip=%s pico_wifi=%s pico_ip=%s pico_clients=%lu/%u sd=%s%s%s",
        (unsigned long)status.uptimeSeconds, (unsigned long)(status.displayTenths / FPS_DECIMAL_SCALE),
        (unsigned long)(status.displayTenths % FPS_DECIMAL_SCALE), video, status.mode, status.homeNetwork,
        status.homeIp, status.hotspotNetwork, status.hotspotIp,
        (unsigned long)status.hotspotClients, unsigned(WIFI_HOTSPOT_MAX_CLIENTS), status.storage, retry, job);
    return size >= 0 && size_t(size) < capacity;
} // FormatStatus
