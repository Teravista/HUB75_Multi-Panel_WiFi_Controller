#pragma once
#include <cstddef>

namespace HttpRequest {
constexpr size_t kFrameBytes = 128 * 256 * 3;
constexpr size_t kHeaderCapacity = 1536;

enum class Route { Page, Frame, Video, Reject };

struct Result {
    Route route;
    int status;
    size_t bodyBytes = 0;
};

// Validates an HTTP header and selects the page or binary media route.
Result ParseRequestHeader(char* header, size_t length, const char* localHost);

// Returns the HTTP status text for a parsed request result.
const char* GetHttpStatusText(int code);

} // namespace HttpRequest
