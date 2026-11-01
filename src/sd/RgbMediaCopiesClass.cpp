#include <include/RgbMediaCopiesClass.hpp>

#include <include/PanelRendererClass.hpp>
#include <include/cpuFrame.hpp>
#include <include/mediaTransfer.hpp>
#include <jpeg/include/jpegFrameDecoder.hpp>
#include <include/storageCrc32.hpp>
#include <config/defines.h>
#include <pico/time.h>

#include <algorithm>
#include <cstring>
#include <limits>

static_assert(RGB_COPIES_REGIONS_OFFSET + PREPARED_FRAME_MAX_REGIONS * RGB_COPIES_REGION_BYTES <= RGB_COPIES_CRC_OFFSET,
    "Prepared rectangles must fit the reserved descriptor");
static_assert(LOGICAL_IMAGE_WIDTH == RGB_COPIES_PREVIEW_WIDTH * RGB_COPIES_PREVIEW_STEP &&
    LOGICAL_IMAGE_HEIGHT == RGB_COPIES_PREVIEW_HEIGHT * RGB_COPIES_PREVIEW_STEP,
    "Prepared preview sampling must cover the logical canvas exactly");
static_assert(LOGICAL_PANEL_WIDTH % RGB_COPIES_SOURCE_BLOCK_WIDTH == 0 &&
    LOGICAL_PANEL_COUNT_X * LOGICAL_PANEL_COUNT_Y *
        (LOGICAL_PANEL_WIDTH / RGB_COPIES_SOURCE_BLOCK_WIDTH) <= PREPARED_FRAME_MAX_REGIONS,
    "Prepared descriptor must fit every panel block");
static_assert(RGB_COPIES_SOURCE_BLOCK_WIDTH * LOGICAL_PANEL_HEIGHT * CpuFrame::PixelBytes <= RGB_COPIES_BLOCK_BYTES,
    "Prepared panel blocks must fit the borrowed storage workspace");

/**
    Releases borrowed preparation state without touching the filesystem.
*/
void
RgbMediaCopies::Reset (
    void
    )
{
    phase_ = Phase::Idle;
    file_ = nullptr;
    fileOpen_ = nullptr;
    workspace_ = nullptr;
    layout_ = {};
} // Reset

/**
    Reports whether derived-copy preparation has started for this transaction.

    @return true - This transaction has active or completed derived copies.
    @return false - No preparation transaction is retained.
*/
bool
RgbMediaCopies::HasStarted (
    void
    ) const
{
    return phase_ != Phase::Idle;
} // HasStarted

/**
    Identifies the current geometry, channel order and CPU pixel representation.

    The serialized fingerprint prevents cached pixels from being reused after wiring,
    mapping or CPU-format changes. Logical originals and previews remain usable.

    @return value - CRC32 signature of the native layout contract.
*/
uint32_t
RgbMediaCopies::LayoutSignature (
    void
    )
{
    uint32_t fields[RGB_COPIES_SIGNATURE_FIELDS] = {
        RGB_COPIES_VERSION, CpuFrame::PixelBytes,
        LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT,
        LOGICAL_PANEL_WIDTH, LOGICAL_PANEL_HEIGHT,
        PHYSICAL_PANEL_WIDTH, PHYSICAL_PANEL_HEIGHT,
        FIRST_FLIPPED_PANEL_INDEX,
        IMAGE_RED_BYTE_OFFSET, IMAGE_GREEN_BYTE_OFFSET, IMAGE_BLUE_BYTE_OFFSET,
        RGB_COPIES_SOURCE_BLOCK_WIDTH, LOGICAL_PANEL_COUNT_X, LOGICAL_PANEL_COUNT_Y,
        MATRIX_PANEL_WIDTH, MATRIX_PANEL_HEIGHT};
    uint32_t crc;
    uint8_t bytes[RGB_COPIES_CHECKSUM_BYTES];
    size_t index;
    size_t row;
    size_t column;

    crc = STORAGE_CRC32_INITIAL_U;
    for (index = 0; index < RGB_COPIES_SIGNATURE_FIELDS; ++index) {
        MediaMetadataCodec::WriteUnsigned32LittleEndian(bytes, fields[index]);
        crc = StorageCrc32::UpdateCrc32Checksum(crc, bytes, sizeof(bytes));
    }

    // Hash the configured chain map without assuming a fixed panel-grid size.
    for (row = 0; row < LOGICAL_PANEL_COUNT_Y; ++row) {
        for (column = 0; column < LOGICAL_PANEL_COUNT_X; ++column) {
            MediaMetadataCodec::WriteUnsigned32LittleEndian(bytes, PANEL_INDEX_BY_LOGICAL_PANEL[row][column]);
            crc = StorageCrc32::UpdateCrc32Checksum(crc, bytes, sizeof(bytes));
        }
    }

    return ~crc;
} // LayoutSignature

/**
    Builds bounded logical source rectangles and their packed physical destinations.

    Each logical panel is split into narrow column blocks. Rotation turns each into
    a native rectangle small enough for the existing scratch buffer, including when
    CPU pixels use four bytes. Only the image's occupied region is stored.

    @return true - Every source pixel belongs to a bounded destination rectangle.
    @return false - Placement, geometry or workspace limits are unsupported.
*/
bool
RgbMediaCopies::BuildRegions (
    void
    )
{
    PanelRenderer mapper;
    uint16_t left;
    uint16_t top;
    unsigned panelX;
    unsigned panelY;
    unsigned block;
    unsigned x0;
    unsigned y0;
    unsigned x1;
    unsigned y1;
    int firstX;
    int firstY;
    int lastX;
    int lastY;
    PreparedFrameRegion* destination = nullptr;

    left = metadata_.Left(LOGICAL_IMAGE_WIDTH);
    top = metadata_.Top(LOGICAL_IMAGE_HEIGHT);
    layout_.frameBytes = 0;
    layout_.regionCount = 0;

    for (panelY = 0; panelY < LOGICAL_PANEL_COUNT_Y; ++panelY) {
        for (panelX = 0; panelX < LOGICAL_PANEL_COUNT_X; ++panelX) {
            for (block = 0; block < LOGICAL_PANEL_WIDTH; block += RGB_COPIES_SOURCE_BLOCK_WIDTH) {
                x0 = std::max(unsigned(left), panelX * LOGICAL_PANEL_WIDTH + block);
                x1 = std::min(unsigned(left + metadata_.width),
                    panelX * LOGICAL_PANEL_WIDTH + block + RGB_COPIES_SOURCE_BLOCK_WIDTH);
                y0 = std::max(unsigned(top), panelY * LOGICAL_PANEL_HEIGHT);
                y1 = std::min(unsigned(top + metadata_.height), (panelY + 1) * LOGICAL_PANEL_HEIGHT);
                if (x0 >= x1 || y0 >= y1) {
                    continue;
                }
                if (layout_.regionCount >= PREPARED_FRAME_MAX_REGIONS) {
                    return false;
                }

                mapper.ConvertLogicalToPhysicalCoordinates(x0, y0, firstX, firstY);
                mapper.ConvertLogicalToPhysicalCoordinates(x1 - 1, y1 - 1, lastX, lastY);
                destination = &layout_.regions[layout_.regionCount];
                *destination = {uint16_t(std::min(firstX, lastX)), uint16_t(std::min(firstY, lastY)),
                    uint16_t(std::abs(lastX - firstX) + 1), uint16_t(std::abs(lastY - firstY) + 1)};
                source_[layout_.regionCount] = {uint16_t(x0 - left), uint16_t(y0 - top),
                    uint16_t(x1 - x0), uint16_t(y1 - y0)};
                if (size_t(destination->width) * destination->height * CpuFrame::PixelBytes > RGB_COPIES_BLOCK_BYTES) {
                    return false;
                }
                layout_.frameBytes += uint32_t(destination->width) * destination->height * CpuFrame::PixelBytes;
                ++layout_.regionCount;
            }
        }
    }

    return layout_.regionCount &&
        layout_.frameBytes == uint32_t(metadata_.width) * metadata_.height * CpuFrame::PixelBytes;
} // BuildRegions

