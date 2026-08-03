#!/bin/sh
# capture-shot-recordings.sh — capture the four recordings the documented 3D
# scene screenshots are rendered from (docs/guides/desktop-gui-scenes.md).
#
# FOUR recordings, not one, and this is structural: `call` events and `df_*`
# events come from different engines, --serve runs ONE engine at a time, and the
# desktop's Session::done_ is a vector of separate Recordings. No single capture
# can satisfy every scene gate. Checked against the corpus too: no golden
# recording carries `call` events at all, which is why the module-ribbon scene
# needs a live capture rather than a fixture.
#
# Needs to attach to a process it did not launch. scenes_victim calls
# PR_SET_PTRACER_ANY so this works under the Ubuntu default ptrace_scope=1; see
# docs/getting-started/host-setup.md if attach is denied anyway.
#
# These captures single-step a live process, so they take a couple of minutes
# and write tens of megabytes into build/shots/rec/.
set -eu

ASMSPY="${ASMSPY:-build/asmspy}"
VICTIM="${VICTIM:-build/scenes_victim}"
OUT="${OUT:-build/shots/rec}"
# How long each --continuous dataflow capture runs. `--continuous` re-arms the
# region and keeps capturing until interrupted, accumulating many invocations
# into one recording delimited by `df_invocation` markers. That length is what
# gives the divergence worldline and the Scrubber's ring something to page
# through: a single invocation of blend_tile is 11 steps at -O2.
#
# It does NOT widen the address plane, and it is worth being exact about why.
# The captured region is blend_tile, whose only memory operand is one 16-byte
# tile, so a capture of ANY duration observes exactly 1 distinct data address
# and 11 distinct code offsets — measured, at both 2s and 20s. Duration buys
# repetitions of the same addresses, not new ones. A rich address plane needs a
# region that touches varied memory, which is a different capture, not a longer
# one.
#
# 2s is therefore a deliberate ceiling rather than a minimum: at ~1000
# invocations/sec these recordings grow ~18 MB per second, and 20s produced
# 364 MB of almost entirely redundant steps.
#
# Interrupted with SIGINT, the documented stop signal, so the recording closes
# cleanly rather than being truncated by a kill.
DF_SECONDS="${DF_SECONDS:-2}"

[ -x "$ASMSPY" ] || { echo "capture: $ASMSPY not built (make cli)" >&2; exit 1; }
[ -x "$VICTIM" ] || { echo "capture: $VICTIM not built" >&2; exit 1; }

mkdir -p "$OUT"

pids=""
cleanup() { for p in $pids; do kill -9 "$p" 2>/dev/null || true; done; }
trap cleanup EXIT INT TERM

say() { echo "capture: $*" >&2; }

# start_victim sets VICTIM_PID. It must NOT be called via `$(...)`: a command
# substitution runs in a subshell that INHERITS the EXIT trap above, so the trap
# fires the moment the substitution ends and kills the victim we just started —
# with `pids` already containing it. The capture then attaches to a corpse and
# asmspy honestly records zero events, which reads like a producer bug rather
# than a dead target. Setting a variable in the current shell avoids the subshell
# entirely. (Measured: this exact shape produced an events:0 tree recording.)
VICTIM_PID=""
start_victim() { # $1 = seed
    "$VICTIM" --seed "$1" >/dev/null 2>&1 &
    VICTIM_PID=$!
    pids="$pids $VICTIM_PID"
    sleep 1 # let the workers reach steady state
    alive "startup"
}

# The target must be alive BEFORE each attach. Without this the failure surfaces
# one step later as an empty recording, which points at the wrong component.
alive() { # $1 = what we are about to do
    kill -0 "$VICTIM_PID" 2>/dev/null || {
        echo "capture: victim $VICTIM_PID is not running (before $1)" >&2
        exit 1
    }
}

# A capture that produced nothing is a failure, not a quiet skip: an empty file
# would sail through to a screenshot showing an empty scene.
require_nonempty() { # $1 = path, $2 = what
    [ -s "$1" ] || { echo "capture: $1 is empty — $2 produced no events" >&2; exit 1; }
}

# A recording whose footer says events:0 is ALSO a failure. asmspy records a
# skip faithfully (header + `end`), so such a file is non-empty but useless —
# require_nonempty alone would wave it through.
require_events() { # $1 = path, $2 = what
    if grep -q '"events":0' "$1" 2>/dev/null; then
        echo "capture: $1 recorded ZERO events — $2 attached but observed nothing" >&2
        exit 1
    fi
}

# --- ModuleRibbon: the whole-process call tree across threads and modules -----
start_victim 1
v1=$VICTIM_PID
say "tree      <- pid $v1"
"$ASMSPY" --tree "$v1" 400 --record="$OUT/tree.asmtrace" >/dev/null 2>&1 || true
require_nonempty "$OUT/tree.asmtrace" "--tree"
require_events "$OUT/tree.asmtrace" "--tree"

# --- Invocation: N coverage events == N invocation slabs ---------------------
alive "--trace"
say "trace     <- pid $v1 blend_tile"
"$ASMSPY" --trace "$v1" blend_tile 12 --record="$OUT/trace-blend.asmtrace" \
    >/dev/null 2>&1 || true
require_nonempty "$OUT/trace-blend.asmtrace" "--trace"

# --- Plane / LanePrism / Divergence A ----------------------------------------
# --fpregs is here for the SCRUBBER's wide register deck. LanePrism does not
# need it: the dataflow producer reads XMM operands directly, which is measured,
# not assumed.
alive "--dataflow (seed 1)"
say "df-a      <- pid $v1 blend_tile (continuous, ${DF_SECONDS}s)"
timeout -s INT "$DF_SECONDS" \
    "$ASMSPY" --dataflow "$v1" blend_tile --steps --mem --fpregs --statediff \
    --continuous --record="$OUT/df-a.asmtrace" >/dev/null 2>&1 || true
require_nonempty "$OUT/df-a.asmtrace" "--dataflow (seed 1)"
kill -9 "$v1" 2>/dev/null || true

# --- Divergence B: same code, different DATA ---------------------------------
# The seed changes input values only. Anything that changed the compiled bytes
# would give the two sides different code_shas, and the divergence scene would
# show a refusal card instead of a fork.
start_victim 2
v2=$VICTIM_PID
say "df-b      <- pid $v2 blend_tile (continuous, ${DF_SECONDS}s, seed 2)"
timeout -s INT "$DF_SECONDS" \
    "$ASMSPY" --dataflow "$v2" blend_tile --steps --mem --fpregs --statediff \
    --continuous --record="$OUT/df-b.asmtrace" >/dev/null 2>&1 || true
require_nonempty "$OUT/df-b.asmtrace" "--dataflow (seed 2)"
kill -9 "$v2" 2>/dev/null || true

say "wrote $OUT/{tree,trace-blend,df-a,df-b}.asmtrace"
