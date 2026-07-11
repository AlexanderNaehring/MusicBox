#define DEBUG true
#define HW_REV 1
#define BLE false
#define WIFI false
#define AllowSleep false

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <driver/adc.h>
#include <esp_adc_cal.h>

#include <vector>

#include "AudioFileSourceBuffer.h"
#include "AudioFileSourceFS.h"
#include "AudioFileSourceID3.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#if HW_REV == 2
#include "ESP32Encoder.h"
#endif
#include "MFRC522.h"
#include "NfcAdapter.h"
#include "OneButton.h"
#include "git_info.h"

#if BLE
#include "MusicBoxBLE.h"
#endif

#if WIFI
#include "MusicBoxWiFi.h"
#endif

void debugPrint(String msg) {
  if (DEBUG) Serial.println(msg);
#if BLE
  sendBleLog(msg + String("\n"));
#endif
}

// PINOUT definitions based on hardware revision
#if HW_REV == 1
// Button inputs
#define BTN_CARD_INSIDE 17
#define BTN_NEXT 35
#define BTN_PREV 34
// RGB LED
#define LED_R 4
#define LED_G 27
#define LED_B 16
#elif HW_REV == 2
// use GPIO 4 for card detection to allow wakeup from sleep
// red LED moved to GPIO17
// Button inputs
#define BTN_CARD_INSIDE GPIO_NUM_4
#define BTN_NEXT GPIO_NUM_35
#define BTN_PREV GPIO_NUM_34
// Encoder
#define RotaryA GPIO_NUM_33
#define RotaryB GPIO_NUM_32
// RGB LED
#define LED_R GPIO_NUM_17
#define LED_G GPIO_NUM_27
#define LED_B GPIO_NUM_16
#endif
// SD (VSPI)
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCK 18
#define SD_CS 5
// RFID (HSPI)
#define RFID_MOSI 13
#define RFID_MISO 12
#define RFID_SCK 14
#define RFID_CS 15
// SPI
SPIClass spi_sd(VSPI);
SPIClass spi_rfid(HSPI);

// General stuff
#define MAX_UID_LEN 10
#define BATTERY_SAMPLES 10
#define BATTERY_ADC_CHANNEL ADC1_CHANNEL_0  // GPIO36
#define BATTERY_CHECK_INTERVAL 30000        // 30 seconds
unsigned long lastBatteryCheck = 0;
// Color tuples
#define RGB_Waiting 200, 200, 200
#define RGB_Error 200, 0, 0
#define RGB_Play 0, 200, 0
#define RGB_Pause 200, 200, 0

// OneButton
// activeLow=true, pullupActive=false
OneButton btnNext(BTN_NEXT, true, false);
OneButton btnPrev(BTN_PREV, true, false);
#if HW_REV == 2
// Rotary Encoder
ESP32Encoder rotaryGain;
#endif
// RFID
MFRC522 mfrc522(RFID_CS, UINT8_MAX, spi_rfid);
NfcAdapter nfc = NfcAdapter(&mfrc522);
// ESP8266Audio
#define MinAudioGain 2
#define MaxAudioGain 52
#define InitialAudioGain 8
#define AUDIO_SOURCE_BUFFER_SIZE 1024 * 4
AudioFileSourceFS* source_fs = NULL;
AudioFileSourceBuffer* source_buffer = NULL;
AudioFileSourceID3* source_id3 = NULL;
AudioGeneratorMP3* mp3 = NULL;
AudioOutputI2S* out_i2s = NULL;
// Filesystem
fs::FS* filesystem = NULL;

// Player states
enum class DeviceState {
  SETUP,        // initial state
  IDLE,         // Waiting for card insertion, nothing in queue
  READING_NFC,  // (New) card detected, reading NFC data
  PLAYING,      // Playing audio from queue, card is inserted
  PAUSED,       // Card removed during playback.
  STOPPED,      // Playback finished, wait for card to be removed.
  ERROR         // ERROR state
};
DeviceState currentState = DeviceState::SETUP;
RTC_DATA_ATTR DeviceState shutdownDeviceState = DeviceState::SETUP;

std::vector<char*> files{};
int currentFile = -1;
char* currentFolder = nullptr;
char* lastTrackFile = nullptr;
unsigned long lastPlayMillis = 0;

unsigned long last_rfid_check_time;
#define RFID_CHECK_INTERVAL 50

byte current_uid[MAX_UID_LEN];
byte last_uid[MAX_UID_LEN];

#if HW_REV == 1
int lastGain = InitialAudioGain;
#elif HW_REV == 2
int64_t lastGain = InitialAudioGain;
#endif

