# ESP32 WiFi Surveyor

An ESP32 Wi-Fi configuration, monitoring, and site-survey tool with a
serial interface, web UI, BSSID/RSSI/channel logging, automatic scans,
RSSI history plotting, and CSV export.

The project began as a simple way to configure Wi-Fi credentials through
the Arduino Serial Monitor and evolved into a lightweight Wi-Fi survey
and logging utility.

## Features

### Wi-Fi configuration

-   Scans for nearby Wi-Fi networks.
-   Displays available networks in a fixed-width serial table.
-   Allows network selection through the Serial Monitor.
-   Prompts for a Wi-Fi passphrase when required.
-   Retries configuration after an unsuccessful connection attempt.
-   Stores successful credentials in ESP32 non-volatile storage (NVS)
    using `Preferences`.
-   Automatically reconnects after reset or power cycle.
-   Provides confirmation before erasing saved credentials.

### Serial interface

The interactive serial menu provides Wi-Fi status, SSID, IP address, MAC
address, hostname, gateway, subnet, RSSI, uptime, Wi-Fi scans,
configuration, credential erasure, and software restart.

``` text
================================
 ESP32 Control Menu
================================
Wi-Fi: Connected to MyNetwork
IP:    192.168.1.135

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

When connected, the ESP32 starts a local HTTP server. The web interface
provides:

-   Network and device status
-   Manual Wi-Fi scans
-   Automatic periodic scans
-   Configurable scan interval
-   Connected SSID and BSSID
-   Current RSSI
-   Complete retained scan history, newest first
-   RSSI history plot for the currently connected access point
-   CSV download
-   Scan-history clearing

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
                                      AP currently serving the ESP32

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
using its MAC address. This is useful for multi-access-point and mesh
networks.

``` text
SSID       BSSID                CH   RSSI
MyNetwork  A4:CF:12:34:56:78     1   -42 dBm
MyNetwork  A4:CF:12:AB:CD:EF     6   -67 dBm
MyNetwork  80:12:34:56:78:90    11   -81 dBm
```

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

The web interface plots RSSI history for the currently connected
**BSSID** on a fixed scale so measurements at different locations are
visually comparable.

## Scan logging

Scan history is stored in **RAM only**. This avoids repeated flash
writes and deliberately starts a new survey session after reset or power
cycle.

The current implementation uses a fixed-size ring buffer of **300
network observations**. One scan can create many observations; for
example, a scan finding 10 networks consumes 10 records. When full, the
oldest records are overwritten.

Wi-Fi credentials are different: they are stored persistently in NVS.

## Automatic scanning

Automatic scanning can be enabled from the web survey page. The interval
is specified in seconds.

``` text
Minimum: 5 seconds
Maximum: 3600 seconds
```

Frequent scans consume radio time and can temporarily affect normal
Wi-Fi communication. A 10-15 second interval is a reasonable starting
point for walking around a building and observing signal changes.

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
3.  Open `ESP32-WiFi-Surveyor.ino`.
4.  Select **ESP32 Dev Module**.
5.  Select the ESP32 COM port.
6.  Compile and upload.
7.  Open Serial Monitor at **115200 baud**.

No additional third-party Arduino libraries are currently required.

## First-time Wi-Fi setup

If no credentials are stored, open Serial Monitor and select:

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

## Persistent credentials and firmware updates

A normal sketch upload does **not** normally erase stored Wi-Fi
credentials because application firmware and NVS occupy separate flash
regions.

A full flash erase can remove NVS data. Credentials can also be
deliberately removed using serial menu option `4`.

## Accessing the web interface

After connecting, Serial Monitor reports the assigned address:

``` text
Connected successfully.
SSID: MyNetwork
IP address: 192.168.1.135
Signal strength: -48 dBm

Web server started.
Open: http://192.168.1.135/
```

Enter the reported IP address in a browser on the same local network.
Use **Scan Wi-Fi** to open the survey interface.

## Basic site-survey workflow

1.  Connect the ESP32 to the network being evaluated.
2.  Open the Wi-Fi Survey page.
3.  Enable automatic scanning.
4.  Choose an interval such as 10 or 15 seconds.
5.  Move or place the ESP32 at the desired test location.
6.  Allow several scans to accumulate.
7.  Review the RSSI plot and scan history.
8.  Download the CSV for additional analysis.
9.  Repeat at other locations as needed.

Because BSSIDs are retained separately, the data can also help identify
which individual access point is visible or serving the ESP32 in
different areas.

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

## Security considerations

This project is intended primarily as a local development and diagnostic
utility.

Current limitations:

-   The web interface uses plain HTTP, not HTTPS.
-   The web interface has no authentication.
-   Wi-Fi credentials are configured through the local serial interface.
-   The web interface does **not** display the stored Wi-Fi passphrase.

Use the device only on networks where these limitations are acceptable.

## Known limitations

-   2.4 GHz Wi-Fi only on the tested classic ESP32.
-   Scan history is intentionally lost on reset/power cycle.
-   Maximum of 300 retained network observations.
-   Automatic scans consume radio time and can affect HTTP
    responsiveness.
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

Git history preserves these iterations while the sketch keeps a single
stable filename.

## Possible future improvements

-   Plot multiple BSSIDs or SSIDs simultaneously.
-   Filter scan history by SSID/BSSID.
-   Add channel-utilization visualization.
-   Add minimum/maximum/average RSSI statistics.
-   Add manually entered location/survey-point labels.
-   Persist selected survey sessions to LittleFS, SPIFFS, or SD card.
-   Add mDNS hostname access.
-   Improve mobile layout.
-   Add OTA firmware updates.

## License

This project is licensed under the MIT License. See the LICENSE file
for details.
