#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

echo "Generating compile_commands.json via PlatformIO..."
pio run -t compiledb

if [ -f compile_commands.json ]; then
  echo "Wrote $ROOT_DIR/compile_commands.json"
else
  echo "compile_commands.json was not generated" >&2
  exit 1
fi

