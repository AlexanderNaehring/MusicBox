#include "Player.h"

#include <algorithm>

#include "Config.h"

#define LOG_TAG "Player"

#define MinAudioGain 2
#define MaxAudioGain 52
#define InitialAudioGain 8
#define AUDIO_SOURCE_BUFFER_SIZE 1024 * 4
#define POSITION_SAVE_INTERVAL 30000  // 30 seconds
#define LAST_PLAYBACK_FILE_SUFFIX "/last.int64"
// Ignore gaps between update() ticks larger than this for bitrate estimate
#define PAUSE_GAP_THRESHOLD_MS 2000
// Minimum accumulated actual playback time before trusting bitrate estimate
#define MIN_CALIBRATION_MS 800

Player player;

struct PlaybackPosition {
  int64_t trackIdx;
  int64_t position;
};

static char* strRight(const char* str, size_t n) {
  size_t len = strlen(str);
  if (n > len) n = len;
  return (char*)str + len - n;
}

void Player::begin(fs::FS& fs, AudioOutputI2S* output) {
  fs_ = &fs;
  output_ = output;
  mp3_ = new AudioGeneratorMP3();
  setGainRaw(InitialAudioGain);
}

void Player::reinitAudioSource() {
  LOGF("Re-init audio source...");
  if (mp3_ && mp3_->isRunning()) mp3_->stop();

  LOGF(" delete audio sources...");
  delete sourceFs_;
  sourceFs_ = nullptr;
  delete sourceBuffer_;
  sourceBuffer_ = nullptr;
  delete sourceId3_;
  sourceId3_ = nullptr;

  LOGF(" create sources...");
  sourceFs_ = new AudioFileSourceFS(*fs_);
  sourceBuffer_ = new AudioFileSourceBuffer(sourceFs_, AUDIO_SOURCE_BUFFER_SIZE);
  // ID3 file source for MP3 files reduces the delay until playback starts, and
  // enables ID3 Tag callbacks
  sourceId3_ = new AudioFileSourceID3(sourceBuffer_);
  LOGF(" done\n");
}

void Player::stop() {
  LOGF("stop()\n");
  if (mp3_ && mp3_->isRunning()) {
    LOGF("   mp3->stop()\n");
    mp3_->stop();
  }
  if (files_.size() > 0) {
    LOGF("  clear files list\n");
    for (char* x : files_) {
      free((void*)x);
    }
    files_.clear();
  }
  LOGF("  reset flags\n");
  currentFile_ = -1;
  if (currentFolder_) {
    free(currentFolder_);
    currentFolder_ = nullptr;
  }
}

void Player::addFileToQueue(fs::File file) {
  // Directly converting fs::File::path() to a String corrupts its content
  // (confirmed empirically - concatenating it char-by-char produced garbage,
  // especially once a "." was involved). Copy the raw bytes by hand instead.
  const char* tmp = file.path();
  if (strcmp(strRight(tmp, 4), ".mp3")) {
    LOGF("Skip %s, files must end with .mp3\n", tmp);
    return;
  }
  char* path = (char*)malloc((strlen(tmp) + 1) * sizeof(char));
  strcpy(path, file.path());
  LOGF("Add to queue: %s\n", path);
  files_.push_back(path);
}

void Player::addFolderToQueue(fs::File root) {
  if (!root.isDirectory()) {
    LOGLN("Not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      addFileToQueue(file);
    }
    file.close();
    file = root.openNextFile();
  }
}

void Player::persistPosition(int64_t trackIdx, int64_t position) {
  if (!(currentFolder_ && strlen(currentFolder_) > 0 && lastTrackFile_ && strlen(lastTrackFile_) > 0)) {
    return;
  }
  PlaybackPosition state{trackIdx, position};
  File file = fs_->open(lastTrackFile_, "w");
  if (!file) {
    LOGF("persistPosition: failed to open %s for writing\n", lastTrackFile_);
    return;
  }
  size_t written = file.write((const uint8_t*)&state, sizeof(state));
  file.close();
  LOGF("persistPosition: wrote trackIdx=%lld position=%lld (%u bytes) to %s\n",
       (long long)state.trackIdx, (long long)state.position, (unsigned)written, lastTrackFile_);
}

