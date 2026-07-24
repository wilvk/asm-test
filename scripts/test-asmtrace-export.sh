#!/bin/sh
# test-asmtrace-export.sh — the asmtrace_export golden suite
# (docs/internal/gui/02-exporters-and-readers.md T4).
#
# Every mode of the exporter is pinned BYTE-FOR-BYTE against a committed
# expected file, and every honest refusal is pinned by exit code AND by a grep
# for the reason it must name. Byte-exactness is the point: speedscope and
# Chrome Trace are stable public formats, so any change to what we emit shows up
# here as a reviewable diff rather than as a viewer that quietly renders
# something else.
#
# The fixtures under tests/golden-asmtrace/export/ are HAND-AUTHORED and never
# regenerated (that directory's README says so) — `make asmtrace-golden` writes
# only flat *.asmtrace files into the parent and never descends here.
#
# Needs cc + python3 only, so it runs on every lane with no self-skip.
#   UPDATE_GOLDEN=1 sh scripts/test-asmtrace-export.sh   # rewrite expected files
set -eu

BUILD=${BUILD:-build}
EXPORT=$BUILD/asmtrace_export
FIX=tests/golden-asmtrace/export
UPDATE_GOLDEN=${UPDATE_GOLDEN:-0}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

fails=0
ok()   { printf 'ok   %s\n' "$1"; }
bad()  { printf 'FAIL %s\n' "$1"; fails=$((fails + 1)); }

# run MODE FIXTURE EXPECTED [EXTRA-ARGS...] — byte-compare one mode's output.
run() {
    mode=$1; fixture=$2; expected=$3; shift 3
    out=$TMP/$(basename "$expected")
    if ! "$EXPORT" "$mode" "$@" "$FIX/$fixture.asmtrace" >"$out" 2>"$TMP/err"; then
        bad "$fixture $mode: exporter failed (exit $?)"
        sed 's/^/     /' "$TMP/err"
        return
    fi
    case $expected in
    *.json)
        if ! python3 -m json.tool <"$out" >/dev/null 2>&1; then
            bad "$fixture $mode: output is not valid JSON"
            return
        fi
        ;;
    esac
    if [ "$UPDATE_GOLDEN" = 1 ]; then
        cp "$out" "$FIX/$expected"
        ok "$fixture $mode -> $expected (regenerated)"
        return
    fi
    if cmp -s "$out" "$FIX/$expected"; then
        ok "$fixture $mode == $expected"
    else
        bad "$fixture $mode differs from $expected"
        diff -u "$FIX/$expected" "$out" | head -20 | sed 's/^/     /'
    fi
}

# refuse FIXTURE MODE PATTERN — assert exit 2 AND that the reason names PATTERN.
# An exit code alone is not enough: a refusal whose reason a user cannot read is
# indistinguishable from a crash.
refuse() {
    fixture=$1; mode=$2; pattern=$3
    set +e
    "$EXPORT" "$mode" "$FIX/$fixture.asmtrace" >"$TMP/out" 2>"$TMP/err"
    rc=$?
    set -e
    if [ "$rc" -ne 2 ]; then
        bad "$fixture $mode: expected exit 2 (honest refusal), got $rc"
        return
    fi
    if grep -q "$pattern" "$TMP/err"; then
        ok "$fixture $mode refused (exit 2, reason names '$pattern')"
    else
        bad "$fixture $mode: exit 2 but the reason never names '$pattern'"
        sed 's/^/     /' "$TMP/err"
    fi
}

# grep_out FIXTURE MODE PATTERN LABEL — assert the OUTPUT carries PATTERN.
grep_out() {
    fixture=$1; mode=$2; pattern=$3; label=$4
    if "$EXPORT" "$mode" "$FIX/$fixture.asmtrace" 2>/dev/null |
        grep -q -- "$pattern"; then
        ok "$fixture $mode: $label"
    else
        bad "$fixture $mode: $label — '$pattern' not in the output"
    fi
}

[ -x "$EXPORT" ] || { echo "no $EXPORT — run: make asmtrace-export"; exit 1; }

# --- the four modes, byte-pinned ------------------------------------------
run --speedscope tree-small     tree-small.speedscope.json
run --chrome     tree-small     tree-small.chrome.json
run --dot-tree   tree-small     tree-small.dot
run --chrome     trace-heat     trace-heat.chrome.json
run --lcov       trace-heat     trace-heat.info            --name=corpus
run --speedscope tree-truncated tree-truncated.speedscope.json
run --dot-tree   tree-truncated tree-truncated.dot
run --speedscope unknown-kind   unknown-kind.speedscope.json
run --lcov       trace-truncated trace-truncated.info       --name=work

