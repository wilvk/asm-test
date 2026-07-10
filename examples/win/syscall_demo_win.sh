#!/bin/sh
# syscall_demo_win.sh — Windows mirror of examples/syscall_demo.sh, driven under Wine.
#
# Launches syscall_victim_win.exe as a SEPARATE Wine process (it does file I/O in a
# loop), then runs syscall_log_win.exe to attach BY PID and log the ntdll calls WITH
# the data crossing the kernel boundary. Both share one wineserver/WINEPREFIX.
# Expects $WIN64_BUILD (default build/win64) and $WINE (default wine64).
set -eu
WIN64_BUILD="${WIN64_BUILD:-build/win64}"
WINE="${WINE:-wine64}"
export WINEDEBUG="${WINEDEBUG:-fixme-all,err-all}"

LOG=$(mktemp)
"$WINE" "$WIN64_BUILD/syscall_victim_win.exe" 2>"$LOG" &
WPID=$!
trap 'kill "$WPID" 2>/dev/null || true; rm -f "$LOG"' EXIT INT TERM

i=0
while [ "$i" -lt 100 ]; do
    grep -q 'pid=' "$LOG" 2>/dev/null && break
    i=$((i + 1))
    sleep 0.1
done
PID=$(grep 'pid=' "$LOG" | head -1 | sed -n 's/.*pid=\([0-9]*\).*/\1/p')
: "${PID:?no victim pid}"
echo "victim is Windows pid $PID — attaching a syscall logger from a separate process"

"$WINE" "$WIN64_BUILD/syscall_log_win.exe" "$PID" 16
