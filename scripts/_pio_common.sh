#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

if ! command -v pio >/dev/null 2>&1; then
  echo "Error: PlatformIO CLI ('pio') not found in PATH."
  exit 1
fi

default_env="$(grep -E '^\[env:' platformio.ini | head -n1 | sed -E 's/^\[env:(.*)\]$/\1/')"
if [[ -z "${default_env}" ]]; then
  echo "Error: could not detect PlatformIO environment from platformio.ini"
  exit 1
fi

PIO_ENV="${PIO_ENV:-${default_env}}"

echo "Using PlatformIO environment: ${PIO_ENV}"
