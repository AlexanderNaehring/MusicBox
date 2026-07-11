#include "Board.h"

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
