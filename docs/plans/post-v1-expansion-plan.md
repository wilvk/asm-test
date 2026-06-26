# asm-test — Post-v1.0 Expansion Plan

A roadmap for what comes *after* the v1.0.0 feature set. The five prior plans —
[DESIGN.md](../../DESIGN.md) (phases 0–11), [expansion-plan.md](expansion-plan.md)
(tracks A–E), [multi-language-bindings-plan.md](multi-language-bindings-plan.md)
(Track 0 + ten languages), [binding-parity-plan.md](binding-parity-plan.md), and the
[Native Win64 tier plan](win64-native-tier-plan.md) — are all **landed**. They
widened *what can be tested*, *how the runner behaves*, and *who can call it*. This
plan covers the two directions left: **reach** (turning a built framework into a
published, installable one) and **depth** (new introspection capability that plays to
the project's unique strength — calling assembly through the real ABI / a virtual CPU
and inspecting the result).

Tracks are grouped into **Part 1 — Reach** (finish what the prior plans explicitly
deferred; known scope) and **Part 2 — Depth** (genuinely new capability, grounded in
verified gaps in the current code). Within each part they are ordered by
value-to-effort; all are independent and can land in any order.

> Status legend: **planned** unless noted. Update this file as tracks land, the way
> DESIGN.md and [expansion-plan.md](expansion-plan.md) track their phases.

---

## Context: current state (as of the v1.0.0 + Unreleased commits)

- ~5,600 lines of C + assembly; 95 Makefile targets; full Sphinx docs; `v1.0.0`
  tagged.
- Native capture tier (int/FP/vector/struct/sret) on x86-64 + AArch64, Linux + macOS,
  GAS + NASM; a native Win64 capture tier (`--no-fork`) under Wine/`ms_abi`.
- Unicorn emulator tier: x86-64, AArch64, RISC-V, ARM32 guests + Win64 ABI;
  instruction trace and basic-block coverage; Keystone in-line assembler tier.
- Ten language bindings (Python, Rust, C++, Zig, Node, Java, .NET, Ruby, Lua, Go),
  Tier 1 + Tier 2, each on a shared conformance corpus, each in its own Docker image.

### Gaps this plan closes

| # | Part | Gap | Symptom |
|---|---|---|---|
| 1 | Reach | Bindings are packaged but **unpublished** | No `pip install` / `cargo add` / `npm i`; only registry *manifests* exist. Adoption still means building the C core. |
| 2 | Reach | Win64 tier is `--no-fork` only | No per-test isolation / `-jN` / benchmarks on Win64; no authoritative `windows-latest` sign-off. |
| 3 | Depth | Diagnostics are **raw byte offsets** | A fault, trace, or uncovered block prints `@0x2f`, not the instruction. The emulator's natural disassembler companion (Capstone) is unused. |
| 4 | Depth | Vectors are **128-bit only** | `vec128_t` / `ASM_VCALLn` cover `xmm`/`q` (128-bit); AVX/AVX-512 (`ymm`/`zmm`) and SVE routines cannot have their full vector state captured. |
| 5 | Depth | Coverage is collected but not **fed back** | The emulator records basic-block coverage — the exact signal a coverage-guided fuzzer needs — but it only ever feeds a report, never test generation. |
| 6 | Depth | The emulator can't assert **mid-execution invariants** | Introspection stops at the result struct; there are no "never writes outside region" / register-invariant guards, despite the hooks being present. |

---

# Part 1 — Reach (finish the deferred work)

## Track A — Publish the bindings *(planned)*

**Goal.** Turn the ten packaging-*scaffolded* bindings into ones a user installs from
their language's registry, with the native libraries bundled — the multi-language
analog of the existing `pkg-config` adoption story.

**Why.** This is the largest gap between "built" and "adoptable." The framework is
done; nobody can `pip install asmtest`. [docs/packaging.md](../packaging.md) already
scopes the remainder: credentials and cross-OS native-payload build matrices.

### Deliverables

1. **Per-ecosystem prebuilds** of `libasmtest`(`_emu`) for each published platform
   tag — PyPI wheels via `cibuildwheel`, npm prebuilds (`prebuildify`), Maven
   classifiers, NuGet `runtimes/<rid>/native/`, a gem, a LuaRocks rock, and the
   source-shipping link bindings (Rust/Zig/C++/Go) wired to build-or-locate the libs.
