#pragma once
#include <cstddef>
#include <cstdint>

namespace StorageCrc32 {
// Updates a running CRC32 using the storage checksum lookup tables.
uint32_t UpdateCrc32Checksum(uint32_t crc, const uint8_t* bytes, size_t length);

} // namespace StorageCrc32
