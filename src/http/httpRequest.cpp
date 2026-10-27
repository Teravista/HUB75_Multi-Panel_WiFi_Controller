#include "http/include/httpRequest.hpp"
#include <cstring>
#include <cstdio>
#include <cstdint>
#include "config/defines.h"

#define ASCII_CASE_OFFSET 32
#define HTTP_LINE_END_BYTES 2
#define HTTP_PREVIOUS_CHARACTER_OFFSET -1
#define HTTP_SCHEME_BYTES 7
#define HTTP_STATUS_CONTENT_TOO_LARGE 413
#define HTTP_STATUS_EXPECTATION_FAILED 417
#define HTTP_STATUS_FORBIDDEN 403
#define HTTP_STATUS_LENGTH_REQUIRED 411
#define HTTP_STATUS_NOT_IMPLEMENTED 501
#define HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE 415

namespace {
/**
    Compares an HTTP token with an expected value without case sensitivity.

    Walks both NUL-terminated tokens with ASCII case folding and requires their terminators to
    coincide. A matching prefix therefore cannot accept a longer header value. This helper only
    compares bytes and does not alter either token.

    @param a First token to compare.
    @param b Second token to compare.

    @return true - Both tokens match without regard to letter case.
    @return false - The tokens differ.
*/
bool
CompareHttpTokenIgnoringCase (
    const char* a,
    const char* b
    )
{
    /**
        Converts an ASCII uppercase letter to lowercase for token comparison.

        @param c Character to validate as part of an HTTP token.

        @return value - ASCII character converted to lowercase; other characters are unchanged.
    */
    auto lower = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c + ASCII_CASE_OFFSET;
        } else {
            return c;
        }
    };

    while (*a && *b) {
        if (lower(*a++) != lower(*b++)) {
            return false;
        }
    }

    return *a == *b;
} // CompareHttpTokenIgnoringCase

/**
    Checks whether a character is permitted in an HTTP token.

    @param c Character to validate as part of an HTTP token.

    @return true - The character is permitted in an HTTP token.
    @return false - The character is a delimiter or is not permitted.
*/
bool
IsHttpTokenCharacter (
    char c
    )
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c && std::strchr("!#$%&'*+-.^_`|~", c));
} // IsHttpTokenCharacter

/**
    Checks that a Host header matches the local address and optional HTTP port.

    @param value Null-terminated HTTP header value to inspect.
    @param host Expected local host address.

    @return true - The host matches the local address with no port or port 80.
    @return false - The Host value does not match the local address.
*/
bool
MatchesLocalHostHeader (
    const char* value,
    const char* host
    )
{
    size_t n;

    n = std::strlen(host);
    return std::strncmp(value, host, n) == 0 && (value[n] == '\0' || std::strcmp(value + n, ":80") == 0);
} // MatchesLocalHostHeader

/**
    Marks the HTTP request as rejected with the supplied status.

    @param status Status snapshot or protocol status code used by this operation.

    @return Result - Rejected route carrying the supplied HTTP error status.
*/
HttpRequest::Result
RejectRequestHeader (
    int status
    )
{
    return {HttpRequest::Route::Reject, status};
} // RejectRequestHeader

} // namespace

