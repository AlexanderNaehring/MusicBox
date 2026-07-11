#ifndef MUSICBOX_BOARD_H
#define MUSICBOX_BOARD_H

#include <Arduino.h>
#include <SPI.h>

#include "AudioOutputI2S.h"
#include "Config.h"
#include "DeviceState.h"

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
#include <driver/gpio.h>
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

// Color tuples, expanded as the (r, g, b) arguments to setLED()/Board::setLED()
#define RGB_Waiting 200, 200, 200
#define RGB_Error 200, 0, 0
#define RGB_Play 0, 200, 0
#define RGB_Pause 200, 200, 0

// SPI buses
extern SPIClass spi_sd;
extern SPIClass spi_rfid;

// Owns the board's shared, passive peripherals: the RGB status LED and the
// I2S audio output (shared between the main Player and MusicBoxWifiDownloader's
// audio cues).
class Board {
 public:
  void begin();

  void setLED(uint8_t red, uint8_t green, uint8_t blue);
  // Sets the LED to the color conventionally associated with a device state.
  void showState(DeviceState state);

  // Creates the shared I2S output. Call once during setup(), before it's
  // handed to Player::begin().
  void beginAudioOutput();
  AudioOutputI2S* audioOutput() const { return audioOutput_; }

 private:
  AudioOutputI2S* audioOutput_ = nullptr;
};

extern Board board;

#endif
