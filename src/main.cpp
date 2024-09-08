#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

#include "AudioFileSourceSD.h"
#include "AudioFileSourceBuffer.h"
#include "AudioFileSourceID3.h"
#include "AudioOutputI2S.h"
#include "AudioGeneratorMP3.h"

#include "MFRC522v2.h"
#include "MFRC522DriverSPI.h"
#include "MFRC522DriverPinSimple.h"
#include "MFRC522Debug.h"

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
MFRC522DriverPinSimple ss_pin(RFID_CS);
const SPISettings spiSettings = SPISettings(SPI_CLOCK_DIV4, MSBFIRST, SPI_MODE0); 
MFRC522DriverSPI spi_driver{ss_pin, spi_2, spiSettings};
MFRC522 mfrc522{spi_driver};

// ESP8266Audio
#define AUDIO_SOURCE_BUFFER_SIZE 4096
AudioFileSourceSD *source_sd = NULL;
AudioFileSourceBuffer *source_buffer = NULL;
AudioFileSourceID3 *source_id3 = NULL;
AudioGeneratorMP3 *mp3 = NULL;
AudioOutputI2S *out_i2s = NULL;

String getHexString(byte *buffer, byte bufferSize) {
  String id = "";
  for (byte i = 0; i < bufferSize; i++) {
  id += buffer[i] < 0x10 ? "0" : "";
  id += String(buffer[i], HEX);
  }
  return id;
}

bool compareUid(MFRC522::Uid &uid1, MFRC522::Uid &uid2) {
  if (uid1.size != uid2.size)
    return false;
  for (byte i = 0; i < uid1.size; i++) {
    if (uid1.uidByte[i] != uid2.uidByte[i])
      return false;
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
  // uint8_t *buffer = (uint8_t*)malloc(sizeof(uint8_t) * AUDIO_SOURCE_BUFFER_SIZE);
  source_buffer = new AudioFileSourceBuffer(source_sd, AUDIO_SOURCE_BUFFER_SIZE);
  // ID3 file source for MP3 files reduces the delay until playback starts, and enabled ID3 Tag callbacks
  source_id3 = new AudioFileSourceID3(source_buffer);
}

void setup() {
  Serial.begin(115200);
  Serial.println("Setup...");

  
  Serial.println("SPI...");
  spi_1.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  Serial.println("SD...");
  if(!SD.begin(SD_CS, spi_1)) {
    Serial.println("Card Mount Failed");
    while(1) {}
  }


  Serial.println("RFID...");
  mfrc522.PCD_Init();
  MFRC522Debug::PCD_DumpVersionToSerial(mfrc522, Serial);

  //file = SD.open("/music/Sonya Belousova - My Sails Are Set (Feat AURORA).mp3");

  audioLogger = &Serial;
  Serial.println("Audio...");
  source_sd = new AudioFileSourceSD();
  //uint8_t *buffer = (uint8_t*)malloc(sizeof(uint8_t) * AUDIO_SOURCE_BUFFER_SIZE);
  source_buffer = new AudioFileSourceBuffer(source_sd, AUDIO_SOURCE_BUFFER_SIZE);
  // ID3 file source for MP3 files reduces the delay until playback starts, and enabled ID3 Tag callbacks
  source_id3 = new AudioFileSourceID3(source_buffer);

  out_i2s = new AudioOutputI2S();
  out_i2s->SetGain(10/100.0);

  mp3 = new AudioGeneratorMP3();

  Serial.println("Waiting for RFID...");
}

bool play_flag = false;

unsigned long last_rfid_time;
MFRC522::Uid last_uid;
unsigned long rfid_flag = 0;
#define RFID_INTERVAL 400
#define RFID_PAUSE_AFTER_INTERVAL 3

byte bufferATQA[2];
byte bufferSize = sizeof(bufferATQA);

void loop() {
  unsigned long now = millis();

  if(play_flag) {
    if (mp3->isRunning()) {
      unsigned long t = millis();
      if(!mp3->loop()) {
        Serial.println("mp3->stop();");
        mp3->stop();
      }

      t = millis() - t;
      Serial.print("mp3 loop took ");
      Serial.print(t);
      Serial.println("ms");
    } else {
      //source_sd->close();
      //source_sd->open("/music/test.mp3");
      //mp3->begin(source_id3, out_i2s);
    }
  }


  if(now - last_rfid_time > RFID_INTERVAL) {
    last_rfid_time = now;

    //if ( mfrc522.PICC_IsNewCardPresent()) {
    unsigned long t = millis();
    MFRC522::StatusCode result = mfrc522.PICC_RequestA(bufferATQA, &bufferSize);
    t = millis() - t;
    Serial.print("RFID check took ");
    Serial.print(t);
    Serial.println("ms");
    if (result == MFRC522::StatusCode::STATUS_OK) {
      // RFID detected
      // Serial.println("RFID detected");
      if (rfid_flag)  {
        // if the RFID pause countdown is set, just re-set to maximum delay
        rfid_flag = RFID_PAUSE_AFTER_INTERVAL;
      } else {
        // no current RFID pause countdown active -> start new or resume last playback
        if (mfrc522.PICC_ReadCardSerial()) {
          rfid_flag = RFID_PAUSE_AFTER_INTERVAL;
          Serial.println(getHexString(mfrc522.uid.uidByte, mfrc522.uid.size));
          // check if new RFID tag is equal to last_uid, or it's completely new -> resume or start fresh
          if (compareUid(mfrc522.uid, last_uid)) {
            // current RFID is same as last RFID, resume playback 
            Serial.println("Resume playback");
            play_flag = true;
          } else {
            // different RFID, read card and start new playback
            memcpy(&last_uid, &mfrc522.uid, sizeof(mfrc522.uid));

            Serial.println("Start new playback");
            play_flag = true;

            // if(mp3->isRunning())
            //   mp3->stop();

            // if (source_id3->isOpen()) {
            //   Serial.println("Close last file");
            //   source_id3->close();
            // }
            re_init_audio_source();

            // source_sd->open("/music/Sonya Belousova - My Sails Are Set (Feat AURORA).mp3");
            source_sd->open("/music/Alan Walker - Fade.mp3");
            mp3->begin(source_id3, out_i2s);

            // jumpstart playback, e.g. to fill source buffer
            for (short int i = 0; i < 1000; i++) {
              // if(mp3->isRunning())
                mp3->loop();
            }
            Serial.println("Go back to main loop...");
            
          }
          // MFRC522Debug::PICC_DumpToSerial(mfrc522, Serial, &(mfrc522.uid));
        }
      }
    } else if(rfid_flag > 0) {
      // no RFID detected and pause countdown is still active
      rfid_flag -= 1;
      if(rfid_flag == 0) {
        Serial.println("RFID removed");
        Serial.println("Pause playback");
        play_flag = false;
      }
    }
  }
}
