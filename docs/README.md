# ESP32 Wireless Surveyor V23 — Static Demo Package

This package is a standalone documentation/demo mock-up of the ESP32 Wireless
Surveyor web interface. It contains **fictional sample data only** and does not
communicate with an ESP32.

## Open the demo

Unzip the package and open `index.html` in a modern browser.

No web server, build tools, Internet connection, or external JavaScript
libraries are required.

## Pages

- `index.html` — Wi-Fi Survey
- `bluetooth.html` — Bluetooth Survey
- `system.html` — System diagnostics and self-tests
- `settings.html` — device/network/interface settings

The shared navigation, theme selector, tables, demo buttons, and RSSI canvases
are implemented with `styles.css` and `app.js`.

## Intended use

This is suitable for:

- product/engineering documentation
- screenshots for a README or design document
- demonstrating the survey workflow without hardware
- explaining the compact Wi-Fi history and diagnostics architecture
- showing stakeholders the local web-interface concept

All SSIDs, BSSIDs, BLE addresses, measurements, IP details, memory values, and
device status shown here should be treated as illustrative demo content.

## Visual fidelity

The static pages are styled from screenshots of the live embedded interface and are intended to closely represent the V23 web UI. Values are simulated. Bluetooth intentionally shows the default disabled state.

For GitHub Pages, place these files at the root of the repository's `docs/` directory and configure **Settings → Pages → Deploy from a branch → main /docs**. `index.html` becomes the demo landing page.
