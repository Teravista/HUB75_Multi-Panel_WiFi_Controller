// Local SD receive-DMA checksum integration; original SPI driver licenses remain unchanged.
#include <SPI/sd_dma_crc.h>

#if SD_SPI_HARDWARE_CRC16
#include <hardware/dma.h>
#include <pico/mutex.h>
#include <stdio.h>

auto_init_mutex(sdDmaCrcMutex);
static bool hardwareCrcDisabled;
#endif

/**
    Reserves the idle checksum engine for the next SD receive DMA without waiting.

    The caller already owns the SPI bus and must finish this reservation after
    transfer completion or timeout. The existing receive channel performs the
    checksum while it reads the peripheral; no channel or pixel buffer is added.
    A busy engine leaves every register untouched and selects software checking.

    @param state Zero-initialized caller-owned reservation for one DMA transfer.
    @param spi Idle SD SPI instance whose receive channel will be started next.

    @return true - Hardware CRC16 is reserved and attached to the next receive.
    @return false - Disabled, busy or invalid; the caller must use software CRC16.
*/
bool
BeginSdDmaCrc16 (
    SdDmaCrc16* state,
    spi_t* spi
    )
{
#if SD_SPI_HARDWARE_CRC16
    uint32_t control;

    if (!state || !spi || state->active ||
        __atomic_load_n(&hardwareCrcDisabled, __ATOMIC_RELAXED)) {
        return false;
    }
    if (!mutex_try_enter(&sdDmaCrcMutex, NULL)) {
        return false;
    }

    // Preserve another user's engine; interrupts stay enabled throughout the transfer.
    control = dma_hw->sniff_ctrl;
    if ((control & DMA_SNIFF_CTRL_EN_BITS) || dma_channel_is_busy(spi->rx_dma)) {
        mutex_exit(&sdDmaCrcMutex);
        return false;
    }
    state->spi = spi;
    state->savedControl = control;
    state->savedChannelSniff = (spi->rx_dma_cfg.ctrl & DMA_CH0_CTRL_TRIG_SNIFF_EN_BITS) != 0;

    // Disable output transforms before saving the raw accumulator and setting the SD seed.
    dma_sniffer_disable();
    state->savedAccumulator = dma_sniffer_get_data_accumulator();
    dma_sniffer_set_data_accumulator(SD_DMA_CRC16_SEED);
    channel_config_set_sniff_enable(&spi->rx_dma_cfg, true);
    dma_sniffer_enable(spi->rx_dma, DMA_SNIFF_CTRL_CALC_VALUE_CRC16, false);
    state->enabledControl = dma_hw->sniff_ctrl;
    state->active = true;
    return true;
#else
    (void)state;
    (void)spi;
    return false;
#endif
} // BeginSdDmaCrc16

/**
    Collects a completed receive checksum and restores the checksum engine and channel settings.

    Call after the SPI wait has completed or aborted both DMA channels. A failed
    transfer never produces an accepted checksum. If an unrelated user changed
    the shared engine, its new registers are left untouched and software checking
    handles this block. The reservation is always released before returning.

    @param state Reservation created before starting this receive transfer.
    @param transferSucceeded Whether the receive and SPI completion checks passed.
    @param checksum Destination for the completed CRC16 when this returns true.

    @return true - The complete hardware checksum is available in checksum.
    @return false - No reservation, failed transfer or engine ownership changed.
*/
bool
FinishSdDmaCrc16 (
    SdDmaCrc16* state,
    bool transferSucceeded,
    uint16_t* checksum
    )
{
#if SD_SPI_HARDWARE_CRC16
    bool ownsEngine;
    bool valid;

    if (!state || !state->active) {
        return false;
    }
    ownsEngine = dma_hw->sniff_ctrl == state->enabledControl;
    valid = ownsEngine && transferSucceeded && checksum;
    if (valid) {
        *checksum = (uint16_t)(dma_sniffer_get_data_accumulator() & SD_DMA_CRC16_MASK);
    }

    // Restore the channel's flag without disturbing its SPI increment/DREQ settings.
    channel_config_set_sniff_enable(&state->spi->rx_dma_cfg, state->savedChannelSniff);
    hw_write_masked(&dma_hw->ch[state->spi->rx_dma].al1_ctrl,
        state->spi->rx_dma_cfg.ctrl, DMA_CH0_CTRL_TRIG_SNIFF_EN_BITS);
    if (ownsEngine) {
        dma_sniffer_disable();
        dma_sniffer_set_data_accumulator(state->savedAccumulator);
        dma_hw->sniff_ctrl = state->savedControl;
    }
    state->active = false;
    state->spi = NULL;
    mutex_exit(&sdDmaCrcMutex);
    return valid;
#else
    (void)state;
    (void)transferSucceeded;
    (void)checksum;
    return false;
#endif
} // FinishSdDmaCrc16

/**
    Disables hardware checking after a confirmed hardware/software discrepancy.

    The current and later blocks retain software CRC validation. This affects
    future reservations only; any already active transfer still finishes normally.
    One important event identifies the fallback without flooding the serial log.
*/
void
DisableSdDmaCrc16 (
    void
    )
{
#if SD_SPI_HARDWARE_CRC16
    if (!__atomic_exchange_n(&hardwareCrcDisabled, true, __ATOMIC_RELAXED)) {
        printf("[sd] DMA CRC differs; retaining software CRC checks\n");
    }
#endif
    return;
} // DisableSdDmaCrc16
