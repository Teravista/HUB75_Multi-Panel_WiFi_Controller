#include "media/jpeg/include/jpegFrameDecoder.hpp"
#include "../../../third_party/tjpgd/tjpgd.h"
#include "config/defines.h"

#define JPEG_DECODER_OUTPUT_RGB888 0
#define JPEG_FAST_DECODE_MODE 2
#define JPEG_WORK_ALIGNMENT 4

namespace {
struct Context {
    JpegFrameDecoder::Input input;
    void* inputContext;
    JpegFrameDecoder::Output output;
    void* outputContext;
    uint16_t width;
    uint16_t height;
    bool outputRGB565 = false;
    ImageTile::Sink tileOutput = nullptr;
    JpegFrameDecoder::Clock clock = nullptr;
    void* clockContext = nullptr;
    uint64_t inputUs = 0;
    uint64_t outputUs = 0;
    uint32_t clockReads = 0;

    /**
        Reads the decoder clock and counts profiling clock samples.

        @return value - Monotonic profiling clock time in microseconds.
    */
    uint64_t
    GetDecodeTimeUs (
        void
        )
    {
        ++clockReads;
        return clock(clockContext);
    } // GetDecodeTimeUs

};

/**
    Reads decoder CPU time with input and output waits excluded.

    @param state Opaque callback state pointing to the current decoder context.

    @return value - Decoder CPU clock in microseconds with time spent in input/output callbacks
                    subtracted.
*/
uint64_t
ReadDecoderCpuTimeUs (
    void* state
    )
{
    Context* context = nullptr;

    context = &(*static_cast<Context*>(state));
    return context->GetDecodeTimeUs() - context->inputUs - context->outputUs;
} // ReadDecoderCpuTimeUs

/**
    Supplies encoded JPEG bytes to the decoder's input callback.

    @param decoder JPEG decoder instance whose device pointer holds the callback state.
    @param bytes Writable byte buffer filled by this operation.
    @param count Number of bytes to process.

    @return count - Number of encoded bytes supplied or skipped, no more than requested.
    @return 0 - No input bytes could be supplied.
*/
size_t
ReadEncodedJpegBytes (
    JDEC* decoder,
    uint8_t* bytes,
    size_t count
    )
{
    Context* context = nullptr;
    uint64_t started;
    size_t result;

    context = &(*static_cast<Context*>(decoder->device));
    if (!context->clock) {
        return context->input(context->inputContext, bytes, count);
    }
    started = context->GetDecodeTimeUs();
    result = context->input(context->inputContext, bytes, count);
    context->inputUs += context->GetDecodeTimeUs() - started;
    return result;
} // ReadEncodedJpegBytes

/**
    Delivers one decoded JPEG tile or its compatible logical row fragments.

    Validates the decoder's MCU rectangle and translates its rows to byte offsets within the
    logical image. A tile sink receives the entire borrowed MCU rectangle once,
    including clipped edge dimensions. Without that sink, the established row
    callback receives the same tightly packed RGB888 or RGB565 fragments as before.
    Neither path copies the decoded pixels; rejection immediately stops decoding.

    @param context Caller-owned state passed unchanged to the callback.
    @param bitmap Decoded MCU pixel block supplied by the JPEG decoder.
    @param rect Inclusive image rectangle covered by the decoded MCU block.

    @return 1 - The decoded rectangle was accepted by the output callback.
    @return 0 - The output callback rejected the rectangle.
*/
int
DeliverDecodedTile (
    Context& context,
    void* bitmap,
    JRECT* rect
    )
{
    size_t rowBytes;
    size_t pixelBytes;
    const uint8_t* pixels = nullptr;
    size_t y;
    size_t pixelOffset;
    ImageTile tile;

    if (rect->right < rect->left || rect->bottom < rect->top || rect->right >= context.width ||
        rect->bottom >= context.height) {
        return 0;
    }
    // The MCU is already in its requested format; keep rows tightly packed and borrowed.
    pixelBytes = RGB888_PIXEL_BYTES;
    if (context.outputRGB565) {
        pixelBytes = RGB565_PIXEL_BYTES;
    }
    rowBytes = size_t(rect->right - rect->left + 1) * pixelBytes;
    pixels = static_cast<const uint8_t*>(bitmap);

    // A tile-capable consumer receives the complete rectangle through one borrowed callback.
    if (context.tileOutput) {
        tile.pixels = pixels;
        tile.left = rect->left;
        tile.top = rect->top;
        tile.width = rect->right - rect->left + 1;
        tile.height = rect->bottom - rect->top + 1;
        tile.pixelBytes = uint8_t(pixelBytes);
        tile.bytes = rowBytes * tile.height;
        if (!context.tileOutput(context.outputContext, tile)) {
            return 0;
        }
        return 1;
    }
    if (!context.output) {
        return 1;
    }

    // Legacy consumers retain their existing row offsets and output representation.
    for (y = rect->top; y <= rect->bottom; ++y) {
        pixelOffset = (y * context.width + rect->left) * pixelBytes;
        if (!context.output(context.outputContext, pixels, rowBytes, pixelOffset)) {
            return 0;
        }
        pixels += rowBytes;
    }

    return 1;
} // DeliverDecodedTile

/**
    Delivers a decoded JPEG tile and accounts for output callback time.

    @param decoder JPEG decoder instance whose device pointer holds the callback state.
    @param bitmap Decoded MCU pixel block supplied by the JPEG decoder.
    @param rect Inclusive image rectangle covered by the decoded MCU block.

    @return 1 - The decoded rectangle was accepted.
    @return 0 - The output callback stopped decoding.
*/
int
WriteDecodedJpegPixels (
    JDEC* decoder,
    void* bitmap,
    JRECT* rect
    )
{
    Context* context = nullptr;
    uint64_t started;
    int result;

    context = &(*static_cast<Context*>(decoder->device));
    if (!context->clock) {
        return DeliverDecodedTile((*context), bitmap, rect);
    }
    started = context->GetDecodeTimeUs();
    result = DeliverDecodedTile((*context), bitmap, rect);
    context->outputUs += context->GetDecodeTimeUs() - started;
    return result;
} // WriteDecodedJpegPixels

} // namespace

