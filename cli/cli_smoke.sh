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

# Unit-test the pure pieces first (no ncurses/ptrace): the TUI scrollback
# viewport math, the call-graph sort comparator, and the jitdump reader.
echo "--- test_logview (TUI scrollback math) ---"
"$BUILD/test_logview" || fail "test_logview"
echo "--- test_graphsort (call-graph sort comparator) ---"
"$BUILD/test_graphsort" || fail "test_graphsort"
echo "--- test_jitdump (binary jitdump reader + JIT resolve chain) ---"
"$BUILD/test_jitdump" || fail "test_jitdump"
echo "--- test_view (data-flow view: annotation + def-use split + L2 slice) ---"
"$BUILD/test_view" || fail "test_view"
echo "--- test_treefilter (call-tree depth cap / symbol focus / module filter) ---"
"$BUILD/test_treefilter" || fail "test_treefilter"
echo "--- test_symtab (symbol reverse lookup: gaps, zero-size, boundaries) ---"
"$BUILD/test_symtab" || fail "test_symtab"

echo "--- asmspy --list (head) ---"
# capture first: a bare `... | head` pipeline masks asmspy's exit status (sh has
# no pipefail), so a crashing/erroring --list would slip past unnoticed.
out=$("$ASM" --list) || fail "--list"
printf '%s\n' "$out" | head -6

echo "--- asmspy --list active (head) ---"
outa=$("$ASM" --list active) || fail "--list active"
printf '%s\n' "$outa" | head -4
printf '%s\n' "$outa" | grep -qi 'CPU' || fail "--list active: no CPU column"

echo "--- asmspy --list scan (head) ---"
outs=$("$ASM" --list scan) || fail "--list scan"
printf '%s\n' "$outs" | head -4
printf '%s\n' "$outs" | grep -qi 'STR' || fail "--list scan: no STR column"

# Bad arguments must be REJECTED UP FRONT, not silently coerced (atoi("nginx")
# is 0). Insist on rc=2, the bad-usage code: a bad pid that slips through to an
# attach also exits nonzero (rc=1), so "not zero" would not catch a regression.
echo "--- asmspy argument validation ---"
expect_badarg() {
    set +e
    "$@" >/dev/null 2>&1
    rc=$?
    set -e
    [ "$rc" -eq 2 ] || fail "expected rc=2 (bad argument) from '$*', got rc=$rc"
}
expect_badarg "$ASM" --list bogus   # unknown sort (used to sort by pid, rc=0)
expect_badarg "$ASM" --syms nginx   # non-numeric pid
expect_badarg "$ASM" --log 0        # pid 0
expect_badarg "$ASM" --stream -3    # negative pid
expect_badarg "$ASM" --log 1 abc    # non-numeric count
echo "bad arguments rejected"

# region trace: attach to attach_victim (has a hot leaf function 'hotfn')
"$BUILD/attach_victim" 2>/dev/null &
AVPID=$!
SVPID=""
WVPID=""
TVPID=""
DVPID=""
CVPID=""
JVPID=""
UPID=""
IPID=""
YPID=""
MVPID=""
HWPID=""
DLPID=""
EXPID=""
FKPID=""
CLPID=""
IVPID=""
SKPID=""
LJPID=""
SGPID=""
trap 'kill "$AVPID" ${WVPID:+"$WVPID"} ${SVPID:+"$SVPID"} ${TVPID:+"$TVPID"} ${DVPID:+"$DVPID"} ${CVPID:+"$CVPID"} ${JVPID:+"$JVPID"} ${UPID:+"$UPID"} ${IPID:+"$IPID"} ${YPID:+"$YPID"} ${MVPID:+"$MVPID"} ${HWPID:+"$HWPID"} ${DLPID:+"$DLPID"} ${EXPID:+"$EXPID"} ${FKPID:+"$FKPID"} ${CLPID:+"$CLPID"} ${IVPID:+"$IVPID"} ${SKPID:+"$SKPID"} ${LJPID:+"$LJPID"} ${SGPID:+"$SGPID"} 2>/dev/null || true; rm -f ${JVPID:+"/tmp/perf-$JVPID.map"} ${UPID:+"$BUILD/jit-$UPID.dump"} "$BUILD/int3_swallow.log" "$BUILD/tid_victim.log" "$BUILD/watch_victim.log" 2>/dev/null || true; rm -f /tmp/asmspy_fork_parent.txt /tmp/asmspy_fork_child.txt /tmp/asmspy_sock_victim.sock "$BUILD/sock_victim.log" 2>/dev/null || true; rm -rf "$BUILD/debuglink_t" 2>/dev/null || true' EXIT INT TERM
sleep 1

echo "--- asmspy --syms $AVPID hotfn ---"
symline=$("$ASM" --syms "$AVPID" hotfn 2>/dev/null | grep -m1 hotfn) \
    || fail "hotfn not resolved"
printf '%s\n' "$symline"
HOTADDR=$(printf '%s' "$symline" | awk '{print $1}') # 0x... runtime address
HOTSIZE=$(printf '%s' "$symline" | awk '{print $2}') # decimal byte size

echo "--- asmspy --trace $AVPID hotfn 2 ---"
out=$("$ASM" --trace "$AVPID" hotfn 2 2>&1) || true
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -q 'ret=57' || fail "expected ret=57 from hotfn(6,7)"
printf '%s\n' "$out" | grep -q 'assembly:' || fail "no assembly section"
# each disassembled line is prefixed with its execution count (loop body runs >1x)
printf '%s\n' "$out" | grep -qE '^ *[0-9]+.*[+]0x' || fail "no per-instruction count"

# Same region by EXPLICIT 0xADDR:LEN — reaches code no symbol need cover. Must
# resolve and trace identically (same base/len reaches the engine either way).
echo "--- asmspy --trace $AVPID $HOTADDR:$HOTSIZE 2 (explicit range) ---"
out=$("$ASM" --trace "$AVPID" "$HOTADDR:$HOTSIZE" 2 2>&1) || true
printf '%s\n' "$out" | grep -q 'ret=57' \
    || fail "explicit 0xADDR:LEN trace did not match by-name trace"
# a bare address no sized symbol covers, and a zero length, must be rejected
"$ASM" --trace "$AVPID" 0x1 1 >/dev/null 2>&1 && fail "--trace accepted an uncovered bare address"
"$ASM" --trace "$AVPID" "$HOTADDR:0" 1 >/dev/null 2>&1 && fail "--trace accepted a zero-length range"

echo "--- asmspy --stream $AVPID 30 (live instruction stream) ---"
out=$("$ASM" --stream "$AVPID" 30 2>&1) || true
printf '%s\n' "$out" | head -8
printf '%s\n' "$out" | grep -qE 'mov|jmp|cmp|add|push|call|lea|test|sub|nop' \
    || fail "stream: no disassembly"

# scoped DATA-FLOW capture: attach to attach_victim, single-step the hot leaf
# 'hotfn' for ONE invocation, and surface the L0 value trace + L1 def-use. The
# producer (src/dataflow_ptrace.c) needs Capstone + ptrace; both are present in
# the CI image, so we assert real output. Where the producer is unavailable the
# subcommand prints "# SKIP --dataflow" and exits 0 (self-skip discipline), which
# the smoke accepts. timeout-guarded: a producer that never reaches the region
# entry (the function isn't executing) would otherwise block at its breakpoint.
echo "--- asmspy --dataflow $AVPID hotfn (scoped L0 value trace + L1 def-use) ---"
set +e
dfout=$(timeout 40 "$ASM" --dataflow "$AVPID" hotfn 2>&1); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--dataflow hung (producer never reached the region entry)"
printf '%s\n' "$dfout" | head -14
if printf '%s\n' "$dfout" | grep -q '^# SKIP --dataflow'; then
    echo "(data-flow producer unavailable here — subcommand self-skipped, OK)"
else
    [ "$rc" -eq 0 ] || fail "--dataflow exited $rc"
    printf '%s\n' "$dfout" | grep -q 'data flow' || fail "--dataflow: no header"
    printf '%s\n' "$dfout" | grep -q 'ret=57' \
        || fail "--dataflow: expected ret=57 from hotfn(6,7)"
    printf '%s\n' "$dfout" | grep -q 'value trace:' \
        || fail "--dataflow: no value-trace section"
    # a per-step disassembled line (offset + a real mnemonic — proves L0 capture)
    printf '%s\n' "$dfout" | grep -qE '#0 .*\+0x' || fail "--dataflow: no step #0"
    printf '%s\n' "$dfout" \
        | grep -qE '(mov|add|cmp|lea|test|sub|xor|imul|neg|jmp|jn?e|jl|jg|ret)' \
        || fail "--dataflow: no disassembled value-trace steps"
    printf '%s\n' "$dfout" | grep -q 'def-use edges' \
        || fail "--dataflow: no def-use section"
    # hotfn's loop threads registers step-to-step, so there ARE last-writer edges
    printf '%s\n' "$dfout" | grep -qE '#[0-9]+->#[0-9]+' \
        || fail "--dataflow: no def-use edges (register data-flow in hotfn expected)"

    # JSON export: machine-readable L0 trace + L1 edges (stdout only, pipes to jq)
    echo "--- asmspy --dataflow $AVPID hotfn --json ---"
    set +e
    djout=$(timeout 40 "$ASM" --dataflow "$AVPID" hotfn --json 2>/dev/null); rc=$?
    set -e
    [ "$rc" -eq 124 ] && fail "--dataflow --json hung"
    printf '%s\n' "$djout" | head -4
    printf '%s' "$djout" | grep -q '^{"pid":' \
        || fail "--dataflow --json: no top-level {\"pid\":...} object"
    printf '%s' "$djout" | grep -q '"func":"hotfn"' \
        || fail "--dataflow --json: func not exported"
    printf '%s' "$djout" | grep -q '"result":57' \
        || fail "--dataflow --json: result 57 missing"
    printf '%s' "$djout" | grep -q '"trace":\[' \
        || fail "--dataflow --json: no trace array"
    printf '%s' "$djout" | grep -q '"defuse":\[' \
        || fail "--dataflow --json: no defuse array"
    # at least one captured operand value in the flattened op stream
    printf '%s' "$djout" | grep -qE '"ops":\[.*"value":"0x' \
        || fail "--dataflow --json: no captured operand value"
    # the human view must NOT leak into JSON mode
    printf '%s' "$djout" | grep -q 'value trace:' \
        && fail "--dataflow --json: human text leaked into JSON"
    if command -v python3 >/dev/null 2>&1; then
        printf '%s' "$djout" | python3 -c 'import json,sys
d = json.load(sys.stdin)
assert d["func"] == "hotfn" and d["result"] == 57
assert d["trace"] and all(k in d["trace"][0] for k in ("step","off","disasm","ops"))
assert isinstance(d["defuse"], list)' \
            || fail "--dataflow --json: not well-formed JSON / missing keys"
        echo "  json validated (python3 json.load: trace + defuse)"
    else
        echo "  json structural checks passed (python3 absent; strict parse skipped)"
    fi
fi
# ---------------------------------------------------------------------------
# BOUNDED ENTRY WAIT (asmspy-plan Theme H)
# ---------------------------------------------------------------------------
# --dataflow arms an int3 at the region ENTRY and waits for a thread to arrive.
# That wait was UNBOUNDED: naming a function that is not currently running did not
# error, it HUNG. Measured before the fix, on one victim/function/thread:
#   --trace    rc=1   4s   "alpha_work never executed on thread N"   <- honest
#   --dataflow rc=124 25s  (header only, killed by timeout)          <- hung
# The producer's DFP_STEP_BACKSTOP did NOT cover this: it counts single-steps, and a
# region that never arrives burns zero steps, so the counter never advanced.
#
# main() IS THE FIXTURE, because of what it is: entered exactly ONCE, before we
# attached, and never re-entered — the precise shape that blocks. hotfn on the SAME
# victim is the control: a bound that also broke the happy path would show up there.
#
# ASMTEST_DF_ENTRY_WAIT_MS forces the deadline in ~0.8s instead of the 10s default.
echo "--- asmspy --dataflow $AVPID main (bounded entry wait: must REPORT, not hang) ---"
set +e
t0=$(date +%s)
nvout=$(ASMTEST_DF_ENTRY_WAIT_MS=800 timeout 30 "$ASM" --dataflow "$AVPID" main 2>&1)
nvrc=$?
t1=$(date +%s)
set -e
[ "$nvrc" -eq 124 ] && fail "--dataflow on a never-re-entered region HUNG (the entry wait is unbounded again)"
[ "$nvrc" -eq 1 ] || fail "--dataflow main: expected rc=1 (not-seen-entering), got $nvrc"
printf '%s\n' "$nvout" | grep -q 'not seen entering' \
    || fail "--dataflow main: no honest not-seen-entering report: $nvout"
# The name must be the SYMBOL, not freed memory: dc.func borrows into the symtab,
# which is released before this message is formatted. A use-after-free here printed
# plausible-looking garbage rather than crashing.
printf '%s\n' "$nvout" | grep -q '^main not seen entering' \
    || fail "--dataflow main: region name wrong/garbled (symtab use-after-free?): $nvout"
[ $((t1 - t0)) -lt 15 ] \
    || fail "--dataflow main took $((t1-t0))s — the 800ms bound did not drive it"
echo "  bounded: reported in $((t1-t0))s instead of hanging"

# THE CONTROL, and it is the whole test's guard: the SAME victim's hotfn must still
# capture. Without this, "rc=1 and a message" would also be produced by a --dataflow
# that had simply stopped working.
echo "--- asmspy --dataflow $AVPID hotfn (control: the bound must not break capture) ---"
set +e
# NB no --max: a --max BELOW the region's step count makes the producer FAIL
# (pre-existing, filed under Theme H; hotfn is 83 steps, so --max=5 => rc=1).
ctlout=$(timeout 40 "$ASM" --dataflow "$AVPID" hotfn 2>&1); ctlrc=$?
set -e
[ "$ctlrc" -eq 0 ] || fail "--dataflow hotfn broke under the entry bound (rc=$ctlrc)"
printf '%s\n' "$ctlout" | grep -q 'ret=57' \
    || fail "--dataflow hotfn: no capture under the entry bound"
echo "  control: hotfn still captures (ret=57)"

# The target must OUTLIVE the timeout path. The disarm INTERRUPTs a thread, restores
# the byte, rewinds rip if it sits AT the trap, and CONTs it. Skip any of that and the
# victim does not fail here — it dies of SIGTRAP on its NEXT arrival, seconds later,
# looking unrelated to the tool. Sleep past several hotfn calls (~5Hz) before asserting.
sleep 2
kill -0 "$AVPID" 2>/dev/null \
    || fail "attach_victim DIED after the bounded --dataflow (entry int3 left armed?)"
echo "  target survived the timeout path + settle"

# ---------------------------------------------------------------------------
# --max TRUNCATES, it does not FAIL (asmspy-plan Theme H)
# ---------------------------------------------------------------------------
# asmspy.h documents --max as bounding the in-region steps captured. It did not
# truncate, it FAILED: the producer reached its cap, appended the partial trace, set
# vt->truncated — and then returned DF_PTRACE_ETRACE ("fork/ptrace/wait failure"), so
# a valid --max surfaced as "ptrace/attach failure (permission? ptrace_scope? … W^X
# JIT page)" and sent the operator to Yama when the truth was "your cap was smaller
# than the function". MEASURED on hotfn (83 steps): --max=3 -> rc=1, --max=5 -> rc=1,
# --max=200 -> rc=0. The flag worked ONLY when it did nothing, which is exactly why no
# test caught it: the suite only ever asserted expect_badarg --max=0.
#
# THE ASSERTION IS THE EXACT STEP COUNT, not merely rc=0. "It did not fail" would also
# pass if --max were ignored entirely — and an ignored cap is the other way this breaks.
# n steps for --max=n can only come from a cap that caps.
for m in 3 5; do
    echo "--- asmspy --dataflow $AVPID hotfn --max=$m (must truncate to exactly $m) ---"
    set +e
    mxout=$(timeout 40 "$ASM" --dataflow "$AVPID" hotfn --max=$m 2>&1); mxrc=$?
    set -e
    [ "$mxrc" -eq 0 ] || fail "--dataflow --max=$m failed (rc=$mxrc) instead of truncating: $mxout"
    printf '%s\n' "$mxout" | grep -qE "[^0-9]$m steps," \
        || fail "--dataflow --max=$m did not truncate to $m steps: $(printf '%s' "$mxout" | grep -o '[0-9]* steps, [0-9]* records')"
    echo "  truncated to exactly $m steps"
