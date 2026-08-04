#!/bin/sh
# fake_asmspy_mismatch.sh — always reports pid 42 regardless of the --info
# argument, for the runner's pid-mismatch-guard test (gui-process-details
# Task 6): a snapshot whose pid disagrees with the pid actually probed must
# be discarded, never rendered under the wrong process's card.
printf '%s\n' '{"asmtrace":1,"provenance":{"exact":true},"arch":"x86_64"}'
printf '%s\n' '{"k":"procinfo","identity":{"pid":42,"start_ticks":1}}'
