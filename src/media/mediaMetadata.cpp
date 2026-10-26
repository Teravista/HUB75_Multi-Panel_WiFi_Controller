#include "media/include/mediaMetadata.hpp"
#include "config/defines.h"
#include <cstring>

#define MEDIA_MAGIC_BYTES 4
#define JPEG_MINIMUM_RECORD_BYTES 24
#define MEDIA_VERSION_OFFSET 4
#define MEDIA_TYPE_OFFSET 5
#define MEDIA_FORMAT_OFFSET 6
#define MEDIA_FLAGS_OFFSET 7
#define MEDIA_WIDTH_OFFSET 8
#define MEDIA_HEIGHT_OFFSET 10
#define MEDIA_FRAME_COUNT_OFFSET 12
#define MEDIA_FPS_NUMERATOR_OFFSET 16
#define MEDIA_FPS_DENOMINATOR_OFFSET 20
#define MEDIA_PAYLOAD_BYTES_OFFSET 24
#define MEDIA_UINT32_BYTES 4
#define MEDIA_UINT32_BYTE_TWO_INDEX 2
#define MEDIA_UINT32_BYTE_THREE_INDEX 3
#define MEDIA_WORD_BYTE_TWO_SHIFT 16
#define MEDIA_WORD_BYTE_THREE_SHIFT 24
#define MEDIA_UINT32_BITS 32

/**
    Creates metadata for a single RGB888 photo.

    @param width Image width in pixels.
    @param height Image height in pixels.

    @return metadata - Single-frame RGB888 photo descriptor with the requested dimensions.
*/
MediaMetadata
MediaMetadata::Photo (
    uint16_t width,
    uint16_t height
    )
{
    MediaMetadata metadata;

    metadata = {};
    metadata.width = width;
    metadata.height = height;
    return metadata;
} // Photo

/**
    Returns the readable name of the pixel format.

    @return "RGB888" - Pixels contain one unsigned byte for each RGB channel.
    @return "RGB565" - Pixels contain a little-endian packed 16-bit color.
    @return "JPEG" - Payload contains indexed baseline JPEG frames.
    @return "unknown" - The descriptor contains an unsupported format identifier.
*/
const char*
MediaMetadata::FormatName (
    void
    ) const
{
    if (pixelFormat == PixelFormat::RGB888) {
        return "RGB888";
    }
    if (pixelFormat == PixelFormat::RGB565) {
        return "RGB565";
    }
    if (pixelFormat == PixelFormat::JPEG) {
        return "JPEG";
    }
    return "unknown";
} // FormatName

/**
    Returns the number of bytes occupied by one raw pixel.

    @return IMAGE_BYTES_PER_PIXEL - One RGB888 pixel occupies three bytes.
    @return RGB565_PIXEL_BYTES - One RGB565 pixel occupies two bytes.
    @return 0 - JPEG is variable-length or the pixel format is unsupported.
*/
size_t
MediaMetadata::PixelBytes (
    void
    ) const
{
    if (pixelFormat == PixelFormat::RGB888) {
        return IMAGE_BYTES_PER_PIXEL;
    }
    if (pixelFormat == PixelFormat::RGB565) {
        return RGB565_PIXEL_BYTES;
    }
    return 0;
} // PixelBytes

/**
    Returns the byte count of one uncompressed frame.

    @return value - Width multiplied by height and supported pixel size.
    @return 0 - At least one dimension is zero or the format is unsupported.
*/
uint64_t
MediaMetadata::FrameBytes (
    void
    ) const
{
    return uint64_t(width) * height * PixelBytes();
} // FrameBytes

