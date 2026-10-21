// Application extension: bounded SD CMD6 High Speed negotiation.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Callback consumes the complete R1 + 64-byte data block + CRC at the slow
// initialization clock, while CS stays asserted. False includes CRC/I/O errors.
typedef bool (*sd_spi_switch_fn)(void *context, uint32_t argument, uint8_t status[64]);

static inline uint32_t sd_spi_default_speed(uint32_t requested) {
    return requested < 25000000u ? requested : 25000000u;
}

typedef enum {
    SD_SPI_SPEED_DEFAULT, SD_SPI_SPEED_HIGH, SD_SPI_SPEED_NO_EXCHANGE,
    SD_SPI_SPEED_CHECK_IO, SD_SPI_SPEED_SWITCH_IO, SD_SPI_SPEED_INVALID_STATUS,
    SD_SPI_SPEED_UNSUPPORTED, SD_SPI_SPEED_BUSY, SD_SPI_SPEED_NOT_SELECTED
} sd_spi_speed_reason_t;

typedef struct {
    uint32_t baud_rate;
    sd_spi_speed_reason_t reason;
} sd_spi_speed_result_t;

static inline const char *sd_spi_speed_reason_name(sd_spi_speed_reason_t reason) {
    switch (reason) {
        case SD_SPI_SPEED_DEFAULT: return "within-default-speed";
        case SD_SPI_SPEED_HIGH: return "high-speed-confirmed";
        case SD_SPI_SPEED_NO_EXCHANGE: return "no-cmd6-exchange";
        case SD_SPI_SPEED_CHECK_IO: return "cmd6-check-failed";
        case SD_SPI_SPEED_SWITCH_IO: return "cmd6-switch-failed";
        case SD_SPI_SPEED_INVALID_STATUS: return "invalid-status-version";
        case SD_SPI_SPEED_UNSUPPORTED: return "high-speed-not-supported";
        case SD_SPI_SPEED_BUSY: return "high-speed-function-busy";
        case SD_SPI_SPEED_NOT_SELECTED: return "high-speed-not-selected";
    }
    return "unknown";
}

static inline sd_spi_speed_reason_t sd_spi_hs_status(const uint8_t status[64]) {
    // Group 1 support: bits 415:400; status version: bits 375:368.
    // Version 1 group 1 busy flags: bits 287:272. Version 0 has no busy flags.
    if (status[17] > 1u) return SD_SPI_SPEED_INVALID_STATUS;
    if (!(status[13] & 2u)) return SD_SPI_SPEED_UNSUPPORTED;
    if (status[17] == 1u && (status[29] & 2u)) return SD_SPI_SPEED_BUSY;
    return SD_SPI_SPEED_HIGH;
}

static inline sd_spi_speed_result_t sd_spi_negotiate_speed_diagnosed(
        uint32_t requested, sd_spi_switch_fn exchange, void *context) {
    sd_spi_speed_result_t result = {sd_spi_default_speed(requested), SD_SPI_SPEED_DEFAULT};
    if (requested <= 25000000u) return result;
    if (!exchange) { result.reason = SD_SPI_SPEED_NO_EXCHANGE; return result; }
    // Word alignment also makes the driver's DMA receive buffer suitable.
    union { uint32_t alignment; uint8_t bytes[64]; } status = {0};
    // Check group 1 function 1; other groups use 0xF (leave unchanged).
    if (!exchange(context, 0x00fffff1u, status.bytes)) {
        result.reason = SD_SPI_SPEED_CHECK_IO; return result;
    }
    result.reason = sd_spi_hs_status(status.bytes);
    if (result.reason != SD_SPI_SPEED_HIGH) return result;
    // Switch only after support and availability are confirmed. No unbounded retry.
    if (!exchange(context, 0x80fffff1u, status.bytes)) {
        result.reason = SD_SPI_SPEED_SWITCH_IO; return result;
    }
    result.reason = sd_spi_hs_status(status.bytes);
    if (result.reason != SD_SPI_SPEED_HIGH) return result;
    if ((status.bytes[16] & 0x0fu) != 1u) {
        result.reason = SD_SPI_SPEED_NOT_SELECTED; return result;
    }
    result.baud_rate = requested < 50000000u ? requested : 50000000u;
    return result;
}

static inline uint32_t sd_spi_negotiate_speed(uint32_t requested,
                                             sd_spi_switch_fn exchange, void *context) {
    return sd_spi_negotiate_speed_diagnosed(requested, exchange, context).baud_rate;
}
