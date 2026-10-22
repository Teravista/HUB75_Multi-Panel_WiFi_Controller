#pragma once
#include "media/include/mediaMetadata.hpp"

namespace MediaTransfer {
constexpr uint16_t Port = 5002;
constexpr size_t ChunkBytes = 1536;
constexpr size_t BulkChunkBytes = 6144;
constexpr size_t RequestBytes = MediaMetadataCodec::HeaderBytes + sizeof(uint32_t);
constexpr size_t FileHeaderBytes = 64;
constexpr uint64_t MaximumFileBytes = 0xffffffffu;
constexpr uint32_t MaximumFps = 60;
constexpr uint32_t ReceiveIdleMs = 30000;
constexpr char BulkRequestMagic[] = "H75V";

// Validates a full-panel RGB888 video against frame-rate and FAT32 limits.
bool IsSupportedVideo(const MediaMetadata& metadata, uint16_t width, uint16_t height);

} // namespace MediaTransfer
