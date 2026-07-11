# MusicBox content tools

Generates the `manifest.json` files `MusicBoxWifiDownloader.cpp` expects, and an
`index.html` for writing NFC tags from a phone via Chrome's Web NFC API.

This script only generates files - it doesn't host anything. Upload the content
root's contents to your own HTTP(S) server afterward.

## Setup

```
uv sync
```

## Usage

1. Organize your MP3s under a content root directory, any number of folder levels
   deep (e.g. `Artist/Album/01.mp3`). A folder with multiple MP3s is played back
   as an album; a single MP3 can also be tagged directly.
2. Generate manifests + the browsing page:
   ```
   uv run main.py generate /path/to/content
   ```
   Only folders that directly contain MP3 files get a `manifest.json` - a folder
   that only holds subfolders (e.g. `Artist/`) does not. `index.html` is written
   to a `nfc-writer/` subfolder of the content root, so it stays out of the raw
   content listing (e.g. `https://.../nfc-writer/`). The paths it writes onto NFC
   tags are still relative to the content root itself, unaffected by this.

   The path is remembered in `config.json` (next to this script), so afterward
   you can just re-run:
   ```
   uv run main.py generate
   ```
   Re-run this whenever you add, remove, or resize MP3s.
3. Upload the whole content root to a server that serves both:
   - **HTTP** - point `DOWNLOAD_BASE_URL` in `credentials.ini` at this address.
     This is what the MusicBox itself downloads from.
   - **HTTPS** - open this address on your phone (Chrome for Android) to write
     NFC tags. Web NFC requires a secure context, so the page will warn you if
     opened over plain HTTP instead.
4. On your phone, open the HTTPS URL, tap "Write tag" next to a folder or track,
   and hold a blank NFC tag to the back of the phone.

Requires these audio cue files to exist on the MusicBox's SD card:
`/sounds/downloading.mp3`, `/sounds/download_done.mp3`, `/sounds/download_failed.mp3`.
