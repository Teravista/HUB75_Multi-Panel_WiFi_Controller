# HTTP controller API

Base URL: `http://<Pico-IP>/api`, for example `http://192.168.1.48/api`. Port 80 is the
same server used by the existing webpage. `GET /api` returns a JSON discovery document
with routes, formats, dimensions and limits. These routes use the same storage/playback
functions as the web page. The binary `POST /api/video` endpoint and Python TCP upload
ports are also supported.

This exposes every operation behind the HTML: status, memory usage, settings, library
pages/metadata, previews, selection, deletion, slideshow and the complete photo/video
save/verify/resume/cancel flow. File picking, MP4 decoding, frame sampling,
crop/fit/manual zoom/drag and pixel encoding run on the uploading client, just as in the
webpage. The Pico receives prepared content/placement metadata rather than source MP4,
PNG or arbitrary JPEG files.

## Control routes

Controls use numeric **URL query parameters**, with an empty request body. They do not
require a binary envelope or a JSON request-body parser. Methods are significant: reads
use GET, selections/settings use POST, deletion uses DELETE.

| Method | Path after `/api` | Result / parameters |
| --- | --- | --- |
| GET | `/status` or `/state` | Active mode/name, displayed IDs, loop mode, SD state and display settings |
| GET | `/settings` | Same live status, including `displaySettings.maxFps` and `brightness` |
| POST | `/settings?maxFps=30&brightness=80` | Set both; maxFps 0–60, brightness 0–100 |
| POST | `/brightness?value=80` | Change brightness only; preserves the FPS cap |
| POST | `/max-fps?value=30` | Change cap only; preserves brightness; 0 uses saved video FPS |
| GET | `/memory` | Freshly measured managed SD `storage` usage and SD state |
| GET | `/photos?after=0` | Up to ten photo records with individual name/format/quality/dimensions/placement/bytes |
| GET | `/videos?after=0` | Up to ten video records with name/format/quality/FPS/frames/duration/maxSecondsMillis/ping-pong/bytes |
| GET | `/photos/{id}/thumbnail` | JSON `{width,height,pixels,error}` |
| GET | `/videos/{id}/thumbnail` | First-frame preview in the same format |
| POST | `/photos/{id}/show` | Save photo selection; returns photo overview |
| POST | `/videos/{id}/show` | Save video selection; returns video overview |
| DELETE | `/photos/{id}` | Delete that photo; returns photo overview |
| DELETE | `/videos/{id}` | Delete that video; returns video overview |
| GET | `/slideshow` | Live playback status |
| POST | `/slideshow/start?intervalSeconds=120` | Start photo slideshow, 6–86400 seconds |
| POST | `/slideshow/stop` | Stop; retains the configured interval and selected photo |

Brightness 0 turns the panel dark while networking continues. A native controller can
remember the previous brightness and restore it to implement its on/off button.

Pagination replies contain `pageSize:10`, `after`, `nextAfter`, `hasMore` and `count`.
When `hasMore` is true, request the next page using `after=nextAfter`. Do not use page
numbers as media IDs. IDs are independent for photos and videos. Selection is stored
immediately, while `mode`/`displayed` describe the frame actually published to the
panel; they may update on the next render iteration.

Thumbnail `pixels` is base64, little-endian RGB565, row-major. Current previews are
32×64. Use the returned width/height rather than assuming a size.
Status/settings reads do not read the SD card.
`/memory`, lists and previews require storage and may take longer.

Status/settings and `/memory` also include `storageState` (`unavailable`, `mounting`,
`ready`, `recovering`), `storageRetryMs`, `storageGeneration`, and `storageError`. Live
status also contains the `storageReady` boolean. These fields require no SD access; they
remain readable while storage is down. Unavailable storage retries mounting every 15
seconds after the previous attempt/cleanup completes. Ready storage gets a read-only
physical-sector check every 15 seconds when no save is active. Serious disk/timeout
errors discard the old session; missing/corrupt files, full/read-only storage and busy
requests do not themselves disconnect the entire card. A successful remount increments
`storageGeneration`; discard cached thumbnails/item state when it changes. Interrupted
receiving/verifying uploads fail and cannot resume across a remount. Published files and
saved selections are retained; incomplete temporary files are never library items.
Recovery never formats media. Driver waits are bounded, but mount/read failures can
still briefly pause frame preparation while the independent display scanner keeps
showing its last frame.

## Quick examples

```powershell
curl.exe http://192.168.1.48/api
curl.exe http://192.168.1.48/api/state
curl.exe -X POST "http://192.168.1.48/api/brightness?value=80"
curl.exe -X POST "http://192.168.1.48/api/max-fps?value=60"
curl.exe "http://192.168.1.48/api/photos?after=0"
curl.exe -X POST http://192.168.1.48/api/photos/1/show
curl.exe -X POST "http://192.168.1.48/api/slideshow/start?intervalSeconds=120"
curl.exe -X POST http://192.168.1.48/api/slideshow/stop
curl.exe -X POST http://192.168.1.48/api/videos/1/show
```

