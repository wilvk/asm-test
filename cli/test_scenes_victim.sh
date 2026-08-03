#!/bin/sh
# test_scenes_victim.sh — the sample process's SHAPE is the contract. Each check
# below maps to one 3D scene gate in desktop/src/ui/shell.cpp.
set -eu
BIN="${1:-build/scenes_victim}"

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$BIN" ] || fail "$BIN not built"

# It must announce pid + the routine address on stderr, so a capture script can
# read them without guessing.
out=$("$BIN" --selftest 2>&1) || fail "--selftest exited nonzero"
echo "$out" | grep -q "^scenes_victim pid=[0-9]* blend_tile=0x[0-9a-f]* seed=1$" \
    || fail "stderr banner missing or malformed: $out"

# Same seed must be deterministic; a different seed must diverge. This is what
# makes the Divergence scene possible at all.
a=$("$BIN" --selftest --seed 1 2>/dev/null)
b=$("$BIN" --selftest --seed 1 2>/dev/null)
c=$("$BIN" --selftest --seed 2 2>/dev/null)
[ "$a" = "$b" ] || fail "same seed produced different output (not deterministic)"
[ "$a" != "$c" ] || fail "seed 1 and seed 2 produced identical output (no divergence)"

# Every symbol the capture step names must be a real, resolvable ELF symbol.
for sym in blend_tile walk_heap sort_batch mix_math; do
    nm "$BIN" 2>/dev/null | grep -q " [Tt] $sym$" || fail "missing ELF symbol: $sym"
done

# blend_tile must actually contain SSE writes, or the LanePrism scene is empty.
objdump -d --disassemble="blend_tile" "$BIN" 2>/dev/null \
    | grep -qE "paddd|pshufd|punpck" \
    || fail "blend_tile contains no recognisable SSE lane ops"

echo "PASS: scenes_victim shape"
