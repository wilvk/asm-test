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
trap 'kill "$AVPID" ${WVPID:+"$WVPID"} ${SVPID:+"$SVPID"} ${TVPID:+"$TVPID"} ${DVPID:+"$DVPID"} ${CVPID:+"$CVPID"} ${JVPID:+"$JVPID"} ${UPID:+"$UPID"} ${IPID:+"$IPID"} ${YPID:+"$YPID"} ${MVPID:+"$MVPID"} ${HWPID:+"$HWPID"} ${DLPID:+"$DLPID"} 2>/dev/null || true; rm -f ${JVPID:+"/tmp/perf-$JVPID.map"} ${UPID:+"$BUILD/jit-$UPID.dump"} "$BUILD/int3_swallow.log" "$BUILD/tid_victim.log" "$BUILD/watch_victim.log" 2>/dev/null || true; rm -rf "$BUILD/debuglink_t" 2>/dev/null || true' EXIT INT TERM
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
kill "$WVPID" 2>/dev/null || true

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
    echo "(IBS-Op unavailable on this host — sampler self-skipped, OK)"
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
sleep 1
kill -0 "$YPID" 2>/dev/null || fail "tid_victim did not start"
ATID=$(sed -n 's/^alpha tid=\([0-9][0-9]*\).*/\1/p' "$TLOG" | head -1)
[ -n "$ATID" ] || fail "tid_victim did not report alpha's tid"
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
