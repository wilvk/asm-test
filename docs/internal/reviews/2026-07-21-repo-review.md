# asm-test — repository review (2026-07-21)

**Scope:** whole-repo health review across six subsystems — the core C
framework (`src/`, `include/`), the `asmspy` live tracer and native tooling
(`cli/`, `tools/`, `pintool*/`, `drclient/`, `bpf/`), the ten language bindings
(`bindings/`), build/CI/packaging (`Makefile`, `mk/`, the `Dockerfile.*` set,
`scripts/`, `packaging/`, `.github/`), documentation (`docs/`), and the
framework's own tests/examples (`tests/`, `examples/`). Unlike the archived
[2026-07-01](../archive/reviews/2026-07-01-repo-review.md) /
[-07-02](../archive/reviews/2026-07-02-repo-review.md) /
[-07-04](../archive/reviews/2026-07-04-repo-review.md) reviews, this pass is a
fresh full sweep after the Zen 2 → Zen 5 dev-box change and the 2026-07-21
Intel-PT-on-silicon validation. It records **findings not already tracked** in
`plans/` or `analysis/`; already-tracked items are cross-referenced in §0 and
excluded from the numbered list.

**Method:** six parallel subsystem reviewers each read their area's source and
proposed findings with `file:line` evidence. The highest-value items were then
re-verified by hand against the tree for this write-up. Each finding carries a
verification marker: **[verified]** = re-checked against the source on
2026-07-21; **[reported]** = solid `file:line` evidence from the subsystem pass
but no independent second read — treat as a lead. `make test` and `make check`
(54/54) are green on the x86-64 dev host; the review is not blocked on hardware,
privileges, or credentials.

**Remediation status:** OPEN — nothing actioned yet. This file moves to
`../archive/reviews/` in the change that closes the last finding.

Paths are repo-relative; every `file:line` is a snapshot as of 2026-07-21 and,
like every review here, may drift.

---

## 0. Already tracked elsewhere (not re-filed here)

Three findings surfaced by this sweep already have a home; they are listed so
this review is self-contained, not to re-open them:

- **.NET managed multi-threaded live-Intel-PT concurrency race** (NULL-deref
  SIGSEGV, non-deterministic) — owned by
  [plans/dotnet-managed-pt-concurrency-plan.md](../plans/dotnet-managed-pt-concurrency-plan.md)
  (authored 2026-07-21, NOT STARTED). `libipt-dev` is deliberately reverted out
  of the dotnet image so no racy privileged lane ships.
- **Documentation drift after the Zen 2 → Zen 5 + PT-validation change**
  (which-box / which-validation-status inconsistencies) — scoped as
  [plans/amd-review-followup-2-plan.md](../plans/amd-review-followup-2-plan.md)
  T2 (high-severity doc-drift sweep). Two *concrete* drift instances not obviously
  inside that T2 are filed below as D1/D2.
- **Data-flow def-use/slice half missing from 7 of 10 bindings** — noted in
  [analysis/2026-07-17-dataflow-tier-open-followups.md](../analysis/2026-07-17-dataflow-tier-open-followups.md)
  item 5. *Reconcile:* the implementations brief
  [dataflow-bindings-slice-codeimage.md](../implementations/dataflow-bindings-slice-codeimage.md)
  reports ✅4/4 over the same area — one of the two is stale; worth settling which.

---

## 1. asmspy / CLI

