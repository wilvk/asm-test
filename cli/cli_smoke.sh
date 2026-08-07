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
echo "--- test_arch (register/step/watch arch seam + AArch64 DBGWCR encoder) ---"
"$BUILD/test_arch" || fail "test_arch"
echo "--- test_logview (TUI scrollback math) ---"
"$BUILD/test_logview" || fail "test_logview"
echo "--- test_graphsort (call-graph sort comparator) ---"
"$BUILD/test_graphsort" || fail "test_graphsort"
echo "--- test_ghash (graph table index: forced-collision probe/grow) ---"
"$BUILD/test_ghash" || fail "test_ghash"
echo "--- test_sha256 (code-header routine-identity digest vs FIPS vectors) ---"
"$BUILD/test_sha256" || fail "test_sha256"
echo "--- test_jitdump (binary jitdump reader + JIT resolve chain) ---"
"$BUILD/test_jitdump" || fail "test_jitdump"
echo "--- test_view (data-flow view: annotation + def-use split + L2 slice) ---"
"$BUILD/test_view" || fail "test_view"
echo "--- test_asmtrace (.asmtrace writer envelope + the schema doc's example) ---"
"$BUILD/test_asmtrace" || fail "test_asmtrace"
echo "--- test_treefilter (call-tree depth cap / symbol focus / module filter) ---"
"$BUILD/test_treefilter" || fail "test_treefilter"
echo "--- test_symtab (symbol reverse lookup: gaps, zero-size, boundaries) ---"
"$BUILD/test_symtab" || fail "test_symtab"
# The --dataflow --auto region picker AND the mode-7 hot-edge drill-in decision
# (asmspy_edge_drill) are pure — the sampler feeding them is AMD-IBS hardware that
# self-skips off an AMD host, so these checks carry the real burden on every host.
# (Built by the cli-smoke prereqs; it was previously built but never RUN.)
echo "--- test_autoregion (--auto region picker + hot-edge drill-in decision) ---"
"$BUILD/test_autoregion" || fail "test_autoregion"
# The PERF-FREE picker (asmspy_ptrace_sample). This one CANNOT be pure: what is
# under test is what ptrace + /proc can observe about a live process, on a host
# where perf_event_open is refused (perf_event_paranoid=4 is Ubuntu's
# compiled-in default). It spawns its own victims — auto_victim for the
# residency-vs-entry disagreement, sigload_victim for signal re-injection,
# clone_victim for post-seize thread following.
echo "--- test_ptracesample (perf-free region picker: residency -> calls -> int3) ---"
"$BUILD/test_ptracesample" "$BUILD" || fail "test_ptracesample"
# libasmspy stands on its own: this binary links the engine LIBRARY and neither
# asmspy.o nor ncurses, so it is the only check that would catch the engine
# quietly depending on the CLI front end. It spawns its own victim.
echo "--- test_libasmspy (the engine library: resolver + one engine, standalone) ---"
"$BUILD/test_libasmspy" "$BUILD/spy_victim" || fail "test_libasmspy"

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

# Dumpability guard (T10, M16): a file-capability execve (`setcap
# cap_perfmon+ep asmspy`) marks the process non-dumpable, which flips
# /proc/self/* to root:root and makes codeimage's soft-dirty probe
# (src/codeimage.c) EACCES -- silently killing every 3D scene while blaming a
# kernel config that is actually enabled. main() now restores dumpability
# with a guarded prctl(PR_SET_DUMPABLE,1) before any capture. Reproducing the
# real trigger needs `setcap` (root) this suite does not have -- that A/B is
# recorded as an OUTSTANDING measurement in task-10-report.md, with the exact
# commands to run on a sudo-capable box.
#
# What CAN be proven unprivileged: PR_SET_DUMPABLE(0) needs no capability (any
# process can un-dump itself), so ASMSPY_TEST_FORCE_NONDUMPABLE forces the
# exact starting condition the guard's `!= 1` branch exists to detect, and
# asmspy reports both sides of the guard on stderr under that lever (silent
# otherwise -- the env var is unset on every real invocation above and below
# this block). This proves the guard's OWN logic restores dumpability from a
# genuinely non-dumpable start; it does NOT prove a setcap'd binary starts
# non-dumpable in the first place (that part of M16 is the outstanding sudo
# measurement).
echo "--- asmspy dumpability guard: forced PR_SET_DUMPABLE(0) must self-restore (T10) ---"
dbg=$(ASMSPY_TEST_FORCE_NONDUMPABLE=1 "$ASM" --list 2>&1 >/dev/null) || fail "dumpability guard: --list exited nonzero under the test lever"
printf '%s\n' "$dbg"
printf '%s\n' "$dbg" | grep -q 'pre-guard dumpable=0' \
    || fail "dumpability guard: the test lever did not actually force PR_SET_DUMPABLE(0) before the guard ran"
printf '%s\n' "$dbg" | grep -q 'post-guard dumpable=1' \
    || fail "dumpability guard: prctl(PR_GET_DUMPABLE)!=1 did not restore to 1 (the guard is missing or broken)"
echo "dumpability guard: forced 0 -> restored 1"

# region trace: attach to attach_victim (has a hot leaf function 'hotfn')
"$BUILD/attach_victim" 2>/dev/null &
AVPID=$!
SVPID=""
SRVPID=""
WVPID=""
TVPID=""
DVPID=""
CVPID=""
JVPID=""
AJPID=""
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
RVPID=""
RWPID=""
LJPID=""
SGPID=""
GSPID=""
EVPID=""
trap 'kill "$AVPID" ${WVPID:+"$WVPID"} ${SVPID:+"$SVPID"} ${TVPID:+"$TVPID"} ${DVPID:+"$DVPID"} ${CVPID:+"$CVPID"} ${JVPID:+"$JVPID"} ${UPID:+"$UPID"} ${IPID:+"$IPID"} ${YPID:+"$YPID"} ${MVPID:+"$MVPID"} ${HWPID:+"$HWPID"} ${DLPID:+"$DLPID"} ${EXPID:+"$EXPID"} ${FKPID:+"$FKPID"} ${CLPID:+"$CLPID"} ${IVPID:+"$IVPID"} ${SKPID:+"$SKPID"} ${LJPID:+"$LJPID"} ${SGPID:+"$SGPID"} ${GSPID:+"$GSPID"} ${EVPID:+"$EVPID"} ${AJPID:+"$AJPID"} ${RVPID:+"$RVPID"} ${RWPID:+"$RWPID"} ${SRVPID:+"$SRVPID"} ${NAPID:+"$NAPID"} ${BAPID:+"$BAPID"} ${U8PID:+"$U8PID"} 2>/dev/null || true; rm -f ${JVPID:+"/tmp/perf-$JVPID.map"} ${AJPID:+"/tmp/perf-$AJPID.map"} ${UPID:+"$BUILD/jit-$UPID.dump"} "$BUILD/int3_swallow.log" "$BUILD/tid_victim.log" "$BUILD/watch_victim.log" "$BUILD/gstop.log" "$BUILD/watch_rec.log" "$BUILD/info_never_ptrace.strace" 2>/dev/null || true; rm -f /tmp/asmspy_fork_parent.txt /tmp/asmspy_fork_child.txt /tmp/asmspy_sock_victim.sock "$BUILD/sock_victim.log" 2>/dev/null || true; rm -rf "$BUILD/debuglink_t" ${U8BASE:+"$U8BASE"} 2>/dev/null || true' EXIT INT TERM
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

# SIGINT mid region-trace must run the two-phase detach — unplant the 0xcc
# entry breakpoint and leave the target alive — not kill the tracer with the
# int3 still planted (2026-07-21 review C1: a POKETEXT byte is plain memory the
# kernel does NOT restore on tracer death; the orphaned int3 later kills the
# target). A negative n runs until interrupted, so the plant/step cycle is
# guaranteed live when the signal lands. attach_victim calls hotfn continuously,
# so a leaked breakpoint would SIGTRAP it within the grace period below.
echo "--- asmspy --trace $AVPID hotfn -1 + SIGTERM (planted-0xcc unwind) ---"
tsig_log="$BUILD/trace_sigint.log"
# SIGTERM, not SIGINT: sh starts `&` jobs with SIGINT SIG_IGNed (and asmspy
# honors an inherited SIG_IGN — the nohup convention), so a headless smoke
# cannot deliver the interactive Ctrl-C. SIGTERM keeps its default disposition
# in a background job and takes the exact same handler -> stop-flag -> detach
# path, so the invariant under test is the same.
"$ASM" --trace "$AVPID" hotfn -1 >"$tsig_log" 2>&1 &
TRPID=$!
i=0
until grep -q 'ret=57' "$tsig_log" 2>/dev/null; do # >=1 sample -> mid-cycle
    i=$((i+1)); [ "$i" -le 100 ] || fail "sigterm: no sample within 10s"
    kill -0 "$TRPID" 2>/dev/null || fail "sigterm: tracer exited before signal"
    sleep 0.1
done
# T10 baseline: the ordinary unprivileged path must be untouched by the guard
# above. /proc has no "Dumpable:" field in status -- the OBSERVABLE symptom
# M16 measured is /proc/<pid> itself flipping to root:root ownership, so stat
# the live tracer's /proc/<pid> (external, no self-report instrumentation)
# while it is mid-capture and require it match OUR OWN uid:gid, not a bare
# "not root" (this smoke may itself run as root in a container, where a
# dumpable process's own /proc/<pid> is legitimately 0:0 too). This
# necessarily matches here (an unprivileged process starts dumpable already),
# so on its own it could not catch the guard being deleted -- the forced-lever
# check above (T10) is what exercises the guard's actual branch.
my_ug="$(id -u):$(id -g)"
proc_ug=$(stat -c '%u:%g' "/proc/$TRPID" 2>/dev/null) || proc_ug=""
[ "$proc_ug" = "$my_ug" ] || fail "sigterm: live tracer /proc/$TRPID owned $proc_ug (want $my_ug) -- non-dumpable flips this to root:root and EACCES's codeimage's /proc/self/* probes"
kill -TERM "$TRPID"
i=0
while :; do # poll /proc state: kill -0 stays true on a zombie, this doesn't
    st=$(awk '{print $3}' "/proc/$TRPID/stat" 2>/dev/null) || st=""
    { [ -z "$st" ] || [ "$st" = "Z" ]; } && break
    i=$((i+1)); [ "$i" -le 100 ] || fail "sigterm: tracer alive 10s after SIGTERM"
    sleep 0.1
done
set +e; wait "$TRPID"; trc=$?; set -e
[ "$trc" -eq 0 ] || fail "sigterm: tracer exited $trc (expected a clean detach)"
sleep 1 # grace: the victim keeps entering hotfn — an orphaned int3 kills it
kill -0 "$AVPID" 2>/dev/null || fail "sigterm: victim died (leaked breakpoint)"
rm -f "$tsig_log"
echo "SIGTERM mid-trace: clean detach, victim survived"

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
    # Remember the producer's absence: every later case that asserts a CAPTURE
    # (steps/ret=57/def-use) gates on DF_AVAIL — the legitimate skip is the correct
    # outcome wherever the value producer is x86-64-only (e.g. AArch64).
    DF_AVAIL=0
    echo "(data-flow producer unavailable here — subcommand self-skipped, OK)"
