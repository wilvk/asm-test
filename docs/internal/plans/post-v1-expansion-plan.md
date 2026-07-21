# asm-test — Post-v1.0 Expansion Plan

A roadmap for what comes *after* the v1.0.0 feature set. The five prior plans —
[DESIGN.md](../../../DESIGN.md) (phases 0–11), [expansion-plan.md](../archive/plans/expansion-plan.md)
(tracks A–E), [multi-language-bindings-plan.md](../archive/plans/multi-language-bindings-plan.md)
(Track 0 + ten languages), [binding-parity-plan.md](../archive/plans/binding-parity-plan.md), and the
[Native Win64 tier plan](../archive/plans/win64-native-tier-plan.md) — are all **landed**. They
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
> DESIGN.md and [expansion-plan.md](../archive/plans/expansion-plan.md) track their phases.

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
| 1 | Reach | Bindings are packaged but **unpublished** | No `pip install` / `cargo add` / `npm i` yet. The release pipeline is dry-run-complete (packages build, install, and smoke-test in CI); only the credentialed go-live remains, so adoption still means building the C core for now. |
| 2 | Reach | Win64 tier is `--no-fork` only | No per-test isolation / `-jN` / benchmarks on Win64; no authoritative `windows-latest` sign-off. |
| 3 | Depth | Diagnostics are **raw byte offsets** | A fault, trace, or uncovered block prints `@0x2f`, not the instruction. The emulator's natural disassembler companion (Capstone) is unused. |
| 4 | Depth | Vectors are **128-bit only** | `vec128_t` / `ASM_VCALLn` cover `xmm`/`q` (128-bit); AVX/AVX-512 (`ymm`/`zmm`) and SVE routines cannot have their full vector state captured. |
| 5 | Depth | Coverage is collected but not **fed back** | The emulator records basic-block coverage — the exact signal a coverage-guided fuzzer needs — but it only ever feeds a report, never test generation. |
| 6 | Depth | The emulator can't assert **mid-execution invariants** | Introspection stops at the result struct; there are no "never writes outside region" / register-invariant guards, despite the hooks being present. |

---

# Part 1 — Reach (finish the deferred work)

## Track A — Publish the bindings — **pipeline was BROKEN, not merely uncredentialed; four defects fixed 2026-07-17, go-live still needs credentials**

**Goal.** Turn the ten packaging-*scaffolded* bindings into ones a user installs from
their language's registry, with the native libraries bundled — the multi-language
analog of the existing `pkg-config` adoption story.

**Why.** This is the largest gap between "built" and "adoptable." The framework is
done; nobody can `pip install asmtest`. [docs/packaging.md](../../reference/packaging.md) already
scopes the remainder: credentials and cross-OS native-payload build matrices.

**Status — corrected 2026-07-17.** This entry previously read "code-complete; blocked
on registry credentials" and claimed the pipeline was "dry-run-proven end to end".
**That was false.** `release.yml` had not been dispatched since **2026-06-26**, so
everything merged after that date entered it **untested** — including the commit that
added the native-trace bundling assert (`53fc151`, 2026-07-01), which had therefore
**never once executed**. A re-dispatch on 2026-07-17 found the pipeline broken in four
independent ways: two workflow-config faults and two real defects in shipped code.

The credentials gate is real, but it was **never the only gate** — and describing it as
such is what let a dead pipeline read as "done" for three weeks. The lesson is
structural, not incidental: **a release workflow that only runs on dispatch decays
silently.** Nothing merged between 2026-06-26 and 2026-07-17 was release-tested.

> **Pointer (2026-07-21):** the go-live remainder is specified in
> [distribution-packaging.md](../implementations/distribution-packaging.md)
> (◐ 7/13 live; its T1–T6 are the credential-gated registry publishes).

