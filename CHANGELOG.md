# Changelog

All notable changes to asm-test are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims
to follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **In-line assembler tier (Keystone).** Pass a routine as an *assembly string*
  and run it, instead of only as pre-assembled object code. `asmtest_assemble()`
  (in the new `include/asmtest_assemble.h`) turns text into machine code for the
  emulator's guest set — x86-64 (Intel or AT&T syntax), AArch64, ARM32, and
  RISC-V where the linked Keystone supports it — with errors reported as data and
  output the caller frees via `asmtest_asm_free()`. Bridge wrappers `emu_call_asm`
  / `emu_arm64_call_asm` / `emu_riscv_call_asm` / `emu_arm_call_asm` assemble at
  the emulator's load base (so PC-relative and branch targets resolve) and run
  through the matching `emu_*_call` in one call. Optional and pkg-config gated
  like the emulator tier: `make asm-test` (links `libkeystone` + `libunicorn`),
  `make deps DEPS_ARGS=--asm`, `make docker-asm`, and a CI `asm` job on both
  x86-64 and arm64. The bindings expose it as `CallAsm` (an opaque-handle shim,
  `asmtest_emu_call_asm`, carried by `libasmtest_emu`), with a conformance case
  per language. See the
  [implementation plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/inline-asm-keystone-plan.md).