done
# JSON must ANNOUNCE the truncation — a prefix that claims to be a whole capture is the
# same confidently-wrong shape the ETRACE return had.
mjout=$(timeout 40 "$ASM" --dataflow "$AVPID" hotfn --max=5 --json 2>/dev/null)
printf '%s' "$mjout" | grep -q '"steps":5' \
    || fail "--dataflow --max=5 --json: steps != 5"
printf '%s' "$mjout" | grep -q '"truncated":true' \
    || fail "--dataflow --max=5 --json: a truncated capture did not set truncated"
echo "  json: steps=5, truncated=true"
# THE CONTROL: a cap ABOVE the region's size must be indistinguishable from no cap —
# proves --max truncates at the CAP rather than just capping everything short.
set +e
bigout=$(timeout 40 "$ASM" --dataflow "$AVPID" hotfn --max=200 2>&1); bigrc=$?
set -e
[ "$bigrc" -eq 0 ] || fail "--dataflow --max=200 (above hotfn's 83 steps) failed: $bigrc"
printf '%s\n' "$bigout" | grep -q 'ret=57' \
    || fail "--dataflow --max=200: no full capture (a cap above the region must not truncate)"
printf '%s\n' "$bigout" | grep -qE '[^0-9]83 steps,' \
    || fail "--dataflow --max=200: expected the full 83 steps"
echo "  control: --max=200 (> 83) captures in full, ret=57"

# ---------------------------------------------------------------------------
# --dataflow --auto: trace what the target is DOING, no symbol named (Theme H)
# ---------------------------------------------------------------------------
# auto_victim's SHAPE is the test, because the intuitive rule and the correct one
# disagree on it:
#   grind_forever()  entered ONCE, never returns, burns the CPU  -> the RESIDENCY
#                    winner. A PC histogram picks it; an entry breakpoint there can
#                    never fire again, so that pick HANGS (until the entry bound).
#   entered_often()  called from grind_forever's inner loop      -> the only pick
#                    the producer can actually catch.
# So "picked entered_often" cannot pass by accident: a residency rule yields
# grind_forever, and a hottest-EDGE-outright rule yields grind_forever's loop
# back-edge (mid-function, not a region at all). Only an ENTRY-ARRIVAL rule lands
# here. quiet_helper() is never called: it must never be named.
#
# The picker itself is unit-tested on every host (build/test_autoregion, 19 checks).
# THIS block covers only the wiring, and it self-skips off AMD IBS — real hardware,
# a legitimate gate per CLAUDE.md. `make docker-cli-ibs` is the lane that runs it.
echo "--- asmspy --dataflow --auto (pick the hot ENTRY, no symbol given) ---"
"$BUILD/auto_victim" 2>"$BUILD/auto_victim.log" &
UVPID=$!
sleep 1
kill -0 "$UVPID" 2>/dev/null || fail "auto_victim did not start"
set +e
auout=$(timeout 60 "$ASM" --dataflow "$UVPID" --auto 2>&1); aurc=$?
set -e
[ "$aurc" -eq 124 ] && fail "--dataflow --auto hung"
if printf '%s\n' "$auout" | grep -q '^# SKIP --dataflow --auto'; then
    printf '%s\n' "$auout" | grep '^# SKIP' | sed 's/^/  /'
    echo "  (IBS-Op unavailable — --auto self-skipped; assertions NOT run. Use make docker-cli-ibs)"
else
    [ "$aurc" -eq 0 ] || fail "--dataflow --auto exited $aurc: $auout"
    # THE assertion: the callee, not the residency winner.
    printf '%s\n' "$auout" | grep -q 'entered_often' \
        || fail "--auto did not pick entered_often: $auout"
    # THE CONTROL, and the whole reason the fixture has two functions: the rival
    # rule's answer must be ABSENT. grind_forever has no entry edge at all (it was
    # entered before we attached), so a picker that names it is ranking residency.
    printf '%s\n' "$auout" | grep -q 'grind_forever' \
        && fail "--auto picked grind_forever — that is the RESIDENCY winner, and an entry breakpoint there can never fire"
    # Never called => never observed => never picked.
    printf '%s\n' "$auout" | grep -q 'quiet_helper' \
        && fail "--auto named quiet_helper, which is never called"
    # It must actually TRACE the pick, not just name it — the point is the data flow.
    printf '%s\n' "$auout" | grep -q 'data flow — entered_often' \
        || fail "--auto picked but did not capture: $auout"
    printf '%s\n' "$auout" | grep -qE '#0 .*endbr64|#0 .*\+0x' \
        || fail "--auto: no value trace from the auto-picked region"
    # The provenance line must report real evidence, not a guess.
    printf '%s\n' "$auout" | grep -qE '\-\-auto: entered_often \[auto_victim\] — [0-9]+ entry samples' \
        || fail "--auto: no entry-sample provenance: $auout"
    echo "  --auto picked + traced entered_often (grind_forever correctly rejected)"

    # --module= scopes the pick. A module that matches nothing must REFUSE
    # honestly rather than fall back to a wrong region.
    set +e
    amout=$(timeout 60 "$ASM" --dataflow "$UVPID" --auto --module=no_such_module 2>&1); amrc=$?
    set -e
    [ "$amrc" -eq 124 ] && fail "--auto --module hung"
    printf '%s\n' "$amout" | grep -q 'no function was observed being ENTERED' \
        || fail "--auto --module=no_such_module should refuse honestly, got: $amout"
    printf '%s\n' "$amout" | grep -q 'entered_often' \
        && fail "--auto --module=no_such_module still picked entered_often (the filter does not filter)"
    echo "  --auto --module= filters the pick (and refuses honestly when empty)"
fi
# The victim must survive being sampled + traced.
sleep 1
kill -0 "$UVPID" 2>/dev/null || fail "auto_victim died under --dataflow --auto"
kill "$UVPID" 2>/dev/null || true
rm -f "$BUILD/auto_victim.log"

# --auto + --tid is a USAGE error, not a precedence rule: the sampler carries no
# tid, so it could only ever pin the capture to a thread that may never arrive.
expect_badarg "$ASM" --dataflow 1 --auto --tid=1
# --module= without --auto would be a silent no-op that reads like a filter.
expect_badarg "$ASM" --dataflow 1 hotfn --module=libc

# bad --tid / --max / pid are rejected up front (rc=2), before any attach
expect_badarg "$ASM" --dataflow "$AVPID" hotfn --tid=nope
expect_badarg "$ASM" --dataflow "$AVPID" hotfn --max=0
expect_badarg "$ASM" --dataflow nginx hotfn

# ---------------------------------------------------------------------------
# separate debug info: .gnu_debuglink + build-id (the stripped-distro-binary case)
# ---------------------------------------------------------------------------
# Reproduce what a distro actually ships — /usr/bin/foo with no .symtab, symbols
# in a separate -dbg(sym) file — by stripping a copy of debuglink_victim and
# attaching its debug info back as a separate file.
#
# The NEGATIVE control is the whole test: assert first that the stripped victim
# resolves NOTHING, so "the name appeared" cannot pass on a build that never
# reads the debug file. Then each search path is added in turn and must bring the
# symbol back. The CRC check gets the same treatment from the other side: the
# mismatched debug file is a byte-appended copy of the GOOD one — still a
# perfectly parseable ELF with the right symbols, differing ONLY in its CRC-32 —
# and it must be REJECTED, while the build-id case below re-resolves from that
# very same file (build-id is keyed by id, not CRC), proving the rejection was
# the CRC gate and not an unreadable file.
DLDIR="$BUILD/debuglink_t"
if ! command -v objcopy >/dev/null 2>&1 || ! command -v strip >/dev/null 2>&1 \
   || ! command -v readelf >/dev/null 2>&1; then
    echo "# SKIP separate-debug-info (binutils objcopy/strip/readelf absent)"
else
    rm -rf "$DLDIR"
    mkdir -p "$DLDIR/bin" "$DLDIR/debugroot"
    cp "$BUILD/debuglink_victim" "$DLDIR/bin/dlv"
    # the debug file (full .symtab, contents carved out), then strip the binary and
    # record the link. --add-gnu-debuglink must come AFTER strip: .gnu_debuglink is
    # non-alloc, so --strip-all would remove it again. objcopy stores the BASENAME
    # ("dlv.debug") plus the CRC-32 of the file's bytes.
    objcopy --only-keep-debug "$DLDIR/bin/dlv" "$DLDIR/dlv.debug"
    strip --strip-all "$DLDIR/bin/dlv"
    objcopy --add-gnu-debuglink="$DLDIR/dlv.debug" "$DLDIR/bin/dlv"
    readelf -S "$DLDIR/bin/dlv" 2>/dev/null | grep -q '\.gnu_debuglink' \
        || fail "fixture: objcopy did not add a .gnu_debuglink section"
    readelf -S "$DLDIR/bin/dlv" 2>/dev/null | grep -q '\.symtab' \
        && fail "fixture: strip left a .symtab — the negative control would be vacuous"
    # search the hermetic debug root, never the host's /usr/lib/debug: the smoke
    # must not need root, and a real -dbg package installed in the image must not
    # be able to satisfy (or mask) any of these cases.
    ASMSPY_DEBUG_DIR="$DLDIR/debugroot"
    export ASMSPY_DEBUG_DIR
    DLABS=$(cd "$DLDIR/bin" && pwd)   # maps reports the absolute path

    # run the stripped victim and count how many symbols asmspy names -> DLN
    dl_run_syms() {
        "$DLDIR/bin/dlv" 2>/dev/null &
        DLPID=$!
        sleep 1
        kill -0 "$DLPID" 2>/dev/null || fail "debuglink_victim did not start"
        DLN=$("$ASM" --syms "$DLPID" debuglink_only_fn 2>/dev/null \
              | grep -c debuglink_only_fn || true)
        DLN=${DLN:-0}
    }
    dl_stop() { kill "$DLPID" 2>/dev/null || true; wait "$DLPID" 2>/dev/null || true; DLPID=""; }

    echo "--- separate debug info: stripped victim, NO debug file (negative control) ---"
    dl_run_syms; dl_stop
    [ "$DLN" -eq 0 ] \
        || fail "stripped victim resolved 'debuglink_only_fn' ($DLN) with NO debug file present — the negative control is broken, every case below would pass vacuously"
    echo "  unresolved, as it must be (0 symbols)"

    # (1) <dir>/<name> — the debug file beside the binary
    echo "--- .gnu_debuglink: <dir>/dlv.debug ---"
    cp "$DLDIR/dlv.debug" "$DLDIR/bin/dlv.debug"
    dl_run_syms
    [ "$DLN" -ge 1 ] || { dl_stop; fail ".gnu_debuglink <dir>/: symbol NOT recovered from the matching debug file"; }
    echo "  resolved ($DLN symbol(s))"
    # the payoff, and the proof the ADDRESS is right and not just the name: trace
    # the recovered symbol. A symbol resolved at a wrong load bias would never hit.
    echo "--- asmspy --trace $DLPID debuglink_only_fn 2 (from separate debug info) ---"
    out=$(timeout 40 "$ASM" --trace "$DLPID" debuglink_only_fn 2 2>&1) || true
    printf '%s\n' "$out" | grep -q 'ret=51' \
        || { printf '%s\n' "$out" | head -5; dl_stop; fail "--trace on the debuglink-resolved symbol: expected ret=51 from debuglink_only_fn(6,7) — the recovered address is not the runtime address"; }
    echo "  traced ret=51 — the recovered symbol is at the real runtime address"
    dl_stop
    rm -f "$DLDIR/bin/dlv.debug"

    # (2) <dir>/.debug/<name>
    echo "--- .gnu_debuglink: <dir>/.debug/dlv.debug ---"
    mkdir -p "$DLDIR/bin/.debug"
    cp "$DLDIR/dlv.debug" "$DLDIR/bin/.debug/dlv.debug"
    dl_run_syms; dl_stop
    [ "$DLN" -ge 1 ] || fail ".gnu_debuglink <dir>/.debug/: symbol not recovered"
    echo "  resolved ($DLN symbol(s))"
    rm -rf "$DLDIR/bin/.debug"

    # (3) <debugdir>/<dir>/<name> — the global debug tree mirrors the real path
    echo "--- .gnu_debuglink: \$ASMSPY_DEBUG_DIR/<dir>/dlv.debug ---"
    mkdir -p "$DLDIR/debugroot$DLABS"
    cp "$DLDIR/dlv.debug" "$DLDIR/debugroot$DLABS/dlv.debug"
    dl_run_syms; dl_stop
    [ "$DLN" -ge 1 ] || fail ".gnu_debuglink \$ASMSPY_DEBUG_DIR/<dir>/: symbol not recovered"
    echo "  resolved ($DLN symbol(s))"
    rm -rf "$DLDIR/debugroot$DLABS"

    # (4) CRC MISMATCH must be REJECTED. The candidate is the good debug file with
    # one byte appended: trailing bytes past the last section leave the ELF fully
    # valid (case (5) below reads this very file), so the ONLY thing that can
    # reject it is the recorded CRC-32.
    echo "--- .gnu_debuglink: CRC MISMATCH must be rejected ---"
    cp "$DLDIR/dlv.debug" "$DLDIR/bad.debug"
    printf 'X' >> "$DLDIR/bad.debug"
    cp "$DLDIR/bad.debug" "$DLDIR/bin/dlv.debug"
    dl_run_syms; dl_stop
    [ "$DLN" -eq 0 ] \
        || fail "a CRC-MISMATCHED debug file resolved $DLN symbol(s) — a stale -dbg package would name every address wrong"
    echo "  rejected (0 symbols) — the recorded CRC-32 is honoured"
    rm -f "$DLDIR/bin/dlv.debug"

    # (5) build-id: <debugdir>/.build-id/ab/cdef....debug, keyed by the note that
    # SURVIVES strip. Uses the byte-appended file from (4) — no CRC is involved on
    # this path, so it must resolve, which is also what proves (4)'s rejection was
    # the CRC check rather than a file asmspy simply could not read.
    BID=$(readelf -n "$DLDIR/bin/dlv" 2>/dev/null \
          | sed -n 's/.*Build ID: \([0-9a-f][0-9a-f]*\).*/\1/p' | head -1)
    if [ -z "$BID" ]; then
        echo "# SKIP build-id (this toolchain emits no .note.gnu.build-id)"
    else
        echo "--- build-id: \$ASMSPY_DEBUG_DIR/.build-id/${BID%${BID#??}}/... ---"
        mkdir -p "$DLDIR/debugroot/.build-id/$(printf '%s' "$BID" | cut -c1-2)"
        cp "$DLDIR/bad.debug" \
           "$DLDIR/debugroot/.build-id/$(printf '%s' "$BID" | cut -c1-2)/$(printf '%s' "$BID" | cut -c3-).debug"
        dl_run_syms; dl_stop
        [ "$DLN" -ge 1 ] \
            || fail "build-id: symbol not recovered from \$ASMSPY_DEBUG_DIR/.build-id/ (note survives strip, so this is the key distros index by)"
        echo "  resolved ($DLN symbol(s)) — from the same file (4) rejected on CRC"

        # A file at the right PATH but carrying the wrong build-id must be
        # rejected. The fixture is the good debug file with spy_victim's build-id
        # grafted in: it still holds debuglink_only_fn in .symtab, so without the
        # id check it WOULD resolve — which is what makes this non-vacuous (a file
        # that simply lacked the symbol would "pass" with no check at all).
        rm -rf "$DLDIR/debugroot/.build-id"
        if objcopy --dump-section .note.gnu.build-id="$DLDIR/foreign.note" \
                   "$BUILD/spy_victim" 2>/dev/null \
           && objcopy --remove-section=.note.gnu.build-id \
                      --add-section .note.gnu.build-id="$DLDIR/foreign.note" \
                      "$DLDIR/dlv.debug" "$DLDIR/wrongid.debug" 2>/dev/null \
           && ! readelf -n "$DLDIR/wrongid.debug" 2>/dev/null | grep -q "$BID"; then
            echo "--- build-id: WRONG build-id at the right path must be rejected ---"
            mkdir -p "$DLDIR/debugroot/.build-id/$(printf '%s' "$BID" | cut -c1-2)"
            cp "$DLDIR/wrongid.debug" \
               "$DLDIR/debugroot/.build-id/$(printf '%s' "$BID" | cut -c1-2)/$(printf '%s' "$BID" | cut -c3-).debug"
            dl_run_syms; dl_stop
            [ "$DLN" -eq 0 ] \
                || fail "a debug file with a FOREIGN build-id resolved $DLN symbol(s) — the build-id is assumed from the path, not verified"
            echo "  rejected (0 symbols) — the build-id is verified, not assumed from the path"
        else
            echo "# SKIP wrong-build-id (objcopy cannot graft a foreign note here)"
        fi
    fi
    unset ASMSPY_DEBUG_DIR
    rm -rf "$DLDIR"
    echo "separate debug info: .gnu_debuglink (3 paths + CRC gate) + build-id OK"