| # | Defect | Kind | Why it hid |
|---|--------|------|-----------|
| 1 | `libipt-dev` installed unguarded on arm64 (no such package on Ubuntu ports) | workflow config | the arm64 leg is release-only |
| 2 | `macos-13` runners retired 2025-12-08 — every Intel-mac leg queued until timeout | workflow config | no runner ⇒ the leg never reports; migrated to `macos-15-intel` (the last x86_64 image, available until 2027-08) |
| 3 | `go vet` failed at 16 sites ("possible misuse of unsafe.Pointer") | real code defect | **`go vet` runs ONLY in release.yml, never in ci.yml**, and `go-version: "stable"` tracks an ever-stricter Go. The rule tightened under a lane nobody ran. |
| 4 | The Linux wheel's **drtrace tier could never load** (`undefined symbol: emu_arm64_call`) | real packaging defect | see below — three compounding causes, and every existing check passed anyway |

Defect 4 is the instructive one. The tier *was* bundled; it just could not be
`dlopen`'d, and **nothing in the pipeline could tell the difference**:

- `package-libs-drtrace` inherited `DRAPP_KEYSTONE=1`, which `mk/native-trace.mk`
  already documents as unloadable (a Keystone-enabled drapp has unresolved `emu_*`
  symbols). Every per-lane *test* target passes `DRAPP_KEYSTONE=0`; the *packaged*
  payload did not.
- `package-native.sh` asserted only that drapp **exports** `asmtest_dr_available`, and
  `package-libs-verify` only that the file exists with an `$ORIGIN` rpath. A broken
  drapp satisfies both: **an export assertion is not a loadability assertion.**
- `package-libs-tiers` chained its sub-make with `;`, discarding the exit status — so
  even a fatal tier failure left `package-libs` **green**. This is why the `native`
  legs happily staged a dead payload and passed.

The tier is therefore fixed at the source (`DRAPP_KEYSTONE=0`) and, more importantly,
made **unable to regress silently**: an `ldd -r` gate now fails the staging step, and
`&&` lets that failure propagate. Self-skipping is unaffected (`package-libs-drtrace`
already self-skips by exiting 0 from inside the rule).

The clean-room assert added by `53fc151` was itself wrong on arm64 in two ways, both
masked because the first killed the leg before the second could run: it called
`drtrace.library_path()` **above its own platform guard**, and it asserted
`hwtrace.HwTrace.available(SINGLESTEP)` unconditionally even though single-step is
x86-64-only (`HWTRACE_HAVE_SINGLESTEP` is gated on `__x86_64__`; on aarch64 the lib
loads and self-skips with "single-step backend is x86-64 Linux/macOS only"). It now
asserts each tier where it ships and asserts its **documented absence** — matched on
the specific skip reason — where it does not, so neither dropping a tier nor silently
starting to ship one can pass.

**drtrace on arm64 — decided, not deferred.** DynamoRIO *does* publish
`DynamoRIO-AArch64-Linux-11.91.20630.tar.gz` for the pinned version, so the engine
exists and the tier is not fundamentally impossible here. It is nonetheless **not
shipped on arm64**, which is the repo's consistent, deliberate scoping
(`scripts/fetch-dynamorio.sh` fetches the x86-64 tarball and says "Linux x86-64 only";
`package-libs-drtrace`, `package-libs-verify`, and `mk/native-trace.mk`'s
`lib64/release` layout all encode linux-x86_64). Enabling it would be a **feature
expansion** — new arch for a whole tier, needing its own digest pin and arm64
validation — not part of repairing the release. Recorded here as a real, evidenced
option rather than an unknown.

**What the pipeline now proves** (all four `native` legs and `native (collect +
verify)` pass; the below is corrected from the prior overclaim):

- Each `make <lang>-package` exposes the **library module** (not the test runner)
  and bundles the native payload; the dlopen bindings **self-locate** it (no
  `ASMTEST_LIB`), so an installed package works out of the box.
- The release workflow builds cross-platform `native-all`, then per binding
  **packages → installs fresh → smoke-tests the bundled-native load → dry-run
  publishes**; the dlopen bindings install-test on Linux **and** macOS. Python is
  per-platform (manylinux / macOS wheels, libunicorn vendored via auditwheel /
  delocate); the link bindings (Go/C++/Zig/Rust) are validated as consumable.
