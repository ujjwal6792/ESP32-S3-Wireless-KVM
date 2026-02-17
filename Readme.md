# ESP32-S3 Wireless KVM

A high-performance firmware that turns an **ESP32-S3** into a 4-Slot Wireless
KVM Switch.

Plug your wired mechanical keyboard into the ESP32, and it becomes a Bluetooth
keyboard that can toggle between 4 devices (Mac, Windows, Linux, Android)
instantly using hotkeys.

## Features

- **4 Device Slots:** Switch between Laptop, PC, Tablet, and Phone instantly.
- **Gaming-Grade Latency:** ~7.5ms latency mode for high-performance input.
- **Cross-Platform:**
  - **MacOS:** Fixed reconnect issues & ghosting.
  - **Linux:** Aggressive advertising to wake up `BlueZ` stack.
  - **Android:** Identity Key Exchange (IRK) to handle Random Mac Addresses.
- **Security Gatekeeper:** Rejects unauthorized connection attempts unless in
  "Pairing Mode".
- **Power Saving:**
  - **Eco Mode:** Auto-lowers polling rate after 10s idle.
  - **Deep Sleep:** Auto-shutdown after 20 mins idle (saves power bank).
- **Factory Reset:** Built-in hotkey to wipe all security bonds.

## Hardware Required

1.  **ESP32-S3 Development Board** (Must be S3 version for USB Host support).
2.  **USB OTG Adapter** (USB-C to USB-A Female).
3.  **Wired USB Keyboard** (Standard HID).
4.  **Power Source** (Power bank or LiPo battery).

**Wiring:**

- Plug the USB OTG adapter into the ESP32's **USB_OTG** port (usually the
  right-side port).
- Plug your keyboard into the adapter.
- Power the ESP32 via the **UART/COM** port or battery pins.

## Usage Guide

### 1. LED Status Colors

| Color            | Status                                    |
| :--------------- | :---------------------------------------- |
| **Solid**        | Connected & Active                        |
| **Triple Blink** | Reconnect Mode (Looking for known device) |
| **Slow Breathe** | Pairing Mode (Visible to new devices)     |
| **Off**          | Deep Sleep / Power Off                    |

- 🔴 **Red:** Slot 1
- 🟢 **Green:** Slot 2
- 🔵 **Blue:** Slot 3
- 🟡 **Yellow:** Slot 4

### 2. Hotkeys (The "Magic" Combo)

All commands use the **Insert** key.

| Action              | Combination                    | Description                                                |
| :------------------ | :----------------------------- | :--------------------------------------------------------- |
| **Switch Device**   | `Insert` + `1/2/3/4`           | Switches to slot. Attempts to reconnect to _saved_ device. |
| **Pair NEW Device** | `Shift` + `Insert` + `1/2/3/4` | Switches to slot & forces **Pairing Mode** (Breathe).      |
| **Factory Reset**   | `Shift` + `Insert` + `0`       | **Warning:** Wipes all pairings and reboots board.         |

### 3. Waking Up

If the device enters **Deep Sleep** (LEDs off, keyboard dead), you must press
the **BOOT Button** on the ESP32 to wake it up.

---

## Configuration & Customization

All settings are defined at the top of `src/main.cpp`.

### 1. Changing LED Colors

Find the `slotColors` array. These are standard Hex Color Codes (`0xRRGGBB`).

```cpp
// Red, Green, Blue, Yellow, Purple (Example)
uint32_t slotColors[4] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00};

2. Changing Hotkeys

If your keyboard doesn't have an Insert key, change KEY_INSERT to another usage ID (e.g., 0x48 for Pause/Break, or 0x39 for CapsLock).
C++

// Key Codes (USB HID Usage IDs)
#define KEY_INSERT      0x49 // Current: Insert
#define KEY_1           0x1E // '1' key

Ref: USB HID Usage Tables (Page 53)
3. Power Management Timers

Adjust how quickly the device sleeps to save battery.
C++

#define IDLE_TIME_ECO_MS      10000   // 10 Seconds -> Low Power Mode
#define IDLE_TIME_SLEEP_MS    1200000 // 20 Minutes -> Deep Sleep

 Installation

    Clone this repo.

    Open in VS Code with PlatformIO extension installed.

    Connect ESP32 via UART port.

    Run "Upload".

 Troubleshooting

    Linux won't auto-connect:

        Ensure you have Paired AND Trusted the device in bluetoothctl.

        The firmware uses aggressive 20ms advertising, so it should appear instantly.

    Android asks to pair repeatedly:

        This is fixed in v2.0 by enabling BLE_SM_PAIR_KEY_DIST_ID. Ensure you are using the latest code.

        If stuck, do a Factory Reset (Shift+Ins+0) and forget the device on Android.

    Keyboard lights are off:

        The ESP32 might be in Deep Sleep. Press the BOOT button on the board.

 License

MIT License - Feel free to modify and use!
```
