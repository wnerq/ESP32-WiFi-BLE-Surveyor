# V40a4 diagnostic LED

The GPIO2 indicator is owned exclusively by `DiagnosticLedManager` in
`src/main.cpp`. Subsystems submit `LedDiagEvent` events or semantic state updates;
the manager uses `millis()` phases, a bounded event bitmask and no allocations,
FreeRTOS LED timers, NVS writes, sleeps or loops waiting for flashes.

## Indications

| Condition | Pattern |
| --- | --- |
| Normal idle | Off |
| Wi-Fi scan | Existing 75 ms on / 75 ms off while scanning |
| BLE scan | Existing 125 ms on / 125 ms off while scanning |
| Saved infrastructure network disconnected | Two 80 ms flashes, separated by 100 ms, every 3 seconds |
| Application recovery attempt | Three 60 ms flashes, 60 ms apart |
| Reconnected with an IP | One 750 ms flash |
| Human page or explicit action | One 50 ms pulse |
| Actual serial RX | Two 25 ms pulses, 25 ms apart |
| Controlled reboot | Five 60 ms flashes, 60 ms apart; begins with 60 ms off so preemption is visible |
| Boot complete | 100 ms on, 120 ms off, 650 ms on, once at the end of setup |

Priority is controlled reboot, reconnect success, reconnect attempt, disconnected
heartbeat, serial RX, human page, Wi-Fi scan, then BLE scan. The once-only boot
pattern has startup priority between reboot and normal runtime diagnostics.
The existing explicit LED self-test is asynchronous, has scan-level priority,
and still allows a hardware test when the persisted LED setting is disabled.

Repeated events coalesce. Higher-priority events may discard an interrupted
transient. The persistent heartbeat automatically resumes on its 3-second
schedule, and lower-priority activity can appear in the quiet part of a cycle.
Reboot waits for all five flashes and the original call site's minimum response
delay. All intentional reboot sites use the existing deferred restart mechanism,
now shared by web, serial and the scan recovery watchdog. Checkpoint and
history-loss confirmation policies remain at their original call sites.

## Trigger scope

- The infrastructure LED uses the saved-credential presence cached by existing
  credential loads/saves/erase, STA mode and actual connection/IP state. It does
  not wait for another survey scan to clear on success or credential deletion.
- Application attempt flashes accompany `SAVED_NETWORK_DISCOVERY` recovery.
  The current Wi-Fi callbacks do not expose native attempt initiation reliably;
  disconnect/association observations are not relabeled as native attempts.
  Successful app/native/manual reconnects use the existing success/source event.
- An exact route allowlist covers HTML pages and explicit actions. Read polls
  such as `/api/wifi/*`, `/api/ble/*`, `/api/terminal`, `/api/ping`, and
  `/status.json` do not generate activity flashes. Specific settings POSTs such
  as `/api/wifi/interval` are intentional user actions and are allowed.
- Serial RX is hooked to successful character reads in the serial mirror, also
  covering the interactive credential input path. TX and merely checking UART
  availability never generate an event. No terminal-open detection is attempted.
- LED infrastructure logs report state transitions and actual attempt/success
  events only. Heartbeat flashes do not generate log messages.

## Verification

```text
wsl --exec python3 Tools/test_led_diag.py
wsl --exec python3 Tools/test_v40a3.py
node Tools/test_terminal.mjs
pio run
git diff --check
```

The LED host test compiles the actual manager, serial mirror and deferred
restart functions against simulated GPIO/UART/time. It checks all patterns,
each adjacent priority, interrupted success/reboot, coalescing, credential/IP/STA
state, heartbeat resumption, RX versus TX, polling exclusions, disabled LED and
self-test, rollover and long service gaps. Structural checks enforce one GPIO
writer and one `ESP.restart()` call, no blocking LED code and unchanged existing
diagnostic buffer capacities. Recovery and terminal regressions remain separate.

Validated with the pinned PlatformIO environment:

| Resource | V40a3 before LED work | V40a4 | Change |
| --- | ---: | ---: | ---: |
| Flash | 1,605,188 bytes | 1,606,880 bytes | +1,692 bytes |
| Static RAM | 81,788 bytes | 81,828 bytes | +40 bytes |

The build, all three host regression suites, generated-sketch synchronization
and diff whitespace checks passed. No new compiler warnings appeared; the two
volatile-qualified BLE counter increment warnings remain.

## Device checks still required

1. Observe boot, idle, normal Wi-Fi/BLE scans, page access and individual serial
   character input. Leave the terminal/status polls running and verify they do
   not themselves flash the LED.
2. Leave coverage with saved credentials: verify two flashes every 3 seconds.
   Return and correlate attempt/success indications with `[INFRA]` and `[LED]`
   logs. If recovery fails, capture diagnostics before manual `/wifi-save`;
   manual success should show `USER_REQUEST` and the 750 ms pulse.
3. Clear credentials or disable STA and verify the heartbeat stops immediately.
   Test overlaps, including webpage/RX activity while disconnected.
4. Exercise System restart, serial restart, settings requiring restart and the
   watchdog path in a controlled test. Verify five flashes before the reset.
5. Compare existing loop-gap diagnostics under the same Wi-Fi/BLE/UI load as
   V40a3. `status.json` includes `ledServiceMaxUs` and `ledServiceMaxGapMs` beside
   the LED settings to distinguish manager execution time from gaps in service.

No physical LED waveform or on-device loop-gap measurement is established by
the host tests. The main loop services LEDs before/after normal work; HTML
streaming and existing serial waits also advance them. Pre-existing blocking
operations (for example a legacy synchronous scan or manual connection handler)
can still stretch a waveform. The manager advances at most one phase per call,
so a delayed handler cannot cause a burst of catch-up flashes. Reconnect timers,
scan scheduling and HTTP polling cadence are unchanged by this revision.
