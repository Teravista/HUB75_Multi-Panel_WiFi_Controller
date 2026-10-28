#pragma once

namespace StorageConfig {
// GPIO numbers; the physical Pico pins are listed in the storage guide.
constexpr unsigned Miso = 16;
constexpr unsigned ChipSelect = 17;
constexpr unsigned Clock = 18;
constexpr unsigned Mosi = 19;
// Requested clock; CMD6 must confirm High Speed before the driver exceeds 25 MHz.
// Hardware dividers may select a lower actual clock.
constexpr unsigned BaudRate = 48'000'000;
} // namespace StorageConfig
