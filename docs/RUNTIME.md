# Runtime ownership and connectivity

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

Wi-Fi radio initialization and reconnects retain the SD session, open playback file,
cache and playback position. Storage recovery never invokes radio or listener lifecycle
operations. A network interruption can cancel an in-progress upload, releasing that
transaction's ownership without remounting SD.

Core 0 advances the independent SD state machine between bounded foreground operations.
Storage loss discards failed handles and clears saved-media display state; the built-in
fallback remains available. A successful remount reloads that card's library and persisted
selection before accepting new storage jobs. Worker completion precedes any buffer or
session invalidation.

## Wi-Fi recovery

Local networking uses `no-wifi`, `wifi-no-tcp` and `ready` states. A usable link/IP
advances to listener startup; all required listeners must be running before service
readiness. Packet reachability is reported separately as `ready-unverified` while
fresh gateway replies or incoming traffic are awaited. Internet access is not required.

Connection attempts wait up to 15 seconds. A timed-out association or DHCP attempt
keeps the radio alive for a further 30-second grace period to adopt a late address.
SDK error `-2` means `PICO_ERROR_TIMEOUT`, distinct from CYW43's `NONET` link value.

Gateway TCP probes are diagnostic. Fresh ARP replies or recent incoming IPv4 traffic
can confirm the LAN path even when the gateway filters the TCP port. Debounced loss
of packet reachability or loss of the station link/IP stops services and schedules
recovery. Wi-Fi restarts retain the SD mount, cache and playback position; SD recovery
leaves the radio and network listeners running.

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

## Serial status

One status line is emitted every ten seconds, including while an upload holds the
CPU frame. It combines completed scan FPS, measured/target video FPS, active mode,
network state/IP and SD state. Upload activity and a pending SD retry appear when
applicable. Scan and video rates cover the actual time since the preceding report.

```text
[status] t=125s display_fps=62.6 video_fps=29.9/30.0 mode=video net=ready ip=192.168.1.48 sd=ready
[status] t=135s display_fps=62.6 video_fps=- mode=builtin net=no-wifi ip=- sd=unavailable retry=7s
```

Boot, network/SD transitions, selected playback, upload completion and failures are
reported as events. Expected busy/unavailable storage queries are quiet; identical
storage failures are limited to one report per 15 seconds, while changed failures
are reported immediately. `HUB75_VERBOSE_LOGS=1` enables extra startup/listener and
storage diagnostics, including the optional serial startup delay.

## Prepared-frame ownership

The foreground loop passes borrowed preparation/submission callbacks to
`RenderPipeline`. Preparation adopts completed uploads, persists a new live photo
and advances saved playback before drawing overlays. Submission updates the
connection overlay and counts a distinct video revision only after handing the
prepared frame to HUB75.

Source ownership stays held while the scanner is busy, and foreground maintenance
continues between readiness checks. A listener handoff or SD availability change
resets pending submission before ownership or saved selection changes. A scanner
timeout releases the CPU source so uploads can proceed, without resetting the
independent panel scanner.
