#include "Controls.h"

#define LOG_TAG "Controls"

#define SEEK_STEP_SECONDS 120
#define MAX_ENCODER_STEPS_PER_TICK 10

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
  btnPrev_.attachLongPressStart([]() { controls.handlePrevLong(); });

  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  rotaryGain_.attachHalfQuad(RotaryA, RotaryB);
  rotaryGain_.clearCount();
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
  int64_t delta = rotaryGain_.getCount();
  rotaryGain_.clearCount();
  if (delta > 0 && delta <= MAX_ENCODER_STEPS_PER_TICK) {
    for (int64_t i = 0; i < delta; i++) player_->volumeUp();
  } else if (delta < 0 && delta >= -MAX_ENCODER_STEPS_PER_TICK) {
    for (int64_t i = 0; i > delta; i--) player_->volumeDown();
  } else if (delta != 0) {
    LOGF("Ignoring implausible encoder delta %lld\n", (long long)delta);
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
void Controls::handlePrevLong() { onPlayAttempt_(player_->playFirst()); }
#endif
