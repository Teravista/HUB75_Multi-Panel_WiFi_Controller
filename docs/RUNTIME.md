# Runtime ownership and connectivity

## Cores and buffers

| Owner | Work |
| --- | --- |
| Core 0 foreground | SD/FatFs operations, upload persistence and verification, saved-media selection, local JPEG fallback, frame preparation and submission |
| Core 0 scan interrupt | HUB75 row/bit-plane progression and complete-scan buffer swaps |
| Core 1 | Wi-Fi association, hotspot/DHCP, local listener lifecycle, HTTP/TCP transport callbacks, response pumping, connection health and reserved JPEG decode jobs |

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

## Wi-Fi and direct hotspot

The radio provides home-network station mode and a password-protected direct
hotspot simultaneously. Both serve the same HTTP page/API on port 80, photo TCP
listener and video TCP listener. The page uses the receiving connection's local
address for origin validation; either address is a separate browser origin.
There is no Internet forwarding, DNS interception or automatic captive portal.

Join the `WIFI_AP_SSID` network from the phone's Wi-Fi settings and open
`http://192.168.4.1/` using the private `WIFI_AP_PASSWORD` from `.env`. Both
hotspot settings are required: `WIFI_AP_SSID` accepts 1–32 bytes and
`WIFI_AP_PASSWORD` accepts 8–63 bytes. There are no built-in credentials; missing
or empty settings stop configuration generation. Changes take effect after building/flashing.
`WIFI_HOTSPOT_MAX_CLIENTS` in `src/config/defines.h` sets the bounded lease and
device-indicator capacity, currently four slots (`192.168.4.2` through
`192.168.4.5`) on `192.168.4.0/24`. Actual simultaneous associations are capped
at the lower of that setting, the firmware-reported ceiling and the Pico 2 W's
documented four-client SoftAP limit. An unused slot stays dark. DHCP listens only
on the AP interface and never on the home LAN. The packet workspace and tracking
tables are fixed-size and require no framebuffer. Dots use actual radio associations,
with ARP/DHCP mapping accepted application traffic to the corresponding device.

Home association and DHCP proceed asynchronously for up to 45 seconds, including
the address-acquisition grace period. Failed attempts retry after ten seconds;
starting a retry waits for active HTTP/photo/video clients to finish. Hotspot
requests and transport maintenance continue while association is pending. The
AP and station share one radio/channel, so association or router channel changes
can briefly interrupt the direct wireless link.

Station `no-wifi`, `wifi-no-tcp` and `ready` states track home connectivity and
shared listeners. `ready-unverified` indicates pending home-path verification.
Without home connectivity, `hotspot` or `hotspot-connecting` reports ready direct
access, with the latter also indicating a pending home connection. The icon is
ready when the corresponding interface and shared listeners are available;
home Wi-Fi also requires path verification. Home Wi-Fi appears at the
bottom-right, and the hosted network appears at the bottom-left. Each has its
own IP label and never substitutes the other interface's address. Serial status
reports both interfaces separately as `home_wifi`/`home_ip` and `pico_wifi`/`pico_ip`.

Home health checks use the station interface explicitly. Gateway TCP probes are
diagnostic; fresh ARP replies or incoming IPv4 traffic verify the home path.
Ordinary home link/address or reachability loss cancels only station monitoring
and retries its interface. Hotspot leases and shared listeners stay running.
A debounced radio-query fault requests a full radio recovery once clients are
idle; this recreates the hotspot and listeners. Listener allocation failures
retry separately without remounting SD. SD recovery never restarts networking.

The home network must not overlap `192.168.4.0/24`. If its address/mask overlaps,
the station disconnects and direct access remains available. Change
`WIFI_HOTSPOT_ADDRESS` and `WIFI_HOTSPOT_NETMASK` in `src/config/defines.h` to a
disjoint /24 when required. DHCP leases use host numbers 2–5 in that subnet.

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
separate home Wi-Fi and Pico hotspot states/IPs, and SD state. Upload activity and a pending SD retry appear when
applicable. Scan and video rates cover the actual time since the preceding report.

```text
[status] t=125s display_fps=62.6 video_fps=29.9/30.0 mode=video home_wifi=ready home_ip=192.168.1.48 pico_wifi=ready pico_ip=192.168.4.1 pico_clients=1/4 sd=ready
[status] t=135s display_fps=62.6 video_fps=- mode=builtin home_wifi=no-wifi home_ip=- pico_wifi=ready pico_ip=192.168.4.1 pico_clients=0/4 sd=unavailable retry=7s
```

Home states include `no-wifi`, `connecting`, `wifi-no-tcp`, `ready-unverified`
and `ready`. Pico hotspot states are `unavailable`, `services-pending` and `ready`.
An interface without an assigned address reports `-` for its IP.
`pico_clients` reports associated devices against the configured hotspot capacity,
for example `1/4`. Idle white dots and active green dots both count as connected;
old DHCP leases without a current radio association do not.

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
