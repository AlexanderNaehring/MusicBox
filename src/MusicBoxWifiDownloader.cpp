#include "MusicBoxWifiDownloader.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include "AudioFileSourceFS.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include "Board.h"

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

// Wipes the entire download cache tree, not just one target's subtree. A completed
// attempt (success or failure) always empties its own cache subtree before
// returning, so anything still sitting under DOWNLOAD_CACHE_DIR when a new download
// is about to start can only be garbage from an attempt that never got to finish -
// e.g. power was lost mid-download. Safe to call even if nothing needs cleaning.
static void cleanupDownloadCache(fs::FS& fs) {
  if (!fs.exists(DOWNLOAD_CACHE_DIR)) return;
  Serial.println("MusicBoxWifiDownloader: sweeping stale download cache...");
  deleteRecursive(fs, DOWNLOAD_CACHE_DIR);
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
      Serial.printf("mkdirRecursive: failed to create '%s'\n", segment.c_str());
      return false;
    }
    start = idx + 1;
  }
  if (!fs.exists(path) && !fs.mkdir(path)) {
    Serial.printf("mkdirRecursive: failed to create '%s'\n", path.c_str());
    return false;
  }
  return true;
}

// ---- WiFi lifecycle -------------------------------------------------------------------

static bool connectWifi() {
  Serial.printf("MusicBoxWifiDownloader: connecting to WiFi SSID '%s'...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("MusicBoxWifiDownloader: WiFi connection timed out");
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      return false;
    }
    delay(100);
  }
  Serial.printf("MusicBoxWifiDownloader: WiFi connected, IP %s\n",
                WiFi.localIP().toString().c_str());

  // ESP32 WiFi defaults to modem-sleep power saving, which periodically naps the
  // radio between beacon intervals - fine for idle standby, but it measurably
  // throttles sustained downloads. We're about to do real transfer work and will
  // disconnect right after, so trade power for throughput here.
  WiFi.setSleep(false);

  return true;
}

static void disconnectWifi() {
  Serial.println("MusicBoxWifiDownloader: disconnecting WiFi...");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// ---- audio cues -------------------------------------------------------------------

static void playCue(fs::FS& fs, const char* path) {
  AudioOutputI2S* output = board.audioOutput();
  if (!output || !fs.exists(path)) {
    Serial.printf("MusicBoxWifiDownloader: cue '%s' unavailable, skipping\n", path);
    return;
  }
  Serial.printf("MusicBoxWifiDownloader: playing cue '%s'\n", path);

  AudioFileSourceFS cueSource(fs);
  if (!cueSource.open(path)) {
    Serial.printf("MusicBoxWifiDownloader: failed to open cue '%s'\n", path);
    return;
  }
  AudioGeneratorMP3 cueMp3;
  if (!cueMp3.begin(&cueSource, output)) {
    Serial.printf("MusicBoxWifiDownloader: failed to start cue '%s'\n", path);
    return;
  }
  while (cueMp3.isRunning()) {
    if (!cueMp3.loop()) {
      cueMp3.stop();
    }
  }
}

// ---- HTTP download -------------------------------------------------------------------

// Downloads `url` to `localPath`, verifying the number of bytes written against
// `expectedSize` (or, if 0, against the response's Content-Length header - at least one
// of the two must be known, otherwise the download is rejected). Leaves no partial file
// behind on failure, and gives up if no data arrives for a while (possible WiFi drop).
static bool httpDownloadToFile(fs::FS& fs, const String& url, const String& localPath,
                               size_t expectedSize) {
  Serial.printf("MusicBoxWifiDownloader: downloading '%s' -> '%s'\n", url.c_str(),
                localPath.c_str());

  HTTPClient http;
  if (!http.begin(url)) {
    Serial.printf("MusicBoxWifiDownloader: failed to begin request for '%s'\n", url.c_str());
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("MusicBoxWifiDownloader: GET '%s' failed, HTTP code %d\n", url.c_str(), httpCode);
    http.end();
    return false;
  }

  int contentLength = http.getSize();
  size_t targetSize = expectedSize > 0      ? expectedSize
                      : (contentLength > 0) ? (size_t)contentLength
                                            : 0;

  File outFile = fs.open(localPath, FILE_WRITE);
  if (!outFile) {
    Serial.printf("MusicBoxWifiDownloader: failed to open '%s' for writing\n", localPath.c_str());
    http.end();
    return false;
  }
  // Batches small writes into fewer, larger physical SD writes instead of issuing
  // one for every DOWNLOAD_READ_BUFFER_SIZE chunk.
  outFile.setBufferSize(DOWNLOAD_READ_BUFFER_SIZE);

  auto* stream = http.getStreamPtr();
  static uint8_t buf[DOWNLOAD_READ_BUFFER_SIZE];  // static: keep it off the stack
  size_t totalWritten = 0;
  unsigned long lastDataMillis = millis();
  int lastReportedPercent = -5;  // so 0% still gets printed on first progress
  uint8_t rainbowStep = 0;

  while (http.connected() && (contentLength <= 0 || totalWritten < (size_t)contentLength)) {
    size_t toRead = sizeof(buf);
    if (contentLength > 0) {
      size_t remaining = (size_t)contentLength - totalWritten;
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
          Serial.printf("MusicBoxWifiDownloader: %s - %d%% (%u/%u bytes)\n", localPath.c_str(),
                        percentBucket, (unsigned)totalWritten, (unsigned)targetSize);
        }
      }
    } else {
      if (millis() - lastDataMillis > DOWNLOAD_STALL_TIMEOUT_MS) {
        Serial.printf("MusicBoxWifiDownloader: download stalled, aborting '%s'\n", url.c_str());
        board.setLED(RGB_Error);
        break;
      }
      delay(10);
    }
  }

  outFile.close();
  http.end();

  Serial.printf("MusicBoxWifiDownloader: wrote %u bytes (expected %u) for '%s'\n",
                (unsigned)totalWritten, (unsigned)targetSize, localPath.c_str());

  if (targetSize == 0 || totalWritten != targetSize) {
    Serial.printf("MusicBoxWifiDownloader: incomplete/unverified download, removing '%s'\n",
                  localPath.c_str());
    fs.remove(localPath);
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
    Serial.printf("MusicBoxWifiDownloader: failed to move '%s' to '%s'\n", cachePath.c_str(),
                  remotePath.c_str());
    deleteRecursive(fs, cachePath);
    return false;
  }

  Serial.printf("MusicBoxWifiDownloader: '%s' ready\n", remotePath.c_str());
  return true;
}

