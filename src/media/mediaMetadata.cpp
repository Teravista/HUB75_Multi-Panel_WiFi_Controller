#include "media/include/mediaMetadata.hpp"
#include "config/defines.h"
#include <cstring>
#define JPEG_MIN_FRAME_PAYLOAD_BYTES 24
#define MAX_MEDIA_PAYLOAD_BYTES_U 0xffffffffu
#define MEDIA_FLAGS_OFFSET 7
#define MEDIA_FORMAT_OFFSET 6
#define MEDIA_FPS_DENOMINATOR_OFFSET 20
#define MEDIA_FPS_NUMERATOR_OFFSET 16
#define MEDIA_FRAME_COUNT_OFFSET 12
#define MEDIA_HEIGHT_OFFSET 10
#define MEDIA_JPEG_QUALITY_SHIFT 1
#define MEDIA_PAYLOAD_BYTES_OFFSET 24
#define MEDIA_PLACEMENT_HEIGHT_MASK_U 255u
#define MEDIA_PLACEMENT_RESERVED_MASK_U 0xc000u
#define MEDIA_PLACEMENT_WIDTH_MASK_U 127u
#define MEDIA_PLACEMENT_X_SHIFT 7
#define MEDIA_PLACEMENT_Y_SHIFT 8
#define MEDIA_TYPE_OFFSET 5
#define MEDIA_VERSION_OFFSET 4
#define MEDIA_WIDTH_OFFSET 8
#define UINT32_BITS 32

/**
    Returns the stored horizontal placement or its centered default.

    @param panelWidth Available logical panel width in pixels.

    @return value - Left edge in logical pixels, using the explicit offset or horizontal
                    centering.
*/
uint16_t
MediaMetadata::Left (
    uint16_t panelWidth
    ) const
{
    if (offsetX == AutoOffset) {
        return (panelWidth - width) / CENTER_ALIGNMENT_DIVISOR;
    } else {
        return offsetX;
    }
} // Left

/**
    Returns the stored vertical placement or its centered default.

    @param panelHeight Available logical panel height in pixels.

    @return value - Top edge in logical pixels, using the explicit offset or vertical centering.
*/
uint16_t
MediaMetadata::Top (
    uint16_t panelHeight
    ) const
{
    if (offsetY == AutoOffset) {
        return (panelHeight - height) / CENTER_ALIGNMENT_DIVISOR;
    } else {
        return offsetY;
    }
} // Top

/**
    Reports whether the metadata specifies an explicit image placement.

    @return true - At least one explicit placement coordinate is present.
    @return false - Both placement coordinates use automatic centering.
*/
bool
MediaMetadata::HasPlacement (
    void
    ) const
{
    return offsetX != AutoOffset || offsetY != AutoOffset;
} // HasPlacement

/**
    Returns the readable name of the stored pixel format.

    @return "JPEG" - The pixel format contains independently compressed JPEG frames.
    @return "RGB565" - The pixel format contains raw 16-bit RGB565 pixels.
    @return "RGB888" - The pixel format contains raw 24-bit RGB888 pixels, or the default format
                       name.
*/
const char*
MediaMetadata::FormatName (
    void
    ) const
{
    if (pixelFormat == PixelFormat::JPEG) {
        return "JPEG";
    } else {
        if (pixelFormat == PixelFormat::RGB565) {
            return "RGB565";
        } else {
            return "RGB888";
        }
    }
} // FormatName

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
    Returns the byte count of one raw pixel, or zero for JPEG.

    @return 3 - One raw RGB888 pixel occupies three bytes.
    @return 2 - One raw RGB565 pixel occupies two bytes.
    @return 0 - JPEG is compressed and has no fixed byte count per pixel.
*/
size_t
MediaMetadata::PixelBytes (
    void
    ) const
{
    if (pixelFormat == PixelFormat::RGB888) {
        return RGB888_PIXEL_BYTES;
    } else {
        if (pixelFormat == PixelFormat::RGB565) {
            return RGB565_PIXEL_BYTES;
        } else {
            return 0;
        }
    }
} // PixelBytes

/**
    Returns the byte count of one uncompressed stored frame.

    @return value - Width times height times raw bytes per pixel.
    @return 0 - The format is compressed or the image dimensions are empty.
*/
uint64_t
MediaMetadata::FrameBytes (
    void
    ) const
{
    return uint64_t(width) * height * PixelBytes();
} // FrameBytes

