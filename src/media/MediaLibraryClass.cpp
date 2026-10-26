#include "media/include/MediaLibraryClass.hpp"
#include "hub75.hpp"
#include "pico/time.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "config/defines.h"

/**
    Checks whether a media ID is temporarily excluded after a playback failure.

    Selects the bounded photo or video rejection list and searches for the requested ID. These
    exclusions represent failures observed during playback in the current runtime session; they do
    not delete media or rewrite its stored metadata.

    @param id Saved media library identifier; valid IDs are nonzero.
    @param video True selects videos; false selects photos.

    @return true - The media ID is in the temporary rejected-item list.
    @return false - The media ID is not currently rejected.
*/
bool
MediaLibrary::Rejected (
    uint32_t id,
    bool video
    ) const
{
    const uint32_t* entries = nullptr;
    size_t i;

    if (!id) {
        return false;
    }
    if (video) {
        entries = rejectedVideos_;
    } else {
        entries = rejected_;
    }
    for (i = 0; i < LIBRARY_REJECTED_ID_CAPACITY; ++i) {
        if (entries[i] == id) {
            return true;
        }
    }

    return false;
} // Rejected

/**
    Excludes a failed media ID from automatic selection.

    Records a failed ID in the corresponding bounded rejection ring, skipping zero and duplicate
    entries. Automatic selection can then move past a damaged item instead of repeatedly
    attempting it. The ring can replace older exclusions when full, and a later library
    initialization clears these temporary decisions.

    @param id Saved media library identifier; valid IDs are nonzero.
    @param video True selects videos; false selects photos.
*/
void
MediaLibrary::Reject (
    uint32_t id,
    bool video
    )
{
    uint32_t* entries = nullptr;
    uint32_t* positionPointer = nullptr;
    uint32_t* position = nullptr;

    if (!id || Rejected(id, video)) {
        return;
    }
    if (video) {
        entries = rejectedVideos_;
    } else {
        entries = rejected_;
    }
    if (video) {
        positionPointer = &videoRejectPosition_;
    } else {
        positionPointer = &photoRejectPosition_;
    }
    position = &(*positionPointer);
    entries[(*position)++ % LIBRARY_REJECTED_ID_CAPACITY] = id;
} // Reject

/**
    Removes a media ID from the temporary failure exclusions.

    @param id Saved media library identifier; valid IDs are nonzero.
    @param video True selects videos; false selects photos.
*/
void
MediaLibrary::Unreject (
    uint32_t id,
    bool video
    )
{
    uint32_t* entries = nullptr;
    size_t i;

    if (video) {
        entries = rejectedVideos_;
    } else {
        entries = rejected_;
    }
    for (i = 0; i < LIBRARY_REJECTED_ID_CAPACITY; ++i) {
        if (entries[i] == id) {
            entries[i] = 0;
        }
    }
} // Unreject

/**
    Caches the selected media name for status responses.

    Reads the selected photo or video metadata through MemoryManager and retains its bounded name
    for subsequent lightweight status responses. The cache is cleared before lookup, so an
    unavailable item cannot leave a previous item's name visible. This lookup can perform
    synchronous SD I/O and belongs on the foreground side.

    @param id Saved media library identifier; valid IDs are nonzero.
    @param video True selects videos; false selects photos.
*/
void
MediaLibrary::CacheDisplayedName (
    uint32_t id,
    bool video
    )
{
    MemoryManager::LibraryVideo item;
    MemoryManager::LibraryPhoto nextItem;

    displayedName_[0] = 0;
    if (video) {
        item = {};
        if (memory_.GetLibraryVideo(id, item)) {
            std::memcpy(displayedName_, item.name, sizeof(displayedName_));
        }
    } else {
        nextItem = {};
        if (memory_.GetLibraryPhoto(id, nextItem)) {
            std::memcpy(displayedName_, nextItem.name, sizeof(displayedName_));
        }
    }
} // CacheDisplayedName

