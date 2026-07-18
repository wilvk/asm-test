#!/bin/sh
#
# tests/expect.sh — meta-test harness for the asm-test framework (Track A of
# docs/internal/plans/expansion-plan.md).
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

# expect_fail_re DESC ERE CMD [args...] : nonzero exit AND output matches the
# extended regex. Used for crash-containment checks, where the diagnostic text
# differs by build: a normal build reports the framework's "fatal signal" (or,
# for a forked child, "exited abnormally"), while a sanitizer build has ASan
# intercept the fault and print its own "AddressSanitizer"/"SEGV" report.
expect_fail_re() {
    desc=$1
    re=$2
    shift 2
    run "$@"
    if [ "$rc" -eq 0 ]; then
        bad "$desc" "expected nonzero exit, got 0"
    elif printf '%s\n' "$out" | grep -Eq "$re"; then
        ok "$desc"
    else
        bad "$desc" "nonzero exit OK but output matched none of: $re"
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
# SIGSEGV is contained and reported (both with and without fork). The diagnostic
# text differs by build, so accept any of them:
#   - a normal build: the framework's own "fatal signal" (in-process handler) or,
#     for a forked child, the parent-synthesized "exited abnormally";
#   - a sanitizer build: the write through a null pointer is undefined behavior,
#     which UBSan flags first as a "runtime error" (gcc on Linux), before it can
#     become a hardware fault that ASan would report as "AddressSanitizer"/"SEGV"
#     (some clang builds). The fork case still shows "exited abnormally" because
#     the parent synthesizes it from the child's wait status regardless.
CRASH_RE='fatal signal|exited abnormally|AddressSanitizer|SEGV|runtime error'
expect_fail_re "SIGSEGV contained (fork)"    "$CRASH_RE" "$NEG" --filter=neg.crash
expect_fail_re "SIGSEGV contained (no-fork)" "$CRASH_RE" "$NEG" --filter=neg.crash --no-fork
# Infinite loop becomes a reported timeout via the per-test alarm.
expect_fail_msg "infinite loop times out"     "timed out" "$NEG" --filter=neg.timeout --timeout=1
# abort() (SIGABRT, uncaught) is contained by fork: parent synthesizes from wait status.
expect_fail_msg "SIGABRT contained by fork"   "SIGABRT" "$NEG" --filter=neg.aborts
# SIGILL / SIGFPE / SIGBUS are contained and named (raise() delivers them the same
# on every arch; the sanitize build sets handle_sig*=0 so the framework still catches).
expect_fail_re  "SIGILL contained"  'SIGILL' "$NEG" --filter=neg.illegal_instruction
expect_fail_re  "SIGFPE contained"  'SIGFPE' "$NEG" --filter=neg.fp_exception
expect_fail_re  "SIGBUS contained"  'SIGBUS' "$NEG" --filter=neg.bus_error
# Guard-page overrun/underrun faults — the headline buffer-safety feature, no
# longer exercised only in the exit-code-ignored demo.
expect_fail_re  "guard-page overrun faults"  "$CRASH_RE" "$NEG" --filter=neg.guard_page_overrun
expect_fail_re  "guard-page underrun faults" "$CRASH_RE" "$NEG" --filter=neg.guard_page_underrun
# Differential testing reports the first disagreeing input...
expect_fail_msg "ref-model mismatch reported" "trial 0 input" "$NEG" --filter=neg.ref_model_mismatch
# ...shrunk to boundary values (buggy_sum diverges for any b != 0, so the greedy
# shrink lands exactly on [0, 1] regardless of the random draw)...
expect_fail_msg "ref-model mismatch shrinks input" "shrinks to [0, 1]" "$NEG" --filter=neg.ref_model_mismatch
# ...and the FP engine reports a ULP-judged mismatch.
expect_fail_msg "FP ref-model mismatch reported" "ASSERT_MATCHES_FREF2" "$NEG" --filter=neg.fref_model_mismatch

# ---- SKIP ----
expect_contains "SKIP reported"    "# SKIP" "$POS" --filter=posit.skip_reports
expect_exit     "SKIP exits 0"     0        "$POS" --filter=posit.skip_reports

# ---- SETUP / TEARDOWN ----
expect_exit "SETUP runs before test" 0 "$POS" --filter=fix.sees_setup
# TEARDOWN actually runs (X1): the whole positive suite passes --no-fork, where
# the lifecycle counters share state and the second test asserts the first test's
# TEARDOWN executed. A skipped teardown makes lifecycle.observes_prior_teardown
# fail -> nonzero exit.
expect_exit "TEARDOWN runs (observed --no-fork)" 0 "$POS" --no-fork

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
# ...and it actually reorders: seed 123's permutation of the current suite differs
# from registration (serial) order, so a no-op --shuffle would fail here. The seed
# is fixed, so this is deterministic, not flaky. (If the suite ever shrinks so this
# seed maps to the identity, pick another differing seed via the loop in the docs.)
serial_ord=$("$POS" 2>/dev/null | grep '^ok')
if [ "$ord1" != "$serial_ord" ]; then ok "--shuffle reorders vs serial (seed=123)"; else bad "--shuffle reorders vs serial (seed=123)" "shuffled order == serial order"; fi

# --jobs=N runs concurrently but keeps output in registration order: the TAP
# body (ok/not ok lines) must be byte-identical to a serial run.
serial=$("$POS" 2>/dev/null | grep '^ok')
par=$("$POS" -j4 2>/dev/null | grep '^ok')
if [ "$serial" = "$par" ]; then ok "-j4 output matches serial order"; else bad "-j4 output matches serial order"; fi
# A parallel run still surfaces failures (nonzero exit) with the right diagnostic.
# These are the only unfiltered negative-suite runs, so they include neg.timeout's
# infinite spin loop: --timeout=1 bounds it (and overrides any ASMTEST_TIMEOUT=0 in
# the environment) so the check can't waste ~10s each or hang forever.
expect_fail_msg "-j4 still reports failures" "ASSERT_EQ" "$NEG" --jobs=4 --timeout=1
# A crash in one child doesn't sink the run: it's reported and the run completes.
expect_fail_re "-j4 contains a crash" "$CRASH_RE" "$NEG" -j4 --timeout=1
# Invalid job counts are rejected with the bad-CLI exit code.
expect_exit "--jobs=0 exits 2" 2 "$POS" --jobs=0

# A poll() failure degrades to blocking reaps: same results, no false greens.
par_pf=$(ASMTEST_DEBUG_FAIL_POLL=1 "$POS" -j4 2>/dev/null | grep '^ok')
if [ "$par_pf" = "$serial" ]; then ok "-j4 survives poll failure (degraded reaps)"; else bad "-j4 survives poll failure (degraded reaps)"; fi
expect_contains "poll-failure warning printed" "falling back to blocking reaps" env ASMTEST_DEBUG_FAIL_POLL=1 "$POS" -j4
expect_fail_msg "-j4 still fails honestly under poll failure" "ASSERT_EQ" env ASMTEST_DEBUG_FAIL_POLL=1 "$NEG" --jobs=4 --timeout=1

# --format=junit emits a testsuites root, and a <failure> for a failing case.
expect_contains "junit root element"  "<testsuites" "$POS" --format=junit
expect_contains "junit failure element" "<failure"   "$NEG" --filter=neg.eq --format=junit
# The JUnit escaper (X4): validate the XML is well-formed for BOTH the all-passing
# suite AND a failing suite whose assertion message is full of `< & > "`, and that
# those metacharacters are actually escaped. CI installs libxml2-utils so this
# runs there; it self-skips only where xmllint is absent locally.
if command -v xmllint >/dev/null 2>&1; then
    if "$POS" --format=junit 2>/dev/null | xmllint --noout - 2>/dev/null; then
        ok "junit is well-formed XML (passing suite)"
    else
        bad "junit is well-formed XML (passing suite)"
    fi
    if "$NEG" --format=junit 2>/dev/null | xmllint --noout - 2>/dev/null; then
        ok "junit is well-formed XML (failures + XML metacharacters)"
    else
        bad "junit is well-formed XML (failures + XML metacharacters)"
    fi
    if "$NEG" --filter=neg.xml_special_chars --format=junit 2>/dev/null \
         | grep -q 'a&lt;b&amp;c&gt;d'; then
        ok "junit escapes < & > in messages"
    else
        bad "junit escapes < & > in messages"
    fi
else
    echo "  - SKIP junit XML validation (xmllint not installed)"
fi

# ---- --fail-fast / --repeat / --shard ----

# --fail-fast stops at the first failure: the negative suite's first test fails,
# so exactly one result line runs, the trailing plan is 1..1, and exit is nonzero.
ff=$("$NEG" --fail-fast --timeout=1 2>/dev/null)
ffrc=$?
ffn=$(printf '%s\n' "$ff" | grep -cE '^(ok|not ok)')
ffplan=$(printf '%s\n' "$ff" | grep -c '^1\.\.1$')
if [ "$ffrc" -ne 0 ] && [ "$ffn" = 1 ] && [ "$ffplan" = 1 ]; then
    ok "--fail-fast stops after the first failure (trailing 1..1)"
else
    bad "--fail-fast stops after the first failure (trailing 1..1)" "rc=$ffrc results=$ffn plan1=$ffplan"
fi
# ...and a fully-passing run under --fail-fast still runs everything and exits 0.
expect_exit "--fail-fast passes a green suite" 0 "$POS" --fail-fast

# --repeat=N runs the selection N times.
rep=$("$POS" --filter=posit.eq --repeat=3 2>/dev/null | grep -c '^ok')
if [ "$rep" = 3 ]; then ok "--repeat=3 runs the selection 3x"; else bad "--repeat=3 runs the selection 3x" "ran $rep"; fi

# --shard=K/N partitions: shards 1/2 and 2/2 are disjoint and their union is
# exactly the full --list.
all=$("$POS" --list 2>/dev/null | sort)
merged=$({ "$POS" --list --shard=1/2 && "$POS" --list --shard=2/2; } 2>/dev/null | sort)
if [ -n "$all" ] && [ "$all" = "$merged" ]; then
    ok "--shard 1/2 + 2/2 partition the selection"
else
    bad "--shard 1/2 + 2/2 partition the selection"
fi
# An out-of-range shard is a CLI error.
expect_exit "--shard=3/2 exits 2" 2 "$POS" --shard=3/2
expect_exit "--repeat=0 exits 2"  2 "$POS" --repeat=0

# Unknown option exits 2; --help exits 0.
expect_exit "unknown option exits 2" 2 "$POS" --bogus-option
expect_exit "--help exits 0"         0 "$POS" --help

# ---- summary ----
total=$((pass + fail))
echo "# $pass passed, $fail failed, $total total"
[ "$fail" -eq 0 ]
