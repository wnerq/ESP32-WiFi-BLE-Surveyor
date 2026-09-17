# V45 shared Wi-Fi storage

V45 replaces fixed AP/observation partitions with one bounded allocation.
Settings offers Signal History, Balanced Survey (default), and Network Inventory.

## Allocation and retention

The pool contains timestamp metadata, AP identities and running statistics,
then the observation ring. History presets begin with 16 AP slots and grow
on demand, normally in batches of 16. Growing the AP region reduces observation
capacity, discards the oldest measurements if needed, and moves the surviving
ring in place. No heap allocation occurs during capture. Clear History resets
the reservation; checkpoint restore reserves only the saved AP count.

After timestamp metadata, Signal History allows up to 25% of the pool for AP
records; Balanced Survey allows 50%. Both retain individual RSSI measurements.
At the AP limit, unreferenced identities can be recycled. If all identities
still have measurements, Signal History skips new identities; Balanced Survey
replaces the least recently seen identity and removes all its measurements
before reusing the index. This prevents old measurements being attributed to
the wrong AP.

Inventory allocates the available pool to AP summaries, with one transient
scan-metadata slot and no observation ring. Repeated sightings update one
record per BSSID: first/last seen, count, latest/min/max/average RSSI. At capacity,
the least recently seen identity is replaced. Statistics restart on admission.
CSV has one summary row per AP; time-series plots are unavailable.

Limits still apply: 1,024 APs maximum, 12,000 observations maximum, and 512
timestamp slots in Wi-Fi-only history mode (64 with BLE). Actual capacity is
usually lower and depends on heap available at startup. The Wi-Fi-only 80 KB
reserve is measured at allocation, before web/mDNS services and transient work.
BLE uses a separate budget; the Wi-Fi presets also apply when BLE is enabled.

The percentages are policies over the same allocator, so intermediate policies
can be added later. V45 exposes three presets, not an allocation slider. The AP
reservation does not shrink continuously as identities disappear; clear or
checkpoint/restart compacts it. A fixed timestamp table remains inside the pool.

## Configuration and compatibility

- Settings changes persist and restart with cleared survey history/checkpoint.
- Saving the active preset is a no-op; export CSV before changing presets.
- Config JSON stores `surveyFocus`: 0 History, 1 Balanced, 2 Inventory.
- Config import validates that field; a different saved focus needs a restart.
- Checkpoint format 4 saves AP statistics and focus. Older formats and a
  different focus are rejected. CRC errors leave live history untouched.
- Inventory CSV adds first-seen time, sightings and RSSI summary columns;
  history CSV keeps its measurement schema.

## Validation

Actual firmware functions are extracted into host harnesses, compiled with
AddressSanitizer and UndefinedBehaviorSanitizer:

```text
python3 Tools/test_shared_storage.py
python3 Tools/test_shared_checkpoint.py
```

Coverage includes wrapped-ring relocation with measurement-value checks,
stable AP references, full-pool admission/eviction policies, Inventory updates,
clear/reuse, all three checkpoint round trips, growth during restore, and
corrupt/mismatched checkpoint rejection. Recovery, power, terminal-command,
browser-terminal and lag-recorder regressions also passed.
Hardware config export/import preserved the selected focus, and invalid focus
values were rejected without changing the setting.

## Hardware measurements, September 14, 2026

PlatformIO esp32dev, Arduino ESP32 3.3.11, Wi-Fi only, ESP32-D0WD-V3 on COM4.
USB and HTTP both identified V45. This board differs from the earlier V44
benchmark board, so these timings are not a controlled V44/V45 comparison.

Balanced Survey allocated 34,532 bytes. With 16 AP slots, capacity was 4,838
measurements. Adding synthetic APs grew the table to 32 slots without changing
pool bytes, leaving 4,603 measurements. The 95% prefill retained 4,372
measurements across 20 APs and 274 scan groups, with zero integrity anomalies.
Checkpoint/restart restored those records; the first real scan increased the
count to 4,375. Restoring 20 AP slots reclaimed some unused reservation and
raised measurement capacity to 4,779.

Three sequential HTTP requests per endpoint at the 95% prefill:

| Endpoint | Median elapsed time |
| --- | ---: |
| Main Wi-Fi page | 516 ms |
| Observed networks fragment | 265 ms |
| Channel fragment | 78 ms |
| Plot endpoint without selected BSSID | 47 ms |
| Terminal page | 500 ms |

The plot timing is an empty selection response, not a populated graph benchmark.
Minimum heap reached 19,164 bytes during this short sequence; no integrity
errors or unintended resets were observed. This is not an endurance test or a
guarantee under concurrent clients or dual-radio load.

Inventory allocated 34,524 bytes for 392 summaries (88 bytes/AP on this build).
Repeated real scans produced unique BSSID rows in CSV and increasing sighting
counts, with zero historical measurements and zero integrity anomalies.
Its checkpoint restored all seven summaries before subsequent scans updated
them. Signal History reported an 86-AP policy limit versus Balanced Survey's
172, with the same initial 4,838-measurement capacity while AP use was low.

Raw serial/JSON/CSV timing evidence is saved locally under ignored `logs/v45-*`;
it contains local network identifiers and is not committed.


## Mode-aware History Test Tools

Developer History Test Tools now accept 50, 75, 95 and 99 percent in every
Survey Focus. Inventory adds distinct locally administered `TEST-PREFILL-*`
AP summaries (including IDs beyond 255); the other modes add compact
measurements. Bluetooth uses its separate observation buffer. Plots and
terminal capture may both be disabled.

Targets are rounded down from the current retention capacity. AP-table growth
can reduce that capacity in measurement modes. An already-satisfied target is
a no-op. Filling adds synthetic data to the real survey; metadata recycling
and normal admission rules may age out existing data. Clear History removes
both real and synthetic data. Fully occupied identity tables can reject new
test identities; clear history before retrying if needed. Synthetic scan
batches may repeat identities to fill large buffers with few metadata slots.

Host validation (no hardware required):

- `wsl --exec python3 Tools/test_shared_storage.py`: all Wi-Fi modes and targets,
  mixed real/synthetic records, unique Inventory BSSIDs, repeated fills,
  busy guards, one-slot metadata, and storage integrity under sanitizers.
- `wsl --exec python3 Tools/test_ble_prefill.py`: every Bluetooth target with
  1, 2 and 256 metadata slots, repeat fills, and disabled/busy guards.

On hardware, select each focus, fill through each target, compare the displayed
count with `floor(capacity * percent / 100)`, export CSV, then allow real scans
to resume. Confirm Inventory rows remain unique by BSSID and history-mode
references remain valid. These prefill changes have not yet been bench-tested.
