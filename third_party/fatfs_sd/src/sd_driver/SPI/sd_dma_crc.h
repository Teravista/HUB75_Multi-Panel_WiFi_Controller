#pragma once

#include <SPI/my_spi.h>

#ifndef SD_SPI_HARDWARE_CRC16
#define SD_SPI_HARDWARE_CRC16 1
#endif

#define SD_DMA_CRC16_SEED 0u
#define SD_DMA_CRC16_MASK 0xffffu

typedef struct SdDmaCrc16 {
    spi_t* spi;
    uint32_t savedControl;
    uint32_t savedAccumulator;
    uint32_t enabledControl;
    bool savedChannelSniff;
    bool active;
} SdDmaCrc16;

// Reserves the idle checksum engine for the next SD receive DMA without waiting.
bool BeginSdDmaCrc16(SdDmaCrc16* state, spi_t* spi);

// Collects a completed receive checksum and restores the checksum engine and channel settings.
bool FinishSdDmaCrc16(SdDmaCrc16* state, bool transferSucceeded, uint16_t* checksum);

// Disables hardware checking after a confirmed hardware/software discrepancy.
void DisableSdDmaCrc16(void);