bool Player::loadPersistedPosition(int64_t& trackIdx, int64_t& position) {
  trackIdx = 0;
  position = 0;
  if (!(lastTrackFile_ && strlen(lastTrackFile_) > 0)) {
    return false;
  }
  File file = fs_->open(lastTrackFile_);
  if (!file) {
    LOGF("loadPersistedPosition: %s not found\n", lastTrackFile_);
    return false;
  }
  PlaybackPosition state{0, 0};
  size_t bytesRead = file.read((uint8_t*)&state, sizeof(state));
  file.close();
  if (bytesRead != sizeof(state)) {
    LOGF("loadPersistedPosition: %s has unexpected size (%u bytes, expected %u)\n", lastTrackFile_,
         (unsigned)bytesRead, (unsigned)sizeof(state));
    return false;
  }
  trackIdx = state.trackIdx;
  position = state.position;
  LOGF("loadPersistedPosition: read trackIdx=%lld position=%lld from %s\n", (long long)trackIdx,
       (long long)position, lastTrackFile_);
  return true;
}

void Player::saveCurrentPosition() { persistPosition(currentFile_, sourceId3_->getPos()); }

bool Player::playNext() {
  LOGLN("playNext()");
  if (files_.size() <= 0) {
    LOGF("Queue empty\n");
    return false;
  }
  if (currentFile_ >= (int)files_.size() - 1) {
    LOGF("End of queue (%d, %u)\n", currentFile_, (unsigned)files_.size());
    persistPosition(-1, 0);
    stop();
    return false;
  }

  currentFile_++;
  char* filepath = files_[currentFile_];

  // save current track number (and resume position, if any) to SD
  persistPosition(currentFile_, resumePosition_);

  // start playback
  reinitAudioSource();
  sourceFs_->open(filepath);
  LOGF("mp3->begin() %s\n", filepath);
  mp3_->begin(sourceId3_, output_);

  if (resumePosition_ > 0) {
    LOGF("Resuming at byte position %lu\n", (unsigned long)resumePosition_);
    sourceId3_->seek(resumePosition_, SEEK_SET);
  }
  resumePosition_ = 0;

  lastPlayMillis_ = millis();
  lastPositionSaveMillis_ = lastPlayMillis_;

  // New file - the previous bytes/sec estimate (if any) no longer applies.
  trackStartBytePos_ = sourceId3_->getPos();
  playedMillisAccum_ = 0;
  lastCalibrationMillis_ = lastPlayMillis_;
  bytesPerSecondEstimate_ = PLAYER_FALLBACK_BYTES_PER_SECOND;
  return true;
}

bool Player::playPrev() {
  LOGLN("playPrev()");
  if (files_.size() <= 0) {
    LOGF("Cannot play, queue empty\n");
    return false;
  }

  // check if more than x seconds into the current file
  if ((millis() - lastPlayMillis_) > 10000) {
    LOGF("Play current file from start");
    currentFile_--;
    return playNext();
  }

  // already at first file in queue?
  if (currentFile_ <= 0) {
    LOGF("Beginning of queue, restart current file\n");
    currentFile_--;
    return playNext();
  }

  // go to previous file in queue
  currentFile_ -= 2;
  return playNext();
}

bool Player::playFirst() {
  LOGLN("playFirst()");
  currentFile_ = -1;
  return playNext();
}

Player::SeekResult Player::seekBySeconds(int32_t deltaSeconds) {
  if (!mp3_ || !mp3_->isRunning() || currentFile_ < 0 || (size_t)currentFile_ >= files_.size()) {
    return SeekResult::NotPlaying;
  }
  if (bytesPerSecondEstimate_ <= 0) {
    LOGLN("seekBySeconds: no valid bytes/sec estimate, ignoring");
    return SeekResult::NoEstimate;
  }

  int32_t byteDelta = (int32_t)(deltaSeconds * bytesPerSecondEstimate_);
  if (byteDelta == 0) return SeekResult::Seeked;

  uint32_t currentPos = sourceId3_->getPos();
  uint32_t fileSize = sourceId3_->getSize();
  int64_t newPos = (int64_t)currentPos + byteDelta;
  SeekResult result = SeekResult::Seeked;
  if (newPos <= 0) {
    newPos = 0;
    result = SeekResult::HitStart;
  }
  if (fileSize > 0 && newPos >= (int64_t)fileSize) newPos = fileSize - 1;

  LOGF("seekBySeconds(%d): rate=%.0f B/s, pos %lu -> %lld\n", (int)deltaSeconds,
       bytesPerSecondEstimate_, (unsigned long)currentPos, (long long)newPos);

  sourceId3_->seek((int32_t)newPos, SEEK_SET);
  mp3_->desync();

  // Restart the playback-time/byte baseline from here, but keep
  // bytesPerSecondEstimate_ - it's still valid for the same file.
  trackStartBytePos_ = sourceId3_->getPos();
  playedMillisAccum_ = 0;
  lastCalibrationMillis_ = millis();

  // lastPositionSaveMillis_ = lastCalibrationMillis_;
  // persistPosition(currentFile_, (int64_t)newPos);
  return result;
}

