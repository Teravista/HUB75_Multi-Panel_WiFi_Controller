#pragma once

#include <cstddef>
#include <cstdint>
#include "config/include/storageConfig.hpp"
#include "config/defines.h"

/**
    Provides the initial SD image-storage boundary for the panel controller.

    Foreground callers mount storage and save or stream one named logical RGB888
    image through this class. It owns the filesystem handle, one shared 6 KiB read
    buffer and hardware lifecycle without allocating another complete framebuffer.
    Exact file/header checks and a payload CRC protect readback; delivered fragments
    remain provisional until the complete read succeeds. A non-waiting guard rejects
    reentrant access, and a temporary-file self-check exercises save/read/cleanup. Alternating retained
    photo slots preserve the previous valid generation during a replacement save.
*/
class MemoryManager {
public:
    static constexpr size_t IoChunkBytes = 6 * 1024;
    // Pixel fragments are provisional until the complete read succeeds.
    using ImageSink = bool (*)(void* context, const uint8_t* pixels, size_t bytes, size_t offset);

    using ImageSource = bool (*)(void* context, uint8_t* pixels, size_t bytes, size_t offset);

    // Saves a new retained photo while keeping the previous valid generation.
    bool SaveLastImage(ImageSource source, void* context);

    // Restores the newest verified retained photo through a chunk sink.
    bool ReadLastImage(ImageSink sink, void* context);

    // Mounts the SD card for image storage.
    bool Initialize(void);

    // Unmounts storage and releases its SPI and DMA resources.
    bool Shutdown(void);

    // Reports whether mounted SD storage is available for use.
    bool IsReady(void) const;

    // Returns the most recent storage operation error.
    const char* GetLastError(void) const;

    // Writes a new named photo without replacing an existing file.
    bool SaveImage(const char* name, const uint8_t* pixels, size_t bytes);

    // Streams a named RGB888 photo and validates its dimensions and checksum.
    bool ReadImage(const char* name, ImageSink sink, void* context);

    // Saves, rereads, and removes an unused temporary test image.
    bool RunReadWriteCheck(const uint8_t* pixels, size_t bytes);

private:
    static constexpr size_t HeaderBytes = PHOTO_HEADER_BYTES;
    static constexpr size_t ChunkBytes = IoChunkBytes;
    static constexpr size_t MaximumNameLength = STORAGE_MAXIMUM_NAME_LENGTH;
    static_assert(ChunkBytes % SD_SECTOR_BYTES == 0 && ChunkBytes % IMAGE_BYTES_PER_PIXEL == 0,
                  "Storage chunks must contain complete sectors and RGB pixels");

    // Private static state keeps FatFs objects and the read buffer off the stack.
    struct StorageState;
    static StorageState storage_;

    /**
        Guards the class-owned filesystem and scratch buffer for one operation.

        Acquisition reports a busy state immediately. Scope exit releases only
        a successful claim, preserving the foreground callback lifetime boundary.
    */
    struct StorageGuard {
        bool acquired;

        // Attempts to claim the shared storage state without waiting.
        StorageGuard(void);

        // Releases the storage-operation guard when its scope ends.
        ~StorageGuard(void);

    };

    struct CompareContext {
        const uint8_t* expected;
        size_t compared = 0;
        bool mismatch = false;
    };

    // Validates a root filename and builds its SD filesystem path.
    static bool BuildValidatedImagePath(const char* name, char* path);

    // Updates the running CRC32 checksum with another byte span.
    static uint32_t UpdateImageCrc32(uint32_t crc, const uint8_t* bytes, size_t length);

    // Writes a 32-bit unsigned value in little-endian byte order.
    static void WriteUnsigned32LittleEndian(uint8_t* p, uint32_t value);

    // Reads a 32-bit unsigned value in little-endian byte order.
    static uint32_t ReadUnsigned32LittleEndian(const uint8_t* p);

    // Builds a stored-photo header with its payload checksum.
    static void BuildPhotoStorageHeader(uint8_t* header, uint32_t checksum);

    // Compares readback pixels against the source used by the storage self-check.
    static bool CompareSavedImageBytes(void* context, const uint8_t* bytes, size_t count, size_t offset);

    enum class SlotState { Missing, Invalid, Valid, IoError };

    struct SavedPhotoSlot {
        SlotState state = SlotState::Missing;
        uint32_t generation = 0;
        int result = 0;
        const char* error = "invalid saved photo";
    };

    // Builds a retained RGB888 photo header containing its generation and CRC.
    static void BuildSavedPhotoHeader(uint8_t* header, uint32_t generation, uint32_t checksum);

    // Reads and validates one retained photo, optionally delivering its pixels.
    static SavedPhotoSlot ReadSavedPhotoSlot(unsigned index, ImageSink sink = nullptr, void* context = nullptr);

    // Selects the newest valid generation with wrap-safe ordering.
    static int FindNewestSavedPhoto(const SavedPhotoSlot* slots);

    // Streams a new retained photo into the selected alternate slot.
    static bool WriteSavedPhotoSlot(unsigned index, uint32_t generation, ImageSource source, void* context, int& result);

    bool ready_ = false;
    const char* error_ = "storage not initialized";

    // Records a storage error and invalidates readiness after failed SD I/O.
    bool ReportStorageFailure(const char* message, int filesystemResult = 0);

};
