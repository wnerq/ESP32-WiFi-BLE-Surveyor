# Building with Arduino IDE

Arduino IDE remains a supported compatibility path and is also the build method for historical firmware commits from before the PlatformIO migration.

## Which source file should I use?

For historical commits before the PlatformIO migration commit `9fb542f`, use the `.ino` source stored by that historical commit, normally under `WifiConnect/`.

For current commits after the migration, `src/main.cpp` is authoritative and `arduino/WifiConnect/WifiConnect.ino` is generated from it. Do not edit the generated `.ino` directly.

## Current Arduino IDE compatibility configuration

Use an ESP32 Arduino environment compatible with the repository configuration:

- Board: ESP32 Dev Module
- ESP32 Arduino Core: 3.3.11
- Flash: 4 MB
- Partition Scheme: Huge APP (3 MB No OTA / 1 MB SPIFFS)
- CPU Frequency: 240 MHz
- Serial: 115200 baud
- NimBLE-Arduino: 2.5.0 where required by the checked-out firmware

Open `arduino/WifiConnect/WifiConnect.ino`, select the matching board and partition settings, then compile and upload normally.

## Regenerating the sketch

For current commits, PlatformIO Build and Upload automatically regenerate the Arduino sketch. It can also be regenerated without compiling by running `python tools/export_arduino.py` from the repository root.

If the generated `.ino` differs after regeneration, commit it together with the corresponding `src/main.cpp` change.

## Historical builds

Git preserves the build layout that existed at each commit. When checking out an older revision, follow the files and documentation present in that revision rather than assuming the current PlatformIO layout existed at that time.

The PlatformIO migration boundary is commit `9fb542f` (`chore: Migrate firmware build to PlatformIO`).
