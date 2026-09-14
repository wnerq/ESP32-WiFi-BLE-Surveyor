# V40a3 recovery and terminal validation

`src/main.cpp` is canonical. Build with PlatformIO; its exporter synchronizes
`arduino/WifiConnect/WifiConnect.ino`. No partition or survey checkpoint schema
change is needed for this revision.

## Behavior

- Native reconnect remains enabled. Application recovery has a 20-second initial
  grace period and requires the saved SSID in a completed survey scan.
- Each application attempt is bounded to 15 seconds, including IP acquisition.
  An existing native association is given an IP acquisition window before it
  can be canceled. Association loss/failure and IP timeout are distinct results.
- Failed attempts back off for at least 10, 30, 60, 120, then 300 seconds, capped
  at 300 seconds. Another completed scan must provide fresh visibility evidence.
  Actual retry timing therefore also depends on the configured survey interval.
- Already-due surveys run before starting recovery. An active recovery can defer
  subsequent automatic Wi-Fi scans by at most its remaining 15-second window;
  a manual Wi-Fi scan cancels recovery. The device AP remains enabled.
- Native, application, boot, and manual success sources are distinguished.
  Recovery details appear in Diagnostics and `status.json` under
  `network.infrastructureRecovery`. A small versioned NVS record is saved at
  controlled restart; the previous record appears as `beforeControlledRestart`.
  This is not continuous flash logging or power-loss persistence.

## Terminal

Open `/terminal` on the device's IP or mDNS address. A Terminal link appears in
Developer view, including on Diagnostics. Enable Live updates to poll output.

The terminal mirrors sketch `Serial.print`/`println` output into an 8 KB byte ring
before forwarding it to UART. It includes partial lines and boot output still
retained in the ring. It cannot capture ESP ROM, core/library logs written
directly to UART, or text already overwritten. Startup AP-password output and
password-command echoes are redacted.

Polling reads at most 1 KB per response, normally once per second, and does not
arm the survey interaction defer or log its own requests. The page supports
pause/resume, auto-scroll, filtering, clearing the browser view, and download.
The browser retains at most 64 KB of text. Downloads contain that captured text,
including lines hidden by the filter. The page shows markers for device restart
and overwritten device output. Clear view does not erase the device ring.

The V40a3 page was read-only. V40a6 adds web command entry; see
[SERIAL_DEBUG.md](SERIAL_DEBUG.md). Serial commands remain available through the UART.

## Automated checks

From the repository root:

```text
pio run
wsl --exec python3 Tools/test_v40a3.py
node Tools/test_terminal.mjs
git diff --check
```

The Python test extracts actual firmware recovery functions and the terminal
ring and compiles them with g++ against radio, UART, and NVS test doubles. It
covers scan evidence, grace timing, scan/interaction guards, association/IP
timeouts, capped backoff, immediate success cleanup, attribution, cancellation,
timer rollover, driver errors, summary persistence, and ring overwrite/cursors.

The JavaScript test executes the actual embedded terminal script with DOM/HTTP
test doubles. It covers filtering, literal markup, pause/resume, gaps, reboot,
UTF-8 split across responses, retries, download, and the browser text bound.
Neither test validates browser rendering or physical radio behavior.

## Bench and field checks (hardware required)

1. Boot with saved infrastructure Wi-Fi available; verify connection/IP,
   headless scans, device AP access, and V40a3 metadata.
   Also boot offline with Device AP disabled, restore infrastructure Wi-Fi,
   and verify the web server starts after recovery obtains an IP.
2. Open `/terminal` from both LAN and device AP. Compare application output to
   Serial, exercise all controls, and leave it polling for several scan cycles.
   Verify scans keep completing and no poll-generated HTTP log flood appears.
3. Reboot the router and test extended loss of coverage. Observe native recovery
   first; when that fails, verify a completed scan showing the saved SSID leads
   to application recovery. Test both the default and a short scan interval.
4. Request Scan Now during recovery. Confirm the request takes priority and
   automatic surveying resumes after any timed-out reconnect attempt.
5. On a test network, separately cause authentication failure and unavailable
   DHCP. Verify distinct results and increasing, capped backoff. Restore service
   and confirm successful IP acquisition clears stale visibility immediately.
6. Capture `status.json` and terminal text before manually recovering a failed
   test. Perform a controlled restart and verify the previous recovery summary
   survives. Confirm no password appears in either output.
7. Repeat with BLE enabled. Watch free/minimum heap, largest block, scan cadence,
   and device AP/web responsiveness. The terminal adds approximately 8 KB of
   static RAM; auto-sized history may shrink to preserve allocation headroom.
8. Perform the decisive regression: home → leave coverage → remain away → return
   home. Verify autonomous reconnection without re-entering credentials and
   uninterrupted surveying while infrastructure is unavailable.
