# ESP32 Wireless Surveyor V34b — Static Demo Package

This directory contains a standalone documentation/demo mock-up of the ESP32 Wireless Surveyor web interface. It uses **fictional sample data only** and does not communicate with an ESP32.

## Open the demo

Open `index.html` in a modern browser. No web server, build tools, Internet connection, or external JavaScript libraries are required.

## Pages

- `index.html` — Wi-Fi Survey
- `bluetooth.html` — Bluetooth Survey; begins in the normal disabled state and can simulate the enabled state
- `system.html` — Device and System Health
- `settings.html` — device/network/survey settings

`styles.css` and `app.js` provide the shared V34b-style navigation, sticky control card, Standard / Advanced / Developer view filtering, System / Light / Dark themes, sortable tables, demo controls, and RSSI plots.

## V34b interface model

The page tells you **what subject you are looking at**: Wi-Fi, Bluetooth, System, or Settings. The View selector tells you **how deep you want to go**:

- Standard — normal operation and useful survey results
- Advanced — deeper operational/troubleshooting information
- Developer — implementation internals, instrumentation, and test tools

Views are cumulative. The common navigation/control card is sticky so page navigation, Live Updates, View, and Theme remain available while scrolling.

## Intended use

The demo is suitable for product/engineering documentation, screenshots, demonstrating the survey workflow without hardware, and reviewing the interface information architecture.

All SSIDs, BSSIDs, BLE addresses, measurements, IP details, memory values, firmware states, and diagnostics are illustrative.

For GitHub Pages, keep these files in the repository `docs/` directory and configure Pages to deploy from `main /docs`. `index.html` is the landing page.
