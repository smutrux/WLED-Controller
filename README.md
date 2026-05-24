# WLED Controller

A battery-powered, handheld touchscreen controller for [WLED](https://kno.wled.ge/) smart lighting, built from scratch on an ESP32-S3 using ESP-IDF. The device communicates bidirectionally with one or more WLED instances over WiFi using the WLED JSON API — changes made on the controller reflect instantly on the lights, and changes made elsewhere (other apps, automations) reflect back on the controller's display.

This is a solo hardware + firmware project built as a portfolio piece, using components I had on hand: a Waveshare ESP32-S3 Touch LCD 3.5", spare battery cells and battery management hardware, a rotary encoder, a motorized fader from an audio mixing board, and a 3D printer.

---

## Goals

### Functional goals
- A physical, battery-powered device that controls WLED lights without needing a phone
- Touchscreen UI with brightness, color, on/off, and preset controls
- Rotary encoder for tactile UI navigation
- Two-way sync: the controller reflects the current state of the lights, not just the last command sent
- Clean, enclosable hardware in a 3D-printed case

### Project goals
- Learn ESP-IDF and FreeRTOS at a meaningful depth — not just Arduino wrappers
- Practice reading hardware documentation: datasheets, schematics, and vendor demos
- Build something incrementally, in numbered stages, with each stage fully working before moving on
- Produce code that is organized well enough to be readable on a portfolio

---

## Hardware

| Component | Part | Notes |
|---|---|---|
| Microcontroller | Waveshare ESP32-S3 Touch LCD 3.5" | ESP32-S3R8, 8MB PSRAM, 16MB flash |
| Display | ST7796, 3.5", 320×480, SPI | Driven via `esp_lcd` with DMA |
| Touch | FT6336 capacitive controller | I2C at 0x38, shared bus |
| IO Expander | TCA9554PWR | Controls LCD_RST and TP_INT via I2C |
| Power IC | AXP2101 | PMIC on the board; I2C accessible |
| IMU | QMI8658 | 6-axis, potential future use for significant motion wake |
| RTC | PCF85063A | For clock display or scheduled automations |
| Audio Codec | ES8311 | On-board; not used in this project |
| Input | Rotary encoder | External; wired to ESP32 GPIO |
| Input | Motorized fader | External; under evaluation for use |
| Battery | 9Wh pouch cell or 18650 cell(s) | Final choice pending current draw measurements |
| Battery management | TP4056 / DW01 boards | Charging + protection |
| Enclosure | 3D printed | Designed after electronics are finalized |

### Key hardware discovery: IO expander

The Waveshare board does not expose LCD_RST or the touch controller's INT/RST pins as direct GPIOs. Both are routed through a **TCA9554PWR I2C IO expander** (address 0x20). This is not clearly documented by Waveshare and was confirmed by reading the schematic. The consequence is that initialization order matters: I2C must be up before the display can be reset, and a custom `tca9554.c` driver is needed to talk to the expander before `esp_lcd` can initialize the panel.

---

## Software architecture

The firmware is written in C using **ESP-IDF 6.0** (not Arduino). Each concern lives in its own file; `main.c` only orchestrates init and task creation.

<!-- ```
wled-controller/
├── CMakeLists.txt
├── sdkconfig.defaults          ← committed; sdkconfig is gitignored
└── main/
    ├── main.c                  ← app_main: init sequence, LVGL task, mutex
    ├── idf_component.yml       ← fetches LVGL and ST7796 driver via component manager
    ├── board/
    │   ├── board.h             ← all pin definitions and display constants in one place
    │   ├── tca9554.c/.h        ← I2C IO expander driver (shadow registers, commit model)
    ├── display/
    │   ├── display.c/.h        ← ST7796 init via esp_lcd, DMA flush callback for LVGL
    ├── touch/
    │   ├── touch_ft6336.c/.h   ← FT6336 I2C touch driver, polling mode
    └── ui.c/.h                 ← LVGL widget tree, event handlers, state update helpers
``` -->
---

## Build and flash

### Prerequisites

- VS Code with the [Espressif IDF extension](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension)
- ESP-IDF v5.4 or later (v6.0 recommended; installed by the extension)

### First-time setup

```bash
# 1. Open the project folder in VS Code
#    The extension detects CMakeLists.txt automatically.

# 2. Fetch managed components
idf.py reconfigure

# 3. Build
idf.py build

# 4. Flash and open serial monitor
idf.py -p /dev/ttyACM0 flash monitor
```

On Windows the port will be something like `COM4`. The VS Code toolbar provides buttons for all of the above.
---

## Roadmap

Each stage produces a fully working, flashable build. Nothing is stubbed out — if a stage is marked complete, it runs on the hardware.

### Stage 1 — Display ✅
Initialize the ST7796 via `esp_lcd`. Manage LCD reset through the TCA9554 IO expander (not a direct GPIO). Set up DMA transfers. Display a solid color to confirm the pipeline works.

### Stage 2 — LVGL UI ✅
Integrate LVGL 8.x with the `esp_lcd` flush callback. Allocate double-buffered draw memory from PSRAM. Create an initial dark-themed UI: header bar with connection indicator, brightness slider, color preview swatch, and power button. All widget state is stubbed with local variables — real data comes in stage 8.

### Stage 3 — Touch ✅
Get the FT6336 touch controller reporting coordinates into LVGL's input device system.

### Stage 4 — Rotary encoder ✅
Decode signals from the encoder using the ESP32's **PCNT (Pulse Counter)** peripheral. PCNT handles the signal in hardware without polling, giving reliable decodes even at fast rotation speeds. The encoder's push button will be wired to a standard GPIO with interrupt.

### Stage 5 — WiFi ✅
Connect to a WiFi network using `esp_wifi`. Implement automatic reconnection using the event loop (`WIFI_EVENT_STA_DISCONNECTED` → retry with exponential backoff). Display connection status on the UI header dot (grey → yellow connecting → green connected).

### Stage 6 — Manual HTTP test ✅
Use `esp_http_client` to perform a `GET /json/state` to a hardcoded WLED IP and log the response. Confirm the JSON response parses correctly. This stage validates network reachability before any UI is wired to it.

### Stage 7 — Button triggers HTTP ✅
Wire the power button and brightness slider in the UI to `POST /json/state` with the appropriate JSON payload. Implement software debounce on the encoder button using a one-shot `esp_timer`.

### Stage 8 — Encoder controls brightness ✅
Translate PCNT count deltas into brightness POST requests. Debounce rapid turns with a short timer so that spinning quickly sends one request at the end of the motion rather than dozens during it.

### Stage 9 — Poll WLED state ✅
Spawn a task on core 0 that `GET /json/state`s every 0.5 - 30 seconds (configure in `idf.py menuconfig`). Parse the JSON response (brightness, color, on/off, effect name) and push updates to the UI via `lvgl_lock()`. This makes the controller a true two-way interface: changes from the WLED web app or automations appear on the controller without any user action.

### Stage 10 — Final UI
Redesign the UI layout now that all data flows are working. Likely a tabview: **Brightness / Color / Effects / Scenes**. The color tab will use LVGL's `lv_colorwheel` widget. The effects tab will show a scrollable list populated from `/json/eff`.

### Stage 11 — Power system
Measure actual current draw with WiFi active and screen on.
- **9Wh pouch cell** (~2400 mAh): compact, flat, fits a slim enclosure
- **Single 18650** (~3000 mAh): easier to replace, standard charger

Add a soft power-on button circuit using the AXP2101 PMIC already present on the board.

### Stage 12 — Enclosure
Design a 3D-printed enclosure sized to the battery choice. Include mounting for the rotary encoder, cable management for the LCD FPC, and a USB-C pass-through for charging. Consider whether the motorized fader is worth the enclosure complexity it adds.