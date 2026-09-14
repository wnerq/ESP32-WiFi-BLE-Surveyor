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
The monitor attempts reconnection if USB disconnects. DTR and RTS start low
to avoid deliberately requesting a reset when opening the monitor; some USB
drivers/boards can still glitch those lines during port opening.

Press **Ctrl+C** to stop the monitor. Stop other serial monitors before opening
this task, and stop this task before uploading if the uploader reports the port
is busy. To capture the entire boot, start the monitor and press the board's
reset button. Serial monitoring does not provide breakpoints or step debugging.

## Web terminal commands (V40a6)

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
