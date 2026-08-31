# ESP32 Wireless Surveyor

An ESP32 Wi-Fi and Bluetooth wireless survey/logger with self-hosted web and serial interfaces, headless automatic surveying, compact in-memory history, RSSI history plotting, 2.4 GHz channel-interference analysis, diagnostics/self-tests, CSV export, session checkpoint/restore, and mDNS hostname access.

## Live Web Interface Demo

A static browser demonstration of the current interface is published with GitHub Pages:

**[Open the Wi-Fi / BLE Surveyor Demo](https://wnerq.github.io/ESP32-WiFi-BLE-Surveyor/)**

The demo uses simulated data and does not require ESP32 hardware. It mirrors the V34b information architecture, including the sticky navigation/control card and Standard / Advanced / Developer views.

## What the project does

The project began as a serial Wi-Fi configuration utility and has grown into a portable wireless-survey instrument. The ESP32 can operate its own access point while simultaneously acting as a station, so the web interface remains usable even when infrastructure Wi-Fi is unavailable. Surveying is designed to continue headlessly without a browser or serial terminal connected.

The current web interface is organized around four subjects:

- **Wi-Fi** — conduct and inspect the Wi-Fi survey.
- **Bluetooth** — conduct and inspect the BLE survey.
- **System** — see what the surveyor is doing and whether it is healthy.
- **Settings** — change device and survey configuration.

A persistent **View** selector controls information depth:

- **Standard** — normal operation and useful survey results.
- **Advanced** — deeper operational information and troubleshooting detail.
- **Developer** — implementation internals, instrumentation, and test facilities.

The views are cumulative: Standard ⊂ Advanced ⊂ Developer. The navigation, Live Updates control, View selector, and Theme selector are held in a sticky card so they remain available while scrolling long pages.

## 3D Printed Enclosure
**[Honeycomb Case](https://www.printables.com/model/1826305-esp32-honeycomb-case-push-together-no-hardwaretool)*

## Key features

### Headless surveying

- Wi-Fi surveying starts automatically after boot.
- Surveying does not depend on a browser, serial terminal, or successful infrastructure Wi-Fi connection.
- An initial scan occurs shortly after startup.
- Configurable Wi-Fi scan interval: 5–3600 seconds.
- Scan interval is measured from completion of the previous successful scan.
- Manual **Scan Now** requests coexist with the automatic scheduler.
- Browser Live Updates repaint survey pages; they do not trigger scans.

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
5. Survey health / deeper diagnostics when Advanced or Developer is selected
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

The current compact `WifiObservation` is 6 bytes. Shared tables hold AP identity/current properties and scan metadata, while summary values such as count, min/max/average RSSI, First Seen, Last Seen, retained scan count, and retained time window are derived when needed.

The Wi-Fi AP table is currently configured for **512 entries in Wi-Fi-only mode**. An AP slot is recycled only when no retained observation still references it; reusing a referenced slot would corrupt the historical identity of compact observations.

On a validated V34-era Wi-Fi-only configuration, the larger AP table reduced the observation-ring capacity to roughly 3,900 observations while allowing substantially more simultaneously retained unique BSSIDs than the earlier 256-entry table. Capacity is a RAM tradeoff, not a fixed product guarantee.

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
- **Survey Mode**
- **Interface & Indicators**

Advanced adds items such as configuration backup/restore and selected troubleshooting details. Developer exposes implementation-oriented diagnostics export and internals.

## Serial interface

The serial interface remains text-oriented and parallels the four web subjects:

```text
================================
 ESP32 Wireless Surveyor
================================

1 - Wi-Fi Survey
2 - Bluetooth Survey
3 - System
4 - Settings

h - Help
>
```

Serial input is non-blocking with respect to the automatic survey scheduler. Surveying continues whether or not a serial terminal is connected.

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

## Development environment

Current tested environment:

```text
Arduino IDE:       1.8.19
Board:             ESP32 Dev Module
ESP32 Arduino:     3.3.11
Flash:             4MB (32Mb)
Partition Scheme:  Huge APP (3MB No OTA / 1MB SPIFFS)
CPU Frequency:     240 MHz
Serial:            115200 baud
```

Primary framework/library components include `WiFi.h`, `WebServer.h`, `Preferences.h`, SPIFFS/FS, `ESPmDNS.h`, ESP-IDF support APIs, and the ESP32 BLE library.

## Installation

1. Install Arduino IDE and Espressif ESP32 board support.
2. Open the current project `.ino` file.
3. Select **ESP32 Dev Module**.
4. Select **Huge APP (3MB No OTA / 1MB SPIFFS)**.
5. Select the ESP32 COM port.
6. Compile and upload.
7. Open Serial Monitor at **115200 baud** if desired.

No additional third-party Arduino libraries are currently required beyond those supplied by the selected ESP32 Arduino core.

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
- survey settings that are explicitly persisted by the firmware

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

Survey-history capacity is primarily a **dynamic RAM** question. Increasing shared table capacity can reduce the number of compact observations that fit in the same memory budget. V34 deliberately accepts that tradeoff for the larger Wi-Fi AP table.

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

## Current firmware state

The current completed UI/IA revision is **V34b**, `WifiConnect34b_sticky_navigation.ino`.

Major milestones leading to the current architecture include:

1. **V18 — survey-oriented web architecture**
   - Wi-Fi became the default survey workspace.
   - Wi-Fi / Bluetooth / System / Settings navigation replaced the older status-page model.
   - Headless survey operation and richer diagnostics were established.

2. **V20–V22 — memory-conscious surveying and interface parity**
   - BLE became optional at boot because of its heap cost.
   - Wi-Fi history moved to a compact normalized representation.
   - Serial navigation and controls were brought closer to web behavior.

3. **V23+ — network identity and survey diagnostics**
   - Configurable mDNS Device Hostname and richer system diagnostics were added.

4. **V31/V32d — full-history web performance**
   - Expensive history rendering and many tiny HTTP writes were identified as causes of severe page blocking.
   - Compact AP-index comparisons and buffered output restored responsiveness at full history.

5. **V33 — checkpoint/restore and test tooling**
   - Structured session checkpoint/restore with CRC validation.
   - Cross-reboot logical survey timebase.
   - Synthetic Wi-Fi history prefill for performance testing.
   - Scan-duration statistics corrected so failed scan starts do not contaminate successful scan timing.
   - Native infrastructure reconnect behavior clarified in diagnostics.

6. **V34/V34b — information architecture and sticky controls**
   - Web content reorganized around page subject and information depth.
   - Added cumulative **Standard / Advanced / Developer** views.
   - Survey results moved to survey pages; system health moved to System; ordinary configuration stays in Settings.
   - Terminology standardized around Surveying, Scan, Observation, History, Infrastructure Wi-Fi, Device AP, Broadcast SSID, and Device Hostname.
   - Added explanatory source comments for functions.
   - V34b makes the common navigation/control card sticky while scrolling.

## Repository documentation

- `/README.md` — project overview and usage.
- `/docs/` — static GitHub Pages demonstration of the web interface.
- `/spec/` — engineering/design specifications, including interface information architecture.

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

## License

This project is licensed under the MIT License. See `LICENSE` for details.
