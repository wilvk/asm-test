#!/bin/sh
# fake_asmspy_info.sh — stands in for `asmspy --info <pid> --json` so the
# runner's fork/exec/read/reap path is tested with no tracer and no live
# target. Echoes the checked-in full fixture regardless of its arguments.
cat "$(dirname "$0")/procinfo_full.asmtrace"
