#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "config/defines.h"


/**
    Caches nearby bytes of the active playback file to reduce repeated SD reads.

    Two sector-aligned blocks share a fixed 64 KiB allocation. They retain reusable file ranges
    while a logical playback cursor supports sequential, reverse and random frame access; playback
    direction guides optional ahead reads. The backend is a borrowed synchronous callback, so a
    cache miss can wait for SD I/O in the calling foreground operation. Core 0 alone owns the
    cache and backend. Decoder workers receive copied bytes through their input pipe, and the
    cache is reset when a playback file or storage session changes.
*/
class PlaybackPrefetch {
public:
    static constexpr size_t Bytes = 64 * 1024;
    static constexpr size_t BlockBytes = Bytes / 2;

    struct Stats {
        uint64_t hitBytes = 0;
        uint64_t readBytes = 0;
        uint32_t loads = 0;
        uint32_t aheadLoads = 0;
        uint32_t failures = 0;
    };

    // Reads are synchronous and borrow the caller's context without allocating.
    struct Backend {
        void* context;
        bool (*read)(void* context, uint64_t offset, uint8_t* bytes, size_t count);
    };

    // Clears cached playback blocks, cursor, direction, and read statistics.
    void Reset(void);

    // Returns the logical cursor within the playback file.
    uint64_t Tell(void) const;

    // Updates the logical playback cursor without discarding reusable cache blocks.
    void Seek(uint64_t position);

    // Selects the direction used for speculative playback reads.
    void SetBackwards(bool value);

    // Returns accumulated playback cache hit and load statistics.
    Stats GetStats(void) const;

    // Copies requested playback bytes through the bounded block cache.
    bool Read(uint8_t* destination, size_t requested, size_t& count, uint64_t size, Backend backend);

    // Prefetches the next likely block using the current playback direction.
    bool Ahead(uint64_t size, Backend backend);

private:
    struct Block {
        alignas(DMA_BUFFER_ALIGNMENT) uint8_t bytes[BlockBytes];
        uint64_t offset = 0;
        uint64_t stamp = 0;
        size_t count = 0;
        bool valid = false;
    } blocks_[PLAYBACK_PREFETCH_BLOCK_COUNT];

    uint64_t position_ = 0;
    uint64_t stamp_ = 0;
    bool backwards_ = false;
    Stats stats_;

    // Finds a cached block containing the requested file offset.
    Block* Find(uint64_t offset);

    // Fills a cache block through the supplied file read callback.
    Block* Load(uint64_t offset, uint64_t size, Backend backend, bool ahead);

};