/**
    Returns the complete encoded or raw media payload size.

    Validates the relationship between media type, frame count, frame rate, placement and color
    format before producing a byte count. Raw media uses checked dimension and frame-size
    multiplication, while JPEG relies on the descriptor's bounded encoded length. Invalid
    combinations return before the output can be treated as a trusted size for allocation,
    transfer or file-boundary checks.

    @param bytes Output number of entries or bytes produced by this operation.

    @return true - The metadata is valid and bytes contains the payload size.
    @return false - Dimensions, format, placement, frame rate or payload size are invalid.
*/
bool
MediaMetadata::PayloadBytes (
    uint64_t& bytes
    ) const
{
    bool invalidJpegPayload;
    bool invalidJpegTimingOrSize;
    uint32_t invalidJpegTiming;
    uint64_t frameBytes;

    if (HasPlacement() && (offsetX == AutoOffset || offsetY == AutoOffset ||
                           !FitsPanel(LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT))) {
        return false;
    }
    if (jpegQuality > MAX_JPEG_QUALITY || (pixelFormat != PixelFormat::JPEG && jpegQuality)) {
        return false;
    }
    if (!width || !height || !frameCount || !fpsDenominator) {
        return false;
    }
    if (pixelFormat == PixelFormat::JPEG) {
        invalidJpegPayload = {};
        invalidJpegTimingOrSize = {};
        invalidJpegTiming = {};
        if (type == MediaType::Photo) {
            invalidJpegTiming = frameCount != 1 || fpsNumerator || fpsDenominator != 1 || pingPong;
        } else {
            invalidJpegTiming = type != MediaType::Video || !fpsNumerator;
        }
        if (invalidJpegTiming) {
            invalidJpegTimingOrSize = true;
        } else {
            invalidJpegTimingOrSize = bool(encodedBytes < uint64_t(frameCount) * JPEG_MIN_FRAME_PAYLOAD_BYTES);
        }
        if (invalidJpegTimingOrSize) {
            invalidJpegPayload = true;
        } else {
            invalidJpegPayload = bool(encodedBytes > MAX_MEDIA_PAYLOAD_BYTES_U);
        }
        if (invalidJpegPayload) {
            return false;
        }
        bytes = encodedBytes;
        return true;
    }
    if (!PixelBytes() || encodedBytes) {
        return false;
    }
    if (type == MediaType::Photo) {
        if (frameCount != 1 || fpsNumerator != 0 || fpsDenominator != 1 || pingPong) {
            return false;
        }
    } else if (type != MediaType::Video || !fpsNumerator) {
        return false;
    }
    frameBytes = uint64_t(width) * height * PixelBytes();
    if (frameCount > UINT64_MAX / frameBytes) {
        return false;
    }
    bytes = frameBytes * frameCount;
    return true;
} // PayloadBytes

/**
    Checks whether the stored dimensions and placement fit the panel.

    Dimensions describe only stored content, with optional explicit placement. Missing borders
    are generated as black; legacy receivers stay full-size.

    @param panelWidth Available logical panel width in pixels.
    @param panelHeight Available logical panel height in pixels.

    @return true - The image and its placement lie entirely within the logical panel.
    @return false - The image is empty, too large or extends outside the panel.
*/
bool
MediaMetadata::FitsPanel (
    uint16_t panelWidth,
    uint16_t panelHeight
    ) const
{
    return width && height && width <= panelWidth && height <= panelHeight &&
           Left(panelWidth) <= panelWidth - width && Top(panelHeight) <= panelHeight - height;
} // FitsPanel

/**
    Checks whether a single photo fits within the supplied dimensions.

    @param panelWidth Available logical panel width in pixels.
    @param panelHeight Available logical panel height in pixels.

    @return true - The descriptor is a valid photo that fits inside the panel.
    @return false - The descriptor is invalid, is not a photo or does not fit.
*/
bool
MediaMetadata::IsPhotoWithin (
    uint16_t panelWidth,
    uint16_t panelHeight
    ) const
{
    uint64_t bytes;

    return PayloadBytes(bytes) && type == MediaType::Photo && FitsPanel(panelWidth, panelHeight);
} // IsPhotoWithin

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

    for (i = 0; i < UINT32_BYTES; ++i) {
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
           (uint32_t(in[UINT32_BYTE_TWO_INDEX]) << WORD_BYTE_TWO_SHIFT) |
           (uint32_t(in[UINT32_BYTE_THREE_INDEX]) << WORD_BYTE_THREE_SHIFT);
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
    WriteUnsigned32LittleEndian(out + UINT32_BYTES, uint32_t(value >> UINT32_BITS));
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
           (uint64_t(ReadUnsigned32LittleEndian(in + UINT32_BYTES)) << UINT32_BITS);
} // ReadUnsigned64LittleEndian