2. **A cross-OS/arch release matrix** (reusing the per-language Docker images) that
   produces those payloads for `{x86-64, AArch64} × {Linux, macOS}` (+ Windows where
   the binding supports it).
3. **Registry publication** behind credentialed CI, version-pinned to
   `ASMTEST_VERSION_NUM` (the manifest already carries the version for load-time
   mismatch detection).

### Acceptance criteria

- A throwaway project in each shipped language installs the binding from its registry
  (no `make` of the C core) and passes the conformance corpus.

**Effort:** ~3–5 days (mostly per-ecosystem CI toil). **Touches:** `.github/workflows/`,
the per-language `Dockerfile`s, the registry manifests, [docs/packaging.md](../packaging.md).

---

## Track B — Win64 isolation, parallelism & sign-off *(planned)*

**Goal.** Bring the Win64 tier up to the POSIX runner's guarantees and add
authoritative real-OS confirmation.

**Why.** The [Win64 plan](win64-native-tier-plan.md) shipped the trampoline, layout,
and an in-process runner under Wine, but deferred forked/`-jN` execution and
benchmarks behind its Phase 3 decision gate. The primitives (`asmtest_win32_run`,
`asmtest_win32_run_pool`) already exist and are tested; what remains is wiring a
re-exec-per-test model into the runner and an optional `windows-latest` job.

### Deliverables

1. **Forked-equivalent isolation + `-jN`** on Win64 via the existing
   `CreateProcess` / `WaitForMultipleObjects` pool, so a crashing test is contained
   the same way it is on POSIX.
2. **Benchmark mode** on Win64 (currently POSIX-only).
3. **Optional `windows-latest` CI** running the same suite for real-OS sign-off,
   kept thin since Wine carries the bulk (the Wine-fidelity caveat in
   [docs/win64.md](../win64.md)).

### Acceptance criteria

- `make win64-check` exercises isolated + `-jN` execution and a benchmark under Wine;
  the optional `windows-latest` job is green.

**Effort:** ~1 week (the re-exec model) + ~0.5 day (`windows-latest`). **Touches:**
`src/asmtest.c`, `src/platform_win32.c`, `.github/workflows/ci.yml`,
[docs/win64.md](../win64.md).

---

# Part 2 — Depth (new introspection capability)

## Track C — Disassembly in diagnostics (Capstone) — **done**

