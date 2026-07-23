#ifndef MUSICBOX_WIFI_DOWNLOADER_H
#define MUSICBOX_WIFI_DOWNLOADER_H

#include <FS.h>

// Ensures `path` (a file or folder path exactly as encoded on an NFC tag, e.g.
// "/music/song.mp3" or "/music/album1") is available locally on `fs`.
//
// If it already exists locally, returns true immediately without touching WiFi.
// Otherwise it connects to WiFi, downloads it from `baseUrl` + path (falling back to
// the compiled-in DOWNLOAD_BASE_URL if `baseUrl` is null/empty - e.g. a tag with no
// baseUrl override in its mode record, see Nfc::PlaybackMode) into a cache folder,
// verifies every downloaded file's size before moving anything into its final place,
// plays audio cues to keep the user informed, and disconnects WiFi again before
// returning. A download that is interrupted or fails verification never touches the
// final location - `path` is only created once everything is confirmed.
//
// `path` is treated as a single mp3 file if it ends in ".mp3", otherwise as a folder.
// Folder downloads require a manifest at <baseUrl><path>/manifest.json:
//   { "files": [ { "name": "01.mp3", "size": 3456789 }, { "name": "02.mp3", "size": 2345678 } ] }
//
// Expects these audio cue files to already exist on the SD card:
//   /sounds/downloading.mp3      - played once a download starts
//   /sounds/download_done.mp3    - played once the download finished successfully
//   /sounds/download_failed.mp3  - played if WiFi or the download failed
//
// Returns true only if `path` is fully available locally when this returns.
bool ensureContentAvailable(fs::FS& fs, const char* path, const char* baseUrl = nullptr);

#endif
