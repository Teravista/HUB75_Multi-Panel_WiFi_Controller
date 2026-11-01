#pragma once

#include <cstdint>
#include <hardware/pio.h>

#define PARALLEL_SCAN_MAX_OUTPUTS 2u
#define PARALLEL_SCAN_ROW_HALVES 2u
#define PARALLEL_SCAN_ROW_FINISHED_IRQ 0u
#define PARALLEL_SCAN_LATCH_FINISHED_IRQ 1u
#define PARALLEL_SCAN_STATE_MACHINES_PER_BLOCK 4u
#define PARALLEL_SCAN_COLORS_PER_HALF 3u
#define PARALLEL_SCAN_SECOND_TOP_PIN 20u
#define PARALLEL_SCAN_SECOND_BOTTOM_PIN 26u
#define PARALLEL_SCAN_SECOND_CLOCK_PIN 14u
#define PARALLEL_SCAN_SECOND_PIN_SPAN 9u
#define PARALLEL_SCAN_RECORD_WORDS 3u
#define PARALLEL_SCAN_WORD_BITS 32u
#define PARALLEL_SCAN_LATCH_CLOCK_DIVISOR 4000000u

/**
    Refreshes one or two panel chains from one shared driver framebuffer.

    The HUB75 driver owns its existing front/back allocations, pixel packing and
    frame-swap boundary. This backend borrows prepared six-bit rows, sending each
    lane's compact words through separate PIO blocks and DMA channels. Both outputs, when enabled, share
    row-select, latch and output-enable signals. The next row loads while the
    latched row is illuminated, but it cannot latch until both pixel programs and
    the previous lit/dark period finish. A separate latch notification authorizes
    preparation, so no next-row clocks can corrupt the current latch. Framebuffer
    release follows the last row's latch; completed scans count its finished dwell.
    All scanner interrupts and row restarts run on the initializing core.
*/
class ParallelPanelScan {
public:
    typedef void (*ScanCallback)(void);

    struct Configuration {
        uint32_t frameWidth = 0;
        uint32_t frameHeight = 0;
        uint32_t outputCount = 1;
        uint32_t wordsPerLaneRow = 0;
        bool invertedLatch = false;
        ScanCallback prepareNextRow = nullptr;
        ScanCallback scanFinished = nullptr;
    };

    // Claims the selected pixel outputs and shared row sequencer without allocating frames.
    void Initialize(const Configuration& configuration);

    // Queues the next row for every active output while retaining its timing until safe latching.
    void StartRow(const volatile uint32_t* pixels, uint32_t row, uint32_t plane, uint32_t litCycles, uint32_t darkCycles);

private:
    struct PixelLane {
        PIO pio = nullptr;
        uint32_t stateMachine = 0;
        uint32_t programOffset = 0;
        int pixelChannel = -1;
    };

    struct alignas(uint32_t) RowRecord {
        uint32_t row = 0;
        uint32_t lit = 0;
        uint32_t dark = 0;
    };

    static ParallelPanelScan* instance_;
    PixelLane lanes_[PARALLEL_SCAN_MAX_OUTPUTS];
    PIO rowPio_ = nullptr;
    uint32_t reservedMachines_[PARALLEL_SCAN_STATE_MACHINES_PER_BLOCK - 1] = {};
    uint32_t rowMachine_ = 0;
    uint32_t rowProgramOffset_ = 0;
    uint32_t width_ = 0;
    uint32_t outputCount_ = 1;
    uint32_t allOutputsMask_ = 1;
    uint32_t rowCount_ = 0;
    uint32_t rowWords_ = 0;
    int rowChannel_ = -1;
    int rowFinishedChannel_ = -1;
    uint32_t completedMask_ = 0;
    uint32_t pixelMask_ = 0;
    volatile uint32_t rowFinishedWord_ = 0;
    volatile RowRecord pendingRecord_;
    volatile RowRecord activeRecord_;
    bool rowPending_ = false;
    bool displayActive_ = false;
    bool awaitingLatch_ = false;
    bool pendingFinishesFrame_ = false;
    bool activeFinishesFrame_ = false;
    ScanCallback onPrepareNextRow_ = nullptr;
    ScanCallback onScanFinished_ = nullptr;

    // Initializes only active pixel outputs and the shared row/latch/output-enable program.
    void ConfigurePio(bool invertedLatch);

    // Connects active pixel streams to a shared row-completion interrupt boundary.
    void ConfigureDma(void);

    // Configures one memory-to-PIO DMA transfer with explicit chaining.
    static void ConfigureInput(int channel, uint32_t words, bool increment, int nextChannel, PIO pio, uint32_t stateMachine);

    // Latches a completed pending row only after the previous display period finishes.
    void TryLatchPendingRow(void);

    // Advances pixel and latch events without loading new pixels before a confirmed latch.
    static void HandlePioInterrupt(void);

    // Reports shared row-dwell completion to the driver scan callback.
    static void HandleDmaInterrupt(void);

};
