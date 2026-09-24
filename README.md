# ESP32 Wireless Surveyor

An ESP32 Wi-Fi and Bluetooth wireless survey/logger with self-hosted web and serial interfaces, headless automatic surveying, compact in-memory history, optional RSSI history plotting, 2.4 GHz channel-interference analysis, diagnostics/self-tests, CSV export, session checkpoint/restore, and mDNS hostname access.

## Live Web Interface Demo

A static browser demonstration of the current interface is published with GitHub Pages:

**[Open the Wi-Fi / BLE Surveyor Demo](https://wnerq.github.io/ESP32-WiFi-BLE-Surveyor/)**

The demo uses simulated data and does not require ESP32 hardware. It demonstrates the V40a page layout with simulated interactions. The V45 firmware includes newer features that are not in the demo, including shared storage presets, configurable terminal capture, Wi-Fi activity holds, developer memory controls, movable cards, and mode-aware prefill tools. See [demo scope](docs/README_DEMO.md).

## What the project does

The project began as a serial Wi-Fi configuration utility and has grown into a portable wireless-survey instrument. The ESP32 can operate its own access point while simultaneously acting as a station, so the web interface remains usable even when infrastructure Wi-Fi is unavailable. Surveying is designed to continue headlessly without a browser or serial terminal connected.

The current firmware identifies as **V45**. Its navigation is organized by task:

| Page | Route | Purpose |
| --- | --- | --- |
| Wi-Fi | `/` | Survey controls, retained observations, RSSI history, channel analysis, connected network |
| Bluetooth | `/ble` | Optional BLE survey, device observations and history |
| System | `/system` | Device identity, runtime state, memory and health |
| Diagnostics | `/diagnostics` | Survey, radio, recovery, timing and transport diagnostics |
| SD Card | `/sd` | SD logging status, file viewer, editor, and downloads |
| Terminal | `/terminal` | Live firmware output and command entry; navigation link appears in Developer view |
| Settings | `/settings` | Network, power, survey and interface configuration |
| Help | `/help` | Feature explanations and operating guidance |

A persistent **View** selector controls information depth:

- **Standard** — normal operation and useful survey results.
- **Advanced** — deeper operational information and troubleshooting detail.
- **Developer** — implementation internals, instrumentation, and test facilities.

The views are cumulative: Standard ⊂ Advanced ⊂ Developer. The navigation, Live Updates control, View selector, and Theme selector are held in a collapsible sticky card. Its expanded/collapsed state is remembered in the browser.

## 3D Printed Enclosure
[Honeycomb Case](https://www.printables.com/model/1826305-esp32-honeycomb-case-push-together-no-hardwaretool)
## Hardware Connections

### SD Card (SPI)
For SD card logging, use an SPI microSD adapter (e.g., HW124) with the following pins:

| Adapter pin | ESP32 GPIO | Silkscreen |
| --- | --- | --- |
| CS | GPIO 5 | D5 |
| SCK | GPIO 18 | D18 |
| MOSI | GPIO 23 | D23 |
| MISO | GPIO 19 | D19 |
| VCC | 3.3V | 3.3V |
| GND | GND | GND |
## Key features

### Headless surveying

- Wi-Fi surveying starts automatically after boot.
- Surveying does not depend on a browser, serial terminal, or successful infrastructure Wi-Fi connection.
- An initial scan occurs shortly after startup.
- Configurable Wi-Fi scan interval: 5–3600 seconds.
- Scan interval is measured from completion of the previous successful scan.
- Manual **Scan Now** requests coexist with the automatic scheduler.
- Browser Live Updates repaint survey pages; they do not trigger scans.

### Optional scan / Wi-Fi wake button

Connect a normally-open momentary button between **GPIO27 and GND**. The firmware
uses the internal pull-up, so no external resistor is needed.

- **Short press:** starts a Wi-Fi scan on release (also wakes the radio if asleep).
- **Hold for 1 second:** wakes Wi-Fi and refreshes the configured access window.
  The hold and its release do not request a scan, including when Wi-Fi is already on.
- Automatic scans continue on their normal schedule. A press during an active scan
  does not queue another scan. A button held during startup is ignored until released.
- Input debounce is 30 ms. Set `SURVEY_BUTTON_PIN` at build time to change the GPIO,
  or to `-1` to disable it. Timing constants are beside the pin definition in `src/main.cpp`.
  Waking preserves the saved power mode; in radio-off mode, Wi-Fi sleeps again after
  the configured access window.

### Infrastructure Wi-Fi and Device AP

The ESP32 normally operates in `AP+STA` mode:

- **Infrastructure Wi-Fi** is the network the ESP32 joins as a station.
- **Device AP** is the network broadcast by the ESP32 for direct access.
- The default Device AP uses a unique SSID derived from the ESP32 MAC suffix, for example `ESP32-Surveyor-670F2C`.
- The AP-side interface is normally reachable at `http://192.168.4.1/`.
- Infrastructure connection failure does not stop surveying.
- Native ESP32/Arduino Wi-Fi auto-reconnect is used for infrastructure recovery.

Because the classic ESP32 has one 2.4 GHz Wi-Fi radio, AP, station, and scan activity share that radio. A scan can therefore briefly affect AP and HTTP responsiveness.

### Device hostname and mDNS

The surveyor has a configurable **Device Hostname**. The default is `surveyor`, giving the preferred friendly address:

```text
http://surveyor.local/
```

The hostname is stored in NVS. IP access remains available as a fallback because `.local` name resolution depends on the client and network.

### Wi-Fi survey

The Wi-Fi page is the default `/` page and primary survey workspace. Its current structure is:

1. Survey Status & Controls
2. History
3. RSSI History — selected network
4. Observed Networks
5. Links to detailed survey health on Diagnostics
6. Observed Channel Interference
7. Infrastructure Connected Network

The Standard Observed Networks table shows:

- SSID
- Channel
- Last RSSI
- Average RSSI
- Count
- Last Seen

Advanced adds Min and Max RSSI. Developer adds BSSID, Security, and First Seen.

RSSI history is keyed by **BSSID**, not SSID. Multiple access points can advertise the same SSID; keeping BSSID identity prevents measurements from different radios from being combined into one plot.

### Compact Wi-Fi history

Wi-Fi observations use a compact normalized representation rather than repeating SSID/BSSID and scan metadata in every record.

**Per observation:**

- AP-table reference
- scan-metadata reference
- RSSI

V45 puts AP identities, running summaries, scan timestamps, and 6-byte
`WifiObservation` records in one bounded RAM pool. History modes initially
reserve 16 AP slots; new APs grow that table in batches, using space previously
available to observations. Old observations are discarded when necessary.
Capture does not repeatedly allocate or free heap blocks.

The selected [Survey Focus](#survey-focus) controls the balance and what happens
when the pool fills. Signal History and Balanced Survey both retain changes
over time. Network Inventory retains one updated summary per AP, with no
measurement timeline. Clearing history returns the AP reservation to its
initial size in the history modes.

Timestamp metadata remains bounded at 512 scan groups in Wi-Fi-only history
mode and 64 with BLE enabled. Capacity depends on available RAM, AP diversity,
and the selected preset; consult the device's runtime figures. The 80 KB
Wi-Fi-only reserve is a target at allocation, before web services and runtime
work consume additional heap. See [V45 design and validation](docs/TEST_STORAGE_V45.md)
and the historical [V44 experiment](docs/TEST_STORAGE_V44.md).

### Observed Channel Interference

The Wi-Fi page provides advisory 2.4 GHz channel analysis based on observed APs and RSSI-weighted co-channel and adjacent-channel interference.

Standard view presents the recommendation and concise comparison. Advanced exposes the full channel table and methodology. The result is an estimate from observed Wi-Fi signals; it is not an airtime-utilization measurement and does not detect non-Wi-Fi interference.

### Bluetooth Low Energy survey

Bluetooth surveying is **disabled by default** because initializing the BLE stack materially reduces heap available for Wi-Fi history.

When disabled, the Bluetooth page explains the memory tradeoff before offering **Enable Bluetooth Survey**. Enabling or disabling BLE is persistent and requires a controlled restart so radio stacks and survey buffers can be allocated safely at boot.

When enabled, BLE can survey automatically and headlessly. The BLE page follows the same survey-first structure as Wi-Fi without forcing identical fields where the technologies differ.

The Standard BLE device table shows:

- Name
- Address
- Last RSSI
- Average RSSI
- Count
- Last Seen

Advanced adds Min and Max RSSI. Developer adds Address Type and First Seen.

BLE addresses are intentionally described as addresses rather than guaranteed physical-device identities. Random/private BLE addresses can change over time.

> **Current limitation:** dual Wi-Fi/BLE operation places substantial pressure on heap and web responsiveness. This remains an area for further engineering work; BLE is therefore best treated as an optional survey mode rather than a zero-cost addition to Wi-Fi surveying.

### Session checkpoint / restore

Active survey history lives primarily in RAM, but current firmware can create a structured session checkpoint in SPIFFS and restore it after a controlled reboot.

- Checkpoint contains survey history/table state and relative survey time information.
- Binary format includes a format version and CRC32 validation.
- Corrupt or incompatible checkpoints are rejected safely.
- Controlled restarts can checkpoint automatically.
- Manual checkpoint and discard controls are available in Developer view.
- This is **not continuous flash logging** and does not guarantee recovery after arbitrary power loss.
- A saved session may be incompatible after a survey-mode change if its saved tables cannot fit the new memory configuration; the firmware rejects that restore rather than corrupting data.

### CSV export

Retained Wi-Fi and BLE observations can be exported to CSV for analysis in Excel, Python, MATLAB, or other tools.

A Wi-Fi CSV contains one row for each AP/BSSID observation from each retained scan, for example:

```csv
scan,uptime_ms,uptime,ssid,bssid,channel,rssi_dbm,security,connected,hidden
1,15231,"0m 15s","MyNetwork","A4:CF:12:34:56:78",6,-44,"WPA2-PSK",YES,NO
2,25284,"0m 25s","MyNetwork","A4:CF:12:34:56:78",6,-48,"WPA2-PSK",YES,NO
```

CSV export is streamed so large histories do not require building the entire file in heap at once.

### SD card logging and downloads

When an SD card is present at boot, the surveyor writes Wi-Fi and, when enabled,
BLE observations to numbered CSV files. To avoid interrupting radio and web work
with an SD transaction for every observation, it appends a batch after a completed
scan reaches 50% of the active in-memory history capacity, then clears that batch
from RAM. A power loss before the next batch flush can lose the current partial
batch. The **SD Card** page lists those files.
**Read File** opens up to 8 KiB in the built-in viewer/editor; **Download File**
streams the complete selected file to the browser as an attachment, without
loading the log into the ESP32 heap.

### System health and diagnostics

The System page is intentionally separated from survey results. It answers: **What is the device doing, and is it healthy?**

Standard view emphasizes interpreted health:

- Wi-Fi subsystem state
- Bluetooth subsystem state (Disabled is neutral, not a failure)
- automatic surveying health
- memory health
- last reset

Advanced adds operational diagnostics such as free/minimum heap, survey memory mode, scan timing, checkpoint status, reconnect state, and mDNS status.

Developer adds implementation detail such as firmware file/build environment, chip/CPU details, flash/app partition information, allocation anatomy, boot heap checkpoints, raw test facilities, and diagnostic export.

### Settings

Ordinary settings stay available in Standard view. Current groups include:

- **Infrastructure Wi-Fi**
- **Device Hostname**
- **Device AP** / **Broadcast SSID**
- **Wi-Fi Power Saving**
- **Survey Mode**
- **Interface & Indicators**

Advanced also includes **Wi-Fi History Capture**, which can exclude future hidden-network observations from retained history while still detecting them for channel analysis.

Advanced adds items such as configuration backup/restore and selected troubleshooting details. Developer exposes implementation-oriented diagnostics export and internals.

### Developer memory settings

In Settings with Developer view selected, **Developer Memory** controls terminal
capture (Off, 1, 2, 8, 16 or 32 KiB) and Wi-Fi/Bluetooth signal plots. Defaults are
1 KiB capture and plots disabled. Save choices, then restart from System to apply.
Active and saved settings are shown separately. Configuration backup includes
`terminalBufferBytes` and `plotsEnabled`; older backups leave these unchanged.

The terminal buffer is allocated before survey storage. Off allocates no capture
buffer; USB output and web commands still work, but web command output requires
capture. Relative to the old 8 KiB buffer, Off frees 8192 bytes and the 1 KiB default
frees 7168 bytes, before allocator overhead. Freed memory feeds the existing survey
allocation policy (split with BLE when enabled). Disabling plots avoids temporary
graph allocations; it does not reduce the networking heap reserve. CSV and survey
capture continue. Export CSV before changing memory allocation and restarting;
a smaller pool can prevent complete checkpoint restoration.

A reported device configuration reached **482 AP summaries** with Network
Inventory, terminal capture Off and plots disabled. Capacity is auto-sized at
boot and varies with radio configuration and available heap; 482 is an observed
result, not a fixed limit.

### Card layouts

Drag a card title's arrow handle to move it, including on touch screens. With
keyboard focus on the handle, use Up/Down, Home or End. Escape cancels a drag.
**Reset Layout** restores the default order for the current page. Layouts are
stored only in the browser, separately by page; `/` and `/scan` share a layout.
Live-updating cards retain their positions, and hidden developer cards keep their
place in the saved order. Clearing browser storage removes saved layouts.

The script is streamed through the existing response buffer with no device-side
layout state. Automated tests cover keyboard and pointer ordering, cancellation,
hidden cards, saved layouts, reset, live fragments and blocked browser storage.
Visual testing on desktop and touch devices remains part of deployment acceptance.

### History Test Tools and Help

Developer History Test Tools fills **50, 75, 95 or 99%** of current capacity:

- **Network Inventory:** distinct synthetic `TEST-PREFILL-*` AP summaries.
- **Signal History / Balanced Survey:** compact synthetic measurements.
- **Bluetooth:** its separate measurement history, when BLE is enabled.

Targets are rounded down; AP-table growth can change measurement capacity.
Already-satisfied targets do nothing. Filling adds data to real survey storage
and may age out older records; Clear History removes both real and synthetic
records. Plots and terminal capture can be disabled. Synthetic batches may repeat
identities to fill large buffers with limited scan metadata. See
[storage and prefill validation](docs/TEST_STORAGE_V45.md).

Buttons, navigation links and dropdowns share a 16px font and 38px minimum height.
Contextual Help covers Survey Focus, power modes, memory controls, terminal,
survey button, checkpoints and diagnostics. The Help page avoids repeating its
main explanation; Developer view adds text only when additional detail exists.

## Survey Focus

Under **Settings → Survey Focus**, choose:

| Preset | Retention | When AP space is full |
| --- | --- | --- |
| Signal History | More measurements over time; AP summaries may use up to 25% of the pool after timestamp metadata | Reuse unreferenced APs; otherwise skip new identities to protect history |
| Balanced Survey (default) | Both AP coverage and a rolling timeline; AP summaries may use up to 50% | Replace the least recently seen AP and remove its measurements |
| Network Inventory | One updated summary per AP; nearly all the pool is available for APs | Replace the least recently seen AP |

Inventory summaries include first/last seen, sightings, and latest/min/max/average
RSSI since that AP was admitted. Its CSV contains one row per AP, and RSSI
timeline plots are unavailable. The other two presets export retained
measurements and calculate displayed network statistics from that retained window.
AP identity is BSSID in every preset. No preset provides unlimited storage.

**Save & Restart (clear history)** persists the choice and restarts with fresh
history. Download CSV first: changing focus clears retained survey data and
the restart checkpoint. Saving the current choice does nothing. The focus
applies to Wi-Fi with or without BLE enabled; BLE keeps its own memory budget.
JSON configuration backup/import includes `surveyFocus` (0 = History,
1 = Balanced, 2 = Inventory); importing a different focus requires a restart.
V45 uses checkpoint format 4 and rejects older checkpoints or checkpoints
from a different focus.

## Wi-Fi power modes

| Mode | Command | Behavior |
| --- | --- | --- |
| Normal (default) | `power normal` | Continuous Wi-Fi; modem sleep disabled |
| Connected saving | `power connected` | Modem sleep while retaining web connectivity |
| Ultra | `power ultra` | Wi-Fi off between scans, with an access window after each scan attempt |

For a 120-second scan interval and a 60-second radio access window:

```text
interval 120
wifi window 60
power ultra
```

These settings persist. Ultra stops both the Device AP and station interface; USB serial remains available. The window starts after a scan attempt, and association/DHCP consume part of it. Active work can defer shutdown. If the scan interval is no longer than the access window, there is no scheduled off period. This is Wi-Fi radio power saving, not whole-device deep sleep.

Use USB `wifi on` to wake Wi-Fi and renew the window, or `power normal` for continuous access. Non-terminal web activity (including page updates, navigation, and button clicks) restarts a five-minute Wi-Fi hold in Ultra; the configured access-window timeout follows that hold. Terminal viewing and polling do not renew it. The status LED breathes during a reachable Ultra access window; diagnostic indications take priority. See [power behavior and validation](docs/TEST_WIFI_POWER.md).

While the Wi-Fi radio is off, the status LED flashes at 25% brightness once per
remaining minute until the next scheduled scan, rounded up. A burst starts when
the radio turns off and repeats every 10 seconds: for example, 61–120 seconds
remaining gives two flashes, and 1–60 seconds gives one. Each flash lasts 50 ms
with a 100 ms gap. The count follows the actual scan deadline (including retry
timing), rather than restarting the interval when the radio turns off. Waking the
radio stops the countdown indication. Diagnostic events take priority, and the
Status LED setting disables it. If PWM initialization fails, flashes use full
brightness as a fallback.

## USB and web terminal

USB serial runs at **115200 baud**. In VS Code, run **Terminal > Run Task > ESP32: Serial Debug Monitor** and select the USB port (COM4 on the development setup). Stop the monitor before uploading. See [serial monitor guide](docs/SERIAL_DEBUG.md).

The web terminal at `/terminal` captures recent firmware output and accepts the same commands as USB. It provides pause/resume, filtering, auto-scroll, clear view, download, and hidden command input. The device retains the selected capture size (Off, 1, 2, 8, 16 or 32 KiB; default 1 KiB) and the browser retains up to 64 KiB; ROM/library UART output is not captured. Pausing or clearing the view does not stop surveying.

Commands are queued once; failed delivery is not automatically retried. The terminal reports overwritten output and restarts. On a fetch failure, a last-reported Ultra mode produces a likely radio-sleep explanation and recovery instructions. Commands cannot reach a sleeping Wi-Fi radio; use USB or wait for the next access window.

### Diagnostic detail

Lag capture retains main-loop gaps and wrapped HTTP handlers of 500 ms or
longer even with streaming off. After a stall, use **Capture Diagnostics** or
download `/status.json` and inspect `webStallTrace`. The bounded RAM history
includes timing, heap and radio/scan context. See [lag capture](docs/SERIAL_DEBUG.md#capturing-lag).

In the web terminal, use the **Diagnostic detail** selector to choose **Terse**
or **Verbose**. It saves the device setting and updates after device confirmation,
including changes made over USB. Any draft command is preserved. The equivalent
commands are:

```text
diag on
diag terse
diag interval 10
diag snapshot
diag verbose
diag summary
```

`diag terse` is the default: compact status/heap fields and web response summaries without per-phase web chatter. `diag verbose` restores full fields and web start/phase/work timings. The choice persists across restarts and applies to both terminals; changing detail does not enable streaming. `diag summary` remains a full report.

Example terse status:

```text
[45572] STATUS radioLeft=34s wifi=connected ble=idle obs=14/0 heap=59096
```

`obs` is Wi-Fi/BLE observation counts; heap is in bytes. In Ultra, `radioLeft` shows the remaining access-window seconds, rounded up. It can also show `off` (asleep), `held` (budget expired but still awake), `pending` (window not armed), or `continuous` (scan interval leaves no off period).

### Numeric command bounds

| Command | Allowed seconds |
| --- | --- |
| `interval <seconds>` | 5..3600 |
| `bleinterval <seconds>` | 5..3600; BLE must be enabled |
| `wifi window <seconds>` | 5..3600 |
| `diag interval <seconds>` | 0..3600; 0 disables periodic snapshots |

USB and web terminal commands clamp out-of-range whole numbers to the nearest bound and report it:

```text
wifi window 1
Adjusted to 5s (allowed: 5..3600s).
Wi-Fi access window saved: 5 seconds.
```

Malformed numbers leave settings unchanged. This behavior is specific to these terminal commands; configuration imports and other forms retain their own validation.

Use `help`, `settings`, or `developer` to discover other commands, including `scan`, `wifi-config`, and category switches such as `diag web off`. Surveying continues without an attached terminal.

## RSSI

RSSI is reported in dBm. Values closer to zero indicate a stronger received signal.

| RSSI | General interpretation |
| ---: | --- |
| -30 dBm | Extremely strong |
| -50 dBm | Strong |
| -60 dBm | Good |
| -70 dBm | Usable but weaker |
| -80 dBm | Weak |
| -90 dBm | Very weak / near the usable limit |

These are general guidelines. Real performance also depends on interference, channel utilization, antenna orientation, multipath, receiver implementation, and other RF conditions.

## Hardware

Development and validation have primarily used an ESP32 DEVKITV1-class board with:

- ESP32-D0WD-V3
- dual-core classic ESP32
- 240 MHz CPU
- 4 MB flash
- CP210x USB-to-UART bridge

Other classic ESP32 boards may work but have not necessarily been validated.

> The tested classic ESP32 supports 2.4 GHz Wi-Fi only. This is not a 5 GHz Wi-Fi survey tool.

## Build and installation

**PlatformIO is the primary build environment.** [platformio.ini](platformio.ini) pins the ESP32 Dev Module target, pioarduino 55.03.311 / Arduino core 3.3.11, Huge APP partition layout, and **NimBLE-Arduino 2.5.0**.

1. Open the repository root in VS Code with PlatformIO installed.
2. Edit **[src/main.cpp](src/main.cpp)**, the authoritative firmware source.
3. Build and upload with PlatformIO, or run from the repository root:

   ```text
   pio run
   pio run -t upload --upload-port COM4
   pio device monitor -e esp32dev --port COM4
   ```

4. Substitute your device's port for COM4. The configured monitor uses 115200 baud.

Build and Upload regenerate `arduino/WifiConnect/WifiConnect.ino` automatically. For Arduino IDE, open that generated sketch, install ESP32 core 3.3.11 and NimBLE-Arduino 2.5.0, and select ESP32 Dev Module, 4 MB flash, Huge APP, and 240 MHz. Do not edit the generated sketch or use the historical `WifiConnect/WifiConnect.ino` for current development.

Manual regeneration: `python tools/export_arduino.py`. Commit the generated sketch with source changes. See [PlatformIO setup](docs/BUILD_PLATFORMIO.md) and [Arduino compatibility](docs/BUILD_ARDUINO.md).

## First boot and web access

On startup, the ESP32 loads persistent configuration, initializes Wi-Fi, starts the Device AP, attempts saved infrastructure Wi-Fi when configured, initializes BLE only when enabled, allocates survey history, initializes session storage/restore, starts the web server and mDNS, and begins headless surveying.

Typical access paths are:

```text
http://surveyor.local/
http://192.168.4.1/
http://<infrastructure DHCP address>/
```

Failure to join infrastructure Wi-Fi does not stop surveying.

## Basic survey workflows

### Portable AP-only survey

1. Power the ESP32 from USB or a power bank.
2. Connect a phone or laptop to the Device AP.
3. Browse to the Device Hostname or `192.168.4.1`.
4. Open **Wi-Fi**.
5. Adjust Scan Interval if needed.
6. Move/place the surveyor at the desired location and allow observations to accumulate.
7. Click a network to inspect its RSSI history.
8. Review Observed Channel Interference if useful.
9. Download CSV for later analysis.

The ESP32 does not need to join the network being surveyed.

### Infrastructure-connected survey

1. Configure **Infrastructure Wi-Fi** under Settings.
2. Access the surveyor through `.local`, its LAN address, or its Device AP.
3. Accumulate survey history.
4. Compare visible APs and the **Infrastructure Connected Network** section.
5. Export CSV if desired.

## Persistent settings

NVS/Preferences stores small device configuration such as:

- infrastructure Wi-Fi credentials
- Device AP settings
- Bluetooth survey enabled/disabled state
- status LED setting
- web Live Updates preference
- Device Hostname
- Wi-Fi and BLE scan intervals
- Wi-Fi power mode and access-window duration
- diagnostic streaming, categories, detail mode, and snapshot interval

A normal sketch upload does not normally erase NVS. A full flash erase can remove it.

Session checkpoints are stored separately in SPIFFS and are not a replacement for continuous survey logging.

## Upload troubleshooting

Some ESP32 development boards do not reliably enter the ROM serial bootloader automatically. A typical failure looks like:

```text
Connecting........
A fatal error occurred: Failed to connect to ESP32
```

On affected DEVKITV1 boards, holding **BOOT** while the upload tool is trying to connect and releasing it once communication starts is often effective. A direct USB connection can also be useful when troubleshooting.

## Flash and memory notes

Wi-Fi/BLE ESP32 firmware includes substantial framework infrastructure: FreeRTOS, radio drivers, TCP/IP, HTTP server, NVS, BLE, filesystem support, ESP-IDF components, and the C/C++ runtime. Arduino reports sketch usage relative to the selected application partition rather than the entire physical flash device.

Survey-history capacity is primarily a **dynamic RAM** question. Increasing shared table capacity can reduce the number of compact observations that fit in the same memory budget. Use the runtime allocation diagnostics to see the tradeoff for the selected survey mode.

## Security considerations

This project is intended primarily as a local engineering/diagnostic utility.

- Web access is plain HTTP, not HTTPS.
- There is no application-level web authentication.
- Anyone with access to the Device AP or reachable LAN interface can potentially access the survey UI.
- Stored infrastructure Wi-Fi passphrases are not displayed by the UI.
- Configuration backup intentionally excludes secrets where appropriate.
- The generated default Device AP password is predictable from device identity and should be changed if meaningful access control is required.
- BSSIDs and BLE addresses identify radio interfaces and can be privacy-sensitive when survey data is shared publicly.

Use the device only where these limitations are acceptable.

## Known limitations

- 2.4 GHz Wi-Fi only on the tested classic ESP32.
- Active survey history is RAM-backed; checkpointing is event-driven rather than continuous logging.
- Arbitrary power loss can therefore lose data accumulated since the last checkpoint.
- BLE substantially reduces available heap and Wi-Fi history capacity when initialized.
- Dual Wi-Fi/BLE operation can impair web responsiveness and remains under investigation.
- AP+STA operation and scanning share one physical Wi-Fi radio.
- Automatic scans can briefly affect AP, station, and HTTP responsiveness.
- RSSI is useful for relative comparison but is not a calibrated RF power measurement.
- mDNS `.local` resolution depends on client/network support; IP access is the fallback.
- HTTP-only, unauthenticated interface.
- Automatic bootloader entry can be intermittent on some DEVKITV1 boards.

## Repository layout and documentation

| Path | Contents |
| --- | --- |
| [src/main.cpp](src/main.cpp) | Current V45 firmware source |
| [platformio.ini](platformio.ini) | Pinned build, libraries, partition and monitor configuration |
| [arduino/WifiConnect/](arduino/WifiConnect/) | Generated Arduino compatibility sketch |
| [WifiConnect/](WifiConnect/) | Historical Arduino source; not the current build input |
| [Tools/](Tools/) | Sketch exporter, host tests and historical Arduino flashing helper |
| [.vscode/](.vscode/) | Extension recommendations and serial monitor task |
| [docs/](docs/) | Static demo plus build, serial and hardware validation guides |
| [spec/](spec/) | Engineering and interface specifications |
| [Enclosure/](Enclosure/) | Enclosure assets |
| `.pio/`, `logs/` | Ignored build products and local serial captures |

Additional guides: [demo](docs/README_DEMO.md), [serial operation](docs/SERIAL_DEBUG.md), [Wi-Fi power](docs/TEST_WIFI_POWER.md), [LED diagnostics](docs/TEST_LED_DIAGNOSTICS.md), and [infrastructure recovery](docs/TEST_V40A3.md).

### Validation

Build and upload run a lightweight Python preflight after regenerating the
Arduino sketch: generated-source consistency and contextual Help destinations.
It needs no ESP32, Node.js, or C++ host compiler. Run it separately with:

```text
python Tools/validate.py
```

Before deployment, run all host regressions (every `Tools/test_*.py` and
`Tools/test_*.mjs`), then build and upload:

```text
python Tools/validate.py --host
pio run
pio run -t upload --upload-port COM4
```

The full suite needs Node.js and `g++`; Python 3.9+ is required for the runner.
On Windows, if `g++` is not on PATH, it invokes the Python tests in WSL, which
must have `python3` and `g++`. Node tests still run with Windows Node.js.
A failing test or missing prerequisite exits nonzero. The full suite is
explicit rather than part of every incremental build.

| Coverage | Tests |
| --- | --- |
| Shared storage, all Survey Focus modes, prefill and checkpoint restore | `test_shared_storage.py`, `test_shared_checkpoint.py`, `test_ble_prefill.py` |
| Wi-Fi power hold, radio transitions and survey button | `test_wifi_power.py`, `test_survey_button.py` |
| Serial capture, commands, terminal browser controls and reconnect policy | `test_v40a3.py`, `test_terminal_commands.py`, `test_terminal.mjs` |
| LEDs and runtime lag evidence | `test_led_diag.py`, `test_lag_capture.py` |
| Memory settings, config validation, old backups and Help links | `test_developer_memory.py` |
| Card movement, persistence, live fragments and Help deduplication | `test_card_layout.mjs` |
| Validation failures and mocked deployment responses | `test_validation.py` |

After uploading and allowing startup scans to complete, check the device:

```text
python Tools/validate.py --device http://192.168.4.1 --expect-version 45
```

Use its actual AP or LAN address. This performs GET-only checks of status,
configuration, feature pages and terminal headers. It verifies storage bounds,
zero Wi-Fi integrity anomalies, supported memory settings and expected firmware
version. It does not flash, restart, prefill, submit commands or initiate scans.
These web requests **renew the Ultra five-minute activity hold**. Wake Wi-Fi first
if needed. No survey records or credentials are printed or saved.

Warnings (including restart-pending settings) are reported separately; add
`--strict` to make them fail deployment acceptance. Build/version checks alone
cannot prove that a particular source revision is installed when builds share
V45; compare the displayed build timestamp too. See the
[deployment checklist](docs/VALIDATION.md) for RF, touch/browser, power and
checkpoint checks that host tests cannot establish.

## Longer-term roadmap

Potential future work includes:

- BLE memory/history normalization and improved dual-radio responsiveness.
- Better AP-table recycling/utilization diagnostics.
- More resilient persistent survey logging beyond controlled-reboot checkpointing.
- GPS/GNSS for location and UTC timestamps.
- Small local display with D-pad/button navigation.
- External voltage/current/power monitoring.
- Boards with more flash and/or PSRAM if the project outgrows the classic 4 MB ESP32.
- Optional authentication/provisioning improvements.
- SD card file operations (write now, delete file, format)
- Auto-select the latest logged file.
- File format option (CSV/JSON)
- Log diagnostic files to SD
- GPS location logging

## License

This project is licensed under the MIT License. See `LICENSE` for details.
