# SD image storage

`MemoryManager` owns the SPI hardware, FAT/exFAT filesystem, file operations and
a shared 24 KiB read buffer. Its API saves and reads one logical 128 x 256 RGB888
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

## Automatic photo persistence

Each completed raw TCP photo is saved once, before drawing the connection icon.
Frame ownership excludes new uploads while the foreground reads the source, so
no second framebuffer is needed. A failed save keeps the live photo visible and
is not retried continuously for the same frame revision.

`SaveLastImage(source, context)` streams logical RGB888 pixels into alternating
reserved `LASTA.H75` and `LASTB.H75` files. `ReadLastImage(sink, context)` reads
the newest generation when it is a photo; a corrupt or incomplete replacement falls back to
the previous valid copy. Each retained photo has a 52-byte version-3 header, with
separate CRCs covering the descriptor/generation header and 98,304-byte payload. These files
are controller-owned and must not be used as arbitrary named-image destinations.

Startup queues the flashed image, then attempts to restore a verified saved
photo or first video frame before Wi-Fi connection begins. Missing or unusable storage leaves the
flashed image available. SD hardware is released for the first radio resource
claim and initialized once afterward, even if association is still pending.
Storage is mounted through explicit initialization.

The `PhotoPersistence` bridge connects MemoryManager to FrameBufferController.
FrameReceiver supplies ownership tokens and completed-frame revisions; TCP
callbacks only receive/map pixels. Filesystem work remains in foreground code.

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
