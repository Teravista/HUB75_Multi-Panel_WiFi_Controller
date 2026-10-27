# SD storage and recovery

`MemoryManager` owns the SPI hardware, FAT/exFAT filesystem, file operations and a
shared 24 KiB read buffer. Its APIs store named photos, retained legacy media and
independent photo/video library items without allocating another framebuffer.

The class overview is in `src/sd/include/MemoryManagerClass.hpp`; its implementation and
private class-owned storage state are in `src/sd/MemoryManagerClass.cpp`.

## Wiring

| Reader | Pico GPIO | Physical pin |
| --- | --- | ---: |
| 3v3 | 3V3 OUT | 36 |
| CS | GP17 | 22 |
| MOSI | GP19 | 25 |
| CLK | GP18 | 24 |
| MISO | GP16 | 21 |
| GND | GND | 23 |

`src/config/include/storageConfig.hpp` defines these pins and requests a 48 MHz transfer
clock, subject to card-mode negotiation and hardware dividers. The card needs an
existing FAT/exFAT filesystem. The pinned SPI driver supports SDHC/SDXC cards; legacy
SDSC cards up to 2 GB are not supported. The SPI driver claims two free DMA channels;
shutdown releases only those channels and its SPI resources.

## API

- `Initialize()` enables monitoring and mounts the card; `Shutdown()` releases hardware
  and suspends retries until the next explicit initialization.
- `Update(nowMs)` advances recovery and returns `None`, `Unavailable` or `Ready`.
- `GetState()`, `GetStateName()`, `GetRetryInMs(nowMs)`, `GetStorageError()` and
  `GetStorageGeneration()` expose the current session and retry state.
- `IsReady()` and `GetLastError()` report availability and operation failures.
- `SaveImage(name, pixels, bytes)` creates a new file, preserving existing files.
- `ReadImage(name, sink, context)` streams bounded fragments and checks the file.
- `RunReadWriteCheck(pixels, bytes)` saves an unused temporary file, compares
  every byte after reading it, then removes that test file.

Call storage methods from foreground code. An ownership guard rejects concurrent or
reentrant operations. Names are root filenames containing letters, digits, underscores
or hyphens, with an optional extension; paths are rejected.

Named and retained photos use checked media headers; legacy 20-byte image headers remain
readable. Full-panel RGB888 contains 98,304 pixel bytes. Reads validate header, exact
size and payload checksum. See [metadata](MEDIA-METADATA.md) for the current layouts.
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

Completed raw TCP photos become independent library items before the connection overlay
is drawn. TCP videos are published to the video collection after SD verification.
Startup restores the saved photo/video selection or slideshow; unavailable content falls
back to retained legacy content or the flashed image. Radio initialization and reconnects
retain SD resources and playback state.

`MediaLibrary` coordinates selection and display settings. `MemoryManager` provides
checked file operations and bounded directory pages. See the [saved media
library](MEDIA-LIBRARY.md) for publication, settings, filenames and migration behavior.
`SaveLastImage`, `ReadLastImage` and `PhotoPersistence` remain available for retained
RGB888 compatibility.

## Availability lifecycle

| State | Behavior |
| --- | --- |
| `Unavailable` | Attempts a fresh mount every 15 seconds after failure |
| `Mounting` | Synchronously initializes SD hardware and mounts the existing FAT/exFAT volume |
| `Ready` | Serves storage operations and checks one physical sector every 15 seconds |
| `Recovering` | Discards failed-session handles and caches before entering `Unavailable` |

Six-pin SPI readers have no card-detect signal. A failed required transfer or the
read-only physical health check detects removal; cached reads alone cannot establish
that the card remains present. Health checks skip active save/verification transactions
so their shared scratch bytes remain intact. Initialization allows a 1 s SD command
wait; mounted commands use 250 ms, two retries and bounded lock/transfer waits.

Disk, readiness, timeout and invalid-object errors invalidate the session. Missing files,
invalid media, full cards and write protection remain operation failures when the card
is still usable. Failed-session cleanup performs no close/flush/delete writes against
a potentially reinserted or replacement card. Existing published media and settings
remain available for restoration.

Each successful mount increments the storage generation and clears read caches. Core 0
reloads the newly mounted card's library and selection; it never resumes an unfinished
upload from a previous session. The firmware neither formats cards nor restarts Wi-Fi
as part of SD recovery. Startup and failed mount attempts keep network services and
the built-in display content available.

Browser status and memory responses include `storageState`, `storageRetryMs`,
`storageGeneration` and `storageError`; these supplement existing readiness/usage fields.

## Per-photo metadata