else
    DF_AVAIL=1
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

    # 26 T5.1: the per-step register RING. --steps arms one `regstate` event per
    # df_step in the RECORDING (the desktop Scrubber's feed), field-compatible with
    # the emulator's; its omission leaves the recording free of regstate (the ring
    # is zero-cost when off). Assert both, plus the descriptor and all 18 fields.
    echo "--- asmspy --dataflow $AVPID hotfn --steps --record (regstate ring, 26) ---"
    dfrec="$BUILD/df_steps_$$.asmtrace"
    dfrec0="$BUILD/df_nosteps_$$.asmtrace"
    rm -f "$dfrec" "$dfrec0"
    set +e
    timeout 40 "$ASM" --dataflow "$AVPID" hotfn --steps --record="$dfrec" \
        >/dev/null 2>&1; rc=$?
    set -e
    [ "$rc" -eq 124 ] && fail "--dataflow --steps hung"
    [ "$rc" -eq 0 ] || fail "--dataflow --steps exited $rc"
    [ -s "$dfrec" ] || fail "--dataflow --steps: no recording written"
    nstep=$(grep -c '"k":"df_step"' "$dfrec" || true)
    nreg=$(grep -c '"k":"regstate"' "$dfrec" || true)
    [ "$nreg" -gt 0 ] \
        || fail "--dataflow --steps: no regstate events (register ring not armed)"
    [ "$nreg" = "$nstep" ] \
        || fail "--dataflow --steps: $nreg regstate != $nstep df_step (not 1:1)"
    grep -q '"desc":"user_regs@x86_64/sysv"' "$dfrec" \
        || fail "--dataflow --steps: regstate missing user_regs@x86_64/sysv descriptor"
    reg1=$(grep -m1 '"k":"regstate"' "$dfrec")
    for f in rax rbx rcx rdx rsi rdi rbp rsp r8 r9 r10 r11 r12 r13 r14 r15 \
             rip rflags; do
        printf '%s' "$reg1" | grep -q "\"$f\":" \
            || fail "--dataflow --steps: regstate missing field $f"
    done
    echo "  regstate ring: $nreg events, 1:1 with df_step, 18 fields, user_regs desc"
    # 37 T1: every df_step states its region base `rbase` — the scoped region base
    # the session captured, identical across that one region and nonzero (0 is
    # omitted, never emitted). The existing "step":N,"off":N prefix is unchanged
    # (rbase follows off), so a reader that keyed on it still parses.
    grep -q '"rbase":' "$dfrec" \
        || fail "--dataflow --steps: df_step carries no rbase (37 region tag)"
    grep -q '"step":0,"off":' "$dfrec" \
        || fail "--dataflow --steps: df_step step/off prefix changed (rbase misplaced)"
    nrb=$(grep -oE '"rbase":[0-9]+' "$dfrec" | sort -u | wc -l | tr -d ' ')
    [ "$nrb" = "1" ] \
        || fail "--dataflow --steps: df_step rbase not identical across the scoped region ($nrb distinct)"
    rbval=$(grep -oE '"rbase":[0-9]+' "$dfrec" | head -1 | grep -oE '[0-9]+')
    [ -n "$rbval" ] && [ "$rbval" != "0" ] \
        || fail "--dataflow --steps: df_step rbase is 0/empty (should be omitted, not 0)"
    echo "  df_step rbase = $rbval (region base stated on the wire, identical across the region, 37)"
    # negative control: WITHOUT --steps the recording carries NO regstate.
    set +e
    timeout 40 "$ASM" --dataflow "$AVPID" hotfn --record="$dfrec0" \
        >/dev/null 2>&1; rc=$?
    set -e
    [ "$rc" -eq 0 ] || fail "--dataflow (no --steps) exited $rc"
    if [ -s "$dfrec0" ]; then
        grep -q '"k":"regstate"' "$dfrec0" \
            && fail "--dataflow WITHOUT --steps emitted regstate (ring not off)"
        echo "  no --steps: recording carries no regstate (ring disarmed), OK"
    fi
    rm -f "$dfrec" "$dfrec0"

    # 29 R2 T3: the `mem` address stream. --mem emits one `mem` event per memory
    # access into the RECORDING (the desktop 3D rich rung's feed), independent of
    # --steps. hotfn is register-only (an arithmetic loop), so it exercises the
    # PLUMBING: the flag is accepted, the recording is well-formed, no `mem` events
    # leak WITHOUT --mem, and --mem does not arm the regstate ring (the two gates
    # are independent). The cross-producer VALUE correctness — that the live `mem`
    # stream matches the emulator on a memory-touching routine — is test_mem_parity.
    dfmem="$BUILD/df_mem_$$.asmtrace"
    dfmem0="$BUILD/df_nomem_$$.asmtrace"
    rm -f "$dfmem" "$dfmem0"
    set +e
    timeout 40 "$ASM" --dataflow "$AVPID" hotfn --mem --record="$dfmem" \
        >/dev/null 2>&1; rc=$?
    set -e
    [ "$rc" -eq 124 ] && fail "--dataflow --mem hung"
    [ "$rc" -eq 0 ] || fail "--dataflow --mem exited $rc"
    [ -s "$dfmem" ] || fail "--dataflow --mem: no recording written"
    grep -q '"k":"df_step"' "$dfmem" \
        || fail "--dataflow --mem: recording carries no df_step"
    grep -q '"k":"end"' "$dfmem" \
        || fail "--dataflow --mem: recording has no footer (torn)"
    # --mem is not --steps: the register ring stays disarmed.
    grep -q '"k":"regstate"' "$dfmem" \
        && fail "--dataflow --mem armed the regstate ring (gates not independent)"
    # Any `mem` event that IS present is well-formed (step/ea/size/rw). hotfn is
    # register-only so there may be none — assert shape only when present.
    memln=$(grep -m1 '"k":"mem"' "$dfmem" || true)
    if [ -n "$memln" ]; then
        printf '%s' "$memln" | grep -qE '"step":[0-9]+,"ea":[0-9]+,"size":[0-9]+,"rw":"[rw]"' \
            || fail "--dataflow --mem: malformed mem event: $memln"
        echo "  --mem: recording carries well-formed mem events"
    else
        echo "  --mem: accepted, recording well-formed (hotfn register-only: no mem access)"
    fi
    # negative control: WITHOUT --mem the recording carries NO mem events.
    set +e
    timeout 40 "$ASM" --dataflow "$AVPID" hotfn --record="$dfmem0" \
        >/dev/null 2>&1; rc=$?
    set -e
    [ "$rc" -eq 0 ] || fail "--dataflow (no --mem) exited $rc"
    if [ -s "$dfmem0" ]; then
        grep -q '"k":"mem"' "$dfmem0" \
            && fail "--dataflow WITHOUT --mem emitted mem events (stream not off)"
        echo "  no --mem: recording carries no mem events (stream disarmed), OK"
    fi
    rm -f "$dfmem" "$dfmem0"

    # 41 L3: the `blame` backward cone (T1) + `statediff` step-delta (T2) on the
    # LIVE serve leg — R6 landed both recorder-only. Both are pure projections the
    # serve leg already has the material for (the def-use graph for blame's cone, the
    # regstate ring for statediff's delta) and spell the wire with the SHARED body
    # builders, so a live blame/statediff and a golden one are byte-identical.
    dfbs="$BUILD/df_blamestate_$$.asmtrace"
    dfbs0="$BUILD/df_noblamestate_$$.asmtrace"
    rm -f "$dfbs" "$dfbs0"
    echo "--- asmspy --dataflow $AVPID hotfn --blame --statediff --record (41 L3) ---"
    set +e
    timeout 40 "$ASM" --dataflow "$AVPID" hotfn --blame --statediff \
        --record="$dfbs" >/dev/null 2>&1; rc=$?
    set -e
    [ "$rc" -eq 124 ] && fail "--dataflow --blame --statediff hung"
    [ "$rc" -eq 0 ] || fail "--dataflow --blame --statediff exited $rc"
    [ -s "$dfbs" ] || fail "--dataflow --blame --statediff: no recording written"
    # T1: at least one blame event, each with an ascending cone INCLUDING the sink
    # and a truthful born_untraced verdict (never an empty cone).
    nblame=$(grep -c '"k":"blame"' "$dfbs" || true)
    [ "$nblame" -ge 1 ] || fail "--dataflow --blame: no blame event emitted"
    blaml=$(grep -m1 '"k":"blame"' "$dfbs")
    printf '%s' "$blaml" | grep -qE '"cone":\[.*"kind":"insn".*\]' \
        || fail "--dataflow --blame: blame carries no cone: $blaml"
    printf '%s' "$blaml" | grep -qE '"born_untraced":(true|false)' \
        || fail "--dataflow --blame: blame missing born_untraced verdict: $blaml"
    echo "  blame: $nblame event(s), cone + born_untraced present"
    # T2: --statediff self-arms the register ring, so statediff pairs 1:1 with
    # regstate; step 0 is computed:false (no predecessor -> empty changed, never a
    # full-delta lie); a later step is computed:true with a non-empty changed set.
    nsd=$(grep -c '"k":"statediff"' "$dfbs" || true)
    nrg=$(grep -c '"k":"regstate"' "$dfbs" || true)
    [ "$nsd" -gt 0 ] \
        || fail "--dataflow --statediff: no statediff (register ring not self-armed)"
    [ "$nsd" = "$nrg" ] \
        || fail "--dataflow --statediff: $nsd statediff != $nrg regstate (not 1:1)"
    grep -q '"k":"statediff","step":0,"changed":{},"computed":false' "$dfbs" \
        || fail "--dataflow --statediff: step 0 not computed:false with empty changed"
    if [ "$nsd" -ge 2 ]; then
        grep -qE '"k":"statediff","step":[0-9]+,"changed":\{"[^}]+\},"computed":true' \
            "$dfbs" \
            || fail "--dataflow --statediff: no computed:true step with a non-empty changed set"
    fi
    echo "  statediff: $nsd event(s), 1:1 with regstate, step-0 computed:false"
    # negative control: WITHOUT the flags the recording carries NEITHER kind.
    set +e
    timeout 40 "$ASM" --dataflow "$AVPID" hotfn --steps --record="$dfbs0" \
        >/dev/null 2>&1; rc=$?
    set -e
    [ "$rc" -eq 0 ] || fail "--dataflow (no blame/statediff) exited $rc"
    if [ -s "$dfbs0" ]; then
        grep -q '"k":"blame"' "$dfbs0" \
            && fail "--dataflow WITHOUT --blame emitted a blame event"
        grep -q '"k":"statediff"' "$dfbs0" \
            && fail "--dataflow WITHOUT --statediff emitted a statediff event"
        echo "  no flags: recording carries neither blame nor statediff, OK"
    fi
    rm -f "$dfbs" "$dfbs0"

    # 35 T1: --continuous re-arms the SAME region and keeps capturing until a stop
    # signal, appending each pass into ONE growing recording delimited by a
    # `df_invocation` marker (each pass's df_step restarts at step 0). attach_victim
    # calls hotfn continuously, so >=2 passes land in a couple of seconds; a SIGINT
    # ends the session cleanly (stop observed between passes). The one-shot default
    # emits NO marker and is byte-identical to before.
    echo "--- asmspy --dataflow $AVPID hotfn --continuous (35 T1: re-arm loop) ---"
    dfcont="$BUILD/df_cont_$$.asmtrace"
    dfone="$BUILD/df_one_$$.asmtrace"
    rm -f "$dfcont" "$dfone"
    set +e
    timeout 40 "$ASM" --dataflow "$AVPID" hotfn --continuous \
        --record="$dfcont" >/dev/null 2>&1 &
    contpid=$!
    sleep 3
    kill -INT "$contpid" 2>/dev/null
    wait "$contpid"; rc=$?
    set -e
    [ "$rc" -eq 124 ] && fail "--dataflow --continuous hung (stop not honored)"
    [ -s "$dfcont" ] || fail "--dataflow --continuous: no recording written"
    ninv=$(grep -c '"k":"df_invocation"' "$dfcont" || true)
    [ "$ninv" -ge 2 ] \
        || fail "--dataflow --continuous: expected >=2 df_invocation passes, got $ninv"
    # ONE growing recording: exactly one header + one footer across all passes.
    [ "$(grep -c '"asmtrace":1' "$dfcont")" = 1 ] \
        || fail "--dataflow --continuous: not one recording (multiple headers)"
    grep -q '"k":"end"' "$dfcont" \
        || fail "--dataflow --continuous: no footer (torn) — stop did not close cleanly"
    # the marker's field order + shape (pass,result,steps,truncated), pass 0 first.
    grep -m1 '"k":"df_invocation"' "$dfcont" \
        | grep -qE '"pass":0,"result":-?[0-9]+,"steps":[0-9]+,"truncated":(true|false)' \
        || fail "--dataflow --continuous: malformed df_invocation marker"
    # segmentation invariant: the FIRST df_step after a marker restarts at step 0,
    # so two passes' step ranges do not conflate (the desktop keys on this).
    awk '/"k":"df_invocation"/{seen++} seen>=1 && /"k":"df_step"/{print; exit}' \
        "$dfcont" | grep -q '"step":0,' \
        || fail "--dataflow --continuous: a pass's df_step did not restart at step 0"
    echo "  --continuous: $ninv df_invocation-delimited passes in one recording, clean stop"
    # negative control: WITHOUT --continuous, one invocation and NO marker (default).
    set +e
    timeout 40 "$ASM" --dataflow "$AVPID" hotfn --record="$dfone" \
        >/dev/null 2>&1; rc=$?
    set -e
    [ "$rc" -eq 0 ] || fail "--dataflow (one-shot) exited $rc"
    if [ -s "$dfone" ]; then
        grep -q '"k":"df_invocation"' "$dfone" \
            && fail "--dataflow one-shot emitted df_invocation (default is not one-shot)"
        echo "  one-shot: no df_invocation (byte-identical default preserved), OK"
    fi
    rm -f "$dfcont" "$dfone"

    # --serve mode "tree" arms a code image over the main executable's TEXT.
    #
    # WHY: the desktop's 3D pane is present only when a recording carries
    # codeimage regions (view_presence.cpp gates Scene3D on
    # regions_from_codeimage). A tree session has no region of its own, so
    # without this it emitted no codeimage and the module-excursion ribbon — the
    # one 3D scene a call tree is FOR — could never be shown from a live capture.
    # Measured before the fix: mode=tree emitted call events and nothing else.
    echo "--- asmspy --serve mode=tree (codeimage over the exe text) ---"
    svtree="$BUILD/serve_tree_$$.ndjson"
    svdf="$BUILD/serve_df_$$.ndjson"
    rm -f "$svtree" "$svdf"
    set +e
    { printf '{"cmd":"start","mode":"tree","pid":%s,"max":40}\n' "$AVPID"; sleep 6; } \
        | timeout 40 "$ASM" --serve > "$svtree" 2>/dev/null
    set -e
    grep -q '"k":"call"' "$svtree" \
        || fail "--serve tree: no call events (the session did not run)"
    grep -q '"k":"codeimage"' "$svtree" \
        || fail "--serve tree: no codeimage — the 3D pane cannot host the module ribbon"
    # The span must be a REAL address with a REAL length, not a zero placeholder.
    grep -m1 '"k":"codeimage"' "$svtree" \
        | grep -qE '"base":[1-9][0-9]*,"len":[1-9][0-9]*' \
        || fail "--serve tree: codeimage carries a zero base/len"
    echo "  tree: codeimage armed over the exe text, call events present"

    # NEGATIVE CONTROL, and the reason the tracked span is a separate field: a
    # REGION session must still track its OWN region, not the executable's text.
    # Sharing p.base/p.len for both would silently retarget every dataflow
    # capture's code image at the whole program.
    hb=$("$ASM" --syms "$AVPID" hotfn 2>/dev/null | awk '/hotfn/{print $1; exit}')
    hl=$("$ASM" --syms "$AVPID" hotfn 2>/dev/null | awk '/hotfn/{print $2; exit}')
    if [ -n "$hb" ] && [ -n "$hl" ]; then
        hbd=$(printf '%d' "$hb")
        set +e
        { printf '{"cmd":"start","mode":"dataflow","pid":%s,"base":%s,"len":%s,"max":60}\n' \
            "$AVPID" "$hbd" "$hl"; sleep 6; } \
            | timeout 40 "$ASM" --serve > "$svdf" 2>/dev/null
        set -e
        if grep -q '"k":"codeimage"' "$svdf"; then
            grep -m1 '"k":"codeimage"' "$svdf" | grep -q "\"base\":$hbd," \
                || fail "--serve dataflow: codeimage base is not the region's own base \
(the exe-text span leaked into a region session)"
            echo "  dataflow: codeimage still tracks the region's own base, OK"
        fi
    fi
    rm -f "$svtree" "$svdf"

    # --serve dataflow "insns": the ordered `trace` instruction stream.
    #
    # WHY: a two-recording view aligns its pair on a trace-or-coverage stream
    # (dt_diff_build refuses without one) while the divergence worldline's ribs
    # come from `statediff`. Only the dataflow engine emits statediff, and only
    # the region engine emitted trace, so no single capture could fill that
    # scene — it refused with "both recordings need a trace or coverage stream
    # to be aligned". The trace here is not synthesised: vt->insn_off[] is the
    # same array df_step already states.
    echo "--- asmspy --serve dataflow insns (ordered trace stream) ---"
    svon="$BUILD/serve_insns_on_$$.ndjson"
    svoff="$BUILD/serve_insns_off_$$.ndjson"
    rm -f "$svon" "$svoff"
    if [ -n "$hb" ] && [ -n "$hl" ]; then
        hbd=$(printf '%d' "$hb")
        set +e
        { printf '{"cmd":"start","mode":"dataflow","pid":%s,"base":%s,"len":%s,"max":40,"steps":true,"statediff":true,"insns":true}\n' \
            "$AVPID" "$hbd" "$hl"; sleep 6; } \
            | timeout 40 "$ASM" --serve > "$svon" 2>/dev/null
        { printf '{"cmd":"start","mode":"dataflow","pid":%s,"base":%s,"len":%s,"max":40,"steps":true,"statediff":true}\n' \
            "$AVPID" "$hbd" "$hl"; sleep 6; } \
            | timeout 40 "$ASM" --serve > "$svoff" 2>/dev/null
        set -e
        non=$(grep -c '"k":"trace"' "$svon" || true)
        noff=$(grep -c '"k":"trace"' "$svoff" || true)
        nstep=$(grep -c '"k":"df_step"' "$svon" || true)
        [ "$non" -gt 0 ] \
            || fail "--serve dataflow insns:true emitted no trace events"
        # One trace per step: the stream IS the step sequence, not a summary.
        [ "$non" = "$nstep" ] \
            || fail "--serve dataflow insns: $non trace vs $nstep df_step — not 1:1"
        # NEGATIVE CONTROL: the default must stay byte-identical to before, or
        # every existing dataflow recording silently changes shape.
        [ "$noff" = "0" ] \
            || fail "--serve dataflow WITHOUT insns emitted trace events (default not off)"
        # The pair must now be alignable: trace AND statediff in one recording.
        grep -q '"k":"statediff"' "$svon" \
            || fail "--serve dataflow insns: lost the statediff stream"
        echo "  insns: $non trace events 1:1 with df_step; default off; statediff intact"
    fi
    rm -f "$svon" "$svoff"

    # 39 T4: a continuous session must SURVIVE a QUIET region — the pinned region
    # not entered for one entry wait — and keep capturing the later burst. 35's
    # re-arm loop cleared its stop flag only on a PRODUCTIVE pass, so a single
    # quiet window ENDED the session (the checkbox promised "until Stop" but the
    # first lull stopped it). quiet_hot_victim alternates a dense hot burst with a
    # ~1.5 s quiet stretch; with a 300 ms entry wait the quiet stretch yields
    # several armed-but-quiet windows (0-step df_invocation markers) and the hot
    # bursts yield productive passes. The recording must therefore show a
    # PRODUCTIVE pass AFTER a quiet one — proof it did not stop at the lull — in
    # one clean, footer-closed recording. (The pick stays PINNED, never re-picks.)
    echo "--- asmspy --dataflow --continuous through a QUIET region (39 T4) ---"
    "$BUILD/quiet_hot_victim" 2>/dev/null &
    QHPID=$!
    sleep 1 # let the warmup hot phase begin so the capture catches the first entry
    kill -0 "$QHPID" 2>/dev/null || fail "quiet_hot_victim did not start"
    dfq="$BUILD/df_quiet_$$.asmtrace"
    rm -f "$dfq"
    set +e
    timeout 40 env ASMTEST_DF_ENTRY_WAIT_MS=300 "$ASM" --dataflow "$QHPID" hotfn \
        --continuous --record="$dfq" >/dev/null 2>&1 &
    qpid=$!
    sleep 6 # warmup-productive, then >=1 quiet stretch, then another hot burst
    kill -INT "$qpid" 2>/dev/null
    wait "$qpid"; qrc=$?
    set -e
    [ "$qrc" -eq 124 ] && fail "--continuous through quiet hung (stop not honored)"
    if [ -s "$dfq" ] && grep -q '"k":"df_invocation"' "$dfq"; then
        # ONE growing recording, cleanly closed (survived to the stop, not torn at
        # a lull).
        [ "$(grep -c '"asmtrace":1' "$dfq")" = 1 ] \
            || fail "--continuous quiet: not one recording (multiple headers)"
        grep -q '"k":"end"' "$dfq" \
            || fail "--continuous quiet: no footer (torn) — ended at a lull?"
        # A 0-step df_invocation is the armed-but-quiet window marker (39 T4).
        grep -qE '"k":"df_invocation".*"steps":0,' "$dfq" \
            || fail "--continuous quiet: no 0-step df_invocation — the quiet window was not surfaced (or the session ended at it): $dfq"
        # THE survival property: a PRODUCTIVE pass (steps>0) lands AFTER a quiet
        # one (steps:0). Without 39 T4 the session ends at the first quiet window,
        # so no productive pass could follow it.
        awk '
          /"k":"df_invocation"/ &&  /"steps":0,/ { sawquiet=1 }
          /"k":"df_invocation"/ && !/"steps":0,/ { if (sawquiet) { print "survived"; exit } }
        ' "$dfq" | grep -q survived \
            || fail "--continuous quiet: no productive pass after a quiet one — did not survive the lull ($(grep -c '"k":"df_invocation"' "$dfq") passes)"
        echo "  --continuous SURVIVED the quiet region: $(grep -c '"k":"df_invocation"' "$dfq") passes, productive after quiet, one clean recording"
    else
        # A slow/loaded box could miss the whole warmup and take the by-design
        # first-pass NEVER_RAN exit; that is not a T4 regression (the survival path
        # needs at least one productive pass first). Do not fail spuriously.
        echo "  (no continuous recording — the capture never caught the warmup burst on this box; T4 survival path not exercised here)"
    fi
    kill "$QHPID" 2>/dev/null || true
    rm -f "$dfq"

    # 35 T2: interruptible Stop. The stop is threaded into the entry wait
    # (dfp_run_to_multi) and the single-step loop (dfp_step_loop), so a SIGTERM
    # mid-session is honored WITHIN one in-flight pass (bounded well under the 10 s
    # entry wait) AND the victim SURVIVES the crash-safe detach — the same clean-
    # detach invariant the SIGTERM-mid-trace case proves for the region engine, now
    # for the re-arm loop.
    echo "--- asmspy --dataflow $AVPID hotfn --continuous + SIGTERM (35 T2) ---"
    dfcsig="$BUILD/df_cont_sig_$$.asmtrace"
    rm -f "$dfcsig"
    "$ASM" --dataflow "$AVPID" hotfn --continuous --steps --record="$dfcsig" \
        >/dev/null 2>&1 &
    DCPID=$!
    i=0
    until grep -q '"k":"df_invocation"' "$dfcsig" 2>/dev/null; do # >=1 pass ran
        i=$((i+1)); [ "$i" -le 100 ] || fail "continuous+sigterm: no pass within 10s"
        kill -0 "$DCPID" 2>/dev/null \
            || fail "continuous+sigterm: tracer exited before signal"
        sleep 0.1
    done
    kill -TERM "$DCPID"
    i=0
    while :; do # poll /proc state: kill -0 stays true on a zombie, this doesn't
        st=$(awk '{print $3}' "/proc/$DCPID/stat" 2>/dev/null) || st=""
        { [ -z "$st" ] || [ "$st" = "Z" ]; } && break
        i=$((i+1)); [ "$i" -le 50 ] \
            || fail "continuous+sigterm: tracer alive 5s after SIGTERM (stop not honored within a pass)"
        sleep 0.1
    done
    set +e; wait "$DCPID"; dcrc=$?; set -e
    [ "$dcrc" -eq 0 ] \
        || fail "continuous+sigterm: tracer exited $dcrc (expected a clean detach)"
    sleep 1 # grace: the victim keeps entering hotfn — an orphaned int3 kills it
    kill -0 "$AVPID" 2>/dev/null \
        || fail "continuous+sigterm: victim died (leaked breakpoint)"
    rm -f "$dfcsig"
    echo "continuous+SIGTERM: honored within a pass, clean detach, victim survived"
fi
# ---------------------------------------------------------------------------
# BOUNDED ENTRY WAIT (asmspy-plan Theme H)
# ---------------------------------------------------------------------------
# --dataflow arms an int3 at the region ENTRY and waits for a thread to arrive.
# That wait was UNBOUNDED: naming a function that is not currently running did not
# error, it HUNG. Measured before the fix, on one victim/function/thread:
#   --trace    rc=1   4s   "alpha_work never executed on thread N"   <- truthful
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
if printf '%s\n' "$nvout" | grep -q '^# SKIP --dataflow'; then
    # The producer gate fires BEFORE the entry wait is armed (e.g. AArch64: the
    # data-flow value producer is x86-64-only), so the bounded-wait shape cannot
    # engage — the legitimate self-skip is the correct outcome, as in the hotfn case.
    echo "(data-flow producer unavailable here — entry-wait case self-skipped, OK)"
else
    [ "$nvrc" -eq 1 ] || fail "--dataflow main: expected rc=1 (not-seen-entering), got $nvrc"
    printf '%s\n' "$nvout" | grep -q 'not seen entering' \
        || fail "--dataflow main: no genuine not-seen-entering report: $nvout"
    # The name must be the SYMBOL, not freed memory: dc.func borrows into the symtab,
    # which is released before this message is formatted. A use-after-free here printed
    # plausible-looking garbage rather than crashing.
    printf '%s\n' "$nvout" | grep -q '^main not seen entering' \
        || fail "--dataflow main: region name wrong/garbled (symtab use-after-free?): $nvout"
    [ $((t1 - t0)) -lt 15 ] \
        || fail "--dataflow main took $((t1-t0))s — the 800ms bound did not drive it"
    echo "  bounded: reported in $((t1-t0))s instead of hanging"
fi

# THE CONTROL, and it is the whole test's guard: the SAME victim's hotfn must still
# capture. Without this, "rc=1 and a message" would also be produced by a --dataflow
# that had simply stopped working. Where the producer self-skips (DF_AVAIL=0) the
# main case above skipped too, so there is nothing for this control to guard.
if [ "${DF_AVAIL:-1}" = 1 ]; then
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
fi

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
if [ "${DF_AVAIL:-1}" = 1 ]; then
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
else
    echo "  (--max truncation cases need the value producer — skipped with it)"