static bool downloadFolder(fs::FS& fs, const String& remotePath) {
  String manifestUrl =
      String(DOWNLOAD_BASE_URL) + urlEncodePath(remotePath) + "/" + DOWNLOAD_MANIFEST_NAME;
  Serial.printf("MusicBoxWifiDownloader: fetching manifest '%s'\n", manifestUrl.c_str());

  HTTPClient http;
  if (!http.begin(manifestUrl)) {
    Serial.println("MusicBoxWifiDownloader: failed to begin manifest request");
    return false;
  }
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("MusicBoxWifiDownloader: manifest GET failed, HTTP code %d\n", httpCode);
    http.end();
    return false;
  }
  String manifestBody = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, manifestBody);
  if (err) {
    Serial.printf("MusicBoxWifiDownloader: failed to parse manifest: %s\n", err.c_str());
    return false;
  }

  JsonArray filesArray = doc["files"].as<JsonArray>();
  if (filesArray.isNull() || filesArray.size() == 0) {
    Serial.println("MusicBoxWifiDownloader: manifest has no files listed");
    return false;
  }
  Serial.printf("MusicBoxWifiDownloader: manifest lists %u file(s)\n", (unsigned)filesArray.size());

  String cachePath = String(DOWNLOAD_CACHE_DIR) + remotePath;
  deleteRecursive(fs, cachePath);
  if (!mkdirRecursive(fs, cachePath)) return false;

  for (JsonObject entry : filesArray) {
    const char* name = entry["name"];
    size_t size = entry["size"] | 0;
    if (!name || name[0] == '\0') {
      Serial.println("MusicBoxWifiDownloader: manifest entry missing 'name', aborting");
      deleteRecursive(fs, cachePath);
      return false;
    }

    String fileUrl =
        String(DOWNLOAD_BASE_URL) + urlEncodePath(remotePath) + "/" + urlEncodePath(name);
    String fileCachePath = cachePath + "/" + name;

    if (!httpDownloadToFile(fs, fileUrl, fileCachePath, size)) {
      Serial.printf("MusicBoxWifiDownloader: failed to download '%s', aborting folder download\n",
                    name);
      deleteRecursive(fs, cachePath);
      return false;
    }
  }

  // Every listed file is confirmed complete on disk - only now make it visible for playback.
  if (!mkdirRecursive(fs, parentPath(remotePath))) {
    deleteRecursive(fs, cachePath);
    return false;
  }
  if (fs.exists(remotePath)) deleteRecursive(fs, remotePath);
  if (!fs.rename(cachePath, remotePath)) {
    Serial.printf("MusicBoxWifiDownloader: failed to move '%s' to '%s'\n", cachePath.c_str(),
                  remotePath.c_str());
    deleteRecursive(fs, cachePath);
    return false;
  }

  Serial.printf("MusicBoxWifiDownloader: '%s' ready with %u file(s)\n", remotePath.c_str(),
                (unsigned)filesArray.size());
  return true;
}

// ---- public entry point -------------------------------------------------------------

bool ensureContentAvailable(fs::FS& fs, const char* path) {
  if (fs.exists(path)) {
    return true;
  }

  Serial.printf("MusicBoxWifiDownloader: '%s' not found locally, attempting WiFi download\n", path);

  cleanupDownloadCache(fs);

  if (!connectWifi()) {
    playCue(fs, SOUND_DOWNLOAD_FAILED);
    return false;
  }

  playCue(fs, SOUND_DOWNLOAD_START);

  String remotePath(path);
  bool ok = isMp3Path(remotePath) ? downloadFile(fs, remotePath) : downloadFolder(fs, remotePath);

  playCue(fs, ok ? SOUND_DOWNLOAD_SUCCESS : SOUND_DOWNLOAD_FAILED);

  disconnectWifi();

  return ok;
}