/**
    Checks whether a usable photo with the requested ID exists.

    @param id Saved media library identifier; valid IDs are nonzero.
    @param requireValid Whether unreadable or previously rejected media must be excluded.

    @return true - The item exists and satisfies the requested validity policy.
    @return false - The ID is missing, unreadable or rejected by the validity policy.
*/
bool
MediaLibrary::Has (
    uint32_t id,
    bool requireValid
    ) const
{
    MemoryManager::LibraryPhoto photo;

    if (!id) {
        return false;
    }
    photo = {};
    return memory_.GetLibraryPhoto(id, photo) && (!requireValid || (photo.valid && !Rejected(id, false)));
} // Has

/**
    Checks whether a usable video with the requested ID exists.

    @param id Saved media library identifier; valid IDs are nonzero.
    @param requireValid Whether unreadable or previously rejected media must be excluded.

    @return true - The item exists and satisfies the requested validity policy.
    @return false - The ID is missing, unreadable or rejected by the validity policy.
*/
bool
MediaLibrary::HasVideo (
    uint32_t id,
    bool requireValid
    ) const
{
    MemoryManager::LibraryVideo video;

    if (!id) {
        return false;
    }
    video = {};
    return memory_.GetLibraryVideo(id, video) && (!requireValid || (video.valid && !Rejected(id, true)));
} // HasVideo

/**
    Finds the next usable photo candidate in one saved-library page.

    Keeps the temporary page buffer in this call so photo and video scans share
    the same stack space instead of reserving both buffers in the caller.

    @param after Exclusive library ID cursor for the page to read.
    @param except Library ID to skip; zero skips no item.
    @param page Output page totals and continuation cursor.
    @param candidate Output usable item ID; zero means no candidate in this page.

    @return true - The page was read, including pages with no usable candidate.
    @return false - Storage could not provide the requested library page.
*/
bool
MediaLibrary::FindNextPhotoCandidate (
    uint32_t after,
    uint32_t except,
    MemoryManager::LibraryPage& page,
    uint32_t& candidate
    ) const
{
    MemoryManager::LibraryPhoto items[MemoryManager::LibraryPageSize];
    size_t count;
    size_t index;

    count = 0;
    candidate = 0;
    if (!memory_.ListLibraryPhotos(items, MemoryManager::LibraryPageSize, count, after, &page)) {
        return false;
    }
    for (index = 0; index < count; ++index) {
        if (items[index].id != except && items[index].valid && !Rejected(items[index].id, false)) {
            candidate = items[index].id;
            break;
        }
    }
    return true;
} // FindNextPhotoCandidate

/**
    Finds the next usable video candidate in one saved-library page.

    Keeps the temporary page buffer in this call so photo and video scans share
    the same stack space instead of reserving both buffers in the caller.

    @param after Exclusive library ID cursor for the page to read.
    @param except Library ID to skip; zero skips no item.
    @param page Output page totals and continuation cursor.
    @param candidate Output usable item ID; zero means no candidate in this page.

    @return true - The page was read, including pages with no usable candidate.
    @return false - Storage could not provide the requested library page.
*/
bool
MediaLibrary::FindNextVideoCandidate (
    uint32_t after,
    uint32_t except,
    MemoryManager::LibraryPage& page,
    uint32_t& candidate
    ) const
{
    MemoryManager::LibraryVideo items[MemoryManager::LibraryPageSize];
    size_t count;
    size_t index;

    count = 0;
    candidate = 0;
    if (!memory_.ListLibraryVideos(items, MemoryManager::LibraryPageSize, count, after, &page)) {
        return false;
    }
    for (index = 0; index < count; ++index) {
        if (items[index].id != except && items[index].valid && !Rejected(items[index].id, true)) {
            candidate = items[index].id;
            break;
        }
    }
    return true;
} // FindNextVideoCandidate

