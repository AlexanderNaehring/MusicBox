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
};

extern Player player;

#endif
