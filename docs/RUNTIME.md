# Runtime ownership

## Cores and buffers

| Owner | Work |
| --- | --- |
| Core 0 foreground | SD/FatFs operations, upload persistence and verification, saved-media selection, local JPEG fallback, frame preparation and submission |
| Core 0 scan interrupt | HUB75 row/bit-plane progression and complete-scan buffer swaps |
| Core 1 | Wi-Fi association, DHCP, local listener lifecycle, HTTP/TCP transport callbacks, response pumping, connection health and reserved JPEG decode jobs |

`StartNetworkCore` launches one long-lived `NetworkServices` worker. The HUB75 driver
uses Core 0 (`HUB75_MULTICORE=false`). Both SDK core stacks are configured to 4 KiB.
The scanner keeps displaying its last submitted frame while foreground work waits.

## Shared JPEG worker

Core 1 advertises decoder availability only after a network pass with no active photo,
video or HTTP client. Core 0 reserves that slot before preparing compressed input.
A reservation prevents Core 1 from starting another foreground network operation that
could wait for a storage handoff. If the slot is busy or the worker has not started,
the frame decodes synchronously on Core 0.

Core 0 supplies encoded data through two 8 KiB slots while Core 1 decodes the previous
bytes into provisional pixels. Networking interrupts remain enabled during decoding.
Read failures signal end-of-input, and Core 0 waits for worker completion before
reusing the context or workspace. Complete record checks still precede publication.

## Lifecycle handoffs

A single-command mailbox lets Core 1 request a safe foreground boundary. Core 0
finishes its current storage/render operation, handles the command and acknowledges it.
Listener creation and shutdown use pause/resume commands so foreground transport users
cannot overlap lifecycle changes.

Radio initialization uses a separate suspend/ready handoff. Core 0 completes canceled
upload cleanup, closes playback and releases the SD session before the radio changes.
After the initialization attempt, Core 0 remounts storage and schedules saved-media
restoration. This finishes before the SDK association/DHCP wait, so that wait leaves
Core 0 available for playback. Failed storage initialization retains the built-in
fallback. A radio restart therefore also reopens storage.

## Upload and rendering ownership

HTTP and video services expose separate storage and transport update functions.
Transport code buffers bounded requests and publishes work under the lwIP lock; Core 0
performs filesystem operations outside that lock and publishes completed responses.
Stop-and-wait video acknowledgements apply backpressure while a chunk is being saved.

An active media save owns the shared framebuffer, pausing saved playback until
completion, cancellation or inactivity cleanup releases it. Rendering claims the frame
before importing a photo, advancing saved playback or drawing the connection overlay.
Reload requests remain pending until that claim succeeds.

## Status access

Core 0 reads a bounded, coherent copy of readiness, state name and IP address through
`NetworkServices::ReadStatus()`. The short status-copy lock is released before rendering,
SD access or a Wi-Fi wait. The display never borrows mutable connection strings from
Core 1.

See [display buffers](DISPLAY.md), [storage](MEMORY-MANAGER.md) and
[upload protocols](VIDEO-UPLOAD.md).