fi

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
# The picker AND the candidate walk are unit-tested on every host
# (build/test_autoregion, incl. the pure asmspy_autoregion_walk cases 39 T1 adds).
#
# 2026-08-06 plan, Task 7. This block used to self-skip whenever the sampler was
# unavailable -- which is ALWAYS in the plain docker-cli lane, because Docker's
# default seccomp profile blocks perf_event_open. Per CLAUDE.md, a test that can
# only ever self-skip is not a test. The ptrace sampler needs no perf, so this
# now RUNS here, and a skip is a failure. `--sampler=ptrace` is forced (rather
# than bare `--auto`) because the bare form's own T5 chain-walk prints IBS's and
# sw's OWN "# SKIP --dataflow --auto:" lines on its way to ptrace on this very
# host -- text this same grep would misread as the WHOLE call self-skipping even
# though the chain ultimately picked for real. Forcing the sampler sidesteps
# that; the chain-walk itself (IBS refused -> sw refused -> ptrace picks) is
# verified live, not by this grep (task-7-report.md, Step 4).
echo "--- asmspy --dataflow --auto --sampler=ptrace (perf-free entry pick, no symbol given) ---"
"$BUILD/auto_victim" 2>"$BUILD/auto_victim.log" &
UVPID=$!
sleep 1
kill -0 "$UVPID" 2>/dev/null || fail "auto_victim did not start"
set +e
auout=$(timeout 60 "$ASM" --dataflow "$UVPID" --auto --sampler=ptrace 2>&1); aurc=$?
set -e
[ "$aurc" -eq 124 ] && fail "--dataflow --auto --sampler=ptrace hung"
printf '%s\n' "$auout" | grep -q '^# SKIP' \
    && fail "the ptrace sampler needs no perf and must not self-skip: $auout"
[ "$aurc" -eq 0 ] || fail "--dataflow --auto --sampler=ptrace exited $aurc: $auout"
# THE assertion: the callee, not the residency winner (auto_victim's shape is the test).
printf '%s\n' "$auout" | grep -q 'entered_often' \
    || fail "--auto --sampler=ptrace must pick the ARRIVED-AT function (auto_victim's shape is the test): $auout"
# THE CONTROL: grind_forever has no entry edge at all (entered before we
# attached, never again), so a picker that names it is ranking residency, not
# confirmed entry arrival -- exactly the hazard this sampler's phase 3 exists
# to reject.
printf '%s\n' "$auout" | grep -q 'grind_forever' \
    && fail "--auto --sampler=ptrace picked grind_forever -- that is the RESIDENCY winner, and an entry breakpoint there can never fire"
# Never called => never observed arriving => never picked.
printf '%s\n' "$auout" | grep -q 'quiet_helper' \
    && fail "--auto --sampler=ptrace named quiet_helper, which is never called"
# It must actually TRACE the pick, not just name it — the point is the data flow.
printf '%s\n' "$auout" | grep -q 'data flow — entered_often' \
    || fail "--auto --sampler=ptrace picked but did not capture: $auout"
printf '%s\n' "$auout" | grep -qE '#0 .*endbr64|#0 .*\+0x' \
    || fail "--auto --sampler=ptrace: no value trace from the auto-picked region"
echo "--dataflow --auto --sampler=ptrace: picked the hot entry with no perf (grind_forever/quiet_helper correctly rejected)"

# --module= scopes the ptrace pick too, same contract as ibs/sw: a module that
# matches nothing must REFUSE transparently rather than fall back to a wrong
# region.
set +e
amout=$(timeout 60 "$ASM" --dataflow "$UVPID" --auto --sampler=ptrace --module=no_such_module 2>&1); amrc=$?
set -e
[ "$amrc" -eq 124 ] && fail "--auto --sampler=ptrace --module hung"
printf '%s\n' "$amout" | grep -q 'no function was observed being ENTERED' \
    || fail "--auto --sampler=ptrace --module=no_such_module should refuse transparently, got: $amout"
printf '%s\n' "$amout" | grep -q 'entered_often' \
    && fail "--auto --sampler=ptrace --module=no_such_module still picked entered_often (the filter does not filter)"
echo "  --auto --sampler=ptrace --module= filters the pick (and refuses transparently when empty)"
# The victim must survive being sampled + traced.
sleep 1
kill -0 "$UVPID" 2>/dev/null || fail "auto_victim died under --dataflow --auto"
kill "$UVPID" 2>/dev/null || true
rm -f "$BUILD/auto_victim.log"

# THE UNCONFIRMED-PICK GATE (auto_pick_ptrace's "THE GATE", cli/asmspy.c) must
# refuse the WHOLE batch whenever asmspy_ptrace_sample could not prove full
# thread-set coverage -- whether coverage was never there to begin with
# (ASMSPY_PS_TEST_CAP, a seize capped below the target's real thread count) or
# was LOST MID-LOOP, after some candidates already carried a REAL phase-3
# measurement (ASMSPY_PS_TEST_LOSE_AFTER, cli/asmspy_ptracesample.c).
#
# The mid-loop case is the one review found the gate's first cut blind to: a
# check that only inspects cands[0]'s own arrivals/first_us sees genuinely
# non-zero data there (candidate 0 WAS confirmed, before coverage dropped) and
# never trips, so the whole unranked batch -- including a tail that never ran
# through phase 3 at all -- gets reported "CONFIRMED by int3 arrival" and
# walked with evidence:"entry". That is exactly the "arm an int3 at a PLT
# stub and hang" hazard this module exists to prevent, just relocated past
# the first candidate. The fix gates on the producer's own authoritative
# verdict (asmspy_ps_arm_note's UNCONFIRMED sentence, appended to `why` iff
# `confirmed` is false for the WHOLE call) instead of any one candidate's
# fields. Both levers are test-only and completely inert unless set.
echo "--- asmspy --dataflow --auto --sampler=ptrace: an unconfirmed batch must never be armed ---"
GLOG="$BUILD/tid_victim_gate.log"
: > "$GLOG"
"$BUILD/tid_victim" 2>"$GLOG" &
GVPID=$!
# Same barrier as the --tid filter block above: wait for both workers to
# report in, so the picker has more than one real candidate to work with.
GATID=""; GBTID=""
_i=0
while [ "$_i" -lt 100 ]; do
    GATID=$(sed -n 's/^alpha tid=\([0-9][0-9]*\).*/\1/p' "$GLOG" | head -1)
    GBTID=$(sed -n 's/^beta tid=\([0-9][0-9]*\).*/\1/p' "$GLOG" | head -1)
    [ -n "$GATID" ] && [ -n "$GBTID" ] && break
    _i=$((_i + 1))
    sleep 0.1
done
kill -0 "$GVPID" 2>/dev/null || fail "tid_victim (gate check) did not start"
[ -n "$GATID" ] && [ -n "$GBTID" ] \
    || fail "tid_victim (gate check) did not report both worker tids"

# Case 1: coverage never there at all (the shape ASMSPY_PS_TEST_CAP always
# provoked, already covered manually before this task's review -- landed
# here so it is asserted, not just described).
set +e
g1out=$(ASMSPY_PS_TEST_CAP=2 timeout 15 "$ASM" --dataflow "$GVPID" --auto \
    --sampler=ptrace --max=50 2>&1)
g1rc=$?
set -e
[ "$g1rc" -eq 124 ] && fail "--auto --sampler=ptrace (ASMSPY_PS_TEST_CAP=2) hung"
[ "$g1rc" -eq 0 ] \
    || fail "--auto --sampler=ptrace (ASMSPY_PS_TEST_CAP=2) exited $g1rc: $g1out"
printf '%s\n' "$g1out" | grep -q '^# SKIP' \
    || fail "an immediately-unconfirmed batch (ASMSPY_PS_TEST_CAP=2) must self-skip, not arm: $g1out"
printf '%s\n' "$g1out" | grep -q 'UNCONFIRMED' \
    || fail "the self-skip must carry the producer's own UNCONFIRMED reason, not a guessed one: $g1out"
printf '%s\n' "$g1out" | grep -q 'CONFIRMED by int3 arrival' \
    && fail "ASMSPY_PS_TEST_CAP=2 must never be reported CONFIRMED: $g1out"
kill -0 "$GVPID" 2>/dev/null \
    || fail "tid_victim died under an ASMSPY_PS_TEST_CAP=2 gate probe"

# Case 2: coverage LOST MID-LOOP, after candidate 0 already got a real
# phase-3 measurement -- THE REVIEW CRITICAL this block exists to close.
# ASMSPY_PS_TEST_CAP cannot provoke this shape (it never lets phase 3 start
# at all, so nothing has a first_us yet); this lever drops coverage from
# INSIDE the confirm loop, right after candidate 0 is measured for real, so a
# cands[0]-only check would see genuine data and wrongly pass.
set +e
g2out=$(ASMSPY_PS_TEST_LOSE_AFTER=0 timeout 15 "$ASM" --dataflow "$GVPID" \
    --auto --sampler=ptrace --max=50 2>&1)
g2rc=$?
set -e
[ "$g2rc" -eq 124 ] && fail "--auto --sampler=ptrace (ASMSPY_PS_TEST_LOSE_AFTER=0) hung"
[ "$g2rc" -eq 0 ] \
    || fail "--auto --sampler=ptrace (ASMSPY_PS_TEST_LOSE_AFTER=0) exited $g2rc: $g2out"
printf '%s\n' "$g2out" | grep -q '^# SKIP' \
    || fail "a batch that lost coverage MID-LOOP must self-skip WHOLE, even though candidate 0 was genuinely confirmed first: $g2out"
printf '%s\n' "$g2out" | grep -q 'UNCONFIRMED' \
    || fail "the mid-loop self-skip must carry the producer's own UNCONFIRMED reason: $g2out"
printf '%s\n' "$g2out" | grep -q 'CONFIRMED by int3 arrival' \
    && fail "a mid-loop coverage loss must never be reported CONFIRMED, even with a genuinely-confirmed cands[0] (the exact bug review found): $g2out"
printf '%s\n' "$g2out" | grep -q 'data-flow capture of' \
    && fail "an unconfirmed batch must never reach the capture engine: $g2out"
kill -0 "$GVPID" 2>/dev/null \
    || fail "tid_victim died under an ASMSPY_PS_TEST_LOSE_AFTER=0 gate probe"

echo "  --auto --sampler=ptrace refuses an unconfirmed batch WHOLE: both when coverage was never there (ASMSPY_PS_TEST_CAP) and when it was lost mid-loop after a genuinely-confirmed prefix (ASMSPY_PS_TEST_LOSE_AFTER)"
kill "$GVPID" 2>/dev/null || true
rm -f "$GLOG"

# --sampler=sw: the PORTABLE --auto path (software-clock residency rule +
# candidate walk; asmspy-plan §H). auto_victim's residency winner IS
# grind_forever by construction, so when this runs for real the walk is what
# lands entered_often: grind is refused at the (shortened) entry wait, the
# next-ranked candidate is tried. Runs wherever perf_event_open is allowed —
# docker-cli-ibs, or a bare host at perf_event_paranoid<=2 — on ANY vendor;
# under Docker's default seccomp it self-skips, and the skip REASON must be
# real (the --sample lesson: an empty reason lands on exactly the host where
# the reason matters).
echo "--- asmspy --dataflow --auto --sampler=sw (portable residency + candidate walk) ---"
"$BUILD/auto_victim" 2>"$BUILD/auto_victim.log" &
SWPID=$!
sleep 1
kill -0 "$SWPID" 2>/dev/null || fail "auto_victim did not start (sw)"
set +e
swout=$(timeout 60 env ASMTEST_DF_ENTRY_WAIT_MS=1500 "$ASM" --dataflow "$SWPID" --auto --sampler=sw 2>&1); swrc=$?
set -e
[ "$swrc" -eq 124 ] && fail "--auto --sampler=sw hung (the entry wait or the candidate walk is unbounded)"
if printf '%s\n' "$swout" | grep -q '^# SKIP --dataflow:'; then
    # The sw SAMPLER may work here while the value PRODUCER is x86-64-only
    # (AArch64): the pick half runs, then the capture self-skips transparently.
    echo "  (sw sampler ran but the value producer is unavailable — capture half self-skipped, OK)"
elif printf '%s\n' "$swout" | grep -q '^# SKIP --dataflow --auto'; then
    printf '%s\n' "$swout" | grep -qE '^# SKIP --dataflow --auto: .*perf' \
        || fail "--sampler=sw skipped with an empty/vague reason (the --sample lesson): $swout"
    echo "  (perf_event_open blocked here — sw sampler self-skipped WITH a real reason. Use make docker-cli-ibs)"
else
    [ "$swrc" -eq 0 ] || fail "--auto --sampler=sw exited $swrc: $swout"
    # The rule must be NAMED: an operator reading a transcript can tell which
    # evidence (entry edges vs residency) picked the region.
    printf '%s\n' "$swout" | grep -q -- '--auto\[sw-clock\]:' \
        || fail "--sampler=sw did not name its rule: $swout"
    printf '%s\n' "$swout" | grep -q 'data flow — entered_often' \
        || fail "--sampler=sw never captured entered_often: $swout"
    printf '%s\n' "$swout" | grep -q 'quiet_helper' \
        && fail "--sampler=sw named quiet_helper, which is never called"
    printf '%s\n' "$swout" | grep -qE '#0 .*endbr64|#0 .*\+0x' \
        || fail "--sampler=sw: no value trace from the walked-to region"
    # If grind_forever topped residency (the expected shape), its attempt must
    # end in the genuine refusal — captured-grind would mean the entry wait let
    # a never-re-entered region through, which cannot happen.
    if printf '%s\n' "$swout" | grep -q 'data-flow capture of grind_forever'; then
        printf '%s\n' "$swout" | grep -q 'not seen ENTERING' \
            || fail "--sampler=sw captured grind_forever — a residency winner whose entry can never fire: $swout"
    fi
    if printf '%s\n' "$swout" | grep -q 'trying candidate'; then
        echo "  sw walk exercised: residency winner refused at the entry wait, next candidate captured"
    else
        echo "  sw picked entered_often directly (grind did not top residency this window)"
    fi
fi
sleep 1
kill -0 "$SWPID" 2>/dev/null || fail "auto_victim died under --sampler=sw"
kill "$SWPID" 2>/dev/null || true
rm -f "$BUILD/auto_victim.log"

# --auto must be able to pick a JIT'd method. jit_victim publishes jit_hot_loop
# (10 bytes, size > 0) in /tmp/perf-<pid>.map and re-enters it ~1M times/s from
# main's call site — the only entry arrival in the process, and one the ELF
# symtab CANNOT resolve (the code lives in an anonymous mapping). The pre-fix
# resolver consulted only the ELF symtab, so --auto REFUSED on exactly this
# victim; the pick assertion below fails on that build. main can never win
# instead (entered once, before we attached), so a pick here proves the JIT
# layer, not a fallback.
"$BUILD/jit_victim" 2>/dev/null &
AJPID=$!
sleep 1
kill -0 "$AJPID" 2>/dev/null || fail "jit_victim did not start (--auto JIT leg)"
set +e
ajout=$(timeout 60 "$ASM" --dataflow "$AJPID" --auto 2>&1); ajrc=$?
set -e
[ "$ajrc" -eq 124 ] && fail "--dataflow --auto hung on jit_victim"
if printf '%s\n' "$ajout" | grep -q '^# SKIP --dataflow --auto'; then
    printf '%s\n' "$ajout" | grep '^# SKIP' | sed 's/^/  /'
    echo "  (IBS-Op unavailable — JIT --auto leg self-skipped; use make docker-cli-ibs)"
else
    [ "$ajrc" -eq 0 ] || fail "--dataflow --auto on jit_victim exited $ajrc: $ajout"
    printf '%s\n' "$ajout" | grep -qE '\-\-auto: jit_hot_loop \[jit\]' \
        || fail "--auto did not pick the JIT method jit_hot_loop: $ajout"
    # The capture must be NAMED too: a JIT winner is invisible to
    # asmspy_symtab_at, so this line is the auto_pick name plumbing, not the
    # symtab fallback.
    printf '%s\n' "$ajout" | grep -q 'data flow — jit_hot_loop' \
        || fail "--auto picked jit_hot_loop but did not capture/name it: $ajout"
    echo "  --auto picked + traced the JIT method (perf-map layered into the pick)"
fi
sleep 1
kill -0 "$AJPID" 2>/dev/null || fail "jit_victim died under --dataflow --auto"
kill "$AJPID" 2>/dev/null || true
rm -f "/tmp/perf-$AJPID.map"

# --auto + --tid is a USAGE error, not a precedence rule: the sampler carries no
# tid, so it could only ever pin the capture to a thread that may never arrive.
expect_badarg "$ASM" --dataflow 1 --auto --tid=1
# --module= without --auto would be a silent no-op that reads like a filter.
expect_badarg "$ASM" --dataflow 1 hotfn --module=libc

# 39 T3: --window sizes the --auto sample window. It is accepted WITH --auto
# (verified above the way the flag reaches the sampler is proven by cli-ibs), and
# rejected as a silent no-op without it — the same posture as --sampler/--module.
# These are pure arg-parse checks (no sampler), so they run on every lane.
expect_badarg "$ASM" --dataflow 1 hotfn --window=100    # --window without --auto
expect_badarg "$ASM" --dataflow 1 hotfn --window=400    # even ==default: no silent no-op
expect_badarg "$ASM" --dataflow 1 --auto --window=abc   # non-numeric window
expect_badarg "$ASM" --dataflow 1 --auto --window=0     # zero window
expect_badarg "$ASM" --dataflow 1 --auto --window=99999 # over the 60000 ms cap

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
# T8: the count must AGGREGATE — work(5) makes 5 helper calls that merge into ONE
# 5x line, not five 1x lines (which the presence grep above would accept). The x
# below is the UTF-8 multiplication sign region_render emits in "%4u×" — a literal
# byte match, no special handling needed.
printf '%s\n' "$out" | grep -qE '^ *5×.*-> *helper' \
    || fail "region edges: work(5)'s five helper calls did not aggregate to a single 5× line"
# merge: helper must appear at most once per sample (the run captures 2 samples,
# one 'functions called' section each). An append-per-edge mutant emits 5x this.
[ "$(printf '%s\n' "$out" | grep -c -- '-> *helper')" -le 2 ] \
    || fail "region edges: helper appears more than once per sample — edges not aggregated by callee"
echo "  region edges: work(5)'s 5 helper calls aggregate to one 5× line per sample"

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
    # the index is FOR but which no assertion here could pin down accurately.
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
# preexec_fn assertion below is what keeps that accurate.
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

