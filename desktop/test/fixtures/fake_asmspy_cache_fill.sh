#!/bin/sh
# fake_asmspy_cache_fill.sh — pid fixed at 100; start_ticks comes from the
# FAKE_START_TICKS environment variable (defaulting to 1 if unset), so the
# runner's cache-eviction test (gui-process-details Task 6, round 3) can
# synthesize as many distinct (pid, start_ticks) cache keys as it needs
# without a fixture file per entry.
st="${FAKE_START_TICKS:-1}"
printf '%s\n' '{"asmtrace":1,"provenance":{"exact":true},"arch":"x86_64"}'
printf '%s\n' "{\"k\":\"procinfo\",\"identity\":{\"pid\":100,\"start_ticks\":$st}}"
