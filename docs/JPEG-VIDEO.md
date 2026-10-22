# JPEG and compact raw video

The Python uploader supports `--pixel-format rgb565`, `rgb888`, or `jpeg`.
New conversions default to RGB565; JPEG uses baseline 4:2:0 at quality 85 by
default (`--jpeg-quality 1..95`). Prepared `.h75v` files retain their encoding.
MP4 decoding, resizing and central cropping run on the PC; the Pico decodes one
independent JPEG frame at a time. Audio is omitted.

```powershell
python utilityScripts/sendVideoOverTCP.py clip.mp4 --ip 192.168.1.48 --pixel-format jpeg --jpeg-quality 85 --fps 30 --seconds 10
python utilityScripts/sendVideoOverTCP.py clip.mp4 --prepare-only --output clip.h75v --pixel-format rgb565
```

RGB565 stores 65,536 bytes per 128 × 256 frame; RGB888 stores 98,304 bytes.
JPEG size varies with scene and quality and is lossy. Each encoded JPEG is
limited to 128 KiB; dimensions must match the panel and the complete SD file
must remain below 4 GiB. FPS is requested timing, not a guarantee of decode speed.
Progressive JPEG and unsupported dimensions are rejected.

## Payload and saved layout

The [32-byte descriptor](MEDIA-METADATA.md) uses format 3 and version 2 for JPEG;
bytes 24–31 carry the exact index-plus-record payload length. All integers below
are little-endian. Raw RGB888/RGB565 use descriptor version 1.

For N frames the payload starts with N twelve-byte entries:

| Field | Bytes | Meaning |
| --- | ---: | --- |
| Record offset | 4 | Relative to payload start |
| JPEG length | 4 | Encoded bytes, excluding record header |
| Entry CRC | 4 | CRC32 of offset, length and zero-based frame number |

Records follow contiguously after the index. Each contains JPEG length (uint32),
encoded CRC32 (uint32), and exactly that many JPEG bytes. Aliases, gaps, out-of-range
lengths and trailing records are rejected during save verification. A complete
JPEG end marker and successful baseline decoding are required as well as CRCs.

The saved slot has the same 64-byte header as raw video, with container version 5,
raw frame size zero, index entry size 12 at offset 48 and payload offset 64.
The payload is stored unchanged; the header retains the expected payload CRC and
its own CRC. `H75P` stays invalid until full SD readback and all frame checks pass.
The previous `LASTA.H75` / `LASTB.H75` generation is retained while saving.

## Bounded playback

Startup checks the selected header and first frame. Later frames validate their
record and encoded CRC before publication; a failed frame rejects that slot and
allows the preceding usable generation to load. Normal and ping-pong playback
seek directly to independent frames. RGB888/RGB565 and older photo slots remain
readable without conversion.

TJpgDec uses the existing 24 KiB storage buffer as 12 KiB compressed input and
12 KiB aligned decoder workspace. MCU tiles are delivered as provisional RGB888
row fragments directly to the existing CPU framebuffer; no second complete
RGB888 frame is allocated. A separate four-KiB cache retains at most 512 checked
index entries, and adjacent forward records skip redundant seeks. Storage access
and decoding run synchronously in foreground; core scheduling is a later change.

The decoder uses `-O3` locally, fast entropy lookup and chroma reuse. Its original
copyright and license remain in the vendored files; local changes are noted in
[third-party notices](../THIRD_PARTY_NOTICES.md).

To check on hardware, upload each format, wait for **Saved and verified**,
power-cycle, and confirm restoration, colors and ping-pong order. A failed upload
must retain the preceding saved media. Host checks cannot establish electrical
stability or guarantee a particular frame rate.
