#pragma once
#include <cstddef>
#include <cstdint>

namespace JpegFrameDecoder {
using Input = size_t (*)(void* context, uint8_t* bytes, size_t count);
using Output = bool (*)(void* context, const uint8_t* bytes, size_t count, size_t offset);
using Clock = uint64_t (*)(void* context);

struct Profile {
    uint64_t prepareUs = 0;
    uint64_t entropyUs = 0;
    uint64_t transformUs = 0;
    uint64_t colorUs = 0;
    uint32_t mcus = 0;
    uint32_t idctBlocks = 0;
    uint32_t dcBlocks = 0;
    uint32_t clockReads = 0;
};

// Decodes or validates one JPEG frame using bounded input and output callbacks.
bool Decode(Input input, void* inputContext, Output output, void* outputContext, uint16_t width, uint16_t height, void* work, size_t workBytes, int& error, Profile* profile = nullptr, Clock clock = nullptr, void* clockContext = nullptr, bool outputRGB565 = false);

} // namespace JpegFrameDecoder
