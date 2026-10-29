#!/usr/bin/env bash
# Sets up the Python environment for tools/emotibit.py on Linux.
# Creates .venv-tools in the repository root and installs esptool, pyserial, PlatformIO and bleak into it,
# then runs `emotibit.py check` to report what is still missing (serial permissions, brltty, git ...).
# Run:  bash tools/setup.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV="$ROOT/.venv-tools"

PY=""
for c in python3 python; do
  if command -v "$c" >/dev/null 2>&1 && "$c" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)' 2>/dev/null; then PY="$c"; break; fi
done
if [ -z "$PY" ]; then
  echo "Python 3.9 or newer was not found. Debian/Ubuntu: sudo apt install python3 python3-venv python3-pip" >&2
  exit 1
fi
if ! "$PY" -c 'import venv, ensurepip' 2>/dev/null; then
  echo "The Python venv module is missing. Debian/Ubuntu: sudo apt install python3-venv python3-pip" >&2
  exit 1
fi
command -v git >/dev/null 2>&1 || echo "WARNING: git was not found; PlatformIO needs it to fetch libraries (sudo apt install git)." >&2

if [ ! -x "$VENV/bin/python" ]; then
  echo "Creating $VENV ..."
  "$PY" -m venv "$VENV"
fi
"$VENV/bin/python" -m pip install --quiet --upgrade pip
"$VENV/bin/python" -m pip install --quiet -r "$ROOT/tools/requirements.txt"

echo
echo "Environment ready. Check the computer:"
"$VENV/bin/python" "$ROOT/tools/emotibit.py" check || true
echo
echo "Use it with:  $VENV/bin/python $ROOT/tools/emotibit.py <command>"