**Goal.** Make every emulator diagnostic show the **instruction**, not a raw byte
offset. Add [Capstone](https://www.capstone-engine.org/) — the disassembler
counterpart to the Keystone assembler already integrated — as an optional companion
to Unicorn.

**Landed.** `src/disasm.c` (its own translation unit, like `assemble.o`, so the core
build stays Capstone-free) adds `emu_disas` (one instruction at an offset, PC-relative
targets resolved), `emu_fault_describe` (fault line naming the offending instruction),
and `emu_trace_disasm` / `emu_trace_report_disasm` / `emu_coverage_uncovered_disasm`
(the reporters, with each block/insn annotated). All four guests; `emu_arch_t` selects
the guest. **Auto-detected** via `pkg-config` and gated by `-DASMTEST_HAVE_CAPSTONE`:
every helper degrades to bare offsets when Capstone is absent
(`emu_disas_available()`), so the same call works either way. `make deps
DEPS_ARGS=--emu` now installs `libcapstone-dev`, so the CI `emu` job exercises the
annotated path on every matrix OS with no `ci.yml` change. RISC-V needs Capstone ≥ 5
and self-skips otherwise. Tracks E/F can now build on readable diagnostics.

**Why first.** Highest leverage-to-effort in Part 2. Today a fault, an instruction
trace, and an uncovered-block report all print byte offsets only
([asmtest_emu.h](../../include/asmtest_emu.h) trace/coverage; the fault carries just
`fault_addr`/`fault_kind`), even though [asmtest_assemble.h](../../include/asmtest_assemble.h)
notes Unicorn "pairs naturally with" Capstone. It's used nowhere. This turns
`uncovered block @0x2f` into `uncovered: 0x2f  cmp rax, 0` across all four guests, for
a small surface and an already-familiar optional-dependency pattern.

### Deliverables

1. **A disassembly helper** (`emu_disas`/`emu_trace_disasm`) that annotates trace
   entries, uncovered blocks, and the faulting instruction with mnemonic + operands,
   per guest arch.
2. **Richer fault/coverage reports** — `emu_trace_report` / `emu_coverage_uncovered`
   / the fault path gain a disassembled line; the lcov export can carry it as context.
3. **Optional + gated** exactly like the emulator/assembler tiers (`-lcapstone`,
   self-skipping when absent); the core build stays dependency-free.

### Acceptance criteria

- A faulting routine reports the offending instruction text; an uncovered-block report
  lists each block's first instruction. Both degrade to offsets when Capstone is absent.

**Effort:** ~2–3 days. **Touches:** `src/emu.c`, `include/asmtest_emu.h`, `Makefile`,
`examples/test_emu.c`, [docs/emulator.md](../emulator.md).

---

## Track D — Wide-vector capture (AVX/AVX-512, SVE) *(planned)*

**Goal.** Capture vector state wider than 128 bits, so AVX2 / AVX-512 (`ymm`/`zmm`)
and AArch64 SVE routines are testable to their full register width.

**Why.** Vector support is *strictly 128-bit* throughout — `vec128_t`, `xmm0..15` /
`q0..31`, `ASM_VCALLn` ([asmtest.h](../../include/asmtest.h)). Modern SIMD lives at
256/512 bits and in scalable vectors; those routines currently can't have their full
state captured. This is a concrete capability gap, not a long-tail architecture.

### Deliverables

1. **A width-tagged vector type** (`vec256_t`/`vec512_t`, or a sized `vecN_t`) and
   capture variants (`asm_call_capture_vec_wide` / emulator equivalents) that marshal
   and snapshot `ymm`/`zmm` (x86) and SVE `z`/predicate registers (AArch64), with a
   runtime feature probe (CPUID / `getauxval`) so a host without AVX-512/SVE
   self-skips rather than faulting.
2. **Lane assertions** at the new widths (`ASSERT_VEC256_EQ`, `ASSERT_DEQ`/`FEQ` over
   the wider lane counts) plus manifest/`_Static_assert` pins for the new layout.
3. **Emulator parity** where Unicorn exposes the registers; documented self-skip
   where it doesn't (mirroring the RISC-V "V" decision in
   [binding-parity-plan.md](binding-parity-plan.md)).

### Acceptance criteria

- An AVX2 `ymm` routine's full 256-bit result is captured and lane-asserted natively;
  the path self-skips cleanly on a host/guest lacking the feature.

### Notes / risk

- The biggest design choice is the type model (fixed `vec256/512` vs. a sized type);
  decide it before pinning the manifest, since it is part of the public contract.
- SVE is *scalable* (vector length is implementation-defined) — scope it as a stretch
  behind fixed-width AVX, which is self-contained.

**Effort:** ~4–6 days (AVX) + extra for SVE. **Touches:** `include/asmtest.h`,
`src/capture.s`/`.asm`, `src/emu.c`, `scripts/gen-manifest.c`, the conformance corpus.

---

## Track E — Coverage-guided fuzzing & mutation testing *(planned)*

**Goal.** Close the loop between the emulator's basic-block coverage and the
property-testing generator: let coverage *drive* input generation, and add mutation
testing that proves a suite catches a perturbed routine.

**Why.** The emulator already records basic-block coverage — the exact signal a
coverage-guided fuzzer consumes — but it only ever feeds a report
([expansion-plan.md](expansion-plan.md) Track C). Meanwhile Keystone can assemble
*mutants* of a routine. Wiring coverage back into generation (Phase 7's
`ASSERT_MATCHES_REF` RNG) and adding instruction-level mutation reuses two systems
already built and differentiates the framework further.

### Deliverables

1. **Coverage-guided generation** — a generator mode that keeps inputs which expand
   the accumulated block-coverage union (a lightweight in-tree loop), reported
   alongside the existing differential mismatch output.