# --- honesty: truncation and drops always surface (D7) ---------------------
grep_out tree-truncated --speedscope ' (truncated)' \
    'each profile name carries the truncation suffix'
grep_out tree-truncated --chrome '"truncated": true' \
    'otherData reports the truncation'
grep_out tree-truncated --chrome '"lost": 3' \
    'otherData reports the lost samples'
grep_out tree-truncated --dot-tree '# truncated recording' \
    'the DOT export carries the truncation trailer'
# lcov has no comment syntax, so its honesty channel is stderr: the WARNING must
# be there and the record on stdout must stay pristine for genhtml.
if "$EXPORT" --lcov "$FIX/trace-truncated.asmtrace" 2>&1 >/dev/null |
    grep -q 'LOWER BOUND'; then
    ok "trace-truncated --lcov: warns on stderr that coverage is a lower bound"
else
    bad "trace-truncated --lcov: no stderr warning about the truncation"
fi
if "$EXPORT" --lcov "$FIX/trace-truncated.asmtrace" 2>/dev/null |
    grep -q '^#'; then
    bad "trace-truncated --lcov: a comment leaked into the lcov record"
else
    ok "trace-truncated --lcov: the record itself stays pristine"
fi

# --- honesty: the ordinal axis is never labelled as time -------------------
grep_out trace-heat --chrome 'event ordinal' \
    'otherData.ts_unit says the axis is an event ordinal, not time'
grep_out tree-small --speedscope '"unit": "none"' \
    'speedscope profiles declare unit=none (no timestamps were measured)'

# --- honesty: survey events are never stacks, in any mode ------------------
for mode in --speedscope --chrome --dot-tree; do
    refuse survey-only "$mode" 'edges are not stacks'
done

# --- refusals by name ------------------------------------------------------
refuse future-major    --speedscope 'major 1 only'
refuse zstd-container  --speedscope 'compressed container'
refuse mixed-basis     --lcov       'mixes address bases'

# --- the heat cap is announced, never silent -------------------------------
if "$EXPORT" --chrome --heat-cap=2 "$FIX/trace-heat.asmtrace" 2>/dev/null |
    grep -q '"heat_offsets_dropped": 4'; then
    ok "trace-heat --chrome --heat-cap=2: the dropped offsets are counted"
else
    bad "trace-heat --chrome --heat-cap=2: dropped offsets not reported"
fi

# --- lcov's own invariant: one DA: line per counted line --------------------
"$EXPORT" --lcov --name=corpus "$FIX/trace-heat.asmtrace" >"$TMP/lcov" 2>/dev/null
da=$(grep -c '^DA:' "$TMP/lcov")
lf=$(sed -n 's/^LF://p' "$TMP/lcov")
if [ "$da" = "$lf" ]; then
    ok "trace-heat --lcov: DA: line count ($da) == LF: ($lf)"
else
    bad "trace-heat --lcov: $da DA: lines but LF:$lf"
fi

# --- --out=FILE writes the same bytes as stdout ----------------------------
"$EXPORT" --speedscope --out="$TMP/viaout.json" "$FIX/tree-small.asmtrace"
if cmp -s "$TMP/viaout.json" "$FIX/tree-small.speedscope.json"; then
    ok "tree-small --speedscope --out=FILE matches the stdout bytes"
else
    bad "tree-small --speedscope --out=FILE differs from stdout"
fi

# --- the standing canary over 01's generated corpus (02 T4 step 5) ---------
# Not a self-skip of the suite: a bonus assertion that runs wherever the
# generated corpus is present (it is committed, so that is everywhere).
corpus=tests/golden-asmtrace/add_signed.asmtrace
if [ -f "$corpus" ]; then
    set +e
    "$EXPORT" --speedscope "$corpus" >/dev/null 2>&1
    rc=$?
    set -e
    # A dataflow-only recording carries no call events, so the honest answer is
    # the refusal — what is asserted is that the reader PARSED the real corpus
    # bytes rather than erroring on them (exit 1).
    if [ "$rc" = 2 ]; then
        ok "canary: the generated corpus parses (add_signed refuses as expected)"
    else
        bad "canary: --speedscope over $corpus exited $rc, expected 2"
    fi
fi

if [ "$fails" -ne 0 ]; then
    printf '\n%d asmtrace_export check(s) FAILED\n' "$fails"
    exit 1
fi
printf '\nasmtrace_export: all checks passed\n'
