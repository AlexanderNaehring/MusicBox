"""MusicBox content tool.

Scans a content root directory for MP3s (any depth of subfolders, e.g.
Artist/Album/track.mp3) and writes the per-folder manifest.json files that
MusicBoxWifiDownloader.cpp expects, plus an index.html (under the
INDEX_SUBDIR subfolder, kept out of the raw content listing) for browsing
the resulting library from a phone and writing NFC tags for it (via
Chrome's Web NFC API).

This script only generates files; hosting them (HTTP for the MusicBox, HTTPS
for the phone - Web NFC requires a secure context) is up to whatever server
you upload the content root to.

Usage:
    uv run main.py generate /path/to/content
    uv run main.py generate            # reuses the last path (see config.json)
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import dataclass
from html import escape
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
CONFIG_PATH = SCRIPT_DIR / "config.json"

# Must match MusicBoxWifiDownloader.cpp's DOWNLOAD_MANIFEST_NAME.
MANIFEST_NAME = "manifest.json"
INDEX_NAME = "index.html"
# Subfolder (relative to the content root) the tag-writer page is written into, so
# it doesn't sit alongside the raw content listing. Paths written into NFC tags are
# always relative to the content root itself, so this move doesn't affect them.
INDEX_SUBDIR = "nfc-writer"


@dataclass
class FolderEntry:
    # Path relative to the content root, e.g. "/music/album1" or "/" for the root
    # itself. This is exactly the string an NFC tag needs to store for the
    # MusicBox to find this folder (see DOWNLOAD_BASE_URL in the firmware).
    url_path: str
    files: list[tuple[str, int]]  # (filename, size in bytes), sorted by filename


# ---------------------------------------------------------------------------
# Last-used root directory (config.json next to this script)
# ---------------------------------------------------------------------------


def load_last_root() -> Path | None:
    if not CONFIG_PATH.exists():
        return None
    try:
        data = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return None
    last_root = data.get("last_root")
    return Path(last_root) if last_root else None


def save_last_root(root: Path) -> None:
    CONFIG_PATH.write_text(json.dumps({"last_root": str(root)}, indent=2), encoding="utf-8")


def resolve_root(arg_root: Path | None) -> Path:
    if arg_root is not None:
        root = arg_root.resolve()
    else:
        last_root = load_last_root()
        if last_root is None:
            print("No content root given, and none saved yet from a previous run.", file=sys.stderr)
            print("Usage: uv run main.py generate <path-to-content-root>", file=sys.stderr)
            sys.exit(1)
        root = last_root.resolve()

    if not root.is_dir():
        print(f"Error: '{root}' does not exist or is not a directory.", file=sys.stderr)
        sys.exit(1)

    save_last_root(root)
    return root


# ---------------------------------------------------------------------------
# Scanning + manifest generation
# ---------------------------------------------------------------------------


def scan_and_write_manifests(root: Path) -> list[FolderEntry]:
    """Walks `root` (at any depth, e.g. Artist/Album/track.mp3) and writes
    manifest.json into every directory that directly contains .mp3 files -
    and only those. Returns those folders, sorted by path."""
    entries: list[FolderEntry] = []

    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        mp3_names = sorted(f for f in filenames if f.lower().endswith(".mp3"))
        if not mp3_names:
            continue

        dir_path = Path(dirpath)
        rel = dir_path.relative_to(root).as_posix()
        url_path = "/" + rel if rel != "." else "/"

        files = [(name, (dir_path / name).stat().st_size) for name in mp3_names]
        manifest = {"files": [{"name": name, "size": size} for name, size in files]}
        (dir_path / MANIFEST_NAME).write_text(json.dumps(manifest, indent=2), encoding="utf-8")

        entries.append(FolderEntry(url_path=url_path, files=files))

    entries.sort(key=lambda e: e.url_path)
    return entries


# ---------------------------------------------------------------------------
# index.html generation
# ---------------------------------------------------------------------------


def format_size(num_bytes: int) -> str:
    size = float(num_bytes)
    for unit in ("B", "KB", "MB", "GB"):
        if size < 1024 or unit == "GB":
            return f"{size:.0f} {unit}" if unit == "B" else f"{size:.1f} {unit}"
        size /= 1024
    return f"{size:.1f} GB"


def render_index_html(entries: list[FolderEntry]) -> str:
    folder_blocks = []
    for entry in entries:
        track_rows = []
        for name, size in entry.files:
            track_path = entry.url_path.rstrip("/") + "/" + name
            track_rows.append(f"""
        <li class="track">
          <span class="track-name">{escape(name)}</span>
          <span class="track-size">{escape(format_size(size))}</span>
          <button class="write-btn" onclick="writeTag('{escape(track_path, quote=True)}', this)">
            Write tag for this track
          </button>
        </li>""")

        folder_blocks.append(f"""
    <div class="folder">
      <div class="folder-header">
        <span class="folder-path">{escape(entry.url_path)}</span>
        <span class="folder-count">{len(entry.files)} track(s)</span>
      </div>
      <button class="write-btn write-btn-folder"
              onclick="writeFolderTag('{escape(entry.url_path, quote=True)}', this, this.closest('.folder'))">
        Write tag for whole folder
      </button>
      <div class="mode-controls">
        <label><input type="checkbox" class="shuffle-checkbox"> Shuffle</label>
        <label>Auto-sleep (minutes): <input type="number" class="autosleep-input" min="0" step="1" placeholder="0"></label>
      </div>
      <details class="track-details">
        <summary>Tracks</summary>
        <ul class="track-list">{"".join(track_rows)}
        </ul>
      </details>
    </div>""")

    folders_html = "".join(folder_blocks) or '<p class="empty">No MP3 files found yet. Add some to the content root and re-run "generate".</p>'

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>MusicBox NFC Tag Writer</title>
<style>
  * {{ box-sizing: border-box; }}
  body {{
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
    margin: 0;
    padding: 16px;
    background: #f4f4f6;
    color: #1a1a1a;
  }}
  h1 {{ font-size: 1.3em; margin: 0 0 4px; }}
  .subtitle {{ color: #666; margin: 0 0 16px; font-size: 0.9em; }}
  #insecure-warning, #unsupported-warning {{
    display: none;
    background: #fff3cd;
    border: 1px solid #ffe69c;
    color: #664d03;
    padding: 10px 12px;
    border-radius: 8px;
    margin-bottom: 16px;
    font-size: 0.9em;
  }}
  .folder {{
    background: white;
    border-radius: 10px;
    padding: 10px 14px;
    margin-bottom: 10px;
    box-shadow: 0 1px 3px rgba(0,0,0,0.1);
  }}
  .folder-header {{
    display: flex;
    justify-content: space-between;
    align-items: center;
    font-weight: 600;
    padding: 4px 0;
  }}
  .folder-count {{ color: #888; font-weight: 400; font-size: 0.85em; }}
  .mode-controls {{
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 14px;
    margin: 8px 0 2px;
    font-size: 0.85em;
    color: #444;
  }}
  .mode-controls input[type="number"] {{
    width: 60px;
    padding: 4px 6px;
    border: 1px solid #ccc;
    border-radius: 4px;
  }}
  .track-details summary {{
    cursor: pointer;
    color: #667eea;
    font-size: 0.85em;
    padding: 6px 0;
  }}
  .base-url-row {{
    background: white;
    border-radius: 10px;
    padding: 10px 14px;
    margin-bottom: 16px;
    box-shadow: 0 1px 3px rgba(0,0,0,0.1);
  }}
  .base-url-row label {{
    display: block;
    font-weight: 600;
    margin-bottom: 6px;
  }}
  .base-url-row input[type="text"] {{
    width: 100%;
    padding: 8px 10px;
    border: 1px solid #ccc;
    border-radius: 6px;
    font-size: 0.9em;
  }}
  .base-url-row .hint {{ color: #888; font-size: 0.8em; margin: 6px 0 0; }}
  .track-list {{ list-style: none; padding: 0; margin: 8px 0 4px; }}
  .track {{
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 6px 0;
    border-top: 1px solid #eee;
    flex-wrap: wrap;
  }}
  .track-name {{ flex: 1; min-width: 120px; }}
  .track-size {{ color: #888; font-size: 0.8em; }}
  .write-btn {{
    background: #667eea;
    color: white;
    border: none;
    border-radius: 6px;
    padding: 8px 12px;
    font-size: 0.85em;
    cursor: pointer;
  }}
  .write-btn:active {{ background: #5568d3; }}
  .write-btn-folder {{ margin: 6px 0; }}
  .empty {{ color: #666; }}
</style>
</head>
<body>
  <h1>MusicBox NFC Tag Writer</h1>
  <p class="subtitle">Tap a button, then hold a blank NFC tag to the back of your phone.</p>

  <div id="insecure-warning">
    This page was opened over plain HTTP. Web NFC only works on HTTPS (or localhost) -
    open it via an https:// URL instead.
  </div>
  <div id="unsupported-warning">
    This browser doesn't support Web NFC. Open this page in Chrome for Android.
  </div>

  <div class="base-url-row">
    <label for="base-url-override">Server base URL override</label>
    <input type="text" id="base-url-override"
           placeholder="leave blank for firmware default">
    <p class="hint">
      Written onto every tag as download location, overwrites firmware default.
    </p>
  </div>

  {folders_html}

<script>
  if (!window.isSecureContext) {{
    document.getElementById('insecure-warning').style.display = 'block';
  }}
  if (!('NDEFReader' in window)) {{
    document.getElementById('unsupported-warning').style.display = 'block';
  }}

  const BASE_URL_STORAGE_KEY = 'musicbox-base-url-override';
  const baseUrlInput = document.getElementById('base-url-override');
  baseUrlInput.value = localStorage.getItem(BASE_URL_STORAGE_KEY) || '';
  baseUrlInput.addEventListener('input', () => {{
    localStorage.setItem(BASE_URL_STORAGE_KEY, baseUrlInput.value.trim());
  }});

  async function performWrite(records, btn) {{
    if (!('NDEFReader' in window)) {{
      alert('Web NFC is not supported in this browser. Use Chrome for Android.');
      return;
    }}
    const originalLabel = btn.textContent;
    try {{
      const ndef = new NDEFReader();
      btn.disabled = true;
      btn.textContent = 'Hold phone to tag...';
      await ndef.write({{ records }});
      btn.textContent = 'Tag written';
      setTimeout(() => {{ btn.textContent = originalLabel; btn.disabled = false; }}, 2500);
    }} catch (err) {{
      alert('Failed to write NFC tag: ' + err);
      btn.textContent = originalLabel;
      btn.disabled = false;
    }}
  }}

  // Builds the optional 2nd NDEF record (a JSON object matching
  // Nfc::PlaybackMode in the firmware) from shuffle/auto-sleep (folder writes
  // only) and the shared base URL override (every write) - null if none of
  // those are set, so a plain tag stays a single record like before.
  function buildModeRecord({{ shuffle = false, autoSleepMinutes = 0 }} = {{}}) {{
    const baseUrl = baseUrlInput.value.trim();
    if (!shuffle && autoSleepMinutes <= 0 && !baseUrl) return null;
    const mode = {{}};
    if (shuffle) mode.shuffle = true;
    if (autoSleepMinutes > 0) mode.autoSleepMinutes = autoSleepMinutes;
    if (baseUrl) mode.baseUrl = baseUrl;
    return {{ recordType: 'text', data: JSON.stringify(mode) }};
  }}

  function writeTag(path, btn) {{
    const records = [{{ recordType: 'text', data: path }}];
    const modeRecord = buildModeRecord();
    if (modeRecord) records.push(modeRecord);
    return performWrite(records, btn);
  }}

  function writeFolderTag(path, btn, folderEl) {{
    const shuffle = folderEl.querySelector('.shuffle-checkbox').checked;
    const autoSleepMinutes = parseInt(folderEl.querySelector('.autosleep-input').value, 10) || 0;
    const records = [{{ recordType: 'text', data: path }}];
    const modeRecord = buildModeRecord({{ shuffle, autoSleepMinutes }});
    if (modeRecord) records.push(modeRecord);
    return performWrite(records, btn);
  }}
</script>
</body>
</html>
"""


