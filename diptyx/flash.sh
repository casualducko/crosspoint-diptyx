#!/bin/sh
# macOS / Linux launcher: sets up a private Python environment with esptool (nothing is installed system-wide)
# and runs flash.py. Usage: ./flash.sh [check|backup|flash] [options]
set -e
cd "$(dirname "$0")"
PY=${PYTHON:-python3}
command -v "$PY" >/dev/null 2>&1 || { echo "Python 3 is required (https://www.python.org/downloads/)."; exit 1; }
if [ ! -x .flasher-venv/bin/python ]; then
  echo "Setting up a private environment (one time, needs internet)..."
  "$PY" -m venv .flasher-venv
  .flasher-venv/bin/python -m pip install --quiet --upgrade pip
  .flasher-venv/bin/python -m pip install --quiet --upgrade "esptool>=5"
fi
exec .flasher-venv/bin/python flash.py "$@"
