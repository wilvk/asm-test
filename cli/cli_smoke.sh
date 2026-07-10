#!/bin/sh
# cli_smoke.sh — headless end-to-end smoke for asmspy (the TUI shares this engine).
#
# Spawns the example victims as SEPARATE processes and drives asmspy's
# non-interactive subcommands against them: --list, --syms, --trace (assembly +
# functions), --log (syscalls with data). Driven by `make cli-smoke` /
# `make docker-cli`. Expects $BUILD (default build).
set -eu
BUILD="${BUILD:-build}"
ASM="$BUILD/asmspy"

fail() { echo "SMOKE FAIL: $1" >&2; exit 1; }

echo "--- asmspy --list (head) ---"
# capture first: a bare `... | head` pipeline masks asmspy's exit status (sh has
# no pipefail), so a crashing/erroring --list would slip past unnoticed.
out=$("$ASM" --list) || fail "--list"
printf '%s\n' "$out" | head -6

# region trace: attach to attach_victim (has a hot leaf function 'hotfn')
"$BUILD/attach_victim" 2>/dev/null &
AVPID=$!
SVPID=""
WVPID=""
trap 'kill "$AVPID" ${WVPID:+"$WVPID"} ${SVPID:+"$SVPID"} 2>/dev/null || true' EXIT INT TERM
sleep 1

echo "--- asmspy --syms $AVPID hotfn ---"
"$ASM" --syms "$AVPID" hotfn || fail "--syms"
"$ASM" --syms "$AVPID" hotfn | grep -q hotfn || fail "hotfn not resolved"

echo "--- asmspy --trace $AVPID hotfn 2 ---"
out=$("$ASM" --trace "$AVPID" hotfn 2 2>&1) || true
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -q 'ret=57' || fail "expected ret=57 from hotfn(6,7)"
printf '%s\n' "$out" | grep -q 'assembly:' || fail "no assembly section"

# call-graph: attach to spy_victim (work() calls helper()) and check the callee
# is resolved by name in the "functions called" view.
"$BUILD/spy_victim" 2>/dev/null &
WVPID=$!
sleep 1
echo "--- asmspy --trace $WVPID work 2 (call-graph) ---"
out=$("$ASM" --trace "$WVPID" work 2 2>&1) || true
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -q 'functions called:' || fail "no functions section"
printf '%s\n' "$out" | grep -q 'helper' || fail "callee 'helper' not resolved"
# the callee line is ranked by call count — a leading "<n>x" (work calls helper 5x)
printf '%s\n' "$out" | grep -qE '^ *[0-9]+.*->.*helper' || fail "no call-count on callee"
kill "$WVPID" 2>/dev/null || true

# syscall log: attach to syscall_victim (does file I/O each loop)
"$BUILD/syscall_victim" 2>/dev/null &
SVPID=$!
sleep 1

echo "--- asmspy --log $SVPID 12 ---"
out=$("$ASM" --log "$SVPID" 12 2>&1) || true
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -q 'write(' || fail "no write() syscall captured"

echo "cli-smoke: PASS"
