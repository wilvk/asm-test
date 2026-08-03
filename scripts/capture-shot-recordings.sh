#!/bin/sh
# capture-shot-recordings.sh — capture the four recordings the documented 3D
# scene screenshots are rendered from (docs/guides/desktop-gui-scenes.md).
#
# WHY --serve AND NOT --record. The desktop's 3D pane is present only when a
# recording carries `codeimage` regions (view_presence.cpp gates Scene3D on
# regions_from_codeimage). `codeimage` is emitted ONLY inside a serve session —
# the headless `asmspy --dataflow ... --record=` path never emits it. Captured
# with --record, all nine 3D shots came out as identical blank frames, because
# the 3D tab was simply absent. This is measured, not inferred.
#
# FOUR recordings, not one, and that is structural: `call` events and `df_*`
# events come from different engines and a serve session runs ONE engine at a
# time. No single capture can satisfy every scene gate.
#
# Needs to attach to a process it did not launch. scenes_victim calls
# PR_SET_PTRACER_ANY so this works under the Ubuntu default ptrace_scope=1; see
# docs/getting-started/host-setup.md if attach is denied anyway.
#
# These captures single-step a live process, so they take a couple of minutes.
set -eu

ASMSPY="${ASMSPY:-build/asmspy}"
VICTIM="${VICTIM:-build/scenes_victim}"
OUT="${OUT:-build/shots/rec}"
# How long each dataflow session runs before a clean stop. `continuous` re-arms
# the region and keeps capturing, accumulating many invocations into one
# recording delimited by `df_invocation` markers, which is what gives the
# divergence worldline and the Scrubber's ring something to page through.
#
# It is a CEILING, not a minimum. blend_tile's only memory operand is one
# 16-byte tile, so a capture of ANY duration observes the same handful of
# addresses — duration buys repetitions, not coverage. Measured at 20s it
# produced 364 MB of almost entirely redundant steps.
DF_SECONDS="${DF_SECONDS:-2}"
TREE_CALLS="${TREE_CALLS:-400}"
TRACE_INVOCATIONS="${TRACE_INVOCATIONS:-12}"

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
# with `pids` already containing it. asmspy then attaches to a corpse and
# faithfully records zero events, which reads like a producer bug rather than a
# dead target. Setting a variable in the current shell avoids the subshell.
# (Measured: this exact shape produced an events:0 tree recording.)
VICTIM_PID=""
start_victim() { # $1 = seed
    "$VICTIM" --seed "$1" >/dev/null 2>&1 &
    VICTIM_PID=$!
    pids="$pids $VICTIM_PID"
    sleep 1 # let the workers reach steady state
    alive "startup"
}

alive() { # $1 = what we are about to do
    kill -0 "$VICTIM_PID" 2>/dev/null || {
        echo "capture: victim $VICTIM_PID is not running (before $1)" >&2
        exit 1
    }
}

# Run one serve session and keep only the RECORDING out of its stream.
#
# A serve stream wraps the recording in protocol chatter — `cmd`, `session` and
# any `err` sit OUTSIDE the [header … end] range (asmtrace-schema.md). Line 1 is
# therefore not the header, and a reader that assumes it is refuses the file with
# 'header has no integer "asmtrace" major'. Slice from the header to the footer.
serve_capture() { # $1 = out file, $2 = seconds, $3 = start-command JSON
    out=$1; secs=$2; cmd=$3
    {
        printf '%s\n' "$cmd"
        sleep "$secs"
        printf '{"cmd":"stop"}\n'
        sleep 1
        printf '{"cmd":"quit"}\n'
    } | timeout $((secs + 40)) "$ASMSPY" --serve 2>/dev/null \
      | awk '/"asmtrace":1/{r=1} r{print} r&&/"k":"end"/{exit}' > "$out"
}

# A capture that produced nothing is a failure, not a quiet skip: an empty file
# would sail through to a screenshot showing an empty scene.
require_nonempty() { # $1 = path, $2 = what
    [ -s "$1" ] || { echo "capture: $1 is empty — $2 produced no events" >&2; exit 1; }
}

