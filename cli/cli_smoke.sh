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

# Unit-test the TUI scrollback viewport math first (pure, no ncurses/ptrace).
echo "--- test_logview (TUI scrollback math) ---"
"$BUILD/test_logview" || fail "test_logview"

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
trap 'kill "$AVPID" ${WVPID:+"$WVPID"} ${SVPID:+"$SVPID"} ${TVPID:+"$TVPID"} 2>/dev/null || true' EXIT INT TERM
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
kill "$WVPID" 2>/dev/null || true

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
set +e
out=$(timeout 30 "$ASM" --procs "$TVPID" 120 2>&1); rc=$?
set -e
[ "$rc" -eq 124 ] && fail "--procs hung on a multi-threaded target"
printf '%s\n' "$out" | head -8
printf '%s\n' "$out" | grep -qE "^node $TVPID \[threads_victim\]  inv=[0-9]" \
    || fail "--procs: no process node with a syscall count"
nt=$(printf '%s\n' "$out" | grep -cE '^  tid [0-9]+.*inv=[0-9]')
echo "thread rows: $nt"
[ "$nt" -ge 2 ] || fail "--procs: expected >=2 thread rows, saw $nt"
# calls mode (single-step) also produces a counted topology
out2=$(timeout 40 "$ASM" --procs "$TVPID" 60 --count=calls 2>&1) \
    || fail "--procs --count=calls"
printf '%s\n' "$out2" | grep -qE '^node [0-9]+.*inv=[0-9]' \
    || fail "--procs --count=calls: no counted node"
# a bad --count value is rejected up front (rc=2)
expect_badarg "$ASM" --procs "$TVPID" --count=bogus
kill "$TVPID" 2>/dev/null || true

echo "cli-smoke: PASS"