/**
    Validates the descriptor and calculates its complete pixel payload size.

    Rejects unsupported types/formats and inconsistent photo or video timing.
    The frame count is checked before multiplication so oversized descriptions
    cannot wrap a 64-bit payload length. Output remains unchanged on failure;
    storage and transports must also apply their own capacity and type limits.

    @param bytes Receives the complete payload length in bytes after validation.

    @return true - All fields are consistent and the payload size is representable.
    @return false - A field is unsupported, empty, inconsistent or would overflow.
*/
bool
MediaMetadata::PayloadBytes (
    uint64_t& bytes
    ) const
{
    uint64_t frameBytes;

    if (!width || !height || !frameCount || !fpsDenominator || (!PixelBytes() && pixelFormat != PixelFormat::JPEG)) {
        return false;
    }
    if (type == MediaType::Photo) {
        if (frameCount != 1 || fpsNumerator != 0 || fpsDenominator != 1 || pingPong) {
            return false;
        }
    } else if (type != MediaType::Video || !fpsNumerator) {
        return false;
    }

    // JPEG payloads contain an index and variable records rather than raw frames.
    if (pixelFormat == PixelFormat::JPEG) {
        if (encodedBytes < uint64_t(frameCount) * JPEG_MINIMUM_RECORD_BYTES ||
            encodedBytes > UINT32_MAX) {
            return false;
        }
        bytes = encodedBytes;
        return true;
    }
    if (encodedBytes) {
        return false;
    }

    // Check the product before publishing the length to a caller.
    frameBytes = FrameBytes();
    if (frameCount > UINT64_MAX / frameBytes) {
        return false;
    }
    bytes = frameBytes * frameCount;
    return true;
} // PayloadBytes

/**
    Checks whether a single photo exactly matches the supplied dimensions.

    @param panelWidth Available logical panel width in pixels.
    @param panelHeight Available logical panel height in pixels.

    @return true - The descriptor is a valid photo with exactly the requested dimensions.
    @return false - The descriptor is invalid, is not a photo or has different dimensions.
*/
bool
MediaMetadata::IsPhotoFor (
    uint16_t panelWidth,
    uint16_t panelHeight
    ) const
{
    uint64_t bytes;

    return PayloadBytes(bytes) && type == MediaType::Photo && pixelFormat == PixelFormat::RGB888 && width == panelWidth &&
           height == panelHeight;
} // IsPhotoFor

/**
    Writes a 16-bit unsigned value in little-endian byte order.

    @param out Writable destination buffer; the caller must provide sufficient storage.
    @param value Unsigned integer value to encode.
*/
void
MediaMetadataCodec::WriteUnsigned16LittleEndian (
    uint8_t* out,
    uint16_t value
    )
{
    out[0] = uint8_t(value);
    out[1] = uint8_t(value >> BITS_PER_BYTE);
} // WriteUnsigned16LittleEndian

/**
    Reads a 16-bit unsigned value in little-endian byte order.

    @param in Read-only source bytes in the format required by this operation.

    @return value - Unsigned integer decoded from the source bytes in little-endian order.
*/
uint16_t
MediaMetadataCodec::ReadUnsigned16LittleEndian (
    const uint8_t* in
    )
{
    return uint16_t(uint16_t(in[0]) | (uint16_t(in[1]) << BITS_PER_BYTE));
} // ReadUnsigned16LittleEndian

/**
    Writes a 32-bit unsigned value in little-endian byte order.

    @param out Writable destination buffer; the caller must provide sufficient storage.
    @param value Unsigned integer value to encode.
*/
void
MediaMetadataCodec::WriteUnsigned32LittleEndian (
    uint8_t* out,
    uint32_t value
    )
{
    unsigned i;

    for (i = 0; i < MEDIA_UINT32_BYTES; ++i) {
        out[i] = uint8_t(value >> (BITS_PER_BYTE * i));
    }
} // WriteUnsigned32LittleEndian

