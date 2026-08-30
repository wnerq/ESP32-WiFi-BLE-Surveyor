# V34 Web Interface Information Architecture Specification

**Project:** ESP32 Wireless Surveyor  
**Target revision:** V34  
**Status:** Planning / implementation specification  
**Scope:** Web-interface information architecture, view-depth model, terminology, and presentation. BLE performance defects are tracked separately.

## 1. Purpose

V34 will reorganize the web interface before further diagnostics and feature work. The current interface has accumulated operational information, troubleshooting data, implementation diagnostics, and test tools on the same pages. The goal is to preserve capability while making the normal survey workflow substantially easier to interpret.

The governing model is:

- **Page = subject:** Wi-Fi, Bluetooth, System, Settings.
- **View = depth/audience:** Standard, Advanced, Developer.
- Views are cumulative: **Standard ⊂ Advanced ⊂ Developer**.
- Standard must remain useful rather than becoming a simplified or crippled interface.
- Advanced adds operational detail useful for troubleshooting and deeper analysis.
- Developer adds implementation internals, instrumentation, test facilities, and firmware-development diagnostics.

## 2. Global Navigation

Top-level navigation remains:

**Wi-Fi | Bluetooth | System | Settings**

Global view selector becomes:

**View: Standard | Advanced | Developer**

Theme remains:

**Theme: System | Light | Dark**

Live Updates remains a global/header control where applicable.

## 3. Terminology Standard

Use these terms consistently throughout firmware, UI, documentation, and the browser simulation.

| Concept | Preferred term |
|---|---|
| Overall ongoing activity | Survey / Surveying |
| One radio acquisition | Scan |
| Persistent Wi-Fi operation | Surveying: Always On |
| Persistent BLE operation | Automatic Surveying |
| Time between acquisitions | Scan Interval |
| Immediate acquisition | Scan Now |
| Number of acquisitions | Scans This Session |
| One retained measurement | Observation |
| Collection of retained observations | History |
| Current retained amount | Stored Observations |
| Maximum retained amount | History Capacity |
| Samples represented by a table row | Count |
| Most recent RSSI | Last |
| Mean RSSI | Average / Avg in compact tables |
| Earliest retained occurrence | First Seen |
| Most recent occurrence | Last Seen |

### Network terminology

| Concept | Preferred label |
|---|---|
| Network joined by ESP32 | Infrastructure Wi-Fi |
| Joined network displayed on Wi-Fi page | Infrastructure Connected Network |
| AP provided by ESP32 | Device AP |
| Device AP SSID | Broadcast SSID |
| Configurable `.local` identity | Device Hostname |
| Reachable `.local` URL | Friendly Web Address |

Use **mDNS** primarily in Advanced/Developer diagnostics rather than as the normal user-facing name for the hostname feature.

### Status terminology

- **PASS** — expected function verified.
- **WARN** — functioning, but attention may be warranted.
- **FAIL** — expected function is not operating.
- **Disabled** — intentionally off/unavailable; neutral, not PASS or WARN.
- **Not initialized** — technical state for Advanced/Developer use.
- **Unknown** — insufficient information to determine state.

## 4. Wi-Fi Page

The Wi-Fi page answers: **What is the Wi-Fi survey showing?** Survey information takes priority over infrastructure/network-management information.

### 4.1 Standard

#### Survey Status & Controls

- Surveying: Always On
- Scan Interval
- Scans This Session
- Last Scan
- Scan Now

#### History

- Stored Observations / History Capacity
- Retained Scans
- Oldest Data
- Retained Time Window
- Download CSV
- Clear History
- Refresh Page where retained

#### RSSI History

- Plot remains Standard.
- Plot heading identifies the last-clicked network, e.g. `RSSI History — Wifi_7`.
- Remove the separate Selected SSID / Selected BSSID status block.
- Where necessary, deeper views may expose BSSID so the graph does not imply aggregation across multiple APs sharing an SSID.

#### Observed Networks

Standard columns:

- SSID
- Channel
- Last
- Avg
- Count
- Last Seen

#### Observed Channel Interference

Standard presents the useful result prominently:

- Suggested channel
- concise interpretation based on observed Wi-Fi interference

#### Infrastructure Connected Network

Place near the bottom rather than giving infrastructure connectivity top billing.

Standard:

- Connected SSID
- Current signal/RSSI

### 4.2 Advanced additions

