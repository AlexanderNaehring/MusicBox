#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include "AudioFileSourceBuffer.h"
#include "AudioFileSourceID3.h"
#include "AudioFileSourceSD.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include "ESP32Encoder.h"
#include "MFRC522.h"
#include "NfcAdapter.h"

// Button inputs
#define BTN_CARD_INSIDE 17

// Encoder
#define RotaryA 32
#define RotaryB 33
ESP32Encoder rotaryGain;

// SPI
SPIClass spi_1(VSPI);
SPIClass spi_2(HSPI);

// SD card
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCK 18
#define SD_CS 5

// RFID
#define RFID_MOSI 13
#define RFID_MISO 12
#define RFID_SCK 14
#define RFID_CS 15
const SPISettings spiSettings =
    SPISettings(SPI_CLOCK_DIV4, MSBFIRST, SPI_MODE0);
MFRC522 mfrc522(RFID_CS, UINT8_MAX, spi_2);
NfcAdapter nfc = NfcAdapter(&mfrc522);

// ESP8266Audio
#define MinAudioGain 1
#define InitialAudioGain 8
#define MaxAudioGain 40
#define AUDIO_SOURCE_BUFFER_SIZE 4096
AudioFileSourceSD *source_sd = NULL;
AudioFileSourceBuffer *source_buffer = NULL;
AudioFileSourceID3 *source_id3 = NULL;
AudioGeneratorMP3 *mp3 = NULL;
AudioOutputI2S *out_i2s = NULL;

String getHexString(const byte *buffer, byte bufferSize) {
  String id = "";
  for (byte i = 0; i < bufferSize; i++) {
    id += buffer[i] < 0x10 ? "0" : "";
    id += String(buffer[i], HEX);
  }
  return id;
}

bool compareUid(MFRC522::Uid &uid1, MFRC522::Uid &uid2) {
  if (uid1.size != uid2.size) return false;
  for (byte i = 0; i < uid1.size; i++) {
    if (uid1.uidByte[i] != uid2.uidByte[i]) return false;
  }
  return true;
}

void re_init_audio_source() {
  if (source_sd) {
    delete source_sd;
    source_sd = NULL;
  }
  if (source_buffer) {
    delete source_buffer;
    source_buffer = NULL;
  }
  if (source_id3) {
    delete source_id3;
    source_id3 = NULL;
  }

  source_sd = new AudioFileSourceSD();
  // uint8_t *buffer = (uint8_t*)malloc(sizeof(uint8_t) *
  // AUDIO_SOURCE_BUFFER_SIZE);
  source_buffer =
      new AudioFileSourceBuffer(source_sd, AUDIO_SOURCE_BUFFER_SIZE);
  // ID3 file source for MP3 files reduces the delay until playback starts, and
  // enabled ID3 Tag callbacks
  source_id3 = new AudioFileSourceID3(source_buffer);
}

void setup() {
  Serial.begin(115200);
  Serial.println("Setup...");

  // Buttons
  pinMode(BTN_CARD_INSIDE, INPUT_PULLUP);

  // Rotary
  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  rotaryGain.attachHalfQuad(RotaryA, RotaryB);
  rotaryGain.setCount(InitialAudioGain);

  // SD card
  Serial.println("SD_SPI...");
  spi_1.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  Serial.println("SD...");
  if (!SD.begin(SD_CS, spi_1)) {
    Serial.println("Card Mount Failed");
    while (1) {
    }
  }

  // RFID - NFC
  Serial.println("RFID...");
  spi_2.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_CS);
  mfrc522.PCD_Init();
  mfrc522.PCD_DumpVersionToSerial();
  nfc.begin();

  // Audio
  audioLogger = &Serial;
  Serial.println("Audio...");
  re_init_audio_source();

  out_i2s = new AudioOutputI2S();
  out_i2s->SetGain(InitialAudioGain / 100.0);

  mp3 = new AudioGeneratorMP3();

  Serial.println("Waiting for RFID...");
}

bool play_flag = false;
unsigned long last_rfid_check_time;
#define RFID_INTERVAL 400
#define RFID_PAUSE_AFTER_INTERVAL 3
#define RFID_CHECK_INTERVAL 50

#define MAX_UID_LEN 10
byte current_uid[MAX_UID_LEN];
byte last_uid[MAX_UID_LEN];

int64_t lastGain = InitialAudioGain;

void loop() {
  unsigned long now = millis();

  // read encoder
  int64_t gain = rotaryGain.getCount();
  if (gain != lastGain) {
    if (gain < MinAudioGain)
      gain = MinAudioGain;
    else if (gain > MaxAudioGain)
      gain = MaxAudioGain;
    lastGain = gain;
    rotaryGain.clearCount();
    rotaryGain.setCount(gain);
    Serial.print("Set gain: ");
    Serial.println(gain);
    out_i2s->SetGain(gain / 100.0);
  }

  if (play_flag) {
    if (mp3->isRunning()) {
      if (!mp3->loop()) {
        Serial.println("mp3->stop();");
        mp3->stop();
      }
    } else {
      // source_sd->close();
    }
  }

  if (now - last_rfid_check_time > RFID_CHECK_INTERVAL) {
    last_rfid_check_time = now;
    if (digitalRead(BTN_CARD_INSIDE) == LOW) {
      // card inside
      if (!play_flag) {
        // nothing is playing
        // wait for read card
        Serial.print(".");
        if (nfc.tagPresent()) {
          // RFID detected
          Serial.println("RFID detected");
          NfcTag tag = nfc.read();

          Serial.print("UID: ");
          Serial.println(tag.getUidString());

          memset(&current_uid, 0, MAX_UID_LEN);
          byte current_uid_len = MAX_UID_LEN;
          tag.getUid(current_uid, &current_uid_len);

          if (memcmp(last_uid, current_uid, current_uid_len) == 0) {
            // current RFID is same as last RFID, resume playback
            Serial.println("Resume playback");
            play_flag = true;
          } else {
            // different RFID, read card and start new playback
            memset(&last_uid, 0, MAX_UID_LEN);
            memcpy(&last_uid, &current_uid, current_uid_len);

            // read path
            if (!tag.hasNdefMessage()) {
              Serial.println("NFC Tag has no NDEF message");
              return;
            }
            NdefMessage message = tag.getNdefMessage();

            if (message.getRecordCount() < 1) {
              Serial.println("Not enough NDEF records found");
              return;
            }

            // read 1st record
            NdefRecord record = message.getRecord(0);
            // NdefRecord record = message[i]; // alternate syntax

            if (record.getTnf() != NdefRecord::TNF::TNF_WELL_KNOWN) {
              Serial.println("NDEF TNF record is not WELL_KNOWN");
              return;
            }

            if (record.getTypeLength() != 1 ||
                ((char *)record.getType())[0] != 'T') {
              Serial.println("NDEF record has incorrect type.");
              return;
            }

            int payloadLength = record.getPayloadLength();
            const byte *payload = record.getPayload();

            int languageLen = (int)payload[0];
            String filePath = "";
            for (int c = 1 + languageLen; c < payloadLength; c++) {
              filePath += (char)payload[c];
            }

            Serial.print("file path: ");
            Serial.println(filePath);

            Serial.println("Start new playback");
            play_flag = true;
            re_init_audio_source();
            source_sd->open(filePath.c_str());
            mp3->begin(source_id3, out_i2s);
            Serial.println("Go back to main loop...");
            return;
          }
        }
      }
    } else {
      // no RFID
      if (play_flag) {
        Serial.println("Pause playback");
        play_flag = false;
      }
    }
  }
}
