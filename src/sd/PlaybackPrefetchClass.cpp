#include "sd/include/PlaybackPrefetchClass.hpp"

/**
    Clears cached playback blocks, cursor, direction, and read statistics.
*/
void
PlaybackPrefetch::Reset (
    void
    )
{
    size_t blockIndex;
    PlaybackPrefetch::Block* block = nullptr;

    position_ = stamp_ = 0;
    backwards_ = false;
    stats_ = {};
    for (blockIndex = 0; blockIndex < sizeof(blocks_) / sizeof(blocks_[0]); ++blockIndex) {
        block = &(blocks_[blockIndex]);
        block->valid = false;
    }
} // Reset

/**
    Returns the logical cursor within the playback file.

    @return value - Absolute byte position of the logical playback cursor.
*/
uint64_t
PlaybackPrefetch::Tell (
    void
    ) const
{
    return position_;
} // Tell

/**
    Updates the logical playback cursor without discarding reusable cache blocks.

    @param position Absolute byte position within the playback file.
*/
void
PlaybackPrefetch::Seek (
    uint64_t position
    )
{
    position_ = position;
} // Seek

/**
    Selects the direction used for speculative playback reads.

    @param value True selects backward read-ahead; false selects forward read-ahead.
*/
void
PlaybackPrefetch::SetBackwards (
    bool value
    )
{
    backwards_ = value;
} // SetBackwards

/**
    Returns accumulated playback cache hit and load statistics.

    @return profile - Copy of accumulated operation counts and timing measurements.
*/
PlaybackPrefetch::Stats
PlaybackPrefetch::GetStats (
    void
    ) const
{
    return stats_;
} // GetStats

/**
    Copies requested playback bytes through the bounded block cache.

    Backend(offset, destination, count) returns true only after an exact read.

    @param destination Writable destination buffer; the caller must provide sufficient storage.
    @param requested Maximum number of bytes requested by the caller.
    @param count Output number of entries or bytes produced by this operation.
    @param size Total file size in bytes.
    @param backend File-read callback used to fill a missing prefetch block.

    @return true - The requested available bytes were copied; count gives the number read.
    @return false - A required backend block read failed.
*/
bool
PlaybackPrefetch::Read (
    uint8_t* destination,
    size_t requested,
    size_t& count,
    uint64_t size,
    Backend backend
    )
{
    PlaybackPrefetch::Block* block = nullptr;
    bool hit;
    size_t at;
    size_t part;

    count = 0;
    while (count < requested && position_ < size) {
        block = Find(position_);
        hit = block != nullptr;
        if (!block) {
            block = Load(position_ / BlockBytes * BlockBytes, size, backend, false);
            if (!block) {
                return false;
            }
        }
        at = size_t(position_ - block->offset);
        part = std::min(requested - count, block->count - at);
        if (destination) {
            std::memcpy(destination + count, block->bytes + at, part);
        }
        if (hit) {
            stats_.hitBytes += part;
        }
        position_ += part;
        count += part;
        block->stamp = ++stamp_;
    }

    return true;
} // Read

/**
    Prefetches the next likely block using the current playback direction.

    Chooses the neighboring block in the current playback direction while preserving the block
    containing the consumer cursor. Already cached ranges and end-of-file boundaries require no
    backend call. A missing range is loaded synchronously, so speculative read-ahead can still
    take SD time even though its bytes are not yet requested by the consumer.

    @param size Total file size in bytes.
    @param backend File-read callback used to fill a missing prefetch block.

    @return true - The next block is cached, no read-ahead is needed, or EOF was reached.
    @return false - Speculative loading of the next block failed.
*/
bool
PlaybackPrefetch::Ahead (
    uint64_t size,
    Backend backend
    )
{
    uint64_t cursor;
    uint64_t block;
    uint64_t next;

    // Preserve the block containing the consumer cursor. No reads past EOF.
    if (backwards_ && position_) {
        cursor = position_ - 1;
    } else {
        cursor = position_;
    }
    if (cursor >= size) {
        return true;
    }
    block = cursor / BlockBytes * BlockBytes;
    if (backwards_ && block < BlockBytes) {
        return true;
    }
    if (backwards_) {
        next = block - BlockBytes;
    } else {
        next = block + BlockBytes;
    }
    if (next >= size || Find(next)) {
        return true;
    }
    return Load(next, size, backend, true) != nullptr;
} // Ahead

/**
    Finds a cached block containing the requested file offset.

    @param offset Byte offset within the source image or file.

    @return pointer - Cached block containing the requested byte offset.
    @return nullptr - No cached block covers the requested offset.
*/
PlaybackPrefetch::Block*
PlaybackPrefetch::Find (
    uint64_t offset
    )
{
    size_t blockIndex;
    PlaybackPrefetch::Block* block = nullptr;

    for (blockIndex = 0; blockIndex < sizeof(blocks_) / sizeof(blocks_[0]); ++blockIndex) {
        block = &(blocks_[blockIndex]);
        if (block->valid && offset >= block->offset && offset - block->offset < block->count) {
            return &(*block);
        }
    }
    return nullptr;
} // Find

/**
    Fills a cache block through the supplied file read callback.

    Chooses an unused or least-recently-used block, protecting the active cursor's block during
    speculative reads. The selected block is invalidated before the synchronous backend fills it,
    then published as valid only after a complete successful read. Failure increments statistics
    without exposing partial data as a cache hit; successful loads update range, recency and read-
    ahead counters.

    @param offset Byte offset within the source image or file.
    @param size Total file size in bytes.
    @param backend File-read callback used to fill a missing prefetch block.
    @param ahead Whether the block is loaded speculatively ahead of the current read.

    @return pointer - Cached or newly loaded block for the requested offset.
    @return nullptr - The offset is at EOF or the backend could not load the block.
*/
PlaybackPrefetch::Block*
PlaybackPrefetch::Load (
    uint64_t offset,
    uint64_t size,
    Backend backend,
    bool ahead
    )
{
    Block* target = nullptr;
    const Block* protectedBlock = nullptr;
    uint32_t conditionalValue21;
    size_t blockIndex;
    PlaybackPrefetch::Block* block = nullptr;
    size_t bytes;

    target = nullptr;
    if (ahead) {
        conditionalValue21 = {};
        if (backwards_ && position_) {
            conditionalValue21 = position_ - 1;
        } else {
            conditionalValue21 = position_;
        }
        protectedBlock = Find(conditionalValue21);
    } else {
        protectedBlock = nullptr;
    }
    for (blockIndex = 0; blockIndex < sizeof(blocks_) / sizeof(blocks_[0]); ++blockIndex) {
        block = &(blocks_[blockIndex]);
        if (&(*block) == protectedBlock) {
            continue;
        }
        if (!target || !block->valid || (target->valid && block->stamp < target->stamp)) {
            target = &(*block);
        }
        if (!block->valid) {
            break;
        }
    }

    if (!target || offset >= size) {
        return nullptr;
    }
    target->valid = false;
    bytes = size_t(std::min(uint64_t(BlockBytes), size - offset));
    if (!backend.read(backend.context, offset, target->bytes, bytes)) {
        ++stats_.failures;
        return nullptr;
    }
    target->offset = offset;
    target->count = bytes;
    target->stamp = ++stamp_;
    target->valid = true;
    stats_.readBytes += bytes;
    ++stats_.loads;
    if (ahead) {
        ++stats_.aheadLoads;
    }
    return target;
} // Load
