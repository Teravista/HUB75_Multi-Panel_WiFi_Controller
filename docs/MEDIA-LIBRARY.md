# Saved media library

`MemoryManager` stores independent photos and videos on the SD card. `MediaLibrary`
selects content, advances slideshows and persists display settings; `MediaPlayer` reads
and publishes the selected frames under framebuffer ownership. Filesystem work stays in
the foreground, outside TCP callbacks.

## Items and pagination

Each photo or video has a nonzero ID, a name of up to 63 ASCII characters, its own media
descriptor and checksums. Photos also carry a 16 × 32 RGB565 thumbnail. Video metadata
includes frame count, rational FPS, loop/ping-pong mode, format and the supplied
maximum-duration setting in milliseconds.

`ListLibraryPhotos` and `ListLibraryVideos` return ordered pages. Pass the previous
`nextAfter` cursor to request the next page; `total` and `hasMore` describe the
collection. The controller retains ten entries per page rather than a complete
directory. Capacity is bounded by card space, filesystem directory capacity and the
28-bit filename ID namespace. Deleted low IDs can be reused.

`GetLibraryPhoto` / `GetLibraryVideo` inspect one entry. `OpenLibraryPhoto` /
`OpenLibraryVideo`, followed by `ReadMediaFrame`, load checked pixels.
`DeleteLibraryPhoto` / `DeleteLibraryVideo` remove managed files; selection repair
belongs to `MediaLibrary::Remove` / `RemoveVideo`.

## Saving and publication

`BeginLibraryPhoto` accepts metadata, payload CRC, name and thumbnail;
`BeginLibraryVideo` accepts metadata, payload CRC, name and duration limit. Both
allocate an ID and create an unpublished `.TMP` file. `AppendMediaBytes`,
`StartMediaVerification` and repeated `VerifyMediaSaveStep` calls use the shared 24 KiB
scratch buffer. Only a completely verified and closed file is renamed to `.H75`. Abort
removes the incomplete staging file and preserves existing items. A returned allocation
ID is not confirmation that the save has completed.

Photo containers use a 1536-byte header, version 6 for raw pixels or 7 for indexed JPEG.
Video headers are 512 bytes, version 8 for raw or 9 for indexed JPEG. Both extend the
checked 64-byte media header with a name and a final header CRC. Raw frames retain their
frame checksums; JPEG retains its checked index and frame records. RGB888, RGB565 and
baseline JPEG support full-panel or smaller placed rectangles.

`SaveLibraryImage` streams the current RGB888 framebuffer, creates its thumbnail and
verifies the new photo without allocating another frame. The firmware saves each
completed raw TCP photo once, before drawing the connection overlay. TCP video uploads
publish independent library items. Completion selects the newly saved item. Legacy
direct storage APIs remain available. A retained legacy video can be adopted once by
rename; `VLIB.H75` records that migration so deleting the imported item remains
permanent.

## Selection, slideshow and settings

`Select` chooses a photo; `SelectVideo` chooses a continuously looping video.
`SetSlideshow(enabled, seconds)` advances usable photos in ID order and wraps at the
end. The interval ranges from 1 to 86400 seconds and defaults to 60. Failed items are
excluded from automatic selection using a bounded runtime cache. Actual pixel loading
occurs in `Update` while the caller holds render ownership.

`SetDisplaySettings(maxFps, brightness, nowMs)` controls the video FPS cap (0 preserves
the video's rate; 1–60 limits it) and brightness (0–100 percent). A successful settings
save precedes application of new values.

Selections, slideshow interval, FPS cap and brightness use alternating checked
`GSET_A.H75` / `GSET_B.H75` records. The newest valid generation wins; an invalid
replacement leaves the preceding valid record usable. Missing settings use built-in
defaults. Startup restores selected library content before Wi-Fi and uses retained
legacy content or the flashed image when no library selection is available. Radio
initialization releases and remounts SD resources during startup.

## Browser collection and previews

The browser lists ten items per page with cursor navigation, saved names, format,
dimensions and JPEG quality (`XX` for older unknown values). Items can be selected or
deleted, including removal of a damaged header. Photo controls also enable the slideshow
and edit its interval. Each upload retains its own format and
[placement](../web/README.md#upload-placement).

`ReadLibraryPreview` verifies and samples a photo or the first video frame into 32 × 64
RGB565 pixels. A separate file handle avoids changing the active frame position or JPEG
index cache. Preview requests generate black borders from placement metadata and reject
corrupted payloads. `Thumbnail` encodes this sample as bounded base64 JSON; the original
16 × 32 on-disk thumbnail remains compatible.

`GetStorageUsage` measures logical managed-file bytes, cluster-rounded allocated bytes
and filesystem data capacity. This includes the collections, settings, retained copies
and staging files, excluding unrelated files and filesystem metadata. JSON responses are
bounded, escape names and return failure when a complete page or preview cannot fit.

## Saved video selection

Video uploads create independent SD items with an ID, name, format, frame count,
rational FPS and loop/ping-pong mode. A supplied upload-duration limit is retained in
milliseconds. The current TCP protocol supplies no name or duration override, so its
items use `Uploaded video` and a zero duration-limit field.

`BeginLibraryVideo` starts a staging transaction. The existing append/readback
verification API publishes it by rename only after complete success. `ListLibraryVideos`
provides cursor pagination; `GetLibraryVideo` inspects one entry and `OpenLibraryVideo`
selects its checked frame stream. Frames stream through bounded buffers; capacity
follows available storage.

`MediaLibrary::SelectVideo` persists the chosen ID. `Update` opens it under render
ownership and loops using the stored playback mode. `RemoveVideo` repairs the selection
before deleting the file, choosing another usable video or a photo. A retained legacy
video can be adopted once without copying its payload.

See [video upload](VIDEO-UPLOAD.md) for the TCP protocol.

Browser video uploads use the same collection. Their named start message stores the
chosen name and duration limit, and JPEG descriptor version 3 preserves the encoder
quality. Blank browser names use the source filename. Completion selects the verified
video; reception progress alone does not change the active selection.

The Videos tab lists ten clips per page with names, format, dimensions, FPS, saved
duration, loop mode, encoded size and JPEG quality (`XX` when unknown). First-frame
previews do not replace the active playback handle or index cache. **Show** selects a
continuously looping clip; **Delete** repairs the selection before removal. Fill/crop,
fit and draggable manual zoom preserve each item's own [stored
placement](../web/README.md#upload-placement).

## Display settings records

`maxFps=0` preserves each video's saved rate; values 1–60 use the lower of the cap and
that rate. The cap can slow playback but does not accelerate a slower clip or rewrite
its metadata. Changing it retains the timeline position. Brightness ranges from 0–100
percent, defaults to 80, and affects all pixels and overlays through driver intensity
rather than pixel rewriting. At zero the controller and web server continue running.

Settings use alternating checksummed records. Current 40-byte `H75C` records keep
selection/slideshow fields, video ID at offset 20, FPS cap at 24, brightness at 28,
reserved zero at 32 and CRC32 at 36. Older 24-byte `H75S` and 32-byte `H75G` records
load with no FPS cap and 80% brightness. Corrupt newest settings fall back to the
previous valid record. Failed persistence does not apply live changes; repeated
identical settings avoid another write. Upload ownership rejects competing changes.
