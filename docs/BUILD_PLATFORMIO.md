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

## Migration baseline

The initial validated PlatformIO migration commit is `9fb542f`. Historical commits before that migration use the Arduino-oriented source layout described in `BUILD_ARDUINO.md`.
