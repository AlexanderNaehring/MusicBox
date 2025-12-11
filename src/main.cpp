#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include <vector>

#include "AudioFileSourceBuffer.h"
#include "AudioFileSourceID3.h"
#include "AudioFileSourceSD.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include "ESP32Encoder.h"
#include "MFRC522.h"
#include "NfcAdapter.h"
#include "OneButton.h"
#include "git_info.h"

#define DEBUG true
#define D_LOG \
  if (DEBUG) Serial

// Button inputs
#define BTN_CARD_INSIDE 17
OneButton btnNext(35, true, false);
OneButton btnPrev(34, true, false);

// Encoder
#define RotaryA 33
#define RotaryB 32
ESP32Encoder rotaryGain;

// RGB LED
#define LED_R 4
#define LED_G 27
#define LED_B 16

#define RGB_Waiting 200, 200, 200
#define RGB_Error 200, 0, 0
#define RGB_Play 0, 200, 0
#define RGB_Pause 200, 200, 0

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
MFRC522 mfrc522(RFID_CS, UINT8_MAX, spi_2);
NfcAdapter nfc = NfcAdapter(&mfrc522);

// ESP8266Audio
#define MinAudioGain 1
#define InitialAudioGain 8
#define MaxAudioGain 50
#define AUDIO_SOURCE_BUFFER_SIZE 1024 * 4
AudioFileSourceSD* source_sd = NULL;
AudioFileSourceBuffer* source_buffer = NULL;
AudioFileSourceID3* source_id3 = NULL;
AudioGeneratorMP3* mp3 = NULL;
AudioOutputI2S* out_i2s = NULL;

// General stuff
#define MAX_UID_LEN 10

// Player states
enum class DeviceState {
  SETUP,        // initial state
  IDLE,         // Waiting for card insertion, nothing in queue
  READING_NFC,  // (New) card detected, reading NFC data
  PLAYING,      // Playing audio from queue, card is inserted
  PAUSED,       // Card removed during playback.
  STOPPED       // Playback finished, wait for card to be removed.
};
DeviceState currentState = DeviceState::SETUP;

std::vector<char*> files{};
int currentFile = -1;
char* currentFolder = nullptr;
char* lastTrackFile = nullptr;
unsigned long lastPlayMillis = 0;

unsigned long last_rfid_check_time;
#define RFID_CHECK_INTERVAL 50

byte current_uid[MAX_UID_LEN];
byte last_uid[MAX_UID_LEN];

int64_t lastGain = InitialAudioGain;

void setLED(uint8_t red, uint8_t green, uint8_t blue) {
  analogWrite(LED_R, 255 - red);
  analogWrite(LED_G, 255 - green);
  analogWrite(LED_B, 255 - blue);
}

void setDeviceState(DeviceState newState) {
  if (currentState == newState) {
    return;  // Already in this state
  }

  // Log transition
  const char* stateNames[] = {"SETUP", "IDLE", "READING_NFC", "PLAYING", "PAUSED", "STOPPED"};
  Serial.printf("State: %s -> %s\n", stateNames[(int)currentState], stateNames[(int)newState]);

  // Update state
  currentState = newState;

  // Set LED based on new state
  switch (newState) {
    case DeviceState::IDLE:
      setLED(RGB_Waiting);
      break;
    case DeviceState::READING_NFC:
      setLED(RGB_Waiting);
      break;
    case DeviceState::PLAYING:
      setLED(RGB_Play);
      break;
    case DeviceState::PAUSED:
      setLED(RGB_Pause);
      break;
    case DeviceState::STOPPED:
      setLED(RGB_Waiting);
      break;
    default:
      setLED(RGB_Error);
      break;
  }
}

void re_init_audio_source(bool deleteSources = true) {
  Serial.printf("Re-init audio source...");

  if (mp3 && mp3->isRunning()) mp3->stop();

  if (deleteSources) {
    Serial.printf(" delete sources...");
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
  }

  Serial.printf(" create sources if required...");
  if (!source_sd) source_sd = new AudioFileSourceSD();
  if (!source_buffer)
    source_buffer = new AudioFileSourceBuffer(source_sd, AUDIO_SOURCE_BUFFER_SIZE);
  // ID3 file source for MP3 files reduces the delay until playback starts, and
  // enabled ID3 Tag callbacks
  if (!source_id3) source_id3 = new AudioFileSourceID3(source_buffer);

  Serial.printf(" done\n");
}

void stop() {
  Serial.printf("stop()\n");
  if (mp3 && mp3->isRunning()) {
    Serial.printf("   mp3->stop()\n");
    mp3->stop();
  }
  Serial.printf("  reset last_uid\n");
  memset(&last_uid, 0, MAX_UID_LEN);
  if (files.size() > 0) {
    Serial.printf("  clear files list\n");
    for (char* x : files) {
      free((void*)x);
    }
    files.clear();
  }
  Serial.printf("  reset flags\n");
  currentFile = -1;
  if (currentFolder) {
    free(currentFolder);
    currentFolder = nullptr;
  }

  setDeviceState(DeviceState::STOPPED);
}