/**
    Serializes media metadata into the versioned transfer descriptor.

    Checks that the metadata describes a valid payload, then writes its versioned wire fields in
    the defined byte order. Optional JPEG length/quality and placement fields are represented
    according to the descriptor version rather than inferred from pixels. Callers must use the
    returned success flag before transmitting or persisting the output buffer.

    @param metadata Media dimensions, pixel format, placement and playback settings.
    @param out Writable destination buffer; the caller must provide sufficient storage.
    @param capacity Number of elements available in the destination buffer, including any text
                    terminator.

    @return true - The descriptor was encoded into the output buffer.
    @return false - The metadata or destination capacity is invalid.
*/
bool
MediaMetadataCodec::EncodeMediaDescriptor (
    const MediaMetadata& metadata,
    uint8_t* out,
    size_t capacity
    )
{
    uint64_t payload;
    uint32_t loopFlag;
    uint32_t encodedWidth;
    uint32_t encodedHeight;

    if (!out || capacity < HeaderBytes || !metadata.PayloadBytes(payload)) {
        return false;
    }

    // These field offsets and little-endian values define the persistent binary format.
    std::memcpy(out, "H75M", MEDIA_MAGIC_BYTES);
    if (metadata.HasPlacement()) {
        out[MEDIA_VERSION_OFFSET] = PlacementVersion;
    } else {
        if (metadata.pixelFormat == PixelFormat::JPEG) {
            if (metadata.jpegQuality) {
                out[MEDIA_VERSION_OFFSET] = JpegQualityVersion;
            } else {
                out[MEDIA_VERSION_OFFSET] = JpegVersion;
            }
        } else {
            out[MEDIA_VERSION_OFFSET] = Version;
        }
    }
    out[MEDIA_TYPE_OFFSET] = uint8_t(metadata.type);
    out[MEDIA_FORMAT_OFFSET] = uint8_t(metadata.pixelFormat);
    loopFlag = {};
    if (metadata.pingPong) {
        loopFlag = PingPongFlag;
    } else {
        loopFlag = 0;
    }
    out[MEDIA_FLAGS_OFFSET] =
        uint8_t((metadata.jpegQuality << MEDIA_JPEG_QUALITY_SHIFT) | loopFlag);
    encodedWidth = {};
    if (metadata.HasPlacement()) {
        encodedWidth = uint16_t((metadata.width - 1) | (metadata.offsetX << MEDIA_PLACEMENT_X_SHIFT));
    } else {
        encodedWidth = metadata.width;
    }
    WriteUnsigned16LittleEndian(out + MEDIA_WIDTH_OFFSET, encodedWidth);
    encodedHeight = {};
    if (metadata.HasPlacement()) {
        encodedHeight = uint16_t((metadata.height - 1) | (metadata.offsetY << MEDIA_PLACEMENT_Y_SHIFT));
    } else {
        encodedHeight = metadata.height;
    }
    WriteUnsigned16LittleEndian(out + MEDIA_HEIGHT_OFFSET, encodedHeight);
    WriteUnsigned32LittleEndian(out + MEDIA_FRAME_COUNT_OFFSET, metadata.frameCount);
    WriteUnsigned32LittleEndian(out + MEDIA_FPS_NUMERATOR_OFFSET, metadata.fpsNumerator);
    WriteUnsigned32LittleEndian(out + MEDIA_FPS_DENOMINATOR_OFFSET, metadata.fpsDenominator);
    WriteUnsigned64LittleEndian(out + MEDIA_PAYLOAD_BYTES_OFFSET, payload);
    return true;
} // EncodeMediaDescriptor

