#!/bin/sh
# fake_serve.sh — a serve host with no tracer in it (07-serve-live-host.md T3).
#
# It speaks the control protocol (docs/internal/gui/asmtrace-schema.md, "Serve
# protocol") and emits canned lifecycle + mode events, so the desktop session
# host is testable on ANY machine: no ptrace, no permissions, no victim, no
# AMD silicon, and no dependence on what a real target happens to do in a
# 200 ms window. What it exercises is the half the desktop actually owns —
# spawning a host, framing lines out of a pipe, and turning the stream into
# Recordings — which is exactly the half a real asmspy cannot test.
#
# It deliberately produces the awkward shapes too: a refusal mid-session (an
# `err` must NOT end the session), a skip (a SUCCESS with nothing to report),
# and finally a TORN session that stops without its `end` footer.
set -eu

emit() { printf '%s\n' "$1"; }

HDR='{"asmtrace":1,"container":"ndjson","producer":{"name":"asmspy","version":"1.1.0"},"provenance":{"backend":"ptrace-syscalls","exact":true,"trust":"exact","redacted":false},"arch":"x86_64","pid":4242,"cmd":"./victim"}'

while IFS= read -r line; do
    case "$line" in
    *'"cmd":"quit"'*)
        emit '{"k":"cmd","cmd":"quit"}'
        exit 0
        ;;
    *'"cmd":"stop"'*)
        emit '{"k":"cmd","cmd":"stop"}'
        emit '{"k":"end","events":2,"truncated":false,"drops":{"lost":0,"throttled":false}}'
        emit '{"k":"session","state":"stopped","mode":"log","events":2,"reason":"stop"}'
        ;;
    *'"mode":"log"'*)
        emit '{"k":"cmd","cmd":"start","mode":"log"}'
        emit '{"k":"session","state":"started","mode":"log","pid":4242,"params":{"follow":false,"max":-1}}'
        emit "$HDR"
        emit '{"k":"syscall","line":"openat(AT_FDCWD, <path>, O_RDONLY) = 3","payload":"/etc/passwd"}'
        # A refusal DURING a session: it must be surfaced and must not end it.
        emit '{"k":"err","reason":"a session is already running — one ptrace jack per target (send \"stop\" first)","cmd":"start"}'
        emit '{"k":"syscall","line":"write(1, <14 bytes>, 14) = 14","payload":"hello, world\n"}'
        ;;
    *'"mode":"sample"'*)
        # A skip: the session RAN and had nothing to report. Note it still
        # produces a closed, honest recording — an empty file would be the bug.
        emit '{"k":"cmd","cmd":"start","mode":"sample"}'
        emit '{"k":"session","state":"started","mode":"sample","pid":4242,"params":{"ms":200}}'
        emit '{"asmtrace":1,"container":"ndjson","producer":{"name":"asmspy","version":"1.1.0"},"provenance":{"backend":"ibs-op","exact":false,"trust":"statistical"},"arch":"x86_64"}'
        emit '{"k":"end","events":0,"truncated":false,"drops":{"lost":0,"throttled":false},"skip":{"code":2,"reason":"IBS-Op is an AMD feature; this host is GenuineIntel"}}'
        emit '{"k":"session","state":"skip","mode":"sample","skip":{"code":2,"reason":"IBS-Op is an AMD feature; this host is GenuineIntel"}}'
        ;;
    *'"mode":"auto"'*'"sampler":"ibs"'*)
        # The STRONG path: an IBS-Op entry edge is a direct observation of the
        # event the capture waits for, so one pick and no walk.
        emit '{"k":"cmd","cmd":"start","mode":"auto"}'
        emit '{"k":"session","state":"started","mode":"auto","pid":4242,"params":{"max":-1,"module":"","sampler":"ibs"}}'
        emit '{"k":"session","state":"pick","mode":"auto","pick":{"sampler":"ibs-op","evidence":"entry","func":"entered_often","base":94207306414080,"len":96,"weight":184,"sites":2,"attempt":1,"of":1}}'
        emit '{"asmtrace":1,"container":"ndjson","producer":{"name":"asmspy","version":"1.1.0"},"provenance":{"backend":"ptrace-dataflow","exact":true,"trust":"exact"},"arch":"x86_64"}'
        emit '{"k":"df_step","step":0,"off":0,"disasm":"endbr64","ops":[]}'
        emit '{"k":"end","events":1,"truncated":false,"drops":{"lost":0,"throttled":false}}'
        emit '{"k":"session","state":"stopped","mode":"auto","events":1,"reason":"max"}'
        ;;
    *'"mode":"auto"'*)
        # The PORTABLE path, and the awkward one: residency is not entry
        # evidence, the first winner is never seen entering, and the server
        # WALKS the ranked candidates. Every step of that must reach the client
        # — a silent substitution would be the front door lying about what it
        # measured.
        emit '{"k":"cmd","cmd":"start","mode":"auto"}'
        emit '{"k":"session","state":"started","mode":"auto","pid":4242,"params":{"max":-1,"module":"","sampler":"sw"}}'
        emit '{"k":"session","state":"pick","mode":"auto","pick":{"sampler":"sw-clock","evidence":"residency","func":"grind_forever","base":94207306414080,"len":320,"weight":97,"sites":11,"attempt":1,"of":3}}'
        emit '{"k":"session","state":"pick","mode":"auto","pick":{"sampler":"sw-clock","evidence":"residency","func":"entered_often","base":94207306414400,"len":96,"weight":41,"sites":4,"attempt":2,"of":3}}'
        emit '{"asmtrace":1,"container":"ndjson","producer":{"name":"asmspy","version":"1.1.0"},"provenance":{"backend":"ptrace-dataflow","exact":true,"trust":"exact"},"arch":"x86_64"}'
        emit '{"k":"df_step","step":0,"off":0,"disasm":"endbr64","ops":[]}'
        emit '{"k":"end","events":1,"truncated":false,"drops":{"lost":0,"throttled":false}}'
        emit '{"k":"session","state":"stopped","mode":"auto","events":1,"reason":"max"}'
        ;;
    *'"mode":"torn"'*)
        # The dishonesty fixture: a session that starts, emits, and DIES with no
        # `end`. The host must mark that recording torn rather than present a
        # prefix as complete (schema Compatibility rules).
        emit '{"k":"cmd","cmd":"start","mode":"stream"}'
        emit '{"k":"session","state":"started","mode":"stream","pid":4242,"params":{"tid":0,"follow":false,"max":-1}}'
        emit "$HDR"
        emit '{"k":"stream","text":"work+0x4 [victim] push rbp"}'
        exit 0 # no end, no terminal session event: torn, on purpose
        ;;
    *)
        emit '{"k":"err","reason":"unknown mode (log|stream|trace|dataflow|tree|graph|procs|sample|watch|auto)","cmd":"start"}'
        ;;
    esac
done
