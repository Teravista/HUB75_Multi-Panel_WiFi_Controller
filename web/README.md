# Local media web interface

The firmware embeds `index.html` in flash and serves it at `http://<Pico-IP>/`
on port 80. The page has no external assets or service dependencies. Choose an
image to preview a centered crop, then use **Save and show photo** to send it.
The browser scales the image to 128 × 256 and sends 98,304 RGB888 bytes; it does
not upload the original JPEG/PNG file.

`POST /api/frame` accepts exactly one full RGB888 frame with
`Content-Type: application/octet-stream`. The server returns `ACK` after complete
pixel reception. The main loop then attempts to save and select a new photo in
the SD library. This acknowledgement confirms reception rather than a durable
SD save; unavailable storage leaves the received photo live on the display.

HTTP and the existing TCP photo uploader share the same framebuffer ownership.
Only one photo transaction can own it, and incomplete HTTP bodies are never
published. The listener accepts one HTTP client, uses bounded request/response
buffers and sends the embedded page in small acknowledged fragments.

Requests require the Pico's local IPv4 Host header. Origin and browser fetch-site
headers, when present, must identify the same local site. Compressed or chunked
request bodies are rejected. Open the page directly at the Pico's address.

The foreground starts HTTP after the existing network/TCP services are ready,
retries failed binds, and closes the listener before radio reinitialization.
`cmake/EmbedWebPage.cmake` regenerates the compiled page when its HTML changes.

## Video upload

Choose a browser-decodable video, optionally enter its saved name, and set FPS
(1–60), maximum source duration (0.1–600 seconds), format and loop/ping-pong mode.
An empty name uses the source filename without its extension. RGB565 is the
default; RGB888 and indexed baseline JPEG are available. JPEG quality defaults to
85 and accepts 1–95. Audio is not transferred.

The browser samples frames from the start of the source and uses the same
centered full-panel crop as photos. It prepares frame records and their CRCs
before sending bounded HTTP messages. Small prepared clips use browser RAM;
larger clips spill into a temporary IndexedDB database. Browser storage capacity
can limit long clips; shorter duration, lower FPS or JPEG reduces staging size.
Temporary records are disposed after completion, failure or cancellation.

Reception and SD verification are separate phases. The page reports a saved clip
only after the Pico confirms durable publication and returns its library ID.
A successfully saved video is selected for playback. Cancel releases an unfinished
transaction while retaining earlier saved media. The frame rate stored with a
video is its requested rate; playback remains subject to SD and decoding throughput.

See [video upload](../docs/VIDEO-UPLOAD.md) for the bounded browser protocol.