/**
    Finds the next usable library item without retaining the full directory.

    Walks bounded directory pages in ID order, ignoring rejected entries and the optional excluded
    ID. After reaching the end it wraps to the beginning at most once, allowing slideshow/deletion
    selection without retaining the full directory. Each page lookup is synchronous storage work;
    a failed lookup or exhausted search returns no candidate.

    @param after Exclusive library ID cursor; zero starts at the first page.
    @param video True selects videos; false selects photos.
    @param except Library ID to skip; zero skips no item.

    @return id - Nonzero identifier of the selected or next usable library item.
    @return 0 - No matching library item is available or selected.
*/
uint32_t
MediaLibrary::FindNext (
    uint32_t after,
    bool video,
    uint32_t except
    ) const
{
    bool wrapped;
    MemoryManager::LibraryPage page;
    uint32_t candidate;
    uint32_t last;

    wrapped = false;
    for (;;) {
        page = {};
        candidate = 0;
        last = after;
        if (video) {
            if (!FindNextVideoCandidate(after, except, page, candidate)) {
                return 0;
            }
        } else {
            if (!FindNextPhotoCandidate(after, except, page, candidate)) {
                return 0;
            }
        }
        if (candidate) {
            return candidate;
        }
        last = page.nextAfter;
        if (page.hasMore && last > after) {
            after = last;
            continue;
        }
        if (!after || wrapped) {
            return 0;
        }
        wrapped = true;
        after = 0;
    }
} // FindNext

/**
    Finds the first usable photo in the library.

    @return id - Nonzero identifier of the selected or next usable library item.
    @return 0 - No matching library item is available or selected.
*/
uint32_t
MediaLibrary::FirstValid (
    void
    ) const
{
    return FindNext(0, false);
} // FirstValid

/**
    Finds the next usable photo after the supplied ID.

    @param id Saved media library identifier; valid IDs are nonzero.

    @return id - Nonzero identifier of the selected or next usable library item.
    @return 0 - No matching library item is available or selected.
*/
uint32_t
MediaLibrary::Next (
    uint32_t id
    ) const
{
    return FindNext(id, false);
} // Next

/**
    Finds the first usable video in the library.

    @param except Library ID to skip; zero skips no item.

    @return id - Nonzero identifier of the selected or next usable library item.
    @return 0 - No matching library item is available or selected.
*/
uint32_t
MediaLibrary::FirstVideo (
    uint32_t except
    ) const
{
    return FindNext(0, true, except);
} // FirstVideo

/**
    Refreshes the photo count and selection from storage.

    @return true - Library state was loaded or refreshed successfully.
    @return false - The required storage, directory or settings were unavailable.
*/
bool
MediaLibrary::Refresh (
    void
    )
{
    size_t i;

    if (!memory_.ListLibraryPhotos(photos_, MemoryManager::LibraryPageSize, count_, 0, &photoPage_)) {
        error_ = memory_.GetLastError();
        return false;
    }
    for (i = 0; i < count_; ++i) {
        if (Rejected(photos_[i].id, false)) {
            photos_[i].valid = false;
        }
    }

    error_ = "";
    return true;
} // Refresh

/**
    Refreshes the video count and selection from storage.

    @return true - Library state was loaded or refreshed successfully.
    @return false - The required storage, directory or settings were unavailable.
*/
bool
MediaLibrary::RefreshVideos (
    void
    )
{
    size_t i;

    if (!memory_.ListLibraryVideos(videos_, MemoryManager::LibraryPageSize, videoCount_, 0, &videoPage_)) {
        error_ = memory_.GetLastError();
        return false;
    }
    for (i = 0; i < videoCount_; ++i) {
        if (Rejected(videos_[i].id, true)) {
            videos_[i].valid = false;
        }
    }

    error_ = "";
    return true;
} // RefreshVideos

/**
    Restores library selections and display settings after storage mounts.

    Refreshes both media collections and reads persisted display/selection settings after a
    storage mount. Missing selections are repaired from usable entries, and legacy last-video
    storage may be imported into the named library. Runtime rejection lists and displayed-name
    state are reset, display settings are applied, and the selected content is scheduled for a
    foreground reload rather than assumed to already exist in the framebuffer.

    @return true - Library state was loaded or refreshed successfully.
    @return false - The required storage, directory or settings were unavailable.
*/
bool
MediaLibrary::Initialize (
    void
    )
{
    uint32_t imported;
    MemoryManager::GallerySettings selected;

    std::memset(rejected_, 0, sizeof(rejected_));
    std::memset(rejectedVideos_, 0, sizeof(rejectedVideos_));
    photoRejectPosition_ = 0;
    videoRejectPosition_ = 0;
    if (!Refresh() || !RefreshVideos() || !memory_.ReadGallerySettings(settings_)) {
        error_ = memory_.GetLastError();
        return false;
    }
    ApplyDisplaySettings(to_ms_since_boot(get_absolute_time()));
    if (settings_.selectedPhoto && !Has(settings_.selectedPhoto)) {
        settings_.selectedPhoto = FirstValid();
    }
    if (!settings_.selectedPhoto) {
        settings_.slideshow = false;
    }
    if (settings_.selectedVideo && !HasVideo(settings_.selectedVideo)) {
        settings_.selectedVideo = FirstVideo();
    }
    {
        imported = 0;
        if (memory_.ImportLastVideo(imported) && imported) {
            RefreshVideos();
            if (!settings_.selectedPhoto) {
                selected = settings_;
                selected.selectedVideo = imported;
                selected.slideshow = false;
                if (memory_.SaveGallerySettings(selected)) {
                    settings_ = selected;
                }
            }
        }
    }
    reload_ = WantsMedia();
    displayed_ = 0;
    displayedName_[0] = 0;
    return true;
} // Initialize

