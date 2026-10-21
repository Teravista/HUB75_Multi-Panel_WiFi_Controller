#pragma once
#include <stdint.h>

// Optional foreground diagnostics. transfer_us includes work performed while
// DMA is active; it must not be added to crc_us to infer elapsed read time.
typedef struct sd_spi_read_profile_t {
    uint64_t command_us, token_us, transfer_us, crc_us;
    uint32_t transactions, sectors;
} sd_spi_read_profile_t;
