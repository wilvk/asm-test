#!/bin/sh
# fake_serve_threads.sh — a serve host that streams a TWO-THREAD abs PC trace, for
# the live-observer overlay (10-spacetime-3d-overview.md T5). It is the threads
# companion to fake_serve.sh (07-T3): same control protocol
# (docs/internal/gui/asmtrace-schema.md, "Serve protocol"), same "no tracer in it"
# property — so the incremental T3 feed and the convergence detector are testable
# on ANY machine, with no ptrace, no victim, and no dependence on thread timing.
#
# The one mode it answers ("threads") emits a `codeimage` region, then per-tid
# `trace` events in TWO batches split by a short sleep. The split is deliberate:
# a reader polling the pipe observes the GROWING recording after batch 1 and again,
# larger, after batch 2 — which is how a subprocess-driven test asserts per-tid
# trajectory GROWTH deterministically rather than by luck. Batch 2 has both tids
# touch one SHARED address (0x401200), so the completed stream carries exactly one
# cross-thread convergence.
set -eu

emit() { printf '%s\n' "$1"; }

HDR='{"asmtrace":1,"container":"ndjson","producer":{"name":"asmspy","version":"1.1.0"},"provenance":{"backend":"ptrace-dataflow","exact":true,"trust":"exact","redacted":false},"arch":"x86_64","pid":4242,"cmd":"./threads_victim"}'

while IFS= read -r line; do
    case "$line" in
    *'"cmd":"quit"'*)
        emit '{"k":"cmd","cmd":"quit"}'
        exit 0
        ;;
    *'"mode":"threads"'*)
        emit '{"k":"cmd","cmd":"start","mode":"threads"}'
        emit '{"k":"session","state":"started","mode":"threads","pid":4242,"params":{"follow":true,"max":-1}}'
        emit "$HDR"
        emit '{"k":"codeimage","base":4198400,"len":4096,"version":0}'
        # --- batch 1: distinct per-tid vertices (tid 7 and tid 9) ---------------
        # tid7: 0x401000, 0x401040, 0x401080   tid9: 0x401010, 0x401050
        emit '{"k":"trace","basis":"abs","off":4198400,"tid":7}'
        emit '{"k":"trace","basis":"abs","off":4198416,"tid":9}'
        emit '{"k":"trace","basis":"abs","off":4198464,"tid":7}'
        emit '{"k":"trace","basis":"abs","off":4198480,"tid":9}'
        emit '{"k":"trace","basis":"abs","off":4198528,"tid":7}'
        # Let a polling reader observe the recording mid-stream (growth milestone).
        sleep 0.2
        # --- batch 2: both tids touch the SHARED address 0x401200 --------------
        emit '{"k":"trace","basis":"abs","off":4198912,"tid":9}'
        emit '{"k":"trace","basis":"abs","off":4198912,"tid":7}'
        emit '{"k":"trace","basis":"abs","off":4198544,"tid":9}'
        emit '{"k":"trace","basis":"abs","off":4198592,"tid":7}'
        emit '{"k":"end","events":10,"truncated":false,"drops":{"lost":0,"throttled":false}}'
        emit '{"k":"session","state":"stopped","mode":"threads","events":10,"reason":"stop"}'
        ;;
    *)
        emit '{"k":"err","reason":"this fake host only serves the threads mode","cmd":"start"}'
        ;;
    esac
done
