# Changelog

All notable changes to asm-test are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims
to follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Emulator FP/SIMD (Track C).** The x86-64 emulator guest marshals `double`
  args (`emu_call_fp`) and 128-bit vector args (`emu_call_vec`) into xmm0..7 and
  captures the whole XMM file (`emu_x86_regs_t.xmm[]`).
- **Emulator assertions (Track C).** `ASSERT_NO_FAULT`, `ASSERT_FAULT`,
  `ASSERT_FAULT_AT`, `ASSERT_EMU_REG_EQ`, `ASSERT_EMU_FP_EQ`, `ASSERT_EMU_VEC_EQ`,
  and coverage `ASSERT_BLOCK_COVERED` / `ASSERT_BLOCKS_AT_LEAST`.
- **Coverage reporting (Track C).** `emu_trace_report`, `emu_coverage_uncovered`
  (lists the blocks a run missed against a universe trace), `emu_trace_lcov`
  (offset-level lcov export), and the `emu_trace_covered` predicate.

### Fixed

- **Emulator handle reuse.** Unicorn's translation-block cache is now flushed
  when new code is loaded, so reusing an `emu_t`/guest handle for a different
  routine no longer re-runs the previous routine's stale translation.

## [1.0.0] — 2026-06-24

First tagged release. Captures the complete Phase 0–11 framework plus the
Track A self-test suite and Track B packaging.

### Added

- **Core framework.** Auto-discovered `TEST(...)` cases, a provided `main()`,
  per-suite `SETUP`/`TEARDOWN`, `SKIP(reason)`, and colored TAP reporting with a
  nonzero exit on failure.
- **Assertions.** `ASSERT_TRUE/FALSE`, signed `ASSERT_EQ/NE/LT/LE/GT/GE`,
  unsigned `ASSERT_UEQ/UNE/ULT/ULE/UGT/UGE`, `ASSERT_STREQ`, `ASSERT_MEM_EQ`
  (hexdump diff), `ASSERT_REG_EQ`, FP `ASSERT_FP_EQ/NEAR` + lane
  `ASSERT_DEQ/DNEAR/FEQ/FNEAR`, and `ASSERT_VEC_EQ`.
- **ABI capture.** Register/flags capture via `ASM_CALLn`, `ASSERT_ABI_PRESERVED`,
  `ASSERT_FLAG_SET/CLEAR`; full call model (`ASM_CALLN`, `ASM_SRET`, `ASM_FCALLn`,
  `ASM_VCALLn`, struct-by-value) across the System V integer/FP/vector paths.
- **Differential / property testing.** `ASSERT_MATCHES_REF{1,2,3}` with a seedable
  splitmix64 RNG (`ASMTEST_SEED`).
- **Robustness & CLI.** Per-test `fork()` isolation with an `alarm()` timeout,
  crash/hang containment, and a runner CLI (`--filter`, `--list`, `--shuffle`/
  `--seed`, `--timeout`, `--no-fork`, `--format=tap|junit`).
- **Benchmark mode.** `BENCH(...)` cases timed in cycles/call via `rdtsc` /
  `cntvct_el0`, run under `--bench`.
- **Portability.** x86-64 and AArch64, Linux and macOS; GAS (default) and NASM
  (`ASM_SYNTAX=nasm`, x86-64) backends. CI covers all four OS/arch combinations.
- **Emulator tier (optional).** Unicorn-backed x86-64, AArch64, RISC-V (RV64),
  and ARM32 guests; Windows x64 ABI on the x86-64 engine; instruction trace and
  basic-block coverage.
- **Framework self-tests (Track A).** `tests/positive.c`, `tests/negative.c`, and
  the `tests/expect.sh` black-box harness, run by `make check` and wired into CI.
- **Packaging (Track B).** `ASMTEST_VERSION` macros; `make lib` builds
  `libasmtest.a`; `make install`/`uninstall` honoring `PREFIX`/`DESTDIR`; an
  `asmtest.pc` pkg-config file; and `make amalgamate` producing the single-header
  `asmtest_single.h`.

[Unreleased]: https://github.com/wilvk/asm-test/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/wilvk/asm-test/releases/tag/v1.0.0
