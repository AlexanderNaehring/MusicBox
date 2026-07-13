#include "Controls.h"

#define LOG_TAG "Controls"

#define SEEK_STEP_SECONDS 120

Controls controls;

void Controls::begin(Player& player, PlayAttemptHandler onPlayAttempt) {
  player_ = &player;
  onPlayAttempt_ = onPlayAttempt;

#if HW_REV == 1
  btnNext_.attachClick([]() { controls.handleVolumeUpClick(); });
  btnNext_.attachDuringLongPress([]() { controls.handleNextLongPress(); });
  btnNext_.attachLongPressStop([]() { controls.handleBothHeldReleased(); });
  btnNext_.setPressMs(600);
  btnNext_.setLongPressIntervalMs(1000);
  btnPrev_.attachClick([]() { controls.handleVolumeDownClick(); });
  btnPrev_.attachDuringLongPress([]() { controls.handlePrevLongPress(); });
  btnPrev_.attachLongPressStop([]() { controls.handleBothHeldReleased(); });
  btnPrev_.setPressMs(600);
  btnPrev_.setLongPressIntervalMs(1000);
#elif HW_REV == 2
  btnNext_.attachClick([]() { controls.handleNextClick(); });
  btnPrev_.attachClick([]() { controls.handlePrevClick(); });
  btnPrev_.attachLongPressStart([]() { controls.handlePlayFirst(); });

  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  rotaryGain_.attachHalfQuad(RotaryA, RotaryB);
  rotaryGain_.setCount(player_->gain());
  lastEncoderCount_ = player_->gain();
#endif
}

// Check if the seekResult counts as "success" (still playing)
static bool seekSucceeded(Player::SeekResult result) {
  return result == Player::SeekResult::Seeked || result == Player::SeekResult::HitStart;
}

void Controls::seekForward() { onPlayAttempt_(seekSucceeded(player_->seekBySeconds(SEEK_STEP_SECONDS))); }

void Controls::seekBack() {
  Player::SeekResult result = player_->seekBySeconds(-SEEK_STEP_SECONDS);
  if (result == Player::SeekResult::HitStart) {
    LOGLN("Hit start of file, call playPrev()");
    onPlayAttempt_(player_->playPrev());
  } else {
    onPlayAttempt_(seekSucceeded(result));
  }
}

void Controls::tick() {
  btnNext_.tick();
  btnPrev_.tick();

#if HW_REV == 2
  int64_t count = rotaryGain_.getCount();
  if (count != lastEncoderCount_) {
    int64_t applied = player_->setGainRaw(count);
    lastEncoderCount_ = applied;
    rotaryGain_.clearCount();
    rotaryGain_.setCount(applied);
    LOGF("Set gain: %lld\n", (long long)applied);
  }
#endif
}

#if HW_REV == 1
void Controls::handleVolumeUpClick() { player_->volumeUp(); }
void Controls::handleVolumeDownClick() { player_->volumeDown(); }

void Controls::handleNextLongPress() {
  if (digitalRead(BTN_PREV) == LOW) {
    if (!bothHeldHandled_) {
      bothHeldHandled_ = true;
      onPlayAttempt_(player_->playFirst());
    }
    return;
  }
  seekForward();
}

void Controls::handlePrevLongPress() {
  if (digitalRead(BTN_NEXT) == LOW) {
    if (!bothHeldHandled_) {
      bothHeldHandled_ = true;
      onPlayAttempt_(player_->playFirst());
    }
    return;
  }
  seekBack();
}

void Controls::handleBothHeldReleased() { bothHeldHandled_ = false; }

#elif HW_REV == 2
void Controls::handleNextClick() { seekForward(); }
void Controls::handlePrevClick() { seekBack(); }
void Controls::handlePlayFirst() { onPlayAttempt_(player_->playFirst()); }
#endif
