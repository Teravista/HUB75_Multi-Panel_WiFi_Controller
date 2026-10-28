#pragma once
#include <cstdint>
#include <cstring>
#include "config/defines.h"

// Bounded URL adapter. Control requests use numeric query parameters; pixel
// uploads keep their prepared binary payloads. No heap or additional body buffer.
namespace HttpControl {
struct Command {
    uint8_t op = 0;
    uint32_t id = 0;
    uint32_t offset = 0;
    bool binary = false;
    bool token = false;
    bool info = false;
};

// Parses a bounded unsigned decimal control value.
bool ParseUnsignedDecimal(const char* text, uint32_t& value, bool positive = false);

// Validates a supported control request and extracts its arguments.
int ParseControllerCommand(const char* method, char* path, Command& command);

} // namespace HttpControl