void setLED(uint8_t red, uint8_t green, uint8_t blue) {
  analogWrite(LED_R, 255 - red);
  analogWrite(LED_G, 255 - green);
  analogWrite(LED_B, 255 - blue);
}

unsigned long lastStateSwitch = millis();
String getDeviceStateStr(DeviceState state = currentState) {
  switch (state) {
    case DeviceState::SETUP:
      return "SETUP";
    case DeviceState::IDLE:
      return "IDLE";
    case DeviceState::READING_NFC:
      return "READING_NFC";
    case DeviceState::PLAYING:
      return "PLAYING";
    case DeviceState::PAUSED:
      return "PAUSED";
    case DeviceState::STOPPED:
      return "STOPPED";
    case DeviceState::ERROR:
      return "ERROR";
    default:
      return "UNKNOWN/ERROR";
  }
}

void setDeviceState(DeviceState newState) {
  if (currentState == newState) {
    return;
  }

  // Log transition
  Serial.printf("State: %s -> %s\n", getDeviceStateStr(currentState), getDeviceStateStr(newState));

  // Update state
  currentState = newState;
  lastStateSwitch = millis();

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
#if BLE
  bleInfo.setState(getDeviceStateStr(newState));
#endif
}

unsigned long getMsInState() { return millis() - lastStateSwitch; }

esp_adc_cal_characteristics_t* adc_chars;

float readBatteryPct() {
  uint32_t adcReading = 0;

  // Take multiple samples and average
  for (int i = 0; i < BATTERY_SAMPLES; i++) {
    adcReading += adc1_get_raw(BATTERY_ADC_CHANNEL);
  }
  adcReading /= BATTERY_SAMPLES;
  // Convert ADC reading to voltage in mV
  uint32_t voltage_mv = esp_adc_cal_raw_to_voltage(adcReading, adc_chars);
  float voltage = voltage_mv / 1000.0 * 2.0;  // 1:2 voltage divider

  Serial.printf("Battery voltage: %.2f V\n", voltage);

  float voltage_min = 3.2;
  float voltage_max = 4.2;
  float pct = ((voltage - voltage_min) / (voltage_max - voltage_min)) * 100.0;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;

#if BLE
  bleInfo.setBatteryPct(pct);
#endif
  return pct;
}

void setupBatteryMonitoring() {
  // Configure ADC
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(BATTERY_ADC_CHANNEL, ADC_ATTEN_DB_12);
  adc_chars = (esp_adc_cal_characteristics_t*)calloc(1, sizeof(esp_adc_cal_characteristics_t));
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 1100, adc_chars);

  float voltage = readBatteryPct();
  Serial.printf("Battery voltage: %.2f V\n", voltage);
}

