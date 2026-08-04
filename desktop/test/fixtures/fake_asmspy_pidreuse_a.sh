#!/bin/sh
# fake_asmspy_pidreuse_a.sh — a tiny, self-contained "process" (pid 100,
# start_ticks 1000) for the runner's cache-key test (gui-process-details Task
# 6): the cache must key on (pid, start_ticks), not pid alone, or a reused
# pid serves the previous process's card. This script and its "_b" twin
# stand in for the two processes on either side of that reuse.
printf '%s\n' '{"asmtrace":1,"provenance":{"exact":true},"arch":"x86_64"}'
printf '%s\n' '{"k":"procinfo","identity":{"pid":100,"start_ticks":1000}}'
