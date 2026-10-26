# Saved media library

`MemoryManager` stores independent photos and videos on the SD card.
`MediaLibrary` selects content, advances slideshows and persists display settings;
`MediaPlayer` reads and publishes the selected frames under framebuffer ownership.
Filesystem work stays in the foreground, outside TCP callbacks.

## Items and pagination

Each photo or video has a nonzero ID, a name of up to 63 ASCII characters,
its own media descriptor and checksums. Photos also carry a 16 × 32 RGB565
thumbnail. Video metadata includes frame count, rational FPS, loop/ping-pong
mode, format and the supplied maximum-duration setting in milliseconds.

`ListLibraryPhotos` and `ListLibraryVideos` return ordered pages. Pass the previous
`nextAfter` cursor to request the next page; `total` and `hasMore` describe the
collection. The controller retains ten entries per page rather than a complete
directory. Capacity is bounded by card space, filesystem directory capacity and
the 28-bit filename ID namespace. Deleted low IDs can be reused.

`GetLibraryPhoto` / `GetLibraryVideo` inspect one entry. `OpenLibraryPhoto` /
`OpenLibraryVideo`, followed by `ReadMediaFrame`, load checked pixels.
`DeleteLibraryPhoto` / `DeleteLibraryVideo` remove managed files; selection repair
belongs to `MediaLibrary::Remove` / `RemoveVideo`.

## Saving and publication

`BeginLibraryPhoto` accepts metadata, payload CRC, name and thumbnail;
`BeginLibraryVideo` accepts metadata, payload CRC, name and duration limit.
Both allocate an ID and create an unpublished `.TMP` file. `AppendMediaBytes`,
`StartMediaVerification` and repeated `VerifyMediaSaveStep` calls use the shared
24 KiB scratch buffer. Only a completely verified and closed file is renamed
to `.H75`. Abort removes the incomplete staging file and preserves existing items.
A returned allocation ID is not confirmation that the save has completed.

Photo containers use a 1536-byte header, version 6 for raw pixels or 7 for
indexed JPEG. Video headers are 512 bytes, version 8 for raw or 9 for indexed
JPEG. Both extend the checked 64-byte media header with a name and a final
header CRC. Raw frames retain their frame checksums; JPEG retains its checked
index and frame records. Full-panel RGB888, RGB565 and baseline JPEG are supported.

`SaveLibraryImage` streams the current RGB888 framebuffer, creates its thumbnail
and verifies the new photo without allocating another frame. The firmware saves
each completed raw TCP photo once, before drawing the connection overlay.
TCP video uploads use the same wire protocol and now publish independent library
items. Completion selects the newly saved item. Legacy direct storage APIs remain
available. A retained legacy video can be adopted once by rename; `VLIB.H75`
records that migration so deleting the imported item remains permanent.

## Selection, slideshow and settings

`Select` chooses a photo; `SelectVideo` chooses a continuously looping video.
`SetSlideshow(enabled, seconds)` advances usable photos in ID order and wraps at
the end. The interval ranges from 1 to 86400 seconds and defaults to 60.
Failed items are excluded from automatic selection using a bounded runtime cache.
Actual pixel loading occurs in `Update` while the caller holds render ownership.

`SetDisplaySettings(maxFps, brightness, nowMs)` controls the video FPS cap
(0 preserves the video's rate; 1–60 limits it) and brightness (0–100 percent).
A successful settings save precedes application of new values.

Selections, slideshow interval, FPS cap and brightness use alternating checked
`GSET_A.H75` / `GSET_B.H75` records. The newest valid generation wins; an invalid
replacement leaves the preceding valid record usable. Missing settings use
built-in defaults. Startup restores selected library content before Wi-Fi and
uses retained legacy content or the flashed image when no library selection is
available. The initial radio setup still releases and remounts SD resources.
