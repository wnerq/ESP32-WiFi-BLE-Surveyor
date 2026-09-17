# Serial debugging in VS Code

1. Connect the ESP32 with a USB data cable.
2. Run **Terminal > Run Task > ESP32: Serial Debug Monitor**.
3. Accept **COM4**, or enter the adapter's current port.
4. Type `help` and press Enter. Commands are buffered until Enter.

The task uses the installed PlatformIO Python environment on Windows and the
`esp32dev` monitor configuration at 115200 baud. Output has host timestamps,
ESP32 exception decoding, and a flushed log file in `logs/device-monitor-*.log`.
Logs are excluded from Git. Exception decoding uses the local build, so keep
it matched to the firmware flashed on the device.

Useful commands include `power normal`, `power connected`, `power ultra`,
`wifi on`, `wifi window 120`, `scan`, and `diag off`. `wifi on` reopens the
configured access window without changing power mode. USB serial works while
Ultra has Wi-Fi turned off.

Use `diag terse` (default) for compact status lines and web response summaries,
or choose **Diagnostic detail > Terse / Verbose** in the web terminal. The
selector reflects the device's reported setting, including changes from USB,
and preserves any draft command. A queued change appears once confirmed by
the next successful poll. Alternatively use `diag verbose`
for the full state, heap triplets, and web start/phase/work
timings. The detail setting persists across restarts and applies to both USB
and web terminal output. It does not enable streaming; use `diag on` for that.
`diag snapshot` uses the selected detail, while `diag summary` remains a full
report. Terse `obs=14/0` means Wi-Fi/BLE observation counts; heap is in bytes.
Both modes show `radioLeft=30s` in Ultra mode, or `off`, `held`, `pending`, or
`continuous` when a countdown does not apply. This is the access-window budget;
active work can defer shutdown. Diagnostic counters and retained events are
still collected in terse mode according to the enabled categories.

Terminal commands `interval`, `bleinterval`, and `wifi window` clamp numeric
entries to 5..3600 seconds; `diag interval` clamps to 0..3600 (0 disables
periodic snapshots). For example, `wifi window 1` saves 5 seconds and prints
`Adjusted to 5s (allowed: 5..3600s).` Malformed numbers are rejected without
changing the setting. This applies to both USB and web terminal commands.

The monitor attempts reconnection if USB disconnects. DTR and RTS start low
to avoid deliberately requesting a reset when opening the monitor; some USB
drivers/boards can still glitch those lines during port opening.

Press **Ctrl+C** to stop the monitor. Stop other serial monitors before opening
this task, and stop this task before uploading if the uploader reports the port
is busy. To capture the entire boot, start the monitor and press the board's
reset button. Serial monitoring does not provide breakpoints or step debugging.

## Capturing lag

Main-loop gaps and wrapped HTTP handlers taking at least 500 ms are retained
as `LOOP` and `HANDLER` records even with `diag off` or terse output selected.
Use **Capture Diagnostics**, or download `/status.json` after a lag, and inspect
`webStallTrace`. Its eight-entry RAM ring also holds existing send/response and
browser stall records; newer events overwrite older ones and reboot clears it.
Save the export promptly. A single stall can produce multiple related records.

Records include duration, route/phase where available, heap, Wi-Fi/BLE scan
state, power mode, radio sleep state and access-window milliseconds remaining.
State is sampled after the delay and is context, not proof of the cause.
This capture does not require verbose logging or allocate strings while
recording. It cannot report a permanent lockup that never returns. `LOOP` is
the gap between loop entries, not a measured browser response time; handlers
are captured where routes use the diagnostic wrapper. Export handlers are
excluded from handler capture to avoid recording the export into itself.

## Web terminal operation

Open `/terminal`, type a command and press Enter or Send. USB and web commands
share the firmware dispatcher. Output appears in the terminal stream; sending
a command resumes polling. Existing filters can still hide matching responses.
Use `help`, `settings`, or `developer` to discover commands. Commands have the
same effects as USB commands, including erasing data and restarting the device.
The existing site access model applies: anyone with access to the web UI can
submit commands. The command endpoint does not add authentication.

Submission is a POST with a custom header, a 192-byte single-line limit and a
one-command mailbox. A 202 response means queued, not completed. Commands wait
for active scans to finish and run from loop(), outside the HTTP handler.
If delivery is uncertain, the browser does not automatically retry a command.
Inspect output before resubmitting, especially for restart or erase commands.

`wifi-config` uses a separate, nonblocking web prompt: submit the SSID, then
the password (empty for an open network). Select **Hide input** before entering
a password. Enter `cancel` to leave setup. This web setup prompt is shared
across web-terminal viewers; use one setup session at a time. After two minutes
of inactivity it expires; enter `wifi-config` or `cancel` to continue. Late
password input is discarded without being echoed. Password responses and
`appass` command echoes are redacted, and the browser saves no command history.

Wi-Fi commands cannot reach an already sleeping radio over the web. Use USB
`wifi on`, or wait for the next scheduled scan to reconnect to the web terminal.

V45 prints the active Survey Focus at startup and in `settings` and
`wifi-survey`. Network Inventory labels capacity as AP summaries and reports
that scan history is not retained. Its terse STATUS uses `ap/bleObs=` and its
verbose STATUS uses `wifiAPs=`. History presets keep observation labels.
Legacy diagnostic-event/JSON observation counters count summaries in Inventory;
use `wifiSurvey.inventory`, `historicalMeasurements`, and `summaryCount` in
`/status.json` to distinguish them. See [storage presets](TEST_STORAGE_V45.md).
