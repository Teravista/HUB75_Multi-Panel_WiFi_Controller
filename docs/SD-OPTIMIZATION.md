# SD throughput improvements

The SD path uses a faster negotiated SPI clock, 24 KiB bulk I/O and a
flash-resident CRC32 implementation. Photo uploads, saved-file layouts and
verification rules remain unchanged.

## SPI clock

`StorageConfig::BaudRate` requests 48 MHz. Initialization still runs at the
slow initialization clock. Before SPI starts, the platform selects the system
clock as the SPI peripheral source so the requested rate is not limited by the
original peripheral clock; hardware dividers determine the actual rate.

The driver checks CMD6 High Speed support and availability, then confirms the
switch before allowing a rate above 25 MHz. Unsupported, busy, malformed or
failed CMD6 exchanges keep Default Speed, capped at 25 MHz. Confirmed High
Speed is capped at 50 MHz and never exceeds the requested clock. Card
initialization clears the previous negotiation result. The mount log reports
the actual SPI clock selected by the hardware.

These limits do not guarantee electrical reliability at the requested rate.
If a card/reader/wiring setup becomes unreliable, lower `StorageConfig::BaudRate`.
The requested clock and the card's advertised transfer speeds are different
quantities; a measured on-device result is needed to establish throughput.

## Bulk reads and RAM

The single shared storage scratch buffer grows from 6 to 24 KiB: 18 KiB more
static RAM, with no extra full-image buffer. Photo chunks contain complete
RGB888 pixels and whole-sector multiples. FatFs manages the file header offset,
partial sectors and cluster boundaries; a bulk buffer does not make every
underlying transfer one continuous SD command. No raw-sector path bypasses the
filesystem or its bounds checks. Consumers still publish an image only after
the complete read and its checksums succeed.

## CRC32 and DMA

Slicing-by-four lookup tables replace the application CRC32 bit loop. The
4 KiB tables are compile-time constants stored in flash. Polynomial, running
state, final inversion and all stored checksums remain compatible, including
unaligned input and arbitrary fragment boundaries. Header, payload and SD
sector CRC checks remain enabled.

The SD read path completes active DMA before returning a previous-sector CRC
error, preventing retry from reusing a destination still owned by DMA. It also
provides an optional foreground work hook and timing counters for later read
pipelines. They are inactive unless a caller installs them. A hook must be
bounded, must not re-enter storage and must not access the current DMA target.
Timing counters overlap; they must not be added to infer elapsed read time.
This step does not move work to another CPU core.

## Diagnostics and device checks

`HUB75_VERBOSE_LOGS` defaults to OFF. It controls optional SD-library diagnostics
and the five-second startup pause used to attach a serial console. Enable it
through CMake when detailed card-mode negotiation logs are needed.

On the Pico, check the reported actual SPI clock, restore an existing photo,
upload a replacement and power-cycle to verify it. Missing/corrupt-card and
readback checks remain part of storage validation. Host tests cover negotiation
fallback, deferred-DMA ordering/error paths, CRC equivalence and real FatFs
photo save/read compatibility; they do not measure physical SD throughput.
