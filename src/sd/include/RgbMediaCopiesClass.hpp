#pragma once

#include <include/preparedFrame.hpp>
#include <include/mediaMetadata.hpp>
#include <ff.h>

#define RGB_COPIES_DESCRIPTOR_BYTES 192u
#define RGB_COPIES_HEADER_RESERVE_BYTES 256u
#define RGB_COPIES_VERSION 1u
#define RGB_COPIES_PREVIEW_VERSION 2u
#define RGB_COPIES_PREVIEW_FLAGS 0u
#define RGB_COPIES_MAXIMUM_FILE_BYTES UINT32_MAX
#define RGB_COPIES_CORRECTION_FLAGS 3u
#define RGB_COPIES_ORIGINAL_COUNT 1u
#define RGB_COPIES_PREPARED_COUNT 3u
#define RGB_COPIES_PREVIEW_COUNT 2u
#define RGB_COPIES_PREVIEW_WIDTH 32u
#define RGB_COPIES_PREVIEW_HEIGHT 64u
#define RGB_COPIES_PREVIEW_BYTES (RGB_COPIES_PREVIEW_WIDTH * RGB_COPIES_PREVIEW_HEIGHT * 2u)
#define RGB_COPIES_PREVIEW_STEP 4u
#define RGB_COPIES_PREVIEW_CENTER 2u
#define RGB_COPIES_SOURCE_BLOCK_WIDTH 32u
#define RGB_COPIES_BLOCK_BYTES 16384u
#define RGB_COPIES_STEP_BUDGET_US 8000u
#define RGB_COPIES_SECTOR_BYTES 512u
#define RGB_COPIES_CHECKSUM_BYTES 4u
#define RGB_COPIES_SIGNATURE_FIELDS 17u
#define RGB_COPIES_PROGRESS_SCALE 1000u
#define RGB_COPIES_PREPARE_PROGRESS 800u
#define RGB_COPIES_MAGIC_BYTES 4u
#define RGB_COPIES_PATH_BYTES 40u
#define RGB_COPIES_JPEG_WORK_OFFSET (12u * 1024u)
#define RGB_COPIES_JPEG_SKIP_BYTES 256u
#define RGB_COPIES_JPEG_END_MARKER 0xffd9u
#define RGB_COPIES_VERSION_OFFSET 4u
#define RGB_COPIES_FLAGS_OFFSET 5u
#define RGB_COPIES_PIXEL_BYTES_OFFSET 6u
#define RGB_COPIES_REGION_COUNT_OFFSET 7u
#define RGB_COPIES_SIGNATURE_OFFSET 8u
#define RGB_COPIES_SOURCE_CRC_OFFSET 12u
#define RGB_COPIES_PREVIEW_CRC_OFFSET 16u
#define RGB_COPIES_FRAME_BYTES_OFFSET 20u
#define RGB_COPIES_PREVIEW_OFFSET_FIELD 24u
#define RGB_COPIES_DISPLAY_OFFSET_FIELD 32u
#define RGB_COPIES_REGIONS_OFFSET 40u
#define RGB_COPIES_REGION_BYTES 8u
#define RGB_COPIES_REGION_Y_OFFSET 2u
#define RGB_COPIES_REGION_WIDTH_OFFSET 4u
#define RGB_COPIES_REGION_HEIGHT_OFFSET 6u
#define RGB_COPIES_CRC_OFFSET (RGB_COPIES_DESCRIPTOR_BYTES - RGB_COPIES_CHECKSUM_BYTES)

/**
    Prepares stored previews for all media and native display copies for raw RGB.

    MemoryManager lends the unpublished file and its existing 24 KiB scratch buffer.
    This helper transposes bounded panel rectangles, preserving the original uploaded
    bytes, and appends the two derived representations inside the same atomic library
    item. It owns only a small preview and progress records, never a complete frame.
    The caller retains filesystem ownership across each bounded progress operation.
    JPEG decodes only its first frame for a preview and retains original-only playback.
    Legacy originals remain readable without derived sections.
*/
class RgbMediaCopies {
public:
    // Releases borrowed preparation state without touching the filesystem.
    void Reset(void);

