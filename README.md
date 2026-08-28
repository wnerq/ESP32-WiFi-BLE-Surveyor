# ESP32 Wireless Surveyor

An ESP32 Wi-Fi and Bluetooth wireless-survey/logger with parallel serial and self-hosted web interfaces, AP+STA operation, headless automatic surveying, compact in-memory Wi-Fi history, RSSI history plotting, 2.4 GHz channel analysis, diagnostics/self-tests, configurable status LED behavior, CSV export, and mDNS hostname access.


## Live Web Interface Demo

Explore a static demonstration of the ESP32 Wireless Surveyor web interface:

**[Open the Wi-Fi / BLE Surveyor Demo](https://wnerq.github.io/ESP32-WiFi-Surveyor/)**

The demo uses simulated survey data and does not require ESP32 hardware.

The project began as a simple way to configure Wi-Fi credentials through the Arduino Serial Monitor and has evolved into a portable wireless survey and logging utility. The ESP32 can host its own access point, so the survey interface can be used even when the device is not connected to an existing Wi-Fi network. Wi-Fi and BLE survey data can be collected and reviewed from a phone or PC.


## Features

### Wi-Fi configuration

-   Scans for nearby 2.4 GHz Wi-Fi networks.
-   Displays available networks in a fixed-width serial table.
-   Allows network selection through the Serial Monitor.
-   Prompts for a Wi-Fi passphrase when required.
-   Retries configuration after an unsuccessful connection attempt.
-   Stores successful station credentials in ESP32 non-volatile storage
    (NVS) using `Preferences`.
-   Automatically reconnects to saved infrastructure Wi-Fi after reset
    or power cycle.
-   Provides confirmation before erasing saved station credentials.

### Access point mode

-   Runs as an ESP32 access point while retaining station capability
    (`AP+STA`).
-   AP mode is enabled by default.
-   Generates a unique default AP SSID from the ESP32 MAC suffix, for
    example:
    -   `ESP32-Surveyor-670F2C`
-   Generates a default AP password from the same suffix, for example:
    -   `survey-670F2C`
-   Normally serves the AP-side web interface at `192.168.4.1`.
-   Keeps the survey interface available even when no infrastructure
    Wi-Fi is configured or reachable.
-   Allows AP enable/disable, SSID, and password configuration from the
    web interface.
-   Stores AP configuration persistently in NVS.
-   Shows AP status, IP address, and connected client count in the web
    and serial interfaces.
-   Keeps the AP active when station credentials are erased.

Because the classic ESP32 has one 2.4 GHz radio, AP, station, and
scanning activity share the same radio. Scanning can therefore briefly
affect AP or HTTP responsiveness.

### Serial interface

The serial interface is organized around the same information architecture as
the web UI while remaining text-oriented and non-blocking:

``` text
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

The Wi-Fi and Bluetooth menus provide survey status and scan controls. The
System menu exposes firmware, radio, flash, heap, boot-checkpoint, history, and
self-test information. Settings provides configuration controls including
infrastructure Wi-Fi, AP settings, BLE enable/disable, status LED control,
web-page auto-refresh, and the mDNS hostname.

Serial input is processed without blocking the automatic survey scheduler, so
headless Wi-Fi/BLE scanning continues whether or not a serial terminal is
connected. The status LED can be enabled/disabled persistently and manually
self-tested from serial.

### Web interface

The ESP32 runs a local HTTP server and uses a common page shell with persistent
navigation:

- **Wi-Fi** — default `/` page and primary survey workspace.
- **Bluetooth** — BLE survey workspace.
- **System** — device status, diagnostics, resources, firmware information,
  boot heap checkpoints, and software self-tests.
- **Settings** — infrastructure Wi-Fi/AP configuration and device-wide options.

The Wi-Fi Survey page provides manual and automatic scans, compact RAM-backed
history, per-BSSID RSSI statistics and plotting, CSV export, relative **First
Seen** / **Last Seen** ages, and advisory 2.4 GHz channel analysis. Survey pages
can automatically refresh after a completed scan; this behavior is
persistently configurable and refresh is suppressed while a form control is
being edited.

The interface retains the lightweight **System / Light / Dark** browser theme
selector. The status LED can be enabled/disabled persistently and manually
self-tested from the web UI.

V23 adds configurable mDNS network identity. The default hostname is
`surveyor`, making the preferred friendly URL:

``` text
http://surveyor.local/
```

The configured hostname is stored in NVS and is shown in the System, Settings,
and serial interfaces. IP addresses remain available as a fallback because
mDNS behavior can vary by client/network.

## Bluetooth Low Energy survey

The firmware also includes a BLE survey/logger using the ESP32 Bluetooth radio. BLE observations are retained in a dynamically allocated history buffer, can be scanned manually or automatically, and can be reviewed in a sortable summary table. Individual devices can be selected for RSSI-history plotting, paralleling the Wi-Fi workflow.

BLE automatic scanning is intended to operate independently of browser interaction. The long-term design goal is that simply powering the device starts data collection; connecting to the web UI is for configuration, inspection, and export rather than for starting the survey.

## Wi-Fi survey data

V21 replaced the original flat 68-byte-per-observation Wi-Fi record with a
normalized in-memory representation. Repeated data is no longer copied into
every observation.

### Compact Wi-Fi history architecture

**Per observation** (6 bytes in the current implementation):

- Reference to the unique AP/BSSID entry.
- Reference to the scan metadata entry.
- RSSI stored at full reported integer-dBm resolution using a compact signed
  representation.

**Per unique AP/BSSID**:

- SSID.
- BSSID stored as a 6-byte binary MAC address and formatted as text only for
  display/export.
- Current channel.
- Current authentication/security mode.

Channel and security are treated as current AP properties rather than repeated
historical measurements. If an AP is later observed on a different channel,
the AP table is updated.

**Per scan**:

- Scan sequence number.
- Scan uptime/timestamp metadata.

The following values are derived rather than stored in every observation:

- Current connected-AP state.
- Hidden-network state from an empty SSID.
- Observation count.
- Minimum, maximum, and average RSSI.
- First Seen and Last Seen ages.
- Retained scan count and retained time window.

This preserves the useful survey information while greatly reducing RAM
duplication. On the validated V21/V22 architecture, the Wi-Fi observation
structure is 6 bytes versus 68 bytes in V20. A measured Wi-Fi-only boot
allocated approximately 6,253 physical observations in about 54.9 KB total
Wi-Fi history RAM, including the AP and scan-metadata tables.

### SSID vs. BSSID

An **SSID** identifies a Wi-Fi network by name. Multiple access points
can advertise the same SSID.

A **BSSID** identifies an individual access point or radio, typically
using its MAC address. This distinction is important for
multi-access-point and mesh networks.

``` text
SSID       BSSID                CH   RSSI
MyNetwork  A4:CF:12:34:56:78     1   -42 dBm
MyNetwork  A4:CF:12:AB:CD:EF     6   -67 dBm
MyNetwork  80:12:34:56:78:90    11   -81 dBm
```

RSSI history is keyed by **BSSID**, not SSID. This prevents measurements
from different radios advertising the same network name from being
combined into one plot.

## RSSI

RSSI is reported in dBm. Values closer to zero indicate a stronger
received signal.

       RSSI General interpretation
  --------- -----------------------------------
    -30 dBm Extremely strong
    -50 dBm Strong
    -60 dBm Good
    -70 dBm Usable but weaker
    -80 dBm Weak
    -90 dBm Very weak / near the usable limit

These are general guidelines. Actual performance also depends on
interference, channel utilization, antenna orientation, multipath
effects, and other RF conditions.

### Selectable RSSI history plot

The survey page contains one RSSI history plot on a fixed scale so
measurements remain visually comparable.

By default:

1.  If the ESP32 is connected to an infrastructure AP and that BSSID
    exists in retained history, that AP is plotted.
2.  Otherwise, the newest retained BSSID is plotted.

Clicking any **SSID** or **BSSID** in the scan-history tables selects
that individual BSSID and redraws the plot using all retained
observations for that radio. The selected SSID and BSSID are shown above
the graph.

Each plotted point represents one logged scan observation.

## Scan logging

Survey history is currently stored in **RAM only** and is intentionally lost
on reset or power cycle. Persistent flash logging is deferred to a future
revision.

Wi-Fi history uses three fixed allocations established during boot:

1. Compact observation ring.
2. Unique AP/BSSID table.
3. Scan-metadata table.

The physical Wi-Fi capacity is allocated once at boot based on available heap
and the firmware safety reserve. The user-facing **Retention Limit** is a
logical limit within that already allocated buffer; changing it does not
reallocate a large history buffer at runtime. This avoids the heap
fragmentation/allocation failures encountered with earlier revisions.

A Wi-Fi observation is one AP/BSSID detected during one scan. A scan finding
10 APs therefore adds 10 observations but only one scan-metadata entry.

The System page reports physical capacity, logical retention, observation
record size, AP-table usage/allocation, scan-metadata allocation, retained scan
count, retained time window, and heap health.

BLE still uses its older flat history representation and is intentionally
deferred for a later normalization pass.

## Automatic scanning

Automatic Wi-Fi surveying is **enabled by default** and is a core headless
operating requirement.

``` text
Default interval: 300 seconds
Minimum interval: 5 seconds
Maximum interval: 3600 seconds
```

After boot, an initial Wi-Fi scan is performed shortly after initialization and
periodic scanning continues without requiring a browser, serial terminal, or
successful infrastructure Wi-Fi connection.

Bluetooth surveying is **disabled by default** to maximize RAM available for
Wi-Fi history. The BLE setting is persistent. Enabling or disabling BLE saves
the setting and restarts the ESP32 so the radio stacks and history buffers can
be allocated safely at boot. When BLE is enabled, BLE automatic scanning also
operates headlessly.

For an active walk-around survey, a shorter Wi-Fi interval such as 10-30
seconds can make signal changes easier to observe. Frequent scans consume radio
time and can temporarily affect AP, station, and HTTP responsiveness.

## CSV export

The complete retained history can be downloaded as CSV.

There is one row for **each network observed during each scan**, so the
same SSID/BSSID normally appears repeatedly with different scan numbers,
timestamps, and RSSI values.

``` csv
scan,uptime_ms,uptime,ssid,bssid,channel,rssi_dbm,security,connected,hidden
1,15231,"0m 15s","MyNetwork","A4:CF:12:34:56:78",6,-44,"WPA2-PSK",YES,NO
2,25284,"0m 25s","MyNetwork","A4:CF:12:34:56:78",6,-48,"WPA2-PSK",YES,NO
3,35311,"0m 35s","MyNetwork","A4:CF:12:34:56:78",6,-53,"WPA2-PSK",YES,NO
```

The file can be analyzed later in Excel, Python, MATLAB, or other
data-analysis tools.

## Hardware

Development and testing have been performed on an ESP32 development
board marked:

``` text
ESP32 DEVKITV1
```

Observed hardware:

-   ESP32-D0WD-V3
-   Dual-core ESP32
-   240 MHz
-   4 MB flash
-   Silicon Labs CP210x USB-to-UART bridge

Other classic ESP32 boards may work but have not necessarily been
tested.

> **Note:** The tested classic ESP32 has a 2.4 GHz Wi-Fi radio. This is
> not a 5 GHz Wi-Fi survey tool.

## Development environment

The project has been developed with:

-   Arduino IDE 1.8.19
-   Arduino ESP32 core 3.3.11
-   `WiFi.h`
-   `WebServer.h`
-   `Preferences.h`
-   `ESPmDNS.h`
-   ESP32 BLE library

Tested board selection:

``` text
ESP32 Dev Module
```

Typical tested settings:

``` text
Flash Size:        4MB (32Mb)
Partition Scheme:  Huge APP (3MB No OTA / 1MB SPIFFS)
CPU Frequency:     240 MHz
Upload Speed:      project/tool dependent
```

Arduino menu wording may vary by ESP32 core or IDE version.

## Installation

1.  Install the Arduino IDE.
2.  Install Espressif ESP32 board support through Boards Manager.
3.  Open the project `.ino` file.
4.  Select **ESP32 Dev Module**.
5.  Select the ESP32 COM port.
6.  Compile and upload.
7.  Open Serial Monitor at **115200 baud**.

No additional third-party Arduino libraries are currently required.

## First boot and web access

On startup, the ESP32 loads persistent configuration, initializes Wi-Fi,
starts its survey AP when enabled, attempts the saved infrastructure STA
connection when configured, starts mDNS, allocates survey history, starts the
web server, and begins headless surveying.

The default mDNS hostname is:

``` text
surveyor
```

Preferred friendly URL:

``` text
http://surveyor.local/
```

The AP-side IP remains normally available at:

``` text
http://192.168.4.1/
```

When the ESP32 successfully joins infrastructure Wi-Fi, the web UI is also
available at its DHCP-assigned LAN address. The System and serial interfaces
show the active addresses and mDNS status.

Failure to connect to infrastructure Wi-Fi does **not** stop surveying. The
device is designed to operate unattended from a USB power bank and accumulate
survey data before a browser or serial terminal is connected.

## Configuring infrastructure Wi-Fi

Infrastructure Wi-Fi can still be configured through Serial Monitor
using:

``` text
3 - Configure Wi-Fi
```

The ESP32 scans and presents a numbered table:

``` text
#   SSID                              SIGNAL      CH   SECURITY
--  --------------------------------  ----------  ---  ----------------
1   MyNetwork                         -46 dBm     6    WPA2-PSK
2   AnotherNetwork                    -58 dBm     11   WPA2/WPA3-PSK
```

Enter the network number and passphrase when prompted.

After a successful connection, the credentials are stored in NVS.
Subsequent resets and power cycles automatically attempt to reconnect.

## Configuring the ESP32 access point

Open **Settings** and use the access-point configuration section.

The AP configuration page allows:

-   Enabling or disabling the AP.
-   Changing the AP SSID.
-   Changing the AP password.

The password must be 8-63 characters. Leaving the password field blank
keeps the existing password.

AP changes are saved to NVS and applied after an automatic restart.

> **Warning:** If you disable the AP while the ESP32 is not connected to
> infrastructure Wi-Fi, the web interface will no longer be reachable.
> Serial configuration remains available.

## Persistent settings and firmware updates

A normal sketch upload does **not** normally erase NVS because application
firmware and NVS occupy separate flash regions. A full flash erase can remove
NVS data.

Persistent device configuration includes:

- Infrastructure Wi-Fi credentials.
- AP enabled/disabled state, SSID, and password.
- Bluetooth survey enabled/disabled state.
- Status LED enabled/disabled state.
- Web survey auto-refresh enabled/disabled state.
- mDNS hostname.

The mDNS hostname defaults to `surveyor`. Hostnames are normalized/validated
before storage; changing the hostname causes a controlled restart so mDNS is
registered cleanly on the next boot.

Survey observations/history remain session-only RAM data. Some survey controls
such as active scan interval/retention behavior may be runtime-oriented; the
System/Settings interfaces report the active values.

## Basic site-survey workflows

### Portable AP-only survey

1.  Power the ESP32.
2.  Connect a laptop or phone to the ESP32 survey AP.
3.  Browse to `http://surveyor.local/` when mDNS is available, or use `192.168.4.1` as the AP-side fallback.
4.  Open the Wi-Fi Survey page.
5.  Leave automatic scanning enabled or select a shorter interval for an
    active survey.
6.  Move or place the ESP32 at the desired test location.
7.  Allow several scans to accumulate.
8.  Click any SSID/BSSID to inspect that specific radio's RSSI history.
9.  Download the CSV for additional analysis.

This mode does not require the ESP32 to join the network being surveyed.

### Infrastructure-connected survey

1.  Configure the ESP32 to join the desired Wi-Fi network.
2.  Access the web UI using `http://surveyor.local/` when available, its LAN address, or its own AP-side IP.
3.  Open the Wi-Fi Survey page.
4.  Accumulate scans.
5.  Compare the connected AP against other visible BSSIDs.
6.  Select individual BSSIDs to redraw the RSSI plot.
7.  Export the session as CSV if desired.

## Upload troubleshooting

Some ESP32 development boards may not reliably enter the ROM serial
bootloader automatically.

A typical failure looks like:

``` text
Connecting........
A fatal error occurred: Failed to connect to ESP32
```

On the tested DEVKITV1 boards, holding **BOOT** while Arduino displays
`Connecting...` and releasing it once upload begins has been effective.

Nominally identical boards may behave differently or require manual BOOT
intervention only intermittently. A direct USB connection may also be
preferable to a USB hub when troubleshooting upload problems.

## Flash and memory usage

Wi-Fi-enabled ESP32 sketches can be much larger than Blink because the
final firmware pulls in substantial supporting infrastructure:

-   ESP32 Arduino core
-   FreeRTOS
-   Wi-Fi driver
-   TCP/IP stack
-   HTTP server
-   NVS/Preferences support
-   ESP-IDF components
-   C/C++ runtime support

Once these are linked, adding application-level features often increases
program size only modestly.

Arduino also reports application usage relative to the selected
**application partition**, not necessarily the ESP32's entire physical
flash capacity.

Scan-history capacity affects **dynamic RAM**, not just program flash.
The System page reports the RAM allocated to survey history and the remaining
heap. The compact V21+ Wi-Fi architecture reduced each observation from the
V20 flat-record size of 68 bytes to 6 bytes, while moving repeated SSID/BSSID,
channel/security, and scan metadata into shared tables. A representative
Wi-Fi-only V21 run allocated 6,253 observations in approximately 54.9 KB total
Wi-Fi history RAM.

## Security considerations

This project is intended primarily as a local development and diagnostic
utility.

Current considerations:

-   The web interface uses plain HTTP, not HTTPS.
-   The web interface has no application-level authentication.
-   The ESP32 access point is password protected, but anyone with the AP
    password can reach the web interface.
-   AP configuration is available from the web interface.
-   Infrastructure Wi-Fi credentials are entered through the local
    serial interface.
-   The web interface does **not** display the stored infrastructure
    Wi-Fi passphrase.
-   The generated default AP password is predictable from the device MAC
    suffix and should be changed if meaningful access control is
    required.

Use the device only in environments where these limitations are
acceptable.

## Known limitations

- 2.4 GHz Wi-Fi only on the tested classic ESP32.
- Survey history is intentionally lost on reset/power cycle.
- BLE uses substantially more heap when initialized and therefore remains
  disabled by default.
- BLE history still uses the older flat-record representation and has not yet
  received the Wi-Fi normalization pass.
- AP+STA operation and scanning share one physical Wi-Fi radio.
- Automatic scans can affect AP, station, and HTTP responsiveness.
- RSSI is useful for relative comparison but is not a calibrated RF power
  measurement.
- The ESP32 cannot directly provide accurate total board supply current/power;
  external monitoring hardware is required for those measurements.
- HTTP-only, unauthenticated web interface.
- mDNS `.local` resolution depends on client/network support; IP access remains
  the fallback.
- Automatic bootloader entry can be intermittent on some DEVKITV1 boards.

## Current firmware state

The current working revision is **V23**, `WifiConnect23_mdns_hostname.ino`.

Major structural milestones since the older V17 README include:

1. **V18 — survey-oriented site architecture**
   - Wi-Fi Survey became the default `/` page.
   - Common Wi-Fi / Bluetooth / System / Settings navigation and shared page
     infrastructure replaced the old Back-to-Status model.
   - Headless boot scanning was explicitly implemented/verified.
   - Expanded System diagnostics/self-tests and advisory 2.4 GHz channel
     analysis were added.

2. **V19/V20 — runtime diagnostics and optional BLE**
   - Boot heap checkpoints quantified subsystem RAM costs.
   - BLE initialization was measured as the dominant heap consumer.
   - BLE became disabled by default and persistently selectable, with a reboot
     on mode change so history allocation occurs safely at boot.
   - Wi-Fi-only operation therefore receives substantially more history RAM.

3. **V21 — normalized compact Wi-Fi history**
   - Replaced the 68-byte flat Wi-Fi observation with a 6-byte compact
     observation plus shared AP and scan-metadata tables.
   - Physical history capacity is allocated once at boot.
   - User retention is a logical limit rather than a runtime reallocation.
   - Added relative First Seen / Last Seen presentation.
   - Added scan-completion page auto-refresh with edit/focus suppression.

4. **V22 — serial/web control parity**
   - Refactored serial navigation to mirror Wi-Fi Survey / Bluetooth Survey /
     System / Settings in text form.
   - Added persistent status-LED enable/disable and manual LED self-test from
     web and serial.
   - Added persistent web survey auto-refresh enable/disable.

5. **V23 — network identity**
   - Added configurable/persistent mDNS hostname.
   - Default friendly URL is `http://surveyor.local/`.
   - Added mDNS status to System diagnostics/self-tests and serial output.

The firmware is approximately 1.7 MB and requires the larger application
partition used by the project. Development currently favors the **Huge APP /
no OTA** layout; persistent survey logging is considered more valuable than
OTA if the 4 MB flash budget ultimately forces that trade-off.

## Next architecture work

The next major data-structure task is a field-by-field review and normalization
of **BLE history**, following the same approach that reduced Wi-Fi history RAM.
This work is intentionally deferred until the V23 Wi-Fi/web/serial architecture
is stable.

## Longer-term roadmap

### Persistent survey logging

Current scan history is volatile RAM. Persistent logging is now considered more valuable than browser-based OTA updates if the 4 MB flash budget forces a choice. A future revision should evaluate a LittleFS-style append log rather than using NVS/`Preferences` as a scan database.

The preferred concept is:

- Continue using NVS for small configuration values.
- Buffer active observations in RAM.
- Periodically append new observations to flash instead of rewriting the entire dataset.
- Use a configurable checkpoint interval (roughly 10-15 minutes is a reasonable starting point).
- On reboot, detect and offer the previous session for download/recovery.
- Keep CSV as the user-facing export format even if a different internal representation is eventually chosen.

This trades some flash write activity for resilience against a dead/disconnected power bank. Flash partitioning must be reviewed before implementation because application size, filesystem capacity, and OTA slots compete for the same 4 MB device flash.

### Hardware expansion

Longer-term possibilities include:

- Small local display.
- D-pad/button navigation, preferred over requiring a touchscreen.
- GPS/GNSS module for location, UTC time, and geotagged observations.
- External INA219/INA226-style monitor for actual supply voltage, board current, power, and energy measurements. The ESP32 alone cannot accurately report total board current/power.
- Boards with more flash and/or PSRAM if persistent logging, GPS, richer UI, or other features outgrow the current classic 4 MB ESP32.

### Other possible improvements

- Normalize/flatten BLE history to remove repeated per-observation data.
- Filter observations by SSID/BSSID/device.
- Manually entered location/survey-point labels before GPS support.
- Evaluate which additional scan settings should persist across reboot.
- Authentication or a more secure provisioning model.
- OTA firmware updates only if flash budget and usefulness justify it. Browser OTA would require uploading a **compiled firmware binary**, not a raw `.ino` source file.

## License

This project is licensed under the MIT License. See the `LICENSE` file
for details.
