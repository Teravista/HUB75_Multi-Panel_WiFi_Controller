# HUB75 Multi-Panel WiFi Controller

## Guides

| Topic | Guide |
| --- | --- |
| Browser tabs, upload options, placement and previews | [Web interface](web/README.md) |
| Saved collections, slideshow, settings and pagination | [Media library](docs/MEDIA-LIBRARY.md) |
| Python uploads, transfer messages and failure handling | [Video uploads](docs/VIDEO-UPLOAD.md) |
| SD wiring, storage API, verification and throughput | [Storage](docs/MEMORY-MANAGER.md) |
| Binary descriptors and container compatibility | [Media metadata](docs/MEDIA-METADATA.md) |
| Compressed frame layout and bounded decoder | [JPEG video](docs/JPEG-VIDEO.md) |
| Frame buffers, scan isolation and FPS | [Display](docs/DISPLAY.md) |
| Core ownership, network lifecycle and upload handoffs | [Runtime](docs/RUNTIME.md) |

<table>
  <tr>
    <td width="50%">
      <img src="readmeAssets/DisplayWithAppleFlashed.png" alt="HUB75 multi-panel display showing an Apple logo">
    </td>
    <td width="50%">
      A Raspberry Pi Pico 2 W controller for a 128x256 HUB75 LED display made from four daisy-chained 64x128 panels, with animation playback and live image uploads over WiFi/TCP. This README covers the hardware build, usage, and internal architecture.
      <br><br>
      <a href="readmeAssets/WalkInTheForest.mp4">Watch the display demo (MP4)</a>
      <br><br>
      <strong>NOTE: The wave-like color artifacts in the video are caused by the interaction between the panel's refresh method and the camera's shutter. They are not visible in person; the display looks closer to the still Apple image.</strong>
    </td>
  </tr>
</table>

## Table of Contents

