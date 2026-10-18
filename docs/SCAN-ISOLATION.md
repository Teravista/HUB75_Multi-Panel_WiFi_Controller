# Display frame buffers and scan stability

The CPU graphics frame is RGB565: 128 x 256 x 2 bytes = 64 KiB. RGB888 remains
the input format for flashed images and TCP uploads. Pixel conversion preserves
the panel mapping and wiring correction, with 5/6/5-bit CPU colour precision.
Set `CPU_FRAME_RGB565=0` in the CMake definitions to use the 128 KiB RGB888 CPU
buffer instead. Driver scan buffers remain unchanged at two 128 KiB buffers.

The driver expands RGB565 through compile-time gamma lookup tables when temporal
dithering is disabled. Foreground submission checks `hub75_ready_for_update()`
before reusing the writer buffer; swaps still happen at full-scan boundaries.
The first flashed frame is submitted before the initial Wi-Fi connection.

PIO/DMA shifts a row at a time, and a Core 0 interrupt advances the next row and
bit-plane. Keeping that interrupt responsive matters even for static images.
The scanner and its literal pool run from SRAM, with the highest IRQ priority;
the completion DMA channel also has high priority. The IRQ performs no network,
storage, logging or allocation work. Row order, bit-plane timing and the scan
clock are unchanged.

Three-word row records have four-byte alignment and explicit size/offset checks.

RGB565 CPU pixels use 16-bit (2-byte) values and only require two-byte
alignment. The previous RGB888 graphics buffer stored each 24-bit colour
in a 32-bit (4-byte) value. This smaller pixel size halves the CPU frame
buffer from 128 KiB to 64 KiB at 128 x 256 pixels. The two driver scan
buffers retain their 32-bit pixels and four-byte alignment.
