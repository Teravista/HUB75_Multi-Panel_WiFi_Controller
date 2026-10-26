#pragma once
#include <cstddef>
#include <cstdint>

namespace JpegFrameDecoder {
using Input = size_t (*)(void* context, uint8_t* bytes, size_t count);
using Output = bool (*)(void* context, const uint8_t* bytes, size_t count, size_t offset);

// Decodes one baseline JPEG using bounded caller-owned memory.
bool Decode(Input input, void* inputContext, Output output, void* outputContext, uint16_t width, uint16_t height, void* work, size_t workBytes, int& error);

} // namespace JpegFrameDecoder
