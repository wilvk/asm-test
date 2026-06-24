# Changelog

All notable changes to asm-test are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims
to follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Shared libraries + ABI manifest (Track 0).** The first slice of the
  multi-language bindings substrate. `make shared` builds
  `libasmtest.{so,dylib}` (framework runtime + capture trampoline, from `-fPIC`
  objects in a separate `build/pic/` tree) and `make shared-emu` builds
  `libasmtest_emu.{so,dylib}` (adds `emu.o`, links `-lunicorn`), both with
  platform-correct versioned filenames, soname/install-name, and dev symlinks;
  `make install-shared` / `install-shared-emu` install them plus a new
  `asmtest-emu.pc`. `make manifest` emits `asmtest_abi.json` — a machine-readable
  struct layout (sizes, field offsets, host arch, sentinels, flag masks) compiled
  from the real headers via `scripts/gen-manifest.c` — so FFI bindings consume
  offsets instead of hand-transcribing them. `_Static_assert`s in `asmtest.h` /
  `asmtest_emu.h` pin `regs_t` and the emulator register structs to `offsetof`,
  preventing the headers, the trampoline's stores, and the manifest from drifting
  apart. `make install` (static + headers) is unchanged. See
  [docs/plans/multi-language-bindings-plan.md](docs/plans/multi-language-bindings-plan.md).
- **Binding ABI + conformance corpus (Track 0).** Non-jumping verdict shims
  `asmtest_check_abi` / `asmtest_check_flag` *return* a verdict + reason instead
  of `longjmp`-ing into the runner, so an FFI binding can validate a capture with
  no C runner present (the existing `ASSERT_ABI_PRESERVED` / `ASSERT_FLAG_*` now
  delegate to them). `ASMTEST_NO_MAIN` builds the runtime without its `main()`
  for embedding. `make conformance` runs `bindings/conformance/conformance.c` —
  the C reference for a fixed corpus of canonical routines (int / FP / SIMD /
  flags / ABI capture + an x86-64 emulator case), checked against expected
  literals — and emits `corpus.json`, the portable expected-results table every
  language binding must reproduce. The binding-ABI contract symbols are
  designated in the API reference.
- **Parallel execution (Track E).** `-jN` / `--jobs=N` runs up to N tests
  concurrently as forked children (a pool over the existing per-test fork model),
  while output stays in registration order regardless of finish order. Per-test
  timeout and crash containment are unchanged; `--no-fork` forces serial. New
  `expect.sh` self-tests pin the ordering, failure reporting, and crash
  containment under `-j4`.
- **libc-callback example (Track E).** `examples/callback.s` / `.asm` with
  `examples/test_callback.c`: `sum_map(arr, n, fn)` and `count_if(arr, n, pred)`
  call a C function pointer per element, demonstrating an assembly routine
  calling back into C with correct callee-saved/stack-alignment discipline.
- **Valgrind story (Track E).** `make valgrind` runs the example suites under
  memcheck (`--no-fork`) to catch bugs in the routine under test, complementing
  the always-on guard-page allocator; `make docker-valgrind` and the
  `--valgrind` flag of `scripts/install-deps.sh` round it out. Documented
  alongside the guard-page approach in the README.
- **Emulator FP/SIMD (Track C).** The x86-64 emulator guest marshals `double`
  args (`emu_call_fp`) and 128-bit vector args (`emu_call_vec`) into xmm0..7 and
  captures the whole XMM file (`emu_x86_regs_t.xmm[]`). The AArch64 guest gains
  the same (`emu_arm64_call_fp` / `emu_arm64_call_vec`, NEON `v[]`); the RISC-V
  (`emu_riscv_call_fp`, `f[]`) and ARM32 (`emu_arm_call_fp`, `q[]`) guests gain
  scalar FP, with their FP units enabled at open (RISC-V `mstatus.FS`, ARM32
  CPACR + FPEXC). Generic `ASSERT_EMU_VEC128_EQ` works across guests.
- **Emulator assertions (Track C).** `ASSERT_NO_FAULT`, `ASSERT_FAULT`,
  `ASSERT_FAULT_AT`, `ASSERT_EMU_REG_EQ`, `ASSERT_EMU_FP_EQ`, `ASSERT_EMU_VEC_EQ`,
  and coverage `ASSERT_BLOCK_COVERED` / `ASSERT_BLOCKS_AT_LEAST`.
- **Coverage reporting (Track C).** `emu_trace_report`, `emu_coverage_uncovered`
  (lists the blocks a run missed against a universe trace), `emu_trace_lcov`
  (offset-level lcov export), and the `emu_trace_covered` predicate.
- **Emulator vector parity & source-line coverage (Track C, leftovers).**
  `emu_arm_call_vec` marshals 128-bit NEON vectors into ARM32 `q0..q3` and
  captures the whole `q0..q15` file, matching the x86-64/AArch64 vector path.
  Source-line coverage: a caller-supplied `emu_line_map_t` (ascending
  `(offset, line)` rows, produced out-of-band) drives `emu_line_lookup`,
  `emu_trace_source_report`, and `emu_trace_lcov_source`, which report block
  coverage against source lines (hit **and** missed) — no DWARF parsing, no new
  dependency. The RISC-V "V" extension has no counterpart: Unicorn's RISC-V
  guest exposes no vector registers, so it stays scalar-FP (documented in
  `src/emu.c` and `docs/emulator.md`). Closes Track C's open C items.

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
