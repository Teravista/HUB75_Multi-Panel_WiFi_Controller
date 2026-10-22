#include "media/jpeg/include/jpegFrameDecoder.hpp"
#include "../../../third_party/tjpgd/tjpgd.h"
#include "config/defines.h"

#define JPEG_WORK_ALIGNMENT 4
#define JPEG_FAST_DECODE_MODE 2
#define JPEG_RGB888_OUTPUT 0

namespace {
struct DecodeContext {
    JpegFrameDecoder::Input input;
    void* inputContext;
    JpegFrameDecoder::Output output;
    void* outputContext;
    uint16_t width;
    uint16_t height;
};

/**
    Supplies encoded bytes through the caller-owned input callback.

    @param decoder Decoder whose device points to the bounded callback context.
    @param bytes Writable input buffer, or null when skipping input.
    @param count Maximum encoded bytes to read or skip.

    @return count - Number of bytes supplied, never exceeding the request.
    @return 0 - No further bytes are available.
*/
size_t
ReadEncodedJpegBytes (
    JDEC* decoder,
    uint8_t* bytes,
    size_t count
    )
{
    DecodeContext* context = nullptr;

    context = static_cast<DecodeContext*>(decoder->device);
    return context->input(context->inputContext, bytes, count);
} // ReadEncodedJpegBytes

/**
    Delivers decoded tiles as correctly positioned RGB888 row fragments.

    The decoder emits small MCU tiles rather than whole rows. Every borrowed row
    is consumed synchronously; offsets may be nonmonotonic. A null sink still
    performs full decoding for save verification without publishing any pixels.

    @param decoder Decoder carrying the borrowed output context.
    @param bitmap Read-only decoded MCU tile.
    @param rect Inclusive logical rectangle occupied by the tile.

    @return 1 - All tile rows were accepted, or this is validation only.
    @return 0 - The rectangle or output consumer rejected the tile.
*/
int
WriteDecodedJpegPixels (
    JDEC* decoder,
    void* bitmap,
    JRECT* rect
    )
{
    DecodeContext* context = nullptr;
    const uint8_t* pixels = nullptr;
    size_t rowBytes;
    size_t row;
    size_t offset;

    context = static_cast<DecodeContext*>(decoder->device);
    if (rect->right < rect->left || rect->bottom < rect->top ||
        rect->right >= context->width || rect->bottom >= context->height) {
        return 0;
    }
    if (!context->output) {
        return 1;
    }
    rowBytes = size_t(rect->right - rect->left + 1) * IMAGE_BYTES_PER_PIXEL;
    pixels = static_cast<const uint8_t*>(bitmap);
    for (row = rect->top; row <= rect->bottom; ++row) {
        offset = (row * context->width + rect->left) * IMAGE_BYTES_PER_PIXEL;
        if (!context->output(context->outputContext, pixels, rowBytes, offset)) {
            return 0;
        }
        pixels += rowBytes;
    }
    return 1;
} // WriteDecodedJpegPixels

} // namespace

/**
    Decodes one baseline JPEG using bounded caller-owned memory.

    Parses tables into the supplied workspace, verifies dimensions before output,
    then reconstructs every MCU. No heap or full-frame allocation is made here.
    Pixel output remains provisional until the surrounding storage reader also
    validates its encoded CRC and end marker.

    @param input Bounded encoded-input callback, including null-buffer skip requests.
    @param inputContext Borrowed input callback state.
    @param output Synchronous provisional pixel consumer, or null for full validation.
    @param outputContext Borrowed pixel consumer state.
    @param width Required logical image width.
    @param height Required logical image height.
    @param work Caller-owned aligned decoder workspace.
    @param workBytes Workspace capacity in bytes.
    @param error Receives the TJpgDec result; zero means success.

    @return true - The expected image decoded completely and all output was accepted.
    @return false - Input, workspace, dimensions, JPEG coding or output acceptance failed.
*/
bool
JpegFrameDecoder::Decode (
    Input input,
    void* inputContext,
    Output output,
    void* outputContext,
    uint16_t width,
    uint16_t height,
    void* work,
    size_t workBytes,
    int& error
    )
{
    DecodeContext context;
    JDEC decoder;

    static_assert(JD_FORMAT == JPEG_RGB888_OUTPUT && JD_FASTDECODE == JPEG_FAST_DECODE_MODE,
                  "JPEG decoder workspace configuration changed");
    error = JDR_PAR;
    if (!input || !work || !width || !height || reinterpret_cast<uintptr_t>(work) % JPEG_WORK_ALIGNMENT) {
        return false;
    }
    context = {input, inputContext, output, outputContext, width, height};
    decoder = {};
    error = jd_prepare(&decoder, ReadEncodedJpegBytes, work, workBytes, &context);
    if (error != JDR_OK) {
        return false;
    }
    if (decoder.width != width || decoder.height != height) {
        error = JDR_PAR;
        return false;
    }
    error = jd_decomp(&decoder, WriteDecodedJpegPixels, 0);
    return error == JDR_OK;
} // Decode
