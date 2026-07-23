#include "MusicBoxWifiDownloader.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include "Board.h"

#define LOG_TAG "WifiDownloader"

#ifndef WIFI_SSID
#define WIFI_SSID "<please set your SSID>"
#define WIFI_PASSWORD "<please set your Wifi password>"
#endif

#ifndef DOWNLOAD_BASE_URL
#define DOWNLOAD_BASE_URL "http://<please set your download server URL>"
#endif

#define DOWNLOAD_CACHE_DIR "/download_cache"
#define DOWNLOAD_MANIFEST_NAME "manifest.json"

#define SOUND_DOWNLOAD_START "/sounds/downloading.mp3"
#define SOUND_DOWNLOAD_SUCCESS "/sounds/download_done.mp3"
#define SOUND_DOWNLOAD_FAILED "/sounds/download_failed.mp3"

#define WIFI_CONNECT_TIMEOUT_MS 15000
#define DOWNLOAD_STALL_TIMEOUT_MS 10000
#define DOWNLOAD_READ_BUFFER_SIZE 4096


// Classic color-wheel step -> RGB (0/85/170 are pure blue/green/red, smoothly
// blended between). Used to cycle the LED through the rainbow while data is
// actively arriving.
static void rainbowColor(uint8_t wheelPos, uint8_t& r, uint8_t& g, uint8_t& b) {
  wheelPos = 255 - wheelPos;
  if (wheelPos < 85) {
    r = 255 - wheelPos * 3;
    g = 0;
    b = wheelPos * 3;
  } else if (wheelPos < 170) {
    wheelPos -= 85;
    r = 0;
    g = wheelPos * 3;
    b = 255 - wheelPos * 3;
  } else {
    wheelPos -= 170;
    r = wheelPos * 3;
    g = 255 - wheelPos * 3;
    b = 0;
  }
}

// ---- path helpers -------------------------------------------------------------------

static String parentPath(const String& path) {
  int idx = path.lastIndexOf('/');
  if (idx <= 0) return "/";
  return path.substring(0, idx);
}

static bool isMp3Path(const String& path) {
  String lower = path;
  lower.toLowerCase();
  return lower.endsWith(".mp3");
}

// Percent-encodes everything except unreserved characters and '/', so folder/file
// names with spaces or other special characters (e.g. "My Folder") survive as a
// valid request URI instead of breaking the HTTP request line. '/' is left alone
// since it's a legitimate path separator here, not part of a name.
static String urlEncodePath(const String& path) {
  String encoded;
  encoded.reserve(path.length());
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < path.length(); i++) {
    char c = path[i];
    bool isUnreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~' ||
                        c == '/';
    if (isUnreserved) {
      encoded += c;
    } else {
      encoded += '%';
      encoded += hex[(uint8_t)c >> 4];
      encoded += hex[(uint8_t)c & 0x0F];
    }
  }
  return encoded;
}

static void deleteRecursive(fs::FS& fs, const String& path) {
  if (!fs.exists(path)) return;
  File file = fs.open(path);
  if (!file) return;
  if (file.isDirectory()) {
    File child = file.openNextFile();
    while (child) {
      String childPath = path + "/" + String(child.name());
      bool childIsDir = child.isDirectory();
      child.close();
      if (childIsDir) {
        deleteRecursive(fs, childPath);
      } else {
        fs.remove(childPath);
      }
      child = file.openNextFile();
    }
    file.close();
    fs.rmdir(path);
  } else {
    file.close();
    fs.remove(path);
  }
}

// SD's mkdir() only creates a single directory level and fails if its parent doesn't
// exist yet, so nested cache/target paths need each intermediate directory created in
// order.
static bool mkdirRecursive(fs::FS& fs, const String& path) {
  if (path.length() == 0 || path == "/") return true;
  int start = path.startsWith("/") ? 1 : 0;
  int idx;
  while ((idx = path.indexOf('/', start)) != -1) {
    String segment = path.substring(0, idx);
    if (segment.length() > 0 && !fs.exists(segment) && !fs.mkdir(segment)) {
      LOGF("mkdirRecursive: failed to create '%s'\n", segment.c_str());
      return false;
    }
    start = idx + 1;
  }
  if (!fs.exists(path) && !fs.mkdir(path)) {
    LOGF("mkdirRecursive: failed to create '%s'\n", path.c_str());
    return false;
  }
  return true;
}