Custom native controllers use the same HTTP requests with `Host: <Pico-IP>`, read
`Content-Length`, close the connection and parse JSON. A native HTTP client does not
need an Origin header. Existing Host/Origin checks remain; requests must address the
Pico's actual IPv4, optionally with `:80`. Browser applications on another origin are
rejected by the same-origin checks.

## Upload routes

Uploads keep the existing prepared media format and SD readback verification. These
routes automatically construct the legacy nine-byte operation envelope; **send only the
descriptor or chunk bytes**, never another envelope.

| Method | Path | Body / result |
| --- | --- | --- |
| POST | `/uploads/photos` | 1124-byte prepared photo header, or 1128 bytes with resume token; returns session |
| POST | `/uploads/videos` | 104-byte named video header, or 108 bytes with resume token |
| PUT | `/uploads/{id}/chunk?offset=N` | 1–6144 prepared payload bytes at the exact confirmed offset |
| GET | `/uploads/{id}` | Current job state, confirmed offset, total and saved media IDs |
| GET | `/uploads/resume?token=N` | Resolve a photo or named-video session after a lost start reply |
| DELETE | `/uploads/{id}` | Cancel the current receiving/verifying transaction |
| POST | `/uploads/{id}/cancel` | Alternative explicit cancellation route |

Start headers:

| Offset | Photo | Named video |
| --- | --- | --- |
| 0–31 | H75M media descriptor | H75M media descriptor |
| 32–35 | Whole encoded payload CRC32, little-endian | Same |
| 36–99 | NUL-terminated ASCII name, 64 bytes | Same |
| 100–1123 | Legacy 16×32 RGB565 thumbnail, 1024 bytes | — |
| 100–103 | — | Selected upload max duration in milliseconds, 100–600000 |
| 104–107 | — | Optional nonzero uint32 resume token |
| 1124–1127 | Optional nonzero uint32 resume token | — |

The descriptor stores type, format, dimensions, frame count, rational FPS, ping-pong,
JPEG quality, payload length and optional placement. Exact bytes are documented in
[media metadata](MEDIA-METADATA.md); use the existing codec rather than sending a C
struct. For RGB565 use little-endian packed pixels; RGB888 is R,G,B row-major. JPEG is
the existing indexed baseline-frame stream. Prepared `.h75v` files are 36-byte
descriptor/CRC followed by payload. A prepared `.h75p` file contains a 1124-byte photo
header followed by payload.

The Pico supports JPEG/RGB565/RGB888, up to 60 FPS and 600 seconds as upload metadata,
with the existing FAT32-compatible file limit. Missing borders are generated as black
from dimensions/placement; don't send absent border pixels. Crop/fit/zoom/position
determine the encoded content and placement on the client.

Start replies are JSON with `id`, `state`, `offset`, `total`, `resumeToken`, `photoId`,
`videoId`, `error` and `warning`. Send chunks while state is `receiving`. The last chunk
starts Pico-side verification. Poll while `verifying`; only `saved` confirms verified
publication. All upload sources use this same SD verification. Receiving photos and
videos have a 15-second idle grace. A confirmed upload request renews the grace, so
progressing transfers may last longer. An abandoned receiving job releases storage and
playback; verification continues independently after the final chunk and may exceed 15
seconds. Unrelated Summary/library polls do not keep a receiving upload alive. The
Python video TCP service uses the same 15-second inactivity threshold; received data,
reply acknowledgments and SD verification progress keep it alive. Status reads are
allowed while saving; settings/library mutations are refused.

If a chunk response is lost, query the job and seek your prepared source to its
confirmed offset before sending again. Continue a verification check without
retransmitting the file. Reuse the same photo or named-video header and token when
retrying a lost start: it is idempotent. A different header with the same token is
rejected. Legacy 1124-byte photo starts still work, but lack a nonce; avoid blindly
repeating a legacy start. The page retries photo network errors, checks confirmed
offsets and attempts nonce-based cancellation if recovery fails.

## Responses and concurrency

Valid responses use JSON. Always inspect nonempty `error` and upload `state`:
application errors preserve the existing HTTP-200 JSON contract. Header/route/
method/body errors use 400/403/404/405/409/411/413/415/etc. Unknown parameters,
duplicate query keys, negative/fractional/overflowing values and request bodies on
control routes are rejected. Requests do not use Transfer-Encoding: chunked.

The server handles one HTTP request at a time and closes each connection. Keep native UI
requests serialized; use status about every 1–3 seconds and immediately after actions.
Retry transient connection/409 failures appropriately; do not automatically repeat
deletion or a non-resumable start. A request losing its connection does not cancel a
saved-media upload or restart verification.
