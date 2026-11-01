# Media metadata

`MediaMetadata` describes logical pixels before panel mapping: type, dimensions, format,
frame count and rational FPS. It owns no pixels. It describes RGB565 and indexed
JPEG photos and videos. Raw TCP photo uploads use full-panel RGB565.

| Field | Photo | Video descriptor |
| --- | --- | --- |
| Type | `Photo` (1) | `Video` (2) |
| Format | `RGB565` (2), `JPEG` (3) | `RGB565` (2), `JPEG` (3) |
| Dimensions | Within 128 × 256 for library photos | Positive width and height |
| Frames | 1 | Positive frame count |
| FPS | 0/1 | Positive numerator and denominator |
| Payload bytes | Raw frame size or indexed JPEG size | Raw frame size × frames or indexed JPEG size |

A one-frame video still needs a positive frame rate. Fractional rates such as 30000/1001
do not require floating point. Unknown types/formats, empty fields, inconsistent photo
timing and payload overflow are rejected. The descriptor describes all library formats.
Legacy named/retained photo APIs and raw TCP photo uploads accept full-panel RGB565;
library photo APIs also accept RGB565 and JPEG. Video uses a separate TCP listener and
checked record container; see [video upload and playback](VIDEO-UPLOAD.md).

## API

```cpp
MediaMetadata photo = MediaMetadata::Photo(128, 256);
memory.SaveLastImage(photo, producePixels, &source);

MediaMetadata restored;
memory.ReadLastImage(consumePixels, &destination, &restored);
memory.ReadImage("FRAME.RGB", consumePixels, &destination, &restored);
```

Check boolean results. Metadata outputs change only after a successful complete read.
Pixel fragments remain provisional until CRC and close succeed. Existing calls without
metadata still work. `FrameBufferController::GetFrameMetadata()` returns the current
completed descriptor under render ownership. The explicit `BeginFrame(bytes, metadata)`
overload validates the photo before claiming it; metadata is published together with a
completed frame.

Raw TCP photo uploads contain exactly 65,536 little-endian RGB565 bytes and receive their descriptor
automatically. Do not prepend a descriptor to this endpoint.

## Descriptor layout, versions 1 and 2

The fixed 32-byte encoding is independent of C++ padding. Multibyte values are
little-endian. Never transmit `sizeof(MediaMetadata)` structure bytes.

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | `H75M` |
| 4 | 1 | Version 1 for raw, version 2 for indexed JPEG |
| 5 | 1 | Media type |
| 6 | 1 | Pixel format |
| 7 | 1 | Bit 0: video ping-pong; other bits reserved zero |
| 8 | 2 | Width |
| 10 | 2 | Height |
| 12 | 4 | Frame count |
| 16 | 4 | FPS numerator |
| 20 | 4 | FPS denominator |
| 24 | 8 | Calculated raw payload length or exact indexed JPEG size |

`MediaMetadataCodec::EncodeMediaDescriptor` validates before writing.
`DecodeMediaDescriptor` checks version, reserved flag bits and calculated length before
publishing its output. Neither operation reads image pixels.

## Photo container, version 3

Named images and alternating `LASTA.H75` / `LASTB.H75` saves use a 52-byte header
followed by the RGB565 photo payload (65,588 bytes total).

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | `H75L` |
| 4 | 1 | Container version 3 |
| 5 | 1 | Reserved zero |
| 6 | 2 | Header size 52 |
| 8 | 4 | Generation; zero for named files |
| 12 | 32 | Media descriptor |
| 44 | 4 | Pixel payload CRC32 |
| 48 | 4 | CRC32 of header bytes 0–47 |

CRC-32/ISO-HDLC uses initial state 0xffffffff, reflected polynomial 0xedb88320 and final
inversion. Header validation protects metadata and generation before any payload reaches
the consumer; payload CRC is checked during streaming.

Descriptor-free photo containers are unsupported. Named and retained photos use
version 3 with an explicit format and payload size.

## Raw video

Video descriptors permit ping-pong looping; photos must leave the flag false. The video
receiver accepts panel-sized or smaller RGB565 or indexed JPEG at 1–60 FPS and a
total stored file below 4 GiB. Legacy photo APIs remain single-frame RGB565; independent
library photos also support RGB565 and JPEG. See the [video guide](VIDEO-UPLOAD.md) for
the separate request and version-4 SD layout.

