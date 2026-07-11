#include "Nfc.h"

#include "Board.h"

#define RFID_CHECK_INTERVAL 50

Nfc nfcReader;

Nfc::Nfc() : mfrc522_(RFID_CS, UINT8_MAX, spi_rfid), nfc_(&mfrc522_) {}

bool Nfc::begin() {
  Serial.println("RFID...");
  spi_rfid.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_CS);
  mfrc522_.PCD_Init();
  // code from mfrc522.PCD_DumpVersionToSerial();
  byte v = mfrc522_.PCD_ReadRegister(mfrc522_.VersionReg);
  if ((v == 0x00) || (v == 0xFF)) {
    Serial.println(F("WARNING: MFRC522 communication failure, is the MFRC522 properly connected?"));
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

  Serial.println("RFID detected");
  NfcTag tag = nfc_.read();
  Serial.print("UID: ");
  Serial.println(tag.getUidString());

  memset(&currentUid_, 0, MAX_UID_LEN);
  currentUidLen_ = MAX_UID_LEN;
  tag.getUid(currentUid_, &currentUidLen_);

  if (memcmp(lastUid_, currentUid_, currentUidLen_) == 0) {
    // Same card
    Serial.println("resume playback");
    return PollResult::SameTagResumed;
  }

  // Different card - forget it until (and unless) we fully commit to it,
  // via rememberCurrentTag().
  forget();

  if (!tag.hasNdefMessage()) {
    Serial.println("NFC Tag has no NDEF message");
    return PollResult::InvalidTag;
  }

  NdefMessage message = tag.getNdefMessage();
  if (message.getRecordCount() < 1) {
    return PollResult::InvalidTag;
  }

  NdefRecord record = message.getRecord(0);  // read 1st record
  // NdefRecord record = message[i]; // alternate syntax

  if (record.getTnf() != NdefRecord::TNF::TNF_WELL_KNOWN) {
    Serial.println("NDEF TNF record is not WELL_KNOWN");
    return PollResult::UnsupportedRecord;
  }

  if (record.getTypeLength() != 1 || ((char*)record.getType())[0] != 'T') {
    Serial.println("NDEF record has incorrect type.");
    return PollResult::UnsupportedRecord;
  }

  int payloadLength = record.getPayloadLength();
  const byte* payload = record.getPayload();

  int languageLen = (int)payload[0];
  String filePath = "";
  for (int c = 1 + languageLen; c < payloadLength; c++) {
    filePath += (char)payload[c];
  }
  Serial.printf("Path: %s\n", filePath.c_str());

  outPath = filePath;
  return PollResult::NewTagPath;
}
