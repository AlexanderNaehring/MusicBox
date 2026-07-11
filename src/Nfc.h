#ifndef MUSICBOX_NFC_H
#define MUSICBOX_NFC_H

#include <Arduino.h>
#include <SPI.h>

#include "MFRC522.h"
#include "NfcAdapter.h"

#define MAX_UID_LEN 10

// Wraps the MFRC522 reader + NDEF parsing. Tracks the UID of the last tag it
// committed to (see rememberCurrentTag()) so a card that's merely still
// present reads as "same tag, just resume" rather than being re-decoded.
class Nfc {
 public:
  Nfc();

  // Powers up the reader. Returns false if the MFRC522 doesn't answer.
  bool begin();

  enum class PollResult {
    NoTag,              // no card present, or checked too recently - try again later
    SameTagResumed,     // still the tag we last committed to
    InvalidTag,         // new tag, but no usable NDEF message on it
    UnsupportedRecord,  // new tag, NDEF message present but not a plain text record
    NewTagPath,         // new tag, decoded a path into `outPath`
  };
  // Call every loop() tick while expecting a card. Internally throttled to
  // MusicBox's RFID_CHECK_INTERVAL, so it's cheap to call every tick.
  PollResult poll(unsigned long now, String& outPath);

  // Commits to the tag most recently reported via poll() (NewTagPath), so the
  // next poll() sees it as SameTagResumed instead of decoding it again.
  // Deliberately separate from poll() - the caller only knows once it has
  // decided the tag's content is actually usable (e.g. after a WiFi content
  // download check), which can happen well after poll() returned.
  void rememberCurrentTag();

  // Forget the last-committed tag, e.g. once its queue has been fully played
  // through - so re-inserting the same physical card later starts it over.
  void forget();

 private:
  MFRC522 mfrc522_;
  NfcAdapter nfc_;

  unsigned long lastCheckMillis_ = 0;
  byte lastUid_[MAX_UID_LEN] = {0};
  byte currentUid_[MAX_UID_LEN] = {0};
  byte currentUidLen_ = 0;
};

extern Nfc nfcReader;

#endif
