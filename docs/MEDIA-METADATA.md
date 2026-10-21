# Media metadata

`MediaMetadata` describes logical pixels before panel mapping: type, dimensions,
format, frame count and rational FPS. It owns no pixels. This step keeps existing
photo uploads unchanged and prepares a shared description for video support.

| Field | Photo | Video descriptor |
| --- | --- | --- |
| Type | `Photo` (1) | `Video` (2) |
| Format | `RGB888` (1) | `RGB888` (1) |
| Dimensions | 128 × 256 for current photo APIs | Positive width and height |
| Frames | 1 | Positive frame count |
| FPS | 0/1 | Positive numerator and denominator |
| Payload bytes | 98,304 | width × height × 3 × frames |

A one-frame video still needs a positive frame rate. Fractional rates such as
30000/1001 do not require floating point. Unknown types/formats, empty fields,
inconsistent photo timing and payload overflow are rejected. The descriptor
can describe video; photo upload/storage APIs still accept only a full-panel
RGB888 photo. This commit does not add a video listener or playback.

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

## Descriptor layout, version 1

The fixed 32-byte encoding is independent of C++ padding. Multibyte values are
little-endian. Never transmit `sizeof(MediaMetadata)` structure bytes.

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | `H75M` |
| 4 | 1 | Version 1 |
| 5 | 1 | Media type |
| 6 | 1 | Pixel format |
| 7 | 1 | Reserved zero |
| 8 | 2 | Width |
| 10 | 2 | Height |
| 12 | 4 | Frame count |
| 16 | 4 | FPS numerator |
| 20 | 4 | FPS denominator |
| 24 | 8 | Calculated payload length |

`MediaMetadataCodec::EncodeMediaDescriptor` validates before writing.
`DecodeMediaDescriptor` checks version, reserved fields and calculated length
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
including counter rollover. New uploads write version 3; no card conversion is
needed. Earlier firmware cannot read the new version-3 files after a downgrade.
