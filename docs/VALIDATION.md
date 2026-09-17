# Build and deployment validation

## Before upload

1. Run `python tools/export_arduino.py` after source edits outside PlatformIO.
2. Run `python Tools/validate.py --host` from the repository root. This runs all
   host tests and returns a nonzero exit code for any failure. Requires Node.js,
   Python 3.9+ and g++; Windows may use WSL's Python/g++ automatically.
3. Run `pio run`. The preflight hook checks generated-source consistency and
   Help destinations after automatic sketch regeneration. PlatformIO checks
   compilation, linking and partition size. Host tests are a separate step.
4. Export real survey data before intentional resets or memory-profile changes.
   Stop any serial monitor before `pio run -t upload --upload-port <port>`.

No validation command flashes hardware automatically. The upload command above
is an explicit deployment action.

## Automated post-upload checks

After startup scans settle, run:

```text
python Tools/validate.py --device http://192.168.4.1 --expect-version 45
```

Substitute the actual device origin. The checker performs only GET requests,
uses a 30-second timeout per request, and limits response sizes. It checks:

- Expected firmware version and presence of current feature pages.
- Wi-Fi/AP capacity bounds, integrity and mode-appropriate inventory counts.
- Active and saved terminal/plot settings; pending changes produce warnings.
- Terminal capacity response headers, including capture Off.
- Failed storage/configuration health indicators.

WARN indicators are printed; `--strict` makes any warning fail the command.
No network identities, logs or credentials are printed or written to disk.
Tests contact only the supplied origin's known endpoints, but HTTP redirects
are handled by Python's standard client. Use the device's direct address.
The requests count as web activity and extend Ultra's awake hold; they cannot
wake a sleeping radio. Use USB `wifi on` or the survey button before testing.

## Manual acceptance

| Feature | Check |
| --- | --- |
| Boot/build | Confirm version and build timestamp match the intended artifact; inspect resets and allocation failures. |
| Memory profiles | Save terminal Off/1/32 KiB and plots off/on, restart, and compare active values, survey capacity, free heap and largest block. |
| Plots off | Wi-Fi/BLE pages and direct plot URLs show disabled messaging; measurements and CSV still work. |
| Terminal Off | No capture buffer is allocated; USB still prints output and web commands still execute. |
| Prefill | In each Survey Focus, fill through 50/75/95/99%; compare with floor(current capacity × percent / 100). Inventory CSV must have unique BSSIDs. Repeat with BLE enabled. |
| Card movement | Test mouse and touch handles, edge scrolling, keyboard arrows/Home/End, Escape cancellation and Reset Layout. Forms, text selection and Help links must still work. |
| Saved layouts | Refresh and navigate between pages; `/` and `/scan` share order. Switch view depth and wait for live fragments; positions must persist. Test blocked browser storage. |
| Help | Developer Memory explanation appears once on Help; Developer view adds only distinct detail. |
| Wi-Fi Ultra | Non-terminal activity renews five minutes plus configured timeout. Terminal polling alone permits sleep. Verify reconnect after next scan. |
| Checkpoints | Save/export data before changing allocation; verify restored history or an explicit restore limitation after reboot. |
| Endurance | Run real scans, concurrent clients and CSV exports at high occupancy; inspect heap, responsiveness and integrity. |

Synthetic data and host test doubles cannot establish RF behavior, electrical
power savings, browser rendering quality, or long-run stability. A successful
build is not a successful hardware deployment.
