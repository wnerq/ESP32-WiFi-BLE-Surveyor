# V40a6 Wi-Fi power modes and access window

Settings > Wi-Fi Power Saving persists one of three modes without a reboot:

| Mode / backup value | USB serial command | Behavior |
| --- | --- | --- |
| Normal / 0 (default) | `power normal` | Continuous Wi-Fi, modem sleep disabled. |
| Connected saving / 1 | `power connected` | Minimum modem sleep; retains AP/STA and web access. |
| Ultra / 2 | `power ultra` | Stop Wi-Fi between survey scans; a configurable awake window follows each scan attempt (default 60 seconds). |

The existing scan interval is measured from the end of the previous successful
scan. Scan intervals no longer than the access window leave no off period. Scan failure retry
backoff is unchanged, and failed/timed-out attempts also open a window. Initial
scans and automatic Wi-Fi scans in Ultra ignore the interaction defer timer;
ordinary browser polling cannot hold the radio on. Active scan, BLE scan, CSV
export and a synchronous HTTP operation may postpone shutdown. A manual USB
`scan` wakes Wi-Fi and opens another window. `power normal` restores continuous
access even while Wi-Fi is off. `wifi on` wakes Wi-Fi and renews the current
window without changing the saved power mode or starting an extra scan.
`wifi window 120` saves a 120-second window and renews its timer. The accepted
range is 5..3600 seconds; the same field is available in Settings > Wi-Fi Power
Saving. Both commands also work through the web terminal while it is reachable.

Ultra stops both AP and STA, but retains credentials, survey history and the
terminal ring in RAM. It does not reboot or write NVS each cycle. The AP returns
when Wi-Fi starts; an infrastructure connection is requested after the scan to
avoid scan/connect contention. Association and DHCP consume part of the
window; it is not a guarantee of that many seconds of browser use.
When the AP is disabled and joining the saved network fails, USB remains the
recovery interface. An already-blocking HTTP send can extend the window, as
seen in the earlier field capture. Reducing those stalls is a separate task.

BLE remains separately configured and can still use RF. CPU, USB bridge and
board LEDs remain powered. This is not whole-device deep sleep, and measured
battery savings are not yet available. Connected modem sleep depends on
association, router beacon timing and traffic; an enabled Device AP limits
savings. See Espressif's [Wi-Fi power-saving documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32/api-guides/wifi.html).

`/config.json` includes `wifiPowerMode` and `wifiAccessWindowSeconds`; imports accept integers 0..2 and 5..3600 respectively, reject
duplicates/invalid values, and retain the current mode when an older backup
omits the field. `/status.json` exposes the requested/applied mode (255 means
not applied yet), radio state, window time remaining and last driver error under
`network`. Serial STATUS includes `powerMode` and `radio`. Scheduled power
cycles do not increase infrastructure reconnect/failure counters or request the
disconnected LED heartbeat. Genuine scan failures still use scan recovery.
During Ultra's access window, when AP or infrastructure access is available,
the status LED fades from off to full brightness over three seconds and back
to off over three seconds, repeating. Existing diagnostic patterns take
priority; LED disable still works. Hardware LEDC provides 5 kHz, 8-bit PWM;
the main loop updates brightness without delays. Blocking work can stretch
or skip parts of the breathing envelope, just as it can delay shutdown.

## Automated checks

```
wsl --exec python3 tools/test_wifi_power.py
wsl --exec python3 tools/test_v40a3.py
wsl --exec python3 tools/test_led_diag.py
node Tools/test_terminal.mjs
wsl --exec python3 tools/test_terminal_commands.py
```

The power tests compile the actual policy/services with simulated driver,
HTTP, NVS and time. They cover both continuous modes, repeated cycles, the
60-second boundary, wraparound, scan/export/restart gates, serial-equivalent
mode changes, and start/stop/modem-sleep failures. They cannot validate RF,
physical power consumption, Arduino event timing or browser reconnection.

## Hardware acceptance (not yet run)

1. Record USB serial throughout. Select each mode in Settings, reboot and verify
   persistence, then export/import the configuration. Invalid mode values and
   duplicate keys must return 400 without changing the saved configuration.
2. With AP enabled, use Ultra and a 120-second interval. Confirm scan results
   keep advancing, the AP disappears after the window and returns on the next
   scan, and browsing/terminal polling does not keep Wi-Fi on indefinitely.
3. Repeat with AP disabled and a saved network. Verify post-scan association
   and DHCP. Test unavailable credentials and restore access over USB using
   `power normal`. Verify mDNS and HTTP return over several hundred cycles.
4. During an off period issue `scan`, `power connected`, and `power normal` in
   separate trials. Confirm wake without reboot, appropriate modem policy,
   retained history and no artificial reconnect failures/heartbeat.
5. Test intervals 5, 59, 60, 61, 120 and 300 seconds, with BLE on/off, CSV export,
   a slow HTTP client and failed scans. Check memory stability and real failures
   remain distinguishable from the intentional `RADIO_SLEEP` state.
6. Measure average supply current over complete cycles for all modes at the
   same interval, BLE setting and traffic load. Include reconnect costs and
   compare Device AP on/off before quoting power or battery-life savings.
7. Test `wifi on` while asleep, and `wifi window 5`, `60`, `120`, `3600` over
   USB and the web terminal. Verify persistence across reboot and config import,
   invalid input rejection, timer renewal, and scan-interval overlap behavior.
8. Observe PWM breathing with the window active, during a higher-priority LED
   event, when the window ends, and with LED disabled. Verify a three-second
   rise and three-second fall on the actual board (scope or logic analyzer).