/**
    Decodes or validates one JPEG frame using bounded input and output callbacks.

    Prepares the caller-provided decoder workspace from encoded-input callbacks and rejects
    unsupported dimensions before reconstruction. An output callback receives logical RGB888 or
    little-endian RGB565 fragments in MCU order; offsets can be nonmonotonic and the callback must
    consume each borrowed fragment synchronously.

    With neither output sink, validation consumes the entropy stream without IDCT/color output, which supports
    faster SD save verification. No full-frame allocation is made. Optional profiling subtracts
    callback work from decoder time and can collect internal decoder stages using the supplied
    clock.

    @param input Callback supplying encoded JPEG bytes to the decoder.
    @param inputContext Caller-owned state passed to the encoded-input callback.
    @param output Callback receiving decoded pixels; false stops decoding.
    @param outputContext Caller-owned state passed to the decoded-output callback.
    @param width Image width in pixels.
    @param height Image height in pixels.
    @param work Caller-provided JPEG decoder workspace with workBytes available bytes.
    @param workBytes Available JPEG decoder workspace size in bytes.
    @param error Output JPEG decoder result code; JDR_OK indicates success.
    @param profile Optional output counters for individual JPEG decoder stages.
    @param clock Optional monotonic profiling clock returning microseconds.
    @param clockContext Caller-owned state passed to the profiling clock.
    @param outputRGB565 Whether decoded output uses RGB565 rather than RGB888 pixels.
    @param tileOutput Optional whole-tile sink, preferred over the compatible row sink.

    @return true - The baseline JPEG decoded completely and its output was accepted.
    @return false - Input, dimensions, workspace, decoding or output acceptance failed; error
                    contains the decoder result.
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
    int& error,
    Profile* profile,
    Clock clock,
    void* clockContext,
    bool outputRGB565,
    ImageTile::Sink tileOutput
    )
{
    Context context;
    JDPROFILE stages;
    JDEC decoder;
    uint64_t started;
    uint64_t prepared;
    decltype(&WriteDecodedJpegPixels) outputCallback;

    static_assert(JD_FORMAT == JPEG_DECODER_OUTPUT_RGB888 && JD_FASTDECODE == JPEG_FAST_DECODE_MODE && !JD_USE_SCALE,
                  "JPEG output/working memory configuration changed");
    error = JDR_PAR;
    if (profile) {
        *profile = {};
    }
    if (profile && !clock) {
        return false;
    }
    if (!input || !work || !width || !height || reinterpret_cast<uintptr_t>(work) % JPEG_WORK_ALIGNMENT) {
        return false;
    }

    // Connect bounded input and output callbacks without allocating a complete decoded image.
    context = {input, inputContext, output, outputContext, width, height};
    context.outputRGB565 = outputRGB565;
    context.tileOutput = tileOutput;
    if (profile) {
        context.clock = clock;
    } else {
        context.clock = nullptr;
    }
    context.clockContext = clockContext;

    // Exclude input waits and output work from the optional decoder-only timing samples.
    stages = {};
    stages.clock = ReadDecoderCpuTimeUs;
    stages.context = &context;
    decoder = {};
    if (profile) {
        started = ReadDecoderCpuTimeUs(&context);
    } else {
        started = 0;
    }

    // Parse JPEG tables into the caller-owned workspace and verify the expected image dimensions.
    error = jd_prepare(&decoder, ReadEncodedJpegBytes, work, workBytes, &context);
    if (error != JDR_OK) {
        return false;
    }
    if (decoder.width != width || decoder.height != height) {
        error = JDR_PAR;
        return false;
    }
    if (profile) {
        prepared = ReadDecoderCpuTimeUs(&context) - started;
    } else {
        prepared = 0;
    }
    if (profile) {
        decoder.profile = &stages;
    } else {
        decoder.profile = nullptr;
    }
    outputCallback = {};
    if (output || tileOutput) {
        outputCallback = WriteDecodedJpegPixels;
    } else {
        outputCallback = nullptr;
    }

    // Select native reconstruction after prepare initializes the decoder state.
    decoder.output_rgb565 = outputRGB565;

    // Decode into row callbacks, or validate the stream when no pixel consumer is supplied.
    error = jd_decomp(&decoder, outputCallback, 0);

    // Publish timing counters only for a successfully decoded frame.
    if (error == JDR_OK && profile) {
        profile->prepareUs = prepared;
        if (stages.load_us >= stages.transform_us) {
            profile->entropyUs = stages.load_us - stages.transform_us;
        } else {
            profile->entropyUs = 0;
        }
        profile->transformUs = stages.transform_us;
        profile->colorUs = stages.color_us;
        profile->mcus = stages.mcus;
        profile->idctBlocks = stages.idct_blocks;
        profile->dcBlocks = stages.dc_blocks;
        profile->clockReads = context.clockReads;
    }
    return error == JDR_OK;
} // Decode
