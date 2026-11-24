#!/bin/bash
# Run httpd stress test against Muzix emulator with proper lifecycle management.
# Usage: run_stress_test.sh <rom_path> <stress_args...>
set -uo pipefail

ROM="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
shift
EMU_DIR="$(cd "$(dirname "$0")/.." && pwd)/emulator"
EMU_BIN="$EMU_DIR/z80pack-emu"
EMU_LOG="/tmp/stress_emu.log"

# Kill any leftover emulators
pkill -f z80pack-emu 2>/dev/null || true
sleep 1

# Start emulator in background
(cd "$EMU_DIR" && (sleep 5; echo "httpd"; while true; do sleep 60; done) | "$EMU_BIN" -s muzix "$ROM" -r -i > "$EMU_LOG" 2>&1) &
EMU_PID=$!
echo "$EMU_PID" > /tmp/stress_emu_pid
sleep 8

# Wait for port 80
for i in $(seq 1 30); do
    if python3 -c "import socket; s=socket.socket(); s.settimeout(1); s.connect(('127.0.0.1',80)); s.close()" 2>/dev/null; then
        echo "Port 80 is open"
        break
    fi
    sleep 0.5
done

# Run stress test
cd "$(cd "$(dirname "$0")/.." && pwd)"
python3 -u tools/httpd_stress.py --host 127.0.0.1 --port 80 "$@"
STATUS=$?

# Kill emulator
kill "$(cat /tmp/stress_emu_pid 2>/dev/null)" 2>/dev/null || true
wait "$(cat /tmp/stress_emu_pid 2>/dev/null)" 2>/dev/null || true

echo "Stress test exit status: $STATUS"
exit $STATUS
