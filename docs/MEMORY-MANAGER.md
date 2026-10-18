# SD image storage

`MemoryManager` owns the SPI hardware, FAT/exFAT filesystem, file operations and
a shared 6 KiB read buffer. Its API saves and reads one logical 128 x 256 RGB888
image without allocating another full framebuffer.

The class overview is in `src/sd/include/MemoryManagerClass.hpp`; its implementation
and private class-owned storage state are in `src/sd/MemoryManagerClass.cpp`.

## Wiring

| Reader | Pico GPIO | Physical pin |
| --- | --- | ---: |
| 3v3 | 3V3 OUT | 36 |
| CS | GP17 | 22 |
| MOSI | GP19 | 25 |
| CLK | GP18 | 24 |
| MISO | GP16 | 21 |
| GND | GND | 23 |

`src/config/include/storageConfig.hpp` defines these pins and the 4 MHz transfer clock.
The card needs an existing FAT/exFAT filesystem. The SPI driver claims two free
DMA channels; shutdown releases only those channels and its SPI resources.

## API

- `Initialize()` mounts the card; `Shutdown()` unmounts and releases hardware.
- `IsReady()` and `GetLastError()` report availability and operation failures.
- `SaveImage(name, pixels, bytes)` creates a new file, preserving existing files.
- `ReadImage(name, sink, context)` streams bounded fragments and checks the file.
- `RunReadWriteCheck(pixels, bytes)` saves an unused temporary file, compares
  every byte after reading it, then removes that test file.

Call storage methods from foreground code. An ownership guard rejects concurrent
or reentrant operations. Names are root filenames containing letters, digits,
underscores or hyphens, with an optional extension; paths are rejected.

Each image contains a 20-byte header with format version, dimensions, pixel size,
payload length and CRC32, followed by 98,304 bytes of RGB888 pixels in the upload
byte order. Reads verify the header, exact file size and payload checksum.
Sink data remains provisional until the complete read succeeds.

```cpp
#include "sd/include/MemoryManagerClass.hpp"

MemoryManager memory;
if (memory.Initialize()) {
    if (memory.SaveImage("FRAME.RGB", imagePixels, SOURCE_FRAME_SIZE_BYTES)) {
        memory.ReadImage("FRAME.RGB", consumeImageChunk, &consumerContext);
    }
    memory.Shutdown();
}
```
