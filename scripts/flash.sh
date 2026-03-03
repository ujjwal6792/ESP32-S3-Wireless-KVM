#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/_pio_common.sh"

UPLOAD_PORT_ARG="${1:-${UPLOAD_PORT:-}}"

if [[ -n "${UPLOAD_PORT_ARG}" ]]; then
  echo "Uploading on port: ${UPLOAD_PORT_ARG}"
  pio run -e "${PIO_ENV}" -t upload --upload-port "${UPLOAD_PORT_ARG}"
else
  echo "Uploading using PlatformIO auto-detected port"
  pio run -e "${PIO_ENV}" -t upload
fi
