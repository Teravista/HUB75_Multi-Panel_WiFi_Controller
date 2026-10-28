#include "http/api/include/httpControl.hpp"

#define HTTP_API_PREFIX_BYTES 4
#define HTTP_CONTROL_MAX_QUERY_PARAMETERS 2
#define HTTP_UPLOAD_PREFIX_BYTES 13
#define MAX_BRIGHTNESS_PERCENT_U 100u
#define MAX_LIBRARY_ID_U 0x0fffffffu
#define MAX_VIDEO_FPS_U 60u
#define MIN_SLIDESHOW_INTERVAL_SECONDS 6

/**
    Parses a bounded unsigned decimal control value.

    Consumes decimal digits only and checks the next multiply/add before it can overflow the
    32-bit output. The caller chooses whether zero is permitted. Malformed input can leave a
    partially accumulated value, so callers must use the output only after a successful return.

    @param text Null-terminated input text.
    @param value Output parsed unsigned decimal value.
    @param positive Whether zero must be rejected in addition to invalid decimal text.

    @return true - The text is a valid bounded unsigned decimal satisfying the zero policy.
    @return false - The text is missing, malformed, out of range or disallowed zero.
*/
bool
HttpControl::ParseUnsignedDecimal (
    const char* text,
    uint32_t& value,
    bool positive
    )
{
    unsigned digit;

    if (!*text) {
        return false;
    }
    value = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') {
            return false;
        }
        digit = unsigned(*text - '0');
        if (value > (UINT32_MAX - digit) / DECIMAL_RADIX) {
            return false;
        }
        value = value * DECIMAL_RADIX + digit;
    }

    return !positive || value != 0;
} // ParseUnsignedDecimal