/**
    Clears storage-dependent playback and library state after a failure.

    Stops saved playback, clears a stored frame and drops cached pages, selections and names so
    callers stop using information from the failed card session. This updates runtime state only;
    it does not attempt settings writes to unavailable storage. A later initialization reloads the
    saved settings after remount.
*/
void
MediaLibrary::StorageUnavailable (
    void
    )
{
    player_.Stop();
    if (upload_.IsStoredFrame()) {
        upload_.ClearStoredFrame();
        std::puts("[playback] built-in (SD unavailable)");
    }
    count_ = videoCount_ = 0;
    photoPage_ = {};
    videoPage_ = {};
    settings_.selectedPhoto = settings_.selectedVideo = 0;
    settings_.slideshow = false;
    displayed_ = 0;
    displayedName_[0] = 0;
    reload_ = clear_ = false;
    error_ = "SD unavailable";
} // StorageUnavailable

/**
    Applies the saved brightness and playback FPS cap.

    @param nowMs Current monotonic time since boot, in milliseconds.
*/
void
MediaLibrary::ApplyDisplaySettings (
    uint32_t nowMs
    )
{
    player_.SetFpsLimit(settings_.maxFps, nowMs);
    setIntensity(float(settings_.brightness) / MAX_BRIGHTNESS_PERCENT_F);
} // ApplyDisplaySettings

/**
    Validates, saves, and applies brightness and FPS settings.

    Validates the requested values and builds a candidate settings record without changing the
    active configuration. Only after the record is saved successfully are brightness and playback
    timing updated. Identical settings return immediately, and a write failure leaves the previous
    runtime values in effect.

    @param maxFps Maximum video frames per second; zero preserves the uploaded rate.
    @param brightness Panel brightness percentage, from 0 to 100.
    @param nowMs Current monotonic time since boot, in milliseconds.

    @return true - The display settings were validated, saved and applied.
    @return false - The values are invalid or the settings could not be saved.
*/
bool
MediaLibrary::SetDisplaySettings (
    uint32_t maxFps,
    uint32_t brightness,
    uint32_t nowMs
    )
{
    MemoryManager::GallerySettings selected;

    if (maxFps > MAX_VIDEO_FPS || brightness > MAX_BRIGHTNESS_PERCENT) {
        error_ = "invalid display settings";
        return false;
    }
    if (maxFps == settings_.maxFps && brightness == settings_.brightness) {
        return true;
    }
    selected = settings_;
    selected.maxFps = maxFps;
    selected.brightness = brightness;
    if (!memory_.SaveGallerySettings(selected)) {
        error_ = memory_.GetLastError();
        return false;
    }
    settings_ = selected;
    ApplyDisplaySettings(nowMs);
    error_ = "";
    std::printf("[display] brightness=%lu%% max_fps=%lu (0=video FPS)\n",
                static_cast<unsigned long>(brightness), static_cast<unsigned long>(maxFps));
    return true;
} // SetDisplaySettings

