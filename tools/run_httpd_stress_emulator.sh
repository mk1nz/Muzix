#!/bin/bash
# Run httpd stress test against the Muzix emulator end-to-end:
#   1. Build ROM (if not already built)
#   2. Start emulator in background, launch httpd via shell stdin
#   3. Wait for httpd to listen on port 80
#   4. Run tools/httpd_stress.py scenarios (a)-(g)
#   5. Stop emulator
#
# Usage: tools/run_httpd_stress_emulator.sh [httpd_stress.py args...]
#   e.g. tools/run_httpd_stress_emulator.sh --iterations 50 --scenarios abcf
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EMU_DIR="$ROOT/emulator"
EMU_BIN="$EMU_DIR/z80pack-emu"
ROM="$ROOT/build/muzix-zeta-v2.rom"
STRESS="$ROOT/tools/httpd_stress.py"
EMU_LOG="/tmp/httpd_stress_emu.log"
EMU_PID=""

cleanup() {
    if [ -n "${EMU_PID:-}" ] && kill -0 "$EMU_PID" 2>/dev/null; then
        kill "$EMU_PID" 2>/dev/null || true
        wait "$EMU_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Ensure emulator is built
if [ ! -x "$EMU_BIN" ]; then
    (cd "$EMU_DIR" && make >/dev/null 2>&1)
fi

# Ensure ROM is built
if [ ! -f "$ROM" ]; then
    (cd "$ROOT" && make rom >/dev/null 2>&1)
fi

# Start emulator with httpd launched via shell stdin (delay so the shell is ready)
echo "Starting emulator..."
( sleep 5; echo "httpd"; while true; do sleep 60; done ) | "$EMU_BIN" -s muzix "$ROM" -r -i > "$EMU_LOG" 2>&1 &
EMU_PID=$!
echo "Emulator PID: $EMU_PID"

# Wait for httpd to listen on port 80 (up to 30 s)
echo "Waiting for httpd on port 80..."
READY=0
for i in $(seq 1 60); do
    if python3 -c '
import socket
s = socket.socket()
s.settimeout(1)
try:
    s.connect(("127.0.0.1", 80))
    s.close()
    raise SystemExit(0)
except OSError:
    raise SystemExit(1)
' 2>/dev/null; then
        READY=1
        break
    fi
    if [ "$i" -eq 60 ]; then
        echo "ERROR: httpd never listened on port 80" >&2
        cat "$EMU_LOG" >&2
        exit 1
    fi
    sleep 0.5
done

if [ "$READY" -eq 1 ]; then
    echo "httpd is listening."
    echo "Running httpd stress test: $*"
    python3 "$STRESS" --host 127.0.0.1 --port 80 "$@"
    STATUS=$?
else
    STATUS=1
fi

# Stop emulator
cleanup
EMU_PID=""
echo "Stress test exit status: $STATUS"
exit $STATUS
