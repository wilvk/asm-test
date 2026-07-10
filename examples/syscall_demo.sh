#!/bin/sh
# syscall_demo.sh — end-to-end "log a separate process's syscalls + data" demo.
#
# Launches examples/syscall_victim as a wholly INDEPENDENT process (it does file
# + stdout I/O in a loop), then runs examples/syscall_log to attach BY PID and log
# the syscalls WITH the data crossing the kernel boundary, out of band. Driven by
# `make hwtrace-syscall-log` (and `make docker-hwtrace-syscall-log`); expects
# $BUILD to point at the build dir (default: build).
set -eu
BUILD="${BUILD:-build}"
VICTIM="$BUILD/syscall_victim"
LOGGER="$BUILD/syscall_log"

# 1) Start the victim as a SEPARATE process; capture its announced pid.
LOG=$(mktemp)
"$VICTIM" 2>"$LOG" &
VPID=$!
trap 'kill "$VPID" 2>/dev/null || true; rm -f "$LOG"' EXIT INT TERM

i=0
while [ "$i" -lt 50 ]; do
    grep -q 'pid=' "$LOG" 2>/dev/null && break
    i=$((i + 1))
    sleep 0.1
done
echo "victim is pid $VPID — attaching a syscall logger from a separate process"

# 2) Log its syscalls + data from a wholly separate process, by PID.
"$LOGGER" "$VPID" 24
