# DDC-Matter

An ESP32 firmware that exposes a connected monitor's brightness, contrast, and input source as a **Matter** accessory — controllable from Apple Home, Google Home, or any Matter-compatible controller.

It uses **DDC/CI over I²C** to read the monitor's EDID, query and set VCP codes, and automatically identify the monitor's input capabilities from the [ddccontrol-db](https://github.com/ddccontrol/ddccontrol-db) database. A built-in mDNS web UI lets you review and override the detected configuration.

---

## Features

| Feature | Details |
|---|---|
| **EDID identification** | Reads 128-byte EDID from I²C address `0x50`; parses PnP manufacturer ID and monitor name |
| **DDC/CI control** | Brightness (VCP `0x10`), Contrast (VCP `0x12`), Input Source (VCP `0x60`) |
| **Monitor database** | Fetches monitor profile XML from ddccontrol-db by PnP ID to map input values to human-readable labels |
| **Matter device** | Brightness → Level Control endpoint; Contrast → Level Control endpoint; Input → Mode Select endpoint |
| **Web config UI** | mDNS-advertised single-page app at `http://display-switcher.local/` for reviewing and overriding configuration |
| **BLE commissioning** | Standard esp-matter BLE + QR-code commissioning flow |
| **NVS persistence** | User config and cached monitor profiles survive reboots |

---

## Hardware

| Signal | GPIO |
|---|---|
| I²C SDA | GPIO 21 |
| I²C SCL | GPIO 22 |
| Bus speed | 100 kHz |

Connect the ESP32 DDC bus to the monitor's DDC/CI pins through a **3.3 V ↔ 5 V level shifter**. The level shifter provides its own pull-ups — do **not** add additional pull-ups on the ESP32 side.

---

## Monitor Detection Fallback Chain

On every boot (and on demand via the web UI), the firmware resolves the monitor's input layout through this ordered chain:

1. **NVS user config** — a saved override from the web UI takes highest precedence
2. **NVS cached profile** — the last successfully fetched ddccontrol-db result for the same PnP ID
3. **Remote ddccontrol-db fetch** — `https://raw.githubusercontent.com/ddccontrol/ddccontrol-db/master/db/monitor/u<PNP_ID>.xml`
4. **Direct DDC capabilities query** — VCP `0xF3` multi-part read, parsed for input values
5. **MCCS defaults** — hardcoded standard input labels (VGA, DVI, DP, HDMI)

> **Note:** The remote fetch (step 3) requires network connectivity. On a freshly commissioned device the network is available after BLE commissioning, so the first boot will fall through to DDC capabilities or MCCS defaults and populate the cache on the next detect cycle.

---

## Matter Endpoints

| Endpoint | Cluster | Maps to |
|---|---|---|
| 1 | Level Control (`0x0008`) | Brightness — VCP `0x10` |
| 2 | Level Control (`0x0008`) | Contrast — VCP `0x12` |
| 3 | Mode Select (`0x0050`) | Input source — VCP `0x60`, up to 4 inputs |

Input slots are populated from the monitor detection chain above and stored in NVS.

---

## Web Config UI

After commissioning, the device advertises `http://display-switcher.local/`. The single-page frontend (embedded from `frontend/index.html`) shows:

- Detected monitor name and PnP ID
- ddccontrol-db match status
- 4 input rows with hex VCP value, label, and a **Test** button
- Brightness/contrast VCP code overrides
- **Refresh from database** — re-runs the remote fetch
- **Save to NVS** — persists the current config as the user override

REST API:

| Method | Path | Description |
|---|---|---|
| `GET` | `/api/config` | Return current config JSON |
| `POST` | `/api/config` | Write and persist config |
| `POST` | `/api/test` | Send a one-shot DDC write to test an input value |
| `GET` | `/api/detect` | Re-run the full detection chain |

---

## Project Structure

```
main/
  app_main.cpp      Boot sequence, Matter callbacks, DDC orchestration
  ddc.c / ddc.h     I²C DDC/CI driver (EDID, VCP get/set, capabilities query)
  edid.c / edid.h   EDID parser (PnP ID, monitor name descriptor)
  mccs.c / mccs.h   MCCS input label table and default slot filler
  monitor_db.c/.h   ddccontrol-db HTTP fetch and capability string parser
  config.c / config.h  NVS load/save for user config and cached profiles
  matter.cpp / matter.h  esp-matter endpoint setup and cluster callbacks
  webserver.c / webserver.h  HTTP server and REST API
  CMakeLists.txt
frontend/
  index.html        Embedded web UI (EMBED_FILES)
idf_component.yml   Component dependencies (esp_matter ^1.4.2, mdns ^1.8.2)
sdkconfig.defaults  Project-level Kconfig defaults
CMakeLists.txt
```

---

## Setup & Build

The repo now includes a consistent setup and build flow:

```bash
make dev-init
make build
make flash PORT=/dev/tty.usbserial-0001
```

What `make dev-init` does:

- clones pinned copies of ESP-IDF and esp-matter into `.deps/`
- installs ESP-IDF toolchain support for `esp32`
- bootstraps the connectedhomeip / Pigweed environment required by esp-matter
- installs a user-local `cmake<4`, because ESP-IDF v5.4.1 is not compatible with Homebrew CMake 4.x on this machine
- writes `.env.mk` so subsequent `make` targets reuse the same paths consistently

Available build targets:

- `make build`
- `make reconfigure`
- `make clean`
- `make fullclean`
- `make detect-port`
- `make size`
- `make flash`
- `make monitor`
- `make flash-monitor`
- `make web-installer`

`make flash`, `make monitor`, and `make flash-monitor` auto-detect a serial port on macOS and Linux. If more than one candidate is present, set `PORT=/dev/...` explicitly.

---

## Web Installer

A GitHub Pages web flasher is now supported via `esp-web-tools`.

- Local staging: `make web-installer`
- Local output: `build/web-installer/`
- Auto deployment: `.github/workflows/pages.yml` builds the firmware on pushes to `main` and deploys the installer page to GitHub Pages

The published page can flash the latest ESP32 firmware directly from Chrome or Edge using Web Serial, without requiring users to install ESP-IDF locally.

---

## Build Status

The firmware now builds successfully into a flashable image for ESP32 classic. The current partition layout uses a custom `partitions.csv` with a `0x300000` factory app slot so the Matter image fits in 4 MB flash.

---

## Dependencies

| Dependency | Source |
|---|---|
| ESP-IDF v5.x | https://github.com/espressif/esp-idf |
| esp-matter ^1.4.2 | https://github.com/espressif/esp-matter |
| connectedhomeip | pulled transitively by esp-matter |
| mdns ^1.8.2 | https://github.com/espressif/esp-idf (managed component) |
| ddccontrol-db | runtime fetch only — https://github.com/ddccontrol/ddccontrol-db |

No compile-time external dependencies beyond ESP-IDF and esp-matter.

---

## References

- MCCS/DDC-CI spec: [MCCS standard](https://www.ddcutil.com/mccs_background/)
- ddcutil VCP codes: https://www.ddcutil.com/command_getvcp/
- EDID byte layout: https://en.wikipedia.org/wiki/Extended_Display_Identification_Data
- ddccontrol packet implementation: [ddcutil src/base/ddc_packets.c](https://github.com/rockowitz/ddcutil)
- esp-matter example — Level Control: `examples/light/`
- esp-matter example — Mode Select: `examples/mode_select_device/`

---

## License

MIT
