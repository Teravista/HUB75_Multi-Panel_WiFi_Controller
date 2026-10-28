# Video upload and SD playback

Python prepares RGB565, RGB888 or indexed JPEG frames and streams them to TCP port 5002.
Browser conversion uses HTTP port 80. Both paths wait for verified SD publication and
select the saved library item; playback reads one checked frame at a time. Stored
dimensions and placement follow the per-item descriptor.

## Python use

Install `utilityScripts/requirements.txt`, then run:

```powershell
python utilityScripts/sendVideoOverTCP.py "clip.mp4" --ip 192.168.1.48 --fps 30 --seconds 10
python utilityScripts/sendVideoOverTCP.py "clip.mp4" --prepare-only --output clip.h75v --ping-pong
python utilityScripts/sendVideoOverTCP.py "clip.h75v" --ip 192.168.1.48
python utilityScripts/sendVideoOverTCP.py "clip.mp4" --ip 192.168.1.48 --pixel-format jpeg --jpeg-quality 85
```

At the top of the script, configure `DEFAULT_VIDEO_PATH`, `DEFAULT_PICO_IP`,
`DEFAULT_VIDEO_NAME`, `DEFAULT_FPS`, `MAX_VIDEO_SECONDS`, `PING_PONG`,
`PIXEL_FORMAT`, `SIZING_MODE` and `JPEG_QUALITY`.
Blank IP uses `.env` PICO_IP. A configured path lets you run the script without a
positional argument. Relative default paths resolve from the repository; explicit command-line
paths resolve from the current directory. A blank `DEFAULT_VIDEO_NAME` uses the source filename;
`--name "Forest walk"` overrides it. The FPS range is 1–60 and accepts rational values such as
30000/1001. Duration limits conversion to the shorter of that setting and the source.
The default is RGB565, 30 FPS, at most 600 seconds and ordinary looping. JPEG quality defaults to
85. `--ping-pong` / `--no-ping-pong` override loop mode; prepared files otherwise keep
their stored mode.

FFmpeg is resolved from `--ffmpeg`, PATH or `imageio-ffmpeg`. `--sizing crop` scales to
cover the panel and crops centrally. `--sizing fit` saves only the fitted rectangle;
playback generates the black borders without reading border pixels from SD. Audio is omitted. Preparation holds one raw frame at a time
and writes a temporary disk-backed file. A `.h75v` prepared file is a 36-byte request
plus the selected raw or indexed JPEG payload; it is not an MP4 or the Pico's SD
container. Do not copy it onto the card as a saved-media slot.

## Bounded TCP protocol

The normal request is a 32-byte [media descriptor](MEDIA-METADATA.md) followed by
little-endian payload CRC32. Magic `H75V` opts into 6144-byte chunks; `H75M` retains
1536-byte chunks. The descriptor otherwise uses canonical `H75M` fields and Video type.
Raw descriptors use version 1; JPEG uses versions 2/3, with version 4 for explicit
placement. See [metadata](MEDIA-METADATA.md) for quality flags. The checksum covers the
complete incoming payload.

Named Python uploads use magic `H75N` for bulk chunks or `H75n` for legacy-sized chunks.
After the 36-byte request, the Pico replies `NAME 64\n`. Send a 64-byte NUL-terminated,
zero-padded ASCII name, then wait for `READY 6144\n` or `READY\n`. The name is saved with
the library item; legacy `H75V`/`H75M` requests remain supported.

1. Send the descriptor and optional name; wait for the negotiated `READY` reply.
2. Send one negotiated chunk and wait for `NEXT\n` before sending another.
3. After the last payload chunk, expect `VERIFY\n`.
4. Wait for `SAVED\n`; an `ERR ...\n` response is failure.

Unnamed bulk requests can retry an explicit metadata rejection with the legacy handshake,
before sending pixels. Named requests require the name handshake and never silently discard
the requested name. It does not replay an uncertain payload transfer. A
separate verification timeout allows long SD readback. A transport inactivity deadline
cancels stalled clients; verification updates its progress while local work advances.

TCP callbacks only copy bounded data and handle replies. Foreground updates append to
MemoryManager and advance readback outside the lwIP lock. The receive buffer is 6 KiB,
independent of clip length. Exclusive frame ownership pauses saved-media updates during
reception/verification; panel scanning retains the last submitted driver buffer. Failure
or cancellation releases ownership and requests restoration of the selected usable
library content.