/**
    Reads a 32-bit unsigned value in little-endian byte order.

    @param in Read-only source bytes in the format required by this operation.

    @return value - Unsigned integer decoded from the source bytes in little-endian order.
*/
uint32_t
MediaMetadataCodec::ReadUnsigned32LittleEndian (
    const uint8_t* in
    )
{
    return uint32_t(in[0]) | (uint32_t(in[1]) << BITS_PER_BYTE) |
           (uint32_t(in[MEDIA_UINT32_BYTE_TWO_INDEX]) << MEDIA_WORD_BYTE_TWO_SHIFT) |
           (uint32_t(in[MEDIA_UINT32_BYTE_THREE_INDEX]) << MEDIA_WORD_BYTE_THREE_SHIFT);
} // ReadUnsigned32LittleEndian

/**
    Writes a 64-bit unsigned value in little-endian byte order.

    @param out Writable destination buffer; the caller must provide sufficient storage.
    @param value Unsigned integer value to encode.
*/
void
MediaMetadataCodec::WriteUnsigned64LittleEndian (
    uint8_t* out,
    uint64_t value
    )
{
    WriteUnsigned32LittleEndian(out, uint32_t(value));
    WriteUnsigned32LittleEndian(out + MEDIA_UINT32_BYTES, uint32_t(value >> MEDIA_UINT32_BITS));
} // WriteUnsigned64LittleEndian

/**
    Reads a 64-bit unsigned value in little-endian byte order.

    @param in Read-only source bytes in the format required by this operation.

    @return value - Unsigned integer decoded from the source bytes in little-endian order.
*/
uint64_t
MediaMetadataCodec::ReadUnsigned64LittleEndian (
    const uint8_t* in
    )
{
    return ReadUnsigned32LittleEndian(in) |
           (uint64_t(ReadUnsigned32LittleEndian(in + MEDIA_UINT32_BYTES)) << MEDIA_UINT32_BITS);
} // ReadUnsigned64LittleEndian

/**
    Serializes valid metadata into a fixed-size transfer descriptor.

    Validates before writing any output. The explicit versioned layout keeps
    file/transfer metadata independent of compiler padding. Only the ping-pong flag is used; other bits stay
    zero; the pixel payload is described but is never accessed by this function.

    @param metadata Logical media description to encode.
    @param out Writable destination containing at least HeaderBytes bytes.
    @param capacity Number of bytes available at out.

    @return true - A complete version-1 raw or version-2 JPEG descriptor was written.
    @return false - Metadata, output pointer or capacity is invalid; output is unchanged.
*/
bool
MediaMetadataCodec::EncodeMediaDescriptor (
    const MediaMetadata& metadata,
    uint8_t* out,
    size_t capacity
    )
{
    uint64_t payload;

    if (!out || capacity < HeaderBytes || !metadata.PayloadBytes(payload)) {
        return false;
    }

    // Serialize every field explicitly; no structure bytes are copied.
    std::memcpy(out, "H75M", MEDIA_MAGIC_BYTES);
    if (metadata.pixelFormat == PixelFormat::JPEG) {
        out[MEDIA_VERSION_OFFSET] = JpegVersion;
    } else {
        out[MEDIA_VERSION_OFFSET] = Version;
    }
    out[MEDIA_TYPE_OFFSET] = uint8_t(metadata.type);
    out[MEDIA_FORMAT_OFFSET] = uint8_t(metadata.pixelFormat);
    if (metadata.pingPong) {
        out[MEDIA_FLAGS_OFFSET] = PingPongFlag;
    } else {
        out[MEDIA_FLAGS_OFFSET] = 0;
    }
    WriteUnsigned16LittleEndian(out + MEDIA_WIDTH_OFFSET, metadata.width);
    WriteUnsigned16LittleEndian(out + MEDIA_HEIGHT_OFFSET, metadata.height);
    WriteUnsigned32LittleEndian(out + MEDIA_FRAME_COUNT_OFFSET, metadata.frameCount);
    WriteUnsigned32LittleEndian(out + MEDIA_FPS_NUMERATOR_OFFSET, metadata.fpsNumerator);
    WriteUnsigned32LittleEndian(out + MEDIA_FPS_DENOMINATOR_OFFSET, metadata.fpsDenominator);
    WriteUnsigned64LittleEndian(out + MEDIA_PAYLOAD_BYTES_OFFSET, payload);
    return true;
} // EncodeMediaDescriptor

