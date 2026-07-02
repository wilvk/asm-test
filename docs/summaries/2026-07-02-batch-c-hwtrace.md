# Implementation summary — Batch C: hardware-trace tier (findings #11–17)

*Source:* [2026-07-02 code review](../analysis/2026-07-02-code-review.md), findings 11–17.
*Host:* AMD Ryzen 9 9950X (Zen 5), Linux 6.17, `perf_event_paranoid=4` (unprivileged
perf/LBR/PT blocked — single-step needs no perf, so it is live-validated; AMD LBR
and Intel PT decode logic is validated by construction + the synthetic tests).
*Validated:* `make hwtrace-test` **95/0** (Docker + Capstone); the single-step
block partition is confirmed against the Unicorn emu backend as ground truth.

## #13 — single-step block normalization diverged from PT/DR/Unicorn (High)

`src/disasm.c` gained `asmtest_disas_is_branch()` (Capstone JUMP/CALL/RET/IRET
groups), declared in `include/asmtest_trace.h`. `src/ss_backend.c` `ss_normalize`
now starts a new block after every branch-class instruction — so the fall-through
of a NOT-taken conditional branch (and a call-return re-entry) begins a block,
which fall-through-discontinuity alone missed — and collapses consecutive
identical offsets (REP-prefixed string insns trap per-iteration under TF).

*Ground truth:* the Unicorn emu backend yields `{0x0,0x7,0xf}` for the loop
fixture and `{0x0,0x11}` for the routine; single-step now matches both exactly.
The pre-existing `test_singlestep_loop` asserted `{0,0x7}` — the exact bug #13
describes — and was corrected to `{0,0x7,0xf}` in `examples/test_hwtrace.c`.

## #16 — AMD replay omitted the not-taken-branch fall-through block (Medium)

`src/amd_backend.c` `amd_replay` uses the same `asmtest_disas_is_branch()` in its
straight-line walk: an intermediate branch-class instruction (a not-taken
conditional branch, since a taken one would be the recorded `from`) now starts a
new block at its fall-through, matching the PT/DR/Unicorn partition.

## #15 — a routine clearing EFLAGS.TF stopped capture without truncated (Medium)

`src/ss_backend.c` `ss_normalize` now flags `truncated` when the last recorded
in-region instruction is a non-branch whose fall-through is still strictly inside
the region — i.e. stepping stopped early (the Intel POPF/IRET-clears-TF case)
rather than exiting via a control transfer. Conservative: it fires only when the
trace genuinely ends mid-region at a non-CTI. (On this AMD host `popfq` clearing
TF does not suppress the trailing #DB, so stepping continues and the trace is
complete — the heuristic correctly does not fire, and all 95 single-step-path
assertions still report not-truncated.)

## #12 — AMD LBR ring-full sample loss was undetectable (High)

`src/hwtrace.c` `hwtrace_end_amd`: the data ring is never drained mid-capture, so
the kernel never writes the pending `PERF_RECORD_LOST`. Now a (near-)full ring is
treated as loss — if less than one maximum-size branch-stack sample of headroom
remains (`span + max_sample > dsz`), `lost` is set — and `lost` unconditionally
sets `trace->truncated` after decode, so even the single-window Tier-A path can no
longer report a ring-truncated capture as complete.

## #11 — Intel PT decode returned an empty trace as complete (High)

`src/pt_backend.c` `asmtest_pt_decode` now counts in-region instructions and
returns `ASMTEST_HW_EDECODE` when a non-empty AUX stream yields zero of them, so
`end()` flags the trace truncated instead of "complete empty". *Remaining (Intel
PT hardware):* the full fix also programs a `PERF_EVENT_IOC_SET_FILTER` region
address filter on the capture side so the trace is scoped to the region; that
half needs a GenuineIntel PT host to validate and is left as a documented
follow-up (this dev box is AMD).

## #14 — available(SINGLESTEP) true on non-Linux x86-64 stub (Medium)

`src/hwtrace.c` `cpu_matches` gates the SINGLESTEP arm on
`defined(__x86_64__) && defined(__linux__)`, so on a non-Linux x86-64 build (e.g.
Intel-mac) `available()` self-skips with the already-written "Linux x86-64 only"
reason instead of reporting available and yielding an empty complete trace.
(Manifests only off Linux; correct by inspection.)

## #17 — init during an active capture corrupted the state machine (Low)

`src/hwtrace.c` `asmtest_hwtrace_init` returns `ASMTEST_HW_ESTATE` when a capture
is live (`g_fd >= 0 || g_active != NULL`), so a backend switch mid-capture can no
longer run the wrong `end()` teardown and leak the perf fd/mmaps. Defensively,
`asmtest_hwtrace_end` now also releases an orphaned fd/mmaps when `g_active` is
NULL, so the tier can never be permanently wedged.