char* strRight(const char* str, size_t n) {
  size_t len = strlen(str);
  if (n > len) n = len;
  return (char*)str + len - n;
}

void addFileToQueue(fs::File file) {
  // String path = file.path();
  // directly converting file.path() to a String totally breaks...
  // the String content is for example " b ?␟"...
  /*
  const char *c_path = file.path();
  String path = "";
  for (int c = 0;; c++) {
    if (c_path[c] == 0) {
      break;
    }
    path += (char)c_path[c];
    Serial.printf("current char: %c (%x), path: %s\n", c_path[c],
                  (byte)c_path[c], path);
  }
  */
  // the String breaks when adding the dot "."
  // use character arrays instead :(
  const char* tmp = file.path();
  if (strcmp(strRight(tmp, 4), ".mp3")) {
    Serial.printf("Skip %s, files must end with .mp3\n", tmp);
    return;
  }
  char* path = (char*)malloc((strlen(tmp) + 1) * sizeof(char));
  strcpy(path, file.path());
  Serial.printf("Add to queue: %s\n", path);
  files.push_back(path);
}

void addFolderToQueue(fs::File root) {
  if (!root.isDirectory()) {
    Serial.println("Not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      addFileToQueue(file);
    }
    file.close();
    file = root.openNextFile();
  }
}

void playNext() {
  Serial.println("playNext()");
  if (files.size() <= 0) {
    Serial.printf("Queue empty\n");
    return;
  }
  if (currentFile >= (int)files.size() - 1) {
    Serial.printf("End of queue (%d, %d)\n", currentFile, files.size());

    if (currentFolder && strlen(currentFolder) > 0 && lastTrackFile && strlen(lastTrackFile) > 0) {
      File file = SD.open(lastTrackFile, "w");
      if (file) {
        Serial.printf("clear lastTrackFile\n");
        file.printf("0");
        file.close();
      }
    }

    stop();
    return;
  }

  currentFile++;
  char* filepath = files[currentFile];

  // save current track number to SD
  if (currentFolder && strlen(currentFolder) > 0 && lastTrackFile && strlen(lastTrackFile) > 0) {
    File file = SD.open(lastTrackFile, "w");
    if (file) {
      Serial.printf("Writing track number to '/last.txt'...\n");
      file.printf("%d", currentFile);
      file.close();
    }
  }

  // start playback
  re_init_audio_source();
  source_sd->open(filepath);
  Serial.printf("mp3->begin() %s\n", filepath);
  mp3->begin(source_id3, out_i2s);
  lastPlayMillis = millis();
  setDeviceState(DeviceState::PLAYING);
}

void playPrev() {
  Serial.println("playPrev()");
  if (files.size() <= 0) {
    Serial.printf("Cannot play, queue empty\n");
    return;
  }

  // check if more than x seconds into the current file
  if ((millis() - lastPlayMillis) > 10000) {
    Serial.printf("Play current file from start");
    currentFile--;
    playNext();
    return;
  }

  // already at first file in queue?
  if (currentFile <= 0) {
    Serial.printf("Beginning of queue, restart current file\n");
    currentFile--;
    playNext();
    return;
  }

  // go to previous file in queue
  currentFile -= 2;
  playNext();
  return;
}

void playFirst() {
  Serial.println("playFirst()");
  currentFile = -1;
  playNext();
}