/**
    Validates and reads a versioned media transfer descriptor.

    Checks descriptor magic, version, reserved fields and available bytes before reconstructing
    media metadata. Older supported versions receive their compatible defaults, while newer fields
    preserve placement and encoded-format information. The reconstructed descriptor is validated
    as a whole before callers can trust its dimensions or payload size; decoding does not read
    image data.

    @param in Read-only source bytes in the format required by this operation.
    @param bytes Number of bytes to process.
    @param metadata Output media descriptor populated by this operation.

    @return true - The descriptor was decoded and validated.
    @return false - The descriptor is truncated, unsupported or contains invalid fields.
*/
bool
MediaMetadataCodec::DecodeMediaDescriptor (
    const uint8_t* in,
    size_t bytes,
    MediaMetadata& metadata
    )
{
    MediaMetadata decoded;
    uint16_t w;
    uint16_t h;
    uint64_t payload;

    if (!in || bytes != HeaderBytes || std::memcmp(in, "H75M", MEDIA_MAGIC_BYTES) ||
        (in[MEDIA_VERSION_OFFSET] != Version && in[MEDIA_VERSION_OFFSET] != JpegVersion &&
         in[MEDIA_VERSION_OFFSET] != JpegQualityVersion && in[MEDIA_VERSION_OFFSET] != PlacementVersion) ||
        (in[MEDIA_VERSION_OFFSET] != JpegQualityVersion && in[MEDIA_VERSION_OFFSET] != PlacementVersion &&
         (in[MEDIA_FLAGS_OFFSET] & ~PingPongFlag))) {
        return false;
    }

    // Keep the caller's metadata unchanged until every descriptor field passes validation.
    decoded = {};
    decoded.type = MediaType(in[MEDIA_TYPE_OFFSET]);
    decoded.pixelFormat = PixelFormat(in[MEDIA_FORMAT_OFFSET]);
    decoded.pingPong = (in[MEDIA_FLAGS_OFFSET] & PingPongFlag) != 0;
    if ((in[MEDIA_VERSION_OFFSET] == JpegQualityVersion || in[MEDIA_VERSION_OFFSET] == PlacementVersion)) {
        decoded.jpegQuality = uint8_t(in[MEDIA_FLAGS_OFFSET] >> MEDIA_JPEG_QUALITY_SHIFT);
    } else {
        decoded.jpegQuality = 0;
    }
    decoded.width = ReadUnsigned16LittleEndian(in + MEDIA_WIDTH_OFFSET);
    decoded.height = ReadUnsigned16LittleEndian(in + MEDIA_HEIGHT_OFFSET);
    if (in[MEDIA_VERSION_OFFSET] == PlacementVersion) {
        w = decoded.width;
        h = decoded.height;
        if (w & MEDIA_PLACEMENT_RESERVED_MASK_U) {
            return false;
        }
        decoded.width = (w & MEDIA_PLACEMENT_WIDTH_MASK_U) + 1;
        decoded.offsetX = w >> MEDIA_PLACEMENT_X_SHIFT;
        decoded.height = (h & MEDIA_PLACEMENT_HEIGHT_MASK_U) + 1;
        decoded.offsetY = h >> MEDIA_PLACEMENT_Y_SHIFT;
    }
    decoded.frameCount = ReadUnsigned32LittleEndian(in + MEDIA_FRAME_COUNT_OFFSET);
    decoded.fpsNumerator = ReadUnsigned32LittleEndian(in + MEDIA_FPS_NUMERATOR_OFFSET);
    decoded.fpsDenominator = ReadUnsigned32LittleEndian(in + MEDIA_FPS_DENOMINATOR_OFFSET);
    if (in[MEDIA_VERSION_OFFSET] != PlacementVersion &&
        (decoded.pixelFormat == PixelFormat::JPEG) !=
            (in[MEDIA_VERSION_OFFSET] == JpegVersion || in[MEDIA_VERSION_OFFSET] == JpegQualityVersion)) {
        return false;
    }
    if (decoded.pixelFormat == PixelFormat::JPEG) {
        decoded.encodedBytes = ReadUnsigned64LittleEndian(in + MEDIA_PAYLOAD_BYTES_OFFSET);
    }
    if (!decoded.PayloadBytes(payload) ||
        ReadUnsigned64LittleEndian(in + MEDIA_PAYLOAD_BYTES_OFFSET) != payload) {
        return false;
    }
    metadata = decoded; // Publish only a fully validated descriptor.
    return true;
} // DecodeMediaDescriptor