- **What remains for go-live is the credentials** — but "only credentials" is the
  claim that hid four defects for three weeks, so it is worth stating precisely: the
  publish steps are credential-gated, *and* the pipeline must be **re-dispatched and
  green on the run that carries the release**, not on a three-week-old run. Register
  the package names, add the per-ecosystem token secrets, and tag a release — the
  gated publish steps light up automatically. The credentials are:
  `PYPI_TOKEN`, `NPM_TOKEN`, `CARGO_REGISTRY_TOKEN`,
  `RUBYGEMS_API_KEY`, `NUGET_API_KEY` (lua + java have no registry leg here, and
  fall through). Each publish step is `if:`-gated to a **tag build on the Linux
  leg**, then guarded in-step by `[ -n "$TOKEN" ] || skip`, so forks and
  untagged runs no-op — see
  [release.yml](../../../.github/workflows/release.yml) (per-binding publish;
  crates.io on its own job). Per-ecosystem registry quirks (PyPI Trusted
  Publishing, Maven Central signing, broader manylinux/python-version coverage)
  are tuned at that point.

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
the per-language `Dockerfile`s, the registry manifests, [docs/packaging.md](../../reference/packaging.md).

---

## Track B — Win64 isolation, parallelism & sign-off — **done**

**Goal.** Bring the Win64 tier up to the POSIX runner's guarantees and add
authoritative real-OS confirmation.

**Why.** The [Win64 plan](../archive/plans/win64-native-tier-plan.md) shipped the trampoline, layout,
and an in-process runner under Wine, but deferred forked/`-jN` execution and
benchmarks behind its Phase 3 decision gate. The primitives (`asmtest_win32_run`,
`asmtest_win32_run_pool`) already exist and are tested; what remains is wiring a
re-exec-per-test model into the runner and an optional `windows-latest` job.

**Landed.** With no `fork()`, the runner **re-execs itself per test** — a hidden
`--asmtest-child=<global index>` runs one test in-process and writes its
`wire_result_t` to a temp file the parent reads back, driven through
`asmtest_win32_run` / `_run_pool`. Isolation is now the **default** on Win64
(matching POSIX); `--no-fork` selects the in-process facility. A crash is
contained in the child (caught there, or backstopped by its death); a hang is
killed by the parent's deadline; the `-jN` pool runs isolated children
concurrently with output in registration order. `--bench` (rdtsc) now runs on
Win64 (a `BENCH` is trusted, run unguarded). `make win64-runner-test` exercises
**all four modes under Wine** (verified here), and an optional `windows-latest`
CI job signs the same suite off natively with no Wine. The effort was well under
the ~1 week estimate because the primitives already existed — the work was the
re-exec child mode + result plumbing + dispatch wiring.

### Deliverables

1. **Forked-equivalent isolation + `-jN`** on Win64 via the existing
   `CreateProcess` / `WaitForMultipleObjects` pool, so a crashing test is contained
   the same way it is on POSIX.
2. **Benchmark mode** on Win64 (currently POSIX-only).
3. **Optional `windows-latest` CI** running the same suite for real-OS sign-off,
   kept thin since Wine carries the bulk (the Wine-fidelity caveat in
   [docs/win64.md](../../guides/win64.md)).

### Acceptance criteria

- `make win64-check` exercises isolated + `-jN` execution and a benchmark under Wine;
  the optional `windows-latest` job is green.

**Effort:** ~1 week (the re-exec model) + ~0.5 day (`windows-latest`). **Touches:**
`src/asmtest.c`, `src/platform_win32.c`, `.github/workflows/ci.yml`,
[docs/win64.md](../../guides/win64.md).

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
([asmtest_emu.h](../../../include/asmtest_emu.h) trace/coverage; the fault carries just
`fault_addr`/`fault_kind`), even though [asmtest_assemble.h](../../../include/asmtest_assemble.h)
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
`examples/test_emu.c`, [docs/emulator.md](../../guides/emulator.md).