/**
    Validates an HTTP header and selects the page or raw-photo route.

    Checks request-line and header syntax, method/body framing and the accepted local Host/Origin
    before choosing the embedded page or binary photo upload route. The complete header is mutated in
    place to isolate tokens, so callers must supply exactly length bytes followed by NUL. Parsing
    returns a route/status description; network replies and storage mutations are handled by later
    layers.

    @param header Buffer holding the storage or HTTP header.
    @param length Number of bytes to process.
    @param localHost Pico IPv4 address accepted by the Host header check.

    @return Result - Selected route, status, body length; Route::Reject carries the
                     HTTP error status for an invalid or unsupported request.
*/
HttpRequest::Result
HttpRequest::ParseRequestHeader (
    char* header,
    size_t length,
    const char* localHost
    )
{
    size_t i;
    uint8_t c;
    char* end = nullptr;
    char* path = nullptr;
    char* version = nullptr;
    const char* p = nullptr;
    const char* nextP = nullptr;
    const char* host = nullptr;
    const char* origin = nullptr;
    const char* contentType = nullptr;
    const char* fetchSite = nullptr;
    const char* encoding = nullptr;
    bool lengthSeen;
    size_t bodyLength;
    char* line = nullptr;
    char* colon = nullptr;
    char* nextNextP = nullptr;
    char* value = nullptr;
    char* trim = nullptr;
    const char* nextNextNextP = nullptr;
    size_t digit;
    const char** slot = nullptr;

    // Check framing and control bytes before tokenizing the untrusted request buffer.
    if (length >= kHeaderCapacity || length < HTTP_HEADER_TERMINATOR_BYTES || header[length] != '\0' ||
        std::memcmp(header + length - HTTP_HEADER_TERMINATOR_BYTES, "\r\n\r\n",
                    HTTP_HEADER_TERMINATOR_BYTES) != 0) {
        return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
    }
    for (i = 0; i < length; ++i) {
        c = static_cast<unsigned char>(header[i]);
        if (!c || c == ASCII_DELETE || (c < ASCII_SPACE && c != '\r' && c != '\n' && c != '\t')) {
            return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
        }
        if ((c == '\r' && (i + 1 == length || header[i + 1] != '\n')) ||
            (c == '\n' && (!i || header[i - 1] != '\r'))) {
            return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
        }
    }

    // Split the request line in place, then leave the remaining header lines for validation.
    end = std::strstr(header, "\r\n");
    if (!end) {
        return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
    }
    *end = '\0';
    path = std::strchr(header, ' ');
    if (!path) {
        return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
    }
    *path++ = '\0';
    version = std::strchr(path, ' ');
    if (!version) {
        return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
    }
    *version++ = '\0';
    if (!*header || !*path || (std::strcmp(version, "HTTP/1.1") && std::strcmp(version, "HTTP/1.0"))) {
        return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
    }
    for (p = header; *p; ++p) {
        if (!IsHttpTokenCharacter(*p)) {
            return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
        }
    }

    for (nextP = path; *nextP; ++nextP) {
        if (static_cast<unsigned char>(*nextP) <= ASCII_SPACE) {
            return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
        }
    }

    // Collect only the headers needed for local-origin checks and bounded body framing.
    host = nullptr;
    origin = nullptr;
    contentType = nullptr;
    fetchSite = nullptr;
    encoding = nullptr;
    lengthSeen = false;
    bodyLength = 0;
    for (line = end + HTTP_LINE_END_BYTES;; line = end + HTTP_LINE_END_BYTES) {
        end = std::strstr(line, "\r\n");
        if (!end) {
            return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
        }
        if (line == end) {
            if (end + HTTP_LINE_END_BYTES != header + length) {
                return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
            }
            break;
        }
        *end = '\0';
        colon = std::strchr(line, ':');
        if (!colon || colon == line) {
            return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
        }
        for (nextNextP = line; nextNextP < colon; ++nextNextP) {
            if (!IsHttpTokenCharacter(*nextNextP)) {
                return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
            }
        }

        *colon = '\0';
        value = colon + 1;
        while (*value == ' ' || *value == '\t') {
            ++value;
        }

        trim = end;
        while (trim > value && (trim[HTTP_PREVIOUS_CHARACTER_OFFSET] == ' ' ||
                                trim[HTTP_PREVIOUS_CHARACTER_OFFSET] == '\t')) {
            *--trim = '\0';
        }

        if (CompareHttpTokenIgnoringCase(line, "Transfer-Encoding")) {
            return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
        }
        if (CompareHttpTokenIgnoringCase(line, "Expect")) {
            return RejectRequestHeader(HTTP_STATUS_EXPECTATION_FAILED);
        }
        if (CompareHttpTokenIgnoringCase(line, "Content-Length")) {
            if (lengthSeen || !*value) {
                return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
            }
            lengthSeen = true;
            for (nextNextNextP = value; *nextNextNextP; ++nextNextNextP) {
                if (*nextNextNextP < '0' || *nextNextNextP > '9') {
                    return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
                }
                digit = static_cast<size_t>(*nextNextNextP - '0');
                if (bodyLength > (SIZE_MAX - digit) / DECIMAL_RADIX) {
                    return RejectRequestHeader(HTTP_STATUS_CONTENT_TOO_LARGE);
                }
                bodyLength = bodyLength * DECIMAL_RADIX + digit;
            }
        }
        slot = nullptr;
        if (CompareHttpTokenIgnoringCase(line, "Host")) {
            slot = &host;
        }
        if (CompareHttpTokenIgnoringCase(line, "Origin")) {
            slot = &origin;
        }
        if (CompareHttpTokenIgnoringCase(line, "Content-Type")) {
            slot = &contentType;
        }
        if (CompareHttpTokenIgnoringCase(line, "Sec-Fetch-Site")) {
            slot = &fetchSite;
        }
        if (CompareHttpTokenIgnoringCase(line, "Content-Encoding")) {
            slot = &encoding;
        }
        if (slot) {
            if (*slot || !*value) {
                return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
            }
            *slot = value;
        }
    }
    // Serve only through the Pico's actual IPv4 address, preventing Host rebinding.
    if (!host) {
        return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
    }
    if (!MatchesLocalHostHeader(host, localHost)) {
        return RejectRequestHeader(HTTP_STATUS_FORBIDDEN);
    }
    if (origin && (std::strncmp(origin, "http://", HTTP_SCHEME_BYTES) ||
                   !MatchesLocalHostHeader(origin + HTTP_SCHEME_BYTES, localHost))) {
        return RejectRequestHeader(HTTP_STATUS_FORBIDDEN);
    }
    if (fetchSite && !CompareHttpTokenIgnoringCase(fetchSite, "same-origin") &&
        !CompareHttpTokenIgnoringCase(fetchSite, "none")) {
        return RejectRequestHeader(HTTP_STATUS_FORBIDDEN);
    }
    if (encoding && !CompareHttpTokenIgnoringCase(encoding, "identity")) {
        return RejectRequestHeader(HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE);
    }
    if (!std::strcmp(header, "GET") && !std::strcmp(path, "/")) {
        if (bodyLength) {
            return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
        }
        return {Route::Page, HTTP_STATUS_OK};
    }

    // Only the full-panel binary photo endpoint accepts a request body.
    if (std::strcmp(path, "/api/frame")) {
        return RejectRequestHeader(HTTP_STATUS_NOT_FOUND);
    }
    if (std::strcmp(header, "POST")) {
        return RejectRequestHeader(HTTP_STATUS_METHOD_NOT_ALLOWED);
    }
    if (!lengthSeen) {
        return RejectRequestHeader(HTTP_STATUS_LENGTH_REQUIRED);
    }
    if (bodyLength > kFrameBytes) {
        return RejectRequestHeader(HTTP_STATUS_CONTENT_TOO_LARGE);
    }
    if (bodyLength != kFrameBytes) {
        return RejectRequestHeader(HTTP_STATUS_BAD_REQUEST);
    }
    if (!contentType || !CompareHttpTokenIgnoringCase(contentType, "application/octet-stream")) {
        return RejectRequestHeader(HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE);
    }
    return {Route::Frame, HTTP_STATUS_OK, bodyLength};
} // ParseRequestHeader