- Auto-scan health/diagnostic
- Automatic scan starts/completions
- Start/completion failures
- Last automatic start/completion
- Successful scan duration statistics
- Full channel-analysis table and methodology
- Infrastructure BSSID and appropriate connection details
- Observed Networks adds Min and Max RSSI

### 4.3 Developer additions

#### Survey Scheduler Diagnostics

- Retry/backoff state
- user-interaction defer mechanics
- other scheduler internals

#### CSV Diagnostics

- CSV exports served
- last export rows/bytes/time/throughput

#### Wi-Fi Memory Diagnostics

- History RAM
- observation size/allocation
- AP table utilization/allocation
- scan metadata utilization/allocation
- Wi-Fi-specific heap/block diagnostics where useful

Observed Networks additionally exposes:

- BSSID
- Security
- First Seen

## 5. Bluetooth Page — Disabled State

Do not call this **Limited Mode**.

Page title remains **Bluetooth Survey** with an obvious disabled state.

### 5.1 Standard

- Bluetooth Survey is disabled.
- Explain that enabling Bluetooth permits Bluetooth surveying but **significantly reduces Wi-Fi history capacity** because both modes share available ESP32 memory.
- State that enabling Bluetooth requires a restart.
- Enable Bluetooth Survey control.
- Do not show empty history, table, or plot sections.

Avoid hardcoding a specific reduced Wi-Fi observation capacity in this warning.

### 5.2 Advanced

- BLE Stack: Not initialized
- concise explanation that disabling Bluetooth maximizes Wi-Fi history

### 5.3 Developer

- BLE History RAM: 0 KB
- implementation/allocation details

## 6. Bluetooth Page — Enabled State

Page title: **Bluetooth Survey**. Do not use **Bluetooth Survey Limited Mode**.

### 6.1 Standard

#### Survey Status & Controls

- Automatic Surveying
- Scan Interval
- Scans This Session
- Last Scan, shown as relative age rather than raw uptime
- Scan Now

#### History

- Stored Observations / History Capacity
- relevant retained-history information
- Download CSV
- Clear History

Show a concise Standard advisory that Bluetooth surveying significantly reduces available Wi-Fi history capacity.

#### RSSI History

- Plot remains Standard.
- Heading identifies the last-clicked device.
- Named example: `RSSI History — Meshtastic_b9c0`
- For unnamed devices, use the BLE address as the useful identity.
- Remove separate Selected Device / BLE Address status fields.

#### Observed BLE Devices

Use wording that does not imply each BLE address is necessarily a unique physical device, e.g. **one row per retained BLE address**.

Standard columns:

- Name
- Address
- Last
- Avg
- Count
- Last Seen

The BLE address remains Standard because many advertisements have no useful device name.

### 6.2 Advanced additions

- scan health
- dropped BLE observations
- Min RSSI
- Max RSSI
- other useful operational scan statistics

### 6.3 Developer additions

- BLE observation size
- BLE address table utilization/allocation
- BLE scan metadata utilization/allocation
- CSV performance instrumentation
- BLE-specific memory diagnostics
- synchronous BLE API / web-servicing implementation information
- Address Type
- First Seen

## 7. System Page

The System page answers: **What is the device doing, and is it healthy?** Survey-specific history belongs on Wi-Fi/Bluetooth rather than System.

### 7.1 Device

Standard:

- Firmware Version
- Uptime
- Last Reset

Advanced adds:

- Build timestamp

Developer adds:

- Firmware filename
- Arduino ESP32 Core
- ESP-IDF
- Chip
- CPU
- other detailed hardware/build information

### 7.2 System Health

Standard presents a compact interpreted summary, for example:

- Wi-Fi Survey — PASS/WARN/FAIL
- Bluetooth Survey — PASS/WARN/FAIL/Disabled
- Automatic Surveying — PASS/WARN/FAIL
- Memory — PASS/WARN
- Last Reset
- Overall — PASS/WARN/FAIL

Advanced adds detailed operational checks:

- history buffers
- scan configuration
- initial boot scans
- automatic survey cadence
- scan timing
- checkpoint storage
- mDNS
- Free Heap
- Minimum Free Heap
- Survey Memory Mode

Developer adds:

- application-space diagnostics
- detailed memory allocation
- Largest Free Block
- target heap reserve
- partition details
- Boot Heap Checkpoints
- raw implementation diagnostics