- **Native Win64 tier (capture).** A Microsoft x64 (“Win64”) capture trampoline
  (`src/capture_win64.asm`) mirrors all eight System V `asm_call_capture*`
  variants on real x86-64 silicon — integer/FP/vector args, the 32-byte shadow
  space, struct return and by-reference struct args, and ABI-preservation over the
  *larger* Win64 callee-saved set (`rdi`/`rsi` plus the callee-saved `xmm6–15`).
  The captured state has a first-class `regs_t` layout in `include/asmtest.h`
  (selected by `-DASMTEST_ABI_WIN64`, LLP64-correct, with `_Static_assert` offset
  pins) and a machine-readable manifest (`make manifest-win64` →
  `asmtest_abi_win64.json`). It runs **with no Windows host**, two ways: the
  native lane via GCC/Clang `__attribute__((ms_abi))` (`make win64-msabi-test`),
  and a real Windows PE built with `nasm -f win64` + MinGW-w64 and run under Wine
  in an isolated image (`Dockerfile.win64`, `make docker-win64`). A new CI `win64`
  job runs both on every push; the capture suite doubles as the native Win64
  conformance check. This is the capture tier (suite runs `--no-fork`); the Win32
  runner port is now underway (see below). See
  [docs/win64.md](https://github.com/wilvk/asm-test/blob/main/docs/win64.md)
  and the [implementation plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/win64-native-tier-plan.md).

- **Native Win64 tier — runner port.** The framework's process-level
  guarantees now have Win32 equivalents for the Win64 tier, each in
  `src/platform_win32.c` (plus the platform-neutral `src/glob_match.c`), compiled
  only for the Win64 target and **verified under Wine**: per-test isolation +
  timeout via `CreateProcess` / `WaitForSingleObject` / `TerminateProcess`
  (`asmtest_win32_run`, classifying OK / CRASH-as-NTSTATUS / TIMEOUT), the `-jN`
  parallel pool via `WaitForMultipleObjects` (`asmtest_win32_run_pool`), the
  guard-page allocator via `VirtualAlloc` + `VirtualProtect(PAGE_NOACCESS)`,
  in-process crash-to-failure via a vectored exception handler +
  `__builtin_longjmp` (`asmtest_win32_guard`, no SEH unwinding), and a portable
  `--filter` glob matcher (`*`, `?`, `[...]` classes, `\` escaping) replacing
  MinGW's missing `fnmatch`. New `make win64-{guard,isolate,pool,filter,seh}-test`
  targets exercise each under Wine and join `make win64-check` / the CI `win64`
  job. A thin platform seam (`src/platform.h`, `ASMTEST_FNMATCH`) wires the
  `--filter` and guard-page paths into `src/asmtest.c` with no POSIX regression.
  The runner itself is then built for Win64: a Win32 `run_one` (the per-test
  facility's vectored handler + watchdog, mapping the recovery reason to
  fail/skip/crash/timeout), `main()` running `--no-fork` with the fork/pipe/poll
  isolation, parallel pool, signal handlers, and SysV-trampoline helpers gated to
  POSIX. `make win64-runner-test` builds `src/asmtest.c` with MinGW and runs a real
  `TEST()` suite (`tests/win64/suite_win64.c`) under Wine: the runner discovers and
  runs the suite, asserts real Win64 captures, and contains a crashing and a
  hanging test as reported failures while surviving. Still POSIX-only: a
  forked/`-jN` mode on Win64 and benchmarks. See
  [docs/win64.md](https://github.com/wilvk/asm-test/blob/main/docs/win64.md).

- **Packaging scaffolding for all ten bindings.** Each binding now has a
  publish-ready registry manifest and a `make <lang>-package` target that
  assembles a distributable bundling the host's prebuilt native libs:
  `asmtest.gemspec` (RubyGems), `asmtest-1.0.0-1.rockspec` (LuaRocks), `pom.xml`
  (Maven), `asmtest.nuspec` (NuGet), `CMakeLists.txt` (a `find_package`-able
  C++ INTERFACE target), `build.zig.zon` (Zig package), plus upgraded
  `pyproject.toml` (wheel `package-data` over a bundled `asmtest/_libs/`),
  `Cargo.toml` (crates.io metadata), and `package.json` (npm `files`). `make
  package-libs` stages the shared libs into `build/dist/native/<plat>/`; the
  dlopen bindings (Python/Ruby/Lua/Node/Java/.NET) bundle `libasmtest_emu`, while
  the link bindings (Rust/Zig/C++/Go) ship as source. A new
  [docs/packaging.md](https://github.com/wilvk/asm-test/blob/main/docs/packaging.md) is the release guide (native-lib split,
  version pinning, per-language commands, the multi-platform caveat). Scaffolding
  only — no registry credentials or cross-OS build matrices.

- **Go binding (Track G).** A `cgo` wrapper in `bindings/go/` over the
  opaque-handle FFI layer — no struct layout mirrored: it declares the
  binding-ABI entry points (`asmtest_corpus_routine`, `asmtest_capture6`/`_fp2` +
  `asmtest_regs_*`, `asmtest_check_abi`, `asmtest_emu_call2` + accessors) and
  links the prebuilt shared libs. Exposes `Regs` (capture / ABI / flags / FP),
  `Emu` + `EmuResult` (faults as data), and Tier-2 `Assert*` helpers over a small
  `TB` interface that `*testing.T` satisfies (so the helpers are themselves
  testable — the suite proves each one bites). `make go-test` runs `go test`;
  [`conformance_test.go`](https://github.com/wilvk/asm-test/blob/main/bindings/go/conformance_test.go) replays the corpus,
  built + run in its own `asmtest-go` image (`make docker-go`) and the `bindings`
  CI matrix. This closes the last language track — **all ten bindings** (Python,
  Rust, C++, Zig, Node, Java, .NET, Ruby, Lua, Go) now ship Tier 1 + Tier 2.

- **Tier-2 idiomatic assertions (all ten bindings).** Optional assertion layers
  over the Tier-1 result objects, with legible failure messages, idiomatic to
  each language: Python (`asmtest.assertions`, raising `AssertionError`), Rust
  (methods on `Regs`/`EmuResult`, panicking), C++ (`asmtest::assert_*` throwing
  `assertion_error`, for GoogleTest/Catch2), Zig (error-union helpers over
  `std.testing`), Node/Ruby/Lua/Java/.NET (throwing/raising `assert_*` helpers in
  the conformance runner), and Go (`Assert*` helpers failing a `*testing.T`). Each
  covers both the pass paths and the failure paths
  (the assertion fails when it should — pytest `raises`, Rust `should_panic`, Zig
  `expectError`, a recording `TB` stub in Go, try/catch elsewhere). `assert_ret`,
  `assert_abi_preserved`,
  `assert_flag`, `assert_fp`, `assert_no_fault`, `assert_reg`, ….

- **Node, Java, .NET, Ruby & Lua bindings (Tracks N/J/D/C).** Five more language
  wrappers, all over a new opaque-handle FFI layer (`src/ffi.c` + emu helpers in
  `emu.c`): `asmtest_regs_new` + `asmtest_capture6` / `_fp2` + `asmtest_regs_*`
  accessors for the capture tier, `asmtest_emu_call2` + `asmtest_emu_*` accessors
  for the emulator, and `asmtest_corpus_routine(name)` for routine addresses — so
  a dynamic binding needs no C struct layout. Bindings: Node (`koffi`), Java
  (FFM/Panama), .NET (P/Invoke), Ruby (stdlib `Fiddle`), Lua (LuaJIT `ffi`); each
  replays the conformance corpus (`make node-test` / `java-test` / `dotnet-test`
  / `ruby-test` / `lua-test`).
- **Isolated per-language Docker images.** Each wrapper is built and tested in
  its **own** image (`bindings/<lang>/Dockerfile` on a shared
  `Dockerfile.bindings-base`), so toolchains never mix. `make docker-<lang>`
  builds + runs one language; `make docker-bindings` does all ten. The CI
  `bindings` job is now a per-language matrix running `make docker-<lang>`.

- **Zig binding (Track Z).** The lowest-ceremony wrapper: `bindings/zig/`
  consumes the C headers directly via `@cImport` — no separate binding layer —
  and replays the conformance corpus (`make zig-test` → `zig build test`,
  `build.zig` targets Zig 0.13.x). Added to the Docker bindings image and the
  `bindings` CI job.

- **Rust binding (Track R).** A no-crates-io crate in `bindings/rust/`:
  `#[repr(C)]` mirrors of `regs_t` and the emulator structs (arch-selected via
  `cfg`) plus `extern "C"` declarations of the binding-ABI entry points, linked
  against the prebuilt shared libs by `build.rs`. Exposes `capture` /
  `capture_fp` / `capture_vec` → `Regs`, `abi_preserved` (native verdict shim),
  and an `Emulator` whose `EmuResult` carries faults as data. `make rust-test`
  runs `cargo test`; `tests/conformance.rs` replays the conformance corpus.
- **C++ binding (Track X).** The C headers now carry `extern "C"` guards (and a
  portable `ASMTEST_STATIC_ASSERT`), so a C++ TU both compiles and links against
  the framework. `bindings/cpp/asmtest.hpp` adds an RAII `Emu`, initializer-list
  `capture*`, vector-lane helpers, and `abi_preserved` / `flag_set` predicates;
  `make cpp-test` runs an example suite that drives the framework from C++. New
  `ASMTEST_NO_MAIN` knob omits the runtime's `main()` for embedding.
- **Docker per-language wrapper testing.** `Dockerfile.bindings` bundles the
  Python, C++, and Rust toolchains plus libunicorn; `make docker-bindings`
  (and `docker-python` / `docker-cpp` / `docker-rust`) build and test every
  wrapper in one reproducible image — verifying a binding on any host, including
  a language not installed locally. A `bindings` CI job runs the same tests
  natively on x86-64 and arm64 Linux.

- **Python binding (Track P).** A pure-ctypes package in `bindings/python/`
  (no `cffi`/compile step) loads the shared library and the `asmtest_abi.json`
  manifest and exposes `capture()` / `capture_fp()` / `capture_vec()` (returning
  a `Regs` snapshot with `ret`, `flags`, `fret`, vector lanes, `abi_preserved`,
  and `flag_set`) plus an `Emulator` context manager whose `EmuResult` surfaces
  faults as data. Struct layout is read from the manifest, so the binding is
  correct for whatever architecture the library was built for. `make
  python-test` builds the shared libs, manifest, corpus, and a routine fixture
  lib, then runs pytest; the suite replays the same `corpus.json` the C
  reference emits and reproduces every case. A new `bindings-python` CI job
  (x86-64 + arm64 Linux) runs it — the reusable per-language CI template
  (bindings plan 0.5), which completes Track 0.

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
  [docs/plans/multi-language-bindings-plan.md](https://github.com/wilvk/asm-test/blob/main/docs/plans/multi-language-bindings-plan.md).
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

First tagged release. Captures the complete framework plus the
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
