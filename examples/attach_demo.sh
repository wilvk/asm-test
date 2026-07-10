#!/bin/sh
# attach_demo.sh — end-to-end "trace a process asm-test did NOT start" demo.
#
# Launches examples/attach_victim as a wholly INDEPENDENT process, then runs
# examples/attach_trace to attach to it BY PID, resolve its hot function, run_to
# the entry, and single-step one invocation out of band. Driven by
# `make hwtrace-attach-demo` (and `make docker-hwtrace-attach-demo`); expects
# $BUILD to point at the build dir (default: build).
set -eu
BUILD="${BUILD:-build}"
VICTIM="$BUILD/attach_victim"
TRACER="$BUILD/attach_trace"

# hotfn's byte length comes straight from the victim's symbol table — the same
# place a debugger or a JIT perf-map gets a symbol size.
LEN=$(nm -S --defined-only "$VICTIM" | awk '$4=="hotfn"{print "0x" $2}')
: "${LEN:?could not read hotfn size from $VICTIM}"

# 1) Start the victim as a SEPARATE process; capture its announced pid + address.
LOG=$(mktemp)
"$VICTIM" 2>"$LOG" &
VPID=$!
trap 'kill "$VPID" 2>/dev/null || true; rm -f "$LOG"' EXIT INT TERM

# Wait for it to announce itself (it prints "victim pid=N hotfn=0x..." once).
i=0
while [ "$i" -lt 50 ]; do
    grep -q hotfn "$LOG" 2>/dev/null && break
    i=$((i + 1))
    sleep 0.1
done
ADDR=$(awk -F'hotfn=' 'NF>1{print $2; exit}' "$LOG")
: "${ADDR:?victim did not announce its hotfn address}"
echo "victim is pid $VPID; hotfn @ $ADDR (len $LEN bytes) — attaching from a separate process"

# 2) Trace it from a wholly separate process, by PID.
LD_LIBRARY_PATH="$BUILD:${LD_LIBRARY_PATH:-}" "$TRACER" "$VPID" "$ADDR" "$LEN"