## Raw-video SD container, version 4 (RGB888 / RGB565)

The legacy `BeginMediaSave` API uses alternating `LASTA.H75` / `LASTB.H75` generations
and retains the previous valid generation during replacement. A partial upload begins
with invalid `H75P` magic and cannot boot as saved media. The final checked header is
installed only after complete SD readback succeeds. The FAT32-compatible total file size
must fit below 4 GiB.

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | `H75L` |
| 4 | 1 | Container version 4 |
| 5 | 1 | Reserved zero |
| 6 | 2 | Header size 64 |
| 8 | 4 | Generation |
| 12 | 32 | Media descriptor |
| 44 | 4 | Frame pixel byte count |
| 48 | 4 | Stored record stride: frame bytes + 4 |
| 52 | 4 | First record offset: 64 |
| 56 | 4 | Whole incoming pixel-payload CRC32 |
| 60 | 4 | CRC32 of header bytes 0–59 |

Each record contains one complete raw frame followed by its little-endian CRC32. CRC
rules match the existing photo format. Incoming payload CRC, stored frame CRCs and
whole-payload readback must all agree before publication. The frame CRC records are
added by the Pico and are absent from TCP payloads.

## Playback and compatibility

Startup opens the selected library item, or retained legacy content when no library
selection exists, and verifies its first frame; it does not scan the entire clip at
boot. Every later frame is verified before publication. The library excludes a failed
item from automatic selection and tries another usable item. Legacy playback can try its
preceding retained generation. No valid saved content means the static flashed image is
used. The large demo animation is disabled by default to keep firmware within the Pico
flash capacity.

The player selects frames from elapsed time. Slow reads may skip stale frames rather
than stretching the timeline; requested FPS is not a guarantee of SD throughput.
Ping-pong runs 0,1,…,N−1,N−2,…,1,0 without repeating turning frames. Photo upload
creates and selects a saved library photo, replacing active video playback. Legacy photo
containers remain readable without card conversion.

## Saved video collection

Firmware TCP uploads publish an independent video library item and select it after
verification. Existing clips remain available. The transfer messages below describe the
TCP handshake and payload encoding. See [saved media library](MEDIA-LIBRARY.md) for item
metadata, publication and selection.

## Named HTTP routes

The [controller API](HTTP-API.md) exposes library controls and resumable photo/video
transactions without the legacy operation envelope. Upload routes accept prepared media
headers and binary payload chunks, support persistent resume tokens and report SD verification.
The named HTTP video start stores the chosen maximum-duration setting. TCP naming sends the
name and existing media descriptor; it does not send that separate duration setting.

## Browser video messages

`POST /api/video` uses `Content-Type: application/octet-stream`. Its nine-byte
little-endian envelope contains operation (one byte), session ID (four bytes) and
confirmed payload offset (four bytes). The body contains at most 6144 bytes. HTTP
callbacks receive only this bounded message; `HttpMediaUpload::Update` performs
filesystem work in the foreground, outside the lwIP lock.

| Operation | ID / offset | Body |
| --- | --- | --- |
| 1: begin | Both zero | 32-byte descriptor and four-byte payload CRC |
| 6: named begin | Both zero | Begin fields, 64-byte ASCII name, duration limit as uint32 milliseconds |
| 2: chunk | Session / confirmed offset | Up to 6144 payload bytes |
| 3: status | Session / zero | Empty |
| 4: cancel | Session / zero | Empty |

Responses contain `id`, `state`, `offset`, `total`, `resumeToken`, `videoId`, `photoId`, `error` and
`warning`. A protocol rejection is reported through `error`; successful HTTP transport
alone does not confirm a saved clip. After `receiving`, the upload enters `verifying`.
Only a fully checked SD publication produces `saved` and a nonzero library ID. A
selection-settings failure is a separate warning when the video itself is already saved.
Frames and earlier clips remain managed by the existing library storage rules.

The browser sends one request at a time and follows acknowledged offsets. Explicit
cancellation, receive inactivity or network-service shutdown releases the unfinished
transaction and restores playback. A lost HTTP connection abandons only its request,
preserving confirmed payload progress and independent verification. Verification advances in bounded foreground steps between
status requests.

