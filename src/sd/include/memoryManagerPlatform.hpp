#pragma once

// Initializes the SD SPI device and prepares its hardware resources.
bool InitializeMemoryStorageHardware(void);

// Releases SD SPI and DMA resources after storage has been closed.
void ReleaseMemoryStorageHardware(void);

// Returns the SD SPI clock currently selected by the driver.
unsigned GetMemoryStorageClockHz(void);

#include "../../../third_party/fatfs_sd/src/sd_driver/SPI/sd_read_profile.h"
using MemoryStorageReadWork = void (*)(void* context);
// Installs bounded CPU work to run during SD read DMA waits.
void SetMemoryStorageReadWork(MemoryStorageReadWork work, void* context);

// Starts low-level SD command, transfer, and checksum timing collection.
void BeginMemoryStorageReadProfile(void);

// Stops low-level SD profiling and returns its accumulated timings.
sd_spi_read_profile_t EndMemoryStorageReadProfile(void);
