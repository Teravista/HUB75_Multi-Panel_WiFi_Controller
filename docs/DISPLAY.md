# Display and frame rates

The CPU graphics frame is RGB565: 128 x 256 x 2 bytes = 64 KiB. Flashed images and raw
TCP photo uploads use RGB888 input. Pixel conversion preserves the panel mapping and
wiring correction, with 5/6/5-bit CPU colour precision. Set `CPU_FRAME_RGB565=0` in the
CMake definitions to use the 128 KiB RGB888 CPU buffer instead. Both output modes
use prepared scan bits: two 128.75 KiB scan buffers for one output, or two
130 KiB scan buffers for two outputs, plus 2.75 KiB of gamma tables. The RGB888 CPU option needs an extra 64 KiB,
so its RAM budget must
also accommodate the playback cache and JPEG input slots; reduce
`PlaybackPrefetch::Bytes` when that combination exceeds available RAM.

The controller prepares gamma-corrected brightness-plane bits before scanning
when temporal dithering is disabled. Foreground submission checks `hub75_ready_for_update()` before
reusing the writer buffer; swaps happen at full-scan boundaries. The first flashed frame
is submitted before the initial Wi-Fi connection.

PIO/DMA shifts a row at a time, and a Core 0 interrupt advances the next row and
bit-plane. Keeping that interrupt responsive matters even for static images. The scanner
and its literal pool run from SRAM, with the highest IRQ priority; the completion DMA
channel also has high priority. The IRQ performs no network, storage, logging or
allocation work. Held source frames with unchanged network indicators reuse the
current scan allocation; only new media revisions or changed overlays rebuild
the brightness planes. Packing extracts groups of five planes with native 32-bit
operations, retaining ten-bit gamma and the existing scan layout. RGB565 uses a
dedicated typed loop with inlined gamma reads and plane extraction. On Pico builds,
`PACKED_PANEL_HOT_CODE_IN_RAM=1` places that loop in SRAM; zero retains flash
execution. RGB888/BGR888 keep the generic fallback. Both paths produce the same
scan words and allocate no additional image buffers. Native RGB565
gamma indices use an additional 768-byte lookup. JPEG output and original RGB565
playback reorder five/six-bit channels directly into the CPU framebuffer, preserving
the exact colours of the RGB888 expansion/repacking path.
Scan timing follows the driver's configured row order, bit-plane
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

The Summary shows a rolling average over the latest 30 seconds, using the available
shorter window at startup. `FrameRateTracker` samples cumulative completed scans and
distinct submitted video revisions independently. Reusing a held frame or redrawing
the Wi-Fi overlay does not increase video FPS. Counters remain cumulative across
clip reloads; clock and counter rollover use unsigned differences.

## RAM figures

Summary reports static allocations, live heap allocations and reserved core stacks
separately. Used RAM is their sum; free RAM is total controller SRAM minus that sum.
The figures include CPU/driver buffers and runtime allocations. Reserved stacks count
their full configured space, rather than measuring each stack's current usage.

## Rendering boundary

`PanelRenderer` maps logical image coordinates into the physical panel chain and
renders separate network indicators through that same mapping: a house for home
Wi-Fi at the bottom-right and an antenna for the hotspot at the bottom-left. Each ready interface
shows its own IP in a compact bitmap font with a text-sized black backing. An
unavailable interface shows a red icon without an IP label. A saved bottom strip
restores the underlying image when an address or connection state changes.
Callers mark a new source frame so that strip is captured from the new image.
Long address pairs use tighter character spacing to keep both labels separate.

Dots above the antenna represent associated hotspot devices. White means connected;
green indicates uploads, page/library access or controller actions and remains visible
for 1.5 seconds after the latest meaningful traffic. Routine status, memory and upload
progress polling leaves an idle device white. Disconnected devices lose their dot;
remaining devices keep their positions. Associations refresh every 500 ms and failed
radio queries retain the last confirmed snapshot. Home-network traffic does not light
hotspot dots. The dot strip and connection/activity tracking use
`WIFI_HOTSPOT_MAX_CLIENTS` in `src/config/defines.h`.

`RenderPipeline` separates synchronous frame preparation from driver submission.
It claims the CPU framebuffer once, lets preparation overlap the preceding scan,
and retains the prepared pixels until the scanner accepts them. Pending frames
are not prepared twice. A 250 ms scanner timeout releases ownership and waits for
scanner recovery before preparing again; the pipeline does not reset DMA/PIO.

`graphicsTypes.hpp` selects the configured CPU graphics type. `cpuFrame.hpp`
declares packing, byte-order conversion and RGB read/write helpers; their
implementations remain in `cpuFrame.cpp`.