# C2: --log --follow must DROP a followed child that execs a 32-bit image. The
# syscall-stream engine decodes against the compile-time x86-64 table and has no
# i386 table, so tracing the child past its exec would render i386 write(4) as
# x86-64 stat(4) — the confident nonsense the attach-time i386 guard refuses. The
# fix (PTRACE_O_TRACEEXEC + an EI_CLASS re-check at the exec-stop) detaches the
# child, so no `stat(` ever appears and the 64-bit parent keeps being logged.
# Gated on ASMSPY_HAVE_M32 (the -m32 fixture), like the refusal leg below. Neither
# fork_victim nor i386_victim makes a REAL x86-64 stat, so any `stat(` is proof of
# the mis-decode.
if [ "${ASMSPY_HAVE_M32:-}" = "yes" ] && [ -x "$BUILD/i386_victim" ]; then
    echo "--- asmspy --log --follow drops a child that execs a 32-bit image (C2) ---"
    fk_spawn "$BUILD/i386_victim"
    set +e
    c2out=$(timeout 60 "$ASM" --log "$FKPID" 500 --follow 2>/dev/null); rc=$?
    set -e
    [ "$rc" -eq 124 ] && fail "--log --follow (C2) hung"
    fk_kill
    # anti-vacuity: the child WAS followed before its exec (its own fd-3 write),
    # so "no stat" cannot pass merely because nothing was traced
    printf '%s\n' "$c2out" | grep -q asmspy_fork_child \
        || fail "C2: the followed child was never traced pre-exec — the drop check would be vacuous (is --follow working?)"
    printf '%s\n' "$c2out" | grep -q asmspy_fork_parent \
        || fail "C2: the 64-bit parent's writes are missing — the run is broken"
    if printf '%s\n' "$c2out" | grep -qE '\bstat\('; then
        printf '%s\n' "$c2out" | grep -E '\bstat\(' | head -3
        fail "C2: an i386 syscall was decoded against the x86-64 table (write=4 shown as stat) — the followed child was NOT dropped at its 32-bit exec"
    fi
    echo "  --follow + 32-bit exec: child dropped at the exec-stop; parent still logged"
    rm -f /tmp/asmspy_fork_parent.txt /tmp/asmspy_fork_child.txt 2>/dev/null || true
fi

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
# CANDID SCOPE. The bug this guards is that an untabled task escapes the
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
# JOB-CONTROL GROUP-STOP branch (asmspy-plan Theme D): PTRACE_EVENT_STOP +
# PTRACE_LISTEN
# ---------------------------------------------------------------------------
# A SEIZE'd tracee that receives SIGSTOP enters a group-stop the tracer must
# HONOR via PTRACE_LISTEN (not resume it with PTRACE_SYSCALL). Every engine has
# this branch and none had a smoke. threads_victim is multi-threaded, so a
# group-stop stops EVERY thread and the branch fires once per tid. GSPID is a
# FRESH variable, NOT $TVPID (still the empty placeholder here — the existing
# threads_victim instance is not started until much later).
#
# -1 (run until the target exits), NOT a fixed line budget: threads_victim's
# workers emit >400 lines/s, so a small budget would be exhausted during the
# settle — before kill -STOP ever lands — leaving the tracer already exited and
# steps 3-6 vacuous. -1 keeps the tracer alive across the whole STOP/CONT cycle;
# the victim is killed explicitly, and the tracer must then exit 0 (never 124).
# Both sinks fflush per line (log_print_sink / stream_print_sink), so a live
# `wc -l` on the redirected file reflects the feed in real time.
gstop_cycle() { # $1 = engine flag (--log or --stream)
    "$BUILD/threads_victim" 2>/dev/null &
    GSPID=$!
    sleep 1
    kill -0 "$GSPID" 2>/dev/null \
        || fail "group-stop ($1): threads_victim did not start"
    timeout 60 "$ASM" "$1" "$GSPID" -1 >"$BUILD/gstop.log" 2>/dev/null &
    gsasm=$!
    sleep 1
    kill -STOP "$GSPID" || fail "group-stop ($1): kill -STOP failed"
    sleep 1
    # (3) genuinely stopped under LISTEN (t/T), not run through (S/R): the
    # mutation this guards (resume with PTRACE_SYSCALL) leaves it running.
    grep -qE '^State:.[tT]' "/proc/$GSPID/status" \
        || fail "group-stop ($1): victim State is not t/T — LISTEN branch not honoring the stop"
    # (4) the tracer survived the group-stop event
    kill -0 "$gsasm" 2>/dev/null \
        || fail "group-stop ($1): the tracer died on the group-stop event"
    # (5) the feed is PAUSED while the whole group is stopped
    n0=$(wc -l <"$BUILD/gstop.log")
    sleep 1
    n1=$(wc -l <"$BUILD/gstop.log")
    [ "$n1" -eq "$n0" ] \
        || fail "group-stop ($1): the feed grew ($n0->$n1) while the group was stopped"
    # (6) SIGCONT wakes the LISTEN'd threads: the feed grows again
    kill -CONT "$GSPID" || fail "group-stop ($1): kill -CONT failed"
    sleep 1
    n2=$(wc -l <"$BUILD/gstop.log")
    [ "$n2" -gt "$n1" ] \
        || fail "group-stop ($1): the feed did not grow ($n1->$n2) after SIGCONT — LISTEN did not wake"
    # with all tracees gone the tracer must exit 0 (the until-exit contract), not 124
    kill "$GSPID" 2>/dev/null || true
    set +e
    wait "$gsasm"; grc=$?
    set -e
    [ "$grc" -eq 0 ] \
        || fail "group-stop ($1): tracer exited $grc (expected 0 when the target exits, not rc 124)"
    GSPID=""
    echo "  group-stop ($1): stopped t/T, feed paused ($n0==$n1), resumed on CONT (->$n2), tracer exited 0"
}
echo "--- asmspy group-stop: SIGSTOP/SIGCONT honored via PTRACE_LISTEN (--log) ---"
gstop_cycle --log
echo "--- asmspy group-stop: the single-step engine's LISTEN branch (--stream) ---"
gstop_cycle --stream

# ---------------------------------------------------------------------------
# NEGATIVE-n "run until exit" (asmspy-plan Theme D): --log/--stream <pid> -1
# ---------------------------------------------------------------------------
# The documented until-exit contract: a negative n runs until every tracee is
# gone, then returns rc 0. Every OTHER victim loops forever, so this row was
# never testable — exit_victim runs ~2s of decodable syscalls then returns 0 (the
# 2s vs the 1s settle guarantees the attach lands while it is alive). The
# load-bearing assertion is rc != 124: a `max < 0` regression that spins on
# ECHILD forever is caught ONLY by the timeout. The victims exit under trace, so
# they are NOT waited on afterwards (EVPID is cleared once each leg returns).
echo "--- asmspy --log EVPID -1 (negative-n: rc 0 when the target exits) ---"
"$BUILD/exit_victim" 2>/dev/null &
EVPID=$!
sleep 1
kill -0 "$EVPID" 2>/dev/null || fail "exit_victim did not start (--log leg)"
set +e
evout=$(timeout 60 "$ASM" --log "$EVPID" -1 2>&1); rc=$?
set -e
[ "$rc" -ne 124 ] \
    || fail "negative-n --log did not return when the target exited (rc 124 — spun forever)"
[ "$rc" -eq 0 ] \
    || fail "negative-n --log exited rc=$rc (exit mishandled as an attach failure)"
printf '%s\n' "$evout" | grep -qE '(nanosleep|clock_nanosleep)\(' \
    || fail "negative-n --log: no traced syscalls (attached-and-returned without tracing?)"
EVPID="" # exited under trace — do NOT wait on it
echo "  --log -1 returned rc 0 with real syscalls after the target exited"

echo "--- asmspy --stream EVPID -1 (negative-n: single-step until exit) ---"
"$BUILD/exit_victim" 2>/dev/null &
EVPID=$!
sleep 1
kill -0 "$EVPID" 2>/dev/null || fail "exit_victim did not start (--stream leg)"
set +e
esout=$(timeout 60 "$ASM" --stream "$EVPID" -1 2>&1); rc=$?
set -e
[ "$rc" -ne 124 ] \
    || fail "negative-n --stream did not return when the target exited (rc 124)"
[ "$rc" -eq 0 ] || fail "negative-n --stream exited rc=$rc"
printf '%s\n' "$esout" | grep -qE 'mov|jmp|cmp|add|push|call|lea|test|sub|nop' \
    || fail "negative-n --stream: no disassembly (attached-and-returned without stepping?)"
EVPID="" # exited under trace — do NOT wait on it
echo "  --stream -1 returned rc 0 with real disassembly after the target exited"

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
# an UNKNOWN shape must say it is unknown rather than claim three. The victim
# makes one DELIBERATELY-undescribed syscall (sysinfo) for this — a determinate
# source, unlike the incidental restart_syscall the kernel only emits when a signal
# interrupts a blocking call inside the window (that flaked to zero matches under
# some kernels). sysinfo's name still resolves; its arg shape does not, so it must
# render "<3 raw words>, ...".
ad_has 'sysinfo(' "an undescribed syscall (name resolves, arg shape does not)"
printf '%s\n' "$adout" | grep -qE 'sysinfo\(.*\.\.\.\) = ' \
    || fail "arg decode: the undescribed sysinfo() did not render '...' — an unknown shape is still claiming a fixed arity"
# CONTROL: the ellipsis must NOT appear on a shape we DO know, or it would just
# be decoration rather than a statement about arity.
printf '%s\n' "$adout" | grep -E '^(openat|mmap|getpid|writev|tgkill)\(' \
    | grep -q '\.\.\.' \
    && fail "arg decode: a KNOWN shape rendered '...' — the ellipsis is supposed to mark an unknown arity, not decorate every line"
echo "  flags/mode/prot/iovec/sigset/signo/timespec/whence decoded; arity 0, 6 and conditional all correct; unknown shapes say so"

# ---- T2: ioctl request names + fcntl command names (conditional arity) ----
ad_has 'fcntl(fd=' "fcntl with a resolved fd"
ad_has 'F_GETFL) = ' "an argument-less fcntl cmd with NO third slot"
ad_has 'F_SETFD, ' "a cmd that takes an argument keeps its slot"
ad_has 'TIOCGWINSZ' "a named ioctl request"
ad_has "_IOC(_IOC_READ, 0xab, 0x1, 4)" "an unknown ioctl request decomposed, not named"
echo "  ioctl/fcntl commands named; fcntl arity conditional; unknown ioctl decomposed"

# ---- T3: futex operation names (private flag folded into the name) ----
ad_has 'FUTEX_WAKE_PRIVATE, 1' "futex op name with the private flag folded in"
# NEGATIVE CONTROL: the op must NOT still render as the bare int 129
# (FUTEX_WAKE|FUTEX_PRIVATE_FLAG) — the pre-change rendering.
printf '%s\n' "$adout" | grep -E '^futex\(' | grep -q ', 129, ' \
    && fail "futex: op still renders as the bare int 129 (masking not applied)"
echo "  futex op named with the private flag folded in (no bare 129)"

# ---- T4: stat/statx result buffers on success (raw pointer on failure) ----
# 18 is DERIVED from the fixture's own writev (9+9 bytes); 0644 from its open —
# the assertion cannot pass against a stale decode.
ad_has '{st_mode=S_IFREG|0644, st_size=18}' "a stat buffer's contents"
ad_has 'statx(AT_FDCWD, ' "statx dirfd + path"
ad_has 'stx_size=18' "a statx buffer honoring its mask"
# NEGATIVE control: a FAILED stat must render a raw pointer + negative return,
# not a struct from a buffer the kernel never filled (proves the ret==0 gate).
# The victim's path-stat is arch-split (AArch64 never had legacy SYS_stat; it
# calls newfstatat there) — assert the exact per-arch rendering either way.
case "$(uname -m)" in
aarch64 | arm64)
    printf '%s\n' "$adout" | grep -qE 'newfstatat\(AT_FDCWD, "/nonexistent-asmspy", 0x[0-9a-f]+, 0x0\) = -' \
        || fail "stat: a failed newfstatat did not render a raw pointer + negative return"
    ;;
*)
    printf '%s\n' "$adout" | grep -qE 'stat\("/nonexistent-asmspy", 0x[0-9a-f]+\) = -' \
        || fail "stat: a failed stat did not render a raw pointer + negative return"
    ;;
esac
echo "  stat/statx buffers decoded on success; a failed stat stays a raw pointer"

# ---- T9: decoder-breadth pins for shapes that landed WITHOUT a rendered-text
#      assertion (a table-entry typo could land silently) ----
ad_has 'readv(fd=' "readv with a resolved fd"
ad_has '["iovec-one", "iovec-two"], 2) = 18' "readv CONTENTS at exit"
# AArch64 has no dup2 syscall — glibc's dup2() routes to dup3(oldfd, newfd, 0)
# there; x86-64 keeps the 2-arg dup2. Assert the exact per-arch rendering.
case "$(uname -m)" in
aarch64 | arm64)
    ad_has 'dup3(fd=' "dup3 fd class (arm64 dup2 routes to dup3)"
    ad_has ', 17, 0) = 17' "dup3's newfd + flags + return"
    ;;
*)
    ad_has 'dup2(fd=' "dup2 fd class"
    ad_has ', 17) = 17' "dup2's plain int second arg and return"
    ;;
esac
ad_has 'ftruncate(fd=' "ftruncate"
ad_has ', 4) = 0' "its A_SIZE arg"
ad_has 'getppid() = ' "a second arity-ZERO shape"
ad_has 'clock_nanosleep(0, 0, {0.002000000}' "the 4-arg clock_nanosleep shape"
echo "  breadth: readv contents, dup2, ftruncate, getppid, clock_nanosleep pinned"

# ---- .asmtrace: --json is a RECORDING, and its syscall lines are payload-free
#      (docs/internal/gui/asmtrace-schema.md, the `syscall` kind).
# The same victim, the same calls — but now asserted through the recording, so
# the redaction split is measured against KNOWN payload strings rather than
# inspected by eye. This is the fidelity gate of the format: `line` must keep
# every structural field and carry NONE of the content; `payload` carries the
# content, separately, where a reader can default-redact it.
echo "--- asmspy --log --json (.asmtrace recording + payload-free lines) ---"
"$BUILD/argdecode_victim" 2>/dev/null &
SGPID=$!
sleep 2
set +e
timeout 90 "$ASM" --log "$SGPID" 400 --json >"$BUILD/argdecode.asmtrace" 2>/dev/null; rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--log --json on argdecode_victim hung"
kill -9 "$SGPID" 2>/dev/null || true
wait "$SGPID" 2>/dev/null || true
SGPID=""
rm -f /tmp/asmspy_argdecode.txt

head -1 "$BUILD/argdecode.asmtrace" | grep -q '"asmtrace":1' \
    || fail "--json: line 1 is not an asmtrace header"
head -1 "$BUILD/argdecode.asmtrace" | grep -q '"backend":"ptrace-syscalls"' \
    || fail "--json: header carries no ptrace-syscalls provenance"
tail -1 "$BUILD/argdecode.asmtrace" | grep -q '"k":"end"' \
    || fail "--json: the recording has no end event (torn)"
grep -q '"k":"syscall"' "$BUILD/argdecode.asmtrace" \
    || fail "--json: no syscall events recorded"

# The structure survives: name, flag words, octal mode, counts, return value.
grep -q 'openat(AT_FDCWD, \\"<path>\\", O_WRONLY|O_CREAT|O_TRUNC, 0644) = 3' \
    "$BUILD/argdecode.asmtrace" \
    || fail "--json: a payload-free openat lost its flags/mode/return"

# THE gate: every known payload string the victim writes must appear ONLY in a
# "payload" field, never in a "line". A grep over the whole file would pass on
# the payload alone, so the line fields are extracted first.
# The line field ends at the UNESCAPED closing quote, which is either the start
# of ,"payload": or the end of the object — a plain s/".*// would cut at the
# first \" (a quoted path placeholder) and silently weaken every check below.
sed -n 's/.*"line":"//p' "$BUILD/argdecode.asmtrace" \
    | sed 's/","payload":".*//; s/"}$//' >"$BUILD/argdecode.lines"
for secret in 'iovec-one' 'iovec-two' '/tmp/asmspy_argdecode.txt' \
              '/nonexistent-asmspy'; do
    grep -qF "$secret" "$BUILD/argdecode.lines" \
        && fail "--json: payload string '$secret' leaked into a payload-free line"
done
# ... and the placeholders that replaced them are really there.
grep -qF '<path>' "$BUILD/argdecode.lines" \
    || fail "--json: no <path> placeholder in any payload-free line"
grep -qE '<[0-9]+ (bytes|buffers)>' "$BUILD/argdecode.lines" \
    || fail "--json: no <N bytes>/<N buffers> placeholder in any payload-free line"
# ... while the payload CHANNEL still carries the content (redaction is a
# reader's default, not a loss at record time).
grep -q '"payload":"/tmp/asmspy_argdecode.txt"' "$BUILD/argdecode.asmtrace" \
    || fail "--json: the decoded path is missing from the payload channel"
echo "  --json recording: header+end intact; structure kept, content split into payload"

echo "--- asmspy --log fd->endpoint (socket:[inode] -> real endpoint) ---"
"$BUILD/sock_victim" 2>"$BUILD/sock_victim.log" &
SKPID=$!
sleep 1
kill -0 "$SKPID" 2>/dev/null || { cat "$BUILD/sock_victim.log"; fail "sock_victim died at startup"; }
SKPORT=$(sed -n 's/.*tcp_port=\([0-9]*\).*/\1/p' "$BUILD/sock_victim.log")
[ -n "$SKPORT" ] || { cat "$BUILD/sock_victim.log"; fail "sock_victim did not report its TCP port"; }
set +e
# 300, not 80: the loop now makes ~4x the socket calls per iteration (sendto +
# a throwaway UDP connect + a TCP client/accept on top of the four writes).
skout=$(timeout 60 "$ASM" --log "$SKPID" 300 2>/dev/null); rc=$?
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

# ---------------------------------------------------------------------------
# SOCKADDR CONTENTS (asmspy-plan Theme E — T1): the argument, not just the fd
# ---------------------------------------------------------------------------
# connect()'s IN sockaddr: the throwaway UDP socket and the TCP client both
# connect to the listener's port, so {AF_INET, 127.0.0.1:$SKPORT} is DERIVED
# from the run (the block already extracted $SKPORT from the victim's stderr).
# `<[^,]*>` (not `<[^>]*>`): the connected socket's fd resolves to an endpoint
# that itself contains a `->` (e.g. <TCP a->b>), so match up to the comma.
printf '%s\n' "$skout" | grep -qE 'connect\(fd=[0-9]+<[^,]*>, \{AF_INET, 127\.0\.0\.1:'"$SKPORT"'\}' \
    || fail "sockaddr: connect() does not render its IN {AF_INET, 127.0.0.1:$SKPORT}"
# sendto()'s IN sockaddr: the bound AF_UNIX path (was a raw hex word before T1)
printf '%s\n' "$skout" | grep -q '{AF_UNIX, "/tmp/asmspy_sock_victim.sock"}' \
    || fail "sockaddr: sendto() does not render {AF_UNIX, \"/tmp/asmspy_sock_victim.sock\"}"
# accept()'s OUT sockaddr, filled in on success (the peer's ephemeral port)
printf '%s\n' "$skout" | grep -qE 'accept\(fd=[0-9]+<[^>]*>, \{AF_INET, 127\.0\.0\.1:[0-9]+\}' \
    || fail "sockaddr: accept() does not render its OUT {AF_INET, 127.0.0.1:<port>} on success"
