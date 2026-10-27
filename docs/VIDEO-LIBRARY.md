# Saved video library

Video uploads create independent SD items with an ID, name, format, frame count,
rational FPS and loop/ping-pong mode. A supplied upload-duration limit is retained
in milliseconds. The current TCP protocol supplies no name or duration override,
so its items use `Uploaded video` and a zero duration-limit field.

`BeginLibraryVideo` starts a staging transaction. The existing append/readback
verification API publishes it by rename only after complete success.
`ListLibraryVideos` provides cursor pagination; `GetLibraryVideo` inspects one
entry and `OpenLibraryVideo` selects its checked frame stream. No clip-length
buffer or fixed five-video limit is needed.

`MediaLibrary::SelectVideo` persists the chosen ID. `Update` opens it under render
ownership and loops using the stored playback mode. `RemoveVideo` repairs the
selection before deleting the file, choosing another usable video or a photo.
A retained legacy video can be adopted once without copying its payload.

See [saved media library](PHOTO-LIBRARY.md) for container versions, settings,
publication rules and storage capacity, and [video upload](VIDEO-UPLOAD.md)
for the TCP protocol.

Browser video uploads use the same collection. Their named start message stores
the chosen name and duration limit, and JPEG descriptor version 3 preserves the
encoder quality. Blank browser names use the source filename. Completion selects
the verified video; reception progress alone does not change the active selection.
