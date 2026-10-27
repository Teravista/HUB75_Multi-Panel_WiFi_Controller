# Photo web interface

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