RGB565 uses little-endian R5G6B5 words. JPEG has no fixed raw frame stride;
`encodedBytes` describes its index plus records and `FrameBytes()` returns zero. See
[JPEG layout and bounded decoder memory](JPEG-VIDEO.md).

## Library containers

Independent photo/video items extend the common checked media header with names, photo
thumbnails or a video duration setting. See [saved media library](MEDIA-LIBRARY.md) for
container versions, pagination and persistent selections.

## JPEG encoder quality, descriptor version 3

JPEG descriptors can retain their encoder quality in byte 7, bits 1–7; bit 0 remains the
ping-pong flag. Quality 1–95 is recorded with each item. Zero means unknown, including
older JPEG saves, and encoding unknown quality retains version 2. Raw descriptors remain
version 1 and reject nonzero JPEG quality. The descriptor stays 32 bytes, and existing
container/header CRCs also protect the saved quality field. Pixel data is unaffected by
this metadata extension.

## Placement, descriptor version 4

`width` and `height` describe the stored rectangle. `offsetX` and `offsetY` locate its
top-left corner within the panel; both `AutoOffset` values select centering. Mixed
automatic/explicit offsets and rectangles extending beyond the panel are rejected.
Payload size uses the rectangle, excluding generated black borders.

Version 4 retains the 32-byte layout and packs placement into the size words:

| Offset | Encoding |
| ---: | --- |
| 8 | `(width - 1) \| (left << 7)`; bits 14–15 reserved zero |
| 10 | `(height - 1) \| (top << 8)` |

Width is 1–128, height 1–256, and offsets must keep the rectangle inside the panel. Byte
7 retains ping-pong and JPEG quality. Earlier descriptor versions use ordinary
dimensions with centered placement; centered browser uploads keep those compatible
versions. The C++ codec uses version 4 for explicit offsets. Container versions and
their existing header/payload checksums are unchanged.

## Prepared RGB copy extension

New RGB/JPEG library photos/videos retain their original descriptor and payload.
Raw frame CRCs and JPEG encoded indexes/records/checksums keep their existing layout. The full library header additionally contains a 192-byte `H75C`
version-1 descriptor at `headerBytes - 256`. The descriptor has its own CRC32 and
is covered by the existing final library-header CRC.

| Descriptor offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | `H75C` |
| 4 | 1 | Extension version 1 |
| 5 | 1 | Correction flags: bit 0 color order, bit 1 panel layout; both set |
| 6 | 1 | Native CPU pixel bytes: 2 |
| 7 | 1 | Number of packed physical rectangles, 1–18 |
| 8 | 4 | Current wiring/CPU-format signature |
| 12 | 4 | Original logical payload CRC32 |
| 16 | 4 | Preview RGB565 byte checksum |
| 20 | 4 | Native bytes per frame, excluding its CRC |
| 24 | 8 | Absolute preview file offset |
| 32 | 8 | Absolute native-frame section offset |
| 40 | 144 | Up to eighteen rectangles: uint16 x, y, width, height |
| 184 | 4 | Reserved zero |
| 188 | 4 | CRC32 of descriptor bytes 0–187 |

All integers are little-endian. The preview begins at the next 512-byte boundary
after the original records and contains 32 × 64 little-endian RGB565 pixels.
Native frames follow immediately, each consisting of the listed rectangles in
order, with row-major CPU pixels inside each rectangle, then a four-byte frame
CRC32. Native pixels use the graphics framebuffer's RGB565 byte order. Consumers must validate the layout
signature before using these bytes directly. Their encoding is not the original
RGB565 wire format.

Regions must be nonoverlapping, fit the physical framebuffer and cover exactly
the original image's pixel count. File size must match the complete layout.
Signature mismatch falls back to the unchanged logical original; original-only
files have no extension. Older firmware without extension support rejects the
larger container. Retained non-library containers do not use this extension.

JPEG uses a preview-only `H75C` version-2 descriptor at the same location, protected
by the same descriptor/header CRCs. Flags, native pixel bytes, region count, wiring
signature, native frame bytes, native offset and bytes 40–187 are zero. Only the
source CRC, preview CRC and aligned preview offset are populated. The file ends
exactly after the 4096-byte preview. There is no per-frame derived section.

Version-1 JPEG containers with native rectangles remain readable for preview,
but playback always uses the encoded original.

Only format IDs 2 (RGB565) and 3 (JPEG) are supported. Other stored format IDs
are unavailable and can be deleted from the library. Re-upload those source files
to use them with this firmware. Format IDs are never reassigned.