## Single and parallel panel chains

`DOUBLE_PIO_DRIVE` in `src/config/defines.h` selects one four-panel chain (`0`)
or two equal two-panel chains (`1`). Both use prepared output bits, explicit PIO
row-completion fences and overlapped pixel loading/display timing. A single output
uses 103 words per row/plane and shifts 512 columns; two outputs use 52 words each
and shift 256 columns per output. Single-output mode claims neither PIO1 nor the
secondary output pins. The `dual-panel-pio` preset forces two outputs.
Normal SD playback, uploads,
web/API services and both Wi-Fi modes remain available. Logical images stay
128 × 256; the CPU graphics surface retains its full 512 × 64 physical layout.

```powershell
cmake --preset dual-panel-pio
cmake --build --preset dual-panel-pio
```

The firmware is `build/dual-panel-pio/hub75.uf2`. Both pixel streams run even
when only one output is connected. Periodic status includes `pio_outputs=2`
and `chain_width=256`; display FPS counts complete scans of both streams.

| Signal | First chain | Second chain |
| --- | --- | --- |
| R0 / G0 / B0 | GP0 / GP1 / GP2 | GP20 / GP21 / GP22 |
| R1 / G1 / B1 | GP3 / GP4 / GP5 | GP26 / GP27 / GP28 |
| Clock | GP11 | GP14 |
| A / B / C / D / E | GP6 / GP7 / GP8 / GP9 / GP10 | Shared with first chain |
| Latch / output enable | GP12 / GP13 | Shared with first chain |
| Ground | Common ground | Common ground |

With two outputs, each chain receives 256 physical columns: indices 0–1 from
the existing mapping go to the first output and indices 2–3 to the second. One
output carries all four panel indices in the existing 512-column order. Image rotation and color
correction remain unchanged. `PACKED_PANEL_DUAL_FIRST_OUTPUT_ROTATE_180=1` in
`PackedPanelFrameClass.hpp` rotates the complete first output in dual-chain mode
to match the portrait mounting, including the order of its two panels. Set it to
`0` to retain that chain's native orientation. The second output keeps its normal
row and column order.
This final packing adjustment applies to live overlays, original JPEG/raw media
and existing corrected RGB copies, without rewriting SD files. Single-output
mode is unaffected. Two DMA streams borrow different halves of each
prepared brightness-plane row. Five six-bit column pairs share each 32-bit
word, with two unused high bits. The foreground prepares those output bits once
per submitted frame; PIO no longer extracts RGB bits during every scan. At the
current geometry, a single-output row is 103 words and each dual-output row is
52 words per plane, making a scan allocation 128.75 KiB or 130 KiB respectively. The 2.75 KiB gamma tables retain all ten brightness bits and cache native RGB565 channel indices. Preparation
uses a five-pair workspace rather than a third image buffer. SD media and CPU
frame formats are unchanged; gamma conversion and pin-gap ordering are internal
to scan preparation. Each row clocks exactly its physical column count; unused
fields in the last packed word are discarded without clocking extra black pixels. Both data loops use ten PIO cycles per column, split into
six low and four high clock cycles, with explicit data-settling time before
the rising clock. `PACKED_PANEL_DATA_CLOCK_DIVIDER` in
`PackedPanelFrameClass.hpp` scales only pixel shifting; row brightness periods
retain their existing clock and duration.
Both finite PIO loops must explicitly signal their final pixel clock, and the
previous lit/dark period must finish, before another shared latch/OE record starts.
DMA completion alone cannot trigger a latch. A hardware notification after the
latch pulse and settling delay authorizes loading the following row while the
current output latch remains illuminated. Only small pending/active timing records
are separate; the CPU image buffer and color precision remain unchanged.

Frame swaps release a scan buffer after its final row is latched, when no later
transfer reads it. Completed-scan FPS advances after that final row's full display
period, independently of preparation. Brightness and BCM dwell retain the normal
settings. The second data program inserts a pin gap around GP23–25
to preserve the Pico W radio pins; the first program matches its cycle count.
The secondary output reserves PIO1, so the radio claims another block and cannot
share the sparse output span. Two data state machines and one row state machine
execute; the other three PIO1 state machines are reserved, not running.

In dual-output mode, a single four-panel cable receives only 256 columns per row,
so that wiring cannot show a valid full four-panel image in parallel mode. A
two-panel chain on the first output shows the corresponding half; the second
output does not require attached panels to complete its PIO/DMA work.

Set the guarded numeric flag to 0 and use `web-upload` for the optimized single
512-column chain. Its firmware is `build/web-upload/hub75.uf2`. Startup reports
the active output count and columns per output. CMake tracks the header toggle;
the dedicated preset forces parallel mode.
