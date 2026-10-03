#!/bin/sh
# macOS / Linux launcher: sets up a private Python environment with esptool (nothing is installed system-wide)
# and runs flash.py. Usage: ./flash.sh [check|backup|flash] [options]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
PY=${PYTHON:-python3}
command -v "$PY" >/dev/null 2>&1 || { echo "Python 3 is required (https://www.python.org/downloads/)."; exit 1; }
"$PY" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' || {
  echo "Python 3.10 or newer is required (found: $("$PY" -V 2>&1))."
  echo "Install a newer Python (https://www.python.org/downloads/, or 'brew install python' on macOS) and run again,"
  echo "or point PYTHON at it:  PYTHON=/path/to/python3.12 ./flash.sh"
  exit 1
}
VENV="$HERE/.flasher-venv"
if [ ! -f "$VENV/.ready" ]; then
  echo "Setting up a private environment (one time, needs internet)..."
  rm -rf "$VENV"
  "$PY" -m venv "$VENV" || { echo "Could not create a Python virtual environment (Debian/Ubuntu: sudo apt install python3-venv)."; rm -rf "$VENV"; exit 1; }
  { "$VENV/bin/python" -m pip install --quiet --upgrade pip && "$VENV/bin/python" -m pip install --quiet --upgrade "esptool>=5,<6"; } || {
    echo "Installing esptool failed (it needs an internet connection and Python 3.10+)."; rm -rf "$VENV"; exit 1; }
  touch "$VENV/.ready"
fi
exec "$VENV/bin/python" "$HERE/flash.py" "$@"
