# ESP32 WiFi Surveyor

An ESP32 Wi-Fi configuration, monitoring, and site-survey tool with a
serial interface, self-hosted web UI, AP+STA operation,
BSSID/RSSI/channel logging, automatic scans, selectable RSSI history
plotting, and CSV export.

The project began as a simple way to configure Wi-Fi credentials through
the Arduino Serial Monitor and has evolved into a portable Wi-Fi survey
and logging utility. The ESP32 can now host its own access point, so the
survey interface can be used even when the device is not connected to an
existing Wi-Fi network.

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

The interactive serial menu provides station Wi-Fi status, SSID, IP
address, MAC address, hostname, gateway, subnet, RSSI, uptime, AP
status, Wi-Fi scans, configuration, credential erasure, and software
restart.

``` text
================================
 ESP32 Control Menu
================================
Wi-Fi: Connected to MyNetwork
IP:    192.168.1.135
AP:    ESP32-Surveyor-670F2C @ 192.168.4.1

1 - Wi-Fi status
2 - Scan Wi-Fi networks
3 - Configure Wi-Fi
4 - Clear saved Wi-Fi credentials
5 - Restart ESP32

mac - Show Wi-Fi MAC address
h   - Show this menu

>
```

### Web interface

The ESP32 starts a local HTTP server whenever either the station
connection or its own access point is available.

The web interface provides:

-   Station network and device status.
-   Access-point status, SSID, IP address, and client count.
-   AP configuration page.
-   Manual Wi-Fi scans.
-   Automatic periodic scans.
-   Configurable scan interval.
-   Configurable scan-history capacity.
-   Connected SSID and BSSID.
-   Current RSSI.
-   Complete retained scan history, newest scan first.
-   A single RSSI history plot that can display any retained BSSID.
-   Clickable SSID/BSSID entries to redraw the RSSI plot.
-   CSV download.
-   Scan-history clearing.
-   Free-heap and scan-history RAM information.

Navigation controls are kept at the top of the survey page so normal
actions do not require scrolling through accumulated scan history.

## Wi-Fi survey data

Each detected network can include:

  -----------------------------------------------------------------------
  Field                               Description
  ----------------------------------- -----------------------------------
  SSID                                Human-readable Wi-Fi network name

  BSSID                               MAC address identifying the
                                      individual access point/radio

  Channel                             2.4 GHz Wi-Fi channel

  RSSI                                Received signal strength in dBm

  Security                            Reported Wi-Fi
                                      authentication/security mode

  Connected                           Whether the record represents the
                                      infrastructure AP currently serving
                                      the ESP32

  Hidden                              Whether the network has a
                                      hidden/empty SSID

  Scan                                Scan sequence number

  Uptime                              ESP32 uptime when the scan was
                                      performed
  -----------------------------------------------------------------------

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

Scan history is stored in **RAM only**. This avoids repeated flash
writes and deliberately starts a new survey session after reset or power
cycle.

The history uses a dynamically allocated ring buffer. The retention
limit is configurable from the survey page:

``` text
Minimum: 50 network observations
Default: 300 network observations
Maximum: 2000 network observations
```

A network observation is one network detected during one scan. A scan
finding 10 networks therefore consumes 10 records.

When the configured buffer is full, the oldest records are overwritten.
Changing the capacity at runtime preserves the newest records when
possible.

The survey page reports:

-   Stored records / configured capacity.
-   Number of retained scan groups.
-   RAM allocated to scan history.
-   Current free heap.

The configured history capacity is currently session-only and returns to
the 300-record default after reset.

Wi-Fi station credentials and AP configuration are different: those are
stored persistently in NVS.

## Automatic scanning

Automatic scanning is **enabled by default** after boot.

``` text
Default interval: 300 seconds
Minimum interval: 5 seconds
Maximum interval: 3600 seconds
```

The interval and automatic-scanning state can be changed from the web
survey page. The current implementation treats these as runtime
settings; after reboot the firmware defaults to automatic scanning
enabled at 300 seconds.

For an active walk-around survey, temporarily selecting a shorter
interval such as 10-15 seconds can make changes in signal level easier
to observe.

