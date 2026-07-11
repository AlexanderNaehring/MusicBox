#ifndef MUSICBOX_CONTROLS_H
#define MUSICBOX_CONTROLS_H

#include <Arduino.h>
#include <OneButton.h>

#include <functional>

#include "Board.h"
#include "Config.h"
#include "Player.h"

#if HW_REV == 2
#include "ESP32Encoder.h"
#endif

// Owns the next/prev buttons (+ the rotary encoder on HW_REV==2) and their
// per-hardware-revision semantics, driving a Player directly. Reports every
// next/prev/first attempt back through the handler given to begin(), since
// only the caller (main.cpp) knows what a finished-queue result should mean
// for the device state.
class Controls {
 public:
  using PlayAttemptHandler = std::function<void(bool started)>;

  void begin(Player& player, PlayAttemptHandler onPlayAttempt);
  void tick();

  // Button/encoder callback targets. Public because OneButton callbacks are
  // plain function pointers, so they're wired up as non-capturing lambdas
  // that reach into the single global `controls` instance - not meant to be
  // called from elsewhere.
#if HW_REV == 1
  void handleVolumeUpClick();
  void handleVolumeDownClick();
  void handleNextLongPress();
  void handlePrevLongPress();
  void handleBothHeldReleased();
#elif HW_REV == 2
  void handleNextClick();
  void handlePrevClick();
  void handlePlayFirst();
#endif

 private:
  Player* player_ = nullptr;
  PlayAttemptHandler onPlayAttempt_;

  OneButton btnNext_{BTN_NEXT, true, false};  // activeLow=true, pullupActive=false
  OneButton btnPrev_{BTN_PREV, true, false};

#if HW_REV == 1
  bool bothHeldHandled_ = false;
#elif HW_REV == 2
  ESP32Encoder rotaryGain_;
  int64_t lastEncoderCount_ = 0;
#endif
};

extern Controls controls;

#endif