    // Reports whether derived-copy preparation has started for this transaction.
    bool HasStarted(void) const;

    // Returns preparation and readback completion in thousandths.
    uint32_t GetProgressPermille(void) const;

    // Checks whether the original and its format-specific copies fit one file.
    static bool FitsFileSize(const MediaMetadata& metadata, size_t headerBytes);

    // Starts format-specific derived copies after the original payload is verified.
    bool Begin(FIL& file, bool& fileOpen, const char* path, const MediaMetadata& metadata, size_t headerBytes, uint32_t checksum, uint8_t* workspace, size_t capacity, int& result);

    // Advances bounded preparation or readback verification before publication.
    bool Advance(bool& complete, int& result);

    // Writes the checked layout descriptor into a library publication header.
    void EncodeDescriptor(uint8_t* header, size_t headerBytes) const;

    // Validates original-only or extended library size and layout metadata.
    static bool DecodeDescriptor(const uint8_t* header, size_t headerBytes, const MediaMetadata& metadata, uint32_t checksum, uint64_t originalEnd, uint64_t fileBytes, PreparedFrameLayout& layout);

    // Reads the stored small preview and checks its independent checksum.
    static bool ReadPreview(FIL& file, const PreparedFrameLayout& layout, uint8_t* pixels, size_t capacity, int& result);

private:
    enum class Phase { Idle, Preparing, CheckingPreview, CheckingDisplay, Ready };

    struct SourceRegion {
        uint16_t x = 0;
        uint16_t y = 0;
        uint16_t width = 0;
        uint16_t height = 0;
    };

    // Identifies the current geometry, channel order and CPU pixel representation.
    static uint32_t LayoutSignature(void);

    // Builds bounded logical source rectangles and their packed physical destinations.
    bool BuildRegions(void);

    // Supplies bounded encoded JPEG bytes for first-frame preview decoding.
    static size_t ReadJpegInput(void* context, uint8_t* bytes, size_t count);

    // Samples decoded RGB565 fragments directly into the small logical preview.
    static bool ConsumeJpegPixels(void* context, const uint8_t* bytes, size_t count, size_t offset);

    // Decodes only the first JPEG frame to prepare a stored logical preview.
    bool PrepareJpegPreview(int& result);

    // Appends the native frame checksum and finalizes completed preview generation.
    bool FinishFrame(int& result);

    // Writes and reopens the preview section before independent readback.
    bool FinishPreview(int& result);

    // Converts source rows into one bounded native panel rectangle.
    bool PrepareRows(int& result);

    // Appends one prepared rectangle and advances the frame checksum cursor.
    bool FinishRegion(int& result);

    // Reads back a bounded span of the complete generated display section.
    bool VerifyDisplayStep(int& result);

    Phase phase_ = Phase::Idle;
    FIL* file_ = nullptr;
    bool* fileOpen_ = nullptr;
    char path_[RGB_COPIES_PATH_BYTES]{};
    uint8_t* workspace_ = nullptr;
    size_t capacity_ = 0;
    MediaMetadata metadata_;
    PreparedFrameLayout layout_;
    SourceRegion source_[PREPARED_FRAME_MAX_REGIONS]{};
    uint8_t preview_[RGB_COPIES_PREVIEW_BYTES]{};
    uint64_t originalOffset_ = 0;
    uint64_t displayWritten_ = 0;
    uint64_t displayVerified_ = 0;
    uint32_t sourceChecksum_ = 0;
    uint32_t frame_ = 0;
    uint32_t frameChecksum_ = 0;
    uint32_t displayChecksum_ = 0;
    uint32_t verifyChecksum_ = 0;
    uint8_t region_ = 0;
    uint16_t row_ = 0;
    uint16_t jpegLastWord_ = 0;
    uint64_t jpegCursor_ = 0;
    size_t jpegRemaining_ = 0;
    size_t jpegDecodedBytes_ = 0;
    uint32_t jpegChecksum_ = 0;
    int jpegResult_ = FR_OK;
};
