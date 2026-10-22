#include "media/include/mediaTransfer.hpp"
#include "config/include/storageConfig.hpp"

/**
    Validates a full-panel RGB888 video against frame-rate and FAT32 limits.

    Validates the descriptor before admitting any filesystem transaction. The file
    size includes its checked header and one CRC record per raw frame.

    @param metadata Logical video description to validate.
    @param width Required logical frame width.
    @param height Required logical frame height.

    @return true - The complete video fits the supported dimensions, rate and file size.
    @return false - The descriptor is invalid or exceeds a supported bound.
*/
bool
MediaTransfer::IsSupportedVideo (
    const MediaMetadata& metadata,
    uint16_t width,
    uint16_t height
    )
{
    uint64_t payload;

    if (!metadata.PayloadBytes(payload) || metadata.type != MediaType::Video ||
        metadata.pixelFormat != PixelFormat::RGB888 || metadata.width != width || metadata.height != height ||
        metadata.fpsNumerator < metadata.fpsDenominator ||
        uint64_t(metadata.fpsNumerator) > uint64_t(MaximumFps) * metadata.fpsDenominator) {
        return false;
    }
    return FileHeaderBytes + payload + uint64_t(FRAME_CHECKSUM_BYTES) * metadata.frameCount <= MaximumFileBytes;
} // IsSupportedVideo
