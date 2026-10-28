# Local media web interface

The firmware serves its embedded page at `http://<Pico-IP>/` on port 80, without
external assets. Three tabs separate the summary, photos and videos.

## Summary

The summary shows the active built-in, photo, slideshow or video mode and the selected
item's name. Set brightness from 0–100 percent and an optional maximum video FPS from
1–60; a blank FPS field preserves the video's saved rate. Settings are persisted before
application. Summary refreshes cached status in the background while preserving
confirmed values and unsaved edits. **Refresh summary** also measures managed SD usage.

Storage reports logical managed-file bytes, their cluster-rounded allocation and the
filesystem's usable data capacity. Managed totals include controller libraries,
settings, retained saves and staging files; unrelated card files and filesystem
bookkeeping are excluded.

## Photos and videos

Choose a source, optionally enter its saved name, and select RGB565 (default), RGB888 or
JPEG. Blank names use the filename without its extension. JPEG quality defaults to 85
and accepts 1–95. The browser prepares pixels rather than sending the original image or
video file. Video controls add FPS (1–60), maximum source duration (0.1–600 seconds) and
loop/ping-pong; audio is omitted.

Both editors share the [upload placement controls](#upload-placement).

Each library shows up to ten items per page with previous/next navigation, metadata and
a 32 × 64 RGB565 preview. Video previews sample the first frame. Choose **Show** or
**Delete** per item, or enable a photo slideshow and set its interval in minutes.
**Refresh library** reloads the current page; there is no fixed photo/video item count
beyond storage and filename capacity.

## Transfer and storage

The page serializes bounded messages to `POST /api/video` for photo/video saves, library
queries and controls. A saved result requires verified SD publication; reception alone
is not a durable confirmation. Prior items survive unfinished or failed uploads. Small
prepared clips use browser RAM; larger clips spill into a temporary IndexedDB database,
removed after completion, cancellation or terminal failure. A paused resumable
video retains its prepared data. Browser storage capacity can limit
long clips.

The legacy `POST /api/frame` endpoint accepts exactly 98,304 full-panel RGB888 bytes and
replies `ACK` after reception. Its SD save is attempted later by the main loop. HTTP and
TCP uploads share framebuffer ownership. One HTTP client is accepted at a time;
filesystem work runs in the foreground outside lwIP callbacks. Requests require a local
IPv4 Host and same-site Origin/fetch headers when present; compressed and chunked
request bodies are rejected.

`cmake/EmbedWebPage.cmake` regenerates the compiled page when the HTML changes. See
[media messages](../docs/VIDEO-UPLOAD.md) for the binary upload envelope.

## Upload placement

Both photo and video editors offer **Fill / crop**, **Fit** and **Manual zoom**.
Fill/crop preserves the source aspect ratio while covering the panel; drag to choose the
cut. Fit keeps the whole source visible at its largest fitting size. Manual zoom accepts
1–500% relative to Fit: 100% equals Fit, smaller values shrink the source and larger
values enlarge it for cropping.

Upload previews display at 256 × 512 CSS pixels, shrinking on narrow screens. The video
preview shows the first decoded source frame. A red outline marks the stored rectangle;
black surrounds it. The outline is an HTML overlay and is excluded from uploaded pixels
and JPEG data. Saved-library thumbnails remain 64 × 128 CSS pixels.

Drag with the primary mouse button or a finger, starting inside visible image content.
An axis that exactly fills the panel cannot move. Smaller content is clamped inside the
panel; larger content continues to cover it. Focus a preview to use arrow keys; Shift
moves ten logical pixels. Dragging uses panel coordinates regardless of the CSS preview
size. The chosen placement applies to every frame of a prepared video.

The browser encodes only the visible rectangle. Its dimensions and exact offsets are
saved per item; the Pico clears surrounding pixels to black in the existing CPU frame.
Borders consume no SD reads, transfer bytes or JPEG decoding. Transparent source pixels
inside the rectangle are composited onto black and remain in its payload. A fitted 16:9
source stores 128 × 72 pixels: RGB565 needs 18 KiB rather than 64 KiB per full frame.

Centered uploads retain descriptor versions 1–3. Custom offsets use the 32-byte
version-4 descriptor. Existing full-panel files remain readable. See
[metadata](../docs/MEDIA-METADATA.md#placement-descriptor-version-4) for its encoding.

## Interrupted uploads

Transient video requests receive up to 100 automatic retries; photos receive up
to eight. After a lost chunk reply, the page reads the Pico's confirmed progress
before sending more data. Lost verification replies continue the same SD check.

If video retries are exhausted, use **Resume upload/check** or **Cancel** while
keeping the page open. Resume reuses prepared frames and checks the original job
token; Cancel releases an unfinished save and retains verified content. A receiving
job expires after 15 seconds without upload activity, so a long outage can require
a new transfer. Verification after full reception is independent of that deadline.
Resume does not survive a page reload or Pico restart. See
[upload recovery](../docs/VIDEO-UPLOAD.md#retry-and-resume) for the protocol and limits.


## Custom controllers

The same settings, libraries and upload sessions are available through the
[HTTP controller API](../docs/HTTP-API.md). Native controllers send numeric query
parameters and prepared binary media; source-file decoding and crop/fit/zoom remain
client-side operations. Browser clients retain the local-origin request checks.

## Summary updates

Summary displays separate display/video FPS averages for the latest 30 seconds,
plus RAM accounting, managed SD usage and card state. A photo shows display FPS;
a video also shows the rate of distinct submitted frames. The startup averaging
window grows to 30 seconds.

While Summary is visible, cached status refreshes every two seconds. Managed SD
totals are measured on manual refresh and at most once per 30 seconds when storage
is idle. Photos and Videos load on tab selection, explicit Refresh, page navigation
or a saved-library action; Summary polling does not fetch their pages or previews.

Refreshes keep the last confirmed values visible. A connection failure adds a
separate warning. Input fields remain editable, and brightness/FPS edits survive
background replies and edits made while a preceding settings save is pending.

## Build

Configure the repository's `.env`, then use CMake and Ninja with the project's
Pico SDK/toolchain configuration:

```powershell
cmake --preset web-upload
cmake --build --preset web-upload
```

The preset selects Pico 2 W and a size-optimized build. Flash
`build/web-upload/hub75.uf2`; HTML embedding runs automatically when the page changes.