---

## Track D — Wide-vector capture (AVX/AVX-512, SVE) — **AVX2 + AVX-512 done (SysV + Win64, all ten bindings); SVE capture landed + qemu-TCG validated, real-silicon execution sign-off gated**

**Goal.** Capture vector state wider than 128 bits, so AVX2 / AVX-512 (`ymm`/`zmm`)
and AArch64 SVE routines are testable to their full register width.

**Why.** Vector support is *strictly 128-bit* throughout — `vec128_t`, `xmm0..15` /
`q0..31`, `ASM_VCALLn` ([asmtest.h](../../../include/asmtest.h)). Modern SIMD lives at
256/512 bits and in scalable vectors; those routines currently can't have their full
state captured. This is a concrete capability gap, not a long-tail architecture.

**Landed (AVX2, the acceptance).** The **fixed-width** type model (decided over a sized
`vecN_t`, since SVE is a deferred stretch and fixed-width reuses the shipped `vec128_t`
→ manifest → binding idiom): `vec256_t` + `asm_call_capture_vec256` marshal `ymm0..7`
and capture the `ymm` file into a `vec256_t[16]`, with `ASM_VCALL256n` /
`ASSERT_VEC256_EQ` and a CPUID probe (`asmtest_cpu_has_avx2` / `_avx512f`, checking the
feature bit **and** OS `XCR0` enablement) so the path self-skips rather than executing
an unsupported instruction. Trampoline in **both** backends (`capture.s` GAS +
`capture.asm` NASM, `vzeroupper` on exit), manifest + `_Static_assert` pinned, and a
`vec_add4d` AVX2 example asserts the full 256-bit result (upper-128 lane included) on
both — **verified on a real AVX2 host**.

**Done since:** the **Win64 wide path** — `asm_call_capture_vec256_win64` mirrors the
SysV trampoline under the Microsoft x64 ABI (loads `ymm0..3`, captures `ymm0..15`,
saves/restores the callee-saved low 128 of `xmm6..15`, `vzeroupper` on exit), verified
on both the native `ms_abi` lane and the PE/Wine lane (real AVX2 — Wine runs PE
instructions on the host CPU); and **vec256 binding parity** (round 2, `capture_vec256`
across all ten bindings).

**Done since (AVX-512 `zmm`):** native 512-bit capture shipped and is **validated on a
real AVX-512 host** (this Zen 5 / Ryzen 9 9950X, `avx512f`). `vec512_t` (64 bytes) +
`asm_call_capture_vec512` marshal 8 `zmm` args and capture the **full zmm0..31 file** (32
registers — AVX-512 doubles the count) into `vec[0..31]`, in both the GAS and NASM
trampolines and the **Win64** path (`asm_call_capture_vec512_win64`, Wine PE lane); gated
on `asmtest_cpu_has_avx512f()` (CPUID + `XCR0` `0xe6`), with `ASM_VCALL512*` / the per-
binding wrappers self-skipping where AVX-512 is absent. Rolled out across **all ten
bindings** with parity tests. The `vaddpd zmm` corpus routine `vec_add8d` exercises the
8th double lane (the bits neither the 128- nor 256-bit path can see).

**Done since (SVE), 2026-07-19:** AArch64 **SVE** scalable-vector capture shipped.
`svec_t` (256-byte VLmax container) / `spred_t` (32-byte PLmax) with `_Static_assert`
+ manifest pins; `asm_call_capture_sve` marshals `z0..z7` / `p0..p3` per AAPCS64 and
captures the whole `z0..z31` / `p0..p15` file (GAS body in `capture.s`, `ret` stubs in
the NASM twin and on every non-SVE target); a `HWCAP_SVE` runtime probe
(`asmtest_cpu_has_sve` / `asmtest_sve_vl`, 0 where SVE is absent — incl. macOS arm64,
which has no non-streaming SVE); self-skipping `ASM_SVCALL_*`; VL-aware
`ASSERT_SVEC_EQ` / `ASSERT_SPRED_EQ` (compare only the live VL/PL bytes); and the
`sve_addd` corpus routine. **Validated under qemu-user TCG** — the SVE `ptrue`/`fadd`
*actually execute*: `make docker-test DOCKER_PLATFORM=linux/arm64` passes
`simd.sve_adds_doubles_at_any_vl` at the default VL=64 B, and the new
`make docker-sve-sweep` passes it at VL 16/48/128/256 B (VQ 1/3/8/16, including the
non-power-of-two 48 B) by steering `QEMU_CPU=…,sve-max-vq=N,sve-default-vector-length=-1`.
**Still gated:** *execution sign-off on real SVE silicon* (Graviton3/3E/4, NVIDIA
Grace, or an A64FX-class host) — a genuine hardware gate per CLAUDE.md, like Intel PT;
qemu TCG is the best-available validation until then, **not** the silicon sign-off. The
implementation brief and its T1–T8 breakdown live in
[aarch64-sve-capture.md](../implementations/aarch64-sve-capture.md).

The **emulator** wide path is the *documented self-skip* case
(deliverable #3): its bundled Unicorn exposes YMM/ZMM but does **not execute** AVX
(`UC_ERR_INSN_INVALID`, even with an AVX-capable CPU model set), so wide-vector capture
is native-only until Unicorn ships AVX execution.

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
   [binding-parity-plan.md](../archive/plans/binding-parity-plan.md)).

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

## Track E — Coverage-guided fuzzing & mutation testing — **done**

**Goal.** Close the loop between the emulator's basic-block coverage and the
property-testing generator: let coverage *drive* input generation, and add mutation
testing that proves a suite catches a perturbed routine.

**Why.** The emulator already records basic-block coverage — the exact signal a
coverage-guided fuzzer consumes — but it only ever feeds a report
([expansion-plan.md](../archive/plans/expansion-plan.md) Track C). Meanwhile Keystone can assemble
*mutants* of a routine. Wiring coverage back into generation (Phase 7's
`ASSERT_MATCHES_REF` RNG) and adding instruction-level mutation reuses two systems
already built and differentiates the framework further.

**Landed.** A new dependency-free `src/fuzz.c` (its own TU, like `disasm.c`) that
drives the emulator with the framework's seedable splitmix64 RNG:
- `emu_fuzz_cover1` — coverage-guided generation: keeps inputs that grow the
  block-coverage union, drawing candidates fresh or by mutating a corpus member
  (the feedback). On the host-independent `classify` example it reaches 5 blocks
  vs a fixed positive vector's 3.
- `emu_mutation_test1` — bit-flip mutation testing, run inside the emulator (a
  broken mutant is contained by the instruction cap + fault hooks); the original
  is the oracle. A weak suite over `classify` leaves 100/192 mutants alive; a
  path-covering suite leaves ~16 (equivalent mutants) — a stronger input set
  demonstrably kills more.

Used byte-flip mutation rather than the Keystone path (deliverable #2's stated
alternative), keeping `fuzz.o` emulator-only with no assembler dependency. ~~The
optional libFuzzer/AFL shim (#3) is left for concrete demand.~~ — **LANDED since
(noted 2026-07-21)**: [libfuzzer-afl-shim.md](../implementations/libfuzzer-afl-shim.md)
✅ 5/5 — the `emu_cover_hits` seam plus libFuzzer and AFL++ harnesses, verified in a
clang-18/afl++ image. Caveat: the 2026-07-21 repo review's **K4** records that **no CI
workflow exercises the fuzz lane yet**
([2026-07-21-repo-review.md](../reviews/2026-07-21-repo-review.md)). Acceptance met by
two host-independent example tests in `examples/test_emu.c`.

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
args + `rsp` reset) — since addressed by snapshot/restore, below. Acceptance met: a
routine writing past its confined region is caught at the offending store with its
instruction text, no host crash.

**Done since:** two additions on top of the acceptance above.

- **Read watchpoints** — `emu_watch_reads(e, addr, size, mode, &w)`, the read-side
  sibling of #1 with the same modes and `emu_watch_t` result: catches a routine
  that reads a secret/uninitialized area (`NEVER`) or reads past a declared length
  (`ONLY`) — which a write watch misses. One watchpoint per handle (it replaces an
  armed write watch); x86-64 guest, like the rest of Track F.
- **`emu_snapshot` / `emu_restore` / `emu_snapshot_free`** (`17556f0`) — the answer
  to the retention property above. Mapped memory and the stack *deliberately*
  persist across `emu_call_*` (that is how a caller preloads data), so a Track E
  fuzz or mutation sweep otherwise runs each candidate against memory dirtied by
  earlier ones, making killed/survived classification depend on handle history.
  Bracketing a sweep with a snapshot/restore pair gives every candidate identical
  starting state. A restore rewinds the guest (mapping set, region bytes, register
  context) but deliberately leaves *handle-level* arming — watchpoints, register
  guards, preloads, the fuzz corpus — untouched: those belong to the harness, not
  the guest.

**Effort:** ~3–4 days. **Touches:** `src/emu.c`, `include/asmtest_emu.h`,
`src/disasm.c` (the describe helper, kept Capstone-side), `examples/test_emu.c`,
[docs/emulator.md](../../guides/emulator.md).

---

## Suggested sequencing

1. **Track C** (disassembly) — **done.** Cheap, transformed every diagnostic, and
   Tracks E/F can build on it. Now also bound across all ten languages via the
   single `libasmtest_emu_full` (Keystone + Capstone) — `disas`/`disas_available`.
2. **Track A** (publish) — **was NOT code-complete; four defects fixed 2026-07-17,
   go-live still needs credentials.** The pipeline had not been dispatched since
   2026-06-26 and was broken in four ways (two workflow-config, two real: `go vet`,
   and a Linux wheel whose drtrace tier could never load). It packages, installs and
   smoke-tests every binding (Linux + macOS) again now. Still the single biggest
   adoption lever with no dependency on the others — but its status must be read off a
   **fresh** dispatch, since a dispatch-only workflow decays silently.
3. **Track D** (wide vectors) — **AVX2 + AVX-512 done** (256- **and** 512-bit
   capture on SysV **and** Win64, plus binding parity across all ten), validated
   on a real AVX-512 host. **SVE capture is now landed and qemu-TCG validated**
   (`asm_call_capture_sve` / `svec_t`/`spred_t` / `ASM_SVCALL_*`; the
   `make docker-sve-sweep` VL sweep runs the `ptrue`/`fadd` under qemu at VL
   16–256 B); only the **execution sign-off on real AArch64+SVE silicon** stays
   hardware-gated.
4. **Track F** (invariants) — **done.** Small, compounded with Track C (the
   offending store is disassembled).
5. **Track E** (fuzzing/mutation) — **done.** Built on C and F; coverage now
   drives generation and a mutation tester scores the suite.
6. **Track B** (Win64 isolation) — **done.** Re-exec per-test isolation, `-jN`,
   and `--bench` on Win64, verified under Wine; optional `windows-latest` sign-off.

## Out of scope (for now)

- **New guest architectures** beyond the existing four (MIPS, PowerPC, s390x, RV32,
  WASM, LoongArch). MIPS would close the MIPSUnit prior-art gap in
  [DESIGN.md](../../../DESIGN.md) §1 and a WASM guest would be novel, but both are
  diminishing-returns vs. effort — reconsider only on concrete demand, as
  [expansion-plan.md](../archive/plans/expansion-plan.md) already records.
- A GUI/TUI front-end (TAP + JUnit already integrate with standard tooling).
- Rewriting the C + asm core in another language (wrapping it is the bindings story).
- Tier 3 bindings (porting the runner/discovery into another language).
