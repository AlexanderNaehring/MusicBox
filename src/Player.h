#ifndef MUSICBOX_PLAYER_H
#define MUSICBOX_PLAYER_H

#include <Arduino.h>
#include <FS.h>

#include <vector>

#include "AudioFileSourceBuffer.h"
#include "AudioFileSourceFS.h"
#include "AudioFileSourceID3.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

// Fallback bytes/sec estimate for seekBySeconds() until update() has
// calibrated the real rate (see Player.cpp's MIN_CALIBRATION_MS).
// Deliberately on the low side, so an uncalibrated seek undershoots rather
// than overshoots - doesn't need to be accurate, just a reasonable guess.
#define PLAYER_FALLBACK_BYTES_PER_SECOND (96000 / 8)  // 96 kbit/s

// Owns the playback queue, the ESP8266Audio decode pipeline, gain, and
// playback-position persistence. Deliberately knows nothing about the device
// state machine, the LED, NFC, or BLE: playNext()/playPrev()/playFirst()
// report whether playback is (still) going, and callers decide what that
// means for the rest of the device.
class Player {
 public:
  void begin(fs::FS& fs, AudioOutputI2S* output);

  enum class PlayResult { Started, OpenFailed, NothingToPlay };
  // Loads `path` (a single .mp3 file or a folder of them) into the queue,
  // replacing whatever was there, and starts playback.
  PlayResult playPathOrFolder(const char* path);

  // Each returns false if there was nothing (more) to play - the queue is
  // now empty or exhausted.
  bool playNext();
  bool playPrev();
  bool playFirst();

  // Clears the queue and any loaded track. Does not touch anything outside
  // the player (no LED/state/NFC side effects - that's the caller's job).
  void stop();

  enum class UpdateResult { Playing, TrackAdvanced, QueueFinished, StalledError };
  // Call every loop() tick while in the PLAYING state: steps the decoder,
  // autosaves the position on interval, and reports what happened.
  UpdateResult update(unsigned long now);

  // Persists the live playback position of the current track, e.g. when the
  // card is pulled mid-play.
  void saveCurrentPosition();

  enum class SeekResult {
    Seeked,      // Seeked to the requested position (or end of file).
    HitStart,    // Negative seek clamped to the beginning of file
    NotPlaying,  // Nothing is currently playing; no seek was attempted.
    NoEstimate   // No valid bitrate estimate available
  };

  // Jumps forward (positive) or backward (negative) within the current
  // track by roughly `deltaSeconds`, clamped to the file's bounds. Converts
  // seconds to a byte offset using a bytes/sec rate estimated from actual
  // playback (see update()), falling back to PLAYER_FALLBACK_BYTES_PER_SECOND
  // until that's calibrated - MP3 doesn't carry an exact duration, and this
  // is intended for large single-file tracks (e.g. audiobooks) where
  // playNext()/playPrev() would otherwise skip to a different file.
  SeekResult seekBySeconds(int32_t deltaSeconds);

  void volumeUp();
  void volumeDown();
  // Clamps to the allowed gain range, applies it, and returns the value
  // actually applied - so callers backed by their own hardware count (e.g. a
  // rotary encoder) can resync to the clamped value.
  int64_t setGainRaw(int64_t gain);
  int64_t gain() const { return gain_; }

  int currentIndex() const { return currentFile_; }
  size_t queueSize() const { return files_.size(); }
  const char* currentFolderPath() const { return currentFolder_; }
  const char* currentTrackPath() const;

 private:
  void reinitAudioSource();
  void addFileToQueue(fs::File file);
  void addFolderToQueue(fs::File root);
  void persistPosition(int64_t trackIdx, int64_t position);
  bool loadPersistedPosition(int64_t& trackIdx, int64_t& position);

  fs::FS* fs_ = nullptr;
  AudioOutputI2S* output_ = nullptr;
  AudioFileSourceFS* sourceFs_ = nullptr;
  AudioFileSourceBuffer* sourceBuffer_ = nullptr;
  AudioFileSourceID3* sourceId3_ = nullptr;
  AudioGeneratorMP3* mp3_ = nullptr;

  // Queue entries are raw C strings, not String: fs::File::path() bytes
  // corrupt when routed through Arduino's String (see addFileToQueue in
  // Player.cpp) - copy them by hand instead, and free() them ourselves.
  std::vector<char*> files_;
  int currentFile_ = -1;
  char* currentFolder_ = nullptr;
  char* lastTrackFile_ = nullptr;
  unsigned long lastPlayMillis_ = 0;
  uint32_t resumePosition_ = 0;
  unsigned long lastPositionSaveMillis_ = 0;
  int64_t gain_ = 0;

  // bitrate calibration for seekBySeconds(), refreshed in update(). 
  // Reset whenever a new track starts; NOT reset by seekBySeconds() itself
  uint32_t trackStartBytePos_ = 0;
  unsigned long playedMillisAccum_ = 0;
  unsigned long lastCalibrationMillis_ = 0;
  double bytesPerSecondEstimate_ = PLAYER_FALLBACK_BYTES_PER_SECOND;
};

extern Player player;

#endif