# socket()'s domain renders as a NAME, not a bare number
printf '%s\n' "$skout" | grep -qE 'socket\(AF_INET, ' \
    || fail "sockaddr: socket() does not name its AF_INET domain"
# NEGATIVE CONTROL: no socket-family call may still render its sockaddr as a raw
# 0x7f... pointer (a decode failure prints the stack address; this is the whole
# item). Mirrors the existing socket:[inode] negative control's `&& fail` idiom.
printf '%s\n' "$skout" | grep -E '^(connect|bind|sendto)\(' | grep -q ', 0x7f' \
    && fail "sockaddr: a socket-family call still renders its sockaddr as a raw pointer"
echo "  sockaddr contents decoded: connect/sendto IN, accept OUT, socket domain (no raw 0x7f)"

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

    # --info NEVER attaches, so unlike the engines above it must SUCCEED on a
    # 32-bit target — and instead report the same refusal as DATA. pi_verdict
    # (cli/asmspy_proc.c) blanks eight of the nine modes for a 32-bit tracee —
    # every single-step engine (dataflow/stream/trace/log/watch/tree/graph/
    # procs) carries the identical `asmspy_elf_class(pid) == 32` guard in
    # cli/asmspy_engine.c and refuses BEFORE attaching, for the identical
    # reason (registers/syscalls decoded against the wrong ABI). Only
    # --sample is untouched: it is IBS silicon, a fact about the HOST, not
    # the target. Originally this list named only the first five (a
    # carry-forward gap from Task 3); review (finding D) caught that the
    # other three engines ALREADY refuse for real forty lines above this
    # very block, while --info's own advisory said "ok" for them — a
    # confidently-wrong answer with no test to catch it. Extended
    # pi_verdict to match what the engines actually do, then flipped this
    # assertion to match.
    set +e
    iout=$(timeout 30 "$ASM" --info "$IVPID" --json 2>&1); irc=$?
    set -e
    [ "$irc" -eq 124 ] && fail "--info hung on a 32-bit tracee — it must never attach"
    [ "$irc" -eq 0 ] \
        || fail "--info refused a 32-bit tracee outright (rc=$irc) — it must always succeed, since it never attaches"
    if command -v python3 >/dev/null 2>&1; then
        printf '%s' "$iout" | python3 -c 'import json,sys
events = [json.loads(l) for l in sys.stdin if l.strip()]
evt = next(e for e in events if e.get("k") == "procinfo")
modes = {m["mode"]: m for m in evt["trace"]["modes"]}
refused = ["dataflow", "stream", "trace", "log", "watch", "tree", "graph", "procs"]
untouched = ["sample"]
for name in refused:
    m = modes[name]
    assert m["ok"] is False, "%s should be refused for a 32-bit tracee, got ok=%r" % (name, m["ok"])
    assert "32-bit" in m["why"], "%s why does not name the 32-bit reason: %r" % (name, m["why"])
# tree/graph/procs single-step to build a call tree, graph or topology and
# never touch the syscall table, so "would name every syscall wrong" was
# simply false for them (and for --watch, which watches memory). The reason
# all eight share is the register ABI, which is what their own guard in
# cli/asmspy_engine.c states.
for name in ("tree", "graph", "procs"):
    assert "syscall" not in modes[name]["why"], \
        "%s is refused with a syscall-decoding reason, which is not true of it: %r" % (name, modes[name]["why"])
for name in untouched:
    m = modes[name]
    assert "32-bit" not in m["why"], \
        "%s was refused with the 32-bit reason, though pi_verdict leaves it to the IBS-host gate: %r" % (name, m["why"])
print("  --info --json: dataflow/stream/trace/log/watch/tree/graph/procs all report the 32-bit refusal; sample does not")' \
        || fail "--info --json: the 32-bit mode-advisory structural check failed"
    else
        printf '%s\n' "$iout" | grep -q '"mode":"dataflow","ok":false,"why":"32-bit' \
            || fail "--info --json: dataflow mode does not report the 32-bit refusal"
        echo "  --info --json: 32-bit mode advisory present (python3 absent; grep-only check)"
    fi
    # the human form names the reason too, not just the JSON.
    "$ASM" --info "$IVPID" 2>&1 | grep -qi '32-bit' \
        || fail "--info text: no 32-bit reason shown for any mode"
    echo "  --info: reports dataflow/stream/trace/log/watch/tree/graph/procs refused with the 32-bit reason"

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
    # JSON export: machine-readable edges + faithful provenance (pipe to jq)
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
# T7 — settle whether the watchpoint actually FIRES, and gate accurately on the
# answer. A timeout with the sentinel ALREADY seen is a real hang after a hit (a
# detach deadlock) and fails on either arch. A timeout with NO hit is arch-split:
# x86-64 hardware watchpoints fire reliably, so a silent timeout is a hang/deadlock
# and fails; on AArch64 the hosted hypervisor may ACCEPT the arm yet never route the
# debug exception (documented precedent: WSL2 x86 gdb hw watchpoints) — a HOST
# property, not an asmspy bug, so it is a NAMED skip, not a failure.
WATCH_ARMED_SILENT=""
if [ "$rc" -eq 124 ]; then
    if printf '%s\n' "$wout" | grep -qi 'd15ea5eddeadbeef'; then
        fail "--watch hung after delivering a hit (detach deadlock?)"
    else
        case "$(uname -m)" in
        aarch64 | arm64)
            echo "# SKIP --watch: watchpoint armed but never fired — host may not route debug exceptions"
            WATCH_ARMED_SILENT=1
            ;;
        *) fail "--watch hung (watchpoint never tripped / detach deadlock)" ;;
        esac
    fi
fi
printf '%s\n' "$wout" | head -10
if [ -n "$WATCH_ARMED_SILENT" ] || printf '%s\n' "$wout" | grep -q '^# SKIP --watch'; then
    echo "(hardware data watchpoints unavailable/undelivered here — --watch skipped, OK)"
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
# a MISALIGNED watch address is rejected (x86 needs a length-aligned address; AArch64
# rejects a window that would cross the 8-byte DBGWVR boundary — here 0x1+8 does), and
# a bad --len is a usage error (rc=2) — neither is silently coerced
"$ASM" --watch "$HWPID" 0x1 --len=8 >/dev/null 2>&1 \
    && fail "--watch accepted a misaligned watch address"
expect_badarg "$ASM" --watch "$HWPID" "$WADDR" --len=3
# Say what actually happened. This line used to be printed unconditionally, so a
# host where the watchpoint never armed still logged "value + PC captured,
# per-thread arming confirmed" — a success sentence for a path that ran none of
# those assertions, which is exactly how a skip turns into a false green.
if [ -n "$WATCH_ARMED_SILENT" ] || printf '%s\n' "$wout" | grep -q '^# SKIP --watch'; then
    echo "  --watch: SKIPPED (no usable hardware watchpoint here); rejects + target survival still asserted"
else
    echo "  --watch: value + PC captured, per-thread arming confirmed, target survived"
fi
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

# ---------------------------------------------------------------------------
# --launch: fork+PTRACE_TRACEME+exec a fresh target and trace it from birth
# (docs/internal/archive/gui/45-launch-and-window-target.md T1/T2). No PT/IBS/debug-
# register silicon needed — just fork/ptrace, which every CI runner has.
# ---------------------------------------------------------------------------
echo "--- asmspy --launch log -- syscall_victim (from-birth launch) ---"
LAUNCH_CLI_OUT="$BUILD/launch_cli.out" # RECDIR does not exist yet this early
set +e
timeout 30 "$ASM" --launch log -- "$BUILD/syscall_victim" >"$LAUNCH_CLI_OUT" 2>&1
rc=$?
set -e
# NEVER capture --launch through a shell pipe/command-substitution: the
# launched target SURVIVES asmspy's exit (D9's "a tracer, not a debugger"),
# still holding the SAME stdout fd it inherited at fork — a pipe reader
# (`$(...)`) then blocks forever waiting for a EOF that only comes once every
# holder of the write end closes it, which the still-running victim never
# does. A plain file redirect has no such wait (measured: this is what turned
# a 0.009s run into an indefinite hang under `out=$(...)`).
[ "$rc" -eq 124 ] && fail "--launch hung"
head -8 "$LAUNCH_CLI_OUT"
# T2's from-birth proof: the FIRST captured syscall must be the dynamic
# linker's own opening move (brk/mmap, well before main() or the victim's
# write() loop) — not several calls in, which is what a detach-then-reattach
# gap (T1's retired interim path) would show instead. The default --launch
# log budget (20) is bounded like every other --log default, so it does not
# necessarily reach the victim's OWN write() loop — that end-to-end shape is
# proven separately by the wire `launch` command below, which runs on a time
# budget instead of a count and does reach it.
first_line=$(head -1 "$LAUNCH_CLI_OUT")
printf '%s\n' "$first_line" | grep -qE '^(brk|mmap|arch_prctl|access)\(' \
    || fail "--launch log: first captured syscall was '$first_line', not an early dynamic-linker call (from-birth launch regressed)"
pkill -9 -f "$BUILD/syscall_victim" 2>/dev/null || true # the survivor from above

echo "--- asmspy --launch log -- /no/such/binary (clean error, not a hang) ---"
LAUNCH_BAD_OUT="$BUILD/launch_cli_bad.out" # RECDIR does not exist yet this early
set +e
timeout 15 "$ASM" --launch log -- /no/such/binary >"$LAUNCH_BAD_OUT" 2>&1
rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--launch of a bad path HUNG instead of failing cleanly"
[ "$rc" -eq 0 ] && fail "--launch of a bad path exited 0 (should report the exec failure)"
grep -qi 'exec failed' "$LAUNCH_BAD_OUT" \
    || fail "--launch of a bad path did not report a clean 'exec failed' reason: $(cat "$LAUNCH_BAD_OUT")"
echo "  --launch: from-birth trace works; a bad path fails clean"
rm -f "$LAUNCH_CLI_OUT" "$LAUNCH_BAD_OUT"

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

# process/thread topology: a multi-threaded victim must render as ONE process node
# with its threads listed underneath, each with an invocation count.
#
# Give --procs its OWN fresh victim rather than the thrice-attached $TVPID above:
# re-seizing a target right after --stream's single-step DETACH is a distinct
# transition that races on a loaded runner (observed on ubuntu-24.04-arm — the
# leader's SEIZE can return EPERM before the prior detach has fully settled), and
# it is not what THIS view validates. The retry respawns the victim so a persistent
# re-seize race gets a clean target each round, not just a fresh attach window.
kill "$TVPID" 2>/dev/null || true
wait "$TVPID" 2>/dev/null || true
out=""
for try in 1 2 3; do
    "$BUILD/threads_victim" 2>/dev/null &
    TVPID=$!
    sleep 1
    echo "--- asmspy --procs $TVPID 120 (process/thread topology, try $try) ---"
    set +e
    out=$(timeout 30 "$ASM" --procs "$TVPID" 120 2>&1); rc=$?
    set -e
    [ "$rc" -eq 124 ] && fail "--procs hung on a multi-threaded target"
    printf '%s\n' "$out" | grep -qE "^node $TVPID \[threads_victim\]  inv=[0-9]" && break
    kill "$TVPID" 2>/dev/null || true
    wait "$TVPID" 2>/dev/null || true
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

# ---------------------------------------------------------------------------
# "never executed" must mean THE REGION DID NOT RUN — not "we failed to look"
# ---------------------------------------------------------------------------
# rgn_race_to_entry distinguishes four outcomes (caught / user-quit / idle / gone /
# unarmable) and asmspy_engine_region used to throw three of them away: a bare
# `if (rc != 0) break;` then decided everything from `sample == 0`. So a target that
# EXITED, and an entry we could not ARM, both reported "never executed" — a claim
# about what the TARGET's code does, inferred from a failure of the TRACER. The
# unarmable case is the one asmspy.h documents as self-skipping via ETRACE for a
# W^X-enforced JIT page: that self-skip never happened.
#
# Both are asserted DETERMINISTICALLY, no W^X page required:
#   - an explicit range at an UNMAPPED address cannot be POKETEXT'd => unarmable
#   - killing the victim while the race waits on a never-re-entered symbol => gone
# The assertion is the ABSENCE of "never executed" in both — that string is the bug.
echo "--- asmspy --trace: an unarmable entry is ETRACE, not 'never executed' ---"
set +e
uaout=$(timeout 25 "$ASM" --trace "$YPID" 0xdeadbeef000:16 1 2>&1); uarc=$?
set -e
[ "$uarc" -eq 124 ] && fail "--trace on an unmappable range hung"
[ "$uarc" -eq 0 ] && fail "--trace on an unmappable range reported success"
printf '%s\n' "$uaout" | grep -q 'never executed' \
    && fail "--trace: an UNARMABLE entry reported 'never executed' — that is a claim about the target drawn from a tracer failure (asmspy.h documents ETRACE here)"
printf '%s\n' "$uaout" | grep -q 'ptrace/attach failure' \
    || fail "--trace: an unarmable entry should self-skip via ETRACE, got: $uaout"
echo "  unarmable entry -> ETRACE (the documented W^X self-skip, now real)"

echo "--- asmspy --trace: a target that EXITS is ENOENT, not 'never executed' ---"
"$BUILD/attach_victim" >/dev/null 2>&1 &
GVPID=$!
sleep 1
( sleep 1; kill -9 "$GVPID" 2>/dev/null ) &
set +e
# main() is entered once, before the attach, so the race is still waiting when the
# victim dies — the zero-sample path, which is the only one that had to guess.
gnout=$(timeout 25 "$ASM" --trace "$GVPID" main 1 2>&1); gnrc=$?
set -e
[ "$gnrc" -eq 124 ] && fail "--trace hung on a dying target"
printf '%s\n' "$gnout" | grep -q 'never executed' \
    && fail "--trace: a target that EXITED reported 'never executed' about its code"
printf '%s\n' "$gnout" | grep -q 'exited before' \
    || fail "--trace: a dead target should say so, got: $gnout"
echo "  target gone -> 'exited before ...' (not a claim about the code)"
kill "$GVPID" 2>/dev/null || true

# --tid pins the sample to one thread. Pinning to the thread that DOES run the
# region samples it; pinning to the one that never does must report "never
# executed" — promptly and without perturbing the busy non-target thread. (This
# path arms a per-thread HARDWARE breakpoint precisely so the non-target thread
# is never trapped and stepped back over a shared int3, which does not converge.)
set +e
pout=$(timeout 40 "$ASM" --trace "$YPID" alpha_work 1 --tid="$ATID" 2>&1); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--trace --tid=<runner> hung"
# HOST-CAPABILITY SPLIT (T7's rule, applied to the EXECUTION breakpoint). This path
# needs a per-thread hardware breakpoint, and some hosts report debug-register slots
# yet refuse to reserve one. MEASURED on the hosted ubuntu-24.04-arm runner
# (Neoverse-N2, 2026-07-22): NT_ARM_HW_BREAK says 6 slots / NT_ARM_HW_WATCH says 4,
# debug_arch=8 — and PTRACE_SETREGSET on either returns **ENOSPC**, so nothing can
# ever arm. asmspy already reports that faithfully (EUNARMABLE -> ETRACE "attach
# failed", NOT "never executed" — the distinction the block above tests), so the
# only wrong thing left would be this smoke DEMANDING a sample the host cannot
# produce. Name it and skip; keep x86-64 strict, where the slots really do work.
TRACE_TID_UNARMABLE=""
if printf '%s\n' "$pout" | grep -qE 'trace failed|needs a per-thread hardware breakpoint'; then
    case "$(uname -m)" in
    aarch64 | arm64)
        echo "# SKIP --trace --tid: per-thread hardware breakpoint unarmable on this host (debug slots reported but reservation refused, e.g. ENOSPC under a hypervisor)"
        TRACE_TID_UNARMABLE=1
        ;;
    esac
fi
if [ -z "$TRACE_TID_UNARMABLE" ]; then
    printf '%s\n' "$pout" | grep -qE '^sample #1' \
        || fail "--trace --tid=$ATID: alpha_work not sampled on the thread that runs it"
    set +e
    nout=$(timeout 40 "$ASM" --trace "$YPID" alpha_work 1 --tid="$BTID" 2>&1); rc=$?
    set -e
    [ "$rc" -eq 124 ] && fail "--trace --tid=<non-runner> hung (entry wait not bounded?)"
    printf '%s\n' "$nout" | grep -qE '^sample #' \
        && fail "--trace --tid=$BTID: sampled alpha_work on a thread that never runs it"
    printf '%s\n' "$nout" | grep -q 'never executed' \
        || fail "--trace --tid=$BTID: no genuine never-executed report"
    echo "  --tid pins the sample (runner sampled; non-runner reports never-executed)"
else
    # The one thing still assertable here: an unarmable entry must NOT be reported
    # as "never executed" (that would send an operator to the opposite place).
    printf '%s\n' "$pout" | grep -q 'never executed' \
        && fail "--trace --tid: unarmable entry reported as 'never executed'"
    echo "  --tid: hardware breakpoint unarmable here — reported as an attach failure, not a false 'never executed'"
fi

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
# Scope (candid): the historical fatal SIGTRAP (commit 6aaad45) reproduced
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
    #
    # WAIT FIRST — a detach-left step is not always instantly fatal. On AArch64 a
    # step armed on a thread parked in a blocking syscall only fires when that
    # syscall RETURNS: MEASURED at 200-400 ms after asmspy exited (Neoverse-N2,
    # 2026-07-22), i.e. long after an immediate `kill -0` says "alive". Checking
    # right away made this tripwire blind to exactly the class it exists to
    # catch, and the corpse was then blamed on whichever command ran next.
    for _s in 1 2 3 4 5 6 7 8 9 10; do
        kill -0 "$DVPID" 2>/dev/null || break
        sleep 0.2
    done
    kill -0 "$DVPID" 2>/dev/null \
        || fail "detach-survival: victim KILLED by $view detach (two-phase detach regressed)"
    echo "  survived $view detach (+2s)"
done
kill "$DVPID" 2>/dev/null || true

# APP-DELIVERED SIGTRAP (si_code split): int3_victim executes its OWN int3
# software breakpoints under a SIGTRAP handler. A single-step engine must tell an
# app-delivered SIGTRAP (an executed int3, si_code SI_KERNEL) from its own step
# trap (TRAP_TRACE / TRAP_BRKPT) and RE-INJECT it via PTRACE_CONT. Two ways to get
# it wrong, both caught here: SWALLOWING the trap (the app's handler never runs, so
# it prints SWALLOWED), or re-injecting via SINGLESTEP (a fatal #DB in the masked
# handler KILLS the victim). Trace under all four engine shapes — the two
# single-step engines (--stream, --procs --count=calls) AND the two syscall-stream
# engines (--log, --procs --count=syscalls), which used to SWALLOW the app trap
# (C3) — and assert the victim SURVIVES and never prints SWALLOWED.
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
for view in "--stream $IPID 100000" "--procs $IPID 100000 --count=calls" \
            "--log $IPID 100000" "--procs $IPID 100000 --count=syscalls"; do
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