/**
    Selects a saved photo and leaves slideshow or video playback.

    Checks the requested photo, then saves a candidate selection that disables slideshow and video
    mode. The in-memory selection is changed only after persistence succeeds. Pixel loading is
    deferred through reload_ to the next foreground update, where render ownership is already
    held.

    @param id Saved media library identifier; valid IDs are nonzero.

    @return true - The requested item was selected and its playback settings saved.
    @return false - The item could not be displayed or its selection could not be saved.
*/
bool
MediaLibrary::Select (
    uint32_t id
    )
{
    MemoryManager::GallerySettings selected;

    if (!Has(id)) {
        error_ = "photo unavailable";
        return false;
    }
    selected = settings_;
    selected.selectedPhoto = id;
    selected.slideshow = false;
    selected.selectedVideo = 0;
    if (!memory_.SaveGallerySettings(selected)) {
        error_ = memory_.GetLastError();
        return false;
    }
    settings_ = selected;
    reload_ = true;
    clear_ = false;
    error_ = "";
    return true;
} // Select

/**
    Saves slideshow mode and its photo-change interval.

    Builds and persists the requested slideshow state and interval before applying them. Stopping
    a running slideshow keeps the currently displayed usable photo; starting one clears video mode
    and chooses a valid photo if needed. The next foreground update performs any scheduled image
    load rather than loading pixels inside the settings operation.

    @param enabled Whether to enable the requested behavior.
    @param seconds Delay between slideshow photos, in seconds.

    @return true - The slideshow setting and interval were saved and applied.
    @return false - The interval, available photos or storage prevented the change.
*/
bool
MediaLibrary::SetSlideshow (
    bool enabled,
    uint32_t seconds
    )
{
    MemoryManager::GallerySettings selected;
    const char* slideshowState = nullptr;

    selected = settings_;
    selected.intervalSeconds = seconds;
    selected.slideshow = enabled;
    if (!reload_ && Has(displayed_)) {
        selected.selectedPhoto = displayed_; // Stop keeps the currently shown photo.
    }
    if (enabled) {
        selected.selectedVideo = 0;
    }
    if (enabled && !Has(selected.selectedPhoto)) {
        selected.selectedPhoto = FirstValid();
    }
    if (enabled && !selected.selectedPhoto) {
        error_ = "save a photo first";
        return false;
    }
    if (!memory_.SaveGallerySettings(selected)) {
        error_ = memory_.GetLastError();
        return false;
    }
    settings_ = selected;
    reload_ = reload_ || WantsPhoto();
    error_ = "";
    slideshowState = {};
    if (enabled) {
        slideshowState = "on";
    } else {
        slideshowState = "off";
    }
    std::printf("[playback] slideshow=%s interval=%lus\n", slideshowState,
                static_cast<unsigned long>(seconds));
    return true;
} // SetSlideshow

/**
    Deletes a saved photo and repairs the active selection if needed.

    If the deleted photo is selected, first persists a replacement or disables the slideshow when
    no replacement exists. It then removes the managed file, clears runtime rejection state and
    refreshes the photo page. Deleting the displayed photo schedules replacement loading or
    framebuffer clearing, preventing persisted state from intentionally pointing at a successfully
    removed item.

    @param id Saved media library identifier; valid IDs are nonzero.

    @return true - The item was removed and library state refreshed.
    @return false - Deletion, replacement selection or library refresh failed.
*/
bool
MediaLibrary::Remove (
    uint32_t id
    )
{
    MemoryManager::GallerySettings selected;

    if (!Has(id, false)) {
        error_ = "photo unavailable";
        return false;
    }
    // Persist the new selection before removing a selected file; never leave a
    // successful delete pointing its settings at a nonexistent photo.
    selected = settings_;
    if (selected.selectedPhoto == id) {
        selected.selectedPhoto = FindNext(0, false, id);
        if (!selected.selectedPhoto) {
            selected.slideshow = false;
        }
        if (!memory_.SaveGallerySettings(selected)) {
            error_ = memory_.GetLastError();
            return false;
        }
    }
    if (!memory_.DeleteLibraryPhoto(id)) {
        error_ = memory_.GetLastError();
        return false;
    }
    std::printf("[library] deleted photo id=%lu\n", static_cast<unsigned long>(id));
    settings_ = selected;
    Unreject(id, false);
    if (displayed_ == id) {
        displayed_ = 0;
        reload_ = WantsMedia();
        clear_ = !WantsMedia();
    }
    if (!Refresh()) {
        return false;
    }
    error_ = "";
    return true;
} // Remove