- **C1 — TUI has no SIGINT / SIGTERM / terminal-restore handler; Ctrl-C can leak
  a planted breakpoint into the live target. [verified]** *(highest-value item in
  this review.)* [cli/asmspy.c:4914-4915](../../../cli/asmspy.c#L4914) does
  `initscr(); cbreak();` and only reaches `endwin()` at
  [:5090](../../../cli/asmspy.c#L5090); the sole signal handler installed is the
  SIGALRM quit-wake ([:3672](../../../cli/asmspy.c#L3672)). Because the TUI uses
  `cbreak()` not `raw()`, **Ctrl-C delivers SIGINT whose default action
  terminates asmspy**. If that lands during a software-int3 region trace (the TUI
  default, `only_tid==0`), the planted `0xcc` from `rgn_plant_bp`
  ([asmspy_engine.c:2715](../../../cli/asmspy_engine.c#L2715)) is **not
  reverted** — unlike debug registers, a `POKETEXT` byte is plain memory the
  kernel does not restore on tracer death, so the target later executes it and
  dies. This is the one hole in asmspy's otherwise fastidious "never kill the
  target" invariant. *Fix:* install a SIGINT/SIGTERM handler that sets `stop`
  (and at minimum calls `endwin()`), so the engine's normal two-phase detach runs.
- **C2 — `--follow` into a 32-bit `execve` decodes against the x86-64 syscall
  table. [reported]** The i386 guard (`asmtest_elf_class(pid)==32`) is checked at
  attach time only ([asmspy_engine.c:2532](../../../cli/asmspy_engine.c#L2532));
  the syscall-stream engine does not set `PTRACE_O_TRACEEXEC`
  ([:2538](../../../cli/asmspy_engine.c#L2538)) and has no exec handling, so a
  followed 64-bit child that `execve`s a 32-bit image is thereafter decoded with
  the wrong syscall table — the exact "confident nonsense" the guard exists to
  prevent. Narrow (needs `--follow` + a child exec to i386) but real. *Fix:* add
  `PTRACE_O_TRACEEXEC` + re-fingerprint on exec, or drop the followed task on an
  i386 exec.
- **C3 — App-delivered SIGTRAP is not re-injected in the syscall-stream engine.
  [reported]** [asmspy_engine.c:2654](../../../cli/asmspy_engine.c#L2654) — a
  target that relies on its own SIGTRAP handler behaves differently while
  `--log`-traced. Self-documented as a limitation; the single-step engines handle
  it correctly via the `si_code` split. Low priority; flag in the docs.

## 2. Core C framework (`src/`, `include/`)

The concurrency items below live in multi-threaded hardware-capture paths this
host cannot fully exercise; they do not affect the well-tested
in-process/emulator/single-step paths.

- **S1 — `g_amd_snap` is a process-global `int` amid per-thread AMD state.
  [verified]** [src/hwtrace.c:755](../../../src/hwtrace.c#L755)
  (`static int g_amd_snap = 0;`) sits beside the `__thread` `g_fd`/`g_base_map`/
  `g_active` at [:601-606](../../../src/hwtrace.c#L601) — and its own comment
  claims "same invariant." Set at [:863](../../../src/hwtrace.c#L863), read in
  `hwtrace_end_amd` at [:1183](../../../src/hwtrace.c#L1183). A concurrent
  snapshot arm on one thread flips another thread's teardown branch → wrong
  branch + leaked `g_fd`/`g_base_map`. *Fix:* make it `__thread` like its siblings.
- **S2 — `g_pt_window` single slot is unsynchronized. [reported]** Checked
  [hwtrace.c:3458](../../../src/hwtrace.c#L3458), set :3464, read/cleared
  :3521-3532 — a genuine data race if two threads open a PT window, unlike the
  mutex-guarded region registry. Documented as single-slot but unguarded.
- **S3 — `render_window` dereferences raw captured absolute RIPs with no
  liveness guard. [reported]** [hwtrace.c:3610](../../../src/hwtrace.c#L3610)
  reads 16 bytes per address → SIGSEGV if the traced region was unmapped after
  capture. The sibling `render_versioned` correctly uses the bounds-checked
  code-image instead; `render_window` should too.
- **S4 — AArch64 hw-breakpoint resume assumes x86 `EFLAGS.RF`. [reported]** The
  wrong-depth path does a bare `PTRACE_CONT`
  ([ptrace_backend.c:1192](../../../src/ptrace_backend.c#L1192)) relying on RF
  auto-advance, which AArch64 lacks; an arm64 W^X JIT re-entering the return
  breakpoint could spin re-trapping. x86-validated only.
- **S5 — Hostile-jitdump integer cast is implementation-defined. [reported]**
  `name_len = (long)total - 56 - (long)code_size`
  ([ptrace_backend.c:206](../../../src/ptrace_backend.c#L206), also :450) over an
  untrusted `/tmp/jit-<pid>.dump`; a `code_size > LONG_MAX` cast is UB/impl-defined
  before `malloc`/`fread` (the `<=0` guard catches only ordinary negatives).
- **S6 — ~15 growable pools double `ncap` without an overflow guard. [reported]**
  `ncap *= 2` / `realloc(p, nc * sizeof …)` at descent.c:31, dataflow.c:138,
  hwtrace.c:2549-2552, trace.c:480, codeimage.c:208/391 and the dataflow
  producers — `ncap` can wrap to 0. Bounded in practice by instruction budgets;
  theoretical, but cheap to harden with a max-cap clamp.
- **S7 — `round_pages` can overflow on caller-controlled sizes. [reported]**
  [hwtrace.c:618-628](../../../src/hwtrace.c#L618) — `(v+pg-1)/pg` can wrap and
  the `p <<= 1` loop shift to 0 on a hostile `aux_size`/`data_size`; no upper clamp.

## 3. Language bindings (`bindings/`)

All ten are real, tier-complete, and validated against one shared conformance
corpus; the register structs are consumed via opaque handles in the six dynamic
bindings, so there is **no ABI-layout risk in the core FFI**. Findings are
localized cleanups.

- **B1 — Rust in-line assembler/disassembler is silently unreachable without
  `ASMTEST_LIB` set. [verified]** The crate links `libasmtest_emu` (which carries
  Keystone/Capstone), yet `asm_available()`
  ([rust/src/lib.rs:701](../../../bindings/rust/src/lib.rs#L701)) gates on a
  separate `dlopen($ASMTEST_LIB)`
  ([:643](../../../bindings/rust/src/lib.rs#L643)); with the env var unset (the
  common downstream case) `assemble()`/`call_asm()`/`disas()` return "not in this
  build" ([:762](../../../bindings/rust/src/lib.rs#L762),
  [:792](../../../bindings/rust/src/lib.rs#L792)) despite the symbols being
  linked in. *Fix:* fall back to `dlopen(NULL)` on the already-linked image.
- **B2 — Java `HwTrace.resolve/resolveTiers/status/…` throw when the hwtrace lib
  fails to load, contradicting the class's own self-skip contract. [verified]**
  `available(int)` self-skips cleanly
  ([java/HwTrace.java:850](../../../bindings/java/HwTrace.java#L850)) exactly as
  the header promises ("callers never see a throw … available self-skips
  cleanly", [:25](../../../bindings/java/HwTrace.java#L25)), but `status`
  ([:882](../../../bindings/java/HwTrace.java#L882)), `resolve`
  ([:916](../../../bindings/java/HwTrace.java#L916)) and the other tier entry
  points `throw new RuntimeException("libasmtest_hwtrace not loaded")`. A harness
  that calls `status`/`resolve` before the `available()` skip guard yields
  `Bail out!` + exit 1 instead of a clean `# SKIP`. Masked today only because the
  lib is always bundled. *Fix:* have the resolve/status family degrade to a
  self-skip return like `available()`.
- **B3 — .NET and Java rely entirely on manual disposal for long-lived native
  handles. [reported]** .NET uses raw `IntPtr` with **no `SafeHandle` and mostly
  no finalizers** (and leaves `NativeCode`/`NativeTrace`/`HwTrace` as
  non-`IDisposable`, `Free()`-only types); Java has **no `Cleaner`**. A dropped
  handle leaks the native mapping/trace in either. (Java's per-call confined
  `Arena` discipline for *transient* buffers is correct — the gap is only the
  long-lived handles.) Contrast Python/Rust/C++/Go/Zig, which tie freeing to
  scope/Drop/deinit.
- **B4 — Rust has no `riscv64` capture struct. [reported]**
  [rust/src/lib.rs:62](../../../bindings/rust/src/lib.rs#L62) defines only
  `x86_64`/`aarch64` `Regs`, so the binding won't compile as a capture tier on a
  RISC-V host even though the C `regs_t` exists there (Python's manifest path is
  arch-agnostic).
- **B5 — Go/Zig `dlopen`-tier structs are hand-mirrored with no compile-time
  cross-check. [reported]** drtrace/hwtrace/dataflow struct layouts are mirrored
  in cgo preambles / `extern struct`s (`go/hwtrace.go:42-158`,
  `zig/src/hwtrace.zig:111-216`) with "must be updated by hand" comments;
  offsets are correct today but desync silently on a header change until a test
  catches it. The shared `at_val_rec_t` hand-offset mirror in both dataflow smoke
  drivers is the riskiest single spot.
- **B6 — Go `go vet` defect. [reported]** `go/conformance_test.go:405` uses
  `unsafe.Pointer(nc.Base())` — the exact `uintptr→unsafe.Pointer` round-trip the
  API added `HwNativeCode.Ptr()` to avoid (`hwtrace.go:996-1002`). Trivial.
- **B7 — Dynamic-language integer-width edges. [reported]** Ruby binds the
  `unsigned long` return `asmtest_regs_ret` as signed `Fiddle::TYPE_LONG`
  (`ruby/asmtest.rb:81`), so a return ≥ 2⁶³ reads back negative; Node/koffi maps
  `unsigned long`/`uint64_t` to JS `Number` (`asmtest.js:87`), losing precision
  above 2⁵³ for fault addresses / register values. Inherent to those FFIs; a
  BigInt/unsigned pass would fix it. LuaJIT is fine (boxed 64-bit cdata).
- **B-note — CoreSight is `not wired` in both .NET and Java** (honest self-skip,
  no implementation behind it) — consistent with the C-side CoreSight-live gate
  (S-adjacent), not a defect.

## 4. Build / CI / packaging

Pin-everything discipline with an integrity manifest
(`scripts/third-party-digests.txt` + `check-thirdparty-versions.sh`) and genuine
anti-vacuity CI. The gaps are supply-chain drift, not breakage.

- **K1 — DynamoRIO is fetched by raw `curl` with no digest check in 12
  Dockerfiles, bypassing the repo's own verifier. [verified]**
  [Dockerfile.drtrace:37](../../../Dockerfile.drtrace#L37)
  (`RUN curl -fsSL "$DR_URL" …`) plus 11 siblings (drtrace-lang, drext-probe,
  pintool, gcprofiler-probe, suspendprof-probe, taint-{native,attach,
  attach-probe,dotnet,managed-attach-probe,oracle}) each copy-paste the same
  block — even though `scripts/fetch-dynamorio.sh` exists and verifies the
  SHA-256 anchor from the manifest, and `check-thirdparty-versions.sh` gates only
  2 of the 12. *Fix:* route all 12 through `fetch-dynamorio.sh` (kills the
  integrity gap and the ×12 copy-paste in one move).
- **K2 — The manylinux PyPI wheel base is fully unpinned. [reported]**
  [Dockerfile.manylinux-wheel:10](../../../Dockerfile.manylinux-wheel#L10)
  (`FROM quay.io/pypa/manylinux_2_28_${ARCH}`, no tag/digest; same in
  `release.yml:207`). This is the one packaging path that reaches end users, on a
  floating base.
- **K3 — Duplicate make recipe for `build/asmtest_nomain.o`. [verified]**
  Defined in both [mk/fuzz.mk:24](../../../mk/fuzz.mk#L24) (adds a `.build-flags`
  prereq) and [mk/bindings.mk:17](../../../mk/bindings.mk#L17) (adds
  `-Wno-unused-function`) with *different* recipes. GNU make prints
  "overriding recipe / ignoring old recipe" on **every** invocation (visible in
  `make help`); bindings.mk wins (included last), so the fuzz object silently
  loses its `.build-flags` dependency — a latent stale-rebuild bug plus permanent
  warning noise. *Fix:* one canonical recipe (or distinct object names).
- **K4 — The fuzz lane is never exercised by CI. [verified]** `Dockerfile.fuzz`,
  `docker-fuzz`, and `fuzz-shim-test` exist and `make help` advertises them, but
  no `.github/workflows/*` references fuzz — a whole capability lane unexercised,
  contrary to the repo's anti-vacuity posture.
- **K5 — Third-party GitHub Actions float, unlike the digest-pinned engines.
  [reported]** `pypa/gh-action-pypi-publish@release/v1` (a moving *branch*, the
  weakest), plus `setup-zig@v2`, `msys2/setup-msys2@v2`,
  `crates-io-auth-action@v1`, `docker/setup-buildx-action@v4` — all major-tag,
  none SHA-pinned; also `actions/setup-python@v6` (release.yml) vs `@v5`
  (ci.yml). SHA-pin at least the publish action.

## 5. Documentation

Complete against the feature matrix and honest about hardware gating; the
weakness is synchronization lag (see §0 for the tracked sweep). Two concrete,
separately-fixable instances:

- **D1 — Residual Zen-3 LBR overclaim contradicting `_positions.md` #2.
  [verified]** [docs/reference/features.md:114](../../reference/features.md#L114)
  lists "AMD LBR (Zen 3 BRS / Zen 4–5 …)" and `reference/diagrams.md:241` says
  "bare-metal Zen 3+", presenting Zen 3 as supported, while the project's own
  position ledger states the floor is Zen 4 and "Zen 3 BRS never opened by this
  tree." Most other pages caveat this correctly. *Fix:* qualify both cells.
- **D2 — Two internal-engineering pages leak onto the public Sphinx site.
  [reported]** `docs/amd_tracing_review.md` (a code review with `src/file.c:line`
  finding IDs, premised on the now-superseded Zen-2 host — stale, and a duplicate
  of `internal/analysis/2026-07-09-amd-tracing-review.md`) and
  `docs/scoped-tracing-implementation.md` (an implementation ledger whose
  `../src/…` relative links point at non-doc sources) are `orphan: true` but
  still built. *Fix:* move both under `docs/internal/`.
- **D3 — `docs/guides/win64.md` self-contradicts on runner-port status.
  [reported]** Intro says the port is "now underway"; the body ("The runner port")
  says it is "done … full parity."
- **D-note — `internal/README.md` lists a `reviews/` directory that did not exist
  until this file created it** (only `archive/reviews/` was present); the layout
  table is now correct again.

## 6. Tests / examples

The hard stuff (crash/timeout/guard-page/struct-ABI/JUnit-escaping/parallel
ordering/differential shrinking) is genuinely asserted via paired positive+negative
cases. Two minor items:

- **T1 — Permanent placeholder SKIP in the default `make test` set. [verified]**
  [examples/test_mem.c:49](../../../examples/test_mem.c#L49) —
  `SKIP("partial-fill semantics not finalized")` shows a *permanent* SKIP in
  every core run. Either finalize the semantics and assert them, or move the case
  out of the default set.
- **T2 — The pure-spine trace/dataflow/ibs/operands suites use a hand-rolled
  `CHECK`/printf `ok/not ok` `main()` [reported]** — a parallel test idiom that
  does not exercise the framework's own `TEST()`/`ASSERT` runner and is not
  covered by `tests/expect.sh`. Functionally fine; worth noting as an unverified
  second path.

---

## Suggested fix order

1. **C1** (asmspy Ctrl-C breakpoint-leak) — small, and it protects live user
   targets; it defeats the subsystem's central invariant.
2. **S1** (`g_amd_snap` → `__thread`) and **B2** (Java hwtrace self-skip
   contract) — one-line-class correctness fixes with clear right answers.
3. **K1** (route the 12 DynamoRIO fetches through `fetch-dynamorio.sh`) and
   **K2** (pin the manylinux base) — supply-chain integrity of shipped/bundled
   artifacts.
4. **K3/K4** (dedupe the make recipe; wire fuzz into CI) and **B1/B4/B6** (Rust
   asm gate, Rust riscv64, Go vet) — mechanical cleanups.
5. **D1/D2/D3** — fold into the tracked doc-drift sweep
   ([amd-review-followup-2-plan.md](../plans/amd-review-followup-2-plan.md) T2).
6. **T1**, **C2/C3**, **S2–S7**, **B3/B5/B7**, **K5** — harden as the
   multi-thread hardware-capture and arm64 paths mature; none is blocking.

Nothing in this review is blocked on hardware, privileges, or a credentialed
action except the live *reproduction* of the tracked .NET PT race (§0), which
needs the bare-metal Intel PT box.
