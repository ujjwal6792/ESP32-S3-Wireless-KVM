# ESP32-S3 USB Keyboard to BLE (4-Slot)

Firmware that turns an **ESP32-S3** into a Bluetooth keyboard bridge:
plug in a wired USB keyboard, then switch it between 4 BLE slots using hotkeys.

## Latest Changes

- Moved TFT runtime code into a dedicated module: `src/tft_display.*`
- Switched the working TFT runtime path to `ST7789`
- Added a Fallout-style TFT status screen
- Rotated the OLED UI vertically so the original top edge is on the left
- Added documentation in `docs/` explaining the display flow and C++ file structure
- Added default ESP32-S3 TFT pin mapping:
  - `CS=GPIO10`
  - `DC=GPIO14`
  - `RST=GPIO15`
  - `SCK=GPIO12`
  - `MOSI=GPIO11`
  - `MISO=GPIO13`
  - `LED/BL=GPIO21`
- Aligned the default OLED I2C pins with the handheld wiring plan:
  - `SDA=GPIO8`
  - `SCL=GPIO18`
- LED type updated to **SK6812 GRBW** (`NEO_GRBW`) with explicit white-channel control.
- Slot colors updated to custom Catppuccin-style colors:
  - Slot 1: `0x04A5E5` (Blue)
  - Slot 2: `0xFE640B` (Orange)
  - Slot 3: `0xD20F39` (Red)
  - Slot 4: `0x40A02B` (Green)
- Added **Consumer Control (media keys / volume)** HID report (Report ID 2).
- BLE device names now follow fixed slot profiles:
  - Slot 1: `ESP-Mac`
  - Slot 2: `ESP-Windows`
  - Slot 3: `ESP-Linux`
  - Slot 4: `ESP-Android`
- Slot identity MAC suffix now uses `0x40 + slot`.
- Increased NimBLE storage limits to improve Android reconnect stability:
  - `CONFIG_BT_NIMBLE_MAX_BONDS=8`
  - `CONFIG_BT_NIMBLE_MAX_CCCDS=32`
- Power management behavior simplified:
  - Eco mode changes LED state after idle.
  - Deep sleep after `IDLE_TIME_SLEEP_MS` (default 20 minutes).
- Added **BOOT button actions**:
  - Short press: switch to next slot.
  - Long press (~1.5s): clear stored bonds and enter pairing mode on current slot.

## Features

- 4 switchable BLE slots.
- Pairing mode per slot (`Shift + Insert + [1-4]`).
- Reconnect mode per slot (`Insert + [1-4]`).
- Factory reset (`Shift + Insert + 0`) clears bonds and restarts.
- Deep sleep wake on **BOOT** button (GPIO0 low).
- Supports standard keyboard reports and media/consumer reports.

## Hardware

1. ESP32-S3 dev board (USB host capable).
2. USB OTG adapter (USB-C to USB-A female).
3. Wired USB keyboard.
4. Power source (USB/battery).
5. 1x addressable LED on GPIO48 (current code targets SK6812 GRBW).
6. Optional OLED on I2C: `SDA=GPIO8`, `SCL=GPIO18`.
7. Optional 2.8in 320x240 SPI TFT:
   - `VCC -> 3.3V`
   - `GND -> GND`
   - `LED -> GPIO21`
   - `CS -> GPIO10`
   - `RESET -> GPIO15`
   - `DC/RS -> GPIO14`
   - `SCK -> GPIO12`
   - `SDI/MOSI -> GPIO11`
   - `SDO/MISO -> GPIO13`

## TFT Demo

- On boot, the TFT shows a themed startup screen.
- After boot, it switches to a live Fallout-style status screen showing slot, profile, BLE state, eco mode, and link status.
- The current runtime code targets the working `ST7789` driver path.
- Touch pins are intentionally unused.

## Docs

- `docs/CPP_BASICS.md`
- `docs/DISPLAY_ARCHITECTURE.md`
- `docs/HARDWARE_PINS.md`

## LED Behavior

### Slot Colors

- Slot 1: Blue `0x04A5E5`
- Slot 2: Orange `0xFE640B`
- Slot 3: Red `0xD20F39`
- Slot 4: Green `0x40A02B`

### State Patterns

- **Connected:** solid slot color.
- **Pairing mode:** breathing slot color.
- **Reconnect mode:** triple-blink slot color.
- **Deep sleep:** LED off.

## Hotkeys

All commands use the **Insert** key.

| Action | Combo |
| :-- | :-- |
| Switch to slot 1-4 (reconnect mode) | `Insert + 1/2/3/4` |
| Switch to slot 1-4 (pairing mode) | `Shift + Insert + 1/2/3/4` |
| Switch to Mac slot | `Insert + M` |
| Switch to Windows slot | `Insert + W` |
| Switch to Linux slot | `Insert + L` |
| Switch to Android slot | `Insert + A` |
| Pair Mac/Windows/Linux/Android slot | `Shift + Insert + M/W/L/A` |
| Factory reset | `Shift + Insert + 0` |

## Board Button Controls

- `BOOT` short press: switch to next slot in reconnect mode.
- `BOOT` long press (~1.5s): clear BLE bonds and enter pairing mode on the current slot.
- `RESET`: hardware reboot only (no runtime action binding).

## Configuration

Main config lives in `src/main.cpp`:

- Brightness: `LED_BRIGHTNESS`
- Idle timers: `IDLE_TIME_ECO_MS`, `IDLE_TIME_SLEEP_MS`
- Slot colors: `slotColors[4]`
- Hotkeys: `KEY_INSERT`, `KEY_1..KEY_4`, `KEY_0`

## Build / Flash

1. Open project in VS Code with PlatformIO.
2. Connect ESP32-S3 via USB.
3. Run PlatformIO Upload.

## Notes

- On deep sleep, press the board **BOOT** button to wake.
- If pairing issues occur, run factory reset and remove old bonds on the host.

## License

MIT