/**
    Updates library selection after a new photo is saved.

    @param id Saved media library identifier; valid IDs are nonzero.

    @return true - The saved item was registered and selected for display.
    @return false - The save, library refresh or selection could not complete.
*/
bool
MediaLibrary::NotifyPhotoSaved (
    uint32_t id
    )
{
    if (!id || id > MemoryManager::MaximumLibraryId) {
        return false;
    }
    Unreject(id, false);
    if (!Refresh()) {
        return false;
    }
    return Select(id);
} // NotifyPhotoSaved

/**
    Saves the current uploaded frame as a named library photo.

    Streams the existing physical framebuffer back through the library-photo save API using a
    borrowed read callback. After save verification, the new ID is registered and selected through
    the normal library notification path. Failure records the storage error and deactivates
    library selection while leaving the live upload's pixels available to the display.

    @return true - The saved item was registered and selected for display.
    @return false - The save, library refresh or selection could not complete.
*/
bool
MediaLibrary::ImportLivePhoto (
    void
    )
{
    uint32_t id;

    id = 0;
    if (!memory_.SaveLibraryImage(
            [](void* context, uint8_t* bytes, size_t count, size_t offset) {
                return static_cast<FrameBufferController*>(context)->ReadFramePixels(bytes, count, offset);
            },
            &upload_, id)) {
        error_ = memory_.GetLastError();
        Deactivate();
        return false;
    }
    return NotifyPhotoSaved(id);
} // ImportLivePhoto

/**
    Leaves the active library mode before external content takes over.

    Clears photo, slideshow and video selection when live content takes control. The cleared
    settings are offered to storage, but runtime library state is released even if that write
    fails, so the new external image is not immediately replaced by a pending library reload.
*/
void
MediaLibrary::Deactivate (
    void
    )
{
    MemoryManager::GallerySettings selected;

    selected = settings_;
    selected.selectedPhoto = 0;
    selected.slideshow = false;
    selected.selectedVideo = 0;
    if (!memory_.SaveGallerySettings(selected)) {
        error_ = memory_.GetLastError();
    }
    settings_ = selected;
    reload_ = clear_ = false;
    displayed_ = 0;
    displayedName_[0] = 0;
} // Deactivate

/**
    Restores selected media and advances timed slideshow changes.

    Runs under foreground render ownership and performs deferred clears, selection reloads and
    timed slideshow transitions. Video mode delegates due frames to MediaPlayer; photo mode waits
    by comparing the slideshow deadline and returns without sleeping while the current photo
    remains due for display.

    Failed items are temporarily rejected and alternative IDs are tried within the known library
    totals. Storage loss stops those attempts immediately so SD recovery can take over. Successful
    loads update the displayed identity/name and timing; exhausting usable content clears the
    relevant selection instead of retrying the same damaged entry on every iteration.

    @param nowMs Current monotonic time since boot, in milliseconds.

    @return true - A new frame or selection was published for display.
    @return false - The displayed frame did not change during this update.
*/
bool
MediaLibrary::Update (
    uint32_t nowMs
    )
{
    uint32_t id;
    size_t attempts;
    size_t i;
    bool changed;
    size_t innerIndex;
    uint32_t nextId;
    size_t nextAttempts;
    size_t nextInnerIndex;

    if (clear_) {
        player_.Stop();
        upload_.ClearStoredFrame();
        clear_ = false;
        displayedName_[0] = 0;
        return true;
    }
    if (!memory_.IsReady()) {
        return false;
    }
    if (WantsVideo()) {
        if (reload_) {
            reload_ = false;
            id = settings_.selectedVideo;
            if (Rejected(id, true)) {
                id = FindNext(id, true);
            }
            for (attempts = 0; id && attempts < videoPage_.total; ++attempts) {
                if (player_.ShowLibraryVideo(id, nowMs)) {
                    displayed_ = 0;
                    CacheDisplayedName(id, true);
                    if (settings_.selectedVideo != id) {
                        settings_.selectedVideo = id;
                        memory_.SaveGallerySettings(settings_);
                    }
                    error_ = "";
                    return true;
                }
                error_ = memory_.GetLastError();
                if (!memory_.IsReady()) {
                    return false;
                }
                Reject(id, true);
                for (i = 0; i < videoCount_; ++i) {
                    if (videos_[i].id == id) {
                        videos_[i].valid = false;
                    }
                }
                if (!memory_.IsReady()) {
                    break;
                }
                id = FindNext(id, true);
            }
            settings_.selectedVideo = 0;
            player_.Stop();
            upload_.ClearStoredFrame();
            return true;
        }
        changed = player_.Update(nowMs);
        if (memory_.IsReady() && !player_.IsPlayingVideo()) {
            Reject(settings_.selectedVideo, true);
            for (innerIndex = 0; innerIndex < videoCount_; ++innerIndex) {
                if (videos_[innerIndex].id == settings_.selectedVideo) {
                    videos_[innerIndex].valid = false;
                }
            }
            reload_ = true;
        }
        return changed;
    }
    if (!WantsPhoto()) {
        return false;
    }
    if (!reload_ && (!settings_.slideshow || uint32_t(nowMs - lastChangeMs_) <
                                                 settings_.intervalSeconds * MILLISECONDS_PER_SECOND_U)) {
        return false;
    }
    if (reload_) {
        nextId = settings_.selectedPhoto;
    } else {
        nextId = Next(displayed_);
    }
    reload_ = false;
    for (nextAttempts = 0; nextId && nextAttempts < photoPage_.total; ++nextAttempts) {
        if (player_.ShowLibraryPhoto(nextId)) {
            displayed_ = nextId;
            CacheDisplayedName(nextId, false);
            lastChangeMs_ = nowMs;
            error_ = "";
            return true;
        }
        error_ = memory_.GetLastError();
        if (!memory_.IsReady()) {
            return false;
        }
        Reject(nextId, false);
        for (nextInnerIndex = 0; nextInnerIndex < count_; ++nextInnerIndex) {
            if (photos_[nextInnerIndex].id == nextId) {
                photos_[nextInnerIndex].valid = false;
            }
        }
        nextId = Next(nextId);
    }

    settings_.selectedPhoto = 0;
    settings_.slideshow = false;
    displayed_ = 0;
    return false;
} // Update

