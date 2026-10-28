#pragma once
#include "media/include/mediaMetadata.hpp"

namespace MediaTransfer {
constexpr uint16_t Port = 5002;
constexpr size_t ChunkBytes = 1536;
constexpr size_t BulkChunkBytes = 6144;
constexpr size_t RequestBytes = MediaMetadataCodec::HeaderBytes + sizeof(uint32_t);
constexpr size_t FileHeaderBytes = 64;
constexpr size_t JpegIndexBytes = 12;
constexpr size_t JpegRecordBytes = 8;
constexpr size_t MaximumJpegBytes = 128 * 1024;
constexpr uint64_t MaximumFileBytes = 0xffffffffu;
constexpr uint32_t MaximumFps = 60;
constexpr uint32_t ReceiveIdleMs = 15000;
constexpr char BulkRequestMagic[] = "H75V";
constexpr char NamedBulkRequestMagic[] = "H75N";
constexpr char NamedRequestMagic[] = "H75n";

// Validates a placed raw or indexed JPEG video against frame-rate and FAT32 limits.
bool IsSupportedVideo(const MediaMetadata& metadata, uint16_t width, uint16_t height);

} // namespace MediaTransfer