# ---------------------------------------------------------------------------
# .asmtrace RECORD MODE across the headless subcommands
# (docs/internal/archive/gui/01-asmtrace-format.md T5)
# ---------------------------------------------------------------------------
# Every headless mode can write a recording. The assertion per mode is the same
# three facts, because they are the ones that make a file USABLE: a v1 header,
# at least one event of that mode's own kind, and a closing `end` — a file
# without an end event is TORN and a reader must say so, so a mode that produced
# one here would be shipping torn recordings.
echo "--- asmspy --record=<f> (.asmtrace recordings per headless mode) ---"
RECDIR="$BUILD/asmtrace-rec"
rm -rf "$RECDIR"; mkdir -p "$RECDIR"

# Assert a recording is well-formed and carries `kind`. A mode that SKIPPED
# (hardware/permission gate) is accepted only when the end event names the skip
# — which is the point of "a skip is a recording", not a loophole.
rec_ok() {
    f="$RECDIR/$1.asmtrace"; kind="$2"
    [ -s "$f" ] || fail "--record $1: no recording written"
    head -1 "$f" | grep -q '"asmtrace":1' \
        || fail "--record $1: line 1 is not an asmtrace header"
    tail -1 "$f" | grep -q '"k":"end"' \
        || fail "--record $1: no end event — the recording is TORN"
    if grep -q "\"k\":\"$kind\"" "$f"; then
        echo "  $1 -> $kind events + clean end"
    elif tail -1 "$f" | grep -q '"skip"'; then
        echo "  $1 -> SKIP recorded with its measured reason (a skip is a recording)"
    else
        fail "--record $1: no \"$kind\" events and no skip in the end event"
    fi
}

"$BUILD/spy_victim" >/dev/null 2>&1 &
RVPID=$!
sleep 1
kill -0 "$RVPID" 2>/dev/null || fail "spy_victim did not start for the record smoke"

timeout 60 "$ASM" --log "$RVPID" 10 --record="$RECDIR/log.asmtrace" >/dev/null 2>&1 || true
rec_ok log syscall
timeout 60 "$ASM" --stream "$RVPID" 20 --record="$RECDIR/stream.asmtrace" >/dev/null 2>&1 || true
rec_ok stream stream
timeout 60 "$ASM" --trace "$RVPID" work 1 --record="$RECDIR/trace.asmtrace" >/dev/null 2>&1 || true
rec_ok trace trace
grep -q '"k":"coverage"' "$RECDIR/trace.asmtrace" \
    || tail -1 "$RECDIR/trace.asmtrace" | grep -q '"skip"' \
    || fail "--record trace: an invocation recorded no coverage event"
timeout 60 "$ASM" --tree "$RVPID" 20 --record="$RECDIR/tree.asmtrace" >/dev/null 2>&1 || true
rec_ok tree call
timeout 60 "$ASM" --graph "$RVPID" 20 --record="$RECDIR/graph.asmtrace" >/dev/null 2>&1 || true
rec_ok graph graph
timeout 60 "$ASM" --procs "$RVPID" 40 --record="$RECDIR/procs.asmtrace" >/dev/null 2>&1 || true
rec_ok procs topo
# --sample is AMD IBS-Op HARDWARE: off an AMD host (and under Docker's default
# seccomp, which blocks perf_event_open) this records the SKIP, and that is the
# assertion — the gate must be visible IN the file, not absent from it.
timeout 60 "$ASM" --sample "$RVPID" 100 --record="$RECDIR/sample.asmtrace" >/dev/null 2>&1 || true
rec_ok sample survey
timeout 90 "$ASM" --dataflow "$RVPID" work --record="$RECDIR/dataflow.asmtrace" >/dev/null 2>&1 || true
rec_ok dataflow df_step

# --watch needs a real DATA address (a function entry is not length-aligned, and
# an unaligned address is refused before any capture — an argument error, which
# must NOT leave a recording behind). watch_victim publishes an aligned global,
# so this exercises the engine rather than the argument check. Where debug-
# register arming is refused, this records the skip like --sample.
"$BUILD/watch_victim" 2>"$BUILD/watch_rec.log" &
RWPID=$!
sleep 1
RWADDR=$(sed -n 's/.*watch_target=\(0x[0-9a-fA-F]*\).*/\1/p' "$BUILD/watch_rec.log" | head -1)
if [ -n "$RWADDR" ]; then
    timeout 60 "$ASM" --watch "$RWPID" "$RWADDR" 3 --record="$RECDIR/watch.asmtrace" \
        >/dev/null 2>&1 || true
    rec_ok watch watch
else
    fail "watch_victim did not report its watch_target address"
fi
kill -9 "$RWPID" 2>/dev/null || true
wait "$RWPID" 2>/dev/null || true
RWPID=""
rm -f "$BUILD/watch_rec.log"

# --info takes no engine "mode" (it never attaches), but it shares the same
# --record contract as every mode above: this is the regression guard for
# "--record with no --json still writes a file" (defect fixed pre-flight —
# gating the recording on --json would silently drop a recording asked for).
timeout 60 "$ASM" --info "$RVPID" --record="$RECDIR/info.asmtrace" >/dev/null 2>&1 || true
rec_ok info procinfo

# --record and --json COMPOSE: the same events reach the file and stdout.
timeout 60 "$ASM" --log "$RVPID" 8 --json --record="$RECDIR/both.asmtrace" \
    >"$RECDIR/both.stdout" 2>/dev/null || true
[ -s "$RECDIR/both.stdout" ] || fail "--record with --json: stdout carried no recording"
fe=$(grep -c '"k":"syscall"' "$RECDIR/both.asmtrace" || true)
se=$(grep -c '"k":"syscall"' "$RECDIR/both.stdout" || true)
[ "$fe" = "$se" ] \
    || fail "--record + --json disagree: $fe syscall events in the file, $se on stdout"
echo "  --record and --json compose ($fe syscall events in both)"

# The live writer shares the golden writer's field-order stability: two runs of
# the same mode differ only in the HEADER (ts/pid), never in how an event is
# spelled. Compared after dropping line 1 — with the same victim and the same
# bounded work, the trace bodies are the same bytes.
timeout 60 "$ASM" --trace "$RVPID" work 1 --record="$RECDIR/stab_a.asmtrace" >/dev/null 2>&1 || true
timeout 60 "$ASM" --trace "$RVPID" work 1 --record="$RECDIR/stab_b.asmtrace" >/dev/null 2>&1 || true
tail -n +2 "$RECDIR/stab_a.asmtrace" >"$RECDIR/stab_a.body"
tail -n +2 "$RECDIR/stab_b.asmtrace" >"$RECDIR/stab_b.body"
cmp -s "$RECDIR/stab_a.body" "$RECDIR/stab_b.body" \
    || { diff "$RECDIR/stab_a.body" "$RECDIR/stab_b.body" | head -10; \
         fail "two --trace recordings of the same region differ below the header"; }
echo "  two --trace recordings are byte-identical below the header (field order is stable)"

kill -9 "$RVPID" 2>/dev/null || true
wait "$RVPID" 2>/dev/null || true
RVPID=""

# ---------------------------------------------------------------------------
# --serve: the live-session control loop (07-serve-live-host.md T6)
#
# Drives a REAL session end to end over a pipe — start log -> events -> stop ->
# start stream -> quit — and asserts the four protocol properties that a client
# is entitled to rely on:
#   1. every line is a JSON object, and every non-header line carries a KNOWN
#      "k" (the serve stream is the recording format, not a second wire format);
#   2. a `session` event BRACKETS each mode, and each mode's own recording sits
#      between them, header and `end` included — protocol law 1;
#   3. the victim is ALIVE after quit (the two-phase detach ran, so this is a
#      tracer and not a debugger);
#   4. a second `start` while a session runs is REFUSED with `err` naming the
#      budget rule (D6: one ptrace jack per target).
#
# The command stream is generated by a subshell with sleeps rather than a single
# printf: EOF on stdin means "no more commands", so a burst that closes the pipe
# immediately would race the engine rather than exercise it.
echo "--- asmspy --serve (live-session control loop) ---"
"$BUILD/spy_victim" 2>/dev/null &
SRVPID=$!
sleep 1
kill -0 "$SRVPID" 2>/dev/null || fail "--serve: spy_victim did not start"
SERVE_OUT="$RECDIR/serve.ndjson"
set +e
{
    printf '{"cmd":"start","mode":"log","pid":%d}\n' "$SRVPID"
    sleep 1
    # The budget backstop: the jack is occupied, so this must be refused.
    printf '{"cmd":"start","mode":"stream","pid":%d}\n' "$SRVPID"
    printf '{"cmd":"stop"}\n'
    sleep 1
    printf '{"cmd":"start","mode":"stream","pid":%d,"max":3}\n' "$SRVPID"
    sleep 2
    printf '{"cmd":"quit"}\n'
    sleep 1
} | timeout 90 "$ASM" --serve >"$SERVE_OUT" 2>/dev/null
svrc=$?
set -e
[ "$svrc" -eq 124 ] && fail "--serve hung"
[ "$svrc" -eq 0 ] || fail "--serve exited $svrc"
[ -s "$SERVE_OUT" ] || fail "--serve produced no output"

# (3) FIRST, because it is the property everything else is worthless without.
kill -0 "$SRVPID" 2>/dev/null \
    || fail "--serve: the victim did NOT survive quit (the two-phase detach was bypassed)"
echo "  the victim survived start/stop/start/quit"

# (1) Every line is an object; every non-header line has a known "k".
nlines=$(wc -l <"$SERVE_OUT")
nobj=$(grep -c '^{.*}$' "$SERVE_OUT" || true)
[ "$nlines" = "$nobj" ] \
    || fail "--serve: $nlines lines but only $nobj are JSON objects"
nhdr=$(grep -c '^{"asmtrace":1,' "$SERVE_OUT" || true)
nk=$(grep -c '"k":"' "$SERVE_OUT" || true)
[ "$((nhdr + nk))" = "$nlines" ] \
    || fail "--serve: $nlines lines, $nhdr headers + $nk kinded — some line is neither"
# Known kinds only: strip the ones the protocol + v1 registry define and expect
# nothing left. An unknown kind here means the wire grew a shape no reader knows.
nunknown=$(grep '"k":"' "$SERVE_OUT" \
    | grep -cvE '"k":"(session|cmd|err|syscall|stream|end)"' || true)
[ "$nunknown" = "0" ] \
    || fail "--serve: $nunknown event(s) carry a kind outside the registry"
echo "  every line is JSON: $nhdr session headers + $nk kinded events, no unknown kinds"

# (2) Two sessions, each bracketed, each with its own header + end between them.
nstart=$(grep -c '"k":"session","state":"started"' "$SERVE_OUT" || true)
nstop=$(grep -c '"k":"session","state":"stopped"' "$SERVE_OUT" || true)
[ "$nstart" = "2" ] || fail "--serve: expected 2 started events, got $nstart"
[ "$nstop" = "2" ] || fail "--serve: expected 2 stopped events, got $nstop"
[ "$nhdr" = "2" ] || fail "--serve: expected 2 provenance headers, got $nhdr"
nend=$(grep -c '"k":"end"' "$SERVE_OUT" || true)
[ "$nend" = "2" ] || fail "--serve: expected 2 end footers, got $nend"
grep -q '"k":"session","state":"started","mode":"log"' "$SERVE_OUT" \
    || fail "--serve: no started event for mode log"
grep -q '"k":"session","state":"started","mode":"stream"' "$SERVE_OUT" \
    || fail "--serve: no started event for mode stream"
# Each mode's ordinary record-mode events actually arrived.
grep -q '"k":"syscall"' "$SERVE_OUT" || fail "--serve: mode log emitted no syscall events"
grep -q '"k":"stream"' "$SERVE_OUT" || fail "--serve: mode stream emitted no stream events"
# The two modes' provenance differs, which is what proves each session carries
# its OWN header rather than one stream-wide header being reused.
grep -q '"backend":"ptrace-syscalls"' "$SERVE_OUT" \
    || fail "--serve: the log session's header has the wrong backend"
grep -q '"backend":"ptrace-stream"' "$SERVE_OUT" \
    || fail "--serve: the stream session's header has the wrong backend"
echo "  2 sessions, each bracketed by session events with its own header + end"

# (4) The refusal — and it must NAME the rule, not just fail.
grep -q '"k":"err"' "$SERVE_OUT" \
    || fail "--serve: the concurrent start was NOT refused (D6 budget unenforced)"
grep '"k":"err"' "$SERVE_OUT" | grep -q 'already running' \
    || fail "--serve: the refusal did not name the one-session rule"
# An err must not end the session: the log session still reached its `stop`.
grep -q '"k":"session","state":"stopped","mode":"log","events":[0-9]*,"reason":"stop"' "$SERVE_OUT" \
    || fail "--serve: the log session did not stop cleanly after the refusal"
echo "  a second concurrent start was refused with err, and the session survived it"

# A session's slice really is a valid recording: cut [header .. end] out of the
# stream and the reader-level checks the .asmtrace tests make must hold on it.
# Exactly the FIRST session: a sed range would restart and concatenate both,
# which is not what a client slicing one session out of the stream would get.
awk '/^{"asmtrace":1,/{inrec=1} inrec{print} /"k":"end"/{if (inrec) exit}' \
    "$SERVE_OUT" >"$RECDIR/serve_slice.asmtrace"
head -1 "$RECDIR/serve_slice.asmtrace" | grep -q '"provenance"' \
    || fail "--serve: the sliced recording has no provenance header"
tail -1 "$RECDIR/serve_slice.asmtrace" | grep -q '"k":"end"' \
    || fail "--serve: the sliced recording does not end with an end footer"
# The footer counts RECORDING events, and `cmd`/`err` are control lines that can
# land mid-session — so the client rule is "slice, then drop the serve-only
# kinds", and THAT is what has to reconcile with the footer. Checking it
# unfiltered would be checking the wrong contract.
slice_ev=$(grep '"k":"' "$RECDIR/serve_slice.asmtrace" \
    | grep -cvE '"k":"(session|cmd|err|end)"' || true)
slice_declared=$(tail -1 "$RECDIR/serve_slice.asmtrace" | sed 's/.*"events":\([0-9]*\).*/\1/')
[ "$slice_ev" = "$slice_declared" ] \
    || fail "--serve: the sliced recording's end claims $slice_declared events but carries $slice_ev"
# ...and the control lines really were inside it, or the rule above is vacuous.
grep -q '"k":"err"' "$RECDIR/serve_slice.asmtrace" \
    || fail "--serve: the refusal did not land inside the session slice — the filter rule is untested here"
echo "  a session's slice is a valid .asmtrace ($slice_ev events, footer agrees once control lines are dropped)"

# 39 T5: a session that ends on its OWN announces itself (T5.1, from the tracer
# tail — an idle client learns without sending a command), and a `stop` for that
# already-ended session is ACKed, not refused. Before, serve_reap ran at the top
# of the command loop and cleared `joinable`, so the very next `stop` hit
# "no session is running" — the desktop's Swap->stop knot. A bounded stream
# session self-ends during the sleep, and the following `stop` must be acked.
echo "--- asmspy --serve: a self-ended session is announced + its stop acked (39 T5) ---"
T5_OUT="$RECDIR/serve_t5.ndjson"
set +e
{
    printf '{"cmd":"start","mode":"stream","pid":%d,"max":2}\n' "$SRVPID"
    sleep 1 # the bounded session self-ends here (2 insns); its terminal `session`
            # event is announced with NO follow-up command driving it
    printf '{"cmd":"stop"}\n' # for the ALREADY self-ended session: must be ACKed
    printf '{"cmd":"quit"}\n'
    sleep 1
} | timeout 60 "$ASM" --serve >"$T5_OUT" 2>/dev/null
t5rc=$?
set -e
[ "$t5rc" -eq 124 ] && fail "--serve T5 hung"
grep -qE '"k":"session","state":"(stopped|skip)","mode":"stream"' "$T5_OUT" \
    || fail "--serve T5: the self-ended stream session was never announced (T5.1)"
grep -q '"k":"cmd","cmd":"stop"' "$T5_OUT" \
    || fail "--serve T5: the stop for a self-ended session was not acked (T5.3)"
if grep '"k":"err"' "$T5_OUT" | grep -q '"cmd":"stop"'; then
    fail "--serve T5: the stop was REFUSED — serve_reap cleared its own precondition (the 39 knot): $(grep '"k":"err"' "$T5_OUT")"
fi
# The reap-ack must NOT turn a genuinely-empty stop into a false ack: a stop with
# no session ever started is still refused.
NS_OUT="$RECDIR/serve_nostop.ndjson"
printf '{"cmd":"stop"}\n{"cmd":"quit"}\n' | timeout 30 "$ASM" --serve >"$NS_OUT" 2>/dev/null || true
grep '"k":"err"' "$NS_OUT" | grep -q 'no session is running' \
    || fail "--serve T5: a stop with no session ever started must still be refused"
echo "  self-ended session announced; its stop acked; a no-session stop still refused"
rm -f "$T5_OUT" "$NS_OUT"

kill -9 "$SRVPID" 2>/dev/null || true
wait "$SRVPID" 2>/dev/null || true
SRVPID=""

# ---------------------------------------------------------------------------
# --serve `launch`: the wire command (docs/internal/gui/45 T1+T2, landed
# together) — fork+exec a fresh target INSIDE the server and trace it FROM
# BIRTH, instead of attaching to a pid the client already has. No
# "launch_gap"/interim-fidelity field: the child is never detached and
# re-SEIZEd (T1's interim draft did this, and would have needed to flag the
# resulting gap; T2's from-birth path replaced it before this landed, so the
# session's trust tier needs no asterisk — same "exact" as any other
# whole-process attach).
# ---------------------------------------------------------------------------
echo "--- asmspy --serve launch (fork+exec inside the server, from birth) ---"
LAUNCH_OUT="$RECDIR/serve_launch.ndjson"
set +e
{
    printf '{"cmd":"launch","mode":"log","argv":["%s"]}\n' "$BUILD/syscall_victim"
    sleep 2
    # mode log has no "max", so it runs unbounded — "stop" it explicitly
    # before "quit" (an unbounded session survives a bare "quit" until its
    # OWN teardown loop notices, which never happens without a stop: the
    # same rule the earlier --serve block's log/stream sequence follows).
    printf '{"cmd":"stop"}\n'
    sleep 1
    printf '{"cmd":"quit"}\n'
    sleep 1
} | timeout 60 "$ASM" --serve >"$LAUNCH_OUT" 2>/dev/null
lrc=$?
set -e
[ "$lrc" -eq 124 ] && fail "--serve launch hung"
[ -s "$LAUNCH_OUT" ] || fail "--serve launch produced no output"
grep -q '"k":"cmd","cmd":"launch"' "$LAUNCH_OUT" \
    || fail "--serve launch: no ack for the launch command"
grep -qE '"k":"session","state":"started","mode":"log","pid":[0-9]+' "$LAUNCH_OUT" \
    || fail "--serve launch: no started event carrying a real pid"
grep -q '"launch_gap"' "$LAUNCH_OUT" \
    && fail "--serve launch: launch_gap present — T1's interim fidelity caveat was not retired"
grep -q '"k":"syscall"' "$LAUNCH_OUT" \
    || fail "--serve launch: the launched target's syscalls were never captured"
