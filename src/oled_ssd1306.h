#pragma once

#include <Arduino.h>

namespace oled_ssd1306 {

struct Status {
  uint8_t slot_1based = 1;
  const char *profile = "";
  const char *state = "";
  bool eco = false;
  bool connected = false;
};

// SSD1306 128x64 over I2C.
// Default pins match your wiring: SDA=GPIO8, SCL=GPIO9.
bool begin(uint8_t sdaPin = 8, uint8_t sclPin = 9, uint8_t i2cAddr = 0x3C);

// Returns the detected/used I2C address (0 if not initialized).
uint8_t address();

// Optional: shown in footer (last 3 bytes) to help distinguish devices.
void setBaseMac(const uint8_t mac[6]);

// Renders a small UI:
// - Header (y=0..15): intended for the yellow band on 0.96" yellow/blue modules.
// - Body (y>=18): status text.
// Internally rate-limited and will redraw only when something changes.
void showStatus(const Status &s);

// Simple "going to sleep" screen.
void showSleep(const Status &s);

} // namespace oled_ssd1306