void re_init_audio_source(bool deleteSources = true) {
  Serial.printf("Re-init audio source...");

  if (mp3 && mp3->isRunning()) mp3->stop();

  if (deleteSources) {
    Serial.printf(" delete audio sources...");
    if (source_fs) {
      delete source_fs;
      source_fs = NULL;
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
  if (!source_fs) source_fs = new AudioFileSourceFS(*filesystem);
  if (!source_buffer)
    source_buffer = new AudioFileSourceBuffer(source_fs, AUDIO_SOURCE_BUFFER_SIZE);
  // ID3 file source for MP3 files reduces the delay until playback starts, and
  // enabled ID3 Tag callbacks
  if (!source_id3) source_id3 = new AudioFileSourceID3(source_buffer);

  Serial.printf(" done\n");
}

void stop(bool setDeviceToStopped = true) {
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

  if (setDeviceToStopped) {
    setDeviceState(DeviceState::STOPPED);
  }
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
      File file = filesystem->open(lastTrackFile, "w");
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
    File file = filesystem->open(lastTrackFile, "w");
    if (file) {
      Serial.printf("Writing track number to '/last.txt'...\n");
      file.printf("%d", currentFile);
      file.close();
    }
  }

  // start playback
  re_init_audio_source();
  source_fs->open(filepath);
  Serial.printf("mp3->begin() %s\n", filepath);
  mp3->begin(source_id3, out_i2s);
  lastPlayMillis = millis();
  setDeviceState(DeviceState::PLAYING);

#if BLE
  bleInfo.beginUpdate();
  bleInfo.setState(getDeviceStateStr());
  bleInfo.setBatteryPct(100);
  bleInfo.setFolder(currentFolder ? String(currentFolder) : String(""));
  bleInfo.setFile(String(filepath));
  bleInfo.setTrackIdx(currentFile + 1);
  bleInfo.setTrackTotal((int)files.size());
  bleInfo.endUpdate();
#endif
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

#if HW_REV == 1
void volumeDown() {
  lastGain = lastGain - 2;
  if (lastGain < MinAudioGain) lastGain = MinAudioGain;
  Serial.printf("Volume: %d\n", lastGain);
  out_i2s->SetGain(lastGain / 100.0);
}

void volumeUp() {
  lastGain = lastGain + 2;
  if (lastGain > MaxAudioGain) lastGain = MaxAudioGain;
  Serial.printf("Volume: %d\n", lastGain);
  out_i2s->SetGain(lastGain / 100.0);
}

bool bothHeldHandled = false;

void onNextLongPress() {
  if (digitalRead(BTN_PREV) == LOW) {
    if (!bothHeldHandled) {
      bothHeldHandled = true;
      playFirst();
    }
  } else {
    playNext();
  }
}

void onPrevLongPress() {
  if (digitalRead(BTN_NEXT) == LOW) {
    if (!bothHeldHandled) {
      bothHeldHandled = true;
      playFirst();
    }
  } else {
    playPrev();
  }
}
#endif  // HW_REV == 1

void playFileOrFolder(const char* path) {
  Serial.printf("playFileOrFolder(%s)\n", path);
  stop(false);
  int lastTrack = 0;

  File root = filesystem->open(path);
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

    File file = filesystem->open(lastTrackFile);
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

void print_wakeup_reason() {
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0:
      Serial.println("Wakeup caused by external signal using RTC_IO");
      break;
    case ESP_SLEEP_WAKEUP_EXT1:
      Serial.println("Wakeup caused by external signal using RTC_CNTL");
      break;
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Wakeup caused by timer");
      break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      Serial.println("Wakeup caused by touchpad");
      break;
    case ESP_SLEEP_WAKEUP_ULP:
      Serial.println("Wakeup caused by ULP program");
      break;
    default:
      Serial.printf("Wakeup was not caused by sleep: %d\n", wakeup_reason);
      break;
  }
}

void lightSleep(uint64_t timeout_ms = 1000, uint8_t wakeup_pin = UINT8_MAX, int level = 0) {
#if !AllowSleep
  return;
#endif
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (wakeup_pin == UINT8_MAX && timeout_ms == 0) {
    timeout_ms = 1000;
  }
  if (wakeup_pin != UINT8_MAX) esp_sleep_enable_ext0_wakeup((gpio_num_t)wakeup_pin, level);
  if (timeout_ms > 0) esp_sleep_enable_timer_wakeup(timeout_ms * 1000);  // 100 ms

  Serial.println("light sleep...");
  if (ESP_OK == esp_light_sleep_start()) {
    print_wakeup_reason();
  } else {
    Serial.println("Error going to light sleep");
  }
}

void shutdown(uint8_t wakeup_pin = UINT8_MAX, int level = 0) {
#if !AllowSleep
  return;
#endif
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (wakeup_pin != UINT8_MAX) esp_sleep_enable_ext0_wakeup((gpio_num_t)wakeup_pin, level);
  shutdownDeviceState = currentState;
  Serial.println("Going to deep sleep...");
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  Serial.println("Welcome to MusicBox");
  Serial.printf("Built from git commit %s on %s at %s\n", GIT_COMMIT, __DATE__, __TIME__);

  print_wakeup_reason();
  setupBatteryMonitoring();

  Serial.printf("Last shutdown state: %s\n", getDeviceStateStr(shutdownDeviceState));

  // LED
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  setLED(RGB_Error);

  // Buttons
  pinMode(BTN_CARD_INSIDE, INPUT_PULLUP);
#if HW_REV == 1
  btnNext.attachClick(volumeUp);
  btnNext.attachLongPressStart(onNextLongPress);
  btnNext.attachLongPressStop([]() { bothHeldHandled = false; });
  btnPrev.attachClick(volumeDown);
  btnPrev.attachLongPressStart(onPrevLongPress);
  btnPrev.attachLongPressStop([]() { bothHeldHandled = false; });
#elif HW_REV == 2
  btnNext.attachClick(playNext);
  btnPrev.attachClick(playPrev);
  btnPrev.attachLongPressStart(playFirst);

  // Rotary
  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  rotaryGain.attachHalfQuad(RotaryA, RotaryB);
  rotaryGain.setCount(InitialAudioGain);
#endif

  // SD card
  Serial.println("SD_SPI...");
  spi_sd.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  Serial.println("Try connecting SD with 40 MHz...");
  if (!SD.begin(SD_CS, spi_sd, 40000000)) {
    Serial.println("Try connecting SD with 25 MHz...");
    if (!SD.begin(SD_CS, spi_sd)) {
      Serial.println("Error: SD card mount failed");
      return;
    }
  }
  filesystem = &SD;

  // RFID - NFC
  Serial.println("RFID...");
  spi_rfid.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_CS);
  mfrc522.PCD_Init();
  // code from mfrc522.PCD_DumpVersionToSerial();
  byte v = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  if ((v == 0x00) || (v == 0xFF)) {
    Serial.println(F("WARNING: MFRC522 communication failure, is the MFRC522 properly connected?"));
    return;
  }
  nfc.begin();

  // Audio
  audioLogger = &Serial;
  Serial.println("Audio...");
  // re_init_audio_source();  // delay init until first playback

  out_i2s = new AudioOutputI2S();
  out_i2s->SetGain(InitialAudioGain / 100.0);
  mp3 = new AudioGeneratorMP3();

// BLE
#if BLE
  Serial.println("BLE...");
  setupBLE("MusicBox");
#endif

// WIFI
#if WIFI
  if (digitalRead(BTN_PREV) == LOW) {
    Serial.println("BTN_PREV pressed, start WIFI setup mode");
    setupWifi(*filesystem);
  } else {
    Serial.println("BTN_PREV not pressed, skip WIFI setup");
    WiFi.mode(WIFI_OFF);
  }
#endif

  Serial.println("Setup ready...");
  setDeviceState(DeviceState::IDLE);

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason != 0 && shutdownDeviceState == DeviceState::STOPPED) {
    Serial.println("Woke up after previous STOPPED state");
    setDeviceState(DeviceState::STOPPED);
  }
}

