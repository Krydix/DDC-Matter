ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

-include $(ROOT_DIR)/.env.mk

DEPS_DIR ?= $(ROOT_DIR)/.deps
DEFAULT_IDF_PATH := $(firstword $(wildcard $(ROOT_DIR)/.deps/esp-idf $(HOME)/esp/esp-idf))
DEFAULT_ESP_MATTER_PATH := $(firstword $(wildcard $(ROOT_DIR)/.deps/esp-matter $(HOME)/esp/esp-matter))
IDF_PATH ?= $(if $(DEFAULT_IDF_PATH),$(DEFAULT_IDF_PATH),$(DEPS_DIR)/esp-idf)
ESP_MATTER_PATH ?= $(if $(DEFAULT_ESP_MATTER_PATH),$(DEFAULT_ESP_MATTER_PATH),$(DEPS_DIR)/esp-matter)
PYTHON ?= python3
PY_USER_BASE ?= $(shell $(PYTHON) -m site --user-base 2>/dev/null)
DEFAULT_CMAKE_BIN_DIR := $(firstword $(wildcard $(HOME)/Library/Python/*/bin $(HOME)/.local/bin))
CMAKE_BIN_DIR ?= $(if $(DEFAULT_CMAKE_BIN_DIR),$(DEFAULT_CMAKE_BIN_DIR),$(PY_USER_BASE)/bin)
IDF_PYTHON_ENV_PATH ?= $(firstword $(sort $(wildcard $(HOME)/.espressif/python_env/idf5.4*_env)))
IDF_PYTHON ?= $(IDF_PYTHON_ENV_PATH)/bin/python
PORT ?=
IDF_BUILD_ARGS ?=
WEB_INSTALLER_DIR ?= $(ROOT_DIR)/build/web-installer
MERGED_BIN ?= $(ROOT_DIR)/build/ddc_matter_display_controller_merged.bin
FLASH_BAUD ?= 460800
MONITOR_BAUD ?= 115200
FLASH_BEFORE ?= default_reset
FLASH_AFTER ?= hard_reset

.DEFAULT_GOAL := help

define run_idf
	@bash -lc 'set -eo pipefail; \
		resolve_port() { \
			if [ -n "$(PORT)" ]; then \
				printf "%s" "$(PORT)"; \
				return 0; \
			fi; \
			shopt -s nullglob; \
			matches=(); \
			for pattern in /dev/cu.usbserial* /dev/cu.usbmodem* /dev/ttyUSB* /dev/ttyACM*; do \
				for device in $$pattern; do \
					matches+=("$$device"); \
				done; \
			done; \
			shopt -u nullglob; \
			case "$${#matches[@]}" in \
				0) echo "no serial port detected; pass PORT=/dev/..." >&2; return 1 ;; \
				1) printf "%s" "$${matches[0]}" ;; \
				*) echo "multiple serial ports detected; pass PORT=/dev/..." >&2; printf "  %s\n" "$${matches[@]}" >&2; return 1 ;; \
			esac; \
		}; \
		port_arg=""; \
		baud_arg=""; \
		if [ "$(2)" = "needs-port" ]; then \
			resolved_port="$$(resolve_port)"; \
			port_arg="-p $$resolved_port"; \
			baud_arg="-b $(FLASH_BAUD)"; \
			echo "Using serial port $$resolved_port"; \
		fi; \
		export IDF_PATH="$(IDF_PATH)"; \
		export ESP_MATTER_PATH="$(ESP_MATTER_PATH)"; \
		export IDF_PYTHON_ENV_PATH="$(IDF_PYTHON_ENV_PATH)"; \
		export PATH="$(CMAKE_BIN_DIR):$$PATH"; \
		test -f "$$IDF_PATH/export.sh" || { echo "missing ESP-IDF at $$IDF_PATH; run make dev-init"; exit 1; }; \
		test -f "$$ESP_MATTER_PATH/connectedhomeip/connectedhomeip/.environment/activate.sh" || { echo "missing esp-matter bootstrap at $$ESP_MATTER_PATH; run make dev-init"; exit 1; }; \
		test -x "$(IDF_PYTHON)" || { echo "missing ESP-IDF python at $(IDF_PYTHON); run make dev-init"; exit 1; }; \
		. "$$ESP_MATTER_PATH/connectedhomeip/connectedhomeip/.environment/activate.sh" >/dev/null 2>&1; \
		. "$$IDF_PATH/export.sh" >/dev/null 2>&1; \
		export PATH="$(CMAKE_BIN_DIR):$$PATH"; \
		cd "$(ROOT_DIR)"; \
		"$(IDF_PYTHON)" "$$IDF_PATH/tools/idf.py" $$port_arg $$baud_arg $(1)'
endef

define run_idf_monitor
	@bash -lc 'set -eo pipefail; \
		resolve_port() { \
			if [ -n "$(PORT)" ]; then \
				printf "%s" "$(PORT)"; \
				return 0; \
			fi; \
			shopt -s nullglob; \
			matches=(); \
			for pattern in /dev/cu.usbserial* /dev/cu.usbmodem* /dev/ttyUSB* /dev/ttyACM*; do \
				for device in $$pattern; do \
					matches+=("$$device"); \
				done; \
			done; \
			shopt -u nullglob; \
			case "$${#matches[@]}" in \
				0) echo "no serial port detected; pass PORT=/dev/..." >&2; return 1 ;; \
				1) printf "%s" "$${matches[0]}" ;; \
				*) echo "multiple serial ports detected; pass PORT=/dev/..." >&2; printf "  %s\n" "$${matches[@]}" >&2; return 1 ;; \
			esac; \
		}; \
		resolved_port="$$(resolve_port)"; \
		echo "Using serial port $$resolved_port"; \
		export IDF_PATH="$(IDF_PATH)"; \
		export ESP_MATTER_PATH="$(ESP_MATTER_PATH)"; \
		export IDF_PYTHON_ENV_PATH="$(IDF_PYTHON_ENV_PATH)"; \
		export PATH="$(CMAKE_BIN_DIR):$$PATH"; \
		test -f "$$IDF_PATH/export.sh" || { echo "missing ESP-IDF at $$IDF_PATH; run make dev-init"; exit 1; }; \
		test -f "$$ESP_MATTER_PATH/connectedhomeip/connectedhomeip/.environment/activate.sh" || { echo "missing esp-matter bootstrap at $$ESP_MATTER_PATH; run make dev-init"; exit 1; }; \
		test -x "$(IDF_PYTHON)" || { echo "missing ESP-IDF python at $(IDF_PYTHON); run make dev-init"; exit 1; }; \
		. "$$ESP_MATTER_PATH/connectedhomeip/connectedhomeip/.environment/activate.sh" >/dev/null 2>&1; \
		. "$$IDF_PATH/export.sh" >/dev/null 2>&1; \
		export PATH="$(CMAKE_BIN_DIR):$$PATH"; \
		cd "$(ROOT_DIR)"; \
		"$(IDF_PYTHON)" "$$IDF_PATH/tools/idf.py" -p "$$resolved_port" monitor -b $(MONITOR_BAUD)'
endef

define run_plain_monitor
	@bash -lc 'set -eo pipefail; \
		resolve_port() { \
			if [ -n "$(PORT)" ]; then \
				printf "%s" "$(PORT)"; \
				return 0; \
			fi; \
			shopt -s nullglob; \
			matches=(); \
			for pattern in /dev/cu.usbserial* /dev/cu.usbmodem* /dev/ttyUSB* /dev/ttyACM*; do \
				for device in $$pattern; do \
					matches+=("$$device"); \
				done; \
			done; \
			shopt -u nullglob; \
			case "$${#matches[@]}" in \
				0) echo "no serial port detected; pass PORT=/dev/..." >&2; return 1 ;; \
				1) printf "%s" "$${matches[0]}" ;; \
				*) echo "multiple serial ports detected; pass PORT=/dev/..." >&2; printf "  %s\n" "$${matches[@]}" >&2; return 1 ;; \
			esac; \
		}; \
		resolved_port="$$(resolve_port)"; \
		echo "Using serial port $$resolved_port"; \
		echo "Press Ctrl+C to exit"; \
		export IDF_PATH="$(IDF_PATH)"; \
		export IDF_PYTHON_ENV_PATH="$(IDF_PYTHON_ENV_PATH)"; \
		export PATH="$(CMAKE_BIN_DIR):$$PATH"; \
		test -x "$(IDF_PYTHON)" || { echo "missing ESP-IDF python at $(IDF_PYTHON); run make dev-init"; exit 1; }; \
		exec "$(IDF_PYTHON)" -m serial.tools.miniterm "$$resolved_port" "$(MONITOR_BAUD)" --raw --exit-char 3'
endef

define run_esptool_flash
	@bash -lc 'set -eo pipefail; \
		resolve_port() { \
			if [ -n "$(PORT)" ]; then \
				printf "%s" "$(PORT)"; \
				return 0; \
			fi; \
			shopt -s nullglob; \
			matches=(); \
			for pattern in /dev/cu.usbserial* /dev/cu.usbmodem* /dev/ttyUSB* /dev/ttyACM*; do \
				for device in $$pattern; do \
					matches+=("$$device"); \
				done; \
			done; \
			shopt -u nullglob; \
			case "$${#matches[@]}" in \
				0) echo "no serial port detected; pass PORT=/dev/..." >&2; return 1 ;; \
				1) printf "%s" "$${matches[0]}" ;; \
				*) echo "multiple serial ports detected; pass PORT=/dev/..." >&2; printf "  %s\n" "$${matches[@]}" >&2; return 1 ;; \
			esac; \
		}; \
		resolved_port="$$(resolve_port)"; \
		echo "Using serial port $$resolved_port"; \
		export IDF_PATH="$(IDF_PATH)"; \
		export ESP_MATTER_PATH="$(ESP_MATTER_PATH)"; \
		export IDF_PYTHON_ENV_PATH="$(IDF_PYTHON_ENV_PATH)"; \
		export PATH="$(CMAKE_BIN_DIR):$$PATH"; \
		test -f "$$IDF_PATH/export.sh" || { echo "missing ESP-IDF at $$IDF_PATH; run make dev-init"; exit 1; }; \
		test -x "$(IDF_PYTHON)" || { echo "missing ESP-IDF python at $(IDF_PYTHON); run make dev-init"; exit 1; }; \
		test -f "$(ROOT_DIR)/build/flash_args" || { echo "missing build/flash_args; run make build first"; exit 1; }; \
		. "$$IDF_PATH/export.sh" >/dev/null 2>&1; \
		cd "$(ROOT_DIR)/build"; \
		"$(IDF_PYTHON)" "$$IDF_PATH/components/esptool_py/esptool/esptool.py" \
			--chip esp32 \
			-p "$$resolved_port" \
			-b "$(FLASH_BAUD)" \
			--before "$(FLASH_BEFORE)" \
			--after "$(FLASH_AFTER)" \
			write_flash @flash_args'
endef

define run_esptool_command
	@bash -lc 'set -eo pipefail; \
		resolve_port() { \
			if [ -n "$(PORT)" ]; then \
				printf "%s" "$(PORT)"; \
				return 0; \
			fi; \
			shopt -s nullglob; \
			matches=(); \
			for pattern in /dev/cu.usbserial* /dev/cu.usbmodem* /dev/ttyUSB* /dev/ttyACM*; do \
				for device in $$pattern; do \
					matches+=("$$device"); \
				done; \
			done; \
			shopt -u nullglob; \
			case "$${#matches[@]}" in \
				0) echo "no serial port detected; pass PORT=/dev/..." >&2; return 1 ;; \
				1) printf "%s" "$${matches[0]}" ;; \
				*) echo "multiple serial ports detected; pass PORT=/dev/..." >&2; printf "  %s\n" "$${matches[@]}" >&2; return 1 ;; \
			esac; \
		}; \
		resolved_port="$$(resolve_port)"; \
		echo "Using serial port $$resolved_port"; \
		export IDF_PATH="$(IDF_PATH)"; \
		export IDF_PYTHON_ENV_PATH="$(IDF_PYTHON_ENV_PATH)"; \
		export PATH="$(CMAKE_BIN_DIR):$$PATH"; \
		test -f "$$IDF_PATH/export.sh" || { echo "missing ESP-IDF at $$IDF_PATH; run make dev-init"; exit 1; }; \
		test -x "$(IDF_PYTHON)" || { echo "missing ESP-IDF python at $(IDF_PYTHON); run make dev-init"; exit 1; }; \
		. "$$IDF_PATH/export.sh" >/dev/null 2>&1; \
		"$(IDF_PYTHON)" "$$IDF_PATH/components/esptool_py/esptool/esptool.py" \
			--chip esp32 \
			-p "$$resolved_port" \
			-b "$(FLASH_BAUD)" \
			--before "$(FLASH_BEFORE)" \
			--after "$(FLASH_AFTER)" \
			$(1)'
endef

define run_esptool_merge
	@bash -lc 'set -eo pipefail; \
		export IDF_PATH="$(IDF_PATH)"; \
		export IDF_PYTHON_ENV_PATH="$(IDF_PYTHON_ENV_PATH)"; \
		export PATH="$(CMAKE_BIN_DIR):$$PATH"; \
		test -f "$$IDF_PATH/export.sh" || { echo "missing ESP-IDF at $$IDF_PATH; run make dev-init"; exit 1; }; \
		test -x "$(IDF_PYTHON)" || { echo "missing ESP-IDF python at $(IDF_PYTHON); run make dev-init"; exit 1; }; \
		test -f "$(ROOT_DIR)/build/bootloader/bootloader.bin" || { echo "missing bootloader binary; run make build first"; exit 1; }; \
		test -f "$(ROOT_DIR)/build/partition_table/partition-table.bin" || { echo "missing partition table binary; run make build first"; exit 1; }; \
		test -f "$(ROOT_DIR)/build/ddc_matter_display_controller.bin" || { echo "missing app binary; run make build first"; exit 1; }; \
		. "$$IDF_PATH/export.sh" >/dev/null 2>&1; \
		"$(IDF_PYTHON)" "$$IDF_PATH/components/esptool_py/esptool/esptool.py" \
			--chip esp32 \
			merge_bin \
			-o "$(MERGED_BIN)" \
			--flash_mode dio \
			--flash_freq 40m \
			--flash_size 4MB \
			0x1000 "$(ROOT_DIR)/build/bootloader/bootloader.bin" \
			0x8000 "$(ROOT_DIR)/build/partition_table/partition-table.bin" \
			0x10000 "$(ROOT_DIR)/build/ddc_matter_display_controller.bin"; \
		echo "Wrote $(MERGED_BIN)"'
endef

.PHONY: help dev-init build merged-bin reconfigure clean fullclean flash flash-safe flash-manual flash-manual-run probe probe-manual monitor monitor-idf flash-monitor flash-monitor-idf size detect-port web-installer

help:
	@printf '%s\n' \
		'Available targets:' \
		'  make dev-init        Clone and bootstrap repo-local ESP-IDF and esp-matter dependencies' \
		'  make build           Build the firmware image' \
		'  make merged-bin      Create a single merged flash image for external tools' \
		'  make reconfigure     Re-run CMake and sdkconfig generation' \
		'  make clean           Clean build outputs' \
		'  make fullclean       Remove all generated build state' \
		'  make flash PORT=...  Flash the device using IDF defaults and FLASH_BAUD' \
		'  make flash-safe      Flash at 115200 baud to reduce link/reset issues' \
		'  make flash-manual    Flash without auto-reset; put the ESP32 in bootloader mode first' \
		'  make probe           Read chip info over serial using esptool' \
		'  make probe-manual    Probe chip info after manually entering bootloader mode' \
		'  make monitor PORT=... Open serial monitor (Ctrl+C exits)' \
		'  make monitor-idf     Open ESP-IDF monitor (Ctrl+] exits)' \
		'  make flash-monitor PORT=... Flash and then open monitor' \
		'  make flash-monitor-idf PORT=... Flash and then open ESP-IDF monitor' \
		'  make detect-port     Print the auto-detected serial port' \
		'  make size            Show binary size report' \
		'  make web-installer   Stage the GitHub Pages web flasher locally' \
		'' \
		'Auto-detection:' \
		'  Uses .env.mk first, then ./ .deps, then ~/esp/esp-idf and ~/esp/esp-matter if present.' \
		'  Flash and monitor targets auto-detect a serial port if PORT is not set.' \
		'' \
		'Flash tuning variables:' \
		'  FLASH_BAUD=460800 MONITOR_BAUD=115200 FLASH_BEFORE=default_reset FLASH_AFTER=hard_reset' \
		'  Example manual fallback: make flash-manual PORT=/dev/cu.usbserial-00000000'

dev-init:
	@./scripts/dev-init.sh

build:
	$(call run_idf,build $(IDF_BUILD_ARGS))

merged-bin:
	$(call run_esptool_merge)

reconfigure:
	$(call run_idf,reconfigure)

clean:
	$(call run_idf,clean)

fullclean:
	$(call run_idf,fullclean)

flash:
	$(call run_idf,flash,needs-port)

flash-safe:
	$(MAKE) flash FLASH_BAUD=115200

flash-manual:
	@printf '%s\n' \
		'Put the ESP32 into download mode manually before continuing:' \
		'  1. Hold BOOT' \
		'  2. Press and release EN/RESET' \
		'  3. Release BOOT' \
		'Running esptool with --before no_reset --after no_reset.'
	$(MAKE) FLASH_BAUD=115200 FLASH_BEFORE=no_reset FLASH_AFTER=no_reset flash-manual-run

flash-manual-run:
	$(call run_esptool_flash)

probe:
	$(call run_esptool_command,chip_id)

probe-manual:
	@printf '%s\n' \
		'Put the ESP32 into download mode manually before continuing:' \
		'  1. Hold BOOT' \
		'  2. Press and release EN/RESET' \
		'  3. Release BOOT' \
		'Running esptool chip_id with --before no_reset --after no_reset.'
	$(MAKE) FLASH_BAUD=115200 FLASH_BEFORE=no_reset FLASH_AFTER=no_reset probe

monitor:
	$(call run_plain_monitor)

monitor-idf:
	$(call run_idf_monitor)

flash-monitor:
	$(MAKE) flash PORT=$(PORT) FLASH_BAUD=$(FLASH_BAUD) FLASH_BEFORE=$(FLASH_BEFORE) FLASH_AFTER=$(FLASH_AFTER)
	$(MAKE) monitor PORT=$(PORT) MONITOR_BAUD=$(MONITOR_BAUD)

flash-monitor-idf:
	$(MAKE) flash PORT=$(PORT) FLASH_BAUD=$(FLASH_BAUD) FLASH_BEFORE=$(FLASH_BEFORE) FLASH_AFTER=$(FLASH_AFTER)
	$(MAKE) monitor-idf PORT=$(PORT) MONITOR_BAUD=$(MONITOR_BAUD)

detect-port:
	@bash -lc 'set -eo pipefail; \
		shopt -s nullglob; \
		matches=(); \
		for pattern in /dev/cu.usbserial* /dev/cu.usbmodem* /dev/ttyUSB* /dev/ttyACM*; do \
			for device in $$pattern; do \
				matches+=("$$device"); \
			done; \
		done; \
		shopt -u nullglob; \
		case "$${#matches[@]}" in \
			0) echo "no serial port detected"; exit 1 ;; \
			1) printf "%s\n" "$${matches[0]}" ;; \
			*) echo "multiple serial ports detected:"; printf "  %s\n" "$${matches[@]}"; exit 1 ;; \
		esac'

size:
	$(call run_idf,size)

web-installer:
	@./scripts/stage-web-installer.sh "$(WEB_INSTALLER_DIR)"