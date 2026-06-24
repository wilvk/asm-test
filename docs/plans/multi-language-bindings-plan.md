# asm-test — Multi-language bindings plan

A roadmap for exposing asm-test to languages other than C, so other ecosystems
can **run**, **test**, and **call** assembly through the framework's two engines —
the native **capture trampoline** and the **Unicorn emulator** — without copying
sources or writing C glue by hand.

This plan implements the conclusions of
[Analysis: multi-language wrappers](../analysis/multi-language-wrappers.md). Read
that first for *why* it works and the correctness rules; this document is the
*how* and *in what order*.

> Status legend: **planned** unless noted. Update this file as tracks land, the
> way [DESIGN.md](../../DESIGN.md) and [expansion-plan.md](expansion-plan.md) track
> their phases.

---

## Goals & non-goals

**Goal.** For each target language, ship a binding that can:

1. **call** a routine through the real ABI (general, non-test use),
2. **capture** a routine's registers/flags/ABI-preservation/FP/vector state via the
   trampoline, and
3. **emulate** a routine (full register file, faults-as-data, trace/coverage) via
   the emulator tier,

and let the language's *own* test runner validate the results (Tier 1). Idiomatic
assertion helpers (Tier 2) are an optional follow-on per language.

**Non-goals.** Re-implementing the C runner, discovery, or reporting in another
language (Tier 3) — each target already has a test runner. Rewriting the C + asm
core. Adding a hard dependency to the core build: every binding consumes a
**separately built** shared library; `make test` / `make check` stay C-only.

---

## Context: what is reusable as-is

- The trampoline surface is FFI-shaped: `asm_call_capture(regs_t*, void* fn,
  const long* args)` plus `_fp` / `_vec` / `_fp_n` / `_vec_n` / `_args` / `_sret` /
  `_bigstruct` — pointers and `long` arrays only. The variadic `ASM_CALLn` are *cpp
  macros*; bindings target the underlying array-form functions (especially
  `asm_call_capture_args(out, fn, args, nargs)`).
- The emulator surface is opaque-handle + value-struct: `emu_open`/`emu_close`,
  `emu_call`/`_fp`/`_vec`/`_traced`, `emu_call_win64`, and the per-guest
  `emu_arm64_*`, `emu_riscv_*`, `emu_arm_*` families, each returning a
  caller-owned `*_result_t`.
- Every routine is an exported, typed symbol via `ASM_FUNC` (`.globl` +
  `.type`), so a loader can `dlsym` it and pass the pointer to the trampoline.
- `regs_t`, `emu_*_regs_t`, and `emu_result_t` have fixed, documented field
  offsets (per-arch `#if` branches in the headers).
- Guard-page buffers (`asmtest_guarded_alloc` / `_under`) and the RNG
  (`asmtest_rng_*`) are plain C-ABI helpers a binding can reuse.

### The one missing prerequisite

The build emits only `libasmtest.a` (static; framework runtime + trampoline) — see
[`Makefile`](https://github.com/wilvk/asm-test/blob/main/Makefile) `FRAMEWORK_OBJS`
and the `lib` target. FFI via `dlopen` wants a position-independent shared object.
That gap is closed in **Track 0**.

---

## Track 0 — Shared substrate *(prerequisite for every language) — done*

**Goal.** Build the artifacts and contracts every binding consumes, once, so each
language track is mechanical.

**Status: done (0.1 – 0.5).** Every sub-item is in place: a binding is *possible*
(loadable shared libraries), *correct* (a drift-proof layout contract),
*generation-friendly* (a macro-free binding-ABI surface with non-jumping verdict
shims), *verifiable* (a shared conformance corpus), and *CI'd* (a reusable
per-language job, first instantiated for Python). Verified on x86-64 macOS:

- **0.1 shared libraries.** `make shared` builds `libasmtest.{so,dylib}` from
  `-fPIC` objects (framework runtime + capture trampoline, in a separate
  `build/pic/` tree); `make shared-emu` adds `emu.o` and links `-lunicorn` as a
  separate `libasmtest_emu.{so,dylib}` so the core binding never pulls in Unicorn.
  Platform-correct versioned filenames + soname/install-name + dev symlinks
  (`libasmtest.dylib → libasmtest.1.dylib → libasmtest.1.0.0.dylib`, the ELF
  `.so.MAJOR` equivalent on Linux). `make install-shared` / `install-shared-emu`
  place them in `$(libdir)` and install a new `asmtest-emu.pc` (the core
  `asmtest.pc` already serves the shared core lib). Verified end to end with a
  `dlopen`/`dlsym` consumer that mirrors `regs_t` from the manifest and calls a
  routine through the trampoline (→ correct result), and by staging the install
  into a temp prefix (symlink chain + manifest + resolving `pkg-config`).
- **0.2 binding ABI.** Non-jumping verdict shims `asmtest_check_abi` /
  `asmtest_check_flag` *return* a verdict + reason instead of `longjmp`-ing into
  the runner, so a binding can validate a capture across the FFI boundary with no
  C runner present (the jumping `asmtest_assert_*` now delegate to them). The
  contract symbol set is designated in the [API reference](../api-reference.md)
  (Binding ABI section) — every call path already has a non-variadic array form,
  so a generator needn't emulate cpp expansion. `ASMTEST_NO_MAIN` lets the
  runtime link without its `main()` for embedding/driver use.
- **0.4 conformance corpus.** `bindings/conformance/conformance.c` is the C
  reference: a fixed set of canonical routines with known captures, driven
  through the binding-ABI entry points and checked against expected literals (8
  cases across int/FP/SIMD/flags/ABI capture + an x86-64 emulator case). `make
  conformance` runs it (TAP, nonzero on mismatch) and emits `corpus.json`, the
  portable expected-results table every language binding must reproduce — one
  source of truth for "did the binding wire the ABI up correctly".

Kept `make install` (static + headers) unchanged so the Track B install stays
toolchain-light; the shared install is opt-in via the new targets. **0.5** landed
alongside the first binding: the `bindings-python` CI job (x86-64 + arm64 Linux)
is the reusable per-language template — install the language toolchain, build the
shared libs + manifest + corpus, run the binding tests. The sub-sections below
describe the full scope.

### 0.1 Shared-library build targets *(done)*

- `make shared` → `libasmtest.{so,dylib}` from `FRAMEWORK_OBJS` built `-fPIC`
  (framework runtime + capture trampoline). This is the artifact the **trampoline**
  bindings load.
- `make shared-emu` → `libasmtest_emu.{so,dylib}` adding `emu.o` and linking
  `-lunicorn`. Kept separate so the core binding never pulls in Unicorn (matches the
  static-lib decision that leaves the emulator out of `libasmtest.a`).
- Platform soname/install-name handling (`-soname` on ELF, `-install_name` on
  Mach-O), versioned filenames, and `make install` extended to place them under
  `$(libdir)` next to `libasmtest.a`. Extend `asmtest.pc` with a shared variant.

### 0.2 A stable, generation-friendly "binding ABI" *(done)*

- **Designate the contract symbols.** Document which functions/structs/constants
  are the public binding ABI (the capture + emulator entry points, `regs_t`,
  `emu_*`, sentinels, flag masks, guard-alloc, RNG). Keep them free of cpp-macro-only
  behaviour so generators can see them.
- **Provide array-form entry points** for everything currently reachable only
  through a variadic macro, so no binding has to emulate cpp expansion (the
  `_args`/`_fp_n`/`_vec_n` functions already do this; audit for gaps).
- **Optional non-jumping verdict shims** for the "validate across the FFI" mode:
  `int asmtest_check_abi(const regs_t*, char* msg, size_t n)` and friends that
  *return* pass/fail + a message instead of `longjmp`-ing into the runner and
  printing TAP. Tier-1 bindings validate host-side and won't need these, but they
  make a C-side validate call possible for those who want both calls in the library.

### 0.3 Per-architecture layout contract *(done)*

