# Media metadata

`MediaMetadata` describes logical pixels before panel mapping: type, dimensions,
format, frame count and rational FPS. It owns no pixels. It describes RGB888/RGB565 and indexed JPEG photos and videos
while preserving the original raw TCP photo upload format.

| Field | Photo | Video descriptor |
| --- | --- | --- |
| Type | `Photo` (1) | `Video` (2) |
| Format | `RGB888` (1), `RGB565` (2), `JPEG` (3) | `RGB888` (1), `RGB565` (2), `JPEG` (3) |
| Dimensions | 128 × 256 for current photo APIs | Positive width and height |
| Frames | 1 | Positive frame count |
| FPS | 0/1 | Positive numerator and denominator |
| Payload bytes | Raw frame size or indexed JPEG size | Raw frame size × frames or indexed JPEG size |

A one-frame video still needs a positive frame rate. Fractional rates such as
30000/1001 do not require floating point. Unknown types/formats, empty fields,
inconsistent photo timing and payload overflow are rejected. The descriptor
describes all library formats. Legacy named/retained photo APIs and raw TCP photo
uploads accept full-panel RGB888; library photo APIs also accept RGB565 and JPEG.
Video uses a separate TCP listener and checked record container; see
[video upload and playback](VIDEO-UPLOAD.md).

## API

```cpp
MediaMetadata photo = MediaMetadata::Photo(128, 256);
memory.SaveLastImage(photo, producePixels, &source);

MediaMetadata restored;
memory.ReadLastImage(consumePixels, &destination, &restored);
memory.ReadImage("FRAME.RGB", consumePixels, &destination, &restored);
```

Check boolean results. Metadata outputs change only after a successful complete
read. Pixel fragments remain provisional until CRC and close succeed. Existing
calls without metadata still work. `FrameBufferController::GetFrameMetadata()`
returns the current completed descriptor under render ownership. The explicit
`BeginFrame(bytes, metadata)` overload validates the photo before claiming it;
metadata is published together with a completed frame.

TCP photo uploads still contain only the original 98,304 RGB bytes and receive
their descriptor automatically. Do not prepend a descriptor to this endpoint.

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
`DecodeMediaDescriptor` checks version, reserved flag bits and calculated length
before publishing its output. Neither operation reads image pixels.

## Photo container, version 3

Named images and alternating `LASTA.H75` / `LASTB.H75` saves now use a 52-byte
header followed by the unchanged RGB888 photo payload (98,356 bytes total).

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

CRC-32/ISO-HDLC uses initial state 0xffffffff, reflected polynomial 0xedb88320
and final inversion. Header validation protects metadata and generation before
any payload reaches the consumer; payload CRC is checked during streaming.

Legacy `H75I` version-1 named files and `H75L` version-2 alternating saves still
load. The reader supplies their known photo/RGB888/one-frame/FPS-0/1 metadata
without rewriting them. Mixed old/new saved slots retain generation ordering,
including counter rollover. Legacy named/retained saves write version 3; no card conversion is
needed. Earlier firmware cannot read the new version-3 files after a downgrade.

## Raw video

Video descriptors permit ping-pong looping; photos must leave the flag false.
The video receiver accepts full-panel RGB888, RGB565 or indexed JPEG at 1–60 FPS
and a total stored file below 4 GiB. Legacy photo APIs remain single-frame RGB888;
independent library photos also support RGB565 and JPEG. See the
[video guide](VIDEO-UPLOAD.md) for the separate request and version-4 SD layout.

RGB565 uses little-endian R5G6B5 words. JPEG has no fixed raw frame stride;
`encodedBytes` describes its index plus records and `FrameBytes()` returns zero.
See [JPEG layout and bounded decoder memory](JPEG-VIDEO.md).

## Library containers

Independent photo/video items extend the common checked media header with names,
photo thumbnails or a video duration setting. See [saved media library](PHOTO-LIBRARY.md)
for container versions, pagination and persistent selections.

## JPEG encoder quality, descriptor version 3

JPEG descriptors can retain their encoder quality in byte 7, bits 1–7; bit 0
remains the ping-pong flag. Quality 1–95 is recorded with each item. Zero means
unknown, including older JPEG saves, and encoding unknown quality retains
version 2. Raw descriptors remain version 1 and reject nonzero JPEG quality.
The descriptor stays 32 bytes, and existing container/header CRCs also protect
the saved quality field. Pixel data is unaffected by this metadata extension.
