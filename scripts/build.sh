#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/_pio_common.sh"

pio run -e "${PIO_ENV}"

build_dir=".pio/build/${PIO_ENV}"
artifact_dir="artifacts/${PIO_ENV}"
mkdir -p "${artifact_dir}"

copy_if_exists() {
  local file="$1"
  if [[ -f "${build_dir}/${file}" ]]; then
    cp "${build_dir}/${file}" "${artifact_dir}/${file}"
  fi
}

copy_if_exists "firmware.bin"
copy_if_exists "firmware.elf"
copy_if_exists "firmware.map"
copy_if_exists "bootloader.bin"
copy_if_exists "partitions.bin"

echo "Artifacts exported to: ${artifact_dir}"