void playFileOrFolder(const char* path) {
  Serial.printf("playFileOrFolder(%s)\n", path);
  stop();
  int lastTrack = 0;

  File root = SD.open(path);
  if (!root) {
    Serial.printf("Failed to open %s\n", path);
    setLED(RGB_Error);
    return;
  }

  if (root.isDirectory()) {
    addFolderToQueue(root);

    if (currentFolder) free(currentFolder);
    currentFolder = strdup(path);

    if (lastTrackFile) free(lastTrackFile);
    lastTrackFile = (char*)malloc(strlen(path) + 10);
    sprintf(lastTrackFile, "%s/last.txt", path);

    File file = SD.open(lastTrackFile);
    if (file) {
      Serial.printf("Found '/last.txt' file, checking content...\n");
      lastTrack = file.parseInt();
      file.close();
    }

  } else {
    addFileToQueue(root);
  }
  root.close();

  if (files.size() > 0) {
    auto cstr_compare = [](const char* s1, const char* s2) { return strcmp(s1, s2) < 0; };
    sort(files.begin(), files.end(), cstr_compare);
    Serial.printf("Queue:\n");
    for (auto x : files) {
      Serial.printf("  %s\n", x);
    }
    if (lastTrack > 0 && lastTrack < files.size()) {
      Serial.printf("Jump to track %d\n", lastTrack);
      currentFile = lastTrack - 1;
    } else {
      Serial.printf("Start queue from start\n");
    }
    playNext();
  } else {
    // no files in queue
    setLED(RGB_Error);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Welcome to MusicBox");
  Serial.printf("Built from git commit %s on %s at %s\n", GIT_COMMIT, __DATE__, __TIME__);

  Serial.println("Setup...");

  // LED
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  setLED(RGB_Error);

  // Buttons
  pinMode(BTN_CARD_INSIDE, INPUT_PULLUP);
  btnNext.attachClick(playNext);
  btnPrev.attachClick(playPrev);
  btnPrev.attachLongPressStart(playFirst);

  // Rotary
  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  rotaryGain.attachHalfQuad(RotaryA, RotaryB);
  rotaryGain.setCount(InitialAudioGain);

  // SD card
  Serial.println("SD_SPI...");
  spi_1.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  Serial.println("SD...");
  if (!SD.begin(SD_CS, spi_1)) {
    Serial.println("Error: Card Mount Failed");
    return;
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

  Serial.println("Setup ready...");
  setDeviceState(DeviceState::IDLE);
}

void loop() {
  unsigned long now = millis();

  // update buttons
  btnNext.tick();
  btnPrev.tick();

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

  bool cardPresent = (digitalRead(BTN_CARD_INSIDE) == LOW);

  switch (currentState) {
      /////////////////////////////////////////////////////////////////////////////////
    case DeviceState::IDLE:
      // waiting for new NFC card to be detected.
      // No playback or other activity.
      if (cardPresent) {
        setDeviceState(DeviceState::READING_NFC);
      }
      break;

      /////////////////////////////////////////////////////////////////////////////////
    case DeviceState::READING_NFC:
      // try to read NFC card
      if (!cardPresent) {
        // card removed during reading
        setDeviceState(DeviceState::IDLE);
        break;
      }

      // throttle NFC checks
      if (now - last_rfid_check_time < RFID_CHECK_INTERVAL) {
        break;
      }
      last_rfid_check_time = now;
      Serial.print(".");
      if (nfc.tagPresent()) {
        Serial.println("RFID detected");
        NfcTag tag = nfc.read();
        Serial.print("UID: ");
        Serial.println(tag.getUidString());

        memset(&current_uid, 0, MAX_UID_LEN);
        byte current_uid_len = MAX_UID_LEN;
        tag.getUid(current_uid, &current_uid_len);

        if (memcmp(last_uid, current_uid, current_uid_len) == 0) {
          // Same card
          Serial.println("resume playback");
          setDeviceState(DeviceState::PLAYING);
        } else {
          // Different card - read and start new playback
          memset(&last_uid, 0, MAX_UID_LEN);

          if (!tag.hasNdefMessage()) {
            Serial.println("NFC Tag has no NDEF message");
            setDeviceState(DeviceState::STOPPED);
            setLED(RGB_Error);
            break;
          }

          NdefMessage message = tag.getNdefMessage();
          if (message.getRecordCount() < 1) {
            setDeviceState(DeviceState::STOPPED);
            setLED(RGB_Error);
            break;
          }

          NdefRecord record = message.getRecord(0);  // read 1st record
          // NdefRecord record = message[i]; // alternate syntax

          if (record.getTnf() != NdefRecord::TNF::TNF_WELL_KNOWN) {
            Serial.println("NDEF TNF record is not WELL_KNOWN");
            setLED(RGB_Error);
            break;
          }

          if (record.getTypeLength() != 1 || ((char*)record.getType())[0] != 'T') {
            Serial.println("NDEF record has incorrect type.");
            setLED(RGB_Error);
            break;
          }

          int payloadLength = record.getPayloadLength();
          const byte* payload = record.getPayload();

          int languageLen = (int)payload[0];
          String filePath = "";
          for (int c = 1 + languageLen; c < payloadLength; c++) {
            filePath += (char)payload[c];
          }

          Serial.printf("Path: %s\n", filePath.c_str());

          re_init_audio_source();
          playFileOrFolder(filePath.c_str());

          // Remember current RFID UID
          memcpy(&last_uid, &current_uid, current_uid_len);

          setDeviceState(DeviceState::PLAYING);
        }
      }
      break;

      /////////////////////////////////////////////////////////////////////////////////
    case DeviceState::PLAYING:
      if (!cardPresent) {
        setDeviceState(DeviceState::PAUSED);
        break;
      }

      if (mp3->isRunning()) {
        if (!mp3->loop()) {
          playNext();
        }
      } else {
        // Audio stopped unexpectedly - maybe SD card error
        Serial.println("Audio stopped unexpectedly");
        setDeviceState(DeviceState::STOPPED);
        setLED(RGB_Error);
      }
      break;

      /////////////////////////////////////////////////////////////////////////////////
    case DeviceState::PAUSED:
      if (cardPresent) {
        // Card inserted - resume playback (or start new card)
        setDeviceState(DeviceState::READING_NFC);
      }
      break;

    /////////////////////////////////////////////////////////////////////////////////
    case DeviceState::STOPPED:
      if (!cardPresent) {
        // Card removed - now ready for a new card
        setDeviceState(DeviceState::IDLE);
      }
      // While card is still in, just wait - do nothing
      break;

    /////////////////////////////////////////////////////////////////////////////////
    case DeviceState::SETUP:
      Serial.println("ERROR - invalid state SETUP during loop()");
      setDeviceState(DeviceState::IDLE);
      setLED(RGB_Error);
      while (1) {
      }
  }
}