/**
    Checks whether the original and its format-specific copies fit one file.

    FAT32 limits one item to the filesystem's file-size range, including native
    frames, frame checksums, alignment and preview. Reject oversized library uploads
    before creating the staging file rather than after receiving their payload.

    @param metadata Validated original media descriptor.
    @param headerBytes Selected library header length.

    @return true - All required representations fit the file-size range.
    @return false - The media format is unsupported or its complete container is too large.
*/
bool
RgbMediaCopies::FitsFileSize (
    const MediaMetadata& metadata,
    size_t headerBytes
    )
{
    uint64_t payload;
    uint64_t originalEnd;
    uint64_t previewOffset;
    uint64_t nativeBytes;

    if ((metadata.pixelFormat != PixelFormat::RGB888 && metadata.pixelFormat != PixelFormat::RGB565 &&
         metadata.pixelFormat != PixelFormat::JPEG) ||
        !metadata.FitsPanel(LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT) || !metadata.PayloadBytes(payload)) {
        return false;
    }
    originalEnd = headerBytes + payload;
    if (metadata.pixelFormat != PixelFormat::JPEG) {
        originalEnd += uint64_t(RGB_COPIES_CHECKSUM_BYTES) * metadata.frameCount;
    }
    previewOffset = (originalEnd + RGB_COPIES_SECTOR_BYTES - 1) / RGB_COPIES_SECTOR_BYTES * RGB_COPIES_SECTOR_BYTES;
    nativeBytes = 0;
    if (metadata.pixelFormat != PixelFormat::JPEG) {
        nativeBytes = (uint64_t(metadata.width) * metadata.height * CpuFrame::PixelBytes +
            RGB_COPIES_CHECKSUM_BYTES) * metadata.frameCount;
    }
    return previewOffset + RGB_COPIES_PREVIEW_BYTES + nativeBytes <= std::min(uint64_t(std::numeric_limits<FSIZE_t>::max()), uint64_t(RGB_COPIES_MAXIMUM_FILE_BYTES));
} // FitsFileSize

/**
    Starts format-specific derived copies after the original payload is verified.

    Raw records or indexed JPEG payload bytes stay at their existing offsets. A sector-
    aligned preview is appended for all formats; raw RGB also gets packed native
    frames. JPEG originals remain the sole playback representation. The borrowed
    workspace must remain reserved until completion or Reset; no full-frame allocation
    is needed. The caller publishes the file header only after readback succeeds.

    @param file Borrowed read/write staging file containing the verified original.
    @param fileOpen Owner flag updated when closing and reopening for readback.
    @param path Stable staging filename used to reopen actual storage for verification.
    @param metadata Original logical media descriptor.
    @param headerBytes Byte offset of the original frame records.
    @param checksum Verified original transfer checksum.
    @param workspace Borrowed existing storage scratch buffer.
    @param capacity Available scratch bytes.
    @param result Receives a FatFs error or invalid-layout result.

    @return true - Bounded preparation can advance.
    @return false - The input, storage size or initial write is invalid.
*/
bool
RgbMediaCopies::Begin (
    FIL& file,
    bool& fileOpen,
    const char* path,
    const MediaMetadata& metadata,
    size_t headerBytes,
    uint32_t checksum,
    uint8_t* workspace,
    size_t capacity,
    int& result
    )
{
    uint64_t originalEnd;
    uint64_t preparedEnd;
    UINT written;
    UINT padding;

    Reset();
    result = FR_INVALID_PARAMETER;
    if ((metadata.pixelFormat != PixelFormat::RGB888 && metadata.pixelFormat != PixelFormat::RGB565 &&
         metadata.pixelFormat != PixelFormat::JPEG) ||
        !FitsFileSize(metadata, headerBytes) || !fileOpen || !path || std::strlen(path) >= sizeof(path_) ||
        !workspace || capacity < RGB_COPIES_BLOCK_BYTES + RGB_COPIES_SOURCE_BLOCK_WIDTH * RGB888_PIXEL_BYTES ||
        (metadata.pixelFormat == PixelFormat::JPEG && capacity < RGB_COPIES_JPEG_WORK_OFFSET * 2u) ||
        headerBytes < RGB_COPIES_HEADER_RESERVE_BYTES + MEDIA_FILE_HEADER_BYTES) {
        return false;
    }

    metadata_ = metadata;
    layout_ = {};
    if (metadata.pixelFormat != PixelFormat::JPEG) {
        layout_.pixelBytes = CpuFrame::PixelBytes;
        layout_.signature = LayoutSignature();
        if (!BuildRegions()) {
            return false;
        }
    }
    metadata.PayloadBytes(originalEnd);
    originalEnd += headerBytes;
    if (metadata.pixelFormat != PixelFormat::JPEG) {
        originalEnd += uint64_t(RGB_COPIES_CHECKSUM_BYTES) * metadata.frameCount;
    }
    layout_.previewOffset = (originalEnd + RGB_COPIES_SECTOR_BYTES - 1) / RGB_COPIES_SECTOR_BYTES * RGB_COPIES_SECTOR_BYTES;
    preparedEnd = layout_.previewOffset + RGB_COPIES_PREVIEW_BYTES;
    if (metadata.pixelFormat != PixelFormat::JPEG) {
        layout_.displayOffset = preparedEnd;
        preparedEnd += uint64_t(layout_.frameBytes + RGB_COPIES_CHECKSUM_BYTES) * metadata.frameCount;
    }
    if (originalEnd != f_size(&file) || preparedEnd > std::min(uint64_t(std::numeric_limits<FSIZE_t>::max()), uint64_t(RGB_COPIES_MAXIMUM_FILE_BYTES))) {
        return false;
    }

    // Clear alignment padding explicitly; preview and display bytes are filled later.
    padding = UINT(layout_.previewOffset - originalEnd);
    std::memset(workspace, 0, RGB_COPIES_SECTOR_BYTES);
    result = f_lseek(&file, FSIZE_t(originalEnd));
    if (result == FR_OK && padding) {
        result = f_write(&file, workspace, padding, &written);
        if (result == FR_OK && written != padding) {
            result = FR_DISK_ERR;
        }
    }
    if (result != FR_OK) {
        return false;
    }

    file_ = &file;
    fileOpen_ = &fileOpen;
    std::memcpy(path_, path, std::strlen(path) + 1);
    workspace_ = workspace;
    capacity_ = capacity;
    originalOffset_ = headerBytes;
    sourceChecksum_ = checksum;
    frame_ = 0;
    region_ = 0;
    row_ = 0;
    displayWritten_ = 0;
    displayVerified_ = 0;
    frameChecksum_ = STORAGE_CRC32_INITIAL_U;
    displayChecksum_ = STORAGE_CRC32_INITIAL_U;
    verifyChecksum_ = STORAGE_CRC32_INITIAL_U;
    std::memset(preview_, 0, sizeof(preview_));
    phase_ = Phase::Preparing;
    return true;
} // Begin

