#!/bin/sh
# attach_demo_win.sh — Windows mirror of examples/attach_demo.sh, driven under Wine.
#
# Launches attach_victim_win.exe as a SEPARATE Wine process, reads the PID + the
# hotfn address range it announces, then runs attach_trace_win.exe to attach BY PID
# and single-step one call. Both run under the SAME wineserver (shared WINEPREFIX),
# so the victim's Windows PID is valid for the tracer's DebugActiveProcess.
# Expects $WIN64_BUILD (default build/win64) and $WINE (default wine64).
set -eu
WIN64_BUILD="${WIN64_BUILD:-build/win64}"
WINE="${WINE:-wine64}"
export WINEDEBUG="${WINEDEBUG:-fixme-all,err-all}"

LOG=$(mktemp)
"$WINE" "$WIN64_BUILD/attach_victim_win.exe" 2>"$LOG" &
WPID=$!
trap 'kill "$WPID" 2>/dev/null || true; rm -f "$LOG"' EXIT INT TERM

# Wait for the victim to announce "victim pid=N hotfn=0x.. marker=0x.."
i=0
while [ "$i" -lt 100 ]; do
    grep -q 'hotfn=' "$LOG" 2>/dev/null && break
    i=$((i + 1))
    sleep 0.1
done
line=$(grep 'hotfn=' "$LOG" | head -1)
PID=$(printf '%s\n' "$line" | sed -n 's/.*pid=\([0-9]*\).*/\1/p')
HOT=$(printf '%s\n' "$line" | sed -n 's/.*hotfn=\(0x[0-9a-fA-F]*\).*/\1/p')
MARK=$(printf '%s\n' "$line" | sed -n 's/.*marker=\(0x[0-9a-fA-F]*\).*/\1/p')
: "${PID:?no victim pid}" "${HOT:?no hotfn addr}" "${MARK:?no marker addr}"
LEN=$(( MARK > HOT ? MARK - HOT : HOT - MARK ))
echo "victim is Windows pid $PID; hotfn @ $HOT (len $LEN bytes) — attaching from a separate process"

"$WINE" "$WIN64_BUILD/attach_trace_win.exe" "$PID" "$HOT" "$LEN"