/**
    Returns the active built-in, photo, slideshow, or video mode name.

    @return "video" - A saved video is playing.
    @return "builtin" - The built-in image is displayed.
    @return "slideshow" - Saved photos are advancing on an interval.
    @return "photo" - One saved photo is displayed.
*/
const char*
MediaLibrary::ActiveMode (
    void
    ) const
{
    // Report published playback, not a photo selection awaiting render ownership.
    if (player_.IsPlayingVideo()) {
        return "video";
    }
    if (!upload_.GetUploadedFrameData()) {
        return "builtin";
    }
    if (displayed_ && settings_.slideshow) {
        return "slideshow";
    } else {
        return "photo";
    }
} // ActiveMode

/**
    Selects a saved video for continuous playback.

    Checks the requested video and persists a candidate selection that clears photo and slideshow
    mode. The current runtime selection changes only after the settings write succeeds. The player
    opens and validates the video during the subsequent foreground reload, keeping selection
    mutation separate from pixel loading.

    @param id Saved media library identifier; valid IDs are nonzero.

    @return true - The requested item was selected and its playback settings saved.
    @return false - The item could not be displayed or its selection could not be saved.
*/
bool
MediaLibrary::SelectVideo (
    uint32_t id
    )
{
    MemoryManager::GallerySettings selected;

    if (!HasVideo(id)) {
        error_ = "video unavailable";
        return false;
    }
    selected = settings_;
    selected.selectedPhoto = 0;
    selected.slideshow = false;
    selected.selectedVideo = id;
    if (!memory_.SaveGallerySettings(selected)) {
        error_ = memory_.GetLastError();
        return false;
    }
    settings_ = selected;
    reload_ = true;
    clear_ = false;
    error_ = "";
    return true;
} // SelectVideo

/**
    Updates library selection after a new video is saved.

    @param id Saved media library identifier; valid IDs are nonzero.

    @return true - The saved item was registered and selected for display.
    @return false - The save, library refresh or selection could not complete.
*/
bool
MediaLibrary::NotifyVideoSaved (
    uint32_t id
    )
{
    if (!id || id > MemoryManager::MaximumLibraryId) {
        return false;
    }
    Unreject(id, true);
    return RefreshVideos() && SelectVideo(id);
} // NotifyVideoSaved