/**
    Converts source rows into one bounded native panel rectangle.

    Source pixels are read from the unchanged file, expanded if necessary, corrected
    and transposed into a scratch rectangle. Mapping is calculated once per source
    row, then a signed stride advances across its pixels. The first frame also fills
    the logical preview before channel correction. A time budget yields between rows.

    @param result Receives the FatFs status of any failed or short read.

    @return true - The current rows were processed successfully.
    @return false - The source read failed or was short.
*/
bool
RgbMediaCopies::PrepareRows (
    int& result
    )
{
    PanelRenderer mapper;
    SourceRegion* source = nullptr;
    PreparedFrameRegion* destination = nullptr;
    uint8_t* input = nullptr;
    size_t sourcePixelBytes;
    size_t rowBytes;
    size_t pixel;
    size_t previewOffset;
    uint64_t offset;
    uint64_t started;
    UINT count;
    int x;
    int y;
    int nextX;
    int nextY;
    ptrdiff_t output;
    ptrdiff_t stride;
    unsigned logicalX;
    unsigned logicalY;
    uint32_t rgb;
    uint8_t channels[RGB888_PIXEL_BYTES];
    uint16_t packed;

    source = &source_[region_];
    destination = &layout_.regions[region_];
    input = workspace_ + RGB_COPIES_BLOCK_BYTES;
    sourcePixelBytes = metadata_.PixelBytes();
    rowBytes = source->width * sourcePixelBytes;
    started = to_us_since_boot(get_absolute_time());

    do {
        offset = uint64_t(source->y + row_) * metadata_.width + source->x;
        result = f_lseek(file_, FSIZE_t(originalOffset_ +
            uint64_t(frame_) * (metadata_.FrameBytes() + RGB_COPIES_CHECKSUM_BYTES) + offset * sourcePixelBytes));
        if (result == FR_OK) {
            result = f_read(file_, input, UINT(rowBytes), &count);
            if (result == FR_OK && count != rowBytes) {
                result = FR_DISK_ERR;
            }
        }
        if (result != FR_OK) {
            return false;
        }

        logicalX = source->x + metadata_.Left(LOGICAL_IMAGE_WIDTH);
        logicalY = source->y + row_ + metadata_.Top(LOGICAL_IMAGE_HEIGHT);
        mapper.ConvertLogicalToPhysicalCoordinates(logicalX, logicalY, x, y);
        output = (ptrdiff_t(y - destination->y) * destination->width + x - destination->x) * CpuFrame::PixelBytes;
        stride = 0;
        if (source->width > 1) {
            mapper.ConvertLogicalToPhysicalCoordinates(logicalX + 1, logicalY, nextX, nextY);
            stride = (ptrdiff_t(nextY - y) * destination->width + nextX - x) * CpuFrame::PixelBytes;
        }

        for (pixel = 0; pixel < source->width; ++pixel) {
            if (metadata_.pixelFormat == PixelFormat::RGB565) {
                packed = MediaMetadataCodec::ReadUnsigned16LittleEndian(input + pixel * sourcePixelBytes);
                rgb = CpuFrame::ExpandRgb565ToRgb888(packed);
                channels[0] = uint8_t(rgb >> RGB888_RED_SHIFT);
                channels[1] = uint8_t(rgb >> RGB888_GREEN_SHIFT);
                channels[RGB_PIXEL_BLUE_INDEX] = uint8_t(rgb);
            } else {
                std::memcpy(channels, input + pixel * sourcePixelBytes, sizeof(channels));
            }

            CpuFrame::StoreRgbPixel(workspace_ + output, channels[IMAGE_RED_BYTE_OFFSET],
                channels[IMAGE_GREEN_BYTE_OFFSET], channels[IMAGE_BLUE_BYTE_OFFSET]);
            output += stride;

            // The preview uses logical colors and placement, never hardware channel order.
            if (frame_ == 0 && (logicalX + pixel) % RGB_COPIES_PREVIEW_STEP == RGB_COPIES_PREVIEW_CENTER &&
                logicalY % RGB_COPIES_PREVIEW_STEP == RGB_COPIES_PREVIEW_CENTER) {
                previewOffset = (size_t(logicalY / RGB_COPIES_PREVIEW_STEP) * RGB_COPIES_PREVIEW_WIDTH +
                    (logicalX + pixel) / RGB_COPIES_PREVIEW_STEP) * RGB565_PIXEL_BYTES;
                packed = CpuFrame::PackRgb565Pixel(channels[0], channels[1], channels[RGB_PIXEL_BLUE_INDEX]);
                MediaMetadataCodec::WriteUnsigned16LittleEndian(preview_ + previewOffset, packed);
            }
        }

        ++row_;
    } while (row_ < source->height && to_us_since_boot(get_absolute_time()) - started < RGB_COPIES_STEP_BUDGET_US);

    return true;
} // PrepareRows

