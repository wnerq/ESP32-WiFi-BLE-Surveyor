# Building with PlatformIO

PlatformIO is the primary build environment for current firmware.

## Source of truth

For current commits, the authoritative firmware source is `src/main.cpp`.

Do not edit the generated Arduino sketch directly. PlatformIO generates `arduino/WifiConnect/WifiConnect.ino` from `src/main.cpp` before each Build or Upload.

## Build configuration

The repository pins the build configuration in `platformio.ini`: ESP32 Dev Module, Arduino framework, pioarduino platform 55.03.311, Huge APP partition layout, NimBLE-Arduino 2.5.0, and a 115200 baud serial monitor.

## VS Code workflow

1. Check out the desired repository commit.
2. Open the repository root in VS Code.
3. Install the recommended PlatformIO extension when prompted.
4. Use PlatformIO Build to compile.
5. Use PlatformIO Upload to build changed files and flash the ESP32.
6. Use PlatformIO Monitor for the serial interface.

Build and Upload run `tools/export_arduino.py` first. The exporter rewrites the generated `.ino` only when its contents change.

If `src/main.cpp` changes, commit the corresponding generated `.ino` with it so that the same commit remains directly usable with Arduino IDE.

To regenerate the Arduino sketch manually from the repository root, run `python tools/export_arduino.py`.

PlatformIO build products under `.pio/` are ignored by Git and should not be committed.

## Optional SD CSV logging

The firmware supports an SPI micro SD adapter such as the HW124 when a card is
present. The default ESP32 VSPI wiring is:

| Adapter pin | ESP32 Dev Module |
| --- | --- |
| CS | GPIO5 |
| SCK | GPIO18 |
| MOSI | GPIO23 |
| MISO | GPIO19 |
| VCC | 3.3 V or the adapter's regulated input |
| GND | GND |

`SURVEY_SD_CS_PIN` can be defined in the build configuration when CS is wired
to another GPIO. SD initialization is optional; a missing or failed card does
not prevent the surveyor from starting. On each boot with a detected card, the
firmware scans existing `WIFI_*.CSV` and `BLE_*.CSV` files, increments the
highest boot number, and creates a new Wi-Fi CSV plus a BLE CSV when Bluetooth
Survey is enabled. Rows are appended as observations are retained.

## Migration baseline

The initial validated PlatformIO migration commit is `9fb542f`. Historical commits before that migration use the Arduino-oriented source layout described in `BUILD_ARDUINO.md`.
