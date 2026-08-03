#!/usr/bin/env bash

set -euo pipefail

if [[ -z "${IDF_PATH:-}" ]]; then
  echo "detect-esp-port: IDF_PATH is not set" >&2
  exit 2
fi

if [[ -z "${IDF_PYTHON:-}" || ! -x "${IDF_PYTHON}" ]]; then
  echo "detect-esp-port: ESP-IDF Python is unavailable; run make dev-init" >&2
  exit 2
fi

esptool="${IDF_PATH}/components/esptool_py/esptool/esptool.py"
if [[ ! -f "${esptool}" ]]; then
  echo "detect-esp-port: esptool is unavailable at ${esptool}; run make dev-init" >&2
  exit 2
fi

shopt -s nullglob
candidates=()
for pattern in /dev/cu.usbserial* /dev/ttyUSB* /dev/cu.usbmodem* /dev/ttyACM*; do
  for device in ${pattern}; do
    candidates+=("${device}")
  done
done
shopt -u nullglob

if (( ${#candidates[@]} == 0 )); then
  echo "no serial ports detected; connect the ESP32 or pass PORT=/dev/..." >&2
  exit 1
fi

esp_ports=()
for device in "${candidates[@]}"; do
  echo "Checking ${device} for an ESP32..." >&2
  if "${IDF_PYTHON}" "${esptool}" \
    --chip esp32 \
    --port "${device}" \
    --baud 115200 \
    --before default_reset \
    --after hard_reset \
    --connect-attempts 2 \
    chip_id >/dev/null 2>&1; then
    esp_ports+=("${device}")
  fi
done

case "${#esp_ports[@]}" in
  1)
    printf '%s\n' "${esp_ports[0]}"
    ;;
  0)
    echo "no ESP32 responded on the detected serial ports:" >&2
    printf '  %s\n' "${candidates[@]}" >&2
    echo "Close any serial monitor, reconnect the board, or pass PORT=/dev/... explicitly." >&2
    exit 1
    ;;
  *)
    echo "multiple ESP32 devices responded; pass PORT=/dev/... to choose one:" >&2
    printf '  %s\n' "${esp_ports[@]}" >&2
    exit 1
    ;;
esac
