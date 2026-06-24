#!/bin/sh
#
# tests/expect.sh — meta-test harness for the asm-test framework (Track A of
# docs/plans/expansion-plan.md).
#
# Treats the framework as a black box through its real main(): builds (via the
# Makefile) and runs the positive/negative suite binaries, then asserts on exit
# codes and output substrings — proving the framework's assertions FAIL when
# they should, that SKIP/SETUP work, and that --list/--filter/--shuffle/--format
# /--timeout and crash containment behave as documented.
#
# Pins on stable substrings and exit codes, not exact formatting, so cosmetic
# TAP changes don't break it. Output is captured (not a tty), so the runner
# emits no color codes. POSIX sh; no dependencies beyond the built binaries
# (xmllint is used only if present).

set -u

BUILD="${BUILD:-./build}"
POS="$BUILD/tests_positive"
NEG="$BUILD/tests_negative"

pass=0
fail=0

ok() {
    pass=$((pass + 1))
    printf 'ok %d - %s\n' "$((pass + fail))" "$1"
}
bad() {
    fail=$((fail + 1))
    printf 'not ok %d - %s\n' "$((pass + fail))" "$1"
    if [ "$#" -gt 1 ]; then printf '  # %s\n' "$2"; fi
}

# Run "$@", capturing combined output in $out and exit status in $rc.
run() {
    out=$("$@" 2>&1)
    rc=$?
}

# expect_exit DESC WANT_RC CMD [args...]
expect_exit() {
    desc=$1
    want=$2
    shift 2
    run "$@"
    if [ "$rc" -eq "$want" ]; then ok "$desc"; else bad "$desc" "exit $rc, want $want"; fi
}

# expect_contains DESC SUBSTR CMD [args...]
expect_contains() {
    desc=$1
    sub=$2
    shift 2
    run "$@"
    case $out in
    *"$sub"*) ok "$desc" ;;
    *) bad "$desc" "output missing: $sub" ;;
    esac
}

# expect_fail_msg DESC SUBSTR CMD [args...] : nonzero exit AND substring present.
expect_fail_msg() {
    desc=$1
    sub=$2
    shift 2
    run "$@"
    if [ "$rc" -eq 0 ]; then
        bad "$desc" "expected nonzero exit, got 0"
    else
        case $out in
        *"$sub"*) ok "$desc" ;;
        *) bad "$desc" "nonzero exit OK but output missing: $sub" ;;
        esac
    fi
}

echo "# asm-test framework self-tests (expect.sh)"

# ---- positive suite: a clean run exits 0 ----
expect_exit "positive suite exits 0" 0 "$POS"
expect_contains "positive suite reports a pass" "ok 1 -" "$POS"

# ---- each negative assertion fails with the right diagnostic ----
# Run filtered (which also exercises --filter), checking nonzero exit + message.
expect_fail_msg "ASSERT_EQ fails"            "ASSERT_EQ"  "$NEG" --filter=neg.eq
expect_fail_msg "ASSERT_NE fails"            "ASSERT_NE"  "$NEG" --filter=neg.ne
expect_fail_msg "ASSERT_LT fails"            "ASSERT_LT"  "$NEG" --filter=neg.lt
expect_fail_msg "ASSERT_TRUE fails"          "ASSERT_TRUE" "$NEG" --filter=neg.truefail
expect_fail_msg "ASSERT_FALSE fails"         "ASSERT_FALSE" "$NEG" --filter=neg.falsefail
expect_fail_msg "ASSERT_ULT unsigned order"  "ASSERT_ULT" "$NEG" --filter=neg.ult_ordering
expect_fail_msg "ASSERT_STREQ fails"         "ASSERT_STREQ" "$NEG" --filter=neg.streq
expect_fail_msg "ASSERT_MEM_EQ fails"        "first diff at byte" "$NEG" --filter=neg.mem_eq
expect_fail_msg "ASSERT_ABI_PRESERVED fails" "not restored" "$NEG" --filter=neg.abi
expect_fail_msg "ASSERT_FLAG_SET fails"      "ASSERT_FLAG_SET" "$NEG" --filter=neg.flag_set
expect_fail_msg "ASSERT_FLAG_CLEAR fails"    "ASSERT_FLAG_CLEAR" "$NEG" --filter=neg.flag_clear
expect_fail_msg "ASSERT_VEC_EQ fails"        "first diff at byte" "$NEG" --filter=neg.vec_eq
expect_fail_msg "ASSERT_*EQ (double) fails"  "double" "$NEG" --filter=neg.fp_eq
expect_fail_msg "ASSERT_*NEAR (double) fails" "ulps"  "$NEG" --filter=neg.fp_near
expect_fail_msg "ASSERT_FEQ (float) fails"   "float" "$NEG" --filter=neg.feq

