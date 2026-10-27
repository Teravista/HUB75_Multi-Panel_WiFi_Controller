# Display and frame rates

The CPU graphics frame is RGB565: 128 x 256 x 2 bytes = 64 KiB. Flashed images and raw
TCP photo uploads use RGB888 input. Pixel conversion preserves the panel mapping and
wiring correction, with 5/6/5-bit CPU colour precision. Set `CPU_FRAME_RGB565=0` in the
CMake definitions to use the 128 KiB RGB888 CPU buffer instead. The driver has two 128
KiB scan buffers.

The driver expands RGB565 through compile-time gamma lookup tables when temporal
dithering is disabled. Foreground submission checks `hub75_ready_for_update()` before
reusing the writer buffer; swaps happen at full-scan boundaries. The first flashed frame
is submitted before the initial Wi-Fi connection.

PIO/DMA shifts a row at a time, and a Core 0 interrupt advances the next row and
bit-plane. Keeping that interrupt responsive matters even for static images. The scanner
and its literal pool run from SRAM, with the highest IRQ priority; the completion DMA
channel also has high priority. The IRQ performs no network, storage, logging or
allocation work. Scan timing follows the driver's configured row order, bit-plane
schedule and clock.

Three-word row records have four-byte alignment and explicit size/offset checks.

RGB565 CPU pixels use 16-bit (2-byte) values and only require two-byte alignment. The
RGB888 CPU-frame option stores each 24-bit colour in a 32-bit (4-byte) value and uses
128 KiB. RGB565 uses 64 KiB at 128 × 256 pixels. Driver scan buffers use 32-bit records
and four-byte alignment.

## Frame rates

Video uploads and saved metadata accept rates from 1–60 FPS, including rational rates
such as `60000/1001`. The global cap uses the lower of its configured value and the
saved rate; zero preserves the saved rate. Panel scan refresh and distinct video-frame
submission are separate counters. SD reads, decoding and rendering limit achievable new
frames, so a requested rate is not a throughput guarantee.
