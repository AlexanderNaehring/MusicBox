#include "Board.h"

#include "AudioFileSourceFS.h"
#include "AudioGeneratorMP3.h"

#define LOG_TAG "Board"

SPIClass spi_sd(VSPI);
SPIClass spi_rfid(HSPI);

Board board;

void Board::begin() {
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  setLED(RGB_Error);
}

void Board::setLED(uint8_t red, uint8_t green, uint8_t blue) {
  analogWrite(LED_R, 255 - red);
  analogWrite(LED_G, 255 - green);
  analogWrite(LED_B, 255 - blue);
}

void Board::showState(DeviceState state) {
  switch (state) {
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

void Board::beginAudioOutput() { audioOutput_ = new AudioOutputI2S(); }

void Board::playCue(fs::FS& fs, const char* path) {
  if (!audioOutput_ || !fs.exists(path)) {
    LOGF("cue '%s' unavailable, skipping\n", path);
    return;
  }
  LOGF("playing cue '%s'\n", path);

  AudioFileSourceFS cueSource(fs);
  if (!cueSource.open(path)) {
    LOGF("failed to open cue '%s'\n", path);
    return;
  }
  AudioGeneratorMP3 cueMp3;
  if (!cueMp3.begin(&cueSource, audioOutput_)) {
    LOGF("failed to start cue '%s'\n", path);
    return;
  }
  while (cueMp3.isRunning()) {
    if (!cueMp3.loop()) {
      cueMp3.stop();
    }
  }
}
