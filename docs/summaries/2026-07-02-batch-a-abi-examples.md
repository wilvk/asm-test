# Implementation summary — Batch A: ABI capture & shipped examples (findings #1–6)

*Source:* [2026-07-02 code review](../analysis/2026-07-02-code-review.md), findings 1–6.
*Validated:* x86-64 (`make test usecases`, 10/10) and linux/arm64 under qemu-user
(`make test` all suites pass, `make usecases` 10/10), plus a targeted d8–d15
capture/containment probe.

## #1 — AArch64 callee-saved SIMD d8–d15 not seeded/checked/restored (High)

Followed the codebase's own Win64 precedent (callee-saved `xmm6`–`xmm15` are
checked by the separate `ASSERT_ABI_PRESERVED_VEC`, not `ASSERT_ABI_PRESERVED`)
and added the AArch64 equivalent for `d8`–`d15`:

- `src/capture.s` — in the two vector trampolines (`asm_call_capture_vec`,
  `asm_call_capture_vec_n`): **save** the caller's `d8`–`d15`, **seed** their low
  64 bits with sentinels `8..15`, and **restore** them after the register file is
  captured. The full `v0..v31` capture already lands `v8`–`v15` in `vec[8..15]`.
  Frames grew 112→176 (fixed) and 160→224 (x29-relative) to hold the 64-byte save
  area. Seeding-then-restoring means a clobber by the routine under test is
  observable via `vec[8..15]` yet cannot leak into the C caller's live doubles.
- `src/asmtest.c` — `asmtest_check_abi_vec` gains an AArch64 branch verifying
  `vec[i].u64[0] == i` for `i` in `8..15` (only the low 64 bits are callee-saved
  per AAPCS64 6.1.2; the upper lane is caller-saved and not checked).
- `include/asmtest.h` — `asmtest_check_abi_vec` / `asmtest_assert_abi_vec` /
  `ASSERT_ABI_PRESERVED_VEC` are now declared for `__aarch64__` as well as Win64.

*Scope note:* the fix lands on the vector-capture path (the path designed for
vector-ABI checking), exactly mirroring the existing Win64 design. The
general-purpose trampolines still do not seed/check FP registers — consistent
with how they already ignore the FP file — so d8–d15 preservation is asserted via
`ASSERT_ABI_PRESERVED_VEC`, not `ASSERT_ABI_PRESERVED`.

*Evidence:* probe with a raw-asm routine that leaves `d8` dirty is now caught
(`check_abi_vec` flags reg 8); a caller holding `3.14159` in `d8` across the call
gets it back intact (containment); GP sentinels still verify (frame-change
regression guard).

## #5 — vm.s AArch64 clobbers callee-saved x23 + under-allocates frame (High)

`examples/vm.s` — save/restore `x23` (paired with `x24` for 16-byte alignment)
and grow the frame 272→320 so the documented 256-byte / 32-slot operand stack
actually fits (base moved to `sp+64`, top at `sp+320`). `make usecases` now
passes `vm.preserves_callee_saved_registers` on AArch64 (was the sole failure).

## #4 — pst_mixed AArch64 body did not implement its C prototype (Medium)

`examples/structparam.s` — the AArch64 `pst_mixed` now reads the second eightbyte
from `x1` (`fmov d0, x1; fcvtzs …`) per AAPCS64 rule C.12 (a non-HFA ≤16-byte
composite goes entirely in GP registers), instead of `d0`. `examples/test_structparam.c`
marshals per-ABI: on AArch64 it passes both eightbytes as integers via `ASM_CALL2`
(the double bit-cast into `x1`); on x86-64 it keeps `asm_call_capture_fp`
(rdi/xmm0). The test now asserts the real platform convention, not the harness's.
Comments corrected in both files.

## #2, #3 — header comments for small-struct returns/args (Medium/Low, docs)

`include/asmtest.h` — corrected the `asm_call_capture_sret` comment to note that
AArch64 `regs_t` captures only `x0`/`d0` (not the second return register `x1`),
and the `asm_call_capture_bigstruct` comment to state that on AArch64 a
`struct{long;double}` is two integer args (`x0` + the double's bits in `x1`),
NOT `asm_call_capture_fp`.

## #6 — doc claimed ASSERT_ABI_PRESERVED checks the stack pointer (Low, docs)

`docs/abi-capture.md` — removed the false "(and the stack pointer)" claim, listed
the exact GP registers checked per ABI, and documented `ASSERT_ABI_PRESERVED_VEC`
(Win64 xmm6–15 / AArch64 d8–15) as the vector complement.
