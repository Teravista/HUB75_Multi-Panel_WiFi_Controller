#pragma once
#include <cstddef>
#include <cstdint>


// Logical media, before panel mapping/wiring correction. RGB888 means one
// unsigned byte each for R, G and B; RGB565 is defined below. Both are
// uncompressed and row-major. Legacy photo endpoints remain RGB888; the photo
// library supports RGB888, RGB565 and a single indexed JPEG frame.
// JPEG video contains an index and independently encoded baseline JPEG frames.
enum class MediaType : uint8_t { Photo = 1, Video = 2 };
// RGB565 uses little-endian 16-bit words: R[15:11], G[10:5], B[4:0].
enum class PixelFormat : uint8_t { RGB888 = 1, RGB565 = 2, JPEG = 3 };

/**
    Describes stored or transferred media consistently across uploads and playback.

    The descriptor carries dimensions, placement, pixel encoding, frame timing and loop behavior
    without owning any pixel bytes. Its validation helpers protect file and transfer size
    calculations, while the codec namespace handles the versioned wire representation. This common
    description lets photos, raw video and indexed JPEG use the same storage and display
    boundaries.
*/
struct MediaMetadata {
    MediaType type = MediaType::Photo;
    PixelFormat pixelFormat = PixelFormat::RGB888;
    bool pingPong = false;
    uint8_t jpegQuality = 0; // Encoder setting 1..95; 0 means unknown/legacy.
    uint16_t width = 0;
    uint16_t height = 0;
    static constexpr uint16_t AutoOffset = UINT16_MAX;
    uint16_t offsetX = AutoOffset;
    uint16_t offsetY = AutoOffset;

    // Returns the stored horizontal placement or its centered default.
    uint16_t Left(uint16_t panelWidth) const;

    // Returns the stored vertical placement or its centered default.
    uint16_t Top(uint16_t panelHeight) const;

    // Reports whether the metadata specifies an explicit image placement.
    bool HasPlacement(void) const;

    uint32_t frameCount = 1;
    uint32_t fpsNumerator = 0;
    uint32_t fpsDenominator = 1;
    uint64_t encodedBytes = 0; // JPEG index + variable records; zero for raw formats.

    // Returns the readable name of the stored pixel format.
    const char* FormatName(void) const;

    // Creates metadata for a single RGB888 photo.
    static MediaMetadata Photo(uint16_t width, uint16_t height);

    // Returns the byte count of one raw pixel, or zero for JPEG.
    size_t PixelBytes(void) const;

    // Returns the byte count of one uncompressed stored frame.
    uint64_t FrameBytes(void) const;

    // Returns the complete encoded or raw media payload size.
    bool PayloadBytes(uint64_t& bytes) const;

    // Checks whether the stored dimensions and placement fit the panel.
    bool FitsPanel(uint16_t panelWidth, uint16_t panelHeight) const;

    // Checks whether a single photo fits within the supplied dimensions.
    bool IsPhotoWithin(uint16_t panelWidth, uint16_t panelHeight) const;

    // Checks whether a single photo exactly matches the supplied dimensions.
    bool IsPhotoFor(uint16_t panelWidth, uint16_t panelHeight) const;

};

// Stable descriptor for storage and media uploads. Never send
// sizeof(MediaMetadata) bytes: C++ padding/host endianness are not the format.
namespace MediaMetadataCodec {
constexpr size_t HeaderBytes = 32;
constexpr uint8_t Version = 1;
constexpr uint8_t JpegVersion = 2;
constexpr uint8_t JpegQualityVersion = 3; // byte 7: ping-pong bit 0, quality bits 1..7.
// Same 32-byte header: v4 packs (dimension-1) and exact pixel offsets into
// the two size words. Older versions retain their implicit centered placement.
constexpr uint8_t PlacementVersion = 4;
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

// Serializes media metadata into the versioned transfer descriptor.
bool EncodeMediaDescriptor(const MediaMetadata& metadata, uint8_t* out, size_t capacity);

// Validates and reads a versioned media transfer descriptor.
bool DecodeMediaDescriptor(const uint8_t* in, size_t bytes, MediaMetadata& metadata);

} // namespace MediaMetadataCodec