/**
    Returns the HTTP status text for a parsed request result.

    Translates the parser's supported numeric status codes into HTTP reason text used by response
    construction. Unrecognized codes fall back to the internal-error response so callers always
    receive a valid status-line value. The returned string has static lifetime and requires no
    allocation.

    @param code Status or result code to classify or format.

    @return "200 OK" - HTTP status text for that code.
    @return "400 Bad Request" - HTTP status text for that code.
    @return "403 Forbidden" - HTTP status text for that code.
    @return "404 Not Found" - HTTP status text for that code.
    @return "405 Method Not Allowed" - HTTP status text for that code.
    @return "409 Conflict" - HTTP status text for that code.
    @return "411 Length Required" - HTTP status text for that code.
    @return "413 Content Too Large" - HTTP status text for that code.
    @return "415 Unsupported Media Type" - HTTP status text for that code.
    @return "417 Expectation Failed" - HTTP status text for that code.
    @return "431 Request Header Fields Too Large" - HTTP status text for that code.
    @return "501 Not Implemented" - HTTP status text for that code.
    @return "500 Internal Server Error" - Fallback status text for an unrecognized code.
*/
const char*
HttpRequest::GetHttpStatusText (
    int code
    )
{
    switch (code) {
    case HTTP_STATUS_OK:
        return "200 OK";
    case HTTP_STATUS_BAD_REQUEST:
        return "400 Bad Request";
    case HTTP_STATUS_FORBIDDEN:
        return "403 Forbidden";
    case HTTP_STATUS_NOT_FOUND:
        return "404 Not Found";
    case HTTP_STATUS_METHOD_NOT_ALLOWED:
        return "405 Method Not Allowed";
    case HTTP_STATUS_CONFLICT:
        return "409 Conflict";
    case HTTP_STATUS_LENGTH_REQUIRED:
        return "411 Length Required";
    case HTTP_STATUS_CONTENT_TOO_LARGE:
        return "413 Content Too Large";
    case HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE:
        return "415 Unsupported Media Type";
    case HTTP_STATUS_EXPECTATION_FAILED:
        return "417 Expectation Failed";
    case HTTP_STATUS_HEADERS_TOO_LARGE:
        return "431 Request Header Fields Too Large";
    case HTTP_STATUS_NOT_IMPLEMENTED:
        return "501 Not Implemented";
    default:
        return "500 Internal Server Error";
    }
} // GetHttpStatusText