void loop() {
  unsigned long now = millis();

  // update buttons
  btnNext.tick();
  btnPrev.tick();

#if HW_REV == 2
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
#endif

  // check battery every interval
  if (now - lastBatteryCheck > BATTERY_CHECK_INTERVAL) {
    lastBatteryCheck = now;
    readBatteryPct();
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
#if HW_REV == 1
      lightSleep(250);
#elif HW_REV == 2
      lightSleep(5000, BTN_CARD_INSIDE, 0);
#endif
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
#if HW_REV == 1
      lightSleep(250);
#elif HW_REV == 2
      lightSleep(5000, BTN_CARD_INSIDE, 0);
#endif
      break;

    /////////////////////////////////////////////////////////////////////////////////
    case DeviceState::STOPPED:
      if (!cardPresent) {
        // Card removed - now ready for a new card
        setDeviceState(DeviceState::IDLE);
      }
#if HW_REV == 1
      // While card is still in, just wait - do nothing
      lightSleep(1000);
#elif HW_REV == 2
      // save energy by sleeping until card is removed
      if (getMsInState() >= 1 * 60 * 1000) {
        shutdown(BTN_CARD_INSIDE, 1);
      } else {
        lightSleep(5000, BTN_CARD_INSIDE, 1);
      }
#endif
      break;

    /////////////////////////////////////////////////////////////////////////////////
    case DeviceState::ERROR:
      Serial.println("ERROR STATE - restarting in 10 seconds...");
      setLED(RGB_Error);
      delay(10000);
      ESP.restart();
      break;
    /////////////////////////////////////////////////////////////////////////////////
    case DeviceState::SETUP:
      Serial.println("ERROR - invalid state SETUP during loop()");
    default:
      setDeviceState(DeviceState::ERROR);
  }
}

#if BLE
void onBleCommand(String command) {
  debugPrint("BLE CMD Rx: " + command);

  if (command == "CMD:NEXT") {
    if (currentState == DeviceState::PLAYING || currentState == DeviceState::PAUSED) playNext();
  } else if (command == "CMD:PREV") {
    if (currentState == DeviceState::PLAYING || currentState == DeviceState::PAUSED) playPrev();
  } else if (command == "CMD:RESTART") {
    debugPrint("Rebooting...");
    delay(500);
    ESP.restart();
  } else if (command == "CMD:DIAG") {
    debugPrint("--- DIAGNOSTICS ---");
    debugPrint("Uptime: " + String(millis() / 1000) + "s");
    debugPrint("Heap Free: " + String(ESP.getFreeHeap()));
    debugPrint("Build: " + String(GIT_COMMIT) + " at " + String(__DATE__) + " at " +
               String(__TIME__));
  } else if (command == "CMD:TREE") {
    debugPrint("--- FILE TREE ---");
    // File root = filesystem->open("/");
    // print...
    // root.close();
    debugPrint("--- END TREE ---");
  }
}
#endif