/**
    Appends one prepared rectangle and advances the frame checksum cursor.

    Rectangles are stored sequentially to avoid per-pixel writes. Each completed
    native frame gets a separate CRC, and a running section CRC is retained for
    complete readback before the library item can be published.

    @param result Receives the FatFs status of a failed write or flush.

    @return true - The rectangle and any frame boundary were written successfully.
    @return false - A write, seek or sync failed or was short.
*/
bool
RgbMediaCopies::FinishRegion (
    int& result
    )
{
    UINT count;
    size_t bytes;

    bytes = size_t(layout_.regions[region_].width) * layout_.regions[region_].height * CpuFrame::PixelBytes;
    result = f_lseek(file_, FSIZE_t(layout_.displayOffset + displayWritten_));
    if (result == FR_OK) {
        result = f_write(file_, workspace_, UINT(bytes), &count);
        if (result == FR_OK && count != bytes) {
            result = FR_DISK_ERR;
        }
    }
    if (result != FR_OK) {
        return false;
    }

    frameChecksum_ = StorageCrc32::UpdateCrc32Checksum(frameChecksum_, workspace_, bytes);
    displayChecksum_ = StorageCrc32::UpdateCrc32Checksum(displayChecksum_, workspace_, bytes);
    displayWritten_ += bytes;
    row_ = 0;
    ++region_;
    if (region_ < layout_.regionCount) {
        return true;
    }

    return FinishFrame(result);
} // FinishRegion

/**
    Appends the native frame checksum and finalizes completed preview generation.

    Each frame is checked independently. After the final one, write and sync the
    first-frame preview, reopen storage, then enter complete derived-section readback.

    @param result Receives a failed write, sync, close or reopen result.

    @return true - The frame boundary and any final preview were written.
    @return false - A storage operation failed or was short.
*/
bool
RgbMediaCopies::FinishFrame (
    int& result
    )
{
    UINT count;
    uint8_t checksum[RGB_COPIES_CHECKSUM_BYTES];

    MediaMetadataCodec::WriteUnsigned32LittleEndian(checksum, ~frameChecksum_);
    result = f_write(file_, checksum, sizeof(checksum), &count);
    if (result != FR_OK || count != sizeof(checksum)) {
        if (result == FR_OK) {
            result = FR_DISK_ERR;
        }
        return false;
    }
    displayChecksum_ = StorageCrc32::UpdateCrc32Checksum(displayChecksum_, checksum, sizeof(checksum));
    displayWritten_ += sizeof(checksum);
    frameChecksum_ = STORAGE_CRC32_INITIAL_U;
    region_ = 0;
    ++frame_;
    if (frame_ < metadata_.frameCount) {
        return true;
    }

    return FinishPreview(result);
} // FinishFrame

/**
    Writes and reopens the preview section before independent readback.

    The preview is the only appended JPEG representation. Raw RGB reaches this
    step after writing its native frames. Sync and reopen prevent verification
    from succeeding merely because the output remains in a dirty write cache.

    @param result Receives a failed write, sync, close or reopen result.

    @return true - The preview is stored and ready for independent verification.
    @return false - Writing or reopening failed.
*/
bool
RgbMediaCopies::FinishPreview (
    int& result
    )
{
    UINT count;

    layout_.previewChecksum = ~StorageCrc32::UpdateCrc32Checksum(STORAGE_CRC32_INITIAL_U, preview_, sizeof(preview_));
    result = f_lseek(file_, FSIZE_t(layout_.previewOffset));
    if (result == FR_OK) {
        result = f_write(file_, preview_, sizeof(preview_), &count);
        if (result == FR_OK && count != sizeof(preview_)) {
            result = FR_DISK_ERR;
        }
    }
    if (result == FR_OK) {
        result = f_sync(file_);
    }
    if (result != FR_OK) {
        return false;
    }

    // Reopen after sync so FatFs cannot satisfy verification from dirty or
    // already cached write sectors. The owner flag makes failed-close cleanup safe.
    result = f_close(file_);
    *fileOpen_ = false;
    if (result == FR_OK) {
        result = f_open(file_, path_, FA_READ | FA_WRITE);
        *fileOpen_ = result == FR_OK;
    }
    if (result != FR_OK) {
        return false;
    }
    phase_ = Phase::CheckingPreview;
    return true;
} // FinishPreview

/**
    Reads back a bounded span of the complete generated display section.

    The readback includes all native frame CRC records and compares against the
    running checksum computed from the actual output buffers. A mismatch leaves
    the staging header invalid, retaining the preceding published media.

    @param result Receives the FatFs error or failed verification result.

    @return true - This readback span passed, possibly completing preparation.
    @return false - The section was unreadable, short or had an incorrect checksum.
*/
bool
RgbMediaCopies::VerifyDisplayStep (
    int& result
    )
{
    UINT bytes;
    UINT count;

    bytes = UINT(std::min(uint64_t(capacity_), displayWritten_ - displayVerified_));
    result = f_lseek(file_, FSIZE_t(layout_.displayOffset + displayVerified_));
    if (result == FR_OK) {
        result = f_read(file_, workspace_, bytes, &count);
        if (result == FR_OK && count != bytes) {
            result = FR_DISK_ERR;
        }
    }
    if (result != FR_OK) {
        return false;
    }
    verifyChecksum_ = StorageCrc32::UpdateCrc32Checksum(verifyChecksum_, workspace_, bytes);
    displayVerified_ += bytes;
    if (displayVerified_ == displayWritten_) {
        if (verifyChecksum_ != displayChecksum_) {
            result = FR_INVALID_OBJECT;
            return false;
        }
        layout_.present = true;
        layout_.displayPresent = true;
        layout_.compatible = true;
        phase_ = Phase::Ready;
    }

    return true;
} // VerifyDisplayStep