# ---------------------------------------------------------------------------
# generate command
# ---------------------------------------------------------------------------


def cmd_generate(root: Path) -> list[FolderEntry]:
    entries = scan_and_write_manifests(root)

    # Tucked into a subfolder rather than the content root itself, so it doesn't
    # show up alongside the raw MP3/manifest listing. The paths it writes into NFC
    # tags are unaffected - they're relative to the content root, not to this page.
    index_dir = root / INDEX_SUBDIR
    index_dir.mkdir(parents=True, exist_ok=True)
    index_path = index_dir / INDEX_NAME
    index_path.write_text(render_index_html(entries), encoding="utf-8")

    print(f"Scanned '{root}': {len(entries)} folder(s) with MP3 files.")
    for entry in entries:
        print(f"  {entry.url_path}  ({len(entry.files)} file(s))")
    print(f"Wrote {index_path}")
    return entries


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="command", required=True)

    generate_parser = subparsers.add_parser(
        "generate", help="Scan a content root and (re)write manifest.json + index.html"
    )
    generate_parser.add_argument(
        "root", nargs="?", type=Path, default=None,
        help="Content root directory. If omitted, reuses the last one used (see config.json).",
    )

    args = parser.parse_args()

    if args.command == "generate":
        root = resolve_root(args.root)
        cmd_generate(root)


if __name__ == "__main__":
    sys.exit(main())
