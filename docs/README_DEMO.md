# ESP32 Wireless Surveyor — Static V40a UI Demo

This directory is a GitHub Pages-ready static demonstration of the V40a web interface.

The current device firmware identifies as V45. This demo is a separate static
snapshot, not a firmware build or a live connection to a surveyor. See the
[project README](../README.md) for current device features and installation.

## Page layout

| Demo page | File | Purpose |
| --- | --- | --- |
| Wi-Fi | [index.html](index.html) | Survey controls, observations, RSSI history and channel analysis |
| Bluetooth | [bluetooth.html](bluetooth.html) | Optional BLE survey and device history |
| System | [system.html](system.html) | Device identity and interpreted health |
| Diagnostics | [diagnostics.html](diagnostics.html) | Survey, memory, timing and recovery detail |
| Settings | [settings.html](settings.html) | Simulated network, survey and interface configuration |
| Help | [help.html](help.html) | Feature explanations |

The shared navigation/control card includes Live Updates, Theme, and cumulative
Standard / Advanced / Developer views. View depth controls how much information
is shown; it is not an access-control mechanism.

## Differences from current firmware

The demo has no `terminal.html` page and does not implement the device's USB/web
command dispatcher, terse/verbose diagnostic output, or terminal numeric-bound
clamping. It also does not simulate the current Wi-Fi power modes, access-window
countdown, radio shutdown/reconnect cycle, or status LED breathing.

V45's shared storage pool and Signal History / Balanced Survey / Network
Inventory presets are also hardware features absent from this demo. Inventory
uses one running summary per AP and a different CSV schema; the simulated
downloads still represent the older history interface. See
[V45 storage design and validation](TEST_STORAGE_V45.md).

On hardware, `/terminal` appears in Developer navigation and accepts commands
such as `diag terse`, `diag verbose`, `interval 120`, and `wifi window 60`.
These are documented in the [serial guide](SERIAL_DEBUG.md) and
[power guide](TEST_WIFI_POWER.md); entering them is not a demo interaction.

## Privacy / sanitization

All network names, MAC/BSSID values, hostnames, and infrastructure IP addresses in this demo are synthetic. Password fields are empty and no credentials are included. The 192.168.4.1 address is the generic ESP32 soft-AP address shown by the UI, not a captured personal network address.

## Interactive demo behavior

The pages do not contact an ESP32 or any external service. JavaScript keeps synthetic demo state in browser `localStorage`. Supported interactions include Wi-Fi Scan Now, scan interval changes, clear history, synthetic history prefill, settings forms, BLE enable/disable and demo scanning, checkpoint controls, status LED test, simulated restart, configuration export/import, diagnostics capture, and CSV downloads.

Synthetic downloads included:

- `wifi_scanlog_demo.csv`
- `ble_scanlog_demo.csv`
- `status_demo.json`
- `config_demo.json`

Use `index.html` as the GitHub Pages entry point.

## Local preview and maintenance

Serve the repository root locally, then open `/docs/`:

```text
python -m http.server 8000
```

Open `http://localhost:8000/docs/`. Clear this origin's browser local storage to
reset persisted demo choices. Firmware builds do not regenerate these HTML
files; interface changes need a separate demo update. The Arduino sketch
exporter only generates `arduino/WifiConnect/WifiConnect.ino`.

This directory also contains [PlatformIO](BUILD_PLATFORMIO.md),
[Arduino](BUILD_ARDUINO.md), [serial](SERIAL_DEBUG.md), and hardware validation
guides. They describe the device and development workflow, not simulated demo
capabilities.
