# DDC-Matter

An ESP32 firmware that exposes a connected monitor's brightness, contrast, and input source as a **Matter** accessory — controllable from Apple Home, Google Home, or any Matter-compatible controller.

It uses **DDC/CI over I²C** to read the monitor's EDID, query and set VCP codes, and automatically identify the monitor's input capabilities from the [ddccontrol-db](https://github.com/ddccontrol/ddccontrol-db) database. A built-in HTTP web UI lets you review and override the detected configuration.

---

## Features

| Feature | Details |
|---|---|
| **EDID identification** | Reads 128-byte EDID from I²C address `0x50`; parses PnP manufacturer ID and monitor name |
| **DDC/CI control** | Brightness (VCP `0x10`), Contrast (VCP `0x12`), Input Source (VCP `0x60`) |
| **Monitor database** | Fetches monitor profile XML from ddccontrol-db by PnP ID to map input values to human-readable labels |
| **Matter device** | Brightness → Level Control endpoint; Contrast → Level Control endpoint; Input → Mode Select endpoint |
| **Web config UI** | HTTP single-page app for reviewing configuration, testing live brightness/contrast/input writes, and saving overrides after commissioning completes |
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

## Known Input Codes

The firmware currently knows the common MCCS input values plus a set of LG-specific values used by some displays. These are the values users can try in the web UI when auto-detection is wrong.

Standard DDC input switching uses destination `0x51` with VCP `0x60`.
LG-specific input switching uses destination `0x50` with manufacturer-specific VCP `0xF4`.

The web UI input fields expect normal decimal numbers, so use the `Decimal value` column below when typing values manually.

Common MCCS values:

| Input | Decimal value | Hex |
|---|---:|---|
| VGA 1 | `1` | `0x01` |
| VGA 2 | `2` | `0x02` |
| DVI 1 | `3` | `0x03` |
| DVI 2 | `4` | `0x04` |
| Composite video 1 | `5` | `0x05` |
| Composite video 2 | `6` | `0x06` |
| S-Video 1 | `7` | `0x07` |
| S-Video 2 | `8` | `0x08` |
| Tuner 1 | `9` | `0x09` |
| Tuner 2 | `10` | `0x0A` |
| Tuner 3 | `11` | `0x0B` |
| Component video 1 | `12` | `0x0C` |
| Component video 2 | `13` | `0x0D` |
| Component video 3 | `14` | `0x0E` |
| DisplayPort 1 | `15` | `0x0F` |
| DisplayPort 2 | `16` | `0x10` |
| HDMI 1 | `17` | `0x11` |
| HDMI 2 | `18` | `0x12` |
| HDMI 3 | `19` | `0x13` |
| HDMI 4 | `20` | `0x14` |
| USB-C 1 | `25` | `0x19` |
| USB-C 2 | `27` | `0x1B` |
| USB-C 3 | `28` | `0x1C` |

LG-specific values used by some displays:

| Input | Decimal value | Hex |
|---|---:|---|
| HDMI 1 (LG specific) | `144` | `0x90` |
| HDMI 2 (LG specific) | `145` | `0x91` |
| HDMI 3 (LG specific) | `146` | `0x92` |
| HDMI 4 (LG specific) | `147` | `0x93` |
| DisplayPort 1 (LG specific) | `208` | `0xD0` |
| DisplayPort 2 (LG specific) | `209` | `0xD1` |
| DisplayPort 3 (LG specific) | `192` | `0xC0` |
| DisplayPort 4 (LG specific) | `193` | `0xC1` |
| USB-C 1 (LG specific) | `210` | `0xD2` |
| USB-C 2 (LG specific) | `211` | `0xD3` |
| USB-C 3 (LG specific) | `224` | `0xE0` |
| USB-C 4 (LG specific) | `225` | `0xE1` |

Not all LG displays use the same family of LG-specific inputs. For example, some models appear to use `USB-C 3` and `DisplayPort 3`, while others use `USB-C 1` and `DisplayPort 1`. So for LG displays, users may need to try both the `1/2` and `3/4` LG-specific families.

---

## Web Config UI

After commissioning completes, the firmware starts the web UI HTTP server on port 80. Access it via the device IP address shown in the serial log or your router's client list. The single-page frontend (embedded from `frontend/index.html`) shows:

- Detected monitor name and PnP ID
- ddccontrol-db match status
- 5 input rows with hex input value, label, and a **Test** button
- Live brightness and contrast test sliders wired directly to DDC/CI
- Current input readback for standard `0x60`, alternate `0xF4`, and the resolved active input
- Brightness/contrast VCP code overrides
- **Refresh from database** — re-runs the remote fetch
- **Auto-Probe Inputs** — cycles known standard and LG-specific input codes, keeps the values that actually read back, and restores the original input
- **Save to NVS** — persists the current config as the user override

REST API:

| Method | Path | Description |
|---|---|---|
| `GET` | `/api/config` | Return current config JSON |
| `POST` | `/api/config` | Write and persist config |
| `GET` | `/api/levels` | Return the current brightness and contrast VCP state |
| `POST` | `/api/levels` | Send a one-shot brightness or contrast write |
| `GET` | `/api/input-source` | Return standard, alternate, and resolved input readback |
| `POST` | `/api/test` | Send a one-shot DDC write to test an input value |
| `POST` | `/api/probe-inputs` | Probe known input values using input readback and update the in-memory mapping |
| `GET` | `/api/detect` | Re-run the full detection chain |

There is no separate `.local` hostname for the web UI anymore. The previous `display-switcher.local` mDNS advertisement was removed so the web UI does not interfere with Matter commissioning.

## Matter Discovery

The firmware uses Matter's normal discovery transports:

- BLE advertisement during the initial commissioning flow
- `_matterc._udp` DNS-SD advertisement while the device is commissionable on the local network
- `_matter._tcp` DNS-SD advertisement after the device has been commissioned onto a fabric

The config UI is not published as its own mDNS HTTP service. Use the device IP address for the web UI.

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

After flashing, commission the device from Apple Home or another Matter controller. The current firmware is built with esp-matter test onboarding parameters, so the setup values are:

```text
Manual setup code: 34970112332
Discriminator:     3840
Passcode:          20202021
QR payload:        MT:Y.K9042C00KA0648G00
```

In Apple Home, use Add Accessory, then More Options if the device does not appear immediately in the first scan list. The device advertises over BLE for initial commissioning, then uses Matter DNS-SD (`_matterc._udp` before pairing and `_matter._tcp` after pairing).

To keep commissioning isolated from the rest of the application, monitor detection, DDC polling, and the web UI now start only after the device has completed Matter commissioning or on later boots when it is already commissioned.

If flashing fails with `Failed to connect to ESP32: No serial data received`, the serial port is correct but the chip did not enter the ROM bootloader. Two built-in fallback paths are available:

```bash
make flash-safe
make flash-manual PORT=/dev/tty.usbserial-0001
```

`make flash-safe` lowers the flash baud rate to `115200`. `make flash-manual` uses direct `esptool` flashing with `--before no_reset --after no_reset`, so you can enter download mode manually by holding `BOOT`, tapping `EN/RESET`, then releasing `BOOT` before the command runs.

What `make dev-init` does:

- clones pinned copies of ESP-IDF and esp-matter into `.deps/`
- installs ESP-IDF toolchain support for `esp32`
- bootstraps the connectedhomeip / Pigweed environment required by esp-matter
- installs a user-local `cmake<4`, because ESP-IDF v5.4.1 is not compatible with Homebrew CMake 4.x on this machine
- writes `.env.mk` so subsequent `make` targets reuse the same paths consistently

Available build targets:

- `make build`
- `make merged-bin`
- `make reconfigure`
- `make clean`
- `make fullclean`
- `make detect-port`
- `make size`
- `make flash`
- `make flash-safe`
- `make flash-manual`
- `make monitor`
- `make monitor-idf`
- `make flash-monitor`
- `make flash-monitor-idf`
- `make web-installer`

`make flash`, `make flash-safe`, `make flash-manual`, `make monitor`, `make monitor-idf`, `make flash-monitor`, and `make flash-monitor-idf` auto-detect a serial port on macOS and Linux. If more than one candidate is present, set `PORT=/dev/...` explicitly.

`make monitor` uses a plain serial monitor that exits with `Ctrl+C`. `make monitor-idf` uses the ESP-IDF monitor if you want its richer decoding behavior; that one still uses the ESP-IDF keybindings such as `Ctrl+]` to exit.

Both monitor targets use the firmware console baud rate (`115200`) by default. This is intentionally separate from the flash baud rate (`460800` by default), because using the flash baud for the serial console will produce garbled output.

If you want to use an external flasher that accepts a single image, run `make merged-bin`. This writes `build/ddc_matter_display_controller_merged.bin`, which contains the bootloader, partition table, and app image merged for flashing at offset `0x0`.

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
