# Display Architecture

## Overview

The firmware has two display modules:

- [`src/oled_ssd1306.cpp`](/Users/ace/projects/esp/USB-Keyboard-To-BLE-ESP32S3/src/oled_ssd1306.cpp)
- [`src/tft_display.cpp`](/Users/ace/projects/esp/USB-Keyboard-To-BLE-ESP32S3/src/tft_display.cpp)

`main.cpp` builds a small status object and sends it to both displays.

## Data Flow

1. `loop()` computes the current runtime state.
2. `main.cpp` fills `oled_ssd1306::Status`.
3. `main.cpp` fills `tft_display::Status`.
4. Each module decides whether a redraw is needed.
5. The module renders its own layout.

This means the display code does not need to know anything about BLE internals.

## Why Move TFT Code Out Of `main.cpp`

The temporary bring-up logic was useful for hardware debugging, but it created three problems:

- `main.cpp` became noisy
- TFT-specific code was mixed with BLE logic
- changing the screen UI required touching unrelated firmware

Now the TFT startup and rendering live in one place:

- [`src/tft_display.h`](/Users/ace/projects/esp/USB-Keyboard-To-BLE-ESP32S3/src/tft_display.h)
- [`src/tft_display.cpp`](/Users/ace/projects/esp/USB-Keyboard-To-BLE-ESP32S3/src/tft_display.cpp)

## OLED Rotation

The OLED is rotated in [`src/oled_ssd1306.cpp`](/Users/ace/projects/esp/USB-Keyboard-To-BLE-ESP32S3/src/oled_ssd1306.cpp) with:

```cpp
g_display.setRotation(3);
```

That rotates the screen counter-clockwise so the original top edge becomes the left edge.

## TFT Theme

The TFT runtime currently uses the ST7789 path that worked during bring-up.
The screen style is intentionally Fallout-inspired:

- dark green background
- phosphor-green text
- scan lines
- terminal-like panels
- status labels such as `STATE`, `MODE`, `LINK`, and `SIGNAL`

The theme is drawn in `renderStatus()` inside [`src/tft_display.cpp`](/Users/ace/projects/esp/USB-Keyboard-To-BLE-ESP32S3/src/tft_display.cpp).