/**
    Advances bounded preparation or readback verification before publication.

    Preparation retains the partial native rectangle in borrowed scratch across
    calls. Other storage work must remain excluded until completion. Preview and
    display readback follow preparation; no success is published provisionally.

    @param complete Set true only after both generated sections pass readback.
    @param result Receives a storage error or invalid transaction result.

    @return true - Progress succeeded; inspect complete for the final result.
    @return false - Preparation or verification failed.
*/
bool
RgbMediaCopies::Advance (
    bool& complete,
    int& result
    )
{
    complete = false;
    result = FR_OK;
    if (phase_ == Phase::Preparing && metadata_.pixelFormat == PixelFormat::JPEG) {
        if (!PrepareJpegPreview(result)) {
            return false;
        }
    } else if (phase_ == Phase::Preparing) {
        if (!PrepareRows(result)) {
            return false;
        }
        if (row_ == source_[region_].height && !FinishRegion(result)) {
            return false;
        }
    } else if (phase_ == Phase::CheckingPreview) {
        if (!ReadPreview(*file_, layout_, workspace_, capacity_, result)) {
            return false;
        }
        if (metadata_.pixelFormat == PixelFormat::JPEG) {
            layout_.present = true;
            phase_ = Phase::Ready;
        } else {
            phase_ = Phase::CheckingDisplay;
        }
    } else if (phase_ == Phase::CheckingDisplay) {
        if (!VerifyDisplayStep(result)) {
            return false;
        }
    } else if (phase_ != Phase::Ready) {
        result = FR_INVALID_OBJECT;
        return false;
    }

    complete = phase_ == Phase::Ready;
    return true;
} // Advance

/**
    Writes the checked layout descriptor into a library publication header.

    The reserved tail records native regions, source identity, preview CRC and
    absolute section offsets. Header and descriptor CRCs protect the on-disk
    representation; original transfer metadata remains unchanged.

    @param header Writable full library header, before its final CRC is generated.
    @param headerBytes Size of that library header in bytes.
*/
void
RgbMediaCopies::EncodeDescriptor (
    uint8_t* header,
    size_t headerBytes
    ) const
{
    uint8_t* descriptor = nullptr;
    uint8_t* region = nullptr;
    size_t index;

    if (phase_ != Phase::Ready || headerBytes < RGB_COPIES_HEADER_RESERVE_BYTES) {
        return;
    }
    descriptor = header + headerBytes - RGB_COPIES_HEADER_RESERVE_BYTES;
    std::memset(descriptor, 0, RGB_COPIES_DESCRIPTOR_BYTES);
    std::memcpy(descriptor, "H75C", RGB_COPIES_MAGIC_BYTES);
    descriptor[RGB_COPIES_VERSION_OFFSET] = RGB_COPIES_VERSION;
    descriptor[RGB_COPIES_FLAGS_OFFSET] = RGB_COPIES_CORRECTION_FLAGS;
    if (metadata_.pixelFormat == PixelFormat::JPEG) {
        descriptor[RGB_COPIES_VERSION_OFFSET] = RGB_COPIES_PREVIEW_VERSION;
        descriptor[RGB_COPIES_FLAGS_OFFSET] = RGB_COPIES_PREVIEW_FLAGS;
    }
    descriptor[RGB_COPIES_PIXEL_BYTES_OFFSET] = layout_.pixelBytes;
    descriptor[RGB_COPIES_REGION_COUNT_OFFSET] = layout_.regionCount;
    MediaMetadataCodec::WriteUnsigned32LittleEndian(descriptor + RGB_COPIES_SIGNATURE_OFFSET, layout_.signature);
    MediaMetadataCodec::WriteUnsigned32LittleEndian(descriptor + RGB_COPIES_SOURCE_CRC_OFFSET, sourceChecksum_);
    MediaMetadataCodec::WriteUnsigned32LittleEndian(descriptor + RGB_COPIES_PREVIEW_CRC_OFFSET, layout_.previewChecksum);
    MediaMetadataCodec::WriteUnsigned32LittleEndian(descriptor + RGB_COPIES_FRAME_BYTES_OFFSET, layout_.frameBytes);
    MediaMetadataCodec::WriteUnsigned64LittleEndian(descriptor + RGB_COPIES_PREVIEW_OFFSET_FIELD, layout_.previewOffset);
    MediaMetadataCodec::WriteUnsigned64LittleEndian(descriptor + RGB_COPIES_DISPLAY_OFFSET_FIELD, layout_.displayOffset);

    for (index = 0; index < layout_.regionCount; ++index) {
        region = descriptor + RGB_COPIES_REGIONS_OFFSET + index * RGB_COPIES_REGION_BYTES;
        MediaMetadataCodec::WriteUnsigned16LittleEndian(region, layout_.regions[index].x);
        MediaMetadataCodec::WriteUnsigned16LittleEndian(region + RGB_COPIES_REGION_Y_OFFSET, layout_.regions[index].y);
        MediaMetadataCodec::WriteUnsigned16LittleEndian(region + RGB_COPIES_REGION_WIDTH_OFFSET, layout_.regions[index].width);
        MediaMetadataCodec::WriteUnsigned16LittleEndian(region + RGB_COPIES_REGION_HEIGHT_OFFSET, layout_.regions[index].height);
    }
    MediaMetadataCodec::WriteUnsigned32LittleEndian(descriptor + RGB_COPIES_CRC_OFFSET,
        ~StorageCrc32::UpdateCrc32Checksum(STORAGE_CRC32_INITIAL_U, descriptor, RGB_COPIES_CRC_OFFSET));
} // EncodeDescriptor

