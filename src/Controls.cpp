#include "Controls.h"

#define LOG_TAG "Controls"

Controls controls;

void Controls::begin(Player& player, PlayAttemptHandler onPlayAttempt) {
  player_ = &player;
  onPlayAttempt_ = onPlayAttempt;

#if HW_REV == 1
  btnNext_.attachClick([]() { controls.handleVolumeUpClick(); });
  btnNext_.attachLongPressStart([]() { controls.handleNextLongPress(); });
  btnNext_.attachLongPressStop([]() { controls.handleBothHeldReleased(); });
  btnPrev_.attachClick([]() { controls.handleVolumeDownClick(); });
  btnPrev_.attachLongPressStart([]() { controls.handlePrevLongPress(); });
  btnPrev_.attachLongPressStop([]() { controls.handleBothHeldReleased(); });
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
  } else {
    onPlayAttempt_(player_->playNext());
  }
}

void Controls::handlePrevLongPress() {
  if (digitalRead(BTN_NEXT) == LOW) {
    if (!bothHeldHandled_) {
      bothHeldHandled_ = true;
      onPlayAttempt_(player_->playFirst());
    }
  } else {
    onPlayAttempt_(player_->playPrev());
  }
}

void Controls::handleBothHeldReleased() { bothHeldHandled_ = false; }

#elif HW_REV == 2
void Controls::handleNextClick() { onPlayAttempt_(player_->playNext()); }
void Controls::handlePrevClick() { onPlayAttempt_(player_->playPrev()); }
void Controls::handlePlayFirst() { onPlayAttempt_(player_->playFirst()); }
#endif
