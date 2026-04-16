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
			for pattern in /dev/cu.usbserial* /dev/cu.usbmodem* /dev/tty.usbserial* /dev/tty.usbmodem* /dev/ttyUSB* /dev/ttyACM*; do \
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
		if [ "$(2)" = "needs-port" ]; then \
			resolved_port="$$(resolve_port)"; \
			port_arg="-p $$resolved_port"; \
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
		"$(IDF_PYTHON)" "$$IDF_PATH/tools/idf.py" $$port_arg $(1)'
endef

.PHONY: help dev-init build reconfigure clean fullclean flash monitor flash-monitor size detect-port web-installer

help:
	@printf '%s\n' \
		'Available targets:' \
		'  make dev-init        Clone and bootstrap repo-local ESP-IDF and esp-matter dependencies' \
		'  make build           Build the firmware image' \
		'  make reconfigure     Re-run CMake and sdkconfig generation' \
		'  make clean           Clean build outputs' \
		'  make fullclean       Remove all generated build state' \
		'  make flash PORT=...  Flash the device' \
		'  make monitor PORT=... Open serial monitor' \
		'  make flash-monitor PORT=... Flash and then open monitor' \
		'  make detect-port     Print the auto-detected serial port' \
		'  make size            Show binary size report' \
		'  make web-installer   Stage the GitHub Pages web flasher locally' \
		'' \
		'Auto-detection:' \
		'  Uses .env.mk first, then ./ .deps, then ~/esp/esp-idf and ~/esp/esp-matter if present.' \
		'  Flash and monitor targets auto-detect a serial port if PORT is not set.'

dev-init:
	@./scripts/dev-init.sh

build:
	$(call run_idf,build $(IDF_BUILD_ARGS))

reconfigure:
	$(call run_idf,reconfigure)

clean:
	$(call run_idf,clean)

fullclean:
	$(call run_idf,fullclean)

flash:
	$(call run_idf,flash,needs-port)

monitor:
	$(call run_idf,monitor,needs-port)

flash-monitor:
	$(call run_idf,flash monitor,needs-port)

detect-port:
	@bash -lc 'set -eo pipefail; \
		shopt -s nullglob; \
		matches=(); \
		for pattern in /dev/cu.usbserial* /dev/cu.usbmodem* /dev/tty.usbserial* /dev/tty.usbmodem* /dev/ttyUSB* /dev/ttyACM*; do \
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