Frequent scans consume radio time and can temporarily affect normal
Wi-Fi and HTTP communication.

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

Tested board selection:

``` text
ESP32 Dev Module
```

Typical tested settings:

``` text
Flash Size:        4MB (32Mb)
Partition Scheme:  Default 4MB with SPIFFS
CPU Frequency:     240 MHz
Upload Speed:      115200
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

On startup, the ESP32 initializes its survey access point. Serial
Monitor reports the AP information, for example:

``` text
Access point started.
AP SSID:     ESP32-Surveyor-670F2C
AP Password: survey-670F2C
AP IP:       192.168.4.1
```

You can then:

1.  Connect a laptop or phone to the ESP32's AP.
2.  Open `http://192.168.4.1/`.
3.  Use the survey interface without configuring infrastructure Wi-Fi.

If saved station credentials exist, the ESP32 also attempts to connect
to that network. When successful, the same web interface is available
from the station-side DHCP address as well.

For example:

``` text
LAN web UI: http://192.168.1.135/
AP web UI:  http://192.168.4.1/
```

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

Open **AP Settings** from the status page.

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

A normal sketch upload does **not** normally erase NVS because
application firmware and NVS occupy separate flash regions.

Currently persistent across ordinary reset/power cycle:

-   Infrastructure Wi-Fi credentials.
-   AP enabled/disabled state.
-   AP SSID.
-   AP password.

Currently session-only:

-   Scan observations/history.
-   History-capacity selection.
-   Scan interval changes.
-   Automatic-scanning changes.

A full flash erase can remove NVS data.

Station credentials can be deliberately removed using serial menu option
`4`. Doing so leaves the survey AP running.

## Basic site-survey workflows

### Portable AP-only survey

1.  Power the ESP32.
2.  Connect a laptop or phone to the ESP32 survey AP.
3.  Browse to `192.168.4.1`.
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
2.  Access the web UI from either its LAN address or its own AP.
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
The web survey page therefore reports the RAM allocated to the history
buffer and the remaining free heap.

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

-   2.4 GHz Wi-Fi only on the tested classic ESP32.
-   Scan history is intentionally lost on reset/power cycle.
-   History capacity is limited to 50-2000 retained network
    observations.
-   History-capacity, scan-interval, and automatic-scan changes
    currently return to firmware defaults after reboot.
-   AP+STA operation and scanning share one physical Wi-Fi radio.
-   Automatic scans can affect AP, station, and HTTP responsiveness.
-   RSSI is useful for relative comparison but is not a calibrated RF
    power measurement.
-   HTTP-only, unauthenticated web interface.
-   Automatic bootloader entry can be intermittent on some DEVKITV1
    boards.

## Project evolution

The project was developed incrementally:

1.  Serial Wi-Fi configuration and persistent credential storage.
2.  Interactive serial control menu and web status page.
3.  Expanded network status and improved Wi-Fi state handling.
4.  Tabular serial scans and browser-based Wi-Fi scanning.
5.  Session logging, detailed security information, BSSID/channel
    capture, automatic scans, and CSV export.
6.  RSSI history plotting and complete newest-first scan-history
    display.
7.  Dynamically configurable scan-history capacity with heap/RAM
    reporting.
8.  Automatic scanning enabled by default at a 300-second interval.
9.  Survey-page navigation controls moved to the top of the page.
10. Configurable AP+STA mode, allowing the ESP32 to host its own survey
    network.
11. Selectable per-BSSID RSSI plotting from the scan-history tables.

Git history can preserve these iterations while the sketch keeps a
single stable filename in the repository.

## Possible future improvements

-   Filter scan history by SSID/BSSID.
-   Add channel-utilization visualization.
-   Add minimum/maximum/average RSSI statistics.
-   Add manually entered location/survey-point labels.
-   Persist selected survey sessions to LittleFS, SPIFFS, or SD card.
-   Persist optional scan settings such as interval/history capacity.
-   Add mDNS hostname access.
-   Improve mobile layout.
-   Add OTA firmware updates.
-   Add authentication or a more secure provisioning model.

## License

This project is licensed under the MIT License. See the `LICENSE` file
for details.
