#!/bin/sh
# fake_asmspy_vanished.sh — stands in for `asmspy --info <pid> --json`
# against a pid that no longer exists: the real binary writes its "no such
# process" line to STDERR (which the runner sends to /dev/null) and exits
# with ASMSPY_INFO_EXIT_NO_SUCH_PID — 3, a code of its own precisely so the
# runner can tell this routine race apart from a broken probe. Nothing on
# stdout, exactly like the real thing.
echo "no such process: $2" >&2
exit 3
