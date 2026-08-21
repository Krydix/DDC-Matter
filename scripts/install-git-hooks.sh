#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

git -C "$ROOT_DIR" config --local core.hooksPath .githooks
echo "Installed repository hooks from .githooks/"
echo "Pushes now run the isolated Pages validation. Use SKIP_CI_PAGES=1 git push only for an intentional bypass."
