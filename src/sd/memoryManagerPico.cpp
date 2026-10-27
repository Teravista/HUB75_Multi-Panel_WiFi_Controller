#include "sd/include/memoryManagerPlatform.hpp"
#include "config/include/storageConfig.hpp"
#include "hw_config.h"
#include "hardware/clocks.h"

#define SD_PERIPHERAL_CLOCK_THRESHOLD_HZ 24'000'000

namespace {
spi_t storageSpi{};
sd_spi_if_t storageInterface{};
sd_card_t storageCard{};
bool configured = false;
sd_spi_read_profile_t storageReadProfile{};
} // namespace

/**
    Reports the number of configured SD card devices to the driver.

    @return 1 - Exactly one SD card is configured.
*/
extern "C" size_t
sd_get_num (
    void
    )
{
    return 1;
} // sd_get_num

/**
    Returns the configured SD card device for a valid driver index.

    @param index Zero-based SD device index.

    @return pointer - Borrowed SD device configuration for index zero.
    @return nullptr - The requested index is not configured.
*/
extern "C" sd_card_t*
sd_get_by_num (
    size_t index
    )
{
    if (index == 0) {
        return &storageCard;
    } else {
        return nullptr;
    }
} // sd_get_by_num

/**
    Initializes the SD SPI device and prepares its hardware resources.

    Configures the dedicated SPI/card descriptors once, then restores the driver hooks and
    claims the bus for each initialization attempt. This allows an explicit retry after a failed
    mount released the reader's DMA channels. It prepares hardware only; MemoryManager performs
    the filesystem mount afterward.

    @return true - Storage initialization succeeded, including an already initialized device.
    @return false - Storage is busy or hardware/filesystem initialization failed.
*/
bool
InitializeMemoryStorageHardware (
    void
    )
{
    bool initialized;
    unsigned systemClock;

    // Set the SPI peripheral clock source before the bus is initialized. The
    // card still starts at the driver's slow initialization clock.
    if (!configured && StorageConfig::BaudRate > SD_PERIPHERAL_CLOCK_THRESHOLD_HZ) {
        systemClock = clock_get_hz(clk_sys);
        clock_configure_undivided(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, systemClock);
    }

    // Configure the dedicated SPI bus once; each mount still reclaims only the resources owned by this
    // reader.
    if (!configured) {
        storageSpi.hw_inst = spi0;
        storageSpi.miso_gpio = StorageConfig::Miso;
        storageSpi.mosi_gpio = StorageConfig::Mosi;
        storageSpi.sck_gpio = StorageConfig::Clock;
        storageSpi.baud_rate = StorageConfig::BaudRate;
        storageInterface.spi = &storageSpi;
        storageInterface.ss_gpio = StorageConfig::ChipSelect;
        storageCard.type = SD_IF_SPI;
        storageCard.spi_if_p = &storageInterface;
        // Six-pin readers have no separate card-detect connection.
        storageCard.use_card_detect = false;
        configured = true;
    }
    // sd_init_driver has a process-wide initialized flag. Explicitly restore
    // the SPI/card hooks here too, so Initialize works after a failed attempt
    // has returned this module's DMA channels and deinitialized SPI0.
    sd_spi_ctor(&storageCard);
    if (!my_spi_init(&storageSpi)) {
        return false;
    }
    initialized = sd_init_driver();
    return initialized;
} // InitializeMemoryStorageHardware

/**
    Returns the SD SPI clock currently selected by the driver.

    @return value - Actual SPI clock in hertz.
    @return 0 - The SD SPI hardware is not initialized.
*/
unsigned
GetMemoryStorageClockHz (
    void
    )
{
    if (storageSpi.initialized) {
        return spi_get_baudrate(storageSpi.hw_inst);
    } else {
        return 0;
    }
} // GetMemoryStorageClockHz

/**
    Releases SD SPI and DMA resources after storage has been closed.

    Releases only DMA channels claimed by the SD SPI instance, deinitializes that bus and resets
    its card state. Data/clock pins return to input mode and chip select stays high so the
    reader is deselected. Unconfigured or already released hardware returns immediately; display
    and radio resources are not reset.
*/
void
ReleaseMemoryStorageHardware (
    void
    )
{
    constexpr unsigned pins[] = {StorageConfig::Miso, StorageConfig::Mosi, StorageConfig::Clock};
    size_t pinIndex;
    unsigned pin;

    SetMemoryStorageReadWork(nullptr, nullptr);
    EndMemoryStorageReadProfile();
    if (!configured || !storageSpi.initialized) {
        return;
    }
    // Only release channels owned by this dedicated SPI instance. In particular,
    // never reset the DMA controller or touch display/radio channels.
    dma_channel_cleanup(storageSpi.tx_dma);
    dma_channel_cleanup(storageSpi.rx_dma);
    dma_channel_unclaim(storageSpi.tx_dma);
    dma_channel_unclaim(storageSpi.rx_dma);
    spi_deinit(storageSpi.hw_inst);
    storageSpi.initialized = false;
    storageCard.state.m_Status |= STA_NOINIT;
    storageCard.state.card_type = SDCARD_NONE;
    for (pinIndex = 0; pinIndex < sizeof(pins) / sizeof(pins[0]); ++pinIndex) {
        pin = pins[pinIndex];
        gpio_set_function(pin, GPIO_FUNC_SIO);
        gpio_set_dir(pin, GPIO_IN);
        gpio_disable_pulls(pin);
    }
    // Leave the card deselected instead of allowing its CS input to float.
    gpio_put(StorageConfig::ChipSelect, 1);
} // ReleaseMemoryStorageHardware

/**
    Installs bounded CPU work to run during SD read DMA waits.

    @param work Bounded CPU-work callback to run while the independent operation progresses.
    @param context Caller-owned state passed unchanged to the callback.
*/
void
SetMemoryStorageReadWork (
    MemoryStorageReadWork work,
    void* context
    )
{
    storageInterface.read_work = work;
    if (work) {
        storageInterface.read_work_context = context;
    } else {
        storageInterface.read_work_context = nullptr;
    }
} // SetMemoryStorageReadWork

/**
    Starts low-level SD command, transfer, and checksum timing collection.
*/
void
BeginMemoryStorageReadProfile (
    void
    )
{
    storageReadProfile = {};
    storageInterface.read_profile = &storageReadProfile;
} // BeginMemoryStorageReadProfile

/**
    Stops low-level SD profiling and returns its accumulated timings.

    @return profile - Accumulated command, token, DMA and checksum timing counters.
*/
sd_spi_read_profile_t
EndMemoryStorageReadProfile (
    void
    )
{
    storageInterface.read_profile = nullptr;
    return storageReadProfile;
} // EndMemoryStorageReadProfile
