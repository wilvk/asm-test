#!/bin/sh
# fake_asmspy_host_perf.sh — stands in for an `asmspy --info <pid> --json` on a
# host where perf_event_open is refused FOR THE asmspy BINARY (2026-08-06 final
# review, finding 8). Identical to procinfo_full.asmtrace except for the `host`
# object, so a test can compare the two and isolate exactly that key.
cat "$(dirname "$0")/procinfo_host_perf.asmtrace"
