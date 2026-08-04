#!/bin/sh
# fake_asmspy_closes_stdout.sh — closes its stdout (the pipe write end), then
# keeps running for a while, for the round-3 review finding
# (gui-process-details Task 6): read() returning EOF means the child's copy
# of the pipe closed, NOT that the child exited. A drain-completion path
# that waits on this child without killing it first can block procinfo_tick
# — the frame loop — for as long as the child keeps running.
#
# `exec sleep 4` (not a bare `sleep 4`) is load-bearing: without exec, sh
# would FORK sleep as a separate grandchild and then wait on it, so
# SIGKILL-ing the runner's direct child (this script's own sh process)
# would leave that grandchild orphaned and still sleeping — a real leak this
# fixture would otherwise cause, not the runner. exec replaces this process
# with sleep in place (same pid), so one SIGKILL is enough.
exec 1>&-
exec sleep 4
