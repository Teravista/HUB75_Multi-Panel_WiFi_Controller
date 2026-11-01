#include <include/ParallelPanelScanClass.hpp>
#include <include/PackedPanelFrameClass.hpp>

#include <algorithm>
#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/irq.h>
#include <hardware/sync.h>
#include <cstdio>
#include <hub75.hpp>
#include <hub75.pio.h>
#include <parallelPanelData.pio.h>
#include <pico/stdlib.h>

ParallelPanelScan* ParallelPanelScan::instance_ = nullptr;

/**
    Claims the selected pixel outputs and shared row sequencer without allocating frames.

    The original driver remains the framebuffer owner. Its next-row callback
    runs only after a hardware latch notification; its scan callback runs after
    the last displayed row finishes its lit/dark period. Pixel loading can then
    overlap illumination without releasing an in-use buffer or latching early.

    @param configuration Geometry and borrowed callbacks, retained until shutdown.
*/
void
ParallelPanelScan::Initialize (
    const Configuration& configuration
    )
{
    if (configuration.frameWidth == 0 || configuration.outputCount == 0 ||
        configuration.outputCount > PARALLEL_SCAN_MAX_OUTPUTS ||
        configuration.frameWidth % configuration.outputCount != 0 ||
        configuration.frameHeight == 0 || configuration.frameHeight % PARALLEL_SCAN_ROW_HALVES != 0 ||
        configuration.frameHeight / PARALLEL_SCAN_ROW_HALVES > (1u << ROWSEL_N_PINS) ||
        configuration.wordsPerLaneRow == 0 ||
        !configuration.prepareNextRow || !configuration.scanFinished) {
        panic("Invalid parallel HUB75 geometry/callbacks");
    }

    width_ = configuration.frameWidth;
    outputCount_ = configuration.outputCount;
    allOutputsMask_ = (1u << outputCount_) - 1u;
    rowCount_ = configuration.frameHeight / PARALLEL_SCAN_ROW_HALVES;
    rowWords_ = configuration.wordsPerLaneRow;
    onPrepareNextRow_ = configuration.prepareNextRow;
    onScanFinished_ = configuration.scanFinished;
    instance_ = this;
    ConfigurePio(configuration.invertedLatch);
    ConfigureDma();

    irq_set_exclusive_handler(DMA_IRQ_1, HandleDmaInterrupt);
    irq_set_priority(DMA_IRQ_1, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(DMA_IRQ_1, true);

    std::printf("[display] pio_outputs=%lu packed=1 overlap=1 clock_cycles=10 columns_per_output=%lu row_words=%lu dual_first_rotate_180=%u\n",
        static_cast<unsigned long>(outputCount_),
        static_cast<unsigned long>(width_ / outputCount_),
        static_cast<unsigned long>(rowWords_),
        static_cast<unsigned int>(outputCount_ == PARALLEL_SCAN_MAX_OUTPUTS && PACKED_PANEL_DUAL_FIRST_OUTPUT_ROTATE_180));
} // Initialize

/**
    Initializes only active pixel outputs and the shared row/latch/output-enable program.

    PIO0 retains the existing panel pins and the same latch/brightness periods.
    Single-output operation never claims PIO1 or the secondary pins. With two
    outputs, PIO1 drives six secondary data pins plus GP14; GP23..25 retain their
    radio functions. The primary output consumes ready six-bit values; the
    secondary inserts the physical pin gap. Both use ten cycles per column,
    with six low and four high clock cycles, and keep their explicit row fence.

    @param invertedLatch Whether the panel requires an inverted latch pulse.
*/
void
ParallelPanelScan::ConfigurePio (
    bool invertedLatch
    )
{
    pio_sm_config config;
    uint32_t pin;
    uint32_t lane;
    uint32_t waitCycles;
    uint32_t instructionIndex;

    lanes_[0].pio = pio0;
    lanes_[0].stateMachine = pio_claim_unused_sm(pio0, true);

    lanes_[0].programOffset = pio_add_program(pio0, &parallel_panel_primary_program);
    if (outputCount_ == PARALLEL_SCAN_MAX_OUTPUTS) {
        lanes_[1].pio = pio1;
        lanes_[1].stateMachine = pio_claim_unused_sm(pio1, true);

        // Keep the sparse output's GP23..25 gap away from radio PIO pin muxing.
        for (lane = 0; lane < PARALLEL_SCAN_STATE_MACHINES_PER_BLOCK - 1; ++lane) {
            reservedMachines_[lane] = pio_claim_unused_sm(pio1, true);
        }
        lanes_[1].programOffset = pio_add_program(pio1, &parallel_panel_data_program);
    }

    // Both pixel programs start at a row-count PULL and finish at IRQ WAIT.
    // Primary pins retain their existing electrical assignment and clock.
    for (pin = 0; pin < DATA_N_PINS; ++pin) {
        pio_gpio_init(pio0, DATA_BASE_PIN + pin);
    }
    pio_gpio_init(pio0, CLK_PIN);
    pio_sm_set_consecutive_pindirs(pio0, lanes_[0].stateMachine, DATA_BASE_PIN, DATA_N_PINS, true);
    pio_sm_set_consecutive_pindirs(pio0, lanes_[0].stateMachine, CLK_PIN, 1, true);
    config = parallel_panel_primary_program_get_default_config(lanes_[0].programOffset);
    sm_config_set_out_pins(&config, DATA_BASE_PIN, DATA_N_PINS);
    sm_config_set_sideset_pins(&config, CLK_PIN);
    sm_config_set_out_shift(&config, true, true, PACKED_PANEL_WORD_BITS);
    sm_config_set_in_shift(&config, false, false, PARALLEL_SCAN_WORD_BITS);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    pio_sm_init(pio0, lanes_[0].stateMachine, lanes_[0].programOffset, &config);

    pio_sm_set_enabled(pio0, lanes_[0].stateMachine, true);

    if (outputCount_ == PARALLEL_SCAN_MAX_OUTPUTS) {
        // Assign only exposed data pins; the holes in the OUT span must not claim Wi-Fi GPIOs.
        for (pin = 0; pin < PARALLEL_SCAN_COLORS_PER_HALF; ++pin) {
            pio_gpio_init(pio1, PARALLEL_SCAN_SECOND_TOP_PIN + pin);
            pio_gpio_init(pio1, PARALLEL_SCAN_SECOND_BOTTOM_PIN + pin);
        }
        pio_gpio_init(pio1, PARALLEL_SCAN_SECOND_CLOCK_PIN);
        pio_sm_set_consecutive_pindirs(pio1, lanes_[1].stateMachine,
            PARALLEL_SCAN_SECOND_TOP_PIN, PARALLEL_SCAN_COLORS_PER_HALF, true);
        pio_sm_set_consecutive_pindirs(pio1, lanes_[1].stateMachine,
            PARALLEL_SCAN_SECOND_BOTTOM_PIN, PARALLEL_SCAN_COLORS_PER_HALF, true);
        pio_sm_set_consecutive_pindirs(pio1, lanes_[1].stateMachine,
            PARALLEL_SCAN_SECOND_CLOCK_PIN, 1, true);
        config = parallel_panel_data_program_get_default_config(lanes_[1].programOffset);
        sm_config_set_out_pins(&config, PARALLEL_SCAN_SECOND_TOP_PIN, PARALLEL_SCAN_SECOND_PIN_SPAN);
        sm_config_set_sideset_pins(&config, PARALLEL_SCAN_SECOND_CLOCK_PIN);
        sm_config_set_out_shift(&config, true, true, PACKED_PANEL_WORD_BITS);
        sm_config_set_in_shift(&config, false, false, PARALLEL_SCAN_WORD_BITS);
        sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
        pio_sm_init(pio1, lanes_[1].stateMachine, lanes_[1].programOffset, &config);

        pio_sm_set_enabled(pio1, lanes_[1].stateMachine, true);
    }

    // The row program emits an explicit post-latch event before its lit period.
    // Keep the original address/latch pulse lengths and anti-ghost settling time.
    rowPio_ = pio0;
    rowMachine_ = pio_claim_unused_sm(rowPio_, true);
    rowProgramOffset_ = pio_add_program(rowPio_, &parallel_panel_row_program);
    pio_sm_set_consecutive_pindirs(rowPio_, rowMachine_, ROWSEL_BASE_PIN, ROWSEL_N_PINS, true);
    pio_sm_set_consecutive_pindirs(rowPio_, rowMachine_, STROBE_PIN, 2, true);
    for (pin = 0; pin < ROWSEL_N_PINS; ++pin) {
        pio_gpio_init(rowPio_, ROWSEL_BASE_PIN + pin);
    }
    pio_gpio_init(rowPio_, STROBE_PIN);
    pio_gpio_init(rowPio_, OEN_PIN);
    config = parallel_panel_row_program_get_default_config(rowProgramOffset_);
    sm_config_set_out_pins(&config, ROWSEL_BASE_PIN, ROWSEL_N_PINS);
    sm_config_set_sideset_pins(&config, STROBE_PIN);
    sm_config_set_in_shift(&config, true, true, PARALLEL_SCAN_WORD_BITS);
    sm_config_set_out_shift(&config, true, true, PARALLEL_SCAN_WORD_BITS);
    pio_sm_init(rowPio_, rowMachine_, rowProgramOffset_, &config);
    if (invertedLatch) {
        for (instructionIndex = 0; instructionIndex < parallel_panel_row_program.length; ++instructionIndex) {
            rowPio_->instr_mem[rowProgramOffset_ + instructionIndex] ^= pio_encode_sideset(2, 1);
        }
    }
    waitCycles = clock_get_hz(clk_sys) / PARALLEL_SCAN_LATCH_CLOCK_DIVISOR;
    pio_sm_exec(rowPio_, rowMachine_, pio_encode_out(pio_y, PARALLEL_SCAN_WORD_BITS));
    pio_sm_put(rowPio_, rowMachine_, waitCycles);
    pio_sm_set_enabled(rowPio_, rowMachine_, true);
    pio_interrupt_clear(rowPio_, PARALLEL_SCAN_LATCH_FINISHED_IRQ);
    pio_set_irq0_source_enabled(rowPio_, pis_interrupt1, true);

    for (lane = 0; lane < outputCount_; ++lane) {
        pio_sm_set_clkdiv(lanes_[lane].pio, lanes_[lane].stateMachine,
            std::max(SM_CLOCKDIV_FACTOR, PACKED_PANEL_DATA_CLOCK_DIVIDER));
        pio_interrupt_clear(lanes_[lane].pio, PARALLEL_SCAN_ROW_FINISHED_IRQ);
        pio_set_irq0_source_enabled(lanes_[lane].pio, pis_interrupt0, true);
        irq_set_exclusive_handler(pio_get_irq_num(lanes_[lane].pio, 0), HandlePioInterrupt);
        irq_set_priority(pio_get_irq_num(lanes_[lane].pio, 0), PICO_HIGHEST_IRQ_PRIORITY);
        irq_set_enabled(pio_get_irq_num(lanes_[lane].pio, 0), true);
    }
    pio_sm_set_clkdiv(rowPio_, rowMachine_, std::max(SM_CLOCKDIV_FACTOR, 1.0f));
} // ConfigurePio

/**
    Configures one memory-to-PIO DMA transfer with explicit chaining.

    @param channel Claimed DMA channel to configure.
    @param words Number of 32-bit words transferred on every trigger.
    @param increment Whether source words advance through a row or record.
    @param nextChannel Channel triggered on completion; the same channel disables chaining.
    @param pio Destination PIO block.
    @param stateMachine Destination TX FIFO state machine.
*/
void
ParallelPanelScan::ConfigureInput (
    int channel,
    uint32_t words,
    bool increment,
    int nextChannel,
    PIO pio,
    uint32_t stateMachine
    )
{
    dma_channel_config config;

    config = dma_channel_get_default_config(channel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
    channel_config_set_read_increment(&config, increment);
    channel_config_set_write_increment(&config, false);
    channel_config_set_high_priority(&config, true);
    channel_config_set_dreq(&config, pio_get_dreq(pio, stateMachine, true));
    channel_config_set_chain_to(&config, nextChannel);
    dma_channel_configure(channel, &config, &pio->txf[stateMachine], nullptr,
        dma_encode_transfer_count(words), false);
} // ConfigureInput

/**
    Connects active pixel streams to a shared row-completion interrupt boundary.

    Each active output owns one DMA channel, followed by a shared row/finish
    pair. Black clocks are already part of the prepared row, so separate dummy
    transfers are unnecessary. DMA completion alone cannot authorize a latch;
    the explicit PIO row-finished IRQs remain the pixel-completion boundary. DMA IRQ1 now reports the shared row-dwell completion;
    DMA IRQ0 remains available to SD and the radio uses ordinary SDK claiming.
*/
void
ParallelPanelScan::ConfigureDma (
    void
    )
{
    uint32_t lane;
    dma_channel_config config;

    static_assert(sizeof(RowRecord) == PARALLEL_SCAN_RECORD_WORDS * sizeof(uint32_t),
        "The row sequencer requires three consecutive 32-bit words");
    for (lane = 0; lane < outputCount_; ++lane) {
        lanes_[lane].pixelChannel = dma_claim_unused_channel(true);
        ConfigureInput(lanes_[lane].pixelChannel, rowWords_, true,
            lanes_[lane].pixelChannel, lanes_[lane].pio, lanes_[lane].stateMachine);
        pixelMask_ |= 1u << lanes_[lane].pixelChannel;
    }

    rowChannel_ = dma_claim_unused_channel(true);
    rowFinishedChannel_ = dma_claim_unused_channel(true);
    ConfigureInput(rowChannel_, PARALLEL_SCAN_RECORD_WORDS, true,
        rowChannel_, rowPio_, rowMachine_);

    config = dma_channel_get_default_config(rowFinishedChannel_);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
    channel_config_set_read_increment(&config, false);
    channel_config_set_write_increment(&config, false);
    channel_config_set_high_priority(&config, true);
    channel_config_set_dreq(&config, pio_get_dreq(rowPio_, rowMachine_, false));
    dma_channel_configure(rowFinishedChannel_, &config, &rowFinishedWord_,
        &rowPio_->rxf[rowMachine_], dma_encode_transfer_count(1), false);
    dma_channel_acknowledge_irq1(rowFinishedChannel_);
    dma_channel_set_irq1_enabled(rowFinishedChannel_, true);
} // ConfigureDma

/**
    Queues the next row for every active output while retaining its timing until safe latching.

    Called initially before scanning, then only after the preceding row's hardware
    latch notification. It borrows the next row from the existing scan allocation;
    the active row timing record remains separate until its DMA and display period
    finish. Both pixel programs must complete before TryLatchPendingRow can publish
    this pending record, regardless of which transfer or display event arrives first.

    @param pixels Borrowed prepared-bit scan buffer, stable through row completion.
    @param row Physical row-pair index within the scan.
    @param plane Brightness bit-plane selected for both PIO programs.
    @param litCycles Shared illuminated dwell length in PIO cycles.
    @param darkCycles Shared dark dwell length in PIO cycles.
*/
void
__not_in_flash_func(ParallelPanelScan::StartRow) (
    const volatile uint32_t* pixels,
    uint32_t row,
    uint32_t plane,
    uint32_t litCycles,
    uint32_t darkCycles
    )
{
    uint32_t rowOffset;
    uint32_t columnPairs;
    uint32_t lane;

    // Pending timing cannot overwrite the three words still being consumed/displayed.
    completedMask_ = 0;
    pendingRecord_.row = row;
    pendingRecord_.lit = litCycles;
    pendingRecord_.dark = darkCycles;
    pendingFinishesFrame_ = row + 1u == rowCount_ && plane + 1u == BIT_DEPTH;
    rowPending_ = true;

    // The prefix is not a pixel: it counts all real and dummy column pairs.
    // A row's final IRQ leaves each state machine blocked before the next prefix.
    columnPairs = width_ / outputCount_;
    rowOffset = (plane * rowCount_ + row) * outputCount_ * rowWords_;
    for (lane = 0; lane < outputCount_; ++lane) {
        pio_sm_put(lanes_[lane].pio, lanes_[lane].stateMachine, columnPairs - 1u);
        dma_channel_set_read_addr(lanes_[lane].pixelChannel,
            pixels + rowOffset + lane * rowWords_, false);
    }
    __dmb();
    dma_start_channel_mask(pixelMask_);
} // StartRow

/**
    Latches a completed pending row only after the previous display period finishes.

    Pixel-ready and display-finished notifications may arrive in either order.
    This single gate owns the transition to a new display period. It copies only
    three timing words, arms completion before starting their DMA, and leaves next
    pixel preparation blocked until the row PIO confirms its latch has settled.
*/
void
__not_in_flash_func(ParallelPanelScan::TryLatchPendingRow) (
    void
    )
{
    if (!rowPending_ || completedMask_ != allOutputsMask_ || displayActive_ || awaitingLatch_) {
        return;
    }

    activeRecord_.row = pendingRecord_.row;
    activeRecord_.lit = pendingRecord_.lit;
    activeRecord_.dark = pendingRecord_.dark;
    activeFinishesFrame_ = pendingFinishesFrame_;
    rowPending_ = false;
    displayActive_ = true;
    awaitingLatch_ = true;

    dma_channel_set_read_addr(rowChannel_, &activeRecord_, false);
    dma_channel_start(rowFinishedChannel_);
    __dmb();
    dma_channel_start(rowChannel_);
} // TryLatchPendingRow

/**
    Advances pixel and latch events without loading new pixels before a confirmed latch.

    Each finite data loop stops at its row-complete IRQ. The shared row program
    separately raises IRQ1 after its latch pulse and settling delay. Only that
    notification authorizes the driver's next-row callback, which can load the
    following pixels while the current output latch remains illuminated. Equal
    IRQ priorities on one core serialize these transitions without a spin wait.
*/
void
__not_in_flash_func(ParallelPanelScan::HandlePioInterrupt) (
    void
    )
{
    ParallelPanelScan* scanner = nullptr;
    uint32_t lane;
    uint32_t completed;

    scanner = instance_;
    completed = 0;
    for (lane = 0; lane < scanner->outputCount_; ++lane) {
        if (pio_interrupt_get(scanner->lanes_[lane].pio, PARALLEL_SCAN_ROW_FINISHED_IRQ)) {
            pio_interrupt_clear(scanner->lanes_[lane].pio, PARALLEL_SCAN_ROW_FINISHED_IRQ);
            completed |= 1u << lane;
        }
    }
    if (scanner->rowPending_) {
        scanner->completedMask_ |= completed;
    }

    // Clear the old row's event before its callback installs a new pending row.
    if (pio_interrupt_get(scanner->rowPio_, PARALLEL_SCAN_LATCH_FINISHED_IRQ)) {
        pio_interrupt_clear(scanner->rowPio_, PARALLEL_SCAN_LATCH_FINISHED_IRQ);
        if (scanner->awaitingLatch_) {
            scanner->awaitingLatch_ = false;
            scanner->onPrepareNextRow_();
        }
    }
    scanner->TryLatchPendingRow();
} // HandlePioInterrupt

/**
    Reports shared row-dwell completion to the driver scan callback.

    Releasing the active output period can expose an already loaded next row to
    TryLatchPendingRow. A late latch notification is still required before its
    pixels are prepared. Completed scan accounting occurs only after the final
    row/bit-plane's full lit and dark periods, independently of buffer release.
*/
void
__not_in_flash_func(ParallelPanelScan::HandleDmaInterrupt) (
    void
    )
{
    ParallelPanelScan* scanner = nullptr;

    scanner = instance_;
    if ((dma_hw->ints1 & (1u << scanner->rowFinishedChannel_)) == 0) {
        return;
    }
    dma_channel_acknowledge_irq1(scanner->rowFinishedChannel_);
    if (!scanner->displayActive_) {
        return;
    }

    scanner->displayActive_ = false;
    if (scanner->activeFinishesFrame_) {
        scanner->activeFinishesFrame_ = false;
        scanner->onScanFinished_();
    }
    scanner->TryLatchPendingRow();
} // HandleDmaInterrupt