/**
    Validates original-only or extended library size and layout metadata.

    Original-only files retain the legacy path. Extended files must have a valid
    descriptor, exact section bounds and nonoverlapping in-panel rectangles covering
    every original pixel once. A different current wiring signature disables native
    playback while preserving logical readback and the stored preview.

    @param header Validated full library header.
    @param headerBytes Size of the library header.
    @param metadata Original logical media descriptor.
    @param checksum Original payload checksum from that header.
    @param originalEnd Exact end of the unchanged original frame records.
    @param fileBytes Actual file size.
    @param layout Receives the checked optional native layout.

    @return true - File size and any derived representation are valid.
    @return false - The extension, bounds or region layout is invalid.
*/
bool
RgbMediaCopies::DecodeDescriptor (
    const uint8_t* header,
    size_t headerBytes,
    const MediaMetadata& metadata,
    uint32_t checksum,
    uint64_t originalEnd,
    uint64_t fileBytes,
    PreparedFrameLayout& layout
    )
{
    const uint8_t* descriptor = nullptr;
    const uint8_t* region = nullptr;
    PreparedFrameRegion* rectangle = nullptr;
    size_t index;
    size_t previous;
    uint64_t pixelCount;
    uint64_t expectedPreview;
    uint64_t expectedEnd;

    layout = {};
    if (fileBytes == originalEnd) {
        // A legacy file has no extension marker. A marked file truncated at the
        // original's end must not masquerade as a successful original-only save.
        return headerBytes < RGB_COPIES_HEADER_RESERVE_BYTES ||
            std::memcmp(header + headerBytes - RGB_COPIES_HEADER_RESERVE_BYTES,
                "H75C", RGB_COPIES_MAGIC_BYTES) != 0;
    }
    if (headerBytes < RGB_COPIES_HEADER_RESERVE_BYTES) {
        return false;
    }
    descriptor = header + headerBytes - RGB_COPIES_HEADER_RESERVE_BYTES;
    if (std::memcmp(descriptor, "H75C", RGB_COPIES_MAGIC_BYTES) ||
        MediaMetadataCodec::ReadUnsigned32LittleEndian(descriptor + RGB_COPIES_SOURCE_CRC_OFFSET) != checksum ||
        MediaMetadataCodec::ReadUnsigned32LittleEndian(descriptor + RGB_COPIES_CRC_OFFSET) !=
            ~StorageCrc32::UpdateCrc32Checksum(STORAGE_CRC32_INITIAL_U, descriptor, RGB_COPIES_CRC_OFFSET)) {
        return false;
    }

    // Version 2 is a JPEG preview-only extension, independent of CPU/wiring.
    if (descriptor[RGB_COPIES_VERSION_OFFSET] == RGB_COPIES_PREVIEW_VERSION) {
        expectedPreview = (originalEnd + RGB_COPIES_SECTOR_BYTES - 1) / RGB_COPIES_SECTOR_BYTES * RGB_COPIES_SECTOR_BYTES;
        if (metadata.pixelFormat != PixelFormat::JPEG || descriptor[RGB_COPIES_FLAGS_OFFSET] != RGB_COPIES_PREVIEW_FLAGS ||
            descriptor[RGB_COPIES_PIXEL_BYTES_OFFSET] || descriptor[RGB_COPIES_REGION_COUNT_OFFSET] ||
            MediaMetadataCodec::ReadUnsigned32LittleEndian(descriptor + RGB_COPIES_SIGNATURE_OFFSET) ||
            MediaMetadataCodec::ReadUnsigned32LittleEndian(descriptor + RGB_COPIES_FRAME_BYTES_OFFSET) ||
            MediaMetadataCodec::ReadUnsigned64LittleEndian(descriptor + RGB_COPIES_DISPLAY_OFFSET_FIELD) ||
            MediaMetadataCodec::ReadUnsigned64LittleEndian(descriptor + RGB_COPIES_PREVIEW_OFFSET_FIELD) != expectedPreview ||
            fileBytes != expectedPreview + RGB_COPIES_PREVIEW_BYTES) {
            return false;
        }
        for (index = RGB_COPIES_REGIONS_OFFSET; index < RGB_COPIES_CRC_OFFSET; ++index) {
            if (descriptor[index]) {
                return false;
            }
        }
        layout.previewOffset = expectedPreview;
        layout.previewChecksum = MediaMetadataCodec::ReadUnsigned32LittleEndian(descriptor + RGB_COPIES_PREVIEW_CRC_OFFSET);
        layout.present = true;
        return true;
    }
    if (descriptor[RGB_COPIES_VERSION_OFFSET] != RGB_COPIES_VERSION ||
        descriptor[RGB_COPIES_FLAGS_OFFSET] != RGB_COPIES_CORRECTION_FLAGS) {
        return false;
    }

    layout.pixelBytes = descriptor[RGB_COPIES_PIXEL_BYTES_OFFSET];
    layout.regionCount = descriptor[RGB_COPIES_REGION_COUNT_OFFSET];
    layout.frameBytes = MediaMetadataCodec::ReadUnsigned32LittleEndian(descriptor + RGB_COPIES_FRAME_BYTES_OFFSET);
    layout.signature = MediaMetadataCodec::ReadUnsigned32LittleEndian(descriptor + RGB_COPIES_SIGNATURE_OFFSET);
    layout.previewChecksum = MediaMetadataCodec::ReadUnsigned32LittleEndian(descriptor + RGB_COPIES_PREVIEW_CRC_OFFSET);
    layout.previewOffset = MediaMetadataCodec::ReadUnsigned64LittleEndian(descriptor + RGB_COPIES_PREVIEW_OFFSET_FIELD);
    layout.displayOffset = MediaMetadataCodec::ReadUnsigned64LittleEndian(descriptor + RGB_COPIES_DISPLAY_OFFSET_FIELD);
    expectedPreview = (originalEnd + RGB_COPIES_SECTOR_BYTES - 1) / RGB_COPIES_SECTOR_BYTES * RGB_COPIES_SECTOR_BYTES;
    expectedEnd = layout.displayOffset + uint64_t(layout.frameBytes + RGB_COPIES_CHECKSUM_BYTES) * metadata.frameCount;
    if ((layout.pixelBytes != RGB565_PIXEL_BYTES && layout.pixelBytes != sizeof(uint32_t)) ||
        !layout.regionCount || layout.regionCount > PREPARED_FRAME_MAX_REGIONS ||
        layout.frameBytes != uint32_t(metadata.width) * metadata.height * layout.pixelBytes ||
        layout.previewOffset != expectedPreview || layout.displayOffset != expectedPreview + RGB_COPIES_PREVIEW_BYTES ||
        expectedEnd != fileBytes) {
        return false;
    }

    pixelCount = 0;
    for (index = 0; index < layout.regionCount; ++index) {
        region = descriptor + RGB_COPIES_REGIONS_OFFSET + index * RGB_COPIES_REGION_BYTES;
        rectangle = &layout.regions[index];
        *rectangle = {MediaMetadataCodec::ReadUnsigned16LittleEndian(region),
            MediaMetadataCodec::ReadUnsigned16LittleEndian(region + RGB_COPIES_REGION_Y_OFFSET),
            MediaMetadataCodec::ReadUnsigned16LittleEndian(region + RGB_COPIES_REGION_WIDTH_OFFSET),
            MediaMetadataCodec::ReadUnsigned16LittleEndian(region + RGB_COPIES_REGION_HEIGHT_OFFSET)};
        if (!rectangle->width || !rectangle->height ||
            uint32_t(rectangle->x) + rectangle->width > MATRIX_PANEL_WIDTH ||
            uint32_t(rectangle->y) + rectangle->height > MATRIX_PANEL_HEIGHT) {
            return false;
        }
        for (previous = 0; previous < index; ++previous) {
            if (rectangle->x < uint32_t(layout.regions[previous].x) + layout.regions[previous].width &&
                layout.regions[previous].x < uint32_t(rectangle->x) + rectangle->width &&
                rectangle->y < uint32_t(layout.regions[previous].y) + layout.regions[previous].height &&
                layout.regions[previous].y < uint32_t(rectangle->y) + rectangle->height) {
                return false;
            }
        }
        pixelCount += uint32_t(rectangle->width) * rectangle->height;
    }
    if (pixelCount * layout.pixelBytes != layout.frameBytes) {
        return false;
    }

    layout.present = true;
    layout.displayPresent = true;
    // Earlier JPEG native copies remain readable for cleanup and preview access,
    // but JPEG playback always decodes the unchanged original.
    layout.compatible = metadata.pixelFormat != PixelFormat::JPEG &&
        layout.signature == LayoutSignature() && layout.pixelBytes == CpuFrame::PixelBytes;
    return true;
} // DecodeDescriptor