- Emit a **machine-readable layout manifest** (a generated JSON/header listing
  each struct's size, fields, offsets, and the active arch) so bindings can pin
  `regs_t` / `emu_*_regs_t` / `emu_result_t` to the *correct* `#if` branch instead
  of hand-transcribing offsets. A wrong layout silently validates garbage (see the
  analysis's trust-boundary note), so this is the highest-leverage correctness aid.
- Add `_Static_assert`s in C tying each documented offset to `offsetof`, so the
  manifest and the trampoline's stores can never drift apart unnoticed.

### 0.4 Cross-language conformance corpus *(done)*

- A fixed set of canonical routines (from `examples/`) plus a table of **expected
  captures** (return value, flags, callee-saved sentinels, FP/vector lanes, and for
  the emulator: faults, traces, block coverage). Every language binding runs this
  same corpus and must reproduce the same results — one source of truth for "did the
  binding wire the ABI up correctly", reused by all tracks.

### 0.5 CI scaffolding *(done)*

- A reusable CI job template (toolchain install → build the two shared libs → run
  the language's binding tests over the conformance corpus) parameterised per
  language, wired into the existing `{x86-64, AArch64} × {Linux, macOS}` matrix
  where the language toolchain is available.

**Acceptance.** `make shared` / `make shared-emu` produce loadable libraries on all
four native targets; the layout manifest matches `offsetof` under `_Static_assert`;
the conformance corpus + expected-results table exist and the C reference passes
them. *(All three clauses met and verified on x86-64 macOS — `make shared` /
`shared-emu` / `manifest` build, the `_Static_assert`s hold, and `make
conformance` passes 8/8 and emits `corpus.json`; the other three native targets
follow when the 0.5 CI template lands.)*

**Effort:** ~2–3 days. **Touches:** `Makefile`, `asmtest.pc.in`, headers
(`_Static_assert` + optional shims), a small manifest generator, CI templates.

---

## Per-language tracks

Each track delivers, unless noted, the same five things:

- **(a) Bindings** — generated where possible, hand-finished for the per-arch
  layout and any verdict shims.
- **(b) Trampoline coverage** — `call` + `capture` across the int / FP / vector /
  args / sret / bigstruct entry points, with a host mirror of `regs_t`.
- **(c) Emulator coverage** — `emu_open/call/close` for every guest (x86-64,
  AArch64, RISC-V, ARM32, Win64), faults surfaced as data, trace/coverage exposed.
- **(d) Memory/pinning handling** — the language's correct way to share a buffer by
  pointer (Finding 4), plus a wrapper over `asmtest_guarded_alloc`.
- **(e) Runner integration + packaging** — plug into the native test runner and
  publish to the language's registry.

### Track P — Python *(first; proves the substrate) — Tier 1 done*

**Status: Tier 1 binding landed** in [`bindings/python/`](../../bindings/python/).
A **pure-ctypes** package (no `cffi`/compile step) loads the shared library and
the `asmtest_abi.json` manifest, then exposes `capture()` / `capture_fp()` /
`capture_vec()` returning a `Regs` snapshot (`ret`, `flags`, `fret`, vector
lanes, `abi_preserved` via the native verdict shim, `flag_set` via the manifest
masks) and an `Emulator` context manager whose `EmuResult` surfaces faults as
data. `make python-test` builds the shared libs + manifest + corpus + a routine
fixture lib and runs pytest; [`tests/test_conformance.py`](../../bindings/python/tests/test_conformance.py)
replays the same `corpus.json` the C reference emits and reproduces all 8 cases
(10/10 green on x86-64 macOS), proving the substrate end to end from another
language. The CI runs it (see the `bindings` matrix). A `pyproject.toml` packages it. **Tier 2
landed**: `asmtest.assertions` (`assert_ret`, `assert_abi_preserved`,
`assert_flag`, `assert_fp`, `assert_vec_f32`, `assert_no_fault`, `assert_reg`, …)
with `tests/test_assertions.py` covering both the pass paths and that the failure
paths raise. Deferred: cffi API-mode and wheels (`cibuildwheel`/PyPI). The ctypes
path consuming the manifest is the layout chosen below; the rest of the table is
the eventual target.

| Aspect | Approach |
|---|---|
| Binding tool | `cffi` API-mode (parses the binding-ABI header directly); `ctypes` fallback |
| Layout | consume the Track 0 manifest to declare `regs_t`/`emu_*` per active arch |
| Memory | non-moving refcounted heap → `bytearray`/`memoryview`/`numpy` buffers are stable; thin `guarded_alloc` wrapper |
| Runner | `pytest`; expose `capture()` / `emulate()` returning rich result objects; assertions are plain `assert` + helper predicates |
| Packaging | wheel on PyPI bundling the prebuilt shared libs per platform tag (`cibuildwheel`) |
| Crash safety | document the emulator-or-`multiprocessing` rule for untrusted routines |

**Why first.** Lowest friction → it validates Track 0 end to end (shared-lib build,
manifest-driven layout, all three call paths, conformance corpus). The reference
binding other tracks copy.

**Effort:** ~3–4 days (incl. shaking out Track 0).

### Track R — Rust *(second; proves no-GC + auto-generation) — Tier 1 done*

**Status: Tier 1 binding landed** in [`bindings/rust/`](../../bindings/rust/). A
no-crates-io crate: `#[repr(C)]` mirrors of `regs_t` / the emulator structs
(arch-selected via `cfg`) plus `extern "C"` declarations of the binding-ABI entry
points, linked against the prebuilt shared libs by `build.rs`. `#[repr(C)]` makes
the layout match the C structs by construction; the no-GC model makes
pointer-sharing trivial and lifetime-checked. Exposes `capture` / `capture_fp` /
`capture_vec` → `Regs`, `abi_preserved` (native verdict shim), and an `Emulator`
whose `EmuResult` carries faults as data. `make rust-test` builds the libs + the
routine fixture and runs `cargo test`;
[`tests/conformance.rs`](../../bindings/rust/tests/conformance.rs) replays the
corpus (8/8, verified in the isolated `asmtest-rust` Docker image). **Tier 2
landed**: assertion methods on `Regs` / `EmuResult` (`assert_ret`,
`assert_abi_preserved`, `assert_flag`, `assert_fp`, `assert_no_fault`,
`assert_reg`, …) with `tests/assertions.rs` covering the pass paths and
`#[should_panic]` failure paths. Deferred: a `bindgen`-generated `-sys` crate and
crates.io packaging. The table below is the eventual target; the shipped Tier 1
is the hand-written FFI above.

| Aspect | Approach |
|---|---|
| Binding tool | `bindgen` over the binding-ABI header, arch selected via `cfg`; `cc`/build script to locate the libs |
| Layout | `bindgen` generates the structs; verify against the manifest in a build-time test |
| Memory | no GC → `&[u8]`/`&mut [u8]` → pointer is trivial and lifetime-checked |
| Runner | `cargo test`; a safe wrapper crate over the `-sys` crate, results as typed structs/enums (faults as `Result`/enum) |
| Packaging | `asm-test-sys` + `asm-test` crates on crates.io; `build.rs` builds or links the shared libs |
| Crash safety | offer an emulator-backed safe API; native path marked `unsafe` |

**Effort:** ~3–4 days.

### Track G — Go *(situational; highest native friction) — planned*

| Aspect | Approach |
|---|---|
| Binding tool | `cgo` |
| Layout | mirror structs from the manifest; cgo cannot see cpp `#define`s, so generate Go consts for sentinels/flag masks |
| Memory | **moving stacks + cgo pointer rules** — prefer the **emulator path** (staged bytes via `emu_write`/`emu_read`, disjoint memory); for the native path, require `runtime.Pinner` and document the cgo pointer-passing constraints |
| Runner | `go test`; results as structs, faults as `error` |
| Packaging | a Go module; build tags + `cgo` `LDFLAGS` to find the libs |
| Crash safety | lean on the emulator; native crashes kill the test binary |

**Note/risk.** Go is the worst native fit (per the analysis); position the emulator
tier as the primary Go entry point and the native trampoline as advanced/pinned use.

**Effort:** ~4–5 days (cgo pointer-rule care).

### Track Z — Zig *(situational; lowest-ceremony native) — Tier 1 done*

**Status: Tier 1 binding landed** in [`bindings/zig/`](../../bindings/zig/). The
lowest-ceremony binding, exactly as planned: `@cImport` translates the C headers
directly — no separate binding layer, no generated code — yielding the structs,
the binding-ABI functions, and the flag-mask constants;
[`src/conformance.zig`](../../bindings/zig/src/conformance.zig) replays the
corpus through them (8/8, verified in the `asmtest-bindings` Docker image).
[`build.zig`](../../bindings/zig/build.zig) (Zig 0.13.x) links the shared libs
and takes `-Dincdir=` / `-Dlibdir=`. Deferred: a `build.zig.zon` package export
and a Tier-2 assertion layer.

| Aspect | Approach |
|---|---|
| Binding tool | `@cImport` the binding-ABI header directly — no separate binding layer |
| Layout | C import yields the structs; arch handled by the Zig target |
| Memory | manual allocators, no GC → trivial; align via allocator |
| Runner | `zig test` |
| Packaging | Zig package; `build.zig` links the shared (or static) libs; excellent cross-compile for the multi-arch story |
| Crash safety | emulator-or-explicit-isolation, as elsewhere |

**Effort:** ~2–3 days.

### Track X — C++ *(nearly free) — done*

**Status: done.** The C headers now carry `extern "C"` guards (and a portable
`ASMTEST_STATIC_ASSERT`), so a C++ TU both compiles and **links** against the
framework. [`bindings/cpp/asmtest.hpp`](../../bindings/cpp/asmtest.hpp) adds an
RAII `Emu`, initializer-list `capture` / `capture_fp` / `capture_vec`, vector-lane
helpers, and `abi_preserved` / `flag_set` predicates;
[`test_cpp.cpp`](../../bindings/cpp/test_cpp.cpp) drives the framework from C++
(`make cpp-test`, 6/6; also run in the Docker bindings image). Consumption is via
the existing `pkg-config` (and CMake's `pkg_check_modules`). Tier-2 GoogleTest/
Catch2 example suites are an optional follow-on.

| Aspect | Approach |
|---|---|
| Binding tool | none — `#include "asmtest.h"` / `asmtest_emu.h` directly (C headers are C++-compatible) |
| Deliverable | thin RAII/typed conveniences (an `emu_t` guard, `std::span` buffer overloads, typed result accessors) and GoogleTest/Catch2/doctest example suites |
| Memory | native; `std::vector`/`std::array` buffers |
| Packaging | header-only convenience layer + CMake `find_package`/pkg-config consumption |

**Note.** Already works today with zero FFI; the track is just ergonomics +
examples + a CMake consumption path, not a binding.

**Effort:** ~1–2 days.

### Track N — Node / TypeScript *(demand-driven) — Tier 1 done*

**Status: Tier 1 binding landed** in [`bindings/node/`](../../bindings/node/),
via [`koffi`](https://koffi.dev). It calls the opaque-handle FFI layer
(`asmtest_corpus_routine` + `asmtest_capture6`/`_fp2` + `asmtest_regs_*` +
`asmtest_emu_call2`), so no struct layout is mirrored in JS.
[`conformance.js`](../../bindings/node/conformance.js) replays the corpus (7/7,
verified in the isolated `asmtest-node` image; `make node-test` / `docker-node`).
Deferred: an npm package + a `vitest` Tier-2 layer.

| Aspect | Approach |
|---|---|
| Binding tool | `koffi` (or N-API addon) over the shared libs; TypeScript typings from the manifest |
| Memory | `ArrayBuffer`/`Buffer`; moving GC → use koffi's pinned/registered buffers |
| Runner | `vitest`/`jest`; results as typed objects, faults as thrown errors or result enums |
| Packaging | npm package with prebuilt binaries (`prebuildify`/`node-gyp` fallback) |

**Effort:** ~3–4 days.

### Track J — Java / Kotlin *(demand-driven) — Tier 1 done*

**Status: Tier 1 binding landed** in [`bindings/java/`](../../bindings/java/),
via the Foreign Function & Memory API (Project Panama). Downcall handles target
the opaque-handle FFI layer — no struct layout mirrored.
[`Conformance.java`](../../bindings/java/Conformance.java) replays the corpus
(7/7, verified in the isolated `asmtest-java` image; `make java-test` /
`docker-java`). FFM is preview in JDK 21 (`--enable-preview`), stable in 22+.
Deferred: a Maven/Gradle artifact, `jextract`, and a JUnit Tier-2 layer.

| Aspect | Approach |
|---|---|
| Binding tool | **FFM / Project Panama** (`java.lang.foreign`), `jextract` over the header |
| Memory | off-heap `MemorySegment` (no pinning needed) or pinned arrays; `Arena` for lifetime |
| Runner | JUnit 5; results as records, faults as exceptions or sealed types |
| Packaging | Maven/Gradle artifact bundling native libs per classifier |

**Effort:** ~4–5 days.

### Track D — C# / .NET *(demand-driven) — Tier 1 done*

**Status: Tier 1 binding landed** in [`bindings/dotnet/`](../../bindings/dotnet/),
via P/Invoke (`DllImport`) over the opaque-handle FFI layer — no struct layout
mirrored. [`Program.cs`](../../bindings/dotnet/Program.cs) replays the corpus
(7/7, verified in the isolated `asmtest-dotnet` image; `make dotnet-test` /
`docker-dotnet`). Deferred: a NuGet package with `runtimes/<rid>/native/`
payloads and an xUnit Tier-2 layer.

| Aspect | Approach |
|---|---|
| Binding tool | P/Invoke (`DllImport`) / `LibraryImport` source generator; structs from the manifest |
| Memory | `fixed` / `GCHandle.Alloc(Pinned)` / `Memory<T>` pinning |
| Runner | xUnit/NUnit; results as structs, faults as exceptions or result enums |
| Packaging | NuGet with `runtimes/<rid>/native/` payloads |

**Effort:** ~3–4 days.

### Track C — Ruby & Lua/LuaJIT *(community tier) — Tier 1 done*

**Status: Tier 1 bindings landed** in [`bindings/ruby/`](../../bindings/ruby/)
(stdlib `Fiddle`, no gem) and [`bindings/lua/`](../../bindings/lua/) (LuaJIT
`ffi`), both over the opaque-handle FFI layer — no struct layout mirrored. Each
replays the corpus (7/7, verified in the isolated `asmtest-ruby` / `asmtest-lua`
images; `make ruby-test` / `lua-test` / `docker-ruby` / `docker-lua`). Deferred:
a gem / LuaRocks rock and Tier-2 (`minitest` / `busted`) layers.

| Aspect | Approach |
|---|---|
| Binding tool | Ruby `Fiddle`/`ffi` gem; LuaJIT `ffi.cdef` over the binding-ABI header |
| Memory | Ruby: `String`/`Fiddle::Pointer`; LuaJIT: `ffi.new` arrays (no GC moves for FFI cdata) |
| Runner | RSpec/minitest; busted (Lua) |
| Packaging | a gem; a LuaRocks rock |

**Note.** Niche audiences; ship only on concrete demand. LuaJIT's `ffi.cdef` can
consume the binding-ABI header almost verbatim, making it cheap if wanted.

**Effort:** ~2–3 days combined.

---

## Cross-cutting concerns

- **Per-arch layout** is the recurring correctness hazard. Every track binds against
  the Track 0 manifest (0.3), never hand-transcribed offsets, and runs the shared
  conformance corpus (0.4).
- **Fault/result mapping.** Standardise how each language surfaces an emulator fault
  (`faulted`/`fault_addr`/`fault_kind`) — typically the language's idiomatic error
  type — and how a *native* crash is reported (process death vs. emulator-as-data vs.
  host subprocess), so the docs give one consistent crash-safety story.
- **Pinning matrix.** Maintain a short per-language table of the correct
  buffer-pinning idiom (from Finding 4), referenced from each track.
- **Packaging.** Each ecosystem ships the prebuilt shared libs per platform
  (PyPI wheels, crates.io + build script, npm prebuilds, Maven classifiers, NuGet
  rids, gems, rocks); a binding must not require the end user to `make` the C core.
- **Versioning.** Bindings pin to `ASMTEST_VERSION_NUM`; the manifest carries the
  version so a mismatched lib is detected at load.
- **Reproducible, isolated per-language testing.** Each wrapper is tested in its
  **own** image — `bindings/<lang>/Dockerfile` FROM a shared C+libunicorn base
  (`Dockerfile.bindings-base`) — so toolchains never mix (npm never lands in the
  Java image, the .NET SDK never in the Lua image, …). `make docker-<lang>`
  builds the base once (cached) then the small per-language image and runs the
  conformance corpus inside it; `make docker-bindings` does all nine. The CI
  `bindings` job is a matrix that runs `make docker-<lang>` per language. This is
  also how languages with no host toolchain are verified.

---

## Suggested sequencing

1. **Track 0** — the shared substrate; gates everything.
2. **Track P (Python)** — proves Track 0 end to end; reference binding.
3. **Track R (Rust)** — proves auto-generation + the no-GC path.
4. **Track X (C++)** + **Track Z (Zig)** — cheap, native-friendly, broaden reach.
5. **Track G (Go)** — once the emulator-first pattern is proven (its safest path).
6. **Tracks N / J / D / C** — demand-driven, in any order.

Tier 2 (idiomatic assertion layers) is a per-language follow-on, picked up after
that language's Tier-1 binding is green against the conformance corpus.

**Status: Tier 2 has landed for all nine bindings** (Python, Rust, C++, Zig,
Node, Java, .NET, Ruby, Lua) — assertion helpers with legible failure messages
(`assert_ret` / `assert_abi_preserved` / `assert_flag` / `assert_fp` /
`assert_no_fault` / `assert_reg`, idiomatic to each language: raising/throwing,
panicking, or error-union), each verified on both the pass paths and the failure
paths (the assertion actually fails when it should). The remaining deferred work
is packaging each binding for its registry.

---

## Acceptance criteria (overall)

- `make shared` / `make shared-emu` build loadable libraries on all four native
  targets, with a layout manifest verified by `_Static_assert`.
- For each shipped language: the conformance corpus passes (trampoline **and**
  emulator paths), a buggy routine's fault is observed as **data** via the emulator
  without crashing the host, and the binding installs from the language's registry
  with the native libs bundled.
- A throwaway external project in each shipped language can build a passing suite
  using only the published package — the multi-language analog of the existing
  pkg-config acceptance test.

## Risks

- **Layout drift** between a binding and the C structs — mitigated by the manifest +
  `_Static_assert` (0.3) and the conformance corpus (0.4).
- **Moving-GC languages** (Go, JVM, .NET, JS) make native pointer-sharing
  error-prone — mitigated by leading with the emulator path and a documented pinning
  idiom per language.
- **Packaging native binaries** across OS/arch is per-ecosystem toil — contained by
  reusing each ecosystem's standard prebuild tooling and the shared CI template (0.5).
- **Unicorn availability** on a given platform/arch gates the emulator path — the
  binding degrades to the trampoline path where Unicorn is absent, mirroring the
  existing optional-emulator stance.

## Out of scope (for now)

- Tier 3 (porting the runner/discovery into another language).
- New guest architectures beyond the existing four (deferred in
  [expansion-plan.md](expansion-plan.md)).
- A native Win64 trampoline (Win64 stays emulator-only; see Track E.1 in
  [expansion-plan.md](expansion-plan.md)).
