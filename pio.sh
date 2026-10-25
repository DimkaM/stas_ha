#!/usr/bin/env bash
# Wrapper around the locally installed PlatformIO Core (no system packages needed).
#   ./pio.sh run          - build firmware
#   ./pio.sh run -t upload - upload over USB (/dev/ttyACM0)
#   ./pio.sh device monitor - serial log
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -x "$ROOT/.venv/bin/pio" ]]; then
  exec "$ROOT/.venv/bin/pio" "$@"
elif [[ -x "$ROOT/.piotools/bin/pio" ]]; then
  export PYTHONPATH="$ROOT/.piotools${PYTHONPATH:+:$PYTHONPATH}"
  exec "$ROOT/.piotools/bin/pio" "$@"
elif command -v pio >/dev/null 2>&1; then
  exec pio "$@"
elif command -v platformio >/dev/null 2>&1; then
  exec platformio "$@"
else
  echo "PlatformIO Core not found." >&2
  echo "Install it with:" >&2
  echo "  python3 -m pip install --break-system-packages --target .piotools 'platformio>=6.1'" >&2
  exit 127
fi