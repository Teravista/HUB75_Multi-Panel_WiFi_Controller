#pragma once

#include <cstddef>
#include <cstdint>

// Logical RGB888/RGB565 pixels or indexed JPEG; panel mapping is separate.
enum class MediaType : uint8_t { Photo = 1, Video = 2 };
enum class PixelFormat : uint8_t { RGB888 = 1, RGB565 = 2, JPEG = 3 };

/**
    Describes logical media without owning its pixels or controlling playback.

    Upload reception and SD storage share dimensions, pixel format, frame count
    and a rational frame rate through this value. Validation protects payload-size
    calculations before a descriptor is accepted. Photos use one frame and FPS
    zero; raw and compressed videos carry their own timing and payload size.
*/
struct MediaMetadata {
    MediaType type = MediaType::Photo;
    PixelFormat pixelFormat = PixelFormat::RGB888;
    bool pingPong = false;
    uint64_t encodedBytes = 0; // Indexed JPEG payload; zero for raw pixels.
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t frameCount = 1;
    uint32_t fpsNumerator = 0;
    uint32_t fpsDenominator = 1;

    // Creates metadata for a single RGB888 photo.
    static MediaMetadata Photo(uint16_t width, uint16_t height);

    // Returns the readable name of the pixel format.
    const char* FormatName(void) const;

    // Returns the number of bytes occupied by one raw pixel.
    size_t PixelBytes(void) const;

    // Returns the byte count of one uncompressed frame.
    uint64_t FrameBytes(void) const;

    // Validates the descriptor and calculates its complete pixel payload size.
    bool PayloadBytes(uint64_t& bytes) const;

    // Checks whether a library photo has supported dimensions and single-frame timing.
    bool IsPhotoWithin(uint16_t panelWidth, uint16_t panelHeight) const;

    // Checks whether a single photo exactly matches the supplied dimensions.
    bool IsPhotoFor(uint16_t panelWidth, uint16_t panelHeight) const;

};

// Explicit wire layout, independent of C++ structure padding and host byte order.
namespace MediaMetadataCodec {
constexpr size_t HeaderBytes = 32;
constexpr uint8_t Version = 1;
constexpr uint8_t JpegVersion = 2;
constexpr uint8_t PingPongFlag = 1;

// Writes a 16-bit unsigned value in little-endian byte order.
void WriteUnsigned16LittleEndian(uint8_t* out, uint16_t value);

// Reads a 16-bit unsigned value in little-endian byte order.
uint16_t ReadUnsigned16LittleEndian(const uint8_t* in);

// Writes a 32-bit unsigned value in little-endian byte order.
void WriteUnsigned32LittleEndian(uint8_t* out, uint32_t value);

// Reads a 32-bit unsigned value in little-endian byte order.
uint32_t ReadUnsigned32LittleEndian(const uint8_t* in);

// Writes a 64-bit unsigned value in little-endian byte order.
void WriteUnsigned64LittleEndian(uint8_t* out, uint64_t value);

// Reads a 64-bit unsigned value in little-endian byte order.
uint64_t ReadUnsigned64LittleEndian(const uint8_t* in);

// Serializes valid metadata into a fixed-size transfer descriptor.
bool EncodeMediaDescriptor(const MediaMetadata& metadata, uint8_t* out, size_t capacity);

// Validates and decodes a transfer descriptor without reading its pixels.
bool DecodeMediaDescriptor(const uint8_t* in, size_t bytes, MediaMetadata& metadata);

} // namespace MediaMetadataCodec