Named and retained photos use the version-3 container with a per-photo media descriptor
and independent header/payload checksums. Existing version-1 named images and version-2
retained saves remain readable. The optional metadata output of `ReadImage` /
`ReadLastImage` is published only after successful validation; `SaveLastImage` also
accepts an explicit full-panel RGB888 photo descriptor. See [media
metadata](MEDIA-METADATA.md) for layouts and API details.

## Raw video transactions

`BeginMediaSave`, `AppendMediaBytes`, `StartMediaVerification` and repeated
`VerifyMediaSaveStep` calls save long raw clips with bounded RAM. Photo I/O is rejected
while that transaction owns the shared buffer. `AbortMediaSave` discards unpublished
content and retains the prior valid slot.

`OpenLastMedia` selects a valid photo/video generation; `ReadMediaFrame` streams one
checked frame and `CloseMediaPlayback` releases its file. A corrupt later frame excludes
its generation on the next open. MediaPlayer connects that storage boundary to the
shared framebuffer under render ownership. See [video upload and
playback](VIDEO-UPLOAD.md) for layouts and failure behavior.

## Compact and JPEG video

Video transactions also accept RGB565 and indexed baseline JPEG. RGB565 retains the raw
version-4 record layout with two bytes per pixel. JPEG uses version 5, validates the
complete saved payload and every decoded frame before publishing, and checks each
selected frame again during playback. A four-KiB bounded index cache avoids rereading
offsets for each frame. JPEG shares the existing 24 KiB scratch allocation between
compressed input and decoder workspace; no extra full framebuffer is allocated. See
[JPEG video](JPEG-VIDEO.md).

## SD throughput

The SD path uses a negotiated SPI clock, 24 KiB bulk I/O and flash-resident CRC32 lookup
tables. Files are published only after their required readback and integrity checks
complete.

### SPI clock

`StorageConfig::BaudRate` requests 48 MHz. Initialization uses a slow clock for card
setup. The SPI peripheral uses the system clock as its source; hardware dividers
determine the actual transfer rate.

The driver checks CMD6 High Speed support and availability, then confirms the switch
before allowing a rate above 25 MHz. Unsupported, busy, malformed or failed CMD6
exchanges keep Default Speed, capped at 25 MHz. Confirmed High Speed is capped at 50 MHz
and never exceeds the requested clock. Card initialization clears the previous
negotiation result. The mount log reports the actual SPI clock selected by the hardware.

If a card/reader/wiring setup becomes unreliable, lower `StorageConfig::BaudRate`. The
SPI bus rate is separate from the card's advertised transfer speed.

### Bulk reads and RAM

A shared 24 KiB scratch buffer holds bounded storage fragments without allocating
another full image. Photo chunks contain complete RGB888 pixels and whole-sector
multiples. FatFs manages the file header offset, partial sectors and cluster boundaries;
a bulk buffer does not make every underlying transfer one continuous SD command. No
raw-sector path bypasses the filesystem or its bounds checks. Consumers publish an image
only after the complete read and its checksums succeed.

The active playback handle has a separate 64 KiB cache (`PlaybackPrefetch::Bytes`),
split into two 32 KiB blocks. Reads reuse cached file ranges and use a logical cursor
for forward, reverse and random frame selection. Closing, replacing or remounting
playback clears cached bytes. Inspection and upload verification read directly from
FatFs. A speculative read failure cannot invalidate an already checked current frame;
a later required read reports any remaining storage error.

Raw playback alternates two 12 KiB halves of the shared scratch buffer. Bounded CRC
and pixel mapping work can run while SD DMA fills the other half. JPEG input can use
two additional 8 KiB slots to feed the shared decoder worker. Filesystem operations
remain on Core 0; worker callbacks consume copied encoded bytes and never access SD.

### CRC32 and DMA

CRC32 uses slicing-by-four lookup tables. The 4 KiB tables are compile-time constants
stored in flash. Polynomial, running state, final inversion and all stored checksums
remain compatible, including unaligned input and arbitrary fragment boundaries. Header,
payload and SD sector CRC checks remain enabled.

The SD read path completes active DMA before returning a previous-sector CRC error,
preventing retry from reusing a destination still owned by DMA. It also provides an
optional foreground work hook and timing counters for read pipelines. The playback pipeline installs the work hook only while a previously filled buffer
has pending CPU work. A hook must be bounded, must not re-enter storage and
must not access the current DMA target. Timing counters overlap; they must not be added
to infer elapsed read time. Storage work runs in foreground code.

### Diagnostics

`HUB75_VERBOSE_LOGS` defaults to OFF. It controls optional SD-library diagnostics and
the five-second startup pause used to attach a serial console. Enable it through CMake
when detailed card-mode negotiation logs are needed.