### 7.3 Network

Standard:

- Infrastructure connection state
- connected network
- signal
- Device AP state
- Broadcast SSID
- Device Hostname / Friendly Web Address

Advanced:

- reconnect status
- IP/network details
- MAC information where appropriate
- mDNS status
- radio/channel details

Developer:

- AP+STA mode
- internal radio/network state
- lower-level network diagnostics

Some state is intentionally repeated across Wi-Fi, System, and Settings because the context differs: survey context, health context, and configuration context respectively.

### 7.4 Session

Advanced:

- Restored This Boot
- saved checkpoint status

Developer:

- Save/Checkpoint Session
- Discard Saved Session
- checkpoint implementation/explanatory detail

### 7.5 History Test Tools

Developer only:

- synthetic Wi-Fi history prefill
- 50 / 75 / 95 percent controls
- clear TEST FEATURE warning

## 8. Settings Page

Settings answers: **What can the user change about device operation?** Normal settings generally remain available in Standard. Higher views add status, maintenance, and implementation detail rather than hiding ordinary controls.

### 8.1 Infrastructure Wi-Fi

Standard:

- connection state and connected SSID
- SSID/password inputs
- Connect & Save
- Clear Saved Wi-Fi
- concise statement that infrastructure Wi-Fi is optional and does not stop automatic surveying
- stored passphrase is not displayed

Advanced adds:

- IP address
- deeper connection detail

### 8.2 Device Hostname

Standard:

- Device Hostname
- Friendly Web Address
- hostname input
- Save & Restart
- basic naming requirements

Advanced:

- mDNS status
- `.local`/IP fallback behavior

### 8.3 Device AP

Standard:

- Status
- **Broadcast SSID**
- Access Point Settings

Use **Device AP** as the section heading.

### 8.4 Survey Mode

Standard:

- Bluetooth Survey enabled/disabled
- enable/disable control
- Wi-Fi-history tradeoff warning
- restart requirement

Developer adds:

- NVS detail
- BLE persistent heap/allocation explanation
- boot-time history-sizing implementation detail

The warning shown here should be consistent with the warning on the disabled Bluetooth page.

### 8.5 Interface & Indicators

Standard:

- status LED enable/disable
- Save Interface Settings
- current enabled/disabled state

Advanced:

- Test Status LED
- browser-local view/theme persistence explanation where useful

Developer:

- GPIO assignment

Remove the redundant Survey Page Live Updates status from Settings; Live Updates is controlled globally in the page header.

### 8.6 Configuration Backup & Restore

Advanced:

- Download Configuration
- Restore Configuration
- Validate & Apply Configuration
- explanation that secrets/passwords are excluded

Prefer user-facing button names that describe the action; `.json` does not need to be emphasized.

### 8.7 Diagnostics Export

Developer:

- Download Status JSON
- explanation that it is a diagnostic snapshot and does not contain retained observation rows

## 9. Table Definitions

### 9.1 Wi-Fi Observed Networks

| Column | Standard | Advanced | Developer |
|---|:---:|:---:|:---:|
| SSID | ✓ | ✓ | ✓ |
| Channel | ✓ | ✓ | ✓ |
| Last RSSI | ✓ | ✓ | ✓ |
| Average RSSI | ✓ | ✓ | ✓ |
| Count | ✓ | ✓ | ✓ |
| Last Seen | ✓ | ✓ | ✓ |
| Min RSSI |  | ✓ | ✓ |
| Max RSSI |  | ✓ | ✓ |
| BSSID |  |  | ✓ |
| Security |  |  | ✓ |
| First Seen |  |  | ✓ |

Compact Standard headings may use `Ch`, `Last`, and `Avg`.

### 9.2 Bluetooth Observed Devices

| Column | Standard | Advanced | Developer |
|---|:---:|:---:|:---:|
| Name | ✓ | ✓ | ✓ |
| Address | ✓ | ✓ | ✓ |
| Last RSSI | ✓ | ✓ | ✓ |
| Average RSSI | ✓ | ✓ | ✓ |
| Count | ✓ | ✓ | ✓ |
| Last Seen | ✓ | ✓ | ✓ |
| Min RSSI |  | ✓ | ✓ |
| Max RSSI |  | ✓ | ✓ |
| Address Type |  |  | ✓ |
| First Seen |  |  | ✓ |

Do not describe each retained BLE address as necessarily representing one unique physical device.