# A recording whose footer says events:0 is ALSO a failure. asmspy records a skip
# faithfully (header + `end`), so such a file is non-empty but useless.
require_events() { # $1 = path, $2 = what
    if grep -q '"events":0' "$1" 2>/dev/null; then
        echo "capture: $1 recorded ZERO events — $2 attached but observed nothing" >&2
        exit 1
    fi
}

# The serve `dataflow`/`trace` modes take base+len, not a symbol name.
sym_base=""; sym_len=""
resolve_sym() { # $1 = symbol
    sym_base=$("$ASMSPY" --syms "$VICTIM_PID" "$1" 2>/dev/null \
               | awk -v s="$1" '$3==s{print $1; exit}')
    sym_len=$("$ASMSPY" --syms "$VICTIM_PID" "$1" 2>/dev/null \
              | awk -v s="$1" '$3==s{print $2; exit}')
    [ -n "$sym_base" ] && [ -n "$sym_len" ] \
        || { echo "capture: could not resolve $1 in pid $VICTIM_PID" >&2; exit 1; }
    sym_base=$(printf '%d' "$sym_base")
}

# --- ModuleRibbon: the whole-process call tree across threads and modules -----
start_victim 1
v1=$VICTIM_PID
say "tree      <- pid $v1"
serve_capture "$OUT/tree.asmtrace" 8 \
    "$(printf '{"cmd":"start","mode":"tree","pid":%s,"max":%s}' "$v1" "$TREE_CALLS")"
require_nonempty "$OUT/tree.asmtrace" "serve tree"
require_events "$OUT/tree.asmtrace" "serve tree"

# --- Invocation: N coverage events == N invocation slabs ---------------------
alive "serve trace"
resolve_sym blend_tile
say "trace     <- pid $v1 blend_tile @ $sym_base+$sym_len"
serve_capture "$OUT/trace-blend.asmtrace" 8 \
    "$(printf '{"cmd":"start","mode":"trace","pid":%s,"base":%s,"len":%s,"max":%s}' \
       "$v1" "$sym_base" "$sym_len" "$TRACE_INVOCATIONS")"
require_nonempty "$OUT/trace-blend.asmtrace" "serve trace"
require_events "$OUT/trace-blend.asmtrace" "serve trace"

# --- Plane / LanePrism / Divergence A ----------------------------------------
# fpregs is here for the SCRUBBER's wide register deck. The lane prism does not
# need it: the dataflow producer reads XMM operands directly, which is measured.
alive "serve dataflow (seed 1)"
say "df-a      <- pid $v1 blend_tile (continuous, ${DF_SECONDS}s)"
serve_capture "$OUT/df-a.asmtrace" "$DF_SECONDS" \
    "$(printf '{"cmd":"start","mode":"dataflow","pid":%s,"base":%s,"len":%s,"max":0,"steps":true,"mem":true,"fpregs":true,"statediff":true,"continuous":true}' \
       "$v1" "$sym_base" "$sym_len")"
require_nonempty "$OUT/df-a.asmtrace" "serve dataflow (seed 1)"
require_events "$OUT/df-a.asmtrace" "serve dataflow (seed 1)"
kill -9 "$v1" 2>/dev/null || true

# --- Divergence B: same code, different DATA ---------------------------------
# The seed changes input values only. Anything that changed the compiled bytes
# would give the two sides different code_shas, and the divergence scene would
# show a refusal card instead of a fork.
start_victim 2
v2=$VICTIM_PID
resolve_sym blend_tile
say "df-b      <- pid $v2 blend_tile (continuous, ${DF_SECONDS}s, seed 2)"
serve_capture "$OUT/df-b.asmtrace" "$DF_SECONDS" \
    "$(printf '{"cmd":"start","mode":"dataflow","pid":%s,"base":%s,"len":%s,"max":0,"steps":true,"mem":true,"fpregs":true,"statediff":true,"continuous":true}' \
       "$v2" "$sym_base" "$sym_len")"
require_nonempty "$OUT/df-b.asmtrace" "serve dataflow (seed 2)"
require_events "$OUT/df-b.asmtrace" "serve dataflow (seed 2)"
kill -9 "$v2" 2>/dev/null || true

say "wrote $OUT/{tree,trace-blend,df-a,df-b}.asmtrace"