### Photo and library messages

The same binary envelope also carries named photos and browser collection controls. For
these controls the two uint32 fields contain the values below rather than an upload
session and offset. All control bodies are empty.

| Operation | ID field | Offset field | Result |
| --- | --- | --- | --- |
| 5: photo begin | 0 | 0 | Upload state; body is descriptor + payload CRC + 64-byte name + 1024-byte legacy thumbnail |
| 10: photos | 0 | Exclusive cursor | Ten-item photo page and selection |
| 11: show photo | Photo ID | 0 | Updated photo page |
| 12: slideshow | Enabled 0/1 | Interval seconds | Updated photo page |
| 13: delete photo | Photo ID | 0 | Updated photo page |
| 14: photo preview | Photo ID | 0 | 32 × 64 RGB565 base64 pixels |
| 15: playback | 0 | 0 | Cached mode, selection and display settings |
| 16: videos | 0 | Exclusive cursor | Ten-item video page |
| 17: show video | Video ID | 0 | Updated video page |
| 18: delete video | Video ID | 0 | Updated video page |
| 19: video preview | Video ID | 0 | First-frame RGB565 base64 pixels |
| 21: display settings | FPS cap, 0–60 | Brightness, 0–100 | Updated playback status |
| 23: storage usage | 0 | 0 | Managed file/allocation totals and data capacity |

Photo begin is followed by the same chunk/status/cancel sequence as video; a verified
save returns a nonzero `photoId`. Cached playback queries remain available during
upload, while filesystem queries and control changes reject competing saves. The page's
shared request queue prevents concurrent commands from claiming the single HTTP mailbox.

## Retry and resume

Photo and named-video starts accept an optional nonzero uint32 resume token
appended to their legacy body: photo headers are 1124 or 1128 bytes; named-video
headers are 104 or 108 bytes. All token fields are little-endian. A matching
repeated token and header checksum returns the same session, progress and saved
ID instead of creating another item. Changed metadata, name or thumbnail under
the same token is rejected. Responses include `resumeToken`; legacy starts
remain supported with token zero.

Operation 20 resolves the current job by token. Its envelope ID and offset are
zero and its body contains only the four-byte token. A mismatched, absent or
replaced token is rejected. Only the current session is retained, including its
terminal status until another upload replaces it. Tokens are job identifiers,
not persistent media metadata.

The browser retries transient network failures, incomplete JSON and HTTP 409/5xx
responses up to 100 times for video or eight times for photos. Upload requests
have a five-second response deadline, with cancelable backoff from one to five
seconds. SD, checksum and metadata errors are terminal. After a lost chunk reply,
the browser queries confirmed progress before resending: a committed chunk is
not written twice, while an uncommitted chunk can be sent again. A verification
check continues the same readback without uploading or checking the file afresh.

When automatic video retries are exhausted, **Resume upload/check** retains its
prepared payload and outstanding chunk boundary in the open page. Resume checks
the session token and confirmed position before continuing, without decoding the
source again. **Cancel** resolves ownership by token, including after a lost
start reply, and releases an unfinished transaction. Verified items are retained.
Browser storage is disposed after success, cancellation or terminal failure.

Receiving jobs expire after 15 seconds without confirmed upload activity. Matching
status/resume requests renew the grace; unrelated Summary or library requests
do not. Progressing TCP uploads use the same inactivity interval, renewed by data,
acknowledgements and verification work. After the final HTTP chunk, verification
continues independently of the browser and can exceed 15 seconds.

Resume requires the same Pico session and open page. A long outage that expires
a receiving job, a Pico restart, replacement by another upload, or page/browser
shutdown requires sending the source again. A retained verifying or saved job can
still be checked without resending its bytes. Python TCP payload transfers use
their existing stop-and-wait protocol and restart an interrupted payload.


## Python photo sizing

`sendImageOverTCP.py` keeps its TCP RGB888 full-panel crop path. `--sizing fit` instead
uses the HTTP photo-library transaction: it stores the fitted RGB888 rectangle, a filename
label and thumbnail, then waits for verified publication. Configure `SIZING_MODE` at the
top or pass `--sizing crop`/`--sizing fit`; `--http-port` defaults to 80. Client-side photo
fitting respects orientation and renders transparent areas against black.