# T2's actual proof, not just "some syscalls arrived": the FIRST recorded
# syscall must be the dynamic linker's own opening move, not several calls
# into main() — the same property --launch's headless test asserts above,
# now over the full NDJSON wire protocol instead of the in-process helper.
first_sys=$(grep -m1 '"k":"syscall"' "$LAUNCH_OUT")
printf '%s\n' "$first_sys" | grep -qE '"line":"(brk|mmap|arch_prctl|access)\(' \
    || fail "--serve launch: first syscall event was '$first_sys', not an early dynamic-linker call (from-birth regressed)"
echo "  launch: ack + started(pid) + syscall events from birth (first event is the dynamic linker's own), no launch_gap"

echo "--- asmspy --serve launch: a bad argv[0] refuses cleanly, no hang ---"
BADLAUNCH_OUT="$RECDIR/serve_launch_bad.ndjson"
set +e
{
    printf '{"cmd":"launch","mode":"log","argv":["/no/such/binary"]}\n'
    sleep 2
    printf '{"cmd":"quit"}\n'
    sleep 1
} | timeout 30 "$ASM" --serve >"$BADLAUNCH_OUT" 2>/dev/null
blrc=$?
set -e
[ "$blrc" -eq 124 ] && fail "--serve launch (bad argv) hung"
grep '"k":"err"' "$BADLAUNCH_OUT" | grep -qi 'exec failed' \
    || fail "--serve launch (bad argv) did not refuse with a clean 'exec failed' reason"
grep -q '"k":"session","state":"started"' "$BADLAUNCH_OUT" \
    && fail "--serve launch (bad argv) must not report a started session"
echo "  launch: a bad argv[0] is refused cleanly (err), no started session, no hang"

echo "--- asmspy --serve launch: mode dataflow is refused (v1 scope) ---"
NOTDF_OUT="$RECDIR/serve_launch_dataflow.ndjson"
printf '{"cmd":"launch","mode":"dataflow","argv":["%s"]}\n{"cmd":"quit"}\n' \
    "$BUILD/syscall_victim" | timeout 15 "$ASM" --serve >"$NOTDF_OUT" 2>/dev/null || true
grep '"k":"err"' "$NOTDF_OUT" | grep -q 'not supported by' \
    || fail "--serve launch: mode dataflow should be refused with a clean reason (v1 scope)"
echo "  launch: mode dataflow is refused with a stated reason, not attempted"
rm -f "$LAUNCH_OUT" "$BADLAUNCH_OUT" "$NOTDF_OUT"

# ---------------------------------------------------------------------------
# --serve + codeimage: JIT-safe bytes for a region session
# (08-observer-views.md T7; schema: "`codeimage` — captured code bytes at a
# version")
# ---------------------------------------------------------------------------
# A region session tracks its region's bytes through asmtest_codeimage and
# streams `codeimage` versions, so a client can disassemble a step against the
# bytes that were live AT THAT STEP rather than re-reading a process that has
# since patched, freed or reused the address.
#
# This is NOT a self-skipping block. The recorder's availability is a kernel
# fact (soft-dirty / PAGEMAP_SCAN, Linux >= 6.7) and either answer is a testable
# outcome: versions on the wire, or a `note` carrying the MEASURED reason there
# are none. What is asserted is that the producer says one of the two — silence
# is the only failure, because silence is indistinguishable from nobody trying.
echo "--- asmspy --serve + codeimage (versioned bytes for a region) ---"
"$BUILD/auto_victim" 2>/dev/null &
CIPID=$!
sleep 1
kill -0 "$CIPID" 2>/dev/null || fail "codeimage: auto_victim did not start"
CI_OUT="$RECDIR/serve_codeimage.ndjson"
set +e
{
    printf '{"cmd":"start","mode":"trace","pid":%d,"func":"entered_often","max":3}\n' "$CIPID"
    sleep 4
    printf '{"cmd":"quit"}\n'
    sleep 1
} | timeout 90 "$ASM" --serve >"$CI_OUT" 2>/dev/null
circ=$?
set -e
[ "$circ" -eq 124 ] && fail "--serve (codeimage): hung"
[ "$circ" -eq 0 ] || fail "--serve (codeimage): exited $circ"
kill -0 "$CIPID" 2>/dev/null \
    || fail "--serve (codeimage): the victim did not survive the session"

nci=$(grep -c '"k":"codeimage"' "$CI_OUT" || true)
ncinote=$(grep '"k":"note"' "$CI_OUT" | grep -c 'codeimage unavailable' || true)
if [ "$nci" -gt 0 ]; then
    # Field order and the bytes/len invariant are the schema's, and a reader
    # resolving bytes at a time depends on both.
    grep '"k":"codeimage"' "$CI_OUT" \
        | grep -qE '^\{"k":"codeimage","base":[0-9]+,"len":[0-9]+,"version":[0-9]+,"when":[0-9]+,"bytes":"[0-9a-f]*"\}$' \
        || fail "codeimage: an event does not match the schema's field order"
    cilen=$(grep -m1 '"k":"codeimage"' "$CI_OUT" | sed 's/.*"len":\([0-9]*\).*/\1/')
    cihex=$(grep -m1 '"k":"codeimage"' "$CI_OUT" | sed 's/.*"bytes":"\([0-9a-f]*\)".*/\1/')
    [ "${#cihex}" = "$((cilen * 2))" ] \
        || fail "codeimage: len=$cilen but bytes carries ${#cihex} hex chars"
    # The entry int3 the region engine arms is REMOVED before the sink runs, so
    # a recorded version must never contain the tracer's own breakpoint at the
    # region's first byte — that would be the capture recording its own
    # perturbation as the target's code.
    case "$cihex" in
    cc*) fail "codeimage: the recorded bytes start with the tracer's own int3" ;;
    esac
    # A static region changes once (version 0) and then does not: the change
    # detector reports the tracer's own int3 write as a page change, and a
    # producer that emitted a version per invocation would bury a real JIT patch
    # in that noise.
    [ "$nci" -le 2 ] \
        || fail "codeimage: $nci versions for a STATIC region — byte-identical snapshots are not new versions"
    echo "  $nci codeimage version(s), well-formed, no tracer int3 in the bytes"
elif [ "$ncinote" -gt 0 ]; then
    grep '"k":"note"' "$CI_OUT" | grep 'codeimage unavailable' | head -1 | sed 's/^/  /'
    echo "  (no code image on this host — the MEASURED reason is on the wire, which is the contract)"
else
    fail "codeimage: the session emitted neither a codeimage version nor a note saying why not"
fi

kill -9 "$CIPID" 2>/dev/null || true
wait "$CIPID" 2>/dev/null || true

# ---------------------------------------------------------------------------
# --serve + dataflow: df_step states its codeimage `when` (37 T4)
# ---------------------------------------------------------------------------
# The serve sink is the ONLY producer that ever states `when` (it is the only
# one holding a live codeimage timeline) — the headless `--dataflow --record`
# case above asserts `rbase` but must carry NO `when`, and this asserts the
# serve path's positive case: a codeimage-capable host's df_step lines carry a
# `when` immediately after `rbase`, matching the schema's normative order.
echo "--- asmspy --serve + dataflow (df_step states its codeimage when, 37 T4) ---"
"$BUILD/auto_victim" 2>/dev/null &
DFWPID=$!
sleep 1
kill -0 "$DFWPID" 2>/dev/null || fail "when: auto_victim did not start"
DFW_OUT="$RECDIR/serve_dataflow_when.ndjson"
set +e
{
    printf '{"cmd":"start","mode":"dataflow","pid":%d,"func":"entered_often","max":64}\n' "$DFWPID"
    sleep 3
    printf '{"cmd":"quit"}\n'
    sleep 1
} | timeout 90 "$ASM" --serve >"$DFW_OUT" 2>/dev/null
dfwrc=$?
set -e
[ "$dfwrc" -eq 124 ] && fail "--serve dataflow (when): hung"
[ "$dfwrc" -eq 0 ] || fail "--serve dataflow (when): exited $dfwrc"
kill -0 "$DFWPID" 2>/dev/null \
    || fail "--serve dataflow (when): the victim did not survive the session"

ndfstep=$(grep -c '"k":"df_step"' "$DFW_OUT" || true)
[ "$ndfstep" -gt 0 ] || fail "--serve dataflow (when): no df_step events captured"
ndfw_ci=$(grep -c '"k":"codeimage"' "$DFW_OUT" || true)
if [ "$ndfw_ci" -gt 0 ]; then
    # Codeimage is available on this host: every df_step must carry `when`,
    # immediately after `rbase` (the schema's normative field order), and it
    # must be a positive integer — never a 0-as-unknown sentinel (D7).
    ndfw_when=$(grep '"k":"df_step"' "$DFW_OUT" | grep -cE '"rbase":[0-9]+,"when":[1-9][0-9]*,' || true)
    [ "$ndfw_when" = "$ndfstep" ] \
        || fail "--serve dataflow (when): $ndfw_when of $ndfstep df_step events carry a well-formed rbase-then-when"
    echo "  $ndfw_when/$ndfstep df_step events carry when, right after rbase"
else
    # No codeimage on this host: df_step must carry NO when at all — the
    # serve sink omits it exactly like the headless/corpus producers do,
    # rather than guessing.
    ndfw_nowhen=$(grep '"k":"df_step"' "$DFW_OUT" | grep -c '"when"' || true)
    [ "$ndfw_nowhen" -eq 0 ] \
        || fail "--serve dataflow (when): no codeimage, but $ndfw_nowhen df_step events carry when anyway"
    echo "  (no code image on this host — df_step correctly carries no when)"
fi

kill -9 "$DFWPID" 2>/dev/null || true
wait "$DFWPID" 2>/dev/null || true

# --serve --record: a SKIPPED LEG's reason must survive INSIDE the session
# file, not just on the live client stream (2026-08-06 plan, Task 9).
#
# WHY: the per-capture `end` footer already carries `skip` (rec_close), and
# that reaches the CLIENT stream — but the SESSION-level sink (--record=<f>,
# one writer/header for the whole session, 61 T7c) is deliberately closed only
# ONCE, at quit, so a per-engine close never stamps it with a second `end`
# (rec_close's own comment). Measured before this fix: an `auto` leg that
# self-skipped left the live stream's `end` with a full `skip:{code,reason}`
# while the recorded SESSION FILE's own footer was a bare
# `{"k":"end","events":0,"truncated":false,"drops":{...}}` — reopening the
# recording lost the only clue the capture ever ran, let alone why it came
# back empty. `--sampler=ibs` is forced (rather than bare `auto`, which chains
# to the perf-free ptrace picker and can succeed) so this is a DETERMINISTIC
# self-skip wherever perf is locked down (paranoid>2, no CAP_PERFMON) or IBS
# is simply absent — on any such host/lane the reason differs, but a skip
# happens either way; where perf IS reachable (docker-cli-ibs), the sampler
# runs for real and there is nothing for the file to lose (the else branch).
echo "--- asmspy --serve --record: a skipped leg's reason lands in the SESSION file (T9) ---"
"$BUILD/spy_victim" >/dev/null 2>&1 &
T9PID=$!
sleep 1
kill -0 "$T9PID" 2>/dev/null || fail "T9: spy_victim did not start"
T9SESS="$RECDIR/serve_t9_session.asmtrace"
T9OUT="$RECDIR/serve_t9_client.ndjson"
rm -f "$T9SESS" "$T9OUT"
set +e
{
    printf '{"cmd":"start","mode":"auto","pid":%d,"sampler":"ibs","max":50}\n' "$T9PID"
    sleep 2
    printf '{"cmd":"quit"}\n'
    sleep 1
} | timeout 40 "$ASM" --serve --record="$T9SESS" >"$T9OUT" 2>/dev/null
t9rc=$?
set -e
[ "$t9rc" -eq 124 ] && fail "--serve --record (T9): hung"
[ "$t9rc" -eq 0 ] || fail "--serve --record (T9): exited $t9rc"
[ -s "$T9SESS" ] || fail "--serve --record (T9): no session file written"
kill -0 "$T9PID" 2>/dev/null \
    || fail "--serve --record (T9): the victim did not survive the session"

if grep -q '"k":"end".*"skip"' "$T9OUT"; then
    # The IBS sampler self-skipped on THIS host/lane — the leg this task
    # exists for. Pull the reason it measured on the CLIENT stream (already
    # correct before this fix) and require the SESSION FILE to carry the
    # identical text, not a generic placeholder.
    skipreason=$(grep -o '"reason":"[^"]*"' "$T9OUT" | head -1 | sed 's/^"reason":"//; s/"$//')
    [ -n "$skipreason" ] || fail "T9: could not extract the measured skip reason from the client stream"

    grep -q '"k":"note"' "$T9SESS" \
        || fail "T9: the session file has no note — reopening it loses why the leg captured nothing"
    grep '"k":"note"' "$T9SESS" | grep -qF "$skipreason" \
        || fail "T9: the session file's note does not carry the MEASURED reason verbatim (got: $(grep '"k":"note"' "$T9SESS"))"

    # The trap: session/cmd/err are serve-only kinds (asmtrace-schema.md) and
    # must NEVER be teed into a `.asmtrace` file — the fix is a `note`, not a
    # copy of the client-side `session state:"skip"` event.
    grep -q '"k":"session"' "$T9SESS" \
        && fail "T9: a serve-only 'session' event was teed into the recording (schema forbids this)"

    tail -1 "$T9SESS" | grep -q '"k":"end"' \
        || fail "T9: the session file's last line is not an end footer (TORN recording)"
    tail -1 "$T9SESS" | grep -q '"skip":{"code":2,' \
        || fail "T9: the session file's OWN end footer still carries no skip (the call-site fix regressed)"
    tail -1 "$T9SESS" | grep -qF "$skipreason" \
        || fail "T9: the session file's own end.skip does not carry the measured reason"
    echo "  IBS self-skipped ($skipreason) — the session file carries it as a note AND on its own end footer"
else
    # perf reachable here (docker-cli-ibs, or paranoid<=2/CAP_PERFMON): the
    # sampler ran for real, so this lane exercises the success path instead —
    # legitimate per CLAUDE.md (hardware/perf access is a real gate), and the
    # well-formed-file checks above already ran regardless of which path.
    echo "  (IBS sampler ran for real on this host/lane — nothing skipped, T9's note/skip checks not exercised here)"
fi

kill -9 "$T9PID" 2>/dev/null || true
wait "$T9PID" 2>/dev/null || true

# ---------------------------------------------------------------------------
# --info — the attach-free process snapshot
# ---------------------------------------------------------------------------
echo "--- --info (attach-free process snapshot) ---"

# Against OUR OWN shell — a target this smoke usually holds no ptrace
# permission for under ptrace_scope=1. NOTE: a clean exit here does NOT by
# itself prove --info never attached (a failed attach is not fatal to any
# assertion below, and this repo's own docs tell operators to set
# ptrace_scope=0, under which the attach could even succeed) — it only
# proves the snapshot itself is well-formed. The actual never-attaches
# enforcement is the `strace -f -e trace=ptrace` probe further down, which
# observes the syscall directly rather than inferring its absence from
# black-box behavior.
info_json="$($BUILD/asmspy --info $$ --json 2>/dev/null)"

echo "$info_json" | head -1 | grep -q '"asmtrace"' \
    || fail "--info --json: no .asmtrace header line"
echo "$info_json" | grep -q '"k":"procinfo"' \
    || fail "--info --json: no procinfo event"
echo "$info_json" | tail -1 | grep -q '"k":"end"' \
    || fail "--info --json: no end footer"
echo "$info_json" | grep -q "\"pid\":$$" \
    || fail "--info --json: wrong pid in identity"
echo "$info_json" | grep -q '"attachable"' \
    || fail "--info --json: no trace verdict"

# The human form names the process and its runtime.
$BUILD/asmspy --info $$ | grep -q "pid $$" \
    || fail "--info text: no pid line"

# 2026-08-06 plan, Task 2: --info MEASURES attachability rather than inferring it
# from the Yama scope. NOT against $$: $$ here is THIS SCRIPT's pid, and asmspy
# runs as a brand-new CHILD process per invocation, so "asmspy --info $$" would
# be a child probing its own PARENT's mem. Measured directly (strace + three
# hand-built fork probes: parent->child open succeeds, child->parent fails,
# sibling->sibling fails) — child->parent is -EPERM under scope 1 with no
# PR_SET_PTRACER, exactly the descendant-only relationship Yama documents, not
# a gap in the probe. $AVPID (examples/attach_victim, started at the top of
# this file and still running here) calls PR_SET_PTRACER_ANY on itself, which
# IS a relationship scope 1 accepts from an unrelated process without root —
# the same mechanism a browser's crash reporter uses, and the case this task
# exists for.
kill -0 "$AVPID" 2>/dev/null \
    || fail "attach_victim ($AVPID) is not alive for the --info measured-yes check"
selfinfo=$("$ASM" --info "$AVPID" 2>&1) || fail "--info on an opted-in target failed"
printf '%s\n' "$selfinfo" | grep -q 'attach *YES' \
    || fail "--info must report a MEASURED yes for a target it can open (got: $(printf '%s\n' "$selfinfo" | grep -i attach))"
printf '%s\n' "$selfinfo" | grep -qi 'launch the target from asmspy' \
    && fail "the launch remedy is measured broken for confined targets and must not be offered unconditionally"
echo "--info: attachability is measured, not inferred"

# The check above only ever exercises the SUCCESS branch of the yama>=1 arm
# ($AVPID always measures attachable=1) — attach_remedy is untouched on that
# path (cli/asmspy.c only prints it `if (pi->attach_remedy[0])`), so the
# "launch" grep finding nothing there is GUARANTEED regardless of whether
# asmspy_target_is_confined() is correct, inverted, or deleted. Prove the
# REFUSED branch, and its remedy, for real: a plain `sleep` backgrounded here
# is a SIBLING of the asmspy process this script goes on to invoke (both are
# children of THIS script; neither is the other's descendant) — confirmed
# directly with three hand-built fork probes (parent->child open succeeds,
# child->parent and sibling->sibling both fail), the same descendant-only
# rule that refuses $$ above. Plain `sleep` carries no LSM label of its own,
# so a CORRECT remedy must be the unconfined one: "...or launch the target
# from asmspy". Verified this catches a real regression: stubbing
# asmspy_target_is_confined() to unconditionally `return 1` flips this
# target's remedy to the confined-only sysctl text and fails the assertion
# below, while the $AVPID checks above stay green throughout — the AVPID-only
# check was insufficient by itself. The CONFINED direction (a refused target
# that correctly gets the sysctl-only remedy) has no privilege-free target in
# this lane — no confined process exists here — so it stays covered only by
# the task's Step 5 manual measurement (a live, confined, refused
# snapd-desktop-integration process), not by this smoke lane.
#
# Guarded on non-root AND on an ENFORCING yama scope, matching BOTH halves of
# the C arm this block exercises (`yama >= 1 && geteuid() != 0`,
# cli/asmspy_proc.c) — neither guard alone is enough, and NEITHER is a silent
# self-skip: each states plainly which fact of the two took the arm out of
# play.
#
# Root: `make docker-cli` (Dockerfile.cli declares no USER, and mk/cli.mk's
# `docker run` passes no --user, so that documented local-verify path runs
# this whole script as UID 0) makes `geteuid() != 0` false, so the arm never
# runs AT ALL — every same-uid target, including this sibling `sleep`, falls
# through to the unconditional "same uid, nothing else traces it" success
# case. Correct kernel behavior (root bypasses Yama entirely), not a bug:
# there is no same-uid target root can be refused on.
#
# ptrace_scope=0: makes `yama >= 1` false, the SAME arm-skip from the OTHER
# half. Not hypothetical — docs/getting-started/host-setup.md *instructs*
# operators to set `kernel.yama.ptrace_scope=0`, and Yama is not namespaced
# (already documented a few hundred lines above, for a different assertion,
# at ~3780: "make docker-cli inherits whatever the HOST already has"), so a
# developer who followed this repo's own setup guide and then ran
# `make cli-smoke` -- container OR bare host -- hits the identical
# arm-skipped, assertion-demands-NO, fail()-aborts shape as the root case.
# Absent/unreadable ptrace_scope (no Yama LSM loaded at all) is the SAME
# "not enforcing" state as 0 and defaults there rather than erroring.
#
# The CI gate (.github/workflows/ci.yml) runs `make cli-smoke` directly on a
# bare, non-root runner at the kernel default ptrace_scope=1 — where NEITHER
# guard fires and the real assertion below is what actually executes. Root
# and scope=0 are the exception paths here, not the common one.
yama_scope=$(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null)
[ -n "$yama_scope" ] || yama_scope=0
if [ "$(id -u)" -eq 0 ]; then
    echo "--info: SKIPPING the refused-target assertion -- running as root (id -u"
    echo "  = 0, e.g. under 'make docker-cli': Dockerfile.cli declares no USER)."
    echo "  Yama's ptrace_scope does not gate root at all, so no same-uid target,"
    echo "  including a sibling sleep, can be refused here to measure NO against."
    echo "  This is a fact about root, not a self-skip of the feature: the"
    echo "  assertion runs for real in the CI lane and any normal (non-root) run."
