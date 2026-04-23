# Project Guidelines

## Architecture

- This repo is an ESP32 firmware that exposes monitor brightness, contrast, and input switching over Matter and a built-in HTTP web UI. See [README.md](README.md) for user-facing behavior, API routes, and commissioning details.
- The normal application entry point is [main/app_main.cpp](main/app_main.cpp). The standalone UART debug shell lives in [main/debug_serial_app.cpp](main/debug_serial_app.cpp) and is selected by `DDC_STANDALONE_DEBUG` in [main/CMakeLists.txt](main/CMakeLists.txt).
- Keep responsibilities separated by file: DDC/CI transport in [main/ddc.c](main/ddc.c), EDID parsing in [main/edid.c](main/edid.c), MCCS/input labels in [main/mccs.c](main/mccs.c), monitor detection/database work in [main/monitor_db.c](main/monitor_db.c), NVS persistence in [main/config.c](main/config.c), Matter setup in [main/matter.cpp](main/matter.cpp), and HTTP routes in [main/webserver.c](main/webserver.c).
- The web UI is embedded from [frontend/index.html](frontend/index.html) through `EMBED_FILES`; frontend edits require rebuilding the firmware.

## Build And Validation

- Prefer the repo `make` targets over raw `idf.py` or VS Code CMake tools. The checked-in Makefile bootstraps ESP-IDF, esp-matter, Python, CMake, serial-port selection, and the correct build directories.
- Standard workflow: `make dev-init`, `make build`, `make flash`, `make monitor`.
- Standalone DDC debug workflow: `make build-debug`, `make flash-debug`, `make monitor`.
- `build/` and `build-debug/` are generated outputs. Do not edit them.
- Serial monitor must run at `115200` even though flashing usually runs at `460800`.
- If flashing reports the serial port is busy, stop any active `make monitor` session first.

## Project Conventions

- Do not start or move non-Matter services earlier in boot without checking commissioning behavior. This repo intentionally defers DDC polling, monitor detection, and the web server until commissioning completes or an existing fabric is present.
- The web UI hostname `display-switcher.local` is supported, but only through Matter's shared ESP-IDF `mdns` backend. Keep `CONFIG_USE_MINIMAL_MDNS=n`, publish `_http._tcp` on the same responder Matter uses, and do not introduce a second mDNS stack beside Matter.
- App-side web mDNS publication is init-order-sensitive: register IP event handlers only after Matter/network initialization has created the default event loop and shared responder state.
- Treat the first IP event as potentially early for web mDNS publication. If delegated hostname or `_http._tcp` registration fails before Matter finishes initializing mDNS, retry later instead of aborting boot or requiring a manual retrigger.
- Matter brightness and contrast are exposed as Level Control values on a `0..254` scale, but DDC VCP writes in this codebase use `0..100` semantics. Preserve the mapping when touching Matter or web UI level code.
- Input handling is monitor-specific. Standard input switching uses VCP `0x60`, but LG displays may require destination `0x50` with VCP `0xF4`, and current-input detection may fall back to LG fingerprint VCP `0xF8` when `0x60` returns unusable data.
- Config persistence is compatibility-sensitive. When changing saved config structures or input slot counts, treat old blobs as recoverable rather than aborting boot.

## Practical Defaults For Future Chats

- Start code reading from the owning module above rather than broad repo exploration.
- Use [README.md](README.md) as the primary product and operations reference; use the Makefile for the authoritative command surface.
- When validating firmware changes, prefer a focused `make build` and, if hardware access is relevant, `make flash` plus `make monitor`.