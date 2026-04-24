ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

-include $(ROOT_DIR)/.env.mk

BUILD_DIR ?= $(ROOT_DIR)/build
DEBUG_BUILD_DIR ?= $(ROOT_DIR)/build-debug
DEPS_DIR ?= $(ROOT_DIR)/.deps
DEFAULT_IDF_PATH := $(firstword $(wildcard $(ROOT_DIR)/.deps/esp-idf $(HOME)/esp/esp-idf))
DEFAULT_ESP_MATTER_PATH := $(firstword $(wildcard $(ROOT_DIR)/.deps/esp-matter $(HOME)/esp/esp-matter))
IDF_PATH ?= $(if $(DEFAULT_IDF_PATH),$(DEFAULT_IDF_PATH),$(DEPS_DIR)/esp-idf)
ESP_MATTER_PATH ?= $(if $(DEFAULT_ESP_MATTER_PATH),$(DEFAULT_ESP_MATTER_PATH),$(DEPS_DIR)/esp-matter)
PYTHON ?= python3
PY_USER_BASE ?= $(shell $(PYTHON) -m site --user-base 2>/dev/null)
DEFAULT_CMAKE_BIN_DIR := $(firstword $(wildcard $(HOME)/Library/Python/*/bin $(HOME)/.local/bin))
CMAKE_BIN_DIR ?= $(if $(DEFAULT_CMAKE_BIN_DIR),$(DEFAULT_CMAKE_BIN_DIR),$(PY_USER_BASE)/bin)
IDF_PYTHON_ENV_PATH ?= $(firstword $(wildcard \
	$(HOME)/.espressif/python_env/idf5.4_py3.1[5-9]_env \
	$(HOME)/.espressif/python_env/idf5.4_py3.14_env \
	$(HOME)/.espressif/python_env/idf5.4_py3.13_env \
	$(HOME)/.espressif/python_env/idf5.4_py3.12_env \
	$(HOME)/.espressif/python_env/idf5.4_py3.11_env \
	$(HOME)/.espressif/python_env/idf5.4_py3.10_env \
	$(HOME)/.espressif/python_env/idf5.4_py3.[0-9]_env \
	$(HOME)/.espressif/python_env/idf5.4*_env))
IDF_PYTHON ?= $(IDF_PYTHON_ENV_PATH)/bin/python
PORT ?=
IDF_BUILD_ARGS ?=
WEB_INSTALLER_DIR ?= $(ROOT_DIR)/build/web-installer
MERGED_BIN ?= $(ROOT_DIR)/build/ddc_matter_display_controller_merged.bin
FLASH_BAUD ?= 460800
MONITOR_BAUD ?= 115200
FLASH_BEFORE ?= default_reset
FLASH_AFTER ?= hard_reset
DEBUG_BUILD_FLAG := -D DDC_STANDALONE_DEBUG=ON
DEFAULT_NVS_OFFSET := $(shell awk -F, '$$1=="nvs"{gsub(/[[:space:]]/, "", $$4); print $$4}' "$(ROOT_DIR)/partitions.csv")
DEFAULT_NVS_SIZE := $(shell awk -F, '$$1=="nvs"{gsub(/[[:space:]]/, "", $$5); print $$5}' "$(ROOT_DIR)/partitions.csv")
NVS_OFFSET ?= $(if $(DEFAULT_NVS_OFFSET),$(DEFAULT_NVS_OFFSET),0x9000)
NVS_SIZE ?= $(if $(DEFAULT_NVS_SIZE),$(DEFAULT_NVS_SIZE),0x6000)

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
		"$(IDF_PYTHON)" "$$IDF_PATH/tools/idf.py" -B "$(BUILD_DIR)" $$port_arg $$baud_arg $(1)'
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
		"$(IDF_PYTHON)" "$$IDF_PATH/tools/idf.py" -B "$(BUILD_DIR)" -p "$$resolved_port" monitor -b $(MONITOR_BAUD)'
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
		test -f "$(BUILD_DIR)/flash_args" || { echo "missing $(BUILD_DIR)/flash_args; run make build first"; exit 1; }; \
		. "$$IDF_PATH/export.sh" >/dev/null 2>&1; \
		cd "$(BUILD_DIR)"; \
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
		test -f "$(BUILD_DIR)/bootloader/bootloader.bin" || { echo "missing bootloader binary; run make build first"; exit 1; }; \
		test -f "$(BUILD_DIR)/partition_table/partition-table.bin" || { echo "missing partition table binary; run make build first"; exit 1; }; \
		test -f "$(BUILD_DIR)/ddc_matter_display_controller.bin" || { echo "missing app binary; run make build first"; exit 1; }; \
		. "$$IDF_PATH/export.sh" >/dev/null 2>&1; \
		"$(IDF_PYTHON)" "$$IDF_PATH/components/esptool_py/esptool/esptool.py" \
			--chip esp32 \
			merge_bin \
			-o "$(MERGED_BIN)" \
			--flash_mode dio \
			--flash_freq 40m \
			--flash_size 4MB \
			0x1000 "$(BUILD_DIR)/bootloader/bootloader.bin" \
			0x8000 "$(BUILD_DIR)/partition_table/partition-table.bin" \
			0x10000 "$(BUILD_DIR)/ddc_matter_display_controller.bin"; \
		echo "Wrote $(MERGED_BIN)"'
endef

.PHONY: help dev-init build build-debug merged-bin reconfigure clean clean-debug fullclean fullclean-debug flash flash-safe flash-manual flash-manual-run flash-debug flash-safe-debug erase-flash erase-nvs fresh-flash probe probe-manual monitor monitor-idf flash-monitor flash-monitor-idf flash-monitor-debug size detect-port web-installer ci-pages

help:
	@printf '%s\n' \
		'Available targets:' \
		'  make dev-init        Clone and bootstrap repo-local ESP-IDF and esp-matter dependencies' \
		'  make build           Build the firmware image' \
		'  make build-debug     Build the standalone serial DDC debug image in build-debug/' \
		'  make merged-bin      Create a single merged flash image for external tools' \
		'  make reconfigure     Re-run CMake and sdkconfig generation' \
		'  make clean           Clean build outputs' \
		'  make clean-debug     Clean the standalone debug build directory' \
		'  make fullclean       Remove all generated build state' \
		'  make fullclean-debug Remove the standalone debug build directory' \
		'  make flash PORT=...  Flash the device using IDF defaults and FLASH_BAUD' \
		'  make flash-debug     Build and flash the standalone serial DDC debug image' \
		'  make flash-safe-debug Flash the debug image at 115200 baud' \
		'  make flash-safe      Flash at 115200 baud to reduce link/reset issues' \
		'  make flash-manual    Flash without auto-reset; put the ESP32 in bootloader mode first' \
		'  make erase-flash     Erase the entire ESP32 flash chip (Wi-Fi, Matter, app config, firmware)' \
		'  make erase-nvs       Erase only the NVS partition (Wi-Fi, Matter, app config)' \
		'  make fresh-flash     Erase the entire chip and flash the current firmware again' \
		'  make probe           Read chip info over serial using esptool' \
		'  make probe-manual    Probe chip info after manually entering bootloader mode' \
		'  make monitor PORT=... Open serial monitor (Ctrl+C exits)' \
		'  make monitor-idf     Open ESP-IDF monitor (Ctrl+] exits)' \
		'  make flash-monitor PORT=... Flash and then open monitor' \
		'  make flash-monitor-idf PORT=... Flash and then open ESP-IDF monitor' \
		'  make flash-monitor-debug Flash the debug image and then open the plain serial monitor' \
		'  make detect-port     Print the auto-detected serial port' \
		'  make size            Show binary size report' \
		'  make web-installer   Stage the GitHub Pages web flasher locally' \
		'  make ci-pages        Simulate the GitHub Pages workflow in .ci-pages/' \
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

build-debug:
	$(MAKE) build BUILD_DIR=$(DEBUG_BUILD_DIR) IDF_BUILD_ARGS='$(strip $(IDF_BUILD_ARGS) $(DEBUG_BUILD_FLAG))'

merged-bin:
	$(call run_esptool_merge)

reconfigure:
	$(call run_idf,reconfigure)

clean:
	$(call run_idf,clean)

clean-debug:
	$(MAKE) clean BUILD_DIR=$(DEBUG_BUILD_DIR) IDF_BUILD_ARGS='$(strip $(DEBUG_BUILD_FLAG))'

fullclean:
	$(call run_idf,fullclean)

fullclean-debug:
	@rm -rf "$(DEBUG_BUILD_DIR)"

flash:
	$(call run_idf,flash,needs-port)

flash-debug:
	$(MAKE) flash BUILD_DIR=$(DEBUG_BUILD_DIR) IDF_BUILD_ARGS='$(strip $(IDF_BUILD_ARGS) $(DEBUG_BUILD_FLAG))' PORT=$(PORT) FLASH_BAUD=$(FLASH_BAUD) FLASH_BEFORE=$(FLASH_BEFORE) FLASH_AFTER=$(FLASH_AFTER)

flash-safe:
	$(MAKE) flash FLASH_BAUD=115200

flash-safe-debug:
	$(MAKE) flash-debug FLASH_BAUD=115200 BUILD_DIR=$(DEBUG_BUILD_DIR)

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

erase-flash:
	$(call run_esptool_command,erase_flash)

erase-nvs:
	$(call run_esptool_command,erase_region $(NVS_OFFSET) $(NVS_SIZE))

fresh-flash:
	$(MAKE) erase-flash PORT=$(PORT) FLASH_BAUD=$(FLASH_BAUD) FLASH_BEFORE=$(FLASH_BEFORE) FLASH_AFTER=$(FLASH_AFTER)
	$(MAKE) flash PORT=$(PORT) FLASH_BAUD=$(FLASH_BAUD) FLASH_BEFORE=$(FLASH_BEFORE) FLASH_AFTER=$(FLASH_AFTER)

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

flash-monitor-debug:
	$(MAKE) flash-debug PORT=$(PORT) FLASH_BAUD=$(FLASH_BAUD) FLASH_BEFORE=$(FLASH_BEFORE) FLASH_AFTER=$(FLASH_AFTER) BUILD_DIR=$(DEBUG_BUILD_DIR)
	$(MAKE) monitor PORT=$(PORT) MONITOR_BAUD=$(MONITOR_BAUD)

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

ci-pages:
	@./scripts/run-pages-workflow-local.sh