- [1. Overview](#1-overview)
  - [What This Project Is](#what-this-project-is)
  - [Forked From](#forked-from)
  - [Hardware Used](#hardware-used)
  - [What It Does](#what-it-does)
  - [Ways to Get Content Onto the Panel](#ways-to-get-content-onto-the-panel)
- [2. Hardware / Wiring](#2-hardware--wiring)
  - [Faulty HUB75 Connector Byte-Reorder Quirk](#faulty-hub75-connector-byte-reorder-quirk)
- [3. Usage](#3-usage)
  - [WiFi connection](#wifi-connection)
  - [Build & Flash Quickstart](#build--flash-quickstart)
  - [USE_ANIMATION flag](#use_animation-flag)
  - [Flashing Your Own Image or Animation](#flashing-your-own-image-or-animation)
  - [Python Utility Setup](#python-utility-setup)
  - [Uploading an Image Over TCP](#uploading-an-image-over-tcp)
- [4. Implementation Details / Architecture](#4-implementation-details--architecture)
  - [Source folders](#source-folders)
  - [Program Flow](#program-flow)
  - [Driver Overview](#driver-overview)
  - [WiFi State Machine](#wifi-state-machine)
- [5. Credits / License](#5-credits--license)
  - [Attribution](#attribution)
  - [License](#license)

# 1. Overview

## What This Project Is

A Raspberry Pi Pico 2 W controller drives four HUB75 panels as one 128 × 256 canvas. It
displays built-in content, plays photos/videos saved on microSD, and accepts browser or
Python uploads over the local Wi-Fi network.

## Forked From

Panel output uses the DMA/PIO/BCM driver from
[JuPfu/hub75](https://github.com/JuPfu/hub75), with controller code for panel mapping,
saved media, playback and network uploads. See [third-party
notices](THIRD_PARTY_NOTICES.md) for attribution and licensing.

## Hardware Used

The controller targets a Raspberry Pi Pico 2 W and four daisy-chained 64 × 128 P2
panels, arranged as a synchronized 128 × 256 canvas. A microSD reader provides storage
for the saved photo/video libraries.

## What It Does

Boot restores the selected saved photo, video or slideshow. If saved content is
unavailable, the panel displays its built-in image or animation. The Wi-Fi icon and IP
address identify the local connection; browser and Python uploaders save content for
display and restoration after restart.

## Ways to Get Content Onto the Panel
Three Python scripts handle content:
- `imageToRGB888ArrayConverter.py` / `mp4ToRGB888ArrayConverter.py` - convert an image or video into a C header with a byte array, included via `defines.h` under the `USE_ANIMATION` flag. This becomes the default image/animation flashed into firmware.
- `sendImageOverTCP.py` - uploads a new image to the running Pico over TCP, replacing what's on screen without reflashing.

All three are configurable via defines at the top of the script or command-line
arguments.

# 2. Hardware / Wiring

<table>
  <tr>
    <td width="50%">
      <img src="readmeAssets/BackSideOfPanel.png" alt="Backside of the panel showing its wiring">
    </td>
    <td width="50%">
      <strong>NOTE: Use this wiring photo as a reference. Pin assignments and wiring must match your panels and controller configuration.</strong>
      <br><br>
      Pin mapping / wiring config lives in <a href="third_party/hub75/src/hub75.hpp">third_party/hub75/src/hub75.hpp</a>. Pins can be changed by editing the defines there, but must follow the rules noted in the comments above each define (e.g. color/row-select pins must be consecutive).
    </td>
  </tr>
</table>

## Faulty HUB75 Connector Byte-Reorder Quirk

The panels used by this configuration have incorrect physical colour annotations on
their HUB75 connectors. The software compensates with the `IMAGE_RED_BYTE_OFFSET`,
`IMAGE_GREEN_BYTE_OFFSET` and `IMAGE_BLUE_BYTE_OFFSET` definitions in
`src/config/defines.h`. This byte reordering preserves the intended colours for that
wiring. For panels with standard RGB order, set RED=0, GREEN=1 and BLUE=2 instead.

# 3. Usage

## WiFi connection
Copy `.env.example` to `.env` in the repository root, then replace every placeholder
with your local settings.

- `PICO_IP` - Pico's current LAN address, used by the Python upload script only.
- `PICO_PORT` - TCP image-upload port. CMake gives this value to the firmware listener, and the Python upload script connects to the same port.
- `WIFI_SSID` - WiFi network name compiled into the firmware.
- `WIFI_PASSWORD` - WiFi password compiled into the firmware.
- `WIFI_GATEWAY_PROBE_PORT` - local gateway TCP diagnostic port

CMake generates `build/generated/networkConfig.h` from `.env`. Do not edit or commit
that generated file; a normal build recreates it when it is missing or when `.env`
changes. The Python upload script reads `PICO_IP` and `PICO_PORT` directly from the same
`.env` file.

If the connection fails, the Wi-Fi icon shows red and network uploads are unavailable.
Saved media or the built-in fallback continues displaying while the controller retries.

## Build & Flash Quickstart
Configure `.env` as described above before the first build. This project is built and
flashed using VS Code with the [Raspberry Pi Pico
extension](https://marketplace.visualstudio.com/items?itemName=raspberry-pi.raspberry-pi-pico).
Import the project through the extension, then use its UI buttons to compile and
run/flash - no manual CLI setup required.

## USE_ANIMATION flag
The `USE_ANIMATION` flag in `src/config/defines.h` controls what gets built into flash:
`0` builds a single still image, `1` builds a multi-frame animation.

## Flashing Your Own Image or Animation
1. Run `imageToRGB888ArrayConverter.py` (still image) or `mp4ToRGB888ArrayConverter.py` (video/animation), pointing it at your source file and adjusting settings like animation delay/fps as needed.
2. The script generates a `.h` file in `imagesVideos/`.
3. `#include` that new file in `src/config/defines.h` under the `USE_ANIMATION` block - that's the only required change.

## Python Utility Setup
Install the Python packages used by the image, video, and TCP upload scripts:

```sh
python -m pip install -r utilityScripts/requirements.txt
```

## Uploading an Image Over TCP
1. After flashing, the default image/animation should be visible with a small white WiFi icon in the top right corner, confirming the Pico is connected (also checkable in your router's admin panel).
2. Look up the Pico's IP address from your router.
3. Update `PICO_IP` in `.env` if the address differs from the value used during setup. Keep `PICO_PORT` equal to the firmware upload-listener port.
4. Set your image path (`IMAGE_PATH`) in `utilityScripts/sendImageOverTCP.py`, then run the script.
5. The new image should be uploaded and displayed by the panel.

Completed photo uploads are saved to microSD. Python video uploads use TCP port 5002 and
retain their format, frame rate and loop setting after restart. If no valid saved
content is available, the controller uses the flashed fallback.

# 4. Implementation Details / Architecture

## Source folders

```text
src/
  config/       Network, panel, storage and filesystem configuration
  display/      CPU frame, rendering and display helpers
  network/      Wi-Fi connection and TCP photo/video uploads
  http/         Request parsing, web server and browser media uploads
  sd/           MemoryManager, SPI storage and checksums
  media/        Metadata, playback, libraries and slideshow
    jpeg/       Bounded JPEG decoder wrapper
third_party/    Bundled HUB75, SD filesystem and JPEG dependencies
```

Implementation files live in each module; declarations belong in its `include/`
subfolder. `mainEntry.cpp` contains startup and the main loop. Include paths are
relative to `src/`, with `src/config/` supplying the SDK's `ffconf.h` and `lwipopts.h`
names.

## Program Flow

Startup initializes HUB75, prepares a fallback frame, mounts storage and restores the
saved library selection where available. It then attempts the Wi-Fi connection and
starts the HTTP and TCP services.

Core 1 advances Wi-Fi and HTTP/TCP transport work and accepts JPEG decode jobs when idle.
Core 0 handles upload/save jobs, library selection, local JPEG fallback and video playback,
then prepares and submits frames
under shared-frame ownership. Lifecycle handoffs pause foreground transport users
while listeners change. SD resources are released for radio initialization and reopened
before the association wait, allowing saved playback to continue during that wait.
See [runtime ownership and handoffs](docs/RUNTIME.md).

## Driver Overview

PIO/DMA outputs rows and bit-planes while a short Core 0 interrupt advances the scan.
The driver uses two scan buffers and swaps them at complete-scan boundaries. Foreground
image preparation can pause while the scanner displays the last submitted frame. See
[display buffers and scan isolation](docs/DISPLAY.md).

## WiFi State Machine

`WifiConnection` advances `NoWifi`, `WifiNoTcp` and `Ready` on Core 1.

- `NoWifi`: retries radio initialization and association after the configured delay.
  A timed-out join receives a grace period so a late DHCP address can be adopted.
- `WifiNoTcp`: has a station address and retries binding the HTTP, photo and video
  listeners until all required services are available.
- `Ready`: accepts uploads and checks station/link health and listener availability.
  Lost links or repeated radio-response failures stop services and schedule recovery.

A periodic TCP connection to the local gateway is diagnostic. A filtered or closed
gateway port alone does not stop active upload services. No Internet access is required.

# 5. Credits / License

## Attribution
- [JuPfu/hub75](https://github.com/JuPfu/hub75) by Jürgen Pfundt and Christoph Mair - the DMA/PIO/BCM HUB75 driver core this project is forked from.
- [Pimoroni](https://github.com/pimoroni) - `pico_graphics`, `bitmap_fonts`, and `hershey_fonts` libraries, vendored under `libraries/`.

## License
This project's original code is licensed under the [MIT License](LICENSE). Vendored and
derived components remain subject to their respective licenses; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Video upload supports RGB565, RGB888 and indexed JPEG, with JPEG quality 85 by default.
See [JPEG video and compact storage](docs/JPEG-VIDEO.md).

The local web page groups summary controls and saved photo/video libraries into three
tabs, with ten-item pages, previews, selection/deletion and slideshows. Uploads support
fit/crop or draggable zoom while storing only visible image data. See [web
interface](web/README.md) and [media placement](web/README.md#upload-placement).