// ---- WiFi lifecycle -------------------------------------------------------------------

static bool connectWifi() {
  LOGF("connecting to WiFi SSID '%s'...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      LOGLN("WiFi connection timed out");
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      return false;
    }
    delay(100);
  }
  LOGF("WiFi connected, IP %s\n", WiFi.localIP().toString().c_str());

  // ESP32 WiFi defaults to modem-sleep power saving, which periodically naps the
  // radio between beacon intervals - fine for idle standby, but it measurably
  // throttles sustained downloads. We're about to do real transfer work and will
  // disconnect right after, so trade power for throughput here.
  WiFi.setSleep(false);

  return true;
}

static void disconnectWifi() {
  LOGLN("disconnecting WiFi...");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// ---- HTTP download -------------------------------------------------------------------

// Downloads `url` to `localPath`, verifying the number of bytes written against
// `expectedSize` (or, if 0, against the response's Content-Length header - at least one
// of the two must be known, otherwise the download is rejected). Gives up if no data
// arrives for a while (possible WiFi drop).
//
// Resume (only when `expectedSize` is known, i.e. the manifest/folder path): if
// `localPath` already has some but not all of `expectedSize` on disk, this asks the
// server for just the remaining bytes (HTTP Range) and appends, instead of
// redownloading the whole file - the point of it, for a large single-file audiobook
// that fails partway through. If the server ignores the Range header (not all static
// hosts support it) and sends the full body from byte 0 instead, it falls back to a
// full restart automatically. On failure, the partial file is kept (not deleted) so a
// later retry can resume from it; only a caller that never resumes (expectedSize == 0)
// gets today's delete-on-failure behavior, since it has no way to make use of it.
static bool httpDownloadToFile(fs::FS& fs, const String& url, const String& localPath,
                               size_t expectedSize) {
  size_t existingSize = 0;
  if (fs.exists(localPath)) {
    File existing = fs.open(localPath);
    if (existing) {
      existingSize = existing.size();
      existing.close();
    }
  }

  if (expectedSize > 0 && existingSize == expectedSize) {
    LOGF("'%s' already complete, skipping\n", localPath.c_str());
    return true;
  }
  // Bigger than expected shouldn't happen (a complete file is caught above, and
  // httpDownloadToFile never writes past expectedSize) - but if it does, e.g. a
  // manifest changed size for the same name, treat it as corrupt and start over.
  if (expectedSize > 0 && existingSize > expectedSize) existingSize = 0;

  bool resume = expectedSize > 0 && existingSize > 0;

  LOGF("%s '%s' -> '%s'\n", resume ? "resuming" : "downloading", url.c_str(),
       localPath.c_str());

  HTTPClient http;
  if (!http.begin(url)) {
    LOGF("failed to begin request for '%s'\n", url.c_str());
    return false;
  }
  if (resume) {
    http.addHeader("Range", "bytes=" + String(existingSize) + "-");
  }

  int httpCode = http.GET();
  if (resume && httpCode == HTTP_CODE_OK) {
    // Server ignored the Range request and is sending the whole file from byte 0 -
    // falling back to a full restart is the only safe option (appending it after our
    // existing bytes would corrupt the file).
    LOGLN("server ignored Range request, restarting from scratch");
    resume = false;
    existingSize = 0;
  } else if (httpCode != HTTP_CODE_OK && httpCode != 206 /* Partial Content */) {
    LOGF("GET '%s' failed, HTTP code %d\n", url.c_str(), httpCode);
    http.end();
    return false;
  }

  int contentLength = http.getSize();  // bytes remaining in *this* response only
  size_t targetSize = expectedSize > 0      ? expectedSize
                      : (contentLength > 0) ? (size_t)(existingSize + contentLength)
                                            : 0;

  File outFile = fs.open(localPath, resume ? FILE_APPEND : FILE_WRITE);
  if (!outFile) {
    LOGF("failed to open '%s' for writing\n", localPath.c_str());
    http.end();
    return false;
  }
  // Batches small writes into fewer, larger physical SD writes instead of issuing
  // one for every DOWNLOAD_READ_BUFFER_SIZE chunk.
  outFile.setBufferSize(DOWNLOAD_READ_BUFFER_SIZE);

  auto* stream = http.getStreamPtr();
  static uint8_t buf[DOWNLOAD_READ_BUFFER_SIZE];  // static: keep it off the stack
  size_t totalWritten = existingSize;             // overall file size, for progress/verification
  size_t sessionWritten = 0;                      // bytes received in this response only
  unsigned long lastDataMillis = millis();
  int lastReportedPercent = -5;  // so 0% still gets printed on first progress
  uint8_t rainbowStep = 0;

  while (http.connected() && (contentLength <= 0 || sessionWritten < (size_t)contentLength)) {
    size_t toRead = sizeof(buf);
    if (contentLength > 0) {
      size_t remaining = (size_t)contentLength - sessionWritten;
      if (remaining < toRead) toRead = remaining;
    }
    // readBytes() does an efficient bulk read of whatever has arrived (up to
    // toRead), blocking briefly if nothing has yet - no need to pre-check
    // available() and cap to it ourselves, which only forced smaller, more
    // frequent reads than the connection could actually deliver.
    int readBytes = stream->readBytes(buf, toRead);
    if (readBytes > 0) {
      outFile.write(buf, readBytes);
      totalWritten += readBytes;
      sessionWritten += readBytes;
      lastDataMillis = millis();

      // Only advances on actual data arrival, so the color freezes in place
      // by itself while nothing is coming in (i.e. while stalled).
      uint8_t r, g, b;
      rainbowColor(rainbowStep++, r, g, b);
      board.setLED(r, g, b);

      if (targetSize > 0) {
        int percent = (int)((totalWritten * 100) / targetSize);
        int percentBucket = (percent / 5) * 5;
        if (percentBucket > lastReportedPercent) {
          lastReportedPercent = percentBucket;
          LOGF("%s - %d%% (%u/%u bytes)\n", localPath.c_str(), percentBucket,
               (unsigned)totalWritten, (unsigned)targetSize);
        }
      }
    } else {
      if (millis() - lastDataMillis > DOWNLOAD_STALL_TIMEOUT_MS) {
        LOGF("download stalled, aborting '%s'\n", url.c_str());
        board.setLED(RGB_Error);
        break;
      }
      delay(10);
    }
  }

  outFile.close();
  http.end();

  LOGF("wrote %u bytes (expected %u) for '%s'\n", (unsigned)totalWritten, (unsigned)targetSize,
       localPath.c_str());

  if (targetSize == 0 || totalWritten != targetSize) {
    if (expectedSize > 0) {
      LOGF("incomplete download, keeping '%s' to resume later\n", localPath.c_str());
    } else {
      LOGF("incomplete/unverified download, removing '%s'\n", localPath.c_str());
      fs.remove(localPath);
    }
    return false;
  }

  return true;
}

// ---- single-file / folder downloads -------------------------------------------------

static bool downloadFile(fs::FS& fs, const String& remotePath) {
  String url = String(DOWNLOAD_BASE_URL) + urlEncodePath(remotePath);
  String cachePath = String(DOWNLOAD_CACHE_DIR) + remotePath;

  deleteRecursive(fs, cachePath);
  if (!mkdirRecursive(fs, parentPath(cachePath))) return false;

  if (!httpDownloadToFile(fs, url, cachePath, 0)) {
    deleteRecursive(fs, cachePath);
    return false;
  }

  if (!mkdirRecursive(fs, parentPath(remotePath))) {
    deleteRecursive(fs, cachePath);
    return false;
  }
  if (fs.exists(remotePath)) deleteRecursive(fs, remotePath);
  if (!fs.rename(cachePath, remotePath)) {
    LOGF("failed to move '%s' to '%s'\n", cachePath.c_str(), remotePath.c_str());
    deleteRecursive(fs, cachePath);
    return false;
  }

  LOGF("'%s' ready\n", remotePath.c_str());
  return true;
}

static bool downloadFolder(fs::FS& fs, const String& remotePath) {
  String manifestUrl =
      String(DOWNLOAD_BASE_URL) + urlEncodePath(remotePath) + "/" + DOWNLOAD_MANIFEST_NAME;
  LOGF("fetching manifest '%s'\n", manifestUrl.c_str());

  HTTPClient http;
  if (!http.begin(manifestUrl)) {
    LOGLN("failed to begin manifest request");
    return false;
  }
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOGF("manifest GET failed, HTTP code %d\n", httpCode);
    http.end();
    return false;
  }
  String manifestBody = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, manifestBody);
  if (err) {
    LOGF("failed to parse manifest: %s\n", err.c_str());
    return false;
  }

  JsonArray filesArray = doc["files"].as<JsonArray>();
  if (filesArray.isNull() || filesArray.size() == 0) {
    LOGLN("manifest has no files listed");
    return false;
  }
  LOGF("manifest lists %u file(s)\n", (unsigned)filesArray.size());

  String cachePath = String(DOWNLOAD_CACHE_DIR) + remotePath;
  String cacheManifestPath = cachePath + "/" + DOWNLOAD_MANIFEST_NAME;

  // A cache subtree left over from a previous attempt is only safe to resume from if
  // it was downloading the exact same manifest - otherwise the folder's contents
  // changed on the server and any partial files in there could be stale/mismatched.
  String cachedManifest;
  File cacheManifestFile = fs.open(cacheManifestPath);
  bool haveCachedManifest = (bool)cacheManifestFile;
  if (haveCachedManifest) {
    cachedManifest = cacheManifestFile.readString();
    cacheManifestFile.close();
  }

  if (haveCachedManifest && cachedManifest == manifestBody) {
    LOGF("resuming previous download of '%s'\n", remotePath.c_str());
  } else {
    LOGF("starting fresh download of '%s'\n", remotePath.c_str());
    deleteRecursive(fs, cachePath);
    if (!mkdirRecursive(fs, cachePath)) return false;
    File manifestFile = fs.open(cacheManifestPath, FILE_WRITE);
    if (!manifestFile) {
      LOGF("failed to write cache manifest for '%s'\n", remotePath.c_str());
      return false;
    }
    manifestFile.print(manifestBody);
    manifestFile.close();
  }

  for (JsonObject entry : filesArray) {
    const char* name = entry["name"];
    size_t size = entry["size"] | 0;
    if (!name || name[0] == '\0') {
      LOGLN("manifest entry missing 'name', aborting");
      return false;
    }

    String fileUrl =
        String(DOWNLOAD_BASE_URL) + urlEncodePath(remotePath) + "/" + urlEncodePath(name);
    String fileCachePath = cachePath + "/" + name;

    // On failure, the cache subtree (including whatever files already completed, and
    // the manifest we just wrote/matched above) is deliberately left in place - a
    // later retry resumes from here instead of starting over.
    if (!httpDownloadToFile(fs, fileUrl, fileCachePath, size)) {
      LOGF("failed to download '%s', will resume later\n", name);
      return false;
    }
  }

  // Every listed file is confirmed complete on disk - only now make it visible for
  // playback. manifest.json rides along into the final folder too (harmless -
  // Player only picks up .mp3 files), which doubles as a locally-persisted record of
  // what's actually in there, in case that's useful later.
  if (!mkdirRecursive(fs, parentPath(remotePath))) return false;
  if (fs.exists(remotePath)) deleteRecursive(fs, remotePath);
  if (!fs.rename(cachePath, remotePath)) {
    LOGF("failed to move '%s' to '%s'\n", cachePath.c_str(), remotePath.c_str());
    return false;
  }

  LOGF("'%s' ready with %u file(s)\n", remotePath.c_str(), (unsigned)filesArray.size());
  return true;
}

// ---- public entry point -------------------------------------------------------------

bool ensureContentAvailable(fs::FS& fs, const char* path) {
  if (fs.exists(path)) {
    return true;
  }

  LOGF("'%s' not found locally, attempting WiFi download\n", path);

  if (!connectWifi()) {
    board.playCue(fs, SOUND_DOWNLOAD_FAILED);
    return false;
  }

  board.playCue(fs, SOUND_DOWNLOAD_START);

  String remotePath(path);
  bool ok = isMp3Path(remotePath) ? downloadFile(fs, remotePath) : downloadFolder(fs, remotePath);

  board.playCue(fs, ok ? SOUND_DOWNLOAD_SUCCESS : SOUND_DOWNLOAD_FAILED);

  disconnectWifi();

  return ok;
}
