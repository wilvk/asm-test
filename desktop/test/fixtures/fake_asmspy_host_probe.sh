#!/bin/sh
# fake_asmspy_host_probe.sh — stands in for `asmspy --info <pid> --json` when
# the CALLER is InspectState::host_probe (2026-08-06 final review, post-review
# finding: the perf verdict's focus/selection dependency). That runner probes
# its OWN pid (getpid()) rather than a selected target, so unlike every other
# fake_asmspy_*.sh in this directory — which hardcode a fixed identity.pid —
# this one echoes back whatever pid it was actually asked about (argv[2],
# between "--info" and "--json"): a fixed pid would fail procinfo_tick's
# pid-mismatch guard on every real host this test runs on, since the real
# test binary's pid changes every run.
#
# Carries a `host` verdict (perf_ok=false) so the session-level probe test can
# assert the refusal crossed the real fork/exec/decode path, not a hand poke.
printf '%s\n' '{"asmtrace":1,"provenance":{"exact":true},"arch":"x86_64"}'
printf '%s\n' "{\"k\":\"procinfo\",\"identity\":{\"pid\":$2,\"start_ticks\":1},\"host\":{\"perf_ok\":false,\"perf_why\":\"perf_event_open refused for the asmspy binary itself (fixture)\"}}"
