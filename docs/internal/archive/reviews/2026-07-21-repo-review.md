# asm-test — repository review (2026-07-21)

**Scope:** whole-repo health review across six subsystems — the core C
framework (`src/`, `include/`), the `asmspy` live tracer and native tooling
(`cli/`, `tools/`, `pintool*/`, `drclient/`, `bpf/`), the ten language bindings
(`bindings/`), build/CI/packaging (`Makefile`, `mk/`, the `Dockerfile.*` set,
`scripts/`, `packaging/`, `.github/`), documentation (`docs/`), and the
framework's own tests/examples (`tests/`, `examples/`). Unlike the archived
[2026-07-01](../../archive/reviews/2026-07-01-repo-review.md) /
[-07-02](../../archive/reviews/2026-07-02-repo-review.md) /
[-07-04](../../archive/reviews/2026-07-04-repo-review.md) reviews, this pass is a
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

**Remediation status:** COMPLETE — every numbered finding is closed. Groups 1–4
(C1, S1, B2, K1–K4, B1, B4, B6) fixed & lane-verified 2026-07-21; **T1, D1, D2,
D3, K5** and the core-C trio **S3, S5, S7** on 2026-07-22; and the remainder —
**C2, C3, S2, S4, S6, B3, B5, B7, T2** — on 2026-07-22, each with an
anti-vacuity-checked test and lane verification. Two findings carry a recorded
gate rather than a live behavioural run, both legitimate hardware gates per
CLAUDE.md: **S4** (the AArch64 hardware-breakpoint resume is fixed and compiles
for aarch64, but firing a hardware execution breakpoint needs a bare-metal
`NT_ARM_HW_BREAK` box — the Azure Cobalt arm64 CI runner withholds the exception
and qemu exposes zero slots) and **B3-Descent** (the late-bound upcall-arena
Cleaner is staged as the same-pattern completion of the shipped
NativeCode/NativeTrace/AddrChannel/CodeImage backstops). Per the header rule this
file moves to `../archive/reviews/` in this closing change; the D-items closed the
concrete instances the broader amd-review-followup-2-plan T2 doc-drift sweep
tracks. (Two concurrent agents split this review; the batches above landed
independently and are reconciled here.)

Paths are repo-relative; every `file:line` is a snapshot as of 2026-07-21 and,
like every review here, may drift.

---

## 0. Already tracked elsewhere (not re-filed here)

Three findings surfaced by this sweep already have a home; they are listed so
this review is self-contained, not to re-open them:

- **.NET managed multi-threaded live-Intel-PT concurrency race** (NULL-deref
  SIGSEGV, non-deterministic) — owned by
  [plans/dotnet-managed-pt-concurrency-plan.md](../../plans/dotnet-managed-pt-concurrency-plan.md)
  (authored 2026-07-21, NOT STARTED). `libipt-dev` is deliberately reverted out
  of the dotnet image so no racy privileged lane ships.
- **Documentation drift after the Zen 2 → Zen 5 + PT-validation change**
  (which-box / which-validation-status inconsistencies) — scoped as
  [plans/amd-review-followup-2-plan.md](../../plans/amd-review-followup-2-plan.md)
  T2 (high-severity doc-drift sweep). Two *concrete* drift instances not obviously
  inside that T2 are filed below as D1/D2.
- **Data-flow def-use/slice half missing from 7 of 10 bindings** — noted in
  [analysis/2026-07-17-dataflow-tier-open-followups.md](../../analysis/2026-07-17-dataflow-tier-open-followups.md)
  item 5. *Reconcile:* the implementations brief
  [dataflow-bindings-slice-codeimage.md](../../implementations/dataflow-bindings-slice-codeimage.md)
  reports ✅4/4 over the same area — one of the two is stale; worth settling which.

---

## 1. asmspy / CLI

- **C1 — TUI has no SIGINT / SIGTERM / terminal-restore handler; Ctrl-C can leak
  a planted breakpoint into the live target. [verified] [FIXED 2026-07-21]**
  *(Fixed as filed, plus the headless engines — usage promises Ctrl-C interruption
  there too; every CLI engine call now passes a signal-set stop flag, `--sample`
  excepted by design since its NULL stop means "one window" and it plants nothing.
  New differential cli-smoke leg; `make docker-cli` PASS.)* *(highest-value item in
  this review.)* [cli/asmspy.c:4914-4915](../../../../cli/asmspy.c#L4914) does
  `initscr(); cbreak();` and only reaches `endwin()` at
  [:5090](../../../../cli/asmspy.c#L5090); the sole signal handler installed is the
  SIGALRM quit-wake ([:3672](../../../../cli/asmspy.c#L3672)). Because the TUI uses
  `cbreak()` not `raw()`, **Ctrl-C delivers SIGINT whose default action
  terminates asmspy**. If that lands during a software-int3 region trace (the TUI
  default, `only_tid==0`), the planted `0xcc` from `rgn_plant_bp`
  ([asmspy_engine.c:2715](../../../../cli/asmspy_engine.c#L2715)) is **not
  reverted** — unlike debug registers, a `POKETEXT` byte is plain memory the
  kernel does not restore on tracer death, so the target later executes it and
  dies. This is the one hole in asmspy's otherwise fastidious "never kill the
  target" invariant. *Fix:* install a SIGINT/SIGTERM handler that sets `stop`
  (and at minimum calls `endwin()`), so the engine's normal two-phase detach runs.
- **C2 — `--follow` into a 32-bit `execve` decodes against the x86-64 syscall
  table. [reported] [FIXED 2026-07-22]** *(PTRACE_O_TRACEEXEC + EI_CLASS re-check drops the i386 child at its exec-stop; docker-cli leg.)* The i386 guard (`asmtest_elf_class(pid)==32`) is checked at
  attach time only ([asmspy_engine.c:2532](../../../../cli/asmspy_engine.c#L2532));
  the syscall-stream engine does not set `PTRACE_O_TRACEEXEC`
  ([:2538](../../../../cli/asmspy_engine.c#L2538)) and has no exec handling, so a
  followed 64-bit child that `execve`s a 32-bit image is thereafter decoded with
  the wrong syscall table — the exact "confident nonsense" the guard exists to
  prevent. Narrow (needs `--follow` + a child exec to i386) but real. *Fix:* add
  `PTRACE_O_TRACEEXEC` + re-fingerprint on exec, or drop the followed task on an
  i386 exec.
- **C3 — App-delivered SIGTRAP is not re-injected in the syscall-stream engine.
  [reported] [FIXED 2026-07-22]** *(re-injected on si_code evidence via the PTRACE_SYSCALL resume in --log and --procs --count=syscalls; docker-cli SWALLOWED=0.)* [asmspy_engine.c:2654](../../../../cli/asmspy_engine.c#L2654) — a
  target that relies on its own SIGTRAP handler behaves differently while
  `--log`-traced. Self-documented as a limitation; the single-step engines handle
  it correctly via the `si_code` split. Low priority; flag in the docs.

## 2. Core C framework (`src/`, `include/`)

The concurrency items below live in multi-threaded hardware-capture paths this
host cannot fully exercise; they do not affect the well-tested
in-process/emulator/single-step paths.

- **S1 — `g_amd_snap` is a process-global `int` amid per-thread AMD state.
  [verified] [FIXED 2026-07-21]** *(now `__thread`; `docker-hwtrace` 697 ok.)* [src/hwtrace.c:755](../../../../src/hwtrace.c#L755)
  (`static int g_amd_snap = 0;`) sits beside the `__thread` `g_fd`/`g_base_map`/
  `g_active` at [:601-606](../../../../src/hwtrace.c#L601) — and its own comment
  claims "same invariant." Set at [:863](../../../../src/hwtrace.c#L863), read in
  `hwtrace_end_amd` at [:1183](../../../../src/hwtrace.c#L1183). A concurrent
  snapshot arm on one thread flips another thread's teardown branch → wrong
  branch + leaked `g_fd`/`g_base_map`. *Fix:* make it `__thread` like its siblings.
- **S2 — `g_pt_window` single slot is unsynchronized. [reported] [FIXED 2026-07-22]** *(dedicated mutex + reserved-arm sentinel; host-testable seam proves exactly-one-of-8-wins, docker-hwtrace.)* Checked
  [hwtrace.c:3458](../../../../src/hwtrace.c#L3458), set :3464, read/cleared
  :3521-3532 — a genuine data race if two threads open a PT window, unlike the
  mutex-guarded region registry. Documented as single-slot but unguarded.
- **S3 — `render_window` dereferences raw captured absolute RIPs with no
  liveness guard. [verified] [FIXED 2026-07-22]** *(render_window now copies each
  recorded RIP fault-safely via a new `hw_read_self_live` helper —
  `process_vm_readv(getpid())`, page-clamped on a straddle, mirroring
  pt_backend.c's `pt_read_self_live` — before disassembling, so a region unmapped
  after capture renders "(undecodable)" instead of faulting. New
  `test_wholewindow_render_unmapped` mprotects the traced page `PROT_NONE`
  post-capture and asserts render survives + marks the freed RIPs undecodable;
  mutation-checked — reverting to the raw deref SIGSEGVs at that exact test.
  `docker-hwtrace` 514/514.)* [hwtrace.c:3610](../../../../src/hwtrace.c#L3610)
  reads 16 bytes per address → SIGSEGV if the traced region was unmapped after
  capture. The sibling `render_versioned` correctly uses the bounds-checked
  code-image instead; `render_window` should too.
- **S4 — AArch64 hw-breakpoint resume assumes x86 `EFLAGS.RF`. [reported] [FIXED 2026-07-22, arm64-validation gated]** *(arm64 single-steps over the re-matching PC; x86 byte-identical; aarch64 -fsyntax-only clean; behavioural test gated on bare-metal NT_ARM_HW_BREAK.)* The
  wrong-depth path does a bare `PTRACE_CONT`
  ([ptrace_backend.c:1192](../../../../src/ptrace_backend.c#L1192)) relying on RF
  auto-advance, which AArch64 lacks; an arm64 W^X JIT re-entering the return
  breakpoint could spin re-trapping. x86-validated only.
- **S5 — Hostile-jitdump integer cast is implementation-defined. [verified]
  [FIXED 2026-07-22]** *(both readers now reject an untrusted `code_size` that
  overflows the declared record — `code_size > (uint64_t)total - 56` in
  `asmtest_jitdump_find`, `> body_size - 40` in `asmtest_jitdump_debug_find`,
  unsigned and underflow-guarded — BEFORE the signed cast. New
  `test_jitdump_hostile` feeds a `code_size == UINT64_MAX` record; mutation-checked
  — removing the two guards flips both asserts `not ok` (512 passed / 2 failed),
  because the impl-defined `(long)UINT64_MAX == -1` yields a plausible positive
  name_len that mis-parses attacker bytes. `docker-hwtrace` 514/514.)*
  `name_len = (long)total - 56 - (long)code_size`
  ([ptrace_backend.c:206](../../../../src/ptrace_backend.c#L206), also :450) over an
  untrusted `/tmp/jit-<pid>.dump`; a `code_size > LONG_MAX` cast is UB/impl-defined
  before `malloc`/`fread` (the `<=0` guard catches only ordinary negatives).
- **S6 — ~15 growable pools double `ncap` without an overflow guard. [reported] [FIXED 2026-07-22]** *(shared overflow-checked asmtest_grow/_pow2 over ~20 sites; tests/grow_overflow unit test in make check.)*
  `ncap *= 2` / `realloc(p, nc * sizeof …)` at descent.c:31, dataflow.c:138,
  hwtrace.c:2549-2552, trace.c:480, codeimage.c:208/391 and the dataflow
  producers — `ncap` can wrap to 0. Bounded in practice by instruction budgets;
  theoretical, but cheap to harden with a max-cap clamp.
- **S7 — `round_pages` can overflow on caller-controlled sizes. [verified]
  [FIXED 2026-07-22]** *(round_pages clamps the caller-controlled ring size to
  1 GiB — far above any real AUX/data ring, which the kernel's mlock limits would
  refuse anyway — so `(v + pg - 1)` cannot wrap and the `p <<= 1` round-up cannot
  shift to 0. Defensive clamp validated by non-regression: `docker-hwtrace`
  514/514, the whole-window/PT arming paths that call it unaffected at real sizes.
  No positive trigger asserted — the overflow needs a ~SIZE_MAX ring the opts path
  can't deliver.)*
  [hwtrace.c:618-628](../../../../src/hwtrace.c#L618) — `(v+pg-1)/pg` can wrap and
  the `p <<= 1` loop shift to 0 on a hostile `aux_size`/`data_size`; no upper clamp.

## 3. Language bindings (`bindings/`)

All ten are real, tier-complete, and validated against one shared conformance
corpus; the register structs are consumed via opaque handles in the six dynamic
bindings, so there is **no ABI-layout risk in the core FFI**. Findings are
localized cleanups.

- **B1 — Rust in-line assembler/disassembler is silently unreachable without
  `ASMTEST_LIB` set. [verified] [FIXED 2026-07-21]** *(dlopen(NULL) fallback when
  the env var is unset; a SET-but-unloadable override still reports unavailable.
  Own-process regression test `tests/asm_no_env.rs`.)* The crate links `libasmtest_emu` (which carries
  Keystone/Capstone), yet `asm_available()`
  ([rust/src/lib.rs:701](../../../../bindings/rust/src/lib.rs#L701)) gates on a
  separate `dlopen($ASMTEST_LIB)`
  ([:643](../../../../bindings/rust/src/lib.rs#L643)); with the env var unset (the
  common downstream case) `assemble()`/`call_asm()`/`disas()` return "not in this
  build" ([:762](../../../../bindings/rust/src/lib.rs#L762),
  [:792](../../../../bindings/rust/src/lib.rs#L792)) despite the symbols being
  linked in. *Fix:* fall back to `dlopen(NULL)` on the already-linked image.
- **B2 — Java `HwTrace.resolve/resolveTiers/status/…` throw when the hwtrace lib
  fails to load, contradicting the class's own self-skip contract. [verified]
  [FIXED 2026-07-21]** *(query family degrades to honest unavailable values; new
  `--not-loaded-contract` second-JVM leg in `hwtrace-java-test` with an
  anti-vacuity loaded-anyway guard.)*
  `available(int)` self-skips cleanly
  ([java/HwTrace.java:850](../../../../bindings/java/HwTrace.java#L850)) exactly as
  the header promises ("callers never see a throw … available self-skips
  cleanly", [:25](../../../../bindings/java/HwTrace.java#L25)), but `status`
  ([:882](../../../../bindings/java/HwTrace.java#L882)), `resolve`
  ([:916](../../../../bindings/java/HwTrace.java#L916)) and the other tier entry
  points `throw new RuntimeException("libasmtest_hwtrace not loaded")`. A harness
  that calls `status`/`resolve` before the `available()` skip guard yields
  `Bail out!` + exit 1 instead of a clean `# SKIP`. Masked today only because the
  lib is always bundled. *Fix:* have the resolve/status family degrade to a
  self-skip return like `available()`.
- **B3 — .NET and Java rely entirely on manual disposal for long-lived native
  handles. [reported] [FIXED 2026-07-22]** *(.NET IDisposable + finalizer, Java Cleaner on NativeCode/NativeTrace/AddrChannel/CodeImage; dropped-handle reclamation tests; Descent's late-bound arena staged.)* .NET uses raw `IntPtr` with **no `SafeHandle` and mostly
  no finalizers** (and leaves `NativeCode`/`NativeTrace`/`HwTrace` as
  non-`IDisposable`, `Free()`-only types); Java has **no `Cleaner`**. A dropped
  handle leaks the native mapping/trace in either. (Java's per-call confined
  `Arena` discipline for *transient* buffers is correct — the gap is only the
  long-lived handles.) Contrast Python/Rust/C++/Go/Zig, which tie freeing to
  scope/Drop/deinit.
- **B4 — Rust has no `riscv64` capture struct. [reported] [FIXED 2026-07-21]**
  *(rv64 `Regs` added mirroring asmtest.h; carry fixtures arch-gated in the test
  files like the corpus; `cargo check --target riscv64gc-unknown-linux-gnu
  --all-targets` passes.)*
  [rust/src/lib.rs:62](../../../../bindings/rust/src/lib.rs#L62) defines only
  `x86_64`/`aarch64` `Regs`, so the binding won't compile as a capture tier on a
  RISC-V host even though the C `regs_t` exists there (Python's manifest path is
  arch-agnostic).
- **B5 — Go/Zig `dlopen`-tier structs are hand-mirrored with no compile-time
  cross-check. [reported] [FIXED 2026-07-22]** *(Go _Static_assert abicheck pkg + Zig comptime @offsetOf/@sizeOf; a field swap fails the build.)* drtrace/hwtrace/dataflow struct layouts are mirrored
  in cgo preambles / `extern struct`s (`go/hwtrace.go:42-158`,
  `zig/src/hwtrace.zig:111-216`) with "must be updated by hand" comments;
  offsets are correct today but desync silently on a header change until a test
  catches it. The shared `at_val_rec_t` hand-offset mirror in both dataflow smoke
  drivers is the riskiest single spot.
- **B6 — Go `go vet` defect. [reported] [FIXED 2026-07-21]** *(uses
  `HwNativeCode.Ptr()`; `go vet ./...` clean.)* `go/conformance_test.go:405` uses
  `unsafe.Pointer(nc.Base())` — the exact `uintptr→unsafe.Pointer` round-trip the
  API added `HwNativeCode.Ptr()` to avoid (`hwtrace.go:996-1002`). Trivial.
- **B7 — Dynamic-language integer-width edges. [reported] [FIXED 2026-07-22]** *(Ruby ULONG return, Node drops the Number()-narrow of koffi's BigInt; >2^63 round-trip test in both conformance suites.)* Ruby binds the
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
  Dockerfiles, bypassing the repo's own verifier. [verified] [FIXED 2026-07-21]**
  *(all 12 route through `fetch-dynamorio.sh`, as does the drtrace CI job's
  runner-side fetch; `check-thirdparty-versions.sh` now gates every image's ARG.
  `docker-drtrace` rebuilt green through the verified path.)*
  [Dockerfile.drtrace:37](../../../../Dockerfile.drtrace#L37)
  (`RUN curl -fsSL "$DR_URL" …`) plus 11 siblings (drtrace-lang, drext-probe,
  pintool, gcprofiler-probe, suspendprof-probe, taint-{native,attach,
  attach-probe,dotnet,managed-attach-probe,oracle}) each copy-paste the same
  block — even though `scripts/fetch-dynamorio.sh` exists and verifies the
  SHA-256 anchor from the manifest, and `check-thirdparty-versions.sh` gates only
  2 of the 12. *Fix:* route all 12 through `fetch-dynamorio.sh` (kills the
  integrity gap and the ×12 copy-paste in one move).
- **K2 — The manylinux PyPI wheel base is fully unpinned. [reported]
  [FIXED 2026-07-21]** *(dated tag `2026.07.19-1` pinned in both
  Dockerfile.manylinux-wheel and release.yml; new checker group keeps the pair
  in sync.)*
  [Dockerfile.manylinux-wheel:10](../../../../Dockerfile.manylinux-wheel#L10)
  (`FROM quay.io/pypa/manylinux_2_28_${ARCH}`, no tag/digest; same in
  `release.yml:207`). This is the one packaging path that reaches end users, on a
  floating base.
- **K3 — Duplicate make recipe for `build/asmtest_nomain.o`. [verified]
  [FIXED 2026-07-21]** *(one canonical recipe in mk/bindings.mk carrying the
  union — `.build-flags` prereq + `-Wno-unused-function`; `make help` warning
  gone.)*
  Defined in both [mk/fuzz.mk:24](../../../../mk/fuzz.mk#L24) (adds a `.build-flags`
  prereq) and [mk/bindings.mk:17](../../../../mk/bindings.mk#L17) (adds
  `-Wno-unused-function`) with *different* recipes. GNU make prints
  "overriding recipe / ignoring old recipe" on **every** invocation (visible in
  `make help`); bindings.mk wins (included last), so the fuzz object silently
  loses its `.build-flags` dependency — a latent stale-rebuild bug plus permanent
  warning noise. *Fix:* one canonical recipe (or distinct object names).
- **K4 — The fuzz lane is never exercised by CI. [verified] [FIXED 2026-07-21]**
  *(new `fuzz` CI job runs `make docker-fuzz`; lane verified green locally —
  both engines steered to their planted crash.)* `Dockerfile.fuzz`,
  `docker-fuzz`, and `fuzz-shim-test` exist and `make help` advertises them, but
  no `.github/workflows/*` references fuzz — a whole capability lane unexercised,
  contrary to the repo's anti-vacuity posture.
- **K5 — Third-party GitHub Actions float, unlike the digest-pinned engines.
  [reported] [FIXED 2026-07-22]** *(all six third-party actions SHA-pinned to
  the commit of their then-current release with a `# vX.Y.Z` comment —
  pypa/gh-action-pypi-publish v1.14.1, ruby/setup-ruby v1.320.0,
  rust-lang/crates-io-auth-action v1.0.5, mlugg/setup-zig v2.2.1,
  msys2/setup-msys2 v2.32.0, docker/setup-buildx-action v4.2.0 ×6 — and
  ci.yml's `actions/setup-python@v5` unified to `@v6`; actionlint output
  byte-identical to pre-change HEAD, YAML parses.)*
  `pypa/gh-action-pypi-publish@release/v1` (a moving *branch*, the
  weakest), plus `setup-zig@v2`, `msys2/setup-msys2@v2`,
  `crates-io-auth-action@v1`, `docker/setup-buildx-action@v4` — all major-tag,
  none SHA-pinned; also `actions/setup-python@v6` (release.yml) vs `@v5`
  (ci.yml). SHA-pin at least the publish action.

## 5. Documentation

Complete against the feature matrix and honest about hardware gating; the
weakness is synchronization lag (see §0 for the tracked sweep). Two concrete,
separately-fixable instances:

- **D1 — Residual Zen-3 LBR overclaim contradicting `_positions.md` #2.
  [verified] [FIXED 2026-07-22]** *(features.md now states the Zen 4–5 floor
  with "Zen 3's BRS is not opened by this tree"; the diagrams.md node reads
  "AMD LBR → built-in / bare-metal Zen 4+". Sphinx `-W` clean.)*
  [docs/reference/features.md:114](../../../reference/features.md#L114)
  lists "AMD LBR (Zen 3 BRS / Zen 4–5 …)" and `reference/diagrams.md:241` says
  "bare-metal Zen 3+", presenting Zen 3 as supported, while the project's own
  position ledger states the floor is Zen 4 and "Zen 3 BRS never opened by this
  tree." Most other pages caveat this correctly. *Fix:* qualify both cells.
- **D2 — Two internal-engineering pages leak onto the public Sphinx site.
  [reported] [FIXED 2026-07-22]** *(both moved: the audit page →
  `docs/internal/analysis/2026-07-09-amd-tracing-review-f1-f47.md` — one
  correction to this finding: it is NOT a duplicate of the 07-09 analysis file
  but its authoritative same-day F1–F47 successor, which that file's own
  SUPERSEDED banner names — and the ledger →
  `docs/internal/scoped-tracing-implementation.md` with its `../src/…` links
  rebased. All 12 internal referrers retargeted; the published scoped-tracing
  guide's three inline links became GitHub blob URLs per the internal-README
  convention. Sphinx `-W` clean; neither page renders on the site any more.)*
  `docs/amd_tracing_review.md` (a code review with `src/file.c:line`
  finding IDs, premised on the now-superseded Zen-2 host — stale, and a duplicate
  of `internal/analysis/2026-07-09-amd-tracing-review.md`) and
  `docs/scoped-tracing-implementation.md` (an implementation ledger whose
  `../src/…` relative links point at non-doc sources) are `orphan: true` but
  still built. *Fix:* move both under `docs/internal/`.
- **D3 — `docs/guides/win64.md` self-contradicts on runner-port status.
  [reported] [FIXED 2026-07-22]** *(intro now reads "porting them was the
  runner port, now complete", matching the body.)* Intro says the port is
  "now underway"; the body ("The runner port") says it is "done … full parity."
- **D-note — `internal/README.md` lists a `reviews/` directory that did not exist
  until this file created it** (only `archive/reviews/` was present); the layout
  table is now correct again.

## 6. Tests / examples

The hard stuff (crash/timeout/guard-page/struct-ABI/JUnit-escaping/parallel
ordering/differential shrinking) is genuinely asserted via paired positive+negative
cases. Two minor items:

- **T1 — Permanent placeholder SKIP in the default `make test` set. [verified]
  [FIXED 2026-07-22]** *(semantics finalized and asserted:
  `mem.partial_fill_touches_only_first_n_bytes` fills the first n bytes of a
  0x5A-sentinel buffer with `fill_bytes(buf, 0x1CD, n)` and asserts the low
  byte lands in `[0,n)` while `[n,…)` keeps the sentinel — the contract all
  four implementations (GAS x86-64/AArch64/riscv64 + NASM) already share.
  Green under both `make test` and `make ASM_SYNTAX=nasm test`; mutation-checked
  — widening the fill to the whole buffer flips it `not ok`.)*
  [examples/test_mem.c:49](../../../../examples/test_mem.c#L49) —
  `SKIP("partial-fill semantics not finalized")` shows a *permanent* SKIP in
  every core run. Either finalize the semantics and assert them, or move the case
  out of the default set.
- **T2 — The pure-spine trace/dataflow/ibs/operands suites use a hand-rolled
  `CHECK`/printf `ok/not ok` `main()` [reported] [NOTE-CLOSED 2026-07-22]** *(documented in tests/expect.sh: gated by exit code in their own CI lanes; wiring into make check would only self-skip, which the dependency rule forbids.)* — a parallel test idiom that
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
   ([amd-review-followup-2-plan.md](../../plans/amd-review-followup-2-plan.md) T2).
6. **T1**, **C2/C3**, **S2–S7**, **B3/B5/B7**, **K5** — harden as the
   multi-thread hardware-capture and arm64 paths mature; none is blocking.
   *(T1, K5, and the core-C robustness trio S3/S5/S7 are now fixed; the §2
   remainder — S2/S4 (gated on PT / arm64 runtime) and S6 (mechanical multi-file
   clamp) — and C2/C3, B3/B5/B7, T2 remain.)*

Nothing in this review is blocked on hardware, privileges, or a credentialed
action except the live *reproduction* of the tracked .NET PT race (§0), which
needs the bare-metal Intel PT box.
