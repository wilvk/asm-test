# Implementation summary — Batch E: emulator & code-image (findings #22–25, #50)

*Source:* [2026-07-02 code review](../analysis/2026-07-02-code-review.md), findings 22–25 (and #50, folded into #24).
*Validated:* `codeimage.c` compiles clean on the host; the emulator suite runs
under Unicorn 2.1.1 in Docker (45/47 pass — the 2 failures,
`emu.fuzz_coverage_beats_fixed_vector` and `emu.mutation_strong_suite_kills_more`,
are **pre-existing** flaky statistical tests confirmed failing on the pristine
`HEAD` too, unaffected by these changes); a targeted probe proved #23 and #24.

## #22 — codeimage_track() re-arms clear_refs globally, wiping pending state (High)

`src/codeimage.c` — factored the "detect soft-dirty + snapshot" loop out of
`asmtest_codeimage_refresh()` into a shared `ci_scan_range(img, lo, hi)` helper.
`asmtest_codeimage_track()` now, when `nreg > 1`, scans+snapshots the
previously-tracked regions (`[0, nreg-1)`) **before** calling `ci_arm()`, so the
global `clear_refs` no longer erases un-snapshotted writes to existing regions.
The just-added region is excluded from the pre-arm scan (it was snapshotted a few
lines above) to avoid a redundant version. `refresh()` now delegates to the same
helper, so both paths share one proven scan-then-arm implementation.

## #23 — emu_call drops SysV integer args beyond the 6th (Medium)

`src/emu.c` `emu_x86_setup_sysv` — args `[6..niargs)` are now marshaled onto the
guest stack at `[rsp+8], [rsp+16], …` per psABI §3.2.3. The reservation above the
return address is rounded to 16 bytes so `rsp ≡ 8 (mod 16)` at entry (16-aligned
after the simulated call). ≤6-arg calls are byte-for-byte unchanged (`pad = 0`).

*Evidence:* a 7-int-arg routine reading `[rsp+8]` now returns 28 with
`faulted == 0` (previously arg 7 landed one byte past the stack mapping →
`EMU_FAULT_READ`).

## #24 — AArch64 emulator marshals only x0..x5 (Low) — also resolves #50

`src/emu.c` `emu_arm64_setup` — `arg_regs` extended to `x0..x7` and the loop bound
to 8 (AAPCS64 stage C rule C.9). `include/asmtest_emu.h` doc updated to "x0..x7".
This makes `docs/emulator.md`'s existing "x0–x7" statement correct, so finding
**#50** (doc-vs-code mismatch) is resolved by the code fix rather than a doc edit.

*Evidence:* an 8-arg AAPCS64 summing routine now returns 36 (was dropping x6/x7).

## #25 — emulator.md preload example maps over EMU_CODE_BASE (Low, doc)

`docs/emulator.md` — the "Preloading memory" example now uses `0x300000` (clear of
the internal code/stack regions) instead of `0x100000` (= `EMU_CODE_BASE`), with a
sentence explaining why, matching the addresses the project's own tests use.
