#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATE_DIR="${CI_PAGES_STATE_DIR:-${ROOT_DIR}/.ci-pages}"
WORK_DIR="${CI_PAGES_WORK_DIR:-${STATE_DIR}/worktree}"
DEPS_DIR="${CI_PAGES_DEPS_DIR:-${STATE_DIR}/deps}"
BIN_DIR="${CI_PAGES_BIN_DIR:-${STATE_DIR}/bin}"
PYTHON_BIN="${PYTHON_BIN:-}"
FRESH_DEPS="${CI_PAGES_FRESH_DEPS:-0}"

log() {
    printf '==> %s\n' "$*"
}

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || fail "missing required command: $1"
}

check_python() {
    if [[ -z "$PYTHON_BIN" ]]; then
        PYTHON_BIN="$(command -v python3.11 || true)"
    fi

    if [[ -z "$PYTHON_BIN" ]] && command -v brew >/dev/null 2>&1; then
        BREW_PYTHON_PREFIX="$(brew --prefix python@3.11 2>/dev/null || true)"
        if [[ -n "$BREW_PYTHON_PREFIX" ]] && [[ -x "$BREW_PYTHON_PREFIX/bin/python3.11" ]]; then
            PYTHON_BIN="$BREW_PYTHON_PREFIX/bin/python3.11"
        fi
    fi

    [[ -n "$PYTHON_BIN" ]] || fail "python3.11 not found; install it or set PYTHON_BIN=/path/to/python3.11"
    "$PYTHON_BIN" - <<'PY'
import sys

if sys.version_info[:2] != (3, 11):
    raise SystemExit(f"expected Python 3.11, got {sys.version.split()[0]}")
PY
}

prepare_python_shims() {
    rm -rf "$BIN_DIR"
    mkdir -p "$BIN_DIR"
    ln -sf "$PYTHON_BIN" "$BIN_DIR/python3"
    ln -sf "$PYTHON_BIN" "$BIN_DIR/python"
}

sync_workspace() {
    rm -rf "$WORK_DIR"
    mkdir -p "$WORK_DIR" "$DEPS_DIR"

    if command -v rsync >/dev/null 2>&1; then
        rsync -a \
            --delete \
            --exclude '.ci-pages/' \
            --exclude '.deps/' \
            --exclude '.env.mk' \
            --exclude '.git/' \
            --exclude '.venv/' \
            --exclude 'build/' \
            --exclude 'build-debug/' \
            "$ROOT_DIR/" "$WORK_DIR/"
        return
    fi

    fail "rsync is required to prepare the isolated Pages workspace"
}

need_cmd git
need_cmd make
need_cmd ninja
check_python
prepare_python_shims

export PATH="$BIN_DIR:$PATH"
export PYTHON_BIN

if [[ "$FRESH_DEPS" == "1" ]]; then
    log "Removing cached CI dependencies"
    rm -rf "$DEPS_DIR"
fi

log "Syncing current workspace into $WORK_DIR"
sync_workspace

cd "$WORK_DIR"

log "Installing user-local CMake < 4"
"$PYTHON_BIN" -m pip install --user --upgrade 'cmake<4'

log "Bootstrapping dependencies (matches GitHub Pages workflow)"
DEPS_DIR="$DEPS_DIR" PYTHON_BIN="$PYTHON_BIN" make dev-init

log "Building firmware in isolated Pages workspace"
make build

log "Staging GitHub Pages web installer"
SOURCE_GIT_ROOT="$ROOT_DIR" make web-installer

printf '%s\n' \
    "Pages simulation completed." \
    "  Workspace: $WORK_DIR" \
    "  Dependencies: $DEPS_DIR" \
    "  Installer: $WORK_DIR/build/web-installer"