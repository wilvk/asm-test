# asm-test — Native Win64 tier: implementation plan

A phased roadmap for adding a **native Win64 tier** — running a routine's real
bytes under the Microsoft x64 ABI on real x86-64 silicon, captured and asserted by
the framework — to complement the existing **emulator** path (`emu_call_win64`,
which already runs Win64 on a System V host).

This plan is the *how + in what order*. The *why + what it takes* is
[Track E.1 — Native Win64 trampoline (scoping)](expansion-plan.md#track-e1--native-win64-trampoline-scoping)
in the expansion plan; read that first. The headline it establishes: **the
trampoline is the easy part; the runner is POSIX and that is the cost** — and the
**Docker + Wine** option there means none of this needs a Windows host.

> Status legend: **planned** unless noted. Update this file as phases land, the way
> [expansion-plan.md](expansion-plan.md) and
> [multi-language-bindings-plan.md](multi-language-bindings-plan.md) track theirs.

---

## Goals & non-goals

**Goal.** Run a routine through the **real** Microsoft x64 ABI on real hardware and
capture/verify it with the framework's existing assertions:

1. **call + capture** a routine via `ASM_CALL*` under Win64 (args in `rcx, rdx, r8,
   r9`; 32-byte shadow space; FP in `xmm0–3` positional with the int slots), and
2. **assert ABI preservation** over the Win64 callee-saved set — `rbx, rbp,
   rdi, rsi, r12–r15` and `xmm6–xmm15` — via `ASSERT_ABI_PRESERVED`,
3. with the framework's isolation / timeout / crash-to-failure / guard-page
   guarantees intact, **without provisioning a Windows host** (Docker + Wine).

**Non-goals.**

- Replacing the emulator path. The emulator stays the answer for AArch64 hosts and
  for faults-as-data; the native tier is the "real silicon, real OS personality"
  complement, not a substitute.
- A native Win64 tier on **AArch64** hosts. Win64-x64 there stays emulator-only
  (Unicorn), exactly as today.
- New guest architectures, or porting the runner to *native* MSVC Windows beyond
  what Wine (and an optional `windows-latest` job) covers.

---

## What already exists (reused as-is)

- **The ABI is already modeled and tested.** `emu_call_win64` /
  `emu_call_win64_traced` (`src/emu.c`) encode the arg registers, the 32-byte
  shadow space, stack args at `[rsp+40+8*i]`, and the `rsi`/`rdi`-nonvolatile rule.
  The native trampoline mirrors this exactly — same contract, real registers.
- **Eight trampoline variants to mirror.** `src/capture.s` (GAS) / `src/capture.asm`
  (NASM): `asm_call_capture`, `_fp`, `_vec`, `_fp_n`, `_vec_n`, `_args`, `_sret`,
  `asm_bigstruct_x86`, built through the `ASM_FUNC`/`ASM_ENDFUNC` macros
  (`include/asm.h`, `include/asm_nasm.inc`).
- **The layout contract.** `regs_t` (`include/asmtest.h`) with `#if __x86_64__` /
  `#elif __aarch64__` branches pinned by `ASMTEST_STATIC_ASSERT` offset checks, the
  manifest generator `scripts/gen-manifest.c` → `asmtest_abi.json`, and the
  conformance corpus `bindings/conformance/conformance.c` → `corpus.json`.
- **Non-jumping verdict shims.** `asmtest_check_abi` / `asmtest_check_flag` (Track
  0.2) already return a verdict instead of `longjmp`-ing, so the Win64 preservation
  check has a clean home.
- **The Docker pattern.** `Dockerfile.bindings-base` (`ubuntu:24.04`) →
  per-target image → `make docker-<x>`, wired into the `bindings` CI matrix.

---

## Execution lanes

Three ways to *run* the native code; this plan uses all three at different phases,
cheapest-first. They share Phases 1–2 (trampoline + layout) entirely.

| Lane | Host | Exercises | Used for |
|---|---|---|---|
| **A — `ms_abi` native** | existing x86-64 Linux/macOS rows | the **ABI** only (no PE loader/OS) | fastest trampoline feedback; Phase 1/3 |
| **B — Docker + Wine** | Linux Docker (no Windows) | real PE + Win32 personality + the runner port | the primary tier; Phases 3–5 |
| **C — `windows-latest`** | a Windows runner | the real OS, authoritative | optional final sign-off; Phase 5 |

Lane A uses GCC/Clang `__attribute__((ms_abi))` on the call site to drive the
Microsoft convention natively on the System V host — no Wine, no PE, no Windows —
so the *trampoline and capture* can be validated on the current CI before any
Docker/Wine work lands. (Caveat: needs `-maccumulate-outgoing-args`; the ELF
lazy-binding caveat is moot on glibc ≥ 2.1.) Lane B is the tier proper. Lane C is
optional insurance against Wine's edge-case fidelity.

---

## Phase 0 — Execution substrate *(gating) — done*

**Status: done.** `Dockerfile.win64` + `make docker-win64` build on the cached
bindings base and run a substrate smoke (`tests/win64/smoke_asm.asm` assembled
`nasm -f win64`, linked by `x86_64-w64-mingw32-gcc`, driven by `tests/win64/smoke.c`)
under `wine64` — verified green on x86-64 (`win64_add3(39) = 42` via the real
Microsoft x64 ABI, arg in `rcx` → `rax`). One packaging gotcha recorded for later
phases: Ubuntu's `wine64` package ships only the loader at `/usr/lib/wine/wine64`
(the `wine64`/`wineboot` PATH wrappers are in the heavier `wine` metapackage, which
pulls i386 multiarch), so the image symlinks the loader to `/usr/local/bin/wine64`
and pre-inits the prefix at build time. Also: Win64 is **LLP64** (`long` is 32-bit),
so the tier's register-width contract must use a fixed 64-bit type, not `long` —
the smoke uses `long long` deliberately.

**Goal.** Stand up the toolchain and a non-Windows way to run a Win64 PE, mirroring
the existing per-target Docker pattern.

**Deliverables.**
- `Dockerfile.win64` on the `ubuntu:24.04` base installing **`mingw-w64`**
  (`x86_64-w64-mingw32-gcc`, GAS) and/or **`nasm`** (`-f win64`) as Linux
  cross-tools, plus **`wine64`** (WineHQ stable; modern WoW64).
- A `make docker-win64` target (parallel to `make docker-<lang>`) that
  cross-compiles a Win64 PE and runs it under `wine64`; joins `make
  docker-bindings` and the `bindings` CI matrix.
- Build wiring: a `WIN64=1` (or `TARGET=win64`) make path that selects the
  cross-compiler/linker and `nasm -f win64`, emitting a `.exe`.

**Files.** new `Dockerfile.win64`; `Makefile` (cross-toolchain vars, `docker-win64`,
NASM `-f win64` rule alongside the existing `-f elf64`/`-f macho64`).

**Acceptance.** `make docker-win64` builds the base (cached) + the win64 image and
runs a trivial cross-compiled `.exe` under `wine64` to completion.

**Effort.** ~1 day.

---

## Phase 1 — Win64 capture trampoline *(in progress)*

**Status: six of eight variants landed.** `src/capture_win64.asm` implements:

- `asm_call_capture_win64` — the Win64 mirror of `asm_call_capture`: 6 integer
  args (rcx/rdx/r8/r9 + two on the stack above the 32-byte shadow space), return +
  callee-saved + RFLAGS capture.
- `asm_call_capture_args_win64` — arbitrary integer arity (rbp-framed, register +
  stack overflow), the mirror of `asm_call_capture_args`.
- `asm_call_capture_vec_win64` — 4 int + 4 vector args, the FP return (xmm0), the
  full vector file (xmm0..15), and the distinctive Win64 piece: **xmm6..15 are
  callee-saved**, so they are saved/seeded-with-sentinels/captured/restored and the
  preservation check covers them.
- `asm_call_capture_fp_win64` — the lighter FP form: 6 int + 4 double args, FP
  return captured, GP callee-saved checked (no xmm6–15 handling, as on SysV `_fp`).
- `asm_call_capture_fp_n_win64` — arbitrary FP arity (rbp-framed); first 4 doubles
  in xmm0–3, the rest spilled to the stack; `nfargs` read from its Win64 stack slot.
- `asm_call_capture_sret_win64` — struct return via the hidden pointer, which on
  Win64 is the *first* arg (rcx), shifting the visible args to rdx/r8/r9 + stack.

All snapshot into `win64_regs_t` (`tests/win64/win64_regs.h`; offsets promoted into
`include/asmtest.h` in Phase 2). Verified **both lanes, 18/18**: `make
win64-msabi-test` natively via `__attribute__((ms_abi))` on x86-64 macOS, and `make
win64-test` as a real PE under Wine via `make docker-win64` — covering return/FP
capture, register + stack-arg + shadow-space marshalling, struct return, and
ABI-violation detection for both an integer (`rbx`) and a vector (`xmm6`) clobber.
The image CMD is `make win64-check` (smoke + capture test).

**Remaining variants** (final slice): `_vec_n` (arbitrary vector arity — note the
Win64 default passes `__m128` by reference, not in xmm, unlike `__vectorcall`; the
fixed `_vec` uses the vectorcall-style xmm0–3 convention) and `bigstruct` (large
struct args by value, part-C even on the SysV path).

**Goal.** Mirror the eight `asm_call_capture*` variants for the Microsoft x64 ABI.

**Deltas from System V** (all already modeled in `emu.c`): integer args in `rcx,
rdx, r8, r9` then stack; FP args in `xmm0–3` *positional* with the int slots (slot
*i* is int **or** xmm); 32-byte shadow space reserved below the return address at
every call site; callee-saved set adds `rdi, rsi` (int) and `xmm6–xmm15` (FP); for
varargs, FP args duplicated into the matching int register. So the trampoline
saves/seeds/verifies a *larger* register set than the SysV one.

**Deliverables.**
- `src/capture_win64.asm` (NASM `-f win64`, the cheapest assembler route) — the
  eight variants under the MS x64 convention. A GAS `.s` variant is optional;
  NASM is the primary backend for this tier.
- Fast feedback via **Lane A**: an `ms_abi` call-site shim so the same trampoline
  logic is exercisable on the existing x86-64 Linux/macOS CI rows before Wine.

**Files.** new `src/capture_win64.asm` (+ optional `.s`); `include/asm_nasm.inc` if
macro tweaks are needed for win64 output.

**Acceptance.** Each variant assembles with `nasm -f win64`; a hand-driven call of
`mov rax, rcx` returns the first arg (the Win64 vs SysV contrast), under both Lane
A and Lane B.

**Effort.** ~2 days.

---

## Phase 2 — Win64 `regs_t`, ABI-preservation, manifest, corpus

**Goal.** Give the captured Win64 state a typed home and a drift-proof contract.

**Deliverables.**
- **A third `regs_t` branch.** `include/asmtest.h` gains a Win64 layout (same
  x86-64 arch, different ABI) adding `rdi`/`rsi` and `xmm6–15` to the
  captured/checked set, selected by a build macro (e.g. `ASMTEST_ABI_WIN64`), with
  matching `ASMTEST_STATIC_ASSERT` offset pins so it cannot silently drift from the
  trampoline's stores.
- **New sentinels** (`ASMTEST_SENTINEL_RDI`/`_RSI` + `xmm6–15` FP sentinels) and
  Win64-aware `asmtest_assert_abi` / `asmtest_check_abi` logic over the larger
  callee-saved set.
- **Manifest + corpus.** `scripts/gen-manifest.c` emits the Win64 layout into
  `asmtest_abi.json`; `bindings/conformance/conformance.c` gains a **native** Win64
  case (the corpus already carries an *emulator* Win64 case) so `corpus.json` pins
  expected native captures.

**Files.** `include/asmtest.h`, `src/asmtest.c` (preservation logic),
`scripts/gen-manifest.c`, `bindings/conformance/conformance.c`.

**Acceptance.** `_Static_assert`s hold; `make manifest` lists the Win64 struct;
`make conformance` (native Win64 build) reproduces the expected captures.

**Effort.** ~1–2 days.

---

## Phase 3 — Trampoline validation slice *(decision gate)*

**Goal.** Validate the trampoline + capture path **without** the runner port — the
"recommended first slice" from Track E.1.

**Deliverables.** A single Win64 suite (e.g. an `examples/` arith + ABI-preservation
case) cross-assembled `nasm -f win64`, run **`--no-fork`**: first under Lane A on
the existing CI for fast feedback, then under Lane B (`make docker-win64` /
`wine64`) for the real PE path.

**Acceptance.** The suite passes green under both lanes; `ASSERT_ABI_PRESERVED`
covers the full Win64 callee-saved set (incl. `rdi`/`rsi`, `xmm6–15`).

**Decision gate.** From here, decide whether the Win32 runner port (Phase 4) is
worth it, or whether `--no-fork` Win64 coverage under Wine is enough.

**Effort.** ~1 day (on top of Phases 1–2).

---

## Phase 4 — Win32 runner port *(the bulk; only if the gate says go)*

**Goal.** Bring the framework's isolation guarantees to the Win64 tier. `src/asmtest.c`
leans on POSIX primitives that don't exist under a Win32 personality; port each to
its Win32 equivalent (all implemented by Wine, so this is developed/tested under
Lane B with no Windows host):

| POSIX (current) | Feature | Win32 port |
|---|---|---|
| `fork`/`waitpid`/`poll` | per-test isolation + `-jN` pool | `CreateProcess` + `WaitForMultipleObjects` |
| `sigaction` + `siglongjmp` | crash-to-failure (SIGSEGV/BUS/FPE/ILL) | vectored exception handler / SEH |
| `alarm()` | per-test timeout | `CreateTimerQueueTimer` / watchdog thread |
| `mmap` + `mprotect(PROT_NONE)` | guard-page allocator | `VirtualAlloc` + `PAGE_NOACCESS` |
| `fnmatch` | `--filter` glob | `PathMatchSpec` / portable matcher |

Keep the POSIX paths; gate the Win32 paths behind the same target macro as the
`regs_t` branch. The MinGW emulated-`fork` shortcut is **rejected** — it is as
unreliable under Wine as on Windows and would degrade the framework's headline
isolation feature; do the proper Win32 port.

**Files.** `src/asmtest.c` (platform-gated isolation/signal/timeout/guard/filter
layers); possibly a small `src/platform_win32.c` to keep the POSIX file legible.

**Acceptance.** `make test` / `make check` green under `wine64` in `make
docker-win64` with isolation (CreateProcess-equivalent), timeout, crash
containment, and the `-jN` pool all working.

**Effort.** ~1–2 weeks done properly.

---

## Phase 5 — CI, authoritative sign-off, docs

**Goal.** Make it permanent and documented.

**Deliverables.**
- The `bindings` (or a new `win64`) CI matrix runs `make docker-win64` on every
  push — no Windows host.
- *Optional* Lane C: a `windows-latest` job running the same suite for
  authoritative real-OS sign-off (NASM `-f win64`, or MSVC `ml64`), kept thin
  since Wine carries the bulk.
- Docs: a `docs/win64.md` (or a section in `docs/emulator.md`/`docs/portability.md`)
  describing the native tier, the three lanes, and the Wine-fidelity caveat;
  `CHANGELOG.md` entry; flip Track E.1's status note.

**Acceptance.** CI green; docs published; Track E.1 + the Track E summary bullet
updated to "landed."

**Effort.** ~1 day (+ ~0.5 day for the optional `windows-latest` job).

---

## Suggested sequencing

1. **Phase 0** — substrate (Docker + Wine image, win64 build path). Gates run.
2. **Phase 1 + 2** — trampoline + layout/manifest/corpus (the ~2–3 day "easy part",
   exercised fast via Lane A).
3. **Phase 3** — the validation slice + **decision gate**.
4. **Phase 4** — the Win32 runner port, *only if* the gate says the full tier is
   wanted; this is the bulk of the effort.
5. **Phase 5** — CI + optional `windows-latest` + docs.

Phases 0–3 deliver a usable, CI'd `--no-fork` Win64 trampoline tier on the existing
Linux CI in ~4–5 days. Phase 4 is the optional deep investment.

---

## Acceptance criteria (overall)

- A Win64 routine builds and is callable via `ASM_CALL*` on real x86-64 hardware,
  with `ASSERT_ABI_PRESERVED` covering the Win64 callee-saved set (`rbx, rbp, rdi,
  rsi, r12–r15`, `xmm6–15`).
- The native Win64 conformance case in `corpus.json` reproduces under a native
  build; the manifest carries the Win64 layout, pinned by `_Static_assert`.
- `make docker-win64` runs the Win64 suite under Wine on the existing Linux CI with
  **no Windows host**; with Phase 4, isolation/timeout/crash-containment are green.

## Risks

- **Wine ≠ Windows at the edges** (SEH corner cases, some syscalls) → keep the
  optional `windows-latest` sign-off, or accept Wine fidelity explicitly.
- **Layout drift** between the Win64 `regs_t` and the trampoline's stores →
  mitigated by the `_Static_assert` pins + the conformance corpus, exactly as for
  the existing branches.
- **Runner-port scope creep** (Phase 4) → contained by the Phase 3 decision gate;
  `--no-fork` under Wine is a shippable stopping point.
- **AArch64 hosts** have no native lane → unchanged: Win64-x64 stays emulator-only
  there, matching the existing optional-emulator stance.

## Out of scope

- A native Win64 tier on AArch64 hosts.
- Porting to native MSVC Windows beyond the optional `windows-latest` job.
- New guest architectures (see [expansion-plan.md](expansion-plan.md)).
