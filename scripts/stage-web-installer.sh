#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${1:-${ROOT_DIR}/build/web-installer}"
WEB_SRC_DIR="${ROOT_DIR}/web-installer"

APP_BIN="${ROOT_DIR}/build/ddc_matter_display_controller.bin"
BOOTLOADER_BIN="${ROOT_DIR}/build/bootloader/bootloader.bin"
PARTITION_BIN="${ROOT_DIR}/build/partition_table/partition-table.bin"
FLASH_ARGS_JSON="${ROOT_DIR}/build/flasher_args.json"

for required in "$APP_BIN" "$BOOTLOADER_BIN" "$PARTITION_BIN" "$FLASH_ARGS_JSON"; do
    if [[ ! -f "$required" ]]; then
        echo "missing build artifact: $required" >&2
        echo "run 'make build' first" >&2
        exit 1
    fi
done

mkdir -p "$OUT_DIR/firmware"
cp "$WEB_SRC_DIR/index.html" "$OUT_DIR/index.html"
cp "$WEB_SRC_DIR/manifest.json" "$OUT_DIR/manifest.json"
cp "$APP_BIN" "$OUT_DIR/firmware/ddc_matter_display_controller.bin"
cp "$BOOTLOADER_BIN" "$OUT_DIR/firmware/bootloader.bin"
cp "$PARTITION_BIN" "$OUT_DIR/firmware/partition-table.bin"
cp "$FLASH_ARGS_JSON" "$OUT_DIR/firmware/flasher_args.json"
touch "$OUT_DIR/.nojekyll"

GIT_SHA="$(git -C "$ROOT_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)"
BUILD_DATE="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

printf '{\n  "project": "DDC-Matter",\n  "version": "%s",\n  "builtAt": "%s"\n}\n' "$GIT_SHA" "$BUILD_DATE" > "$OUT_DIR/build-info.json"

echo "staged web installer in $OUT_DIR"