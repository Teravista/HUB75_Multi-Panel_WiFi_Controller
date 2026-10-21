#include "sd/include/storageCrc32.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include "config/defines.h"

#define BYTE_MASK_U 255u
#define CRC32_REFLECTED_POLYNOMIAL_U 0xedb88320u
#define CRC32_SLICE_COUNT 4
#define CRC32_TABLE_ENTRIES 256
#define CRC32_WORD_BYTES 4
#define CRC32_BYTE_TWO_INDEX 2
#define CRC32_BYTE_THREE_INDEX 3
#define CRC32_BYTE_TWO_SHIFT 16
#define CRC32_BYTE_THREE_SHIFT 24
#define CRC32_WORD_LOW_SLICE 3
#define CRC32_WORD_MIDDLE_SLICE 2

namespace StorageCrc32 {
/**
    Builds lookup tables for the storage CRC32 implementation.

    Reflected IEEE CRC-32, same polynomial/state/final inversion as existing files. Slicing by
    four breaks the byte-at-a-time dependency; all 4 KiB live in flash.

    @return tables - Four lookup tables for slicing-by-four IEEE CRC32 calculation.
*/
constexpr auto
BuildCrc32LookupTables (
    void
    )
{
    std::array<std::array<uint32_t, CRC32_TABLE_ENTRIES>, CRC32_SLICE_COUNT> tables{};
    size_t index{};
    uint32_t crc{};
    unsigned bit{};
    uint32_t polynomial{};
    size_t slice{};
    size_t innerIndex{};
    uint32_t previous{};

    tables = {};
    for (index = 0; index < CRC32_TABLE_ENTRIES; ++index) {
        crc = uint32_t(index);
        for (bit = 0; bit < BITS_PER_BYTE; ++bit) {
            polynomial = {};
            if ((crc & 1)) {
                polynomial = CRC32_REFLECTED_POLYNOMIAL_U;
            } else {
                polynomial = 0u;
            }
            crc = (crc >> 1) ^ polynomial;
        }
        tables[0][index] = crc;
    }

    for (slice = 1; slice < tables.size(); ++slice) {
        for (innerIndex = 0; innerIndex < CRC32_TABLE_ENTRIES; ++innerIndex) {
            previous = tables[slice - 1][innerIndex];
            tables[slice][innerIndex] = tables[0][previous & BYTE_MASK_U] ^ (previous >> BITS_PER_BYTE);
        }
    }

    return tables;
} // BuildCrc32LookupTables

constexpr auto Tables = BuildCrc32LookupTables();

/**
    Updates a running CRC32 using the storage checksum lookup tables.

    Processes complete word-sized groups with the precomputed slicing tables, then handles
    remaining tail bytes with the bytewise CRC recurrence. The running state can be carried across
    arbitrarily split chunks without changing the checksum. Final inversion belongs to the caller
    at the end of the complete payload.

    @param crc Running CRC32 state before processing these bytes.
    @param bytes Read-only input byte buffer.
    @param length Number of bytes to process.

    @return value - Updated running CRC32 state; apply the protocol final inversion when
                    completing the checksum.
*/
uint32_t
UpdateCrc32Checksum (
    uint32_t crc,
    const uint8_t* bytes,
    size_t length
    )
{
    uint32_t word;

    while (length >= CRC32_WORD_BYTES) {
        // Explicit byte loads permit unaligned headers/chunks without aliasing UB.
        word = crc ^ uint32_t(bytes[0]) ^ (uint32_t(bytes[1]) << BITS_PER_BYTE) ^
                              (uint32_t(bytes[CRC32_BYTE_TWO_INDEX]) << CRC32_BYTE_TWO_SHIFT) ^
                              (uint32_t(bytes[CRC32_BYTE_THREE_INDEX]) << CRC32_BYTE_THREE_SHIFT);
        crc = Tables[CRC32_WORD_LOW_SLICE][word & BYTE_MASK_U] ^
              Tables[CRC32_WORD_MIDDLE_SLICE][(word >> BITS_PER_BYTE) & BYTE_MASK_U] ^
              Tables[1][(word >> CRC32_BYTE_TWO_SHIFT) & BYTE_MASK_U] ^
              Tables[0][word >> CRC32_BYTE_THREE_SHIFT];
        bytes += CRC32_WORD_BYTES;
        length -= CRC32_WORD_BYTES;
    }

    while (length--) {
        crc = Tables[0][(crc ^ *bytes++) & BYTE_MASK_U] ^ (crc >> BITS_PER_BYTE);
    }

    return crc; // Caller supplies the seed and performs final inversion.
} // UpdateCrc32Checksum

} // namespace StorageCrc32
