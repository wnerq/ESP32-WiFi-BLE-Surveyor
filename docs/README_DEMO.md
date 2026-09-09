# ESP32 Wireless Surveyor — Static V40a UI Demo

This directory is a GitHub Pages-ready static demonstration of the V40a web interface.

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