Player::PlayResult Player::playPathOrFolder(const char* path) {
  LOGF("playPathOrFolder(%s)\n", path);
  stop();
  int lastTrack = -1;

  File root = fs_->open(path);
  if (!root) {
    LOGF("Failed to open %s\n", path);
    return PlayResult::OpenFailed;
  }

  if (root.isDirectory()) {
    addFolderToQueue(root);

    if (currentFolder_) free(currentFolder_);
    currentFolder_ = strdup(path);

    if (lastTrackFile_) free(lastTrackFile_);
    lastTrackFile_ = (char*)malloc(strlen(path) + strlen(LAST_PLAYBACK_FILE_SUFFIX) + 1);
    sprintf(lastTrackFile_, "%s%s", path, LAST_PLAYBACK_FILE_SUFFIX);

    int64_t savedTrackIdx = 0;
    int64_t savedPosition = 0;
    if (loadPersistedPosition(savedTrackIdx, savedPosition)) {
      lastTrack = (int)savedTrackIdx;
      resumePosition_ = (uint32_t)savedPosition;
    }

  } else {
    addFileToQueue(root);
  }
  root.close();

  if (files_.size() == 0) {
    return PlayResult::NothingToPlay;
  }

  auto cstrCompare = [](const char* s1, const char* s2) { return strcmp(s1, s2) < 0; };
  std::sort(files_.begin(), files_.end(), cstrCompare);
  LOGF("Queue:\n");
  for (auto x : files_) {
    LOGF("  %s\n", x);
  }
  if (lastTrack >= 0 && lastTrack < (int)files_.size()) {
    LOGF("Jump to track %d\n", lastTrack);
    currentFile_ = lastTrack - 1;
  } else {
    LOGF("Start queue from start\n");
    resumePosition_ = 0;
  }
  return playNext() ? PlayResult::Started : PlayResult::NothingToPlay;
}

Player::UpdateResult Player::update(unsigned long now) {
  if (!mp3_->isRunning()) {
    return UpdateResult::StalledError;
  }

  // Accumulate actual playback time (not wall-clock time since track start)
  // so a paused stretch doesn't skew the bytes/sec estimate: a gap between
  // ticks larger than PAUSE_GAP_THRESHOLD_MS means playback was paused in
  // between and is simply not counted.
  unsigned long tickMillis = now - lastCalibrationMillis_;
  lastCalibrationMillis_ = now;
  if (tickMillis < PAUSE_GAP_THRESHOLD_MS) {
    playedMillisAccum_ += tickMillis;
    if (playedMillisAccum_ >= MIN_CALIBRATION_MS) {
      uint32_t bytesSinceTrackStart = sourceId3_->getPos() - trackStartBytePos_;
      bytesPerSecondEstimate_ = bytesSinceTrackStart * 1000.0 / playedMillisAccum_;
    }
  }

  if (!mp3_->loop()) {
    return playNext() ? UpdateResult::TrackAdvanced : UpdateResult::QueueFinished;
  }
  if (now - lastPositionSaveMillis_ >= POSITION_SAVE_INTERVAL) {
    lastPositionSaveMillis_ = now;
    saveCurrentPosition();
  }
  return UpdateResult::Playing;
}

int64_t Player::setGainRaw(int64_t gain) {
  if (gain < MinAudioGain) gain = MinAudioGain;
  if (gain > MaxAudioGain) gain = MaxAudioGain;
  gain_ = gain;
  LOGF("Volume: %lld\n", (long long)gain_);
  output_->SetGain(gain_ / 100.0);
  return gain_;
}

void Player::volumeUp() { setGainRaw(gain_ + 2); }
void Player::volumeDown() { setGainRaw(gain_ - 2); }

const char* Player::currentTrackPath() const {
  if (currentFile_ < 0 || currentFile_ >= (int)files_.size()) return "";
  return files_[currentFile_];
}
