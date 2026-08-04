#!/bin/sh
# fake_asmspy_pidreuse_b.sh — same pid (100) as fake_asmspy_pidreuse_a.sh but
# a DIFFERENT start_ticks (2000): a different, unrelated process that got the
# same pid number reassigned to it after the first one exited.
printf '%s\n' '{"asmtrace":1,"provenance":{"exact":true},"arch":"x86_64"}'
printf '%s\n' '{"k":"procinfo","identity":{"pid":100,"start_ticks":2000}}'