# ---- crash / timeout / abort containment ----
# SIGSEGV is caught in-process and reported (both with and without fork).
expect_fail_msg "SIGSEGV contained (fork)"    "fatal signal" "$NEG" --filter=neg.crash
expect_fail_msg "SIGSEGV contained (no-fork)" "fatal signal" "$NEG" --filter=neg.crash --no-fork
# Infinite loop becomes a reported timeout via the per-test alarm.
expect_fail_msg "infinite loop times out"     "timed out" "$NEG" --filter=neg.timeout --timeout=1
# abort() (SIGABRT, uncaught) is contained by fork: parent synthesizes from wait status.
expect_fail_msg "SIGABRT contained by fork"   "SIGABRT" "$NEG" --filter=neg.aborts

# ---- SKIP ----
expect_contains "SKIP reported"    "# SKIP" "$POS" --filter=posit.skip_reports
expect_exit     "SKIP exits 0"     0        "$POS" --filter=posit.skip_reports

# ---- SETUP / TEARDOWN ----
expect_exit "SETUP runs before test" 0 "$POS" --filter=fix.sees_setup

# ---- runner CLI behavior ----

# --list count equals the run plan "1..N".
plan=$("$POS" 2>/dev/null | sed -n 's/^1\.\.\([0-9][0-9]*\)$/\1/p')
listn=$("$POS" --list 2>/dev/null | grep -c .)
if [ -n "$plan" ] && [ "$plan" = "$listn" ]; then
    ok "--list count matches run plan ($plan)"
else
    bad "--list count matches run plan" "plan=$plan list=$listn"
fi

# --filter selects exactly one test.
seln=$("$POS" --filter=posit.eq 2>/dev/null | grep -c '^ok')
if [ "$seln" = 1 ]; then ok "--filter selects one"; else bad "--filter selects one" "matched $seln"; fi

# --shuffle --seed is deterministic: same seed -> same order.
ord1=$("$POS" --shuffle --seed=123 2>/dev/null | grep '^ok')
ord2=$("$POS" --shuffle --seed=123 2>/dev/null | grep '^ok')
if [ "$ord1" = "$ord2" ]; then ok "--shuffle --seed deterministic"; else bad "--shuffle --seed deterministic"; fi

# --jobs=N runs concurrently but keeps output in registration order: the TAP
# body (ok/not ok lines) must be byte-identical to a serial run.
serial=$("$POS" 2>/dev/null | grep '^ok')
par=$("$POS" -j4 2>/dev/null | grep '^ok')
if [ "$serial" = "$par" ]; then ok "-j4 output matches serial order"; else bad "-j4 output matches serial order"; fi
# A parallel run still surfaces failures (nonzero exit) with the right diagnostic.
expect_fail_msg "-j4 still reports failures" "ASSERT_EQ" "$NEG" --jobs=4
# A crash in one child doesn't sink the run: it's reported and the run completes.
expect_fail_msg "-j4 contains a crash" "fatal signal" "$NEG" -j4
# Invalid job counts are rejected with the bad-CLI exit code.
expect_exit "--jobs=0 exits 2" 2 "$POS" --jobs=0

# --format=junit emits a testsuites root, and a <failure> for a failing case.
expect_contains "junit root element"  "<testsuites" "$POS" --format=junit
expect_contains "junit failure element" "<failure"   "$NEG" --filter=neg.eq --format=junit
if command -v xmllint >/dev/null 2>&1; then
    if "$POS" --format=junit 2>/dev/null | xmllint --noout - 2>/dev/null; then
        ok "junit is well-formed XML"
    else
        bad "junit is well-formed XML"
    fi
fi

# Unknown option exits 2; --help exits 0.
expect_exit "unknown option exits 2" 2 "$POS" --bogus-option
expect_exit "--help exits 0"         0 "$POS" --help

# ---- summary ----
total=$((pass + fail))
echo "# $pass passed, $fail failed, $total total"
[ "$fail" -eq 0 ]