/**
    Deletes a saved video and repairs the active selection if needed.

    Chooses and saves a replacement selection before deleting a selected video, falling back to a
    photo when no other video is available. It removes the managed video, stops it if currently
    playing and refreshes the video page. Reload/clear state then lets the foreground display a
    valid replacement without keeping a reader attached to the deleted clip.

    @param id Saved media library identifier; valid IDs are nonzero.

    @return true - The item was removed and library state refreshed.
    @return false - Deletion, replacement selection or library refresh failed.
*/
bool
MediaLibrary::RemoveVideo (
    uint32_t id
    )
{
    MemoryManager::GallerySettings selected;
    bool playing;

    if (!HasVideo(id, false)) {
        error_ = "video unavailable";
        return false;
    }
    selected = settings_;
    if (selected.selectedVideo == id) {
        selected.selectedVideo = FirstVideo(id);
        if (!selected.selectedVideo) {
            selected.selectedPhoto = FirstValid();
        }
        if (!memory_.SaveGallerySettings(selected)) {
            error_ = memory_.GetLastError();
            return false;
        }
    }
    playing = player_.GetLibraryVideoId() == id;
    if (!memory_.DeleteLibraryVideo(id)) {
        error_ = memory_.GetLastError();
        return false;
    }
    std::printf("[library] deleted video id=%lu\n", static_cast<unsigned long>(id));
    settings_ = selected;
    Unreject(id, true);
    if (playing) {
        reload_ = WantsMedia();
        clear_ = !WantsMedia();
    }
    return RefreshVideos();
} // RemoveVideo

/**
    Connects library selection, saved media, and display settings.

    @param memory SD storage manager used for saved media operations.
    @param upload Shared framebuffer owner and photo upload controller.
    @param player Saved photo/video playback controller.
*/
MediaLibrary::MediaLibrary (
    MemoryManager& memory,
    FrameBufferController& upload,
    MediaPlayer& player
    ) : memory_(memory), upload_(upload), player_(player)
{
    return;
} // MediaLibrary

/**
    Returns the configured panel brightness.

    @return value - Configured panel brightness percentage, from 0 to 100.
*/
uint32_t
MediaLibrary::GetBrightness (
    void
    ) const
{
    return settings_.brightness;
} // GetBrightness

/**
    Returns the configured video playback FPS cap.

    @return value - Configured video FPS cap.
    @return 0 - No override is applied; use the frame rate stored with the video.
*/
uint32_t
MediaLibrary::GetMaxFps (
    void
    ) const
{
    return settings_.maxFps;
} // GetMaxFps

/**
    Returns the configured interval between slideshow photos.

    @return value - Configured delay between photos in seconds.
*/
uint32_t
MediaLibrary::GetSlideshowInterval (
    void
    ) const
{
    return settings_.intervalSeconds;
} // GetSlideshowInterval

/**
    Schedules restoration of the selected library media.
*/
void
MediaLibrary::RequestReload (
    void
    )
{
    reload_ = WantsMedia();
} // RequestReload

/**
    Reports whether the library contains any saved photos.

    @return true - The photo directory contains at least one item.
    @return false - The photo directory is empty.
*/
bool
MediaLibrary::HasPhotos (
    void
    ) const
{
    return photoPage_.total != 0;
} // HasPhotos

/**
    Reports whether the selected mode requires a library photo.

    @return true - A saved photo selection is configured.
    @return false - No saved photo is selected.
*/
bool
MediaLibrary::WantsPhoto (
    void
    ) const
{
    return settings_.selectedPhoto != 0;
} // WantsPhoto

/**
    Reports whether the selected mode requires a library video.

    @return true - A saved video selection is configured.
    @return false - No saved video is selected.
*/
bool
MediaLibrary::WantsVideo (
    void
    ) const
{
    return settings_.selectedVideo != 0;
} // WantsVideo

/**
    Reports whether any saved library media should be displayed.

    @return true - A saved photo or video selection is configured.
    @return false - The built-in image is selected instead.
*/
bool
MediaLibrary::WantsMedia (
    void
    ) const
{
    return WantsPhoto() || WantsVideo();
} // WantsMedia
