#pragma once
#include "media/include/JsonWriterClass.hpp"
#include "sd/include/MemoryManagerClass.hpp"
#include "network/include/FrameBufferControllerClass.hpp"
#include "media/include/MediaPlayerClass.hpp"
#include "config/defines.h"

/**
    Coordinates the user's saved-media selection and the panel's active display mode.

    It connects the SD-backed photo and video collections to single-photo display, slideshows and
    looping video playback, while preserving selections and display settings across restarts. It
    uses bounded pages while searching library items, without retaining a complete directory.
    MemoryManager owns filesystem access and MediaPlayer handles frame playback; this class
    decides what should be shown and when a selection should change. Foreground updates require
    render ownership, and storage-loss handling clears stale runtime state without discarding
    saved settings on the card.
*/
class MediaLibrary {
public:
    // Connects library selection, saved media, and display settings.
    MediaLibrary(MemoryManager& memory, FrameBufferController& upload, MediaPlayer& player);

    // Restores library selections and display settings after storage mounts.
    bool Initialize(void);

    // Clears storage-dependent playback and library state after a failure.
    void StorageUnavailable(void); // Foreground, under render ownership; preserve SD settings.

    // Refreshes the photo count and selection from storage.
    bool Refresh(void);

    // Refreshes the video count and selection from storage.
    bool RefreshVideos(void);

    // Selects a saved video for continuous playback.
    bool SelectVideo(uint32_t id);

    // Deletes a saved video and repairs the active selection if needed.
    bool RemoveVideo(uint32_t id);

    // Updates library selection after a new video is saved.
    bool NotifyVideoSaved(uint32_t id);

    // Selects a saved photo and leaves slideshow or video playback.
    bool Select(uint32_t id);

    // Saves slideshow mode and its photo-change interval.
    bool SetSlideshow(bool enabled, uint32_t seconds);

    // Validates, saves, and applies brightness and FPS settings.
    bool SetDisplaySettings(uint32_t maxFps, uint32_t brightness, uint32_t nowMs);

    // Returns the configured panel brightness.
    uint32_t GetBrightness(void) const;

    // Returns the configured video playback FPS cap.
    uint32_t GetMaxFps(void) const;

    // Returns the configured interval between slideshow photos.
    uint32_t GetSlideshowInterval(void) const;

    // Deletes a saved photo and repairs the active selection if needed.
    bool Remove(uint32_t id);

    // Updates library selection after a new photo is saved.
    bool NotifyPhotoSaved(uint32_t id);

    // Saves the current uploaded frame as a named library photo.
    bool ImportLivePhoto(void);

    // Leaves the active library mode before external content takes over.
    void Deactivate(void);

    // Schedules restoration of the selected library media.
    void RequestReload(void);

    // Reports whether the library contains any saved photos.
    bool HasPhotos(void) const;

    // Reports whether the selected mode requires a library photo.
    bool WantsPhoto(void) const;

    // Reports whether the selected mode requires a library video.
    bool WantsVideo(void) const;

    // Reports whether any saved library media should be displayed.
    bool WantsMedia(void) const;

    // Restores selected media and advances timed slideshow changes.
    bool Update(uint32_t nowMs);

    // Returns the active built-in, photo, slideshow, or video mode name.
    const char* ActiveMode(void) const;

    // Formats one page of saved videos and their metadata as JSON.
    bool VideoOverview(char* json, size_t capacity, uint32_t after = 0);

    // Formats one page of saved photos and their metadata as JSON.
    bool Overview(char* json, size_t capacity, uint32_t after = 0);

    // Returns a saved media preview as a JSON pixel response.
    bool Thumbnail(uint32_t id, char* json, size_t capacity, bool video = false);

    // Formats the current display selection and saved controls as JSON.
    bool PlaybackStatus(char* json, size_t capacity) const;

    // Formats managed SD storage capacity and consumption as JSON.
    bool MemoryStatus(char* json, size_t capacity);

private:
    // Calculates the saved video duration in milliseconds.
    static uint64_t CalculateVideoDurationMs(const MediaMetadata& metadata);

    MemoryManager& memory_;
    FrameBufferController& upload_;
    MediaPlayer& player_;
    MemoryManager::LibraryPhoto photos_[MemoryManager::LibraryPageSize]{};
    MemoryManager::GallerySettings settings_;
    MemoryManager::LibraryVideo videos_[MemoryManager::LibraryPageSize]{};
    size_t count_ = 0;
    size_t videoCount_ = 0;
    MemoryManager::LibraryPage photoPage_;
    MemoryManager::LibraryPage videoPage_;
    // Recent failures are a bounded cache, never a limit on library membership.
    uint32_t rejectedVideos_[LIBRARY_REJECTED_ID_CAPACITY]{};
    uint32_t rejected_[LIBRARY_REJECTED_ID_CAPACITY]{};
    uint32_t photoRejectPosition_ = 0;
    uint32_t videoRejectPosition_ = 0;
    uint32_t displayed_ = 0;
    uint32_t lastChangeMs_ = 0;
    char displayedName_[MemoryManager::PhotoNameBytes]{};
    bool reload_ = false;
    bool clear_ = false;
    const char* error_ = "";
    // Finds the first usable photo in the library.
    uint32_t FirstValid(void) const;

    // Finds the next usable photo after the supplied ID.
    uint32_t Next(uint32_t id) const;

    // Checks whether a usable photo with the requested ID exists.
    bool Has(uint32_t id, bool requireValid = true) const;

    // Checks whether a usable video with the requested ID exists.
    bool HasVideo(uint32_t id, bool requireValid = true) const;

    // Finds the first usable video in the library.
    uint32_t FirstVideo(uint32_t except = 0) const;

    // Checks whether a media ID is temporarily excluded after a playback failure.
    bool Rejected(uint32_t id, bool video) const;

    // Excludes a failed media ID from automatic selection.
    void Reject(uint32_t id, bool video);

    // Removes a media ID from the temporary failure exclusions.
    void Unreject(uint32_t id, bool video);

    // Finds the next usable photo candidate in one saved-library page.
    bool FindNextPhotoCandidate(uint32_t after, uint32_t except, MemoryManager::LibraryPage& page, uint32_t& candidate) const;

    // Finds the next usable video candidate in one saved-library page.
    bool FindNextVideoCandidate(uint32_t after, uint32_t except, MemoryManager::LibraryPage& page, uint32_t& candidate) const;

    // Finds the next usable library item without retaining the full directory.
    uint32_t FindNext(uint32_t after, bool video, uint32_t except = 0) const;

    // Caches the selected media name for status responses.
    void CacheDisplayedName(uint32_t id, bool video);

    // Applies the saved brightness and playback FPS cap.
    void ApplyDisplaySettings(uint32_t nowMs);

};