/**
    Validates a supported control request and extracts its arguments.

    Splits a bounded route/query into ReadSingleQueryParameter internal command and validates the method, required
    keys, duplicate parameters and each operation's numeric limits. Route aliases are translated
    into the same command representation used by foreground HTTP processing. Parsing may modify
    the supplied path buffer; no library change, SD access or upload operation is performed until
    the resulting command is handled.

    @param method HTTP request method, such as GET or POST.
    @param path Mutable HTTP route/query text split in place while extracting parameters.
    @param command Output controller command parsed from the request.

    @return HTTP_STATUS_OK - The supported method and parameters were parsed.
    @return HTTP_STATUS_BAD_REQUEST - A parameter is missing, malformed or out of range.
    @return HTTP_STATUS_NOT_FOUND - No supported API route matches the path.
    @return HTTP_STATUS_METHOD_NOT_ALLOWED - The route exists but the HTTP method is not
                                             supported.
*/
int
HttpControl::ParseControllerCommand (
    const char* method,
    char* path,
    Command& command
    )
{
    const char* keys[HTTP_CONTROL_MAX_QUERY_PARAMETERS];
    uint32_t values[HTTP_CONTROL_MAX_QUERY_PARAMETERS];
    unsigned count;
    uint32_t maximumValue;
    char* query = nullptr;
    char* next = nullptr;
    char* equals = nullptr;
    unsigned i;
    bool fps;
    bool brightness;
    unsigned innerIndex;
    bool nextBrightness;
    unsigned kind;
    bool video;
    const char* base = nullptr;
    size_t size;
    char* id = nullptr;
    char* suffix = nullptr;
    char* nextId = nullptr;
    char* nextSuffix = nullptr;
    /**
        Checks that the control request has no query parameters.

        @return true - The request contains no query parameters.
        @return false - The request contains at least ReadSingleQueryParameter query parameter.
    */
    auto HasNoQueryParameters = [&] {
        return count == 0;
    };
    /**
        Validates and extracts a single expected numeric query parameter.

        @param key Expected query parameter name.
        @param value Output parsed unsigned decimal value.
        @param optional Whether a missing parameter is allowed and defaults to zero.

        @return true - The expected numeric parameter was extracted, or an optional missing
                       parameter defaulted to zero.
        @return false - The query count or parameter name is invalid.
    */
    auto ReadSingleQueryParameter = [&](const char* key, uint32_t& value, bool optional = false) {
        if (!count && optional) {
            value = 0;
            return true;
        }
        if (count != 1 || std::strcmp(keys[0], key)) {
            return false;
        }
        value = values[0];
        return true;
    };
    /**
        Checks the required HTTP method and returns its response status.

        @param expected HTTP method required by this API route.

        @return HTTP_STATUS_OK - The request uses the expected HTTP method.
        @return HTTP_STATUS_METHOD_NOT_ALLOWED - The request method differs from the expected
                                                 method.
    */
    auto CheckRequestMethod = [&](const char* expected) {
        if (std::strcmp(method, expected)) {
            return HTTP_STATUS_METHOD_NOT_ALLOWED;
        } else {
            return HTTP_STATUS_OK;
        }
    };

    if (std::strncmp(path, "/api", HTTP_API_PREFIX_BYTES) ||
        (path[HTTP_API_PREFIX_BYTES] && path[HTTP_API_PREFIX_BYTES] != '/')) {
        return HTTP_STATUS_NOT_FOUND;
    }
    std::memset(keys, 0, sizeof(keys));
    std::memset(values, 0, sizeof(values));
    count = 0;
    query = std::strchr(path, '?');
    if (query) {
        *query++ = '\0';
        if (!*query) {
            return HTTP_STATUS_BAD_REQUEST;
        }
        for (;;) {
            if (count == HTTP_CONTROL_MAX_QUERY_PARAMETERS) {
                return HTTP_STATUS_BAD_REQUEST;
            }
            next = std::strchr(query, '&');
            if (next) {
                *next++ = '\0';
            }
            equals = std::strchr(query, '=');
            if (!equals) {
                return HTTP_STATUS_BAD_REQUEST;
            }
            *equals++ = '\0';
            if (!*query || !ParseUnsignedDecimal(equals, values[count])) {
                return HTTP_STATUS_BAD_REQUEST;
            }
            for (i = 0; i < count; ++i) {
                if (!std::strcmp(keys[i], query)) {
                    return HTTP_STATUS_BAD_REQUEST;
                }
            }
            keys[count++] = query;
            if (!next) {
                break;
            }
            if (!*next) {
                return HTTP_STATUS_BAD_REQUEST;
            }
            query = next;
        }
    }

    if (!std::strcmp(path, "/api") || !std::strcmp(path, "/api/")) {
        command.info = true;
        if (HasNoQueryParameters()) {
            return CheckRequestMethod("GET");
        } else {
            return HTTP_STATUS_BAD_REQUEST;
        }
    }
    if (!std::strcmp(path, "/api/status") || !std::strcmp(path, "/api/state") ||
        !std::strcmp(path, "/api/settings") || !std::strcmp(path, "/api/slideshow")) {
        if (!std::strcmp(path, "/api/settings") && !std::strcmp(method, "POST")) {
            if (count != HTTP_CONTROL_MAX_QUERY_PARAMETERS) {
                return HTTP_STATUS_BAD_REQUEST;
            }
            fps = false;
            brightness = false;
            for (innerIndex = 0; innerIndex < count; ++innerIndex) {
                if (!std::strcmp(keys[innerIndex], "maxFps")) {
                    command.id = values[innerIndex];
                    fps = true;
                } else if (!std::strcmp(keys[innerIndex], "brightness")) {
                    command.offset = values[innerIndex];
                    brightness = true;
                } else {
                    return HTTP_STATUS_BAD_REQUEST;
                }
            }
            if (!fps || !brightness || command.id > MAX_VIDEO_FPS ||
                command.offset > MAX_BRIGHTNESS_PERCENT) {
                return HTTP_STATUS_BAD_REQUEST;
            }
            command.op = HTTP_OP_DISPLAY_SETTINGS_SET;
            return HTTP_STATUS_OK;
        }
        command.op = HTTP_OP_PLAYBACK_STATUS;
        if (HasNoQueryParameters()) {
            return CheckRequestMethod("GET");
        } else {
            return HTTP_STATUS_BAD_REQUEST;
        }
    }
    if (!std::strcmp(path, "/api/memory")) {
        command.op = HTTP_OP_MEMORY_STATUS;
        if (HasNoQueryParameters()) {
            return CheckRequestMethod("GET");
        } else {
            return HTTP_STATUS_BAD_REQUEST;
        }
    }
    if (!std::strcmp(path, "/api/brightness") || !std::strcmp(path, "/api/max-fps")) {
        nextBrightness = !std::strcmp(path, "/api/brightness");
        if (!ReadSingleQueryParameter("value", command.id)) {
            return HTTP_STATUS_BAD_REQUEST;
        }
        if (nextBrightness) {
            maximumValue = MAX_BRIGHTNESS_PERCENT_U;
        } else {
            maximumValue = MAX_VIDEO_FPS_U;
        }
        if (command.id > maximumValue) {
            return HTTP_STATUS_BAD_REQUEST;
        }
        if (nextBrightness) {
            command.op = HTTP_OP_BRIGHTNESS_SET;
        } else {
            command.op = HTTP_OP_FPS_LIMIT_SET;
        }
        return CheckRequestMethod("POST");
    }
    if (!std::strcmp(path, "/api/slideshow/start")) {
        command.op = HTTP_OP_SLIDESHOW_SET;
        command.id = 1;
        if (!ReadSingleQueryParameter("intervalSeconds", command.offset) || command.offset < MIN_SLIDESHOW_INTERVAL_SECONDS ||
            command.offset > MAX_SLIDESHOW_INTERVAL_SECONDS) {
            return HTTP_STATUS_BAD_REQUEST;
        }
        return CheckRequestMethod("POST");
    }
    if (!std::strcmp(path, "/api/slideshow/stop")) {
        command.op = HTTP_OP_SLIDESHOW_STOP;
        if (HasNoQueryParameters()) {
            return CheckRequestMethod("POST");
        } else {
            return HTTP_STATUS_BAD_REQUEST;
        }
    }
    for (kind = 0; kind < HTTP_CONTROL_MAX_QUERY_PARAMETERS; ++kind) {
        video = kind != 0;
        if (video) {
            base = "/api/videos";
        } else {
            base = "/api/photos";
        }
        size = std::strlen(base);
        if (std::strncmp(path, base, size)) {
            continue;
        }
        if (!path[size]) {
            if (video) {
                command.op = HTTP_OP_VIDEOS_LIST;
            } else {
                command.op = HTTP_OP_PHOTOS_LIST;
            }
            if (ReadSingleQueryParameter("after", command.offset, true)) {
                return CheckRequestMethod("GET");
            } else {
                return HTTP_STATUS_BAD_REQUEST;
            }
        }
        if (path[size] != '/') {
            continue;
        }
        id = path + size + 1;
        suffix = std::strchr(id, '/');
        if (suffix) {
            *suffix++ = '\0';
        }
        if (!ParseUnsignedDecimal(id, command.id, true) || command.id > MAX_LIBRARY_ID_U || !HasNoQueryParameters()) {
            return HTTP_STATUS_BAD_REQUEST;
        }
        if (!suffix) {
            if (video) {
                command.op = HTTP_OP_VIDEO_DELETE;
            } else {
                command.op = HTTP_OP_PHOTO_DELETE;
            }
            return CheckRequestMethod("DELETE");
        }
        if (!std::strcmp(suffix, "show")) {
            if (video) {
                command.op = HTTP_OP_VIDEO_SHOW;
            } else {
                command.op = HTTP_OP_PHOTO_SHOW;
            }
            return CheckRequestMethod("POST");
        }
        if (!std::strcmp(suffix, "thumbnail")) {
            if (video) {
                command.op = HTTP_OP_VIDEO_PREVIEW;
            } else {
                command.op = HTTP_OP_PHOTO_PREVIEW;
            }
            return CheckRequestMethod("GET");
        }
        return HTTP_STATUS_NOT_FOUND;
    }

    if (!std::strcmp(path, "/api/uploads/photos") || !std::strcmp(path, "/api/uploads/videos")) {
        if (!std::strcmp(path, "/api/uploads/photos")) {
            command.op = HTTP_OP_UPLOAD_PHOTO_BEGIN;
        } else {
            command.op = HTTP_OP_UPLOAD_NAMED_VIDEO_BEGIN;
        }
        command.binary = true;
        if (HasNoQueryParameters()) {
            return CheckRequestMethod("POST");
        } else {
            return HTTP_STATUS_BAD_REQUEST;
        }
    }
    if (!std::strcmp(path, "/api/uploads/resume")) {
        command.op = HTTP_OP_UPLOAD_RESUME;
        command.token = true;
        if (!ReadSingleQueryParameter("token", command.id) || !command.id) {
            return HTTP_STATUS_BAD_REQUEST;
        }
        return CheckRequestMethod("GET");
    }
    if (!std::strncmp(path, "/api/uploads/", HTTP_UPLOAD_PREFIX_BYTES)) {
        nextId = path + HTTP_UPLOAD_PREFIX_BYTES;
        nextSuffix = std::strchr(nextId, '/');
        if (nextSuffix) {
            *nextSuffix++ = '\0';
        }
        if (!ParseUnsignedDecimal(nextId, command.id, true)) {
            return HTTP_STATUS_BAD_REQUEST;
        }
        if (!nextSuffix) {
            if (!HasNoQueryParameters()) {
                return HTTP_STATUS_BAD_REQUEST;
            }
            if (!std::strcmp(method, "DELETE")) {
                command.op = HTTP_OP_UPLOAD_CANCEL;
                return HTTP_STATUS_OK;
            }
            command.op = HTTP_OP_UPLOAD_STATUS;
            return CheckRequestMethod("GET");
        }
        if (!std::strcmp(nextSuffix, "chunk")) {
            command.op = HTTP_OP_UPLOAD_CHUNK;
            command.binary = true;
            if (ReadSingleQueryParameter("offset", command.offset)) {
                return CheckRequestMethod("PUT");
            } else {
                return HTTP_STATUS_BAD_REQUEST;
            }
        }
        if (!std::strcmp(nextSuffix, "cancel")) {
            command.op = HTTP_OP_UPLOAD_CANCEL;
            if (HasNoQueryParameters()) {
                return CheckRequestMethod("POST");
            } else {
                return HTTP_STATUS_BAD_REQUEST;
            }
        }
    }
    return HTTP_STATUS_NOT_FOUND;
} // ParseControllerCommand