fi

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

# whole-process call graph: same victim (main -> work -> helper). Build the graph
# from a bounded number of CALLS, then assert the caller/callee counts and the
# internal/external tag. timeout-guarded (single-stepping the whole process).
echo "--- asmspy --graph $WVPID 60 (whole-process call graph) ---"
set +e
out=$(timeout 40 "$ASM" --graph "$WVPID" 60 2>&1); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--graph hung (whole-process single-step deadlock)"
printf '%s\n' "$out" | head -12
printf '%s\n' "$out" | grep -q 'call graph' || fail "no call-graph header"
# work() calls helper() -> work has fanout>=1 and helper is invoked >=1x
printf '%s\n' "$out" | grep -qE 'work[^Z]*fanout=[1-9]' || fail "work fanout not counted"
printf '%s\n' "$out" | grep -qE 'helper[^Z]*inv=[1-9]' || fail "helper invocations not counted"
# internal/external tag: work/helper are the target's own exe -> [int]
printf '%s\n' "$out" | grep -qE '\[int\][^Z]*work' || fail "internal marker missing"
# a call into libc goes through the PLT: the stub resolves to name@plt and is
# tagged [EXT] (spy_victim's main calls usleep/fprintf/... via the PLT). This
# also proves the anonymous-stub node is gone.
printf '%s\n' "$out" | grep -qE '\[EXT\][^Z]*@plt' \
    || fail "PLT thunk not resolved to name@plt / tagged external"

echo "--- asmspy --graph $WVPID 60 --sort=fanout ---"
out=$(timeout 40 "$ASM" --graph "$WVPID" 60 --sort=fanout 2>&1) \
    || fail "--graph --sort=fanout"
printf '%s\n' "$out" | grep -q 'functions called' || fail "sort=fanout header missing"
# a bad --sort value is rejected up front (rc=2), not silently coerced
expect_badarg "$ASM" --graph "$WVPID" --sort=bogus