/**
    Reads the stored small preview and checks its independent checksum.

    The logical RGB565 preview is ready for the existing API's base64 response.
    Reading it neither decodes a frame nor changes active playback state.

    @param file Borrowed file containing the checked layout descriptor.
    @param layout Checked preview location and checksum.
    @param pixels Writable destination for the preview pixels.
    @param capacity Destination capacity in bytes.
    @param result Receives the storage or verification error.

    @return true - Exactly one small preview was read and verified.
    @return false - Capacity, I/O, length or checksum validation failed.
*/
bool
RgbMediaCopies::ReadPreview (
    FIL& file,
    const PreparedFrameLayout& layout,
    uint8_t* pixels,
    size_t capacity,
    int& result
    )
{
    UINT count;

    result = FR_INVALID_PARAMETER;
    if (!pixels || capacity < RGB_COPIES_PREVIEW_BYTES) {
        return false;
    }

    result = f_lseek(&file, FSIZE_t(layout.previewOffset));
    if (result == FR_OK) {
        result = f_read(&file, pixels, RGB_COPIES_PREVIEW_BYTES, &count);
        if (result == FR_OK && count != RGB_COPIES_PREVIEW_BYTES) {
            result = FR_DISK_ERR;
        }
    }
    if (result != FR_OK) {
        return false;
    }
    if (~StorageCrc32::UpdateCrc32Checksum(STORAGE_CRC32_INITIAL_U, pixels, RGB_COPIES_PREVIEW_BYTES) != layout.previewChecksum) {
        result = FR_INVALID_OBJECT;
        return false;
    }

    return true;
} // ReadPreview

/**
    Returns preparation and readback completion in thousandths.

    Includes the partially transposed source rectangle without reading or writing
    storage. Preparation occupies the first portion; final readback occupies the
    remainder. This snapshot permits status drawing between bounded save steps.

    @return value - Bounded progress from zero to one thousand.
*/
uint32_t
RgbMediaCopies::GetProgressPermille (
    void
    ) const
{
    uint64_t prepared;
    uint64_t total;

    if (phase_ == Phase::Idle) {
        return 0;
    }
    if (phase_ == Phase::Ready) {
        return RGB_COPIES_PROGRESS_SCALE;
    }
    if (phase_ == Phase::Preparing) {
        total = uint64_t(layout_.frameBytes + RGB_COPIES_CHECKSUM_BYTES) * metadata_.frameCount;
        prepared = displayWritten_ + uint64_t(row_) * source_[region_].width * CpuFrame::PixelBytes;
        return uint32_t(prepared * RGB_COPIES_PREPARE_PROGRESS / total);
    }
    if (phase_ == Phase::CheckingPreview) {
        return RGB_COPIES_PREPARE_PROGRESS;
    }
    return RGB_COPIES_PREPARE_PROGRESS + uint32_t(displayVerified_ *
        (RGB_COPIES_PROGRESS_SCALE - RGB_COPIES_PREPARE_PROGRESS) / displayWritten_);
} // GetProgressPermille

/**
    Supplies bounded encoded JPEG bytes for first-frame preview decoding.

    Every read stays within the original frame record and updates its checksum.
    The small preview is retained separately and written only after decoding.
    Skipped marker data is read too, preserving the complete encoded-frame checksum.

    @param context Borrowed preparation owner under exclusive storage access.
    @param bytes Decoder input destination; null consumes data without retaining it.
    @param count Maximum encoded bytes requested by the decoder.

    @return value - Number of checked input bytes supplied, possibly zero on failure.
*/
size_t
RgbMediaCopies::ReadJpegInput (
    void* context,
    uint8_t* bytes,
    size_t count
    )
{
    RgbMediaCopies* copies = nullptr;
    uint8_t skipped[RGB_COPIES_JPEG_SKIP_BYTES];
    uint8_t* destination = nullptr;
    size_t remaining;
    size_t supplied;
    UINT requested;
    UINT read;
    size_t index;

    copies = static_cast<RgbMediaCopies*>(context);
    remaining = std::min(count, copies->jpegRemaining_);
    supplied = 0;
    while (remaining && copies->jpegResult_ == FR_OK) {
        destination = bytes;
        requested = UINT(remaining);
        if (bytes) {
            destination += supplied;
        } else {
            destination = skipped;
            requested = UINT(std::min(remaining, sizeof(skipped)));
        }
        copies->jpegResult_ = f_lseek(copies->file_, FSIZE_t(copies->jpegCursor_));
        if (copies->jpegResult_ == FR_OK) {
            copies->jpegResult_ = f_read(copies->file_, destination, requested, &read);
            if (copies->jpegResult_ == FR_OK && read != requested) {
                copies->jpegResult_ = FR_DISK_ERR;
            }
        }
        if (copies->jpegResult_ != FR_OK) {
            break;
        }
        copies->jpegChecksum_ = StorageCrc32::UpdateCrc32Checksum(copies->jpegChecksum_, destination, read);
        for (index = 0; index < read; ++index) {
            copies->jpegLastWord_ = uint16_t((copies->jpegLastWord_ << BITS_PER_BYTE) | destination[index]);
        }
        copies->jpegCursor_ += read;
        copies->jpegRemaining_ -= read;
        remaining -= read;
        supplied += read;
    }
    return supplied;
} // ReadJpegInput

