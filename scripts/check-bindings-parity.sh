#!/usr/bin/env bash
#
# check-bindings-parity.sh — keep the language bindings in lock-step with the
# native-trace tier contract.
#
# WHY: the bindings are hand-written FFI wrappers. Struct LAYOUT is already
# pinned (the _Static_asserts in the headers + the generated asmtest_abi.json the
# bindings read), and BEHAVIOUR is pinned (the conformance corpus every binding
# replays). The one dimension nothing guarded was the FUNCTION SURFACE: a new C
# entry point could be wired into nine bindings and silently missed in the tenth.
#
# WHAT: this gate enforces that every binding wraps every symbol declared in the
# native-trace tier headers — the surface that is uniformly mirrored across all
# ten bindings (hwtrace is 11/11, drtrace 16/16 today) and the one most often
# grown "across all bindings" in a single commit. It deliberately does NOT police
# the core/emu surface, which the bindings consume through two different ABI
# styles (struct-mirror vs. opaque-handle FFI shim) and so is heterogeneous by
# design — a flat parity rule there would be all noise.
#
# The surface is derived from the headers, so adding a function to either tier
# header automatically requires it in every binding. Intentional omissions live
# in scripts/bindings-parity-allow.txt; stale exemptions fail the gate too.
#
# Usage:
#   scripts/check-bindings-parity.sh            # gate: exit nonzero on drift
#   scripts/check-bindings-parity.sh --report   # print the symbol x binding matrix
#
# A binding "wraps" a symbol if its name appears (whole-word) anywhere in that
# binding's tracked sources. This is a tripwire, not a proof — it catches the
# totally-missing case, which is the failure mode that actually happens.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

# Overridable so the gate can be widened later (e.g. add a tier header) without
# touching the logic.
TIER_HEADERS=${TIER_HEADERS:-"include/asmtest_hwtrace.h include/asmtest_drtrace.h include/asmtest_trace_auto.h include/asmtest_ptrace.h"}
BINDINGS=${BINDINGS:-"python cpp rust zig node java dotnet ruby lua go"}
ALLOW=${ALLOW:-scripts/bindings-parity-allow.txt}

mode=gate
case "${1:-}" in
    --report) mode=report ;;
    "") ;;
    *) echo "usage: $0 [--report]" >&2; exit 2 ;;
esac

# Tier surface: function names (an identifier immediately before '(') declared in
# the tier headers, case-sensitive so UPPERCASE macros drop out, minus *_t type
# names that appear in prose/casts.
surface=$(grep -rhoE '\b(asmtest_|emu_)[a-z0-9_]+[[:space:]]*\(' $TIER_HEADERS \
    | sed -E 's/[[:space:]]*\($//' | grep -vE '_t$' | sort -u)

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
allow_norm="$work/allow"   # normalized "<lang> <symbol>" exemptions
allow_used="$work/used"    # exemptions actually relied on this run
: >"$allow_norm"; : >"$allow_used"
if [ -f "$ALLOW" ]; then
    sed -E 's/#.*//' "$ALLOW" | awk 'NF>=2 {print $1" "$2}' | sort -u >"$allow_norm"
fi

allowed() { # lang symbol -> 0 if exempt (records the exemption used)
    if grep -qxF "$1 $2" "$allow_norm"; then echo "$1 $2" >>"$allow_used"; return 0; fi
    if grep -qxF "ALL $2" "$allow_norm"; then echo "ALL $2" >>"$allow_used"; return 0; fi
    return 1
}

violations="$work/viol"; : >"$violations"

[ "$mode" = report ] && { printf '%-34s' "symbol"; for l in $BINDINGS; do
    printf '%-5s' "$(printf '%s' "$l" | cut -c1-4)"; done; printf '\n'; }

for sym in $surface; do
    [ "$mode" = report ] && printf '%-34s' "$sym"
    for lang in $BINDINGS; do
        if git grep -qwF "$sym" -- "bindings/$lang" 2>/dev/null; then
            mark=Y
        elif allowed "$lang" "$sym"; then
            mark=-
        else
            mark=X
            echo "$lang $sym" >>"$violations"
        fi
        [ "$mode" = report ] && printf '%-5s' "$mark"
    done
    [ "$mode" = report ] && printf '\n'
done

[ "$mode" = report ] && exit 0

# Stale exemptions: an allow entry that was never needed this run (the binding
# already wraps the symbol, or the symbol is no longer in the tier headers).
stale="$work/stale"; : >"$stale"
sort -u "$allow_used" -o "$allow_used"
while IFS= read -r line; do
    [ -z "$line" ] && continue
    grep -qxF "$line" "$allow_used" || echo "$line" >>"$stale"
done <"$allow_norm"

fail=0
if [ -s "$violations" ]; then
    fail=1
    echo "bindings-parity: missing tier symbols (binding -> symbol):" >&2
    sort "$violations" | sed 's/^/  /' >&2
    echo "  -> wrap each in the binding, or exempt it in $ALLOW with a reason." >&2
fi
if [ -s "$stale" ]; then
    fail=1
    echo "bindings-parity: stale exemptions in $ALLOW (no longer needed):" >&2
    sed 's/^/  /' "$stale" >&2
    echo "  -> remove these lines (the symbol is now wrapped, or left the headers)." >&2
fi

if [ "$fail" -eq 0 ]; then
    nsym=$(printf '%s\n' "$surface" | grep -c .)
    nbind=$(printf '%s\n' $BINDINGS | grep -c .)
    echo "bindings-parity: OK — $nsym tier symbols x $nbind bindings in sync."
fi
exit "$fail"
