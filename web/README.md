# Local media web interface

The firmware serves its embedded page at `http://<Pico-IP>/` on port 80, without
external assets. Three tabs separate the summary, photos and videos.

## Summary

The summary shows the active built-in, photo, slideshow or video mode and the selected
item's name. Set brightness from 0–100 percent and an optional maximum video FPS from
1–60; a blank FPS field preserves the video's saved rate. Settings are persisted before
application. **Refresh summary** retrieves the current mode and SD usage without
clearing already displayed values first.

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
removed after completion, failure or cancellation. Browser storage capacity can limit
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