2. **Mutation testing** — perturb the routine's bytes (via the assembler / a
   byte-flip set), re-run the suite, and report mutants the suite *failed* to catch
   (surviving mutants = test-gap signal), all inside the emulator so a broken mutant
   can't crash the host.
3. *(Optional)* a **libFuzzer/AFL harness shim** exposing the emulator's coverage as
   the fuzzer's feedback channel, for users who want an external engine.

### Acceptance criteria

- A weak suite over the `classify` example reports surviving mutants; a
  coverage-guided run reaches blocks a fixed-vector run misses.

**Effort:** ~4–6 days. **Touches:** `src/emu.c`, `include/asmtest.h` (RNG/property
layer), `src/assemble.c`, `examples/`.

---

## Track F — Emulator invariant & watchpoint assertions — **done**

**Goal.** Assert properties *during* a routine's execution, not just on its result —
the introspection no ABI-boundary tool can do.

**Why.** The emulator owns the hooks (`UC_HOOK_MEM_*`, `UC_HOOK_CODE`) but exposes
only post-run state and faults-as-data. Mid-execution guards play directly to its
unique strength and are a small surface on top of existing infrastructure.

**Landed.** Guards are armed on the emu handle (`struct emu` gained `watch` / `reg`
contexts) and persist across `emu_call_*` until cleared, each recording the first
violation as data via a new `UC_HOOK_MEM_WRITE` / second `UC_HOOK_BLOCK` hook in
`emu_x86_run` (x86-64 guest):

1. **Memory-write watchpoints** — `emu_watch_writes(e, addr, size, mode, &w)` with
   `EMU_WATCH_ONLY` / `EMU_WATCH_NEVER`; `ASSERT_NO_WRITE_VIOLATION` /
   `ASSERT_WRITE_VIOLATION`. Catches a *logical* write into mapped memory that does
   not fault, naming the offending store via `emu_watch_describe` (reuses the
   Track C disassembler — pairs as planned).
2. **Register invariants** — `emu_guard_reg(e, "rbx", want, &g)` /
   `ASSERT_REG_INVARIANT`, checked at every basic-block entry; catches mid-routine
   corruption even when restored by return.
3. **Step-bounded** — no new API: `max_insns=N` (the single-step path) +
   `ASSERT_EMU_REG_EQ` on `out->regs` asserts a condition at instruction N
   (documented).

Three host-independent example tests in `examples/test_emu.c`; surfaced the real
property that the engine **retains register state across calls** on a handle (only
args + `rsp` reset). Acceptance met: a routine writing past its confined region is
caught at the offending store with its instruction text, no host crash.

**Effort:** ~3–4 days. **Touches:** `src/emu.c`, `include/asmtest_emu.h`,
`src/disasm.c` (the describe helper, kept Capstone-side), `examples/test_emu.c`,
[docs/emulator.md](../emulator.md).

---

## Suggested sequencing

1. **Track C** (disassembly) — **done.** Cheap, transformed every diagnostic, and
   Tracks E/F can build on it.
2. **Track A** (publish) — if reach is the priority, this is the single biggest
   adoption lever; it has no dependency on the others.
3. **Track D** (wide vectors) — closes a concrete modern gap in the capture model.
4. **Track F** (invariants) — **done.** Small, compounded with Track C (the
   offending store is disassembled).
5. **Track E** (fuzzing/mutation) — the deepest engineering; C and F have landed.
6. **Track B** (Win64 isolation) — opportunistic, on concrete demand for isolated
   Win64 execution (its Phase 3 gate already deemed `--no-fork` a shipping milestone).

## Out of scope (for now)

- **New guest architectures** beyond the existing four (MIPS, PowerPC, s390x, RV32,
  WASM, LoongArch). MIPS would close the MIPSUnit prior-art gap in
  [DESIGN.md](../../DESIGN.md) §1 and a WASM guest would be novel, but both are
  diminishing-returns vs. effort — reconsider only on concrete demand, as
  [expansion-plan.md](expansion-plan.md) already records.
- A GUI/TUI front-end (TAP + JUnit already integrate with standard tooling).
- Rewriting the C + asm core in another language (wrapping it is the bindings story).
- Tier 3 bindings (porting the runner/discovery into another language).
