# SD image storage

`MemoryManager` owns the SPI hardware, FAT/exFAT filesystem, file operations and
a shared 24 KiB read buffer. Its APIs store named photos, retained legacy media
and independent photo/video library items without allocating another framebuffer.

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

`src/config/include/storageConfig.hpp` defines these pins and requests a 48 MHz
transfer clock, subject to card-mode negotiation and hardware dividers.
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

## Saved library and automatic persistence

Completed raw TCP photos become independent library items before the connection
overlay is drawn. TCP videos are published to the video collection after SD
verification. Startup restores the saved photo/video selection or slideshow;
unavailable content falls back to retained legacy content or the flashed image.
The first radio setup releases SD resources and explicitly mounts storage again.

`MediaLibrary` coordinates selection and display settings. `MemoryManager`
provides checked file operations and bounded directory pages. See the
[saved media library](PHOTO-LIBRARY.md) for publication, settings, filenames and
migration behavior. `SaveLastImage`, `ReadLastImage` and `PhotoPersistence` remain
available for retained RGB888 compatibility.

## Per-photo metadata

New named and retained photos use the version-3 container with a per-photo
media descriptor and independent header/payload checksums. Existing version-1
named images and version-2 retained saves remain readable. The optional metadata
output of `ReadImage` / `ReadLastImage` is published only after successful
validation; `SaveLastImage` also accepts an explicit full-panel RGB888 photo
descriptor. See [media metadata](MEDIA-METADATA.md) for layouts and API details.

## SD throughput

Storage uses one 24 KiB scratch buffer, larger bulk photo reads and compatible table-based CRC32. The driver negotiates its
requested clock before exceeding Default Speed. See [SD throughput](SD-OPTIMIZATION.md)
for card fallback, DMA behavior, RAM cost and diagnostic settings.

## Raw video transactions

`BeginMediaSave`, `AppendMediaBytes`, `StartMediaVerification` and repeated
`VerifyMediaSaveStep` calls save long raw clips with bounded RAM. Photo I/O is
rejected while that transaction owns the shared buffer. `AbortMediaSave`
discards unpublished content and retains the prior valid slot.

`OpenLastMedia` selects a valid photo/video generation; `ReadMediaFrame` streams
one checked frame and `CloseMediaPlayback` releases its file. A corrupt later
frame excludes its generation on the next open. MediaPlayer connects that
storage boundary to the shared framebuffer under render ownership. See
[video upload and playback](VIDEO-UPLOAD.md) for layouts and failure behavior.

## Compact and JPEG video

Video transactions also accept RGB565 and indexed baseline JPEG. RGB565 retains
the raw version-4 record layout with two bytes per pixel. JPEG uses version 5,
validates the complete saved payload and every decoded frame before publishing,
and checks each selected frame again during playback. A four-KiB bounded index
cache avoids rereading offsets for each frame. JPEG shares the existing 24 KiB
scratch allocation between compressed input and decoder workspace; no extra
full framebuffer is allocated. See [JPEG video](JPEG-VIDEO.md).
