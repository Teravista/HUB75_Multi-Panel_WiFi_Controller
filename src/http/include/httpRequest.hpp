#pragma once
#include <cstddef>
#include "http/api/include/httpControl.hpp"

namespace HttpRequest {
constexpr size_t kFrameBytes = 128 * 256 * 3;
constexpr size_t kHeaderCapacity = 1536;
enum class Route { Page, Frame, Video, Control, ApiInfo, Reject };

struct Result {
    Route route;
    int status;
    size_t bodyBytes = 0;
    HttpControl::Command command{};
};

// Validates an HTTP request header and identifies its API or page route.
Result ParseRequestHeader(char* header, size_t length, const char* localHost);

// Returns the HTTP status text for a parsed request result.
const char* GetHttpStatusText(int code);

} // namespace HttpRequest
