#pragma once

// Initializes the SD SPI device and prepares its hardware resources.
bool InitializeMemoryStorageHardware(void);

// Releases SD SPI and DMA resources after storage has been closed.
void ReleaseMemoryStorageHardware(void);

// Returns the SD SPI clock currently selected by the driver.
unsigned GetMemoryStorageClockHz(void);
