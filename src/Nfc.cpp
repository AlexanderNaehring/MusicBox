#include "Nfc.h"

#include <ArduinoJson.h>

#include "Board.h"

#define LOG_TAG "Nfc"

#define RFID_CHECK_INTERVAL 50

Nfc nfcReader;

Nfc::Nfc() : mfrc522_(RFID_CS, UINT8_MAX, spi_rfid), nfc_(&mfrc522_) {}

// Decodes a tag's second NDEF record (same plain-text record format as the
// path) as a small JSON object of mode overrides, e.g.
// {"shuffle": true, "autoSleepMinutes": 30, "baseUrl": "http://host:port"}.
// Missing fields keep `mode`'s defaults; a malformed document is logged and
// otherwise ignored, not fatal.
static void parsePlaybackMode(const String& text, Nfc::PlaybackMode& mode) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, text);
  if (error) {
    LOGF("Failed to parse playback mode JSON '%s': %s\n", text.c_str(), error.c_str());
    return;
  }
  mode.shuffle = doc["shuffle"] | false;
  mode.autoSleepMinutes = doc["autoSleepMinutes"] | 0;
  mode.baseUrl = doc["baseUrl"] | "";
}

bool Nfc::begin() {
  LOGLN("RFID...");
  spi_rfid.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_CS);
  mfrc522_.PCD_Init();
  // code from mfrc522.PCD_DumpVersionToSerial();
  byte v = mfrc522_.PCD_ReadRegister(mfrc522_.VersionReg);
  if ((v == 0x00) || (v == 0xFF)) {
    LOGLN(F("WARNING: MFRC522 communication failure, is the MFRC522 properly connected?"));
    return false;
  }
  nfc_.begin();
  return true;
}

void Nfc::forget() { memset(&lastUid_, 0, MAX_UID_LEN); }

void Nfc::rememberCurrentTag() { memcpy(&lastUid_, &currentUid_, currentUidLen_); }

Nfc::PollResult Nfc::poll(unsigned long now, String& outPath) {
  if (now - lastCheckMillis_ < RFID_CHECK_INTERVAL) {
    return PollResult::NoTag;
  }
  lastCheckMillis_ = now;
  Serial.print(".");

  if (!nfc_.tagPresent()) {
    return PollResult::NoTag;
  }

  LOGLN("RFID detected");
  NfcTag tag = nfc_.read();
  LOGF("UID: %s\n", tag.getUidString().c_str());

  memset(&currentUid_, 0, MAX_UID_LEN);
  currentUidLen_ = MAX_UID_LEN;
  tag.getUid(currentUid_, &currentUidLen_);

  if (memcmp(lastUid_, currentUid_, currentUidLen_) == 0) {
    // Same card
    LOGLN("resume playback");
    return PollResult::SameTagResumed;
  }

  // Different card - forget it until (and unless) we fully commit to it,
  // via rememberCurrentTag().
  forget();

  if (!tag.hasNdefMessage()) {
    LOGLN("NFC Tag has no NDEF message");
    return PollResult::InvalidTag;
  }

  NdefMessage message = tag.getNdefMessage();
  if (message.getRecordCount() < 1) {
    return PollResult::InvalidTag;
  }

  NdefRecord record = message.getRecord(0);  // read 1st record
  // NdefRecord record = message[i]; // alternate syntax

  if (record.getTnf() != NdefRecord::TNF::TNF_WELL_KNOWN) {
    LOGLN("NDEF TNF record is not WELL_KNOWN");
    return PollResult::UnsupportedRecord;
  }

  if (record.getTypeLength() != 1 || ((char*)record.getType())[0] != 'T') {
    LOGLN("NDEF record has incorrect type.");
    return PollResult::UnsupportedRecord;
  }

  int payloadLength = record.getPayloadLength();
  const byte* payload = record.getPayload();

  int languageLen = (int)payload[0];
  String filePath = "";
  for (int c = 1 + languageLen; c < payloadLength; c++) {
    filePath += (char)payload[c];
  }
  LOGF("Path: %s\n", filePath.c_str());

  outPath = filePath;

  currentMode_ = PlaybackMode();
  if (message.getRecordCount() > 1) {
    NdefRecord modeRecord = message.getRecord(1);
    if (modeRecord.getTnf() == NdefRecord::TNF::TNF_WELL_KNOWN && modeRecord.getTypeLength() == 1 &&
        ((char*)modeRecord.getType())[0] == 'T') {
      int modePayloadLength = modeRecord.getPayloadLength();
      const byte* modePayload = modeRecord.getPayload();
      int modeLanguageLen = (int)modePayload[0];
      String modeText = "";
      for (int c = 1 + modeLanguageLen; c < modePayloadLength; c++) {
        modeText += (char)modePayload[c];
      }
      LOGF("Mode: %s\n", modeText.c_str());
      parsePlaybackMode(modeText, currentMode_);
    } else {
      LOGLN("NDEF mode record present but not a plain text record, ignoring");
    }
  }

  return PollResult::NewTagPath;
}