/**
    Samples decoded RGB888 fragments directly into the small logical preview.

    The JPEG decoder supplies MCU row fragments with absolute logical offsets.
    Only preview sample centers are retained; placement leaves the other preview
    pixels black. No full decoded image or corrected playback pixels are stored.

    @param context Borrowed preparation owner under exclusive storage access.
    @param bytes Logical RGB888 fragment from the first JPEG frame.
    @param count Complete RGB888 bytes in the fragment.
    @param offset Byte position inside the decoded logical image.

    @return true - The fragment was validated and its preview samples retained.
    @return false - The fragment has invalid pixel alignment or bounds.
*/
bool
RgbMediaCopies::ConsumeJpegPixels (
    void* context,
    const uint8_t* bytes,
    size_t count,
    size_t offset
    )
{
    RgbMediaCopies* copies = nullptr;
    size_t pixel;
    size_t x;
    size_t y;
    size_t logicalX;
    size_t logicalY;
    size_t previewOffset;
    size_t input;
    uint16_t packed;

    copies = static_cast<RgbMediaCopies*>(context);
    if (!bytes || !count || count % RGB888_PIXEL_BYTES || offset % RGB888_PIXEL_BYTES ||
        offset + count > uint64_t(copies->metadata_.width) * copies->metadata_.height * RGB888_PIXEL_BYTES) {
        return false;
    }
    pixel = offset / RGB888_PIXEL_BYTES;
    x = pixel % copies->metadata_.width;
    y = pixel / copies->metadata_.width;
    if (x + count / RGB888_PIXEL_BYTES > copies->metadata_.width) {
        return false;
    }
    copies->jpegDecodedBytes_ += count;

    logicalY = y + copies->metadata_.Top(LOGICAL_IMAGE_HEIGHT);
    if (logicalY % RGB_COPIES_PREVIEW_STEP == RGB_COPIES_PREVIEW_CENTER) {
        for (input = 0; input < count; input += RGB888_PIXEL_BYTES) {
            logicalX = x + input / RGB888_PIXEL_BYTES + copies->metadata_.Left(LOGICAL_IMAGE_WIDTH);
            if (logicalX % RGB_COPIES_PREVIEW_STEP == RGB_COPIES_PREVIEW_CENTER) {
                previewOffset = (logicalY / RGB_COPIES_PREVIEW_STEP * RGB_COPIES_PREVIEW_WIDTH +
                    logicalX / RGB_COPIES_PREVIEW_STEP) * RGB565_PIXEL_BYTES;
                packed = CpuFrame::PackRgb565Pixel(bytes[input], bytes[input + 1], bytes[input + RGB_PIXEL_BLUE_INDEX]);
                MediaMetadataCodec::WriteUnsigned16LittleEndian(copies->preview_ + previewOffset, packed);
            }
        }
    }
    return true;
} // ConsumeJpegPixels

/**
    Decodes only the first JPEG frame to prepare a stored logical preview.

    Index, encoded CRC and end-marker checks precede preview publication. Pixel
    fragments are sampled directly; no display copy or decoded frame is retained.
    Other video frames already passed original-payload verification and need no
    additional decode for preview generation.

    @param result Receives an invalid original, decode or filesystem result.

    @return true - The first-frame preview was stored for readback.
    @return false - Input, decoding, preview writing or finalization failed.
*/
bool
RgbMediaCopies::PrepareJpegPreview (
    int& result
    )
{
    uint8_t entry[MediaTransfer::JpegIndexBytes];
    uint8_t record[MediaTransfer::JpegRecordBytes];
    uint32_t offset;
    uint32_t length;
    uint32_t checksum;
    UINT count;
    int decoderError;
    bool decoded;

    // Bind the index checksum to this frame number and bound it to the original
    // encoded payload; derived sections can never become decoder input.
    result = f_lseek(file_, FSIZE_t(originalOffset_ + uint64_t(frame_) * sizeof(entry)));
    if (result == FR_OK) {
        result = f_read(file_, entry, sizeof(entry), &count);
        if (result == FR_OK && count != sizeof(entry)) {
            result = FR_DISK_ERR;
        }
    }
    if (result != FR_OK) {
        return false;
    }
    offset = MediaMetadataCodec::ReadUnsigned32LittleEndian(entry);
    length = MediaMetadataCodec::ReadUnsigned32LittleEndian(entry + RGB_COPIES_CHECKSUM_BYTES);
    checksum = MediaMetadataCodec::ReadUnsigned32LittleEndian(entry + MediaTransfer::JpegRecordBytes);
    MediaMetadataCodec::WriteUnsigned32LittleEndian(entry + MediaTransfer::JpegRecordBytes, frame_);
    if (checksum != ~StorageCrc32::UpdateCrc32Checksum(STORAGE_CRC32_INITIAL_U, entry, sizeof(entry)) ||
        offset < uint64_t(metadata_.frameCount) * sizeof(entry) || !length || length > MediaTransfer::MaximumJpegBytes ||
        uint64_t(offset) + sizeof(record) + length > metadata_.encodedBytes) {
        result = FR_INVALID_OBJECT;
        return false;
    }
    // Cross-check the frame record before trusting its encoded checksum.
    result = f_lseek(file_, FSIZE_t(originalOffset_ + offset));
    if (result == FR_OK) {
        result = f_read(file_, record, sizeof(record), &count);
        if (result == FR_OK && count != sizeof(record)) {
            result = FR_DISK_ERR;
        }
    }
    if (result != FR_OK) {
        return false;
    }
    if (MediaMetadataCodec::ReadUnsigned32LittleEndian(record) != length) {
        result = FR_INVALID_OBJECT;
        return false;
    }
    checksum = MediaMetadataCodec::ReadUnsigned32LittleEndian(record + RGB_COPIES_CHECKSUM_BYTES);
    // Decode the first frame locally under the existing storage loan; only
    // small preview samples survive the decode.
    jpegCursor_ = originalOffset_ + offset + sizeof(record);
    jpegRemaining_ = length;
    jpegChecksum_ = STORAGE_CRC32_INITIAL_U;
    jpegLastWord_ = 0;
    jpegDecodedBytes_ = 0;
    jpegResult_ = FR_OK;
    decoderError = 0;
    decoded = JpegFrameDecoder::Decode(ReadJpegInput, this, ConsumeJpegPixels, this,
        metadata_.width, metadata_.height, workspace_ + RGB_COPIES_JPEG_WORK_OFFSET,
        capacity_ - RGB_COPIES_JPEG_WORK_OFFSET, decoderError);
    // Include any trailing encoded bytes in CRC/EOI validation too.
    while (decoded && jpegRemaining_ && jpegResult_ == FR_OK) {
        ReadJpegInput(this, nullptr, jpegRemaining_);
    }
    result = jpegResult_;
    if (!decoded || result != FR_OK || jpegRemaining_ || ~jpegChecksum_ != checksum ||
        jpegLastWord_ != RGB_COPIES_JPEG_END_MARKER ||
        jpegDecodedBytes_ != size_t(metadata_.width) * metadata_.height * RGB888_PIXEL_BYTES) {
        if (result == FR_OK) {
            result = FR_INVALID_OBJECT;
        }
        return false;
    }
    return FinishPreview(result);
} // PrepareJpegPreview
