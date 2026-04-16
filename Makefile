ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

-include $(ROOT_DIR)/.env.mk

DEPS_DIR ?= $(ROOT_DIR)/.deps
IDF_PATH ?= $(DEPS_DIR)/esp-idf
ESP_MATTER_PATH ?= $(DEPS_DIR)/esp-matter
PYTHON ?= python3
PY_USER_BASE ?= $(shell $(PYTHON) -m site --user-base 2>/dev/null)
DEFAULT_CMAKE_BIN_DIR := $(firstword $(wildcard $(HOME)/Library/Python/*/bin $(HOME)/.local/bin))
CMAKE_BIN_DIR ?= $(if $(DEFAULT_CMAKE_BIN_DIR),$(DEFAULT_CMAKE_BIN_DIR),$(PY_USER_BASE)/bin)
IDF_PYTHON_ENV_PATH ?= $(firstword $(sort $(wildcard $(HOME)/.espressif/python_env/idf5.4*_env)))
IDF_PYTHON ?= $(IDF_PYTHON_ENV_PATH)/bin/python
PORT ?=
IDF_BUILD_ARGS ?=

.DEFAULT_GOAL := help

define run_idf
	@bash -lc 'set -eo pipefail; \
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
		"$(IDF_PYTHON)" "$$IDF_PATH/tools/idf.py" $(1)'
endef

.PHONY: help dev-init build reconfigure clean fullclean flash monitor flash-monitor size

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
		'  make size            Show binary size report'

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
	$(call run_idf,$(if $(PORT),-p $(PORT) )flash)

monitor:
	$(call run_idf,$(if $(PORT),-p $(PORT) )monitor)

flash-monitor:
	$(call run_idf,$(if $(PORT),-p $(PORT) )flash monitor)

size:
	$(call run_idf,size)