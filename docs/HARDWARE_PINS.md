# Hardware Pins

## OLED

- `SDA -> GPIO8`
- `SCL -> GPIO18`
- `VCC -> 3.3V`
- `GND -> GND`

The OLED UI is rotated vertically in software.

## TFT

Current working TFT wiring in code:

- `CS -> GPIO10`
- `DC -> GPIO14`
- `RST -> GPIO15`
- `SCK -> GPIO12`
- `MOSI -> GPIO11`
- `MISO -> GPIO13`
- `BL/LED -> GPIO21`
- `VCC -> your working power rail`
- `GND -> GND`

## Notes

- The TFT runtime code is in [`src/tft_display.cpp`](/Users/ace/projects/esp/USB-Keyboard-To-BLE-ESP32S3/src/tft_display.cpp).
- The OLED runtime code is in [`src/oled_ssd1306.cpp`](/Users/ace/projects/esp/USB-Keyboard-To-BLE-ESP32S3/src/oled_ssd1306.cpp).
- If hardware behavior and earlier pin notes disagree, trust the hardware that actually works.