/**
    Validates and decodes a transfer descriptor without reading its pixels.

    Checks framing, version and reserved flags, reconstructs the fields and
    recomputes their payload length. Decoding publishes the result only after
    every check succeeds. This validates a description, not the actual media
    bytes, and does not enable video playback or change legacy photo uploads.

    @param in Read-only versioned descriptor bytes.
    @param bytes Exact number of descriptor bytes supplied.
    @param metadata Receives the validated descriptor on success.

    @return true - Framing, fields and declared payload length are valid.
    @return false - The input is truncated, unsupported or inconsistent; output is unchanged.
*/
bool
MediaMetadataCodec::DecodeMediaDescriptor (
    const uint8_t* in,
    size_t bytes,
    MediaMetadata& metadata
    )
{
    MediaMetadata decoded;
    uint64_t payload;

    if (!in || bytes != HeaderBytes || std::memcmp(in, "H75M", MEDIA_MAGIC_BYTES) ||
        (in[MEDIA_VERSION_OFFSET] != Version && in[MEDIA_VERSION_OFFSET] != JpegVersion) || (in[MEDIA_FLAGS_OFFSET] & ~PingPongFlag) != 0) {
        return false;
    }

    // Reconstruct into a temporary value to keep rejected input private.
    decoded.type = MediaType(in[MEDIA_TYPE_OFFSET]);
    decoded.pixelFormat = PixelFormat(in[MEDIA_FORMAT_OFFSET]);
    decoded.pingPong = (in[MEDIA_FLAGS_OFFSET] & PingPongFlag) != 0;
    decoded.width = ReadUnsigned16LittleEndian(in + MEDIA_WIDTH_OFFSET);
    decoded.height = ReadUnsigned16LittleEndian(in + MEDIA_HEIGHT_OFFSET);
    decoded.frameCount = ReadUnsigned32LittleEndian(in + MEDIA_FRAME_COUNT_OFFSET);
    decoded.fpsNumerator = ReadUnsigned32LittleEndian(in + MEDIA_FPS_NUMERATOR_OFFSET);
    decoded.fpsDenominator = ReadUnsigned32LittleEndian(in + MEDIA_FPS_DENOMINATOR_OFFSET);
    if (decoded.pixelFormat == PixelFormat::JPEG) {
        if (in[MEDIA_VERSION_OFFSET] != JpegVersion) {
            return false;
        }
        decoded.encodedBytes = ReadUnsigned64LittleEndian(in + MEDIA_PAYLOAD_BYTES_OFFSET);
    } else if (in[MEDIA_VERSION_OFFSET] != Version) {
        return false;
    }
    if (!decoded.PayloadBytes(payload) ||
        ReadUnsigned64LittleEndian(in + MEDIA_PAYLOAD_BYTES_OFFSET) != payload) {
        return false;
    }
    metadata = decoded;
    return true;
} // DecodeMediaDescriptor

/**
    Checks whether a library photo has supported dimensions and single-frame timing.

    Library records retain raw RGB888, raw RGB565 or indexed JPEG metadata. The
    legacy raw-photo endpoint continues to use IsPhotoFor and admits RGB888 only.

    @param panelWidth Required full logical panel width.
    @param panelHeight Required full logical panel height.

    @return true - The descriptor is a valid single full-panel library photo.
    @return false - Type, dimensions, frame count, encoding or timing is invalid.
*/
bool
MediaMetadata::IsPhotoWithin (
    uint16_t panelWidth,
    uint16_t panelHeight
    ) const
{
    uint64_t bytes;
    return PayloadBytes(bytes) && type == MediaType::Photo && width == panelWidth && height == panelHeight;
} // IsPhotoWithin
