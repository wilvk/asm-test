#!/bin/sh
# fake_asmspy_hang.sh — ignores its --info/--json arguments and genuinely
# sleeps, for the runner's switch-kills-the-in-flight-child test
# (gui-process-details Task 6). Invoking /bin/sleep DIRECTLY with the
# runner's fixed argv ("--info <pid> --json") does NOT hang: GNU coreutils
# rejects "--info" as an unrecognized option and exits in about a
# millisecond — too fast to reliably tell "killed by the switch" apart from
# "exited on its own" in a test with no real delay between ticks. This
# script sleeps regardless of its arguments, so it stays alive until
# something actually kills it.
exec sleep 5