# JSON export: --graph --json emits a machine-readable node list (pipe to jq / a
# visualizer) instead of the human table. Assert it is well-formed and carries
# the per-function fields plus the internal/external/jit classification.
echo "--- asmspy --graph $WVPID 60 --json (machine-readable export) ---"
set +e
jout=$(timeout 40 "$ASM" --graph "$WVPID" 60 --json 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--graph --json hung"
printf '%s\n' "$jout" | head -4
printf '%s' "$jout" | grep -q '^{"pid":' || fail "--json: no top-level {\"pid\":...} object"
printf '%s' "$jout" | grep -q '"functions":\[' || fail "--json: no functions array"
printf '%s' "$jout" | grep -q '"name":"helper"' || fail "--json: callee 'helper' not exported"
printf '%s' "$jout" | grep -q '"kind":"internal"' || fail "--json: no internal-classified node"
printf '%s' "$jout" | grep -q '"kind":"external"' || fail "--json: no external (libc/PLT) node"
printf '%s' "$jout" | grep -qE '"invocations":[0-9]+,"out_calls":[0-9]+,"fanout":[0-9]+}' \
    || fail "--json: per-node counts missing"
# edges carry the caller->callee structure (work -> helper etc.), address-keyed
printf '%s' "$jout" | grep -q '"edges":\[' || fail "--json: no edges array"
printf '%s' "$jout" | grep -qE '"caller":"0x[0-9a-f]+","callee":"0x[0-9a-f]+","count":[0-9]+' \
    || fail "--json: edges missing caller/callee/count"
# the human table must NOT leak into JSON mode
printf '%s' "$jout" | grep -q 'call graph' && fail "--json: human header leaked into JSON"
# strict well-formedness when python3 is present; degrade cleanly otherwise
if command -v python3 >/dev/null 2>&1; then
    printf '%s' "$jout" | python3 -c 'import json,sys
d = json.load(sys.stdin)
assert d["functions"] and d["edges"]
assert all(k in d["functions"][0] for k in ("addr","name","module","kind","invocations","out_calls","fanout"))
assert all(k in d["edges"][0] for k in ("caller","callee","count"))' \
        || fail "--json: not well-formed JSON / missing node or edge keys"
    echo "  json validated (python3 json.load: nodes + edges)"
    # NODE/EDGE UNIQUENESS — the guard on the hash index that replaced the O(n)
    # scans (asmspy-plan Theme E). Both lookups are "find it, else append", so a
    # broken index does not error: it MISSES, appends a second node for an
    # address it already has, and renders a plausible graph with the call counts
    # silently split across the duplicates. Uniqueness is the property the index
    # must preserve, so it is what gets asserted — not the speed, which is what
    # the index is FOR but which no assertion here could pin down honestly.
    printf '%s' "$jout" | python3 -c 'import json,sys
d = json.load(sys.stdin)
addrs = [f["addr"] for f in d["functions"]]
dup = {a for a in addrs if addrs.count(a) > 1}
assert not dup, "duplicate node addresses (index missed a hit): %s" % sorted(dup)[:4]
keys = [(e["caller"], e["callee"]) for e in d["edges"]]
edup = {k for k in keys if keys.count(k) > 1}
assert not edup, "duplicate caller->callee edges (index missed a hit): %s" % sorted(edup)[:4]
print("  nodes/edges unique (%d nodes, %d edges) — hash index dedups correctly" % (len(addrs), len(keys)))' \
        || fail "--json: duplicate nodes/edges — the graph node/edge index is missing hits it should find"
else
    echo "  json structural checks passed (python3 absent; strict parse skipped)"
fi

# DOT export: --graph --dot emits a Graphviz digraph (asmspy --graph <pid> --dot
# | dot -Tsvg). Assert the digraph structure + a caller->callee edge; if graphviz
# is installed, assert `dot` actually parses it.
echo "--- asmspy --graph $WVPID 60 --dot (Graphviz export) ---"
set +e
dout=$(timeout 40 "$ASM" --graph "$WVPID" 60 --dot 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--graph --dot hung"
printf '%s\n' "$dout" | head -4
printf '%s' "$dout" | grep -q '^digraph asmspy {' || fail "--dot: not a digraph"
printf '%s' "$dout" | grep -qE '"0x[0-9a-f]+" \[label="helper' || fail "--dot: node 'helper' missing"
printf '%s' "$dout" | grep -qE '"0x[0-9a-f]+" -> "0x[0-9a-f]+" \[label="[0-9]+"\]' \
    || fail "--dot: no caller->callee edges"
printf '%s' "$dout" | grep -q '^}' || fail "--dot: unterminated digraph"
if command -v dot >/dev/null 2>&1; then
    printf '%s' "$dout" | dot -Tsvg >/dev/null 2>&1 || fail "--dot: graphviz rejected the output"
    echo "  dot validated (graphviz dot -Tsvg)"
else
    echo "  dot structural checks passed (graphviz absent)"
fi

# live call tree: same victim (main -> work -> helper). The indentation must
# reflect real depth — work is called from main (depth 0) and helper from work
# (depth 1, indented two spaces). timeout-guarded (whole-process single-step).
echo "--- asmspy --tree $WVPID 30 (live call tree) ---"
set +e
out=$(timeout 40 "$ASM" --tree "$WVPID" 30 2>&1); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--tree hung (whole-process single-step deadlock)"
printf '%s\n' "$out" | head -10
printf '%s\n' "$out" | grep -qE '^-> work ' || fail "tree: work not at depth 0"
printf '%s\n' "$out" | grep -qE '^  -> helper ' \
    || fail "tree: helper not nested one level under work"

# --tree JSON export: the faithful temporal call log (seq/tid/depth/addr/name/
# module per call), --graph --json's output conventions.
echo "--- asmspy --tree $WVPID 30 --json (machine-readable export) ---"
set +e
tjout=$(timeout 40 "$ASM" --tree "$WVPID" 30 --json 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--tree --json hung"
printf '%s\n' "$tjout" | head -4
printf '%s' "$tjout" | grep -q '^{"pid":' || fail "--tree --json: no top-level {\"pid\":...} object"
printf '%s' "$tjout" | grep -q '"calls":\[' || fail "--tree --json: no calls array"
printf '%s' "$tjout" | grep -qE '"seq":[0-9]+,"tid":[0-9]+,"depth":[0-9]+,"addr":"0x[0-9a-f]+","name":"' \
    || fail "--tree --json: per-call fields missing"
printf '%s' "$tjout" | grep -q '"name":"helper"' || fail "--tree --json: callee 'helper' not exported"
# helper is entered at depth 1 (called from work) — the depth must survive export
printf '%s' "$tjout" | grep -qE '"depth":1,"addr":"0x[0-9a-f]+","name":"helper"' \
    || fail "--tree --json: helper not exported at depth 1"
# the human tree must NOT leak into JSON mode
printf '%s' "$tjout" | grep -q -- '->' && fail "--tree --json: human '->' lines leaked into JSON"
if command -v python3 >/dev/null 2>&1; then
    printf '%s' "$tjout" | python3 -c 'import json,sys
d = json.load(sys.stdin)
assert d["calls"]
assert all(k in d["calls"][0] for k in ("seq","tid","depth","addr","name","module"))' \
        || fail "--tree --json: not well-formed JSON / missing call keys"
    echo "  json validated (python3 json.load: calls)"
else
    echo "  json structural checks passed (python3 absent; strict parse skipped)"
fi

# --tree DOT export: a Graphviz digraph with the calls AGGREGATED into
# caller->callee edges (work -> helper), --graph --dot's output conventions.
echo "--- asmspy --tree $WVPID 30 --dot (Graphviz export) ---"
set +e
tdout=$(timeout 40 "$ASM" --tree "$WVPID" 30 --dot 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--tree --dot hung"
printf '%s\n' "$tdout" | head -4
printf '%s' "$tdout" | grep -q '^digraph asmspy {' || fail "--tree --dot: not a digraph"
printf '%s' "$tdout" | grep -qE '"0x[0-9a-f]+" \[label="helper' || fail "--tree --dot: node 'helper' missing"
printf '%s' "$tdout" | grep -qE '"0x[0-9a-f]+" -> "0x[0-9a-f]+" \[label="[0-9]+"\]' \
    || fail "--tree --dot: no aggregated caller->callee edges"
printf '%s' "$tdout" | grep -q '^}' || fail "--tree --dot: unterminated digraph"
if command -v dot >/dev/null 2>&1; then
    printf '%s' "$tdout" | dot -Tsvg >/dev/null 2>&1 || fail "--tree --dot: graphviz rejected the output"
    echo "  dot validated (graphviz dot -Tsvg)"
else
    echo "  dot structural checks passed (graphviz absent)"
fi

# ---------------------------------------------------------------------------
# CALL-TREE OUTPUT FILTERS: --depth / --focus / --module (asmspy-plan Theme E)
# ---------------------------------------------------------------------------
# The unfiltered tree above is this test's NEGATIVE CONTROL, and it is the whole
# point: it PROVES the lines each filter must remove are present when the filter
# is off. spy_victim's shape supplies a control for every case --
#
#   -> work [spy_victim]            real depth 0
#     -> helper [spy_victim]        real depth 1   (x5 per iteration)
#   -> usleep@plt [spy_victim]      real depth 0   <- same depth as work
#     -> (libc frames under usleep)  real depth 1+
#
# Only the first three are relied on: they are spy_victim's OWN functions and its
# own PLT stub, fixed by its source. What libc does UNDER usleep is a glibc
# implementation detail that varies between hosts, so nothing below asserts on it
# (an earlier version did, and broke on a host that routes usleep differently).
#
# -- so "--focus=work dropped usleep@plt" cannot pass by dropping deep lines (it
# is at the SAME depth as the surviving work), and "--focus=helper printed helper
# at column 0" cannot pass without a real re-base (unfiltered, helper only ever
# appears indented two columns).
tf_capture() { # tf_capture <n> <flag...> -> sets $out; fails on hang/error
    set +e
    out=$(timeout 60 "$ASM" --tree "$WVPID" "$@" 2>&1); rc=$?
    set -e
    [ "$rc" -eq 124 ] && fail "--tree $* hung (whole-process single-step)"
    [ "$rc" -eq 0 ] || fail "--tree $* exited rc=$rc"
    [ -n "$out" ] || fail "--tree $* produced no output at all"
}

echo "--- asmspy --tree --depth=1 (depth cap) ---"
tf_capture 8 --depth=1
printf '%s\n' "$out" | head -3
printf '%s\n' "$out" | grep -qE '^-> work ' || fail "--depth=1: work (depth 0) missing"
# the cap must remove the depth-1 callee the unfiltered run proved was there
printf '%s\n' "$out" | grep -q 'helper' && fail "--depth=1: helper (depth 1) leaked past the cap"
# and it must not just be dropping everything but the first line
printf '%s\n' "$out" | grep -qE '^-> usleep@plt ' || fail "--depth=1: usleep@plt (depth 0) wrongly dropped"
# n must count SURVIVING lines, not raw calls: unfiltered, 8 calls yield only ~2
# depth-0 lines, so a budget spent on filtered-away calls lands far short of 8.
[ "$(printf '%s\n' "$out" | grep -c '^-> ')" -ge 8 ] \
    || fail "--depth=1: fewer than 8 lines — n is counting raw calls, not surviving lines"
echo "  depth cap: only depth-0 calls, and n counts surviving lines"

echo "--- asmspy --tree --focus=helper (symbol focus + depth re-base) ---"
tf_capture 6 --focus=helper
printf '%s\n' "$out" | head -3
# helper runs at real depth 1 (unfiltered: indented two columns). Under focus it
# roots the tree, so it must render at column 0 — a filter that suppressed lines
# without re-basing would print "  -> helper" and fail here.
printf '%s\n' "$out" | grep -qE '^-> helper \[spy_victim\]' \
    || fail "--focus=helper: helper not re-based to depth 0"
printf '%s\n' "$out" | grep -q '  -> ' && fail "--focus=helper: leaked an indented non-root line"
# work is helper's CALLER: focusing on a callee must not show it
printf '%s\n' "$out" | grep -q -- '-> work ' && fail "--focus=helper: caller 'work' leaked"
echo "  focus: subtree rooted + re-based to depth 0, caller excluded"

echo "--- asmspy --tree --focus=work (subtree scope) ---"
tf_capture 8 --focus=work
printf '%s\n' "$out" | head -3
printf '%s\n' "$out" | grep -qE '^-> work \[spy_victim\]' || fail "--focus=work: root missing"
printf '%s\n' "$out" | grep -qE '^  -> helper \[spy_victim\]' \
    || fail "--focus=work: work's callee helper missing (focus must keep the SUBTREE)"
# THE scope assertion: usleep@plt runs at the SAME real depth as work but OUTSIDE
# it, so a "focus" implemented as a plain name filter (or as a depth cut) keeps it.
printf '%s\n' "$out" | grep -q 'usleep' \
    && fail "--focus=work: usleep@plt leaked — focus is not scoping to the subtree"
echo "  focus: keeps the subtree, drops a same-depth sibling outside it"

echo "--- asmspy --tree --module=libc (module filter) ---"
tf_capture 4 --module=libc
printf '%s\n' "$out" | head -3
printf '%s\n' "$out" | grep -q '\[libc' || fail "--module=libc: no libc frames captured"
# the victim's own functions dominate the unfiltered tree and must all be gone
printf '%s\n' "$out" | grep -q '\[spy_victim\]' \
    && fail "--module=libc: the target's own [spy_victim] frames leaked"
echo "  module filter: libc callees only, target's own frames dropped"

# (--depth measured from the --focus root is asserted end-to-end further down,
# against longjmp_victim's OWN three-deep chain. It used to be tested here with
# --focus=usleep --depth=2, expecting usleep@plt -> __nanosleep ->
# clock_nanosleep — which hardcoded GLIBC'S INTERNAL call chain and broke on a
# host that routes usleep differently. The composition is a property of the
# filter, not of libc, so it is now tested against code this repo owns.)

# the re-base must survive the JSON export too: helper is exported at depth 1
# unfiltered (asserted above) and MUST be depth 0 under --focus=helper — same
# symbol, same run shape, the depth difference is the filter's alone.
echo "--- asmspy --tree --focus=helper --json (re-based depth survives export) ---"
set +e
tfj=$(timeout 60 "$ASM" --tree "$WVPID" 4 --focus=helper --json 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--tree --focus --json hung"
printf '%s' "$tfj" | grep -qE '"depth":0,"addr":"0x[0-9a-f]+","name":"helper"' \
    || fail "--tree --focus=helper --json: helper not exported at re-based depth 0"
printf '%s' "$tfj" | grep -q '"name":"work"' \
    && fail "--tree --focus=helper --json: out-of-focus caller leaked into the export"
echo "  json: focused subtree exported with re-based depths"

# bad filter arguments are rejected up front (rc=2), not silently coerced.
# --depth=0 is a usage error, NOT "unlimited": it can only ever print nothing.
expect_badarg "$ASM" --tree "$WVPID" --depth=0
expect_badarg "$ASM" --tree "$WVPID" --depth=-1
expect_badarg "$ASM" --tree "$WVPID" --depth=abc
expect_badarg "$ASM" --tree "$WVPID" --focus=
expect_badarg "$ASM" --tree "$WVPID" --module=
echo "  bad --depth/--focus/--module rejected up front"
kill "$WVPID" 2>/dev/null || true

# ---------------------------------------------------------------------------
# --tree DEPTH IS A REAL RETURN-ADDRESS STACK, not a counter (Theme C)
# ---------------------------------------------------------------------------
# longjmp_victim: main setjmps, calls three deep (level_one -> level_two ->
# jump_out), and longjmp()s straight back — then calls after_jump() from main at
# depth 0.
#
# longjmp restores rsp and rip directly: those three frames are discarded without
# a single `ret` retiring. A tracer that counts +1 per CALL and -1 per RET
# therefore never comes back down, and the drift is CUMULATIVE — MEASURED
# against exactly that algorithm, after_jump rendered at depth 5, then 10, and
# level_one marched 0 -> 10 -> 20 columns across three iterations of a process
# behaving completely normally.
#
# The rsp-keyed stack pops every frame the stack pointer moved above, so
# after_jump lands at depth 0. Both halves are asserted, and they are each
# other's control:
#   * jump_out at depth 2  => real nesting IS still tracked (so "depth 0" is not
#                             just a tracer that never counts anything)
#   * after_jump at depth 0 => the discarded frames were actually unwound
echo "--- asmspy --tree across a longjmp (real return-address stack) ---"
"$BUILD/longjmp_victim" 2>/dev/null &
LJPID=$!
sleep 1
set +e
ljout=$(timeout 60 "$ASM" --tree "$LJPID" 24 2>&1); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--tree on longjmp_victim hung"
kill -9 "$LJPID" 2>/dev/null || true
wait "$LJPID" 2>/dev/null || true
LJPID=""
printf '%s\n' "$ljout" | head -6
# control: the call chain really did nest before the longjmp
printf '%s\n' "$ljout" | grep -qE '^-> level_one \[' \
    || fail "longjmp: level_one not at depth 0"
printf '%s\n' "$ljout" | grep -qE '^  -> level_two \[' \
    || fail "longjmp: level_two not nested one level under level_one"
printf '%s\n' "$ljout" | grep -qE '^    -> jump_out \[' \
    || fail "longjmp: jump_out not nested two levels — real nesting is not being tracked, so the depth-0 check below would be vacuous"
# the payload: after_jump is called from main at depth 0, and those three frames
# were discarded with NO ret. A push/pop counter renders it 5+ levels deep.
printf '%s\n' "$ljout" | grep -qE '^-> after_jump \[' \
    || fail "longjmp: after_jump is not at depth 0 — the frames longjmp discarded were never unwound (a push-on-call/pop-on-ret counter cannot do this)"
printf '%s\n' "$ljout" | grep -qE '^ +-> after_jump \[' \
    && fail "longjmp: after_jump ALSO appears indented — the depth drifted after a longjmp"
echo "  nesting tracked (jump_out at depth 2) AND after_jump back at depth 0"

# COMPOSITION: --depth must measure from the RE-BASED --focus root, not from the
# real call depth (asmspy-plan Theme E). longjmp_victim's chain is
# level_one(0) -> level_two(1) -> jump_out(2) -> longjmp@plt(3), all of it code
# THIS REPO OWNS — deliberately, because the previous version of this check
# expected usleep@plt -> __nanosleep -> clock_nanosleep and so depended on
# glibc's internal routing of usleep, which differs between hosts.
#
# --focus=level_two roots at REAL depth 1, so:
#   level_two   real 1 -> eff 0   (kept, re-based)
#   jump_out    real 2 -> eff 1   (kept)  <- a cap on the REAL depth cuts this
#   longjmp@plt real 3 -> eff 2   (cut by --depth=2)
echo "--- asmspy --tree --focus=level_two --depth=2 (composition) ---"
"$BUILD/longjmp_victim" 2>/dev/null &
LJPID=$!
sleep 1
set +e
cjout=$(timeout 60 "$ASM" --tree "$LJPID" 6 --focus=level_two --depth=2 2>&1); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--tree --focus=level_two hung"
kill -9 "$LJPID" 2>/dev/null || true
wait "$LJPID" 2>/dev/null || true
LJPID=""
printf '%s\n' "$cjout" | head -3
printf '%s\n' "$cjout" | grep -qE '^-> level_two \[longjmp_victim\]' \
    || fail "--focus=level_two: root not re-based to depth 0 (it runs at real depth 1)"
# THE discriminator: jump_out is at REAL depth 2, so a --depth=2 cap applied to
# the real depth would cut it. It must survive, because its EFFECTIVE depth is 1.
printf '%s\n' "$cjout" | grep -qE '^  -> jump_out \[longjmp_victim\]' \
    || fail "--focus=level_two --depth=2: jump_out (eff depth 1, REAL depth 2) was cut — the cap is measuring the real depth, not the re-based one"
# and eff depth 2 (longjmp@plt, real depth 3) is cut: nothing indented 4+ columns
printf '%s\n' "$cjout" | grep -qE '^    -> ' \
    && fail "--focus=level_two --depth=2: a line at effective depth 2 leaked past the cap"
# the caller is outside the focus
printf '%s\n' "$cjout" | grep -q 'level_one' \
    && fail "--focus=level_two: the caller level_one leaked into the focused subtree"
echo "  composition: --depth measures from the --focus root, not the real depth"

# ---------------------------------------------------------------------------
# INDIRECT-CALL ATTRIBUTION AT A SIGNAL BOUNDARY (asmspy-plan Theme C)
# ---------------------------------------------------------------------------
# A `call *reg` carries no target in its bytes, so both stepping engines resolve
# it at the NEXT stop. Stepping the call site does not guarantee the call RETIRES:
# a signal pending at that moment is delivered first, and the stop after it is the
# HANDLER's entry. Taking that as the callee fabricates a caller -> handler edge
# the target never executed.
#
# The window is one instruction wide, so a signal storm hits it only by luck — and
# a test that only sometimes reproduces the bug it guards silently stops testing.
# ASMSPY_TEST_SIGRACE=<signo> makes it deterministic instead: the engine queues one
# signal while the tracee is stopped AT the call site with the pend armed, so the
# kernel must deliver it before the call executes. Only the signal's TIMING is
# chosen; every line of the resolution path under test is the production one.
#
# Three names, three separate things that must be true (each covers a way the
# other two could pass while testing nothing):
#   * indirect_target present  => the real edge SURVIVES. Guards against a "fix"
#                                 that just drops indirect calls, and against a
#                                 run that never traced the loop at all.
#   * handler_helper present   => the injected signal really WAS delivered and the
#                                 handler really ran under the tracer. Without it
#                                 the sig_handler_fn assertion below would pass on
#                                 a run where no race ever happened.
#   * sig_handler_fn ABSENT    => the payload. Nothing CALLS the handler, so an
#                                 edge into it can only be the fabrication.
echo "--- asmspy --tree: indirect call racing a signal (forced) ---"
"$BUILD/sigcall_victim" 2>/dev/null &
SGPID=$!
sleep 1
set +e
sgout=$(ASMSPY_TEST_SIGRACE=10 timeout 60 "$ASM" --tree "$SGPID" 40 2>&1); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--tree on sigcall_victim hung"
printf '%s\n' "$sgout" | head -5
# the forced race really happened: the handler ran under the tracer
printf '%s\n' "$sgout" | grep -q -- '-> handler_helper \[' \
    || fail "sigrace: handler_helper absent — the injected signal was never delivered, so the sig_handler_fn check below would be vacuous"
# the real edge survived the race (sigreturn re-runs the call site; it re-resolves)
printf '%s\n' "$sgout" | grep -q -- '-> indirect_target \[' \
    || fail "sigrace: indirect_target absent — indirect calls are not being attributed AT ALL (a fix that drops them is not a fix)"
# THE PAYLOAD: the handler entry must never be attributed to the call site
printf '%s\n' "$sgout" | grep -q -- '-> sig_handler_fn' \
    && fail "sigrace: caller -> sig_handler_fn edge FABRICATED — the pending indirect call resolved against a signal handler's entry instead of its callee"
echo "  handler ran + indirect_target attributed, and NO caller -> sig_handler_fn edge"

# Control: with the lever OFF, nothing in sigcall_victim raises a signal at all.
# handler_helper must therefore VANISH — which is what proves the run above was
# testing an injected race rather than passing because the trace was empty.
echo "--- asmspy --tree: same victim, NO injection (control) ---"
set +e
sgc=$(timeout 60 "$ASM" --tree "$SGPID" 40 2>&1); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--tree on sigcall_victim (control) hung"
printf '%s\n' "$sgc" | grep -q -- '-> indirect_target \[' \
    || fail "sigrace control: indirect_target absent — the victim is not being traced"
printf '%s\n' "$sgc" | grep -q -- '-> handler_helper' \
    && fail "sigrace control: handler_helper appeared with the lever OFF — a signal arrived from somewhere else, so the injected run proves nothing about injection"
echo "  no injection => no handler activity (the lever is the only signal source)"

# The GRAPH engine resolves pending indirect calls the same way and needs the same
# proof. Here the bogus edge is not a name but a COUNT: the handler legitimately
# appears as a CALLER (it calls handler_helper), so grepping for its name would
# always match. `invocations` counts exactly "was recorded as a CALLEE" — which is
# precisely the bug — so it must be 0 for the handler and nonzero for the others.
echo "--- asmspy --graph: indirect call racing a signal (forced) ---"
set +e
sgg=$(ASMSPY_TEST_SIGRACE=10 timeout 90 "$ASM" --graph "$SGPID" 60 --json 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--graph on sigcall_victim hung"
[ "$rc" -eq 0 ] || fail "--graph on sigcall_victim exited rc=$rc"
# invocations of node $1 in the --graph JSON, empty if the node is absent
ginv() {
    printf '%s' "$sgg" |
        grep -o "\"name\":\"$1\"[^}]*\"invocations\":[0-9]*" |
        sed 's/.*"invocations"://'
}
gt=$(ginv indirect_target); gh=$(ginv handler_helper); gs=$(ginv sig_handler_fn)
echo "  invocations: indirect_target=${gt:-absent} handler_helper=${gh:-absent} sig_handler_fn=${gs:-absent}"
[ -n "$gt" ] && [ "$gt" -gt 0 ] \
    || fail "sigrace graph: indirect_target has no invocations — the indirect call was never recorded, so the handler check below would be vacuous"
[ -n "$gh" ] && [ "$gh" -gt 0 ] \
    || fail "sigrace graph: handler_helper has no invocations — the injected signal was never delivered, so the handler check below would be vacuous"
[ -z "$gs" ] || [ "$gs" -eq 0 ] \
    || fail "sigrace graph: sig_handler_fn recorded as a CALLEE ($gs invocations) — a call-graph edge into a signal handler that nothing calls"
echo "  handler never recorded as a callee (graph engine verifies the same way)"
kill -9 "$SGPID" 2>/dev/null || true
wait "$SGPID" 2>/dev/null || true
SGPID=""

# ---------------------------------------------------------------------------
# EXEC-STOP RE-RESOLUTION (asmspy-plan Theme B): PTRACE_O_TRACEEXEC
# ---------------------------------------------------------------------------
# exec_victim runs preexec_fn, then execv()s exec_stage2, which runs postexec_fn.
# The two functions live in DIFFERENT binaries at different load biases, so ONE
# traced run that names BOTH can only have re-read the symbol table at the
# exec-stop: the table asmspy loaded at attach is exec_victim's and knows nothing
# of postexec_fn.
#
# The two halves are each other's control, which is what makes this airtight:
#   * preexec_fn present  => asmspy really was attached BEFORE the exec, so
#                            "postexec_fn appeared" is not just a late attach to
#                            the post-exec image.
#   * postexec_fn present => the reload happened, and it cannot be faked by the
#                            stale table.
# --graph also proves the exebase re-read: postexec_fn must be tagged INTERNAL.
# A stale exebase ("exec_victim") would label exec_stage2's own functions
# external — a wrong answer that still renders as a plausible graph.
echo "--- asmspy --graph across an execve (exec-stop re-resolution) ---"
"$BUILD/exec_victim" "$BUILD/exec_stage2" 2>/dev/null &
EXPID=$!
sleep 1
set +e
exout=$(timeout 90 "$ASM" --graph "$EXPID" 4000 --json 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--graph across execve hung"
[ "$rc" -eq 0 ] || fail "--graph across execve exited rc=$rc"
# control: the PRE-exec image was traced and resolved from the attach-time table
printf '%s' "$exout" | grep -q '"name":"preexec_fn","module":"exec_victim"' \
    || fail "exec: preexec_fn [exec_victim] absent — asmspy did not trace the pre-exec image (the post-exec proof below would be vacuous)"
echo "  pre-exec:  preexec_fn [exec_victim] resolved from the attach-time table"
# the payload: a symbol that exists ONLY in the exec'd binary
printf '%s' "$exout" | grep -q '"name":"postexec_fn","module":"exec_stage2"' \
    || fail "exec: postexec_fn [exec_stage2] absent — the symtab was NOT re-read at the exec-stop (new image named from the old binary's table)"
# and the exe basename was re-read too: stage2's own function is INTERNAL
printf '%s' "$exout" \
    | grep -qE '"name":"postexec_fn","module":"exec_stage2","kind":"internal"' \
    || fail "exec: postexec_fn not tagged internal — the exebase went stale, so the new binary's own code is labelled external"
echo "  post-exec: postexec_fn [exec_stage2] resolved + tagged internal (symtab AND exebase re-read)"
# both in ONE capture => the trace really did span the exec
printf '%s' "$exout" | grep -q '"pid":'"$EXPID" \
    || fail "exec: graph is not for the traced pid"
echo "  one capture spans both images (pid $EXPID unchanged across the exec)"

kill "$EXPID" 2>/dev/null || true
wait "$EXPID" 2>/dev/null || true
EXPID=""

# The instruction stream re-resolves too (same option, same reload path). This
# needs a FRESH victim: the run above already let the first one exec, and
# attaching to an ALREADY-exec'd process would load stage2's symbols at attach
# and resolve postexec_fn with no reload at all — a vacuously green test. The
# preexec_fn assertion below is what holds that honest.
echo "--- asmspy --stream across an execve (fresh pre-exec victim) ---"
"$BUILD/exec_victim" "$BUILD/exec_stage2" 2>/dev/null &
EXPID=$!
sleep 1
set +e
# Budget: postexec_fn is reached at ~620 steps (MEASURED, and stable across
# runs), so 20000 is a ~32x margin. It used to be 200000 against a first
# appearance at 136,072 — a 1.47x margin, 87% of which was glibc's static
# startup registering unwind tables. That is what turned CI red: the count moves
# with the environment (136,629 -> 140,893 from ifunc dispatch alone) and a
# slower runner ran out of budget before reaching the symbol. See exec_stage2.c.
sxout=$(timeout 120 "$ASM" --stream "$EXPID" 20000 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--stream across execve hung"
# control first: we were attached BEFORE the exec. (grep -c, not grep -q: -q
# exits on the first match and SIGPIPEs the printf feeding it, which dash reports
# as "printf: I/O error" — harmless, but it buries the real failure.)
printf '%s' "$sxout" | grep -c 'preexec_fn.*\[exec_victim\]' >/dev/null \
    || fail "--stream: preexec_fn absent — attached after the exec, so the postexec_fn check below would be vacuous"
printf '%s' "$sxout" | grep -c 'postexec_fn.*\[exec_stage2\]' >/dev/null \
    || fail "--stream: postexec_fn [exec_stage2] never named — no exec re-resolution in the stream engine"
echo "  stream: pre-exec AND post-exec text each named from their own image"
kill "$EXPID" 2>/dev/null || true
wait "$EXPID" 2>/dev/null || true
EXPID=""

# ---------------------------------------------------------------------------
# CHILD-PROCESS FOLLOWING: --follow (asmspy-plan Theme B, `strace -f` parity)
# ---------------------------------------------------------------------------
# fork_victim forks; parent and child run DISTINCT functions (parent_fn /
# child_fn) and each open a DIFFERENT file at the SAME fd number (3, opened
# after the fork).
#
# --follow is OPT-IN, which hands every case below a real negative control: the
# identical run without the flag must never show the child. That is the control
# the whole section rests on — "the child appeared" means nothing unless the same
# command without --follow proves it does not appear by default.
#
# fork_victim waits 2s before forking and we attach after 1, because
# PTRACE_O_TRACEFORK reports forks that happen WHILE traced and cannot adopt a
# child that already existed (the same semantics as `strace -f -p`). Attaching
# late would silently test nothing — hence the parent_fn/child_fn counts below,
# not just a "child appeared" grep.
fk_spawn() { # fk_spawn [stage2] -> $FKPID, attached-before-the-fork
    "$BUILD/fork_victim" ${1:+"$1"} 2>/dev/null &
    FKPID=$!
    sleep 1
}
fk_kill() {
    kill "$FKPID" 2>/dev/null || true # the child dies with it (PR_SET_PDEATHSIG)
    wait "$FKPID" 2>/dev/null || true
    FKPID=""
}

echo "--- asmspy --log --follow (child processes, strace -f parity) ---"
# CONTROL: no --follow -> the child must be invisible
fk_spawn
set +e
nfout=$(timeout 60 "$ASM" --log "$FKPID" 300 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--log (control) hung"
fk_kill
[ "$(printf '%s\n' "$nfout" | grep -c asmspy_fork_parent)" -gt 0 ] \
    || fail "--log control: the PARENT's own writes are missing — the run is broken, so the --follow comparison below would be meaningless"
printf '%s\n' "$nfout" | grep -q asmspy_fork_child \
    && fail "--log without --follow: the child's writes appeared — following is supposed to be OPT-IN (and this destroys the control for the check below)"
echo "  control: without --follow the forked child is invisible (parent-only)"

# --follow: BOTH processes
fk_spawn
set +e
fout=$(timeout 60 "$ASM" --log "$FKPID" 300 --follow 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--log --follow hung"
fk_kill
printf '%s\n' "$fout" | grep -q asmspy_fork_parent \
    || fail "--log --follow: parent's writes missing"
printf '%s\n' "$fout" | grep -q asmspy_fork_child \
    || fail "--log --follow: the forked child's writes never appeared (TRACEFORK not set / child not followed)"
[ "$(printf '%s\n' "$fout" | grep -oE '^\[[0-9]+\]' | sort -u | wc -l)" -ge 2 ] \
    || fail "--log --follow: expected syscalls tagged with >=2 distinct pids"
# THE fd-table assertion. Parent and child BOTH write to fd 3, pointing at
# different files. A followed child has its OWN fd table, so resolving its
# write(3) through the PARENT's /proc/<pid>/fd yields the parent's path — the
# right syscall with a confidently wrong argument, rendering as a perfectly
# plausible line. Each fd=3 must name its own process's file.
printf '%s\n' "$fout" | grep -q 'write(fd=3</tmp/asmspy_fork_child.txt>' \
    || fail "--log --follow: the child's fd 3 does not resolve to the CHILD's file — fd->path is being decoded through the parent's fd table"
printf '%s\n' "$fout" | grep -q 'write(fd=3</tmp/asmspy_fork_parent.txt>' \
    || fail "--log --follow: the parent's fd 3 no longer resolves to the parent's file"
echo "  --follow: both processes logged; each one's fd 3 resolves to its OWN file"

echo "--- asmspy --stream --follow (single-step both processes) ---"
# CONTROL first, again: the child's code must not appear by default
fk_spawn
set +e
nsout=$(timeout 90 "$ASM" --stream "$FKPID" 4000 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--stream (control) hung"
fk_kill
[ "$(printf '%s\n' "$nsout" | grep -c parent_fn)" -gt 0 ] \
    || fail "--stream control: parent_fn missing — run is broken"
printf '%s\n' "$nsout" | grep -q child_fn \
    && fail "--stream without --follow: child_fn appeared — following must be opt-in"
echo "  control: without --follow only the parent's code is stepped"

fk_spawn
set +e
fsout=$(timeout 90 "$ASM" --stream "$FKPID" 4000 --follow 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--stream --follow hung"
fk_kill
printf '%s\n' "$fsout" | grep -q parent_fn || fail "--stream --follow: parent_fn missing"
printf '%s\n' "$fsout" | grep -q child_fn \
    || fail "--stream --follow: child_fn never appeared — the forked child is not being stepped"
echo "  --follow: both the parent's and the forked child's code are stepped"

echo "--- asmspy --graph --follow with a child that EXECs (per-process symbols) ---"
# The child forks AND THEN execs exec_stage2, so the two followed processes are
# in different images at unrelated load biases: parent_fn is PIE-relocated in
# fork_victim, postexec_fn is at a static address in exec_stage2. Naming BOTH in
# one graph is only possible with a per-PROCESS symbol table — a single table
# (whichever image it came from) cannot resolve the other.
fk_spawn "$BUILD/exec_stage2"
set +e
fgout=$(timeout 90 "$ASM" --graph "$FKPID" 6000 --follow --json 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--graph --follow hung"
fk_kill
printf '%s' "$fgout" | grep -q '"name":"parent_fn","module":"fork_victim"' \
    || fail "--graph --follow: parent_fn [fork_victim] missing"
printf '%s' "$fgout" | grep -q '"name":"postexec_fn","module":"exec_stage2"' \
    || fail "--graph --follow: postexec_fn [exec_stage2] missing — a followed child that EXECs is not getting its own symbol table"
echo "  --follow + exec: two processes, two images, two symbol tables, one graph"

# --tid pins ONE task; --follow adds whole processes. Asking for both is a
# contradiction, so it is a usage error rather than a silent precedence rule.
expect_badarg "$ASM" --stream 1 --tid=1 --follow
expect_badarg "$ASM" --graph 1 --tid=1 --follow
expect_badarg "$ASM" --tree 1 --tid=1 --follow
echo "  --tid with --follow rejected (contradictory scopes)"
rm -f /tmp/asmspy_fork_parent.txt /tmp/asmspy_fork_child.txt 2>/dev/null || true

# ---------------------------------------------------------------------------
# POST-ATTACH CLONE FOLLOWING (Theme D) + thr_get OOM RELEASE (Theme C)
# ---------------------------------------------------------------------------
# clone_victim stays single-threaded until after we attach, then keeps spawning
# short-lived threads. spawned_fn runs ONLY on those post-attach clones, so it
# cannot be reached by seize_threads' one-shot /proc scan — only by
# PTRACE_O_TRACECLONE plus the clone-event handler.
#
# The two checks below are EACH OTHER'S CONTROL, and that is the point:
#
#   * no injection -> spawned_fn MUST appear (clones are followed and stepped)
#   * ASMSPY_TEST_THR_OOM=1 (only the leader may be tabled) -> spawned_fn must
#     NOT appear: a task we cannot table is RELEASED, not traced-but-untracked.
#
# Without the first, the second passes for the wrong reason — anything that
# breaks clone following at all (TRACECLONE unset, say) also yields spawned_fn=0
# and would look like a clean OOM release. MEASURED: that exact mutation does
# make the OOM check pass on its own.
#
# HONEST SCOPE. The bug this guards is that an untabled task escapes the
# two-phase detach and is left step-armed, which kills the target LATER by
# SIGTRAP. That consequence is NOT what is asserted here, because it does not
# reproduce on a simple victim — MEASURED: this victim survives with AND without
# the fix, the same limitation already recorded for the two-phase-detach
# tripwire (the crash reproduced reliably only on a real V8/Node JIT). So this
# asserts the POLICY that prevents it — untabled implies released — which is
# mutation-detectable, rather than a crash that would pass either way.
#
# ASMSPY_TEST_THR_OOM is a test-only fault-injection knob: a mid-trace
# allocation failure cannot be provoked from outside and is silent when it
# happens, so without a lever the fix could only be argued, not demonstrated.
echo "--- asmspy post-attach clone following (Theme D) ---"
"$BUILD/clone_victim" 2>/dev/null &
CLPID=$!
sleep 1
set +e
clout=$(timeout 90 "$ASM" --stream "$CLPID" 20000 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--stream on clone_victim hung"
kill -9 "$CLPID" 2>/dev/null || true
wait "$CLPID" 2>/dev/null || true
CLPID=""
printf '%s\n' "$clout" | grep -q main_fn \
    || fail "clone-follow: main_fn (the leader's own code) missing — the run is broken, so the spawned_fn check would be meaningless"
printf '%s\n' "$clout" | grep -q spawned_fn \
    || fail "clone-follow: spawned_fn never appeared — a thread created AFTER the attach is not being followed (TRACECLONE / clone-event handling)"
[ "$(printf '%s\n' "$clout" | grep -oE '^\[[0-9]+\]' | sort -u | wc -l)" -ge 2 ] \
    || fail "clone-follow: expected >=2 distinct tids once post-attach clones are followed"
echo "  post-attach clones followed (leader + spawned threads, spawned_fn stepped)"

echo "--- asmspy thr_get OOM: an untabled task is RELEASED, not left traced (Theme C) ---"
"$BUILD/clone_victim" 2>/dev/null &
CLPID=$!
sleep 1
set +e
oomout=$(ASMSPY_TEST_THR_OOM=1 timeout 90 "$ASM" --stream "$CLPID" 20000 2>/dev/null)
rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--stream under injected thr_get OOM hung"
[ "$rc" -eq 0 ] || fail "--stream under injected thr_get OOM exited rc=$rc"
# the leader IS tabled (cap=1), so it must still be traced — proving the run
# happened at all and that the injection did not simply break everything
printf '%s\n' "$oomout" | grep -q main_fn \
    || fail "thr_get OOM: the leader's own code is missing — the injection broke the whole trace, not just the untabled tasks"
# every post-attach clone is UNTABLED under cap=1, and each must therefore be
# detached on sight rather than resumed. Stepping one proves it was resumed
# while absent from the table detach_threads walks — the escape this fixes.
printf '%s\n' "$oomout" | grep -q spawned_fn \
    && fail "thr_get OOM: spawned_fn was stepped even though the task could not be tabled — an untracked task is being resumed, so it escapes the two-phase detach"
[ "$(printf '%s\n' "$oomout" | grep -oE '^\[[0-9]+\]' | sort -u | wc -l)" -le 1 ] \
    || fail "thr_get OOM: more than one tid was traced under a 1-task table cap"
echo "  untabled tasks released on sight; the tabled leader keeps tracing"
kill -9 "$CLPID" 2>/dev/null || true
wait "$CLPID" 2>/dev/null || true
CLPID=""

# ---------------------------------------------------------------------------
# fd -> ENDPOINT enrichment for sockets (asmspy-plan Theme E)
# ---------------------------------------------------------------------------
# readlink("/proc/<pid>/fd/N") on a socket yields "socket:[12345]" — an inode,
# the one thing a person watching a trace does not care about. asmspy resolves it
# through /proc/<pid>/net (the TARGET's pid, so a container's socket is looked up
# in ITS netns, not ours).
#
# The victim binds to port 0 and PRINTS the port the kernel picked, so the
# expected strings are DERIVED from the run rather than hardcoded — the
# assertions below cannot pass against a stale/wrong socket, and cannot be
# satisfied by echoing the inode back.
# ---------------------------------------------------------------------------
# SYSCALL ARGUMENT DECODING (asmspy-plan Theme E)
# ---------------------------------------------------------------------------
# argdecode_victim makes a fixed set of calls with KNOWN arguments, so each
# assertion below pins RENDERED TEXT rather than "something plausible appeared".
# Every one of these was three raw hex slots (or a bare pointer) before.
#
# The arity assertions are the point of the item: an undescribed syscall used to
# print exactly three slots and thereby ASSERT an arity it had never
# established. getpid() proves 0 is now expressible; mmap proves 6 is; the two
# opens prove it is CONDITIONAL (mode is an argument only when a creating flag
# is set); and the "..." proves an unknown shape now says so.
echo "--- asmspy --log syscall argument decoding (flags/vectors/sigsets/arity) ---"
"$BUILD/argdecode_victim" 2>/dev/null &
SGPID=$!
sleep 2
set +e
adout=$(timeout 90 "$ASM" --log "$SGPID" 400 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--log on argdecode_victim hung"
kill -9 "$SGPID" 2>/dev/null || true
wait "$SGPID" 2>/dev/null || true
SGPID=""
rm -f /tmp/asmspy_argdecode.txt
ad_has() {
    printf '%s\n' "$adout" | grep -qF "$1" \
        || { printf '%s\n' "$adout" | sort -u | head -30; \
             fail "arg decode: expected to render $2 — did not find: $1"; }
}
# flag WORDS, not hex
ad_has 'O_WRONLY|O_CREAT|O_TRUNC, 0644' "open flags + octal mode"
ad_has 'PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS' "mmap prot + map flags"
ad_has 'mprotect(' "mprotect"
ad_has ', PROT_READ) = 0' "a single-bit prot word"
# a VECTOR: the bytes, not the array's address
ad_has 'writev(fd=' "writev with a resolved fd"
ad_has '["iovec-one", "iovec-two"], 2' "the iovec CONTENTS"
# a sigset BITMASK and a how enum
ad_has 'rt_sigprocmask(SIG_BLOCK, [SIGUSR2]' "sigprocmask how + sigset members"
# a signal NUMBER as a name
ad_has ', SIGUSR1) = 0' "tgkill's signal number as a name"
# a struct read out of the target (glibc routes nanosleep -> clock_nanosleep)
ad_has '{0.002000000}' "a timespec's contents"
# enum
ad_has 'SEEK_END' "lseek whence"
# ARITY: zero, six, and conditional
ad_has 'getpid() = ' "arity ZERO (no invented argument slots)"
ad_has 'mmap(0x0, 4096,' "arity 6 (mmap's later args are no longer truncated away)"
ad_has 'O_RDONLY) = ' "a non-creating open with NO mode slot"
# mmap's fd is -1: an int arg arrives zero-extended, and must be shown as -1
ad_has 'fd=-1' "mmap's -1 fd sign-extended (not 4294967295)"
# an UNKNOWN shape must say it is unknown rather than claim three
printf '%s\n' "$adout" | grep -qE '\.\.\.\) = ' \
    || fail "arg decode: no '...' anywhere — an undescribed syscall is still claiming an arity of exactly three"
# CONTROL: the ellipsis must NOT appear on a shape we DO know, or it would just
# be decoration rather than a statement about arity.
printf '%s\n' "$adout" | grep -E '^(openat|mmap|getpid|writev|tgkill)\(' \
    | grep -q '\.\.\.' \
    && fail "arg decode: a KNOWN shape rendered '...' — the ellipsis is supposed to mark an unknown arity, not decorate every line"
echo "  flags/mode/prot/iovec/sigset/signo/timespec/whence decoded; arity 0, 6 and conditional all correct; unknown shapes say so"

echo "--- asmspy --log fd->endpoint (socket:[inode] -> real endpoint) ---"
"$BUILD/sock_victim" 2>"$BUILD/sock_victim.log" &
SKPID=$!
sleep 1
kill -0 "$SKPID" 2>/dev/null || { cat "$BUILD/sock_victim.log"; fail "sock_victim died at startup"; }
SKPORT=$(sed -n 's/.*tcp_port=\([0-9]*\).*/\1/p' "$BUILD/sock_victim.log")
[ -n "$SKPORT" ] || { cat "$BUILD/sock_victim.log"; fail "sock_victim did not report its TCP port"; }
set +e
skout=$(timeout 60 "$ASM" --log "$SKPID" 80 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--log on sock_victim hung"
[ "$rc" -eq 0 ] || fail "--log on sock_victim exited rc=$rc"
printf '%s\n' "$skout" | grep -E '^write' | sort -u | head -4

# THE negative control: the raw inode form must be GONE for these sockets. This
# is the whole item — "socket:[12345]" is what it used to print.
printf '%s\n' "$skout" | grep -q 'fd=[0-9]*<socket:\[' \
    && fail "fd->endpoint: a socket still renders as socket:[inode] — not enriched"

# a connected TCP pair: both ends are in this process, so each must show its own
# local->remote direction, and the kernel-chosen port must appear on both sides
printf '%s\n' "$skout" | grep -qE "fd=[0-9]+<TCP 127\.0\.0\.1:[0-9]+->127\.0\.0\.1:$SKPORT>" \
    || fail "fd->endpoint: the TCP client end does not render as ...->127.0.0.1:$SKPORT (the port the victim reported)"
printf '%s\n' "$skout" | grep -qE "fd=[0-9]+<TCP 127\.0\.0\.1:$SKPORT->127\.0\.0\.1:[0-9]+>" \
    || fail "fd->endpoint: the TCP server end does not render as 127.0.0.1:$SKPORT->..."
# a LISTENing socket names its state, not a bogus 0.0.0.0:0 peer
printf '%s\n' "$skout" | grep -qE "fd=[0-9]+<TCP LISTEN 127\.0\.0\.1:$SKPORT>" \
    || fail "fd->endpoint: the listening socket does not render as 'TCP LISTEN 127.0.0.1:$SKPORT'"
# an AF_UNIX socket bound to a path shows the path
printf '%s\n' "$skout" | grep -q 'fd=[0-9]*<unix:/tmp/asmspy_sock_victim.sock>' \
    || fail "fd->endpoint: the AF_UNIX socket does not render its bound path"
echo "  sockets resolved: TCP both directions + LISTEN + unix path (port $SKPORT, derived from the run)"

# The enrichment must be ADDITIVE — a regular file's fd must still resolve to
# its path. That is already asserted downstream, on syscall_victim ("write()'s
# fd is resolved to the file it points at"), which runs after this section and
# would fail if fd_endpoint() had swallowed the ordinary case. Not repeated here:
# syscall_victim is not spawned yet at this point in the script.
kill "$SKPID" 2>/dev/null || true
wait "$SKPID" 2>/dev/null || true
SKPID=""
rm -f /tmp/asmspy_sock_victim.sock "$BUILD/sock_victim.log" 2>/dev/null || true

# ---------------------------------------------------------------------------
# 32-bit (i386) TRACEE REFUSAL (asmspy-plan Theme F3)
# ---------------------------------------------------------------------------
# asmspy's engines read rip/eflags-TF/orig_rax through the x86-64 ABI and decode
# against the x86-64 syscall table. Pointed at an i386 task they do not fail —
# they report CONFIDENT NONSENSE, because the two syscall tables overlap and
# disagree (i386 4 = write, x86-64 4 = stat). So the engines read
# /proc/<pid>/exe's EI_CLASS before attaching and refuse.
#
# Dockerfile.cli installs gcc-multilib precisely so this runs for real here — a
# 32-bit process is not hardware, so it is a dependency to add, not a gate. The
# `make docker-cli` lane therefore HARD-FAILS below if the victim is missing,
# rather than quietly skipping. $ASMSPY_HAVE_M32 comes from mk/cli.mk's
# parse-time probe and is only ever non-"yes" on a toolchain without multilib
# (e.g. the bare CI runner, whose apt line needs gcc-multilib added).
if [ "${ASMSPY_HAVE_M32:-}" = "yes" ]; then
    echo "--- asmspy refuses a 32-bit (i386) tracee ---"
    [ -x "$BUILD/i386_victim" ] \
        || fail "i386_victim missing though the -m32 probe said yes — the F3 lane must not silently skip"
    # prove the fixture really is 32-bit; a 64-bit "i386_victim" would make every
    # assertion below pass for the wrong reason
    if command -v python3 >/dev/null 2>&1; then
        cls=$(python3 -c 'import sys; f=open(sys.argv[1],"rb").read(5); print(f[4])' "$BUILD/i386_victim")
        [ "$cls" = "1" ] || fail "i386_victim is not ELFCLASS32 (e_ident[EI_CLASS]=$cls) — the refusal test would be vacuous"
        echo "  fixture verified ELFCLASS32 (e_ident[EI_CLASS]=1)"
    fi
    "$BUILD/i386_victim" 2>/dev/null &
    IVPID=$!
    sleep 1
    kill -0 "$IVPID" 2>/dev/null || fail "i386_victim did not start (no 32-bit runtime?)"

    # CONTROL: the identical command against a 64-bit victim must SUCCEED, so a
    # refusal cannot be passed off by anything that breaks --log generally.
    set +e
    ok64=$(timeout 30 "$ASM" --log "$AVPID" 5 2>&1); rc64=$?
    set -e
    [ "$rc64" -eq 0 ] || fail "control: --log on the 64-bit victim failed (rc=$rc64) — the i386 refusal below would prove nothing"
    echo "  control: --log on a 64-bit tracee succeeds"

    # every ptrace engine must refuse, BEFORE attaching
    for v in "--log $IVPID 5" "--stream $IVPID 5" "--graph $IVPID 5" \
             "--tree $IVPID 5" "--procs $IVPID 5"; do
        set +e
        # shellcheck disable=SC2086
        out=$(timeout 30 "$ASM" $v 2>&1); rc=$?
        set -e
        [ "$rc" -eq 124 ] && fail "asmspy $v hung on a 32-bit tracee"
        [ "$rc" -eq 0 ] && fail "asmspy $v ACCEPTED a 32-bit tracee (rc=0) — it is decoding an i386 task against the x86-64 syscall table and reporting confident nonsense"
        printf '%s\n' "$out" | grep -qi '32-bit' \
            || { printf '%s\n' "$out" | head -3; fail "asmspy $v refused a 32-bit tracee but the message never says so (a clear message is the whole fix)"; }
    done
    echo "  --log/--stream/--graph/--tree/--procs all refuse with a clear 32-bit message"

    # the refusal must be a REFUSAL, not a failed attach: nothing was traced, so
    # the victim is untouched and still running
    kill -0 "$IVPID" 2>/dev/null \
        || fail "the 32-bit victim died — asmspy attached before refusing"
    echo "  32-bit victim untouched (refused before attach, not after)"
    kill "$IVPID" 2>/dev/null || true
    wait "$IVPID" 2>/dev/null || true
    IVPID=""
else
    # NOT a self-skip of the feature: `make docker-cli` always has gcc-multilib
    # and always runs the block above. This branch is only reachable on a
    # toolchain that cannot build ANY 32-bit binary.
    echo "--- asmspy 32-bit refusal: no -m32 toolchain here ---"
    echo "  NOT RUN on this toolchain (gcc-multilib absent). The feature is"
    echo "  covered by 'make docker-cli', whose image installs it; to cover this"
    echo "  lane too, add gcc-multilib to its apt line."
fi

# statistical hot-edge sampler: attach AMD IBS-Op to a CPU-busy victim OUT OF
# BAND (no ptrace, no single-step) and check the hot function is named. IBS-Op is
# AMD-only (and needs kernel swfilt), so on any other host / VM / non-AMD CI leg
# asmspy prints a "# SKIP" line and exits 0 — the smoke accepts that cleanly, the
# same self-skip discipline as `make ibs-test`.
"$BUILD/sample_victim" 2>/dev/null &
MVPID=$!
sleep 1
echo "--- asmspy --sample $MVPID 400 (IBS-Op hot edges, out of band) ---"
set +e
out=$(timeout 20 "$ASM" --sample "$MVPID" 400 2>&1); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--sample hung"
[ "$rc" -eq 0 ] || fail "--sample exited $rc"
printf '%s\n' "$out" | head -8
if printf '%s\n' "$out" | grep -q '^# SKIP --sample'; then
    # NOT necessarily "unavailable on this host" — that message was FALSE on an
    # AMD box whose perf is merely locked down (Docker's default seccomp blocks
    # perf_event_open, so `make docker-cli` ALWAYS lands here and every assertion
    # below is skipped: a green gate over an untested view). asmspy now prints the
    # real perf errno, so echo ITS reason rather than asserting a host property we
    # did not measure. `make docker-cli-ibs` is the lane that runs the else branch.
    printf '%s\n' "$out" | grep '^# SKIP --sample' | sed 's/^/  /'
    echo "  (sampler self-skipped — assertions below NOT run; use make docker-cli-ibs)"
else
    # on an IBS host the busy hot_spin() back-edge dominates the histogram
    printf '%s\n' "$out" | grep -q 'statistical hot edges' \
        || fail "--sample: no header"
    printf '%s\n' "$out" | grep -q 'hot_spin' \
        || fail "--sample: hot function hot_spin not named in the survey"
    # JSON export: machine-readable edges + honest provenance (pipe to jq)
    echo "--- asmspy --sample $MVPID 300 --json ---"
    jout=$(timeout 20 "$ASM" --sample "$MVPID" 300 --json 2>/dev/null) \
        || fail "--sample --json"
    printf '%s\n' "$jout" | head -4
    printf '%s\n' "$jout" | grep -q '"mode":"ibs-op"' \
        || fail "--sample --json: no mode field"
    printf '%s\n' "$jout" | grep -q '"from_name":"hot_spin' \
        || fail "--sample --json: hot_spin not resolved in edges"
fi
# a non-positive window is a bad argument (rc=2), not silently coerced
expect_badarg "$ASM" --sample "$MVPID" 0
kill "$MVPID" 2>/dev/null || true

# HARDWARE DATA WATCHPOINT (--watch): watch_victim's WORKER thread (not the leader)
# stores a known magic (0xd15ea5eddeadbeef) into a known 8-byte global. asmspy must
# arm an x86 debug register on EVERY thread — a leader-only arm would trap NONE of
# the worker's writes — PTRACE_CONT the target, and at each #DB report the faulting
# thread + PC + the value read back (process_vm_readv). On a host without real debug
# registers (qemu-user emulates zero slots) or where PTRACE_POKEUSER is refused,
# asmspy prints "# SKIP --watch" and exits 0 — the same self-skip discipline as
# --sample / --dataflow. timeout-guarded (a never-tripped watch / detach deadlock
# would otherwise hang the smoke).
echo "--- asmspy --watch (hardware data watchpoint: who touches a field + value) ---"
WLOG="$BUILD/watch_victim.log"
: > "$WLOG"
"$BUILD/watch_victim" 2>"$WLOG" &
HWPID=$!
sleep 1
kill -0 "$HWPID" 2>/dev/null || fail "watch_victim did not start"
WADDR=$(sed -n 's/.*watch_target=\(0x[0-9a-fA-F]*\).*/\1/p' "$WLOG" | head -1)
WTID=$(sed -n 's/^watch worker_tid=\([0-9][0-9]*\).*/\1/p' "$WLOG" | head -1)
[ -n "$WADDR" ] || fail "watch_victim did not report its watch_target address"
[ -n "$WTID" ] || fail "watch_victim did not report its worker tid"
echo "  watched field @ $WADDR, writer worker tid=$WTID"
set +e
wout=$(timeout 30 "$ASM" --watch "$HWPID" "$WADDR" 5 2>&1); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--watch hung (watchpoint never tripped / detach deadlock)"
printf '%s\n' "$wout" | head -10
if printf '%s\n' "$wout" | grep -q '^# SKIP --watch'; then
    echo "(hardware data watchpoints unavailable here — --watch self-skipped, OK)"
else
    # the EXACT written value was captured (post-store read out of the tracee)
    printf '%s\n' "$wout" | grep -qi 'd15ea5eddeadbeef' \
        || fail "--watch: written value 0xd15ea5eddeadbeef not captured"
    # a write-only watch is self-labelling — every hit is a store
    printf '%s\n' "$wout" | grep -q 'write' \
        || fail "--watch: hit not labelled a write"
    # PER-THREAD arming: the hit came from the WORKER thread, not the leader — proof
    # asmspy armed every task's debug registers, not just the group leader
    printf '%s\n' "$wout" | grep -qE "\[tid $WTID\]" \
        || fail "--watch: no hit from writer thread tid=$WTID (per-thread arming regressed?)"
    # the faulting PC resolves into the writer function ("who touched it")
    printf '%s\n' "$wout" | grep -q 'writer' \
        || fail "--watch: faulting PC not resolved to the writer function"

    # JSON export: one object with the hits array (pipe to jq)
    echo "--- asmspy --watch $HWPID $WADDR 3 --json ---"
    set +e
    wjout=$(timeout 30 "$ASM" --watch "$HWPID" "$WADDR" 3 --json 2>/dev/null); rc=$?
    set -e
    [ "$rc" -eq 124 ] && fail "--watch --json hung"
    printf '%s\n' "$wjout" | head -4
    printf '%s' "$wjout" | grep -q '^{"pid":' \
        || fail "--watch --json: no top-level {\"pid\":...} object"
    printf '%s' "$wjout" | grep -q '"mode":"write"' \
        || fail "--watch --json: no write mode field"
    printf '%s' "$wjout" | grep -q '"value":"0xd15ea5eddeadbeef"' \
        || fail "--watch --json: written value not exported"
    printf '%s' "$wjout" | grep -qE "\"tid\":$WTID" \
        || fail "--watch --json: no hit from the writer thread"
    printf '%s' "$wjout" | grep -q 'hit ' \
        && fail "--watch --json: human text leaked into JSON"

    # read+write watch (--rw) decodes the faulting instruction to label direction;
    # the worker's store must still resolve to a write
    echo "--- asmspy --watch $HWPID $WADDR 3 --rw (read+write, insn-decoded label) ---"
    set +e
    wrwout=$(timeout 30 "$ASM" --watch "$HWPID" "$WADDR" 3 --rw 2>&1); rc=$?
    set -e
    [ "$rc" -eq 124 ] && fail "--watch --rw hung"
    printf '%s\n' "$wrwout" | head -6
    if ! printf '%s\n' "$wrwout" | grep -q '^# SKIP --watch'; then
        printf '%s\n' "$wrwout" | grep -q 'write' \
            || fail "--watch --rw: the worker store not labelled a write (insn decode regressed)"
    fi
fi
# the watched target must SURVIVE the arm + detach cycles (debug registers disarmed,
# two-phase detach) — a regression that left a slot armed would kill it by SIGTRAP
kill -0 "$HWPID" 2>/dev/null \
    || fail "--watch: target KILLED by the watch/detach cycle (debug regs not disarmed?)"
# a MISALIGNED watch address is rejected (x86 needs a length-aligned address), and a
# bad --len is a usage error (rc=2) — neither is silently coerced
"$ASM" --watch "$HWPID" 0x1 --len=8 >/dev/null 2>&1 \
    && fail "--watch accepted a misaligned watch address"
expect_badarg "$ASM" --watch "$HWPID" "$WADDR" --len=3
echo "  --watch: value + PC captured, per-thread arming confirmed, target survived"
kill "$HWPID" 2>/dev/null || true
rm -f "$WLOG"

# syscall log: attach to syscall_victim (does file I/O each loop)
"$BUILD/syscall_victim" 2>/dev/null &
SVPID=$!
sleep 1

echo "--- asmspy --log $SVPID 20 ---"
out=$("$ASM" --log "$SVPID" 20 2>&1) || true
printf '%s\n' "$out"
# write()'s fd is resolved to the file it points at (strace -y style) — the fd is
# still open at the syscall's exit-stop, so /proc/<pid>/fd/<n> readlinks. This
# asserts both that write() is captured AND that fd->path resolution works.
printf '%s\n' "$out" | grep -qE 'write\(fd=[0-9]+</tmp/asmtest_syscall_demo.txt>' \
    || fail "write() fd not resolved to its path (strace -y decode regressed)"
# The victim's access() is named from the generated syscall table (it is not one
# of the hand-decoded four) and its path is decoded. glibc routes access() to
# either SYS_access or SYS_faccessat2 depending on version, so accept both --
# both prove naming + path decoding, which is the point.
printf '%s\n' "$out" | grep -qE '(access|faccessat2?)\(.*"/tmp/asmtest_syscall_demo.txt"' \
    || fail "access() not named+path-decoded (syscall table / path decode regressed)"

# thread following: a MULTI-threaded victim (main + 3 workers, all syscalling).
# asmspy must SEIZE every thread and tag each line "[tid]", so we see >1 distinct
# tid. timeout-guarded: a thread-follow deadlock would otherwise hang the smoke.
"$BUILD/threads_victim" 2>/dev/null &
TVPID=$!
sleep 1
echo "--- asmspy --log $TVPID 80 (follow all threads) ---"
set +e
out=$(timeout 30 "$ASM" --log "$TVPID" 80 2>&1); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--log hung on a multi-threaded target (thread-follow deadlock)"
printf '%s\n' "$out" | head -8
ntids=$(printf '%s\n' "$out" | sed -n 's/^\[\([0-9][0-9]*\)\].*/\1/p' | sort -u | wc -l)
echo "distinct tids seen: $ntids"
[ "$ntids" -ge 2 ] || fail "expected syscalls from >=2 threads, saw $ntids (thread-follow regressed)"

# The instruction stream follows every thread too: single-step them all and tag
# each line "[tid]". Same victim; expect >1 distinct tid and real disassembly.
echo "--- asmspy --stream $TVPID 150 (follow all threads) ---"
set +e
out=$(timeout 30 "$ASM" --stream "$TVPID" 150 2>&1); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--stream hung on a multi-threaded target (thread-follow deadlock)"
printf '%s\n' "$out" | head -6
stids=$(printf '%s\n' "$out" | sed -n 's/^\[\([0-9][0-9]*\)\].*/\1/p' | sort -u | wc -l)
echo "distinct tids in stream: $stids"
[ "$stids" -ge 2 ] || fail "stream expected >=2 threads, saw $stids (thread-follow regressed)"
printf '%s\n' "$out" | grep -qE 'mov|jmp|cmp|add|push|call|lea|test|sub|nop' \
    || fail "multi-thread stream: no disassembly"

# process/thread topology: the same multi-threaded victim must render as ONE
# process node with its threads listed underneath, each with an invocation count.
echo "--- asmspy --procs $TVPID 120 (process/thread topology) ---"
# Retry a transient EMPTY topology ("0 tasks observed") — under heavy load the
# attach/first-syscall-count window occasionally comes up empty; the assertions
# below still hold on any non-empty capture.
out=""
for try in 1 2 3; do
    set +e
    out=$(timeout 30 "$ASM" --procs "$TVPID" 120 2>&1); rc=$?
    set -e
    [ "$rc" -eq 124 ] && fail "--procs hung on a multi-threaded target"
    printf '%s\n' "$out" | grep -qE "^node $TVPID \[threads_victim\]  inv=[0-9]" && break
    sleep 1
done
printf '%s\n' "$out" | head -8
printf '%s\n' "$out" | grep -qE "^node $TVPID \[threads_victim\]  inv=[0-9]" \
    || fail "--procs: no process node with a syscall count (empty across retries)"
nt=$(printf '%s\n' "$out" | grep -cE 'tid [0-9]+.*inv=[0-9]')
echo "thread rows: $nt"
[ "$nt" -ge 2 ] || fail "--procs: expected >=2 thread rows, saw $nt"
# threads (and child processes) are drawn under their process with box-tree
# glyphs (├─ for a sibling with more below, └─ for the last)
printf '%s\n' "$out" | grep -qE '(├─|└─) tid [0-9]' \
    || fail "--procs: thread rows not drawn with tree glyphs (├─/└─)"
# calls mode (single-step) also produces a counted topology
out2=$(timeout 40 "$ASM" --procs "$TVPID" 60 --count=calls 2>&1) \
    || fail "--procs --count=calls"
printf '%s\n' "$out2" | grep -qE '^node [0-9]+.*inv=[0-9]' \
    || fail "--procs --count=calls: no counted node"
# --procs JSON export (asmspy-plan Theme E): the flat TASK list, which is what
# the engine actually observed — the forest the human view draws is derivable
# from tgid+ppid, so exporting a rendered tree would throw information away and
# force a consumer to re-parse box glyphs.
echo "--- asmspy --procs $TVPID 120 --json (machine-readable export) ---"
set +e
pjout=$(timeout 60 "$ASM" --procs "$TVPID" 120 --json 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--procs --json hung"
[ "$rc" -eq 0 ] || fail "--procs --json exited rc=$rc"
printf '%s\n' "$pjout" | head -3
printf '%s' "$pjout" | grep -q '^{"pid":' || fail "--procs --json: no top-level {\"pid\":...} object"
printf '%s' "$pjout" | grep -q '"tasks":\[' || fail "--procs --json: no tasks array"
# "count" must be exported: inv means something DIFFERENT per mode, and a bare
# number that silently switches meaning is what an exporter must not emit
printf '%s' "$pjout" | grep -q '"count":"syscalls"' \
    || fail "--procs --json: the count mode is not exported (inv would be ambiguous)"
printf '%s' "$pjout" | grep -qE '"tid":[0-9]+,"tgid":[0-9]+,"ppid":[0-9]+,"leader":(true|false),"comm":"' \
    || fail "--procs --json: per-task fields missing"
# the human tree must NOT leak into JSON mode (box glyphs / the header line)
printf '%s' "$pjout" | grep -q 'process/thread topology' && fail "--procs --json: human header leaked into JSON"
printf '%s' "$pjout" | grep -q 'node ' && fail "--procs --json: human tree rows leaked into JSON"
if command -v python3 >/dev/null 2>&1; then
    printf '%s' "$pjout" | python3 -c 'import json,sys
d = json.load(sys.stdin)
assert d["tasks"], "no tasks"
assert all(k in d["tasks"][0] for k in ("tid","tgid","ppid","leader","comm","exe","inv"))
# the victim is multi-threaded: exactly one leader, several tasks, all one tgid
leaders = [t for t in d["tasks"] if t["leader"]]
assert len(leaders) == 1, "expected exactly 1 leader, got %d" % len(leaders)
assert len(d["tasks"]) >= 2, "expected the threads to be exported too"
assert leaders[0]["tid"] == leaders[0]["tgid"], "leader tid != tgid"
assert all(t["tgid"] == leaders[0]["tgid"] for t in d["tasks"]), "tasks span >1 process"
assert sum(t["inv"] for t in d["tasks"]) > 0, "every task exported inv=0"
print("  json validated (python3: %d tasks, 1 leader, tid==tgid, inv>0)" % len(d["tasks"]))' \
        || fail "--procs --json: not well-formed / task invariants violated"
else
    echo "  json structural checks passed (python3 absent; strict parse skipped)"
fi

# --procs DOT export: the process forest as a Graphviz digraph. Processes are
# boxes, threads are dashed-edged ellipses — the two kinds of "child" the human
# view stacks in one glyph column stay distinguishable here.
echo "--- asmspy --procs $TVPID 120 --dot (Graphviz export) ---"
set +e
pdout=$(timeout 60 "$ASM" --procs "$TVPID" 120 --dot 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--procs --dot hung"
printf '%s\n' "$pdout" | head -4
printf '%s' "$pdout" | grep -q '^digraph asmspy {' || fail "--procs --dot: not a digraph"
printf '%s' "$pdout" | grep -qE "\"p$TVPID\" \[label=\"$TVPID \[threads_victim\]" \
    || fail "--procs --dot: the traced process node is missing/mislabelled"
printf '%s' "$pdout" | grep -qE '"t[0-9]+" \[label="tid [0-9]+' \
    || fail "--procs --dot: no thread nodes (the victim is multi-threaded)"
printf '%s' "$pdout" | grep -qE "\"p$TVPID\" -> \"t[0-9]+\" \[style=dashed\]" \
    || fail "--procs --dot: no process->thread edges"
printf '%s' "$pdout" | grep -q '^}' || fail "--procs --dot: unterminated digraph"
if command -v dot >/dev/null 2>&1; then
    printf '%s' "$pdout" | dot -Tsvg >/dev/null 2>&1 || fail "--procs --dot: graphviz rejected the output"
    echo "  dot validated (graphviz dot -Tsvg)"
else
    echo "  dot structural checks passed (graphviz absent)"
fi

# a bad --count value is rejected up front (rc=2)
expect_badarg "$ASM" --procs "$TVPID" --count=bogus
kill "$TVPID" 2>/dev/null || true

# C++ DEMANGLING: cpp_victim's hot function demo::hot_loop(int) keeps a MANGLED
# ELF symbol (_ZN4demo8hot_loopEi). asmspy's resolver must demangle it at the
# sym_push chokepoint, so --syms shows the human-readable signature and the raw
# mangled form never leaks through.
"$BUILD/cpp_victim" 2>/dev/null &
CVPID=$!
sleep 1
echo "--- asmspy --syms $CVPID hot_loop (C++ demangling) ---"
out=$("$ASM" --syms "$CVPID" hot_loop 2>/dev/null) || fail "--syms on cpp_victim"
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -q 'demo::hot_loop(int)' \
    || fail "C++ symbol not demangled (expected 'demo::hot_loop(int)')"
printf '%s\n' "$out" | grep -q '_ZN4demo' \
    && fail "C++ symbol left mangled (_ZN4demo... leaked through the resolver)"
kill "$CVPID" 2>/dev/null || true

# JIT / perf-map symbol resolution: jit_victim mmaps an ANONYMOUS executable
# region, runs a hot loop there, and registers it in /tmp/perf-<pid>.map as
# "jit_hot_loop" — exactly what Node/V8, .NET, and OpenJDK do for JIT-compiled
# code. That region is invisible to the ELF symtab, so without perf-map
# resolution asmspy renders it as a bare "0x..". Assert --stream names it
# "[jit]" and --graph tags the method [JIT].
"$BUILD/jit_victim" 2>/dev/null &
JVPID=$!
sleep 1
echo "--- asmspy --stream $JVPID 400 (JIT/perf-map naming) ---"
out=$("$ASM" --stream "$JVPID" 400 2>&1) || true
printf '%s\n' "$out" | grep -m3 jit_hot_loop || true
printf '%s\n' "$out" | grep -qE 'jit_hot_loop.*\[jit\]' \
    || fail "JIT region not named from the perf-map (expected 'jit_hot_loop ... [jit]')"

echo "--- asmspy --graph $JVPID 5 (JIT method tagged [JIT]) ---"
set +e
gout=$(timeout 40 "$ASM" --graph "$JVPID" 5 2>&1); grc=$?
set -e
[ "$grc" -eq 124 ] && fail "--graph hung on jit_victim (whole-process single-step)"
printf '%s\n' "$gout" | grep -m3 jit_hot_loop || true
printf '%s\n' "$gout" | grep -qE '\[JIT\][^Z]*jit_hot_loop' \
    || fail "JIT method not tagged [JIT] in the call graph"
kill "$JVPID" 2>/dev/null || true
rm -f "/tmp/perf-$JVPID.map"

# BINARY jitdump resolution: jitdump_victim publishes the same anonymous hot
# loop via perf's binary jit-<pid>.dump format instead — created in $BUILD (a
# non-/tmp directory, so the /tmp fallback can't find it) and discovered the
# way perf discovers it: the victim mmaps the file's header page and asmspy
# spots the filename in /proc/<pid>/maps. NO text perf-map exists, so the name
# can only have come from the jitdump reader.
"$BUILD/jitdump_victim" "$BUILD" 2>/dev/null &
UPID=$!
sleep 1
kill -0 "$UPID" 2>/dev/null || fail "jitdump_victim did not start"
[ -e "/tmp/perf-$UPID.map" ] && fail "jitdump_victim unexpectedly wrote a text perf-map"
echo "--- asmspy --stream $UPID 400 (binary jitdump naming, maps-discovered) ---"
out=$("$ASM" --stream "$UPID" 400 2>&1) || true
printf '%s\n' "$out" | grep -m3 dump_hot_loop || true
printf '%s\n' "$out" | grep -qE 'dump_hot_loop.*\[jit\]' \
    || fail "JIT region not named from the jitdump (expected 'dump_hot_loop ... [jit]')"
kill "$UPID" 2>/dev/null || true
rm -f "$BUILD/jit-$UPID.dump"

# PER-THREAD (--tid) FILTER: tid_victim runs two threads in DISTINCT functions
# (alpha_work / beta_work). --stream with no filter steps the whole process, so
# BOTH appear; --stream --tid=<alpha-tid> must step ONLY that thread, so alpha_work
# appears and beta_work NEVER does (and, being single-thread, no "[tid]" prefix).
echo "--- asmspy --stream --tid= (per-thread filter) ---"
TLOG="$BUILD/tid_victim.log"
: > "$TLOG"
"$BUILD/tid_victim" 2>"$TLOG" &
YPID=$!
# BARRIER, not a bet: each worker prints its tid as the first thing it does, so
# "both tids printed" means both threads exist and are entering their work loops.
# A bare `sleep 1` only assumes they got there — and everything below depends on
# both being runnable, so that assumption is the test's foundation, not a detail.
ATID=""; BTID=""
_i=0
while [ "$_i" -lt 100 ]; do
    ATID=$(sed -n 's/^alpha tid=\([0-9][0-9]*\).*/\1/p' "$TLOG" | head -1)
    BTID=$(sed -n 's/^beta tid=\([0-9][0-9]*\).*/\1/p' "$TLOG" | head -1)
    [ -n "$ATID" ] && [ -n "$BTID" ] && break
    _i=$((_i + 1))
    sleep 0.1
done
kill -0 "$YPID" 2>/dev/null || fail "tid_victim did not start"
[ -n "$ATID" ] || fail "tid_victim did not report alpha's tid"
[ -n "$BTID" ] || fail "tid_victim did not report beta's tid"
# control: whole-process stream sees BOTH distinct functions
set +e
cout=$(timeout 40 "$ASM" --stream "$YPID" 800 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--stream (control) hung on tid_victim"
printf '%s\n' "$cout" | grep -q 'alpha_work' || fail "--stream control: alpha_work missing"
printf '%s\n' "$cout" | grep -q 'beta_work' || fail "--stream control: beta_work missing"
# filtered: only alpha's thread is stepped -> alpha_work yes, beta_work never.
# Retry a transient EMPTY capture (a load-induced attach flake, same class as the
# other single-step steps under heavy load); the invariants below are the real
# assertions — a retry can't turn a leaked beta_work into a pass.
echo "  filtering to alpha tid=$ATID"
fout=""
for try in 1 2 3; do
    set +e
    fout=$(timeout 40 "$ASM" --stream "$YPID" 800 --tid="$ATID" 2>/dev/null)
    set -e
    printf '%s\n' "$fout" | grep -q 'alpha_work' && break
    sleep 1
done
printf '%s\n' "$fout" | grep -m3 alpha_work || true
printf '%s\n' "$fout" | grep -q 'alpha_work' \
    || fail "--tid: alpha_work never seen across retries (filter stepped the wrong thread?)"
printf '%s\n' "$fout" | grep -q 'beta_work' \
    && fail "--tid: beta_work leaked — a thread other than tid=$ATID was stepped"
printf '%s\n' "$fout" | grep -qE '^\[[0-9]+\]' \
    && fail "--tid: unexpected [tid] prefix — more than one thread was followed"
# a bad --tid value is rejected up front (rc=2)
expect_badarg "$ASM" --stream "$YPID" --tid=nope
echo "  per-thread filter: alpha only (beta_work absent)"

# MULTI-THREAD [tid] TAGGING for --tree (asmspy-plan Theme D). The tree prefixes
# every line with "[tid] " once it follows more than one thread — without it, two
# threads' call trees interleave into one indented column that reads like a
# single nonsensical call chain. tid_victim runs alpha_work and beta_work on
# DISTINCT threads, so both must appear, each tagged, under >=2 distinct tids.
#
# Why this is now a real margin rather than a lucky one. --tree counts CALLS, and
# tid_victim's main() used to nanosleep(5ms) in a loop, emitting FOUR call lines
# every 5ms for free (a sleeping thread costs the single-stepper nothing) while
# each worker emitted ONE line per ~80,000 steps. MEASURED: 34 of 40 lines were
# main's, and the workers contributed 2-4 apiece — entirely from the attach
# transient. Worse, main's rate is WALL-CLOCK bound and the workers' is
# stepper-throughput bound, so on a slower box main's share only grows: at
# --cpus=0.2 a worker was observed contributing ZERO lines. Worst of all, "main +
# one worker" already satisfies ">=2 tids", so the check passed while the thing
# it names was absent.
#
# main now blocks in pause() (zero call lines) and the workers make many small
# calls, so the workers are the ONLY sources and split the window ~50/50.
# MEASURED after the fix: 21/19, 20/20, 21/19 with main at 0; both tids appear
# within 2 lines and both *_work names within 4. 200 is a ~100x/50x margin and
# still runs in ~0.2s (it was ~0.45s at 40).
echo "--- asmspy --tree $YPID (multi-thread [tid] tagging) ---"
set +e
tt=$(timeout 60 "$ASM" --tree "$YPID" 200 2>/dev/null); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--tree on tid_victim hung"
printf '%s
' "$tt" | head -3
printf '%s
' "$tt" | grep -qE '^\[[0-9]+\] '     || fail "--tree: no [tid] prefix on a multi-threaded target — two threads' trees would interleave indistinguishably"
[ "$(printf '%s
' "$tt" | grep -oE '^\[[0-9]+\]' | sort -u | wc -l)" -ge 2 ]     || fail "--tree: expected entries from >=2 distinct tids on a multi-threaded target"
# NAME the two threads, do not just count tids. ">=2 distinct tids" was
# satisfiable by "main + one worker" — MEASURED at --cpus=0.1 before this fix:
# 39 lines from main and 1 from a single worker PASSED the count while the OTHER
# worker was entirely absent. Requiring alpha's and beta's own tids (from the
# barrier above) is what the check always meant, and no bystander thread can
# satisfy it.
printf '%s\n' "$tt" | grep -qE "^\[$ATID\] " \
    || fail "--tree: no entries tagged with alpha's tid ($ATID) — that thread was not followed/tagged"
printf '%s\n' "$tt" | grep -qE "^\[$BTID\] " \
    || fail "--tree: no entries tagged with beta's tid ($BTID) — that thread was not followed/tagged"
# and the tags are real: the two threads' DISTINCT functions each appear
printf '%s
' "$tt" | grep -q 'alpha_work' || fail "--tree: alpha_work missing"
printf '%s
' "$tt" | grep -q 'beta_work' || fail "--tree: beta_work missing"
# control: with --tid the tree follows ONE thread, so the prefix is gone again
set +e
tt1=$(timeout 60 "$ASM" --tree "$YPID" 20 --tid="$ATID" 2>/dev/null)
set -e
printf '%s
' "$tt1" | grep -qE '^\[[0-9]+\] '     && fail "--tree --tid: a single-thread trace must NOT carry the [tid] prefix"
echo "  --tree: [tid]-tagged across >=2 threads; single-thread trace unprefixed"
kill "$YPID" 2>/dev/null || true
rm -f "$TLOG"

# REGION SAMPLING ON A WORKER THREAD (asmspy-plan Theme B).
#
# tid_victim's main() only sleeps: alpha_work/beta_work run ONLY on worker
# threads. The pre-Theme-B engine attached the thread-group LEADER and ran it to
# the region, so this returned ASMSPY_REGION_NEVER_RAN — the exact shape of a
# managed method, which almost never runs on the leader. The engine now SEIZEs
# every thread and races them to the entry, so the arriving worker is sampled.
echo "--- asmspy --trace on a WORKER-thread function (Theme B) ---"
TLOG="$BUILD/tid_victim.log"
: > "$TLOG"
"$BUILD/tid_victim" 2>"$TLOG" &
YPID=$!
sleep 1
kill -0 "$YPID" 2>/dev/null || fail "tid_victim did not start"
ATID=$(sed -n 's/^alpha tid=\([0-9][0-9]*\).*/\1/p' "$TLOG" | head -1)
BTID=$(sed -n 's/^beta tid=\([0-9][0-9]*\).*/\1/p' "$TLOG" | head -1)
[ -n "$ATID" ] && [ -n "$BTID" ] || fail "tid_victim did not report both worker tids"

set +e
wout=$(timeout 40 "$ASM" --trace "$YPID" alpha_work 2 2>&1); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--trace hung on a worker-thread region"
printf '%s\n' "$wout" | grep -qE '^sample #1' \
    || fail "--trace: worker-thread alpha_work never sampled (leader-only regression?)"
printf '%s\n' "$wout" | grep -q 'never executed' \
    && fail "--trace: reported NEVER_RAN for a function a worker runs constantly"
echo "  worker-thread region sampled (no longer NEVER_RAN)"

# --tid pins the sample to one thread. Pinning to the thread that DOES run the
# region samples it; pinning to the one that never does must report "never
# executed" — promptly and without perturbing the busy non-target thread. (This
# path arms a per-thread HARDWARE breakpoint precisely so the non-target thread
# is never trapped and stepped back over a shared int3, which does not converge.)
set +e
pout=$(timeout 40 "$ASM" --trace "$YPID" alpha_work 1 --tid="$ATID" 2>&1); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--trace --tid=<runner> hung"
printf '%s\n' "$pout" | grep -qE '^sample #1' \
    || fail "--trace --tid=$ATID: alpha_work not sampled on the thread that runs it"
set +e
nout=$(timeout 40 "$ASM" --trace "$YPID" alpha_work 1 --tid="$BTID" 2>&1); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--trace --tid=<non-runner> hung (entry wait not bounded?)"
printf '%s\n' "$nout" | grep -qE '^sample #' \
    && fail "--trace --tid=$BTID: sampled alpha_work on a thread that never runs it"
printf '%s\n' "$nout" | grep -q 'never executed' \
    || fail "--trace --tid=$BTID: no honest never-executed report"
echo "  --tid pins the sample (runner sampled; non-runner reports never-executed)"

# THE TARGET MUST OUTLIVE US. This is the assertion that matters: an entry trap
# left behind on detach does NOT fail loudly — the victim runs on and dies of
# SIGTRAP (exit 133) on its NEXT arrival at the region, seconds later and
# seemingly unrelated to the tool. Both the shared int3 and the debug registers
# survive PTRACE_DETACH, and both are refused on a RUNNING thread, so a disarm
# that skips stopping the thread first still passes an immediate liveness check.
# Sleep past the region's next call before asserting.
sleep 2
kill -0 "$YPID" 2>/dev/null \
    || fail "tid_victim DIED after --trace detached (entry trap left armed?)"
echo "  target survived 4 attach/detach cycles + settle (no trap left armed)"
expect_badarg "$ASM" --trace "$YPID" alpha_work --tid=nope
kill "$YPID" 2>/dev/null || true
rm -f "$TLOG"

# TWO-PHASE DETACH: assert a traced target SURVIVES detach.
#
# A whole-process single-step run leaves threads SEIZEd and Trap-Flag-armed;
# detach_threads() must interrupt + clear TF + release them all at once, or a
# thread can resume TF-armed / mid-step-inconsistent and die by a fatal SIGTRAP
# that tears down the whole target. Every OTHER victim in this smoke is killed
# right after tracing, so a detach that silently kills the target would look like
# SUCCESS — this block is the one place that asserts the target is STILL ALIVE
# after a single-step trace + detach, across two engines (stream + tree).
#
# Scope (honest): the historical fatal SIGTRAP (commit 6aaad45) reproduced
# RELIABLY only on a real JIT — V8/Node's own cross-thread self-check int3s —
# which can't be scripted safely here; a plain compute victim exits cleanly even
# against the pre-fix one-at-a-time detach. So this is a happy-path survival
# TRIPWIRE for gross detach regressions, NOT a deterministic reproducer of that
# JIT crash. The victim installs no SIGTRAP handler, so a fatal detach-SIGTRAP
# would actually kill it (a handler would mask it). Timeout-guarded, since a
# detach deadlock would otherwise hang the smoke.
echo "--- asmspy two-phase detach: target survives single-step + detach ---"
"$BUILD/threads_victim" 2>/dev/null &
DVPID=$!
sleep 1
kill -0 "$DVPID" 2>/dev/null || fail "detach-survival: victim did not start"
# --stream (instruction-stepped) and --tree (call-stepped) exercise the shared
# two-phase detach_threads() from the two different whole-process single-step
# engines; the small --tree budget keeps it quick on a compute-heavy victim.
for view in --stream --tree; do
    case $view in
        --stream) ct=200 ;; # instruction budget — reached instantly
        --tree)   ct=5 ;;   # call budget — small, since calls are sparse here
    esac
    set +e
    timeout 40 "$ASM" "$view" "$DVPID" "$ct" >/dev/null 2>&1
    rc=$?
    set -e
    [ "$rc" -eq 124 ] && fail "detach-survival: $view hung on the multi-threaded victim"
    # THE ASSERTION: the target is still alive after asmspy single-stepped every
    # thread and detached. A regression in detach_threads() kills it here.
    kill -0 "$DVPID" 2>/dev/null \
        || fail "detach-survival: victim KILLED by $view detach (two-phase detach regressed)"
    echo "  survived $view detach"
done
kill "$DVPID" 2>/dev/null || true

# APP-DELIVERED SIGTRAP (si_code split): int3_victim executes its OWN int3
# software breakpoints under a SIGTRAP handler. A single-step engine must tell an
# app-delivered SIGTRAP (an executed int3, si_code SI_KERNEL) from its own step
# trap (TRAP_TRACE / TRAP_BRKPT) and RE-INJECT it via PTRACE_CONT. Two ways to get
# it wrong, both caught here: SWALLOWING the trap (the app's handler never runs, so
# it prints SWALLOWED), or re-injecting via SINGLESTEP (a fatal #DB in the masked
# handler KILLS the victim). Trace under --stream and --procs --count=calls (the
# two engine shapes) and assert the victim SURVIVES and never prints SWALLOWED.
#
# NB: CONT-delivery ends fine-grained stepping of that thread until its next stop,
# so on a looping-int3 victim asmspy won't reach its step budget — the runs are
# time-bounded and their exit codes deliberately ignored; the INVARIANTS (alive +
# no SWALLOWED) are the assertions.
echo "--- asmspy app-int3 re-injection (si_code split) ---"
SWLOG="$BUILD/int3_swallow.log"
: > "$SWLOG"
"$BUILD/int3_victim" >"$SWLOG" 2>/dev/null &
IPID=$!
sleep 1
kill -0 "$IPID" 2>/dev/null || fail "int3_victim did not start"
for view in "--stream $IPID 100000" "--procs $IPID 100000 --count=calls"; do
    # shellcheck disable=SC2086  # deliberate word-split of "<view> <pid> <count> ..."
    timeout 4 "$ASM" $view >/dev/null 2>&1 || true
    kill -0 "$IPID" 2>/dev/null \
        || fail "int3_victim KILLED tracing '$view' (SINGLESTEP re-inject / fatal-SIGTRAP regression)"
    echo "  survived '$view'"
done
# A genuine swallow regression drops EVERY int3 (~140 SWALLOWED across the two
# runs, measured). A rare tracer-kill detach-race — timeout's SIGTERM landing in
# the GETSIGINFO->CONT window, so the kernel's auto-detach drops one in-flight
# SIGTRAP — can contribute at most ~1 per engine. Assert a THRESHOLD so the check
# keeps full teeth against the regression without flaking on that race.
nsw=$(grep -c SWALLOWED "$SWLOG" 2>/dev/null || true)
[ "${nsw:-0}" -lt 10 ] \
    || fail "app int3 repeatedly SWALLOWED ($nsw) — si_code split regressed (trap not re-injected)"
echo "  app int3 delivered (SWALLOWED=${nsw:-0}; <10 tolerates the tracer-kill detach-race)"
kill "$IPID" 2>/dev/null || true
rm -f "$SWLOG"

echo "cli-smoke: PASS"