## 10. Functional Issues Explicitly Outside the IA Refactor

These issues are important V34 engineering work but should remain separate from the information-architecture implementation so their root causes can be investigated independently.

### 10.1 Controlled-restart browser recovery

Observed when enabling Bluetooth:

- setting is saved and ESP32 restarts successfully
- interstitial page reports `Waiting for the surveyor...`
- browser may remain there indefinitely

Expected behavior:

1. Save setting.
2. Perform controlled restart.
3. Detect that the ESP32 is reachable again.
4. Navigate to a normal GET page, preferably Bluetooth Survey for a Bluetooth-mode change.
5. If automatic recovery fails, provide an explicit reconnect/return control.

Avoid leaving the browser on a POST/restart response that creates refresh/form-resubmission problems.

### 10.2 BLE-mode web responsiveness

Current dual Wi-Fi + BLE operation is severely impaired for web navigation. Page changes can take too long or time out entirely. This is more serious than the restart interstitial alone.

Investigation should distinguish among at least:

- synchronous BLE scanning blocking web servicing
- BLE scan interval/cadence effects
- heap pressure / fragmentation
- scheduling interactions
- other server/radio contention

A longer BLE scan interval should be tested before redesigning the operating mode.

### 10.3 Deferred fallback concept: temporary BLE acquisition mode

Do **not** implement yet.

If reliable simultaneous Wi-Fi/BLE operation cannot be achieved on this hardware, consider a future workflow that:

1. configures BLE acquisition parameters beforehand
2. temporarily switches away from normal Wi-Fi surveying
3. performs a configurable number of BLE scans
4. saves/preserves the BLE results
5. returns to Wi-Fi mode for normal operation and web access

This is a contingency concept, not a V34 requirement until the root cause of current BLE performance is understood.

## 11. Implementation Constraints

- Preserve known-good survey behavior while refactoring presentation.
- Do not couple the IA refactor to BLE performance fixes unnecessarily.
- Avoid large page-generation allocations; retain the buffered/streamed response techniques established after the full-history performance work.
- Standard/Advanced/Developer selection remains browser-local unless deliberately changed later.
- Avoid hardcoded firmware-version text; use central firmware identity constants.
- Keep terminology consistent between firmware UI, documentation, exported diagnostics where appropriate, and the browser simulation.

## 12. Post-Implementation Documentation Work

After the V34 interface structure is implemented and validated:

1. Update project documentation to match the new page hierarchy and terminology.
2. Update the browser/simulation mockup to match the actual firmware interface.
3. Review screenshots/examples so they reflect Standard, Advanced, and Developer views correctly.
4. Avoid documenting transient implementation details as permanent user-facing behavior.

## 13. V34 UI/IA Acceptance Checklist

- [ ] Global View selector provides Standard / Advanced / Developer.
- [ ] Views are cumulative.
- [ ] Wi-Fi page prioritizes survey information over infrastructure connection information.
- [ ] Infrastructure Connected Network appears near the bottom of Wi-Fi.
- [ ] Wi-Fi RSSI plot identifies the last-clicked network without redundant Selected SSID/BSSID fields.
- [ ] Wi-Fi table columns follow the defined S/A/D split.
- [ ] Channel recommendation is useful in Standard; detailed model is available in Advanced.
- [ ] Disabled Bluetooth page is intentionally sparse and warns about reduced Wi-Fi history before enable.
- [ ] Bluetooth page no longer uses `Limited Mode` as its title.
- [ ] Enabled Bluetooth RSSI plot identifies the last-clicked device/address.
- [ ] BLE table columns follow the defined S/A/D split.
- [ ] System presents interpreted health before raw diagnostics.
- [ ] Boot heap checkpoints and synthetic history tools are Developer-only.
- [ ] Settings retains normal user controls in Standard.
- [ ] Settings uses Device AP / Broadcast SSID terminology.
- [ ] Settings uses Device Hostname as the normal user-facing term.
- [ ] Configuration backup/restore is Advanced.
- [ ] Status JSON export is Developer.
- [ ] Live Updates status is not redundantly repeated in Settings.
- [ ] Observation/History/Survey/Scan terminology is consistent across Wi-Fi and Bluetooth.
- [ ] Disabled is treated as neutral rather than PASS/WARN.
- [ ] BLE responsiveness and restart/reconnect defects remain separately testable from the IA refactor.
