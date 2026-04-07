#pragma once

#include <Arduino.h>

namespace tft_display {

struct Status {
  uint8_t slot_1based = 1;
  const char *profile = "";
  const char *state = "";
  bool eco = false;
  bool connected = false;
};

struct Pins {
  uint8_t cs;
  uint8_t dc;
  int8_t rst;
  uint8_t sck;
  uint8_t mosi;
  uint8_t miso;
  int8_t backlight;
};

// Initializes the 320x240 SPI TFT. Current runtime driver is ST7789.
bool begin(const Pins &pins);

bool ready();

// Draws a themed startup screen once the panel is ready.
void showBootScreen();

// Shows the live runtime status screen.
void showStatus(const Status &s);

// Shows a low-power style sleep screen.
void showSleep(const Status &s);

} // namespace tft_display