elif [ "$yama_scope" -lt 1 ] 2>/dev/null; then
    echo "--info: SKIPPING the refused-target assertion -- yama ptrace_scope=$yama_scope"
    echo "  (docs/getting-started/host-setup.md instructs operators to set this to"
    echo "  0, and Yama is not namespaced, so make docker-cli inherits it from the"
    echo "  host too). Below scope 1, Yama does not restrict same-uid ptrace at"
    echo "  all, so no same-uid target, including a sibling sleep, can be refused"
    echo "  here to measure NO against. This is a fact about the sysctl, not a"
    echo "  self-skip of the feature: the assertion runs for real at the kernel"
    echo "  default ptrace_scope=1 (this host, and every CI runner)."
else
    sleep 100 &
    SIBPID=$!
    sleep 0.3
    kill -0 "$SIBPID" 2>/dev/null \
        || fail "sibling sleep ($SIBPID) is not alive for the --info measured-no check"
    sibinfo=$("$ASM" --info "$SIBPID" 2>&1) || fail "--info on a refused sibling target failed"
    sibattach=$(printf '%s\n' "$sibinfo" | grep -iE 'attach|->')
    printf '%s\n' "$sibinfo" | grep -q 'attach *NO' \
        || fail "--info must report a MEASURED no for an unrelated same-uid target (got: $sibattach)"
    printf '%s\n' "$sibinfo" | grep -qi 'launch the target from asmspy' \
        || fail "--info: an unconfined refused target must still offer the launch remedy (got: $sibattach)"
    kill "$SIBPID" 2>/dev/null || true
    wait "$SIBPID" 2>/dev/null || true
    echo "--info: a genuinely refused, unconfined target measures NO and keeps the launch remedy"
fi

# A nonexistent pid is refused, not rendered blank — and refused with its OWN
# exit code (ASMSPY_INFO_EXIT_NO_SUCH_PID = 3, cli/libasmspy.h), not the
# generic 1 that also means "the --record you asked for could not be
# written". A caller that browses a live process list hits this race
# routinely (the desktop details pane fires a probe on every selection
# change) and must be able to tell it from a broken asmspy: the pane renders
# 3 as "pid N exited" and anything else as a failure.
set +e
$BUILD/asmspy --info 134217727 >/dev/null 2>&1
nprc=$?
set -e
[ "$nprc" -eq 3 ] \
    || fail "--info: a nonexistent pid must exit 3 (ASMSPY_INFO_EXIT_NO_SUCH_PID), got $nprc"

# A --record path that cannot be opened must exit nonzero too (finding A):
# info_emit_json's failure must reach cmd_info's return value, not just its
# stderr line — "asmspy --info $p --json --record=/mnt/full/x && upload x"
# must never fire the && on a recording that was never written.
if $BUILD/asmspy --info $$ --json --record=/nonexistent/x >/dev/null 2>&1; then
    fail "--info: a --record that cannot be opened must exit nonzero"
fi

# Strict structural checks when python3 is present; degrade cleanly otherwise
# (same pattern as the --graph --json checks above).
#
# Two properties earn their own assertion here rather than a passing grep:
#  1. addresses cross the wire as hex STRINGS, never JSON numbers — a number
#     is a double in many readers, which silently rounds a 64-bit pointer.
#     `modules[0].base` is always present (every process maps a module), so
#     it is the reliable probe.
#  2. every thread row carries EXACTLY ONE of `syscall` / `syscall_why` —
#     never both, never neither. This used to assert `syscall_why`
#     specifically and `syscall` absent, on the assumption that $$ (this
#     smoke's own shell) never has ptrace permission on itself — true under
#     the default `ptrace_scope=1`, but this repo's own
#     docs/getting-started/host-setup.md tells operators to SET
#     `ptrace_scope=0`, and Yama is not namespaced, so `make docker-cli`
#     inherits whatever the HOST already has. Under scope=0 (or
#     CAP_SYS_PTRACE, or root) `/proc/<tid>/syscall` on our own pid can be
#     readable, and the old assertion broke on a premise it never actually
#     verified — the emitter is correct in both states; only the test's
#     assumption about WHICH branch fires was wrong. The dichotomy is what
#     the emitter's if/else construction always guarantees, in every
#     environment (mirrors the "have_syscall implies a why" invariant
#     cli/test_procinfo.c checks at the struct level, :329-363 / :547-568,
#     extended here to a strict either/or since the JSON omits one key
#     entirely rather than leaving it blank).
if command -v python3 >/dev/null 2>&1; then
    printf '%s' "$info_json" | python3 -c 'import json,sys
lines = [l for l in sys.stdin if l.strip()]
assert len(lines) == 3, "expected header + procinfo + end, got %d lines" % len(lines)
hdr, evt, end = (json.loads(l) for l in lines)
assert hdr.get("asmtrace") == 1
assert evt["k"] == "procinfo"
assert end["k"] == "end"
m0 = evt["modules"][0]
assert isinstance(m0["base"], str) and m0["base"].startswith("0x"), \
    "modules[0].base is not a hex string: %r" % (m0["base"],)
assert isinstance(m0["size"], int), \
    "modules[0].size should stay a JSON number: %r" % (m0["size"],)
assert evt["threads"], "no thread rows to check the syscall dichotomy on"
for t in evt["threads"]:
    has_sc = "syscall" in t
    has_why = "syscall_why" in t
    assert has_sc != has_why, \
        "thread %r carries syscall=%s syscall_why=%s -- exactly one must be present" % (
            t.get("tid"), has_sc, has_why)
    if has_why:
        assert t["syscall_why"], "syscall_why key present but empty on thread %r" % (t.get("tid"),)
print("  json validated (python3 json.load): hex-string base, syscall XOR syscall_why on every thread")' \
        || fail "--info --json: structural check failed (address-as-string / syscall dichotomy)"
else
    echo "  json structural checks skipped (python3 absent)"
fi

# Couple the checked-in desktop fixture to the LIVE producer (finding: Task
# 6's runner resolves `asmspy` off $PATH, so the binary parsed at runtime
# need not be the one desktop/test/fixtures/procinfo_full.asmtrace came
# from -- nothing before this cross-checked them, so a producer key rename
# is undetectable by construction). A KEY-SET comparison, not a byte
# comparison: the fixture's VALUES (pid, timestamps, addresses) are host-
# and run-specific and will never match; its STRUCTURE (which keys exist,
# at every nesting level, unioned across array elements) should.
#
# Two normalizations are load-bearing, not cosmetic (review found this
# check hard-failed a CORRECT tree on both counts):
#  1. threads[].syscall / threads[].syscall_why is a DELIBERATE, already-
#     asserted dichotomy (the XOR check just above) driven by ptrace
#     PERMISSION, not by the producer's schema -- under --cap-add=SYS_PTRACE
#     or ptrace_scope=0 (which docs/getting-started/host-setup.md tells
#     operators to set, and Yama is not namespaced, so make docker-cli
#     inherits whatever the host has), a live thread reports `syscall`
#     where the fixture (captured under denial) reports `syscall_why`,
#     and neither "renamed/removed" nor "fixture is stale" is true. Both
#     keys, and everything nested under `syscall` (nr/name/args/pc/sp/
#     pc_sym -- equally permission-gated), collapse to one synthetic key
#     before differencing.
#  2. An array that is empty on EITHER side contributes no per-element
#     keys there BY CONSTRUCTION (there is nothing to derive them from) --
#     `children[]` on a childless target is the routine case, not an edge
#     case. Keys derived from an array empty on either side are excluded
#     from the diff entirely; the array's own key (present either way,
#     even as `[]`) is still compared normally.
if command -v python3 >/dev/null 2>&1 && [ -f desktop/test/fixtures/procinfo_full.asmtrace ]; then
    printf '%s' "$info_json" | python3 -c 'import json,sys

def keyset(obj, prefix=""):
    keys = set()
    if isinstance(obj, dict):
        for k, v in obj.items():
            if prefix == "threads" and k in ("syscall", "syscall_why"):
                keys.add("threads.syscall_or_why")
                continue
            path = prefix + "." + k if prefix else k
            keys.add(path)
            keys |= keyset(v, path)
    elif isinstance(obj, list):
        for item in obj:
            keys |= keyset(item, prefix)
    return keys

def empty_array_prefixes(obj, prefix=""):
    empties = set()
    if isinstance(obj, dict):
        for k, v in obj.items():
            path = prefix + "." + k if prefix else k
            if isinstance(v, list):
                if len(v) == 0:
                    empties.add(path)
                else:
                    for item in v:
                        empties |= empty_array_prefixes(item, path)
            elif isinstance(v, dict):
                empties |= empty_array_prefixes(v, path)
    return empties

def under_empty_array(key, prefixes):
    return any(key.startswith(p + ".") for p in prefixes)

live_lines = [l for l in sys.stdin if l.strip()]
live_evt = next(json.loads(l) for l in live_lines if json.loads(l).get("k") == "procinfo")

with open("desktop/test/fixtures/procinfo_full.asmtrace") as f:
    fixture_lines = [l for l in f if l.strip()]
fixture_evt = next(json.loads(l) for l in fixture_lines if json.loads(l).get("k") == "procinfo")

fk = keyset(fixture_evt)
lk = keyset(live_evt)
empties = empty_array_prefixes(fixture_evt) | empty_array_prefixes(live_evt)
missing = {k for k in (fk - lk) if not under_empty_array(k, empties)}
extra = {k for k in (lk - fk) if not under_empty_array(k, empties)}
assert not missing, "keys the fixture has but --info --json no longer emits (renamed/removed?): %s" % sorted(missing)
assert not extra, "keys --info --json emits that the fixture does not (fixture is stale?): %s" % sorted(extra)
print("  desktop/test/fixtures/procinfo_full.asmtrace key set matches live --info --json output (%d keys)" % len(fk))' \
        || fail "--info --json: key set diverged from desktop/test/fixtures/procinfo_full.asmtrace"
else
    echo "  fixture/producer key-set coupling check skipped (python3 or the fixture absent)"
fi

# THE premise of --info, actually enforced: every assertion above is a
# black-box behavioral check that would stay green even if --info attached —
# a failed attach is not fatal to any of them, and an attach that SUCCEEDED
# (against a target that opted in via PR_SET_PTRACER_ANY, like spy_victim
# below) would SIGSTOP a live process while every other assertion in this
# section kept passing. strace observes the ptrace(2) syscall itself,
# independent of whether an attach would succeed or fail against the target,
# so it is the one check here that cannot be fooled by permission state.
if command -v strace >/dev/null 2>&1; then
    "$BUILD/spy_victim" >/dev/null 2>&1 &
    NAPID=$!
    sleep 1
    kill -0 "$NAPID" 2>/dev/null || fail "spy_victim did not start for the never-ptrace probe"
    NASTRACE="$BUILD/info_never_ptrace.strace"
    rm -f "$NASTRACE"
    # set +e / set -e (this file's own convention, e.g. the i386 --info block
    # above): under set -eu a nonzero strace/asmspy exit would kill the WHOLE
    # script right here, before narc=$? or the fail() below ever ran — and
    # since this line's stdout+stderr are both /dev/null, that death would be
    # SILENT, and leave NAPID's spy_victim running forever (nothing past this
    # line, including its own kill/wait cleanup, would ever execute).
    set +e
    strace -f -e trace=ptrace -o "$NASTRACE" -- "$BUILD/asmspy" --info "$NAPID" --json >/dev/null 2>&1
    narc=$?
    set -e
    [ "$narc" -eq 0 ] || fail "--info under strace exited $narc against a live, attachable target"
    kill -0 "$NAPID" 2>/dev/null \
        || fail "the never-ptrace probe target died — --info must never touch it"
    kill "$NAPID" 2>/dev/null || true
    wait "$NAPID" 2>/dev/null || true
    NAPID=""
    nptrace=$(grep -c "ptrace(" "$NASTRACE" || true)
    if [ "$nptrace" -ne 0 ]; then
        cat "$NASTRACE" >&2
        fail "--info made $nptrace ptrace(2) call(s) against a live target — it must never attach"
    fi
    rm -f "$NASTRACE"
    echo "  --info under 'strace -f -e trace=ptrace': zero ptrace(2) calls against a live, attachable target"
else
    echo "  never-ptrace strace probe skipped (strace absent)"
fi

# Regression test for finding E (the argv escape-buffer clip): a real
# process with a single argv entry past the 1024-byte buffer this used to
# clip at, but still well under the gatherer's real 4096-byte/64-entry cap,
# must cross the wire INTACT — not silently cut to ~1017 chars with
# argv_truncated staying false, which is exactly what shipped before the
# fix (a 2600-char entry clipped to 1017, while the human TEXT form of the
# SAME snapshot printed all of it).
if command -v python3 >/dev/null 2>&1; then
    BIGARG=$(python3 -c "print('a' * 2600)")
    # python3, NOT `sh -c 'sleep 60; :' "$BIGARG"`. That form leaked TWO
    # orphaned `sleep 60` grandchildren per bare-host run: the shell forks
    # sleep, and killing $! (the shell) leaves the sleep behind. The obvious
    # repair — `exec sleep 60` — is WRONG here on both available shells: it
    # does not remove the grandchild under dash, and under bash-as-/bin/sh
    # the exec optimization replaces the shell with sleep and DISCARDS the
    # 2600-byte argv this very test measures, turning the assertion below
    # red. python3 does its own sleeping in-process, so $! is the only
    # process there is, and its argv keeps the big entry. Already inside the
    # `command -v python3` guard.
    python3 -c 'import time; time.sleep(60)' "$BIGARG" >/dev/null 2>&1 &
    BAPID=$!
    sleep 1
    kill -0 "$BAPID" 2>/dev/null || fail "the big-argv victim did not start"
    baout=$($BUILD/asmspy --info "$BAPID" --json 2>/dev/null)
    printf '%s' "$baout" | sed -n '2p' | python3 -c 'import json,sys
evt = json.loads(sys.stdin.readline())
argv = evt["identity"]["argv"]
assert argv, "no argv captured for the big-argv victim"
last = argv[-1]
assert len(last) == 2600, "argv[-1] is %d chars, expected 2600 (clipped?)" % len(last)
assert evt["identity"]["argv_truncated"] is False, \
    "argv_truncated is %r for a snapshot well under the real cap" % (evt["identity"]["argv_truncated"],)
print("  --info --json: a 2600-char argv entry crosses the wire intact (argv_truncated:false)")' \
        || fail "--info --json: the argv escape-buffer regression check failed"
    kill "$BAPID" 2>/dev/null || true
    wait "$BAPID" 2>/dev/null || true
    BAPID=""
else
    echo "  argv escape-buffer regression check skipped (python3 absent)"
fi

# UTF-8 sanitization (finding, deferred twice as "pre-existing" until a later
# review measured its real impact): comm/argv/exe/cwd/module path/the
# header's cmd are arbitrary KERNEL bytes with no encoding guarantee. A raw
# invalid byte used to pass straight through asmtrace_escape, and nlohmann
# (and every conformant JSON parser) rejects invalid UTF-8 outright -- so the
# desktop failed to load the WHOLE recording, not just the field carrying
# the bad byte. Reproduces the exact repro that found this: an ordinary,
# unprivileged process launched from a latin-1-named directory (a real 0xe9
# byte, standalone -- not valid UTF-8 on its own) with a matching bad byte
# in its own argv too.
if command -v python3 >/dev/null 2>&1; then
    U8BASE=$(mktemp -d)
    U8DIR="$U8BASE/lat$(printf '\351')n1"
    mkdir -p "$U8DIR"
    # python3, for the same grandchild-orphan reason as the big-argv victim
    # above (python sleeps in-process; a shell forks `sleep` and leaves it).
    # The raw 0xe5 byte survives argv unharmed — CPython decodes argv with
    # surrogateescape and this script never reads it; what matters is that
    # /proc/<pid>/cmdline carries the invalid byte for asmspy to sanitize.
    ( cd "$U8DIR" && exec python3 -c 'import time; time.sleep(60)' \
        "$(printf 'arg\345bad')" ) >/dev/null 2>&1 &
    U8PID=$!
    sleep 1
    kill -0 "$U8PID" 2>/dev/null || fail "the non-UTF-8 cwd/argv victim did not start"
    u8out=$($BUILD/asmspy --info "$U8PID" --json 2>/dev/null)
    printf '%s' "$u8out" | python3 -c 'import json,sys
data = sys.stdin.buffer.read()
text = data.decode("utf-8")
lines = [l for l in text.split("\n") if l.strip()]
assert len(lines) == 3, "expected header + procinfo + end, got %d lines" % len(lines)
for l in lines:
    json.loads(l)
print("  --info --json: a non-UTF-8 cwd/argv still crosses the wire as valid UTF-8 and parses")' \
        || fail "--info --json: non-UTF-8 cwd/argv broke the recording (invalid UTF-8 passed through raw)"
    kill "$U8PID" 2>/dev/null || true
    wait "$U8PID" 2>/dev/null || true
    U8PID=""
    rm -rf "$U8BASE"
else
    echo "  non-UTF-8 cwd/argv regression check skipped (python3 absent)"
fi

# It must be FAST — this is fired automatically as an operator browses.
t0=$(date +%s%N)
$BUILD/asmspy --info $$ --json >/dev/null 2>&1
t1=$(date +%s%N)
ms=$(( (t1 - t0) / 1000000 ))
echo "    --info wall: ${ms}ms"
[ "$ms" -lt 1000 ] || fail "--info took ${ms}ms — too slow to fire on selection"

echo "    --info OK"

echo "cli-smoke: PASS"
