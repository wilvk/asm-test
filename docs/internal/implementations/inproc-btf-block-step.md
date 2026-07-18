# W3: in-process BTF branch-granular single-step — implementation

> **Sources.** Actioned from
> [zen2-singlestep-trace-plan.md](../plans/zen2-singlestep-trace-plan.md) (the W3
> item, filed forward-look) and
> [amd-tracing-plan.md](../plans/amd-tracing-plan.md) (the headline BTF
> correction and Improvement Phase 2, which shipped the out-of-process form).
> Written 2026-07-17. If this doc and a source disagree, this doc wins (sources
> may be stale); if the CODE and this doc disagree, re-verify before
> implementing.

## Why this work exists

asm-test can already trace a routine one `#DB` per **taken branch** instead of
one per instruction — but only out-of-process, through a forked or attached
ptrace tracee (`PTRACE_SINGLEBLOCK`). The in-process single-step backend
([src/ss_backend.c](../../../src/ss_backend.c)) still pays one fault per
instruction, because branch-granular stepping needs `DEBUGCTL.BTF` (MSR
`0x1D9`, bit 1) — a ring-0 MSR that `EFLAGS`-only user code cannot touch. This
work adds the missing form: an **in-process, no-fork** branch-granular capture
that arms `DEBUGCTL.BTF=1 + EFLAGS.TF=1` through the same thread-pinned
`/dev/cpu/N/msr` route [src/msr_lbr.c](../../../src/msr_lbr.c) already landed,
records one `SIGTRAP` per taken branch, and feeds synthesized `(from,to)` pairs
into [src/amd_backend.c](../../../src/amd_backend.c)'s replay loop — the AMD
LBR waypoint model with **no 16-entry ceiling** and no ptrace child. External
research (see Research notes) settled the open question the plan left: Linux
does **not** preserve a user-written BTF across a context switch, so this tier
is deliberately scoped to a pinned, small-routine envelope with per-trap
re-arm and honest truncation — and a kernel helper is explicitly **not** built.

## What already exists (verified 2026-07-17)

- [src/ss_backend.c](../../../src/ss_backend.c) — the in-process `EFLAGS.TF`
  per-instruction stepper: TLS frame stack, `SIGTRAP` handler that records RIPs
  and re-asserts TF in the saved context (`ss_on_sigtrap`, lines 202–233), the
  `pushfq/orq/popfq` arm/disarm asm (lines 186–197), and the Capstone post-pass.
  `grep BTF src/ss_backend.c` has **zero hits** — no in-process branch-granular
  form exists anywhere in `src/`.
- [src/ptrace_backend.c](../../../src/ptrace_backend.c) — the shipped
  **out-of-process** BTF block-step entry points:
  `asmtest_ptrace_blockstep_available` (line 1543, wrapping the hang-proof
  functional probe `probe_singleblock`, lines 1509–1541),
  `asmtest_ptrace_trace_call_blockstep` (line 1708),
  `asmtest_ptrace_trace_attached_blockstep` (line 1887), and the windowed
  foreign-memory form `asmtest_ptrace_trace_attached_windowed_blockstep`
  (line 2380, declared in
  [include/asmtest_ptrace.h](../../../include/asmtest_ptrace.h) line 174). Its
  reconstruction core — `br_kind_t` (1557–1564), `classify_branch`
  (1571–1593), `bs_scan_terminator` (1623–1665), `bs_record_run` (1669–1689),
  `blockstep_reconstruct` (1695–1706) and the `BS_FAIL/BS_OK/BS_AMBIGUOUS`
  codes (1552–1554) — is all `static` to that TU today. Crucially, that core
  has **two** consumers of the low-level classifier, not one: the region
  reconstructor `bs_scan_terminator` AND its foreign-memory twin
  `window_scan_terminator` (line 2283, feeding `window_block_walk` at line 2343,
  which serves the windowed entry point above). Both call `classify_branch` and
  switch on `br_kind_t`/`BR_*` and the `BS_*` codes; `blockstep_reconstruct`,
  its two callers (1808/1818, 1974/1978), `window_block_walk` and its caller
  (2465/2469) all reference `BS_*` too. So any extraction of
  `classify_branch`/`br_kind_t`/`BS_*` must keep them visible to **both**
  consumers — T1 handles this explicitly.
- [src/msr_lbr.c](../../../src/msr_lbr.c) — the "same bucket" precedent that
  DID land: static `msr_rd`/`msr_wr`/`open_msr` helpers over `/dev/cpu/N/msr`
  (lines 49–69), `asmtest_amd_msr_available` (74–85), and the thread-pinned
  capture shape — pin via `sched_setaffinity`, save MSR state, arm, `run_fn`,
  disarm, decode, restore (110–215).
- [src/amd_backend.c](../../../src/amd_backend.c) — the replay loop this tier
  feeds: `amd_replay` (line 256) walks registered bytes between `(from,to)`
  waypoints. The right public entry for an unbounded synthesized sequence is
  `asmtest_amd_decode_stitched` (line 708) — unlike `asmtest_amd_decode`
  (line 530), it does **not** flag `truncated` when `nbr >=`
  `asmtest_amd_lbr_depth()` (the 16-deep check at lines 525–527), and it takes
  a `gap` honest-loss parameter.
- [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h) — the
  declaration pattern to mirror: the `asmtest_amd_msr_trace` /
  `asmtest_amd_msr_available` block at lines 616–635 (callback-thunk capture +
  pure substrate probe). Status codes: `ASMTEST_HW_OK/EINVAL/EUNAVAIL/ENOSYS/`
  `EFULL/EDECODE` (lines 52–58).
- [mk/native-trace.mk](../../../mk/native-trace.mk) — build wiring to mirror:
  the `msr_lbr.o` rule (line 1994), `HWTRACE_OBJS` (2047–2053), the
  `test_hwtrace` link (2058–2059), `hwtrace-test` (2128), the PIC rule (2758),
  `NATIVE_TRACE_OBJS` (2775–2779), and the `shared-hwtrace` prerequisite list
  (2784–2799).
- [mk/docker.mk](../../../mk/docker.mk) — `docker-hwtrace-msr` (lines
  548–558): the `--privileged` lane that exposes `/dev/cpu/N/msr` and runs
  `make hwtrace-test`. This tier rides that exact lane; **no new Docker lane
  is needed**.
- [examples/test_hwtrace.c](../../../examples/test_hwtrace.c) — the test
  patterns to mirror: `test_amd_msr` (line 1230, self-skip + live assertions),
  `test_amd_msr_spec_filter` (line 1275, host-independent pure-helper test,
  with the extern-declaration idiom at line 68), `test_singlestep_live`
  (line 2091, the `ROUTINE` fixture whose exact stream is
  `[0x0,0x3,0x6,0xc,0x11]`), `test_singlestep_loop` (line 2145, the 16-byte
  `jnz` `LOOP` fixture), and `main` (line 8132).
- [scripts/bindings-parity-allow.txt](../../../scripts/bindings-parity-allow.txt)
  — lines 129–131 allowlist `asmtest_amd_snapshot_trace` /
  `asmtest_amd_msr_trace` / `asmtest_amd_msr_available` as C-level-only
  symbols with no per-binding FFI. The new symbols follow the same posture.

**Prove the baseline is green before touching anything.** On any host with
Docker: `make docker-hwtrace` — expect every test to print `ok`/`# SKIP …` and
the run to exit 0. On this macOS host additionally `make hwtrace-test` runs the
Darwin subset (single-step live, PT/AMD self-skips) and must exit 0. `make
help` lists all targets named below.

## Tasks

### T1 — Extract the block-step terminator scanner into a shared TU  (S, depends on: none)

**Goal.** `bs_scan_terminator` and its `classify_branch` helper become callable
from a second TU with zero behavior change to the shipped ptrace block-step
tier.

**Steps.**
1. Create `src/bs_recon.h` (internal header, sibling of
   [src/amd_backend.h](../../../src/amd_backend.h) — do **not** put it in
   `include/`, so the bindings-parity scan never sees it).
2. Create `src/bs_recon.c`. Move from
   [src/ptrace_backend.c](../../../src/ptrace_backend.c): the
   `BS_FAIL/BS_OK/BS_AMBIGUOUS` defines (lines 1552–1554, renamed
   `ASMTEST_BS_*` — see Code), `br_kind_t` + the `BR_*` enum (1557–1564),
   `classify_branch` (1571–1593), and `bs_scan_terminator` (1623–1665). Leave
   `bs_record_run` (1669–1689) and `blockstep_reconstruct` (1695–1706) in
   `ptrace_backend.c` — only the scanner and its classifier move. **Watch the
   second consumer:** `classify_branch`, `br_kind_t`/`BR_*`, and the `BS_*`
   codes are ALSO used by `window_scan_terminator` (line 2283, the
   foreign-memory twin that drives `window_block_walk` at 2343, used by
   `asmtest_ptrace_trace_attached_windowed_blockstep`), which stays behind. So
   these three symbol groups must be **exported from `bs_recon.h`, not hidden
   inside `bs_recon.c`** — the header is what lets the code that stays keep
   using them.
3. In `ptrace_backend.c`: `#include "bs_recon.h"`, delete the moved
   *definitions*, and update **every** remaining reference to the moved symbols
   to the exported forms — miss one and the TU fails to compile:
   - `blockstep_reconstruct` (1695–1706): call `asmtest_bs_scan_terminator`
     (passing `PTRACE_TRACE_ARCH` as the new leading `arch` arg) and change its
     `BS_FAIL`/`BS_AMBIGUOUS` returns to `ASMTEST_BS_*`;
   - its two callers at lines 1808/1818 and 1974/1978
     (`BS_FAIL`/`BS_AMBIGUOUS` → `ASMTEST_BS_*`);
   - `window_scan_terminator` (2283–2325): call the now-exported
     `classify_branch` with `PTRACE_TRACE_ARCH` as its leading `arch` arg (the
     call at 2294), keep switching on `br_kind_t`/`BR_*` (now from the header),
     and return `ASMTEST_BS_*` (2308/2317/2319/2322/2324);
   - `window_block_walk` (2343–2378) and its caller at 2465/2469
     (`BS_OK`/`BS_FAIL`/`BS_AMBIGUOUS` → `ASMTEST_BS_*`).
   (Minimal-churn alternative: leave a `#define BS_FAIL ASMTEST_BS_FAIL` / …
   back-compat alias block in `ptrace_backend.c` so the `BS_*` sites need no
   edit — but you MUST still re-point the `classify_branch` and
   `bs_scan_terminator` *calls* at the exported symbols and add the `arch`
   argument; those are signature changes an alias cannot cover.)
4. Wire the build (see Code), then run `make hwtrace-test` on Linux (or
   `make docker-hwtrace` here) — the block-step tests must pass unchanged.
5. `make fmt` (clang-format is CI-gated via `fmt-check`).

**Code.**
- `src/bs_recon.h` must export **all three** shared symbol groups, because both
  TUs consume them (see step 2): the codes
  `#define ASMTEST_BS_FAIL 0`, `ASMTEST_BS_OK 1`, `ASMTEST_BS_AMBIGUOUS 2`; the
  `br_kind_t` enum with its `BR_*` members (moved verbatim from lines
  1557–1564); the scanner
  `int asmtest_bs_scan_terminator(int arch, const uint8_t *code, size_t len,
  uint64_t base_ip, uint64_t from_off, uint64_t next_pc, uint64_t *term_out);`;
  and the classifier
  `br_kind_t classify_branch(int arch, const uint8_t *code, size_t code_len,
  uint64_t base_addr, uint64_t off, uint64_t next_pc, size_t *len_out);`. The
  only signature change vs. the static originals is the leading `arch`
  parameter (the originals read the TU-local `PTRACE_TRACE_ARCH` macro, which
  does not exist in `bs_recon.c`). `classify_branch` must be **non-`static`**
  (exported), NOT hidden in `bs_recon.c` — `window_scan_terminator`, which
  stays in `ptrace_backend.c`, calls it directly across the TU boundary. Keep
  the whole doc comment from lines 1595–1622 with the scanner — the dual-guard
  `je T; …; je T` ambiguity rule is the load-bearing part.
- `src/bs_recon.c` is pure decode logic over `asmtest_disas*` — no platform
  `#if` guard needed (the disas layer self-stubs).
- [mk/native-trace.mk](../../../mk/native-trace.mk): add a
  `$(BUILD)/bs_recon.o: src/bs_recon.c src/bs_recon.h | $(BUILD)` rule mirroring
  `msr_lbr.o` (line 1994); add `$(BUILD)/bs_recon.o` to `HWTRACE_OBJS`
  (2047–2053) and `NATIVE_TRACE_OBJS` (2775–2779); add a
  `$(BUILD)/pic/bs_recon.o` rule mirroring line 2758 and list it in the
  `shared-hwtrace` prerequisites (2784–2799). Add `src/bs_recon.h` to
  `ptrace_backend.o`'s prerequisite list (2007–2009).

**Tests.** No new test: this is a pure extraction. The regression proof is
`test_ptrace_blockstep` ([examples/test_hwtrace.c](../../../examples/test_hwtrace.c)
line 3923) and `test_ptrace_windowed_blockstep` (line 4524) still passing —
they assert the block-step stream is byte-identical to the per-instruction
tracer. A failure looks like `FAIL: blockstep matches single-step stream`; a
pass is the unchanged `ok` lines and exit 0 from `make docker-hwtrace`.

**Docs.** Internal-only, no user-facing docs — no observable behavior changes.

**Done when.**
- `make docker-hwtrace` exits 0 with the same pass/skip set as before the
  change. This requires `ptrace_backend.o` to compile with BOTH
  `classify_branch` consumers resolving through `bs_recon.h` — the region form
  (`bs_scan_terminator`, now in `bs_recon.c`) and the windowed twin
  (`window_scan_terminator`, still in `ptrace_backend.c`); `test_ptrace_blockstep`
  and `test_ptrace_windowed_blockstep` both pass unchanged.
- No moved symbol dangles in the TU that stays:
  `grep -nE '\bBS_(FAIL|OK|AMBIGUOUS)\b' src/ptrace_backend.c` returns nothing
  on the full-rename path (or only the optional `#define BS_* ASMTEST_BS_*`
  alias lines if you took that shortcut).
- `make shared-hwtrace` links (`build/libasmtest_hwtrace.so` contains
  `asmtest_bs_scan_terminator`: `nm -D build/libasmtest_hwtrace*.so | grep
  bs_scan_terminator` prints one line, on a Linux builder).
- `make fmt-check` passes.

### T2 — `asmtest_ss_btf_available()`: gating + hang-proof functional probe  (M, depends on: none)

**Goal.** A cached probe that returns 1 only where in-process BTF actually
traps branch-granularly — bare-metal x86-64 Linux with `/dev/cpu/N/msr`
writable — and 0 (self-skip) everywhere else, without ever hanging.

**Steps.**
1. Create `src/ss_btf.c` with the file-header comment stating the envelope and
   the research verdict (pinning blocks migration, not preemption; the kernel
   restores BTF only for `TIF_BLOCKSTEP` tasks; BTF is a hardware one-shot —
   cite this doc's Research notes).
2. Guard the real body with `#if defined(__linux__) && defined(__x86_64__)`
   and provide `return 0` / `ASMTEST_HW_ENOSYS` stubs in the `#else`, exactly
   as [src/msr_lbr.c](../../../src/msr_lbr.c) lines 217–229 do.
3. Duplicate the static `msr_rd`/`msr_wr`/`open_msr` helpers from
   `msr_lbr.c` lines 49–69 (they are `static` there; a 20-line deliberate
   duplication, the same way `msr_lbr.c` carries its own copies).
4. Implement the probe (see Code). Declare the symbol in
   [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h) in a new
   comment block directly after line 635, mirroring the
   `asmtest_amd_msr_available` block above it.
5. Add the two new public symbols (`asmtest_ss_btf_available`,
   `asmtest_ss_btf_trace` — declare both now, T3 fills the second in) to
   [scripts/bindings-parity-allow.txt](../../../scripts/bindings-parity-allow.txt)
   as `ALL …` entries modeled on lines 129–131 ("C-level callback capture,
   self-hosted privileged lane only — same posture as asmtest_amd_msr_trace").
6. Wire `$(BUILD)/ss_btf.o` + `$(BUILD)/pic/ss_btf.o` into the same four
   [mk/native-trace.mk](../../../mk/native-trace.mk) lists as T1's object.
7. `make docker-hwtrace` (probe compiles and the — not yet called — symbol
   links), then `make fmt`.

**Code.**
- Constants: `#define SS_BTF_MSR_DEBUGCTL 0x1d9u` and
  `#define SS_BTF_DEBUGCTL_BTF (1ull << 1)` (MSR `MSR_IA32_DEBUGCTLMSR`,
  `DEBUGCTLMSR_BTF` — same bit on Intel and AMD; see Research notes).
- `int asmtest_ss_btf_available(void)`, cached (`static int cached = -1`):
  1. Require `asmtest_disas_available()` (Capstone drives the reconstruction,
     the same gate `asmtest_ptrace_blockstep_available` applies at
     [src/ptrace_backend.c](../../../src/ptrace_backend.c) line 1546).
  2. Pin the calling thread to its current CPU, saving/restoring affinity
     (mirror `msr_lbr.c` lines 117–128), and open that CPU's
     `/dev/cpu/N/msr` `O_RDWR`; failure → 0 (needs `CAP_SYS_ADMIN` + the
     `msr` module, i.e. the `docker-hwtrace-msr` lane).
  3. **Functional probe** (the in-process analogue of `probe_singleblock`,
     ptrace_backend.c lines 1509–1541): mmap an RWX blob of `8 × nop; ret`;
     read-and-save `DEBUGCTL`; install a counting `SA_SIGINFO` `SIGTRAP`
     handler that increments `total_stops`, increments `nop_stops` when the
     saved RIP lies inside the nop run, re-writes `saved|BTF` to the MSR fd
     (`pwrite`, async-signal-safe) and re-asserts TF in `uc_mcontext`; write
     `DEBUGCTL |= BTF` (a failed `pwrite` → restore and return 0 — some
     hypervisors reject the write outright); arm TF with the
     `pushfq/orq $0x100/popfq` sequence copied from `ss_backend.c` lines
     186–191; call the blob; disarm TF (lines 192–197); clear BTF and restore
     the saved `DEBUGCTL`, the prior `SIGTRAP` disposition, and affinity.
  4. Functional iff `total_stops >= 1 && nop_stops == 0`. A BTF-masking
     hypervisor (GitHub-hosted runners, Docker Desktop's VM) silently degrades
     to per-instruction TF stepping — `nop_stops >= 8` — and the probe returns
     0, exactly the degradation signature `probe_singleblock` catches
     out-of-process. Zero stops means TF never armed → 0.
- Hang-proofness is structural: no child, no `waitpid` — worst case is
  per-instruction stepping of a 9-instruction blob.

**Tests.** Covered by T5's live test self-skip path (the probe has no separate
harness — same as `asmtest_amd_msr_available`, which is only exercised through
`test_amd_msr`). Manual check on a non-bare-metal host: `make
docker-hwtrace-msr` prints `# SKIP in-process BTF: …` and exits 0.

**Docs.** Internal-only at this task; T6 documents the user-facing surface.

**Done when.**
- `make docker-hwtrace` still exits 0 (new TU compiles on the plain lane).
- `make docker-hwtrace-msr` on THIS macOS host (Docker's Linux VM masks
  DEBUGCTL.BTF) exits 0 with the T5 test skipping — record the skip line.
- `bash scripts/check-bindings-parity.sh` passes with the two allowlist
  entries.

### T3 — `asmtest_ss_btf_trace()`: the capture + `(from,to)` synthesis into the AMD replay loop  (M, depends on: T1, T2)

**Goal.** One `SIGTRAP` per taken branch around `run_fn(arg)`, post-passed into
the same `asmtest_trace_t` the other tiers fill, honestly `truncated` whenever
BTF persistence cannot be proven.

**Steps.**
1. In `src/ss_btf.c`, implement
   `int asmtest_ss_btf_trace(const void *base, size_t len,
   void (*run_fn)(void *), void *arg, asmtest_trace_t *trace)` — the exact
   callback-thunk signature of `asmtest_amd_msr_trace`
   ([include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h) line 629).
2. Implement the pure pairing post-pass `asmtest_ss_btf_pair_stops` (see
   Code) and declare it in `src/bs_recon.h` under a
   `#if defined(__linux__) && defined(__x86_64__)` guard — the same combined
   guard the T4 extern uses, and the one wrapping its implementation in
   `ss_btf.c` (T2 step 2). It fills `struct perf_branch_entry`, but the guard
   must match where the *definition* exists, not just where the type does: a
   `#if defined(__linux__)`-only declaration would name a symbol with no body
   on non-x86_64 Linux.
3. Run `make docker-hwtrace` after each compiling step; `make fmt` at the end.

**Code.**
- **Capture flow** (mirror `msr_lbr.c` lines 110–215 stage for stage):
  `EINVAL` on NULL/0 args; `EUNAVAIL` unless `asmtest_ss_btf_available()`;
  `ASMTEST_HW_EFULL` if a capture is already armed (a single non-nested
  capture per process — `DEBUGCTL` is physical per-CPU state, so nesting is
  meaningless; guard with a `pthread_mutex_t` + armed flag). Then: pin thread
  (save affinity) → `getrusage(RUSAGE_THREAD, &ru_before)` → open msr fd
  (stash fd and the precomputed `saved_debugctl|BTF` re-arm value in statics
  the handler reads) → `malloc` a stop buffer of `1u<<16` `uint64_t`s (the
  `SS_STREAM_CAP` envelope of `ss_backend.c` line 114) → install the
  `SIGTRAP` handler saving the old disposition → write `DEBUGCTL|BTF` → arm
  TF → `run_fn(arg)` → disarm TF → clear BTF / restore `DEBUGCTL` → restore
  disposition → `getrusage(RUSAGE_THREAD, &ru_after)` → close fd, unpin,
  release the armed flag. Restore everything on every failure path.
- **Handler** (async-signal-safe only, like `ss_on_sigtrap`): if not armed,
  return; bounded-append the absolute RIP from `uc_mcontext.gregs[REG_RIP]`
  to the stop buffer (overflow → flag, drop); `pwrite` the cached
  `saved_debugctl|BTF` back to offset `0x1d9` of the msr fd — **the #DB
  cleared BTF; every trap must re-arm it** (Research notes) — then re-assert
  TF in the saved context (`ss_backend.c` line 232 idiom). TF is clear during
  handler execution (the kernel clears it for the handler's own frame), so
  the handler's branches never recurse.
- **Pairing post-pass** (normal context, pure, exported for the T4 unit test):
  `size_t asmtest_ss_btf_pair_stops(const uint8_t *code, size_t len,
  uint64_t base_ip, const uint64_t *stops, size_t nstops,
  struct perf_branch_entry *out, size_t out_cap, int *gap)`.
  Skip leading stops outside `[base_ip, base_ip+len)` (glue branches between
  arm and the region call; the call INTO the region is itself a taken branch,
  so the first in-region stop is normally offset 0). Then for each adjacent
  stop pair `(prev, s)`: `asmtest_bs_scan_terminator(ASMTEST_ARCH_X86_64,
  code, len, base_ip, prev-base_ip, s, &term)` (T1). `ASMTEST_BS_OK` →
  append `{.from = base_ip+term, .to = s}`; `ASMTEST_BS_AMBIGUOUS` or
  `ASMTEST_BS_FAIL` → set `*gap = 1` and stop pairing (a FAIL here is the
  context-switch signature: a branch whose trap was lost leaves the next stop
  unreachable). The first out-of-region `s` after entry yields the final pair
  (the `ret`/tail-exit edge) and ends pairing. Reverse the array to
  newest-first before returning (the order `amd_replay` consumes;
  `asmtest_amd_stitch` reverses the same way at
  [src/amd_backend.c](../../../src/amd_backend.c) lines 696–701).
- **Decode**: `asmtest_amd_decode_stitched(pairs, n, base, len, trace, gap)`
  ([src/amd_backend.c](../../../src/amd_backend.c) line 708) — chosen over
  `asmtest_amd_decode` because the synthesized sequence is unbounded and the
  16-deep overflow flag at lines 525–527 would falsely truncate it; `gap`
  carries honest loss instead.
- **Honest-truncation belts**, each ORed into `trace->truncated`:
  (1) stop-buffer overflow; (2) `gap` from pairing/decode; (3) **any**
  context switch during the armed window —
  `(ru_after.ru_nvcsw + ru_after.ru_nivcsw) >
  (ru_before.ru_nvcsw + ru_before.ru_nivcsw)` — because a switched-out thread
  without `TIF_BLOCKSTEP` gets no BTF restore (Research notes), so
  completeness is unprovable; (4) zero in-region stops → `truncated = true`,
  `rc = ASMTEST_HW_OK` (the `msr_lbr.c` lines 198–204 "honest, never
  empty-complete" shape).
- **Contract** (state it in the header comment): pure-compute **leaf**
  routines only — no syscalls, no `POPF`/`IRET`, no in-routine signal
  handlers, no calls out of the region (the first region exit ends the
  trace). This is the `ss_backend.c` Phase-3 contract narrowed to the pinned
  BTF envelope; anything richer belongs to the shipped ptrace block-step.

**Tests.** T4 (pure) and T5 (live). A quick manual smoke on an AMD dev box:
`sudo modprobe msr && make hwtrace-test` — the T5 test prints its live lines.

**Docs.** T6.

**Done when.**
- `make docker-hwtrace` exits 0 everywhere (test self-skips off-gate).
- On a bare-metal Linux x86-64 box with the `msr` module:
  `make docker-hwtrace-msr` runs the T5 assertions live (exit 0).
- All failure paths restore `DEBUGCTL`, the `SIGTRAP` disposition, and
  affinity (code-review the four `goto`-style unwind paths against
  `msr_lbr.c`'s `restore:`/`out:` labels).

### T4 — Host-independent unit tests for the pairing post-pass  (S, depends on: T3)

**Goal.** The `(from,to)` synthesis is proven on every CI host with scripted
stop streams — no MSR, no privilege, no specific silicon.

**Steps.**
1. In [examples/test_hwtrace.c](../../../examples/test_hwtrace.c), add
   `static void test_ss_btf_pairing(void)` next to `test_amd_msr_spec_filter`
   (line 1275). Declare the pure helper `extern` at the top of the file the
   way line 68 declares `asmtest_amd_msr_decode_entry` — inside the existing
   `#if defined(__linux__) && defined(__x86_64__)` block (lines 49–89), the
   same combined guard `bs_recon.h` gives it (T3 step 2).
2. Register it in `main` (line 8132) in the early "backend-independent"
   cluster, right after `test_amd_msr_spec_filter();`.
3. `make docker-hwtrace` — the new checks run (not skip) on this host.

**Code / assertions.** Use the 16-byte `LOOP` bytes from
`test_singlestep_loop` (lines 2149–2151: `mov rax,0` at 0x0 [7 bytes];
`add rax,rdi` 0x7; `dec rsi` 0xa; `jnz -8` 0xd → target 0x7; `ret` 0xf), with
a synthetic `base_ip = 0x400000`:
- **Three-trip loop stream**: stops
  `{0x400000, 0x400007, 0x400007, 0xdead0000}` → expect 3 pairs, `gap == 0`,
  and (after the newest-first reversal) oldest pair
  `{from 0x40000d, to 0x400007}` twice, newest `{from 0x40000f, to
  0xdead0000}` (the `ret` exit edge; `bs_scan_terminator` resolves a
  ret-terminated run as `BR_HARD_UNKNOWN` → OK).
- **Ambiguity**: hand-assembled `je +N; nop…; je +N; ret` bytes where both
  `je`s share one target and the stop lands there → expect `gap == 1` and
  pairing stops (the dual-guard rule inherited from T1's scanner).
- **Desync**: a stop address no terminator can reach → `gap == 1`, prefix
  count only — the lost-trap (context-switch) signature.
- **End-to-end pure decode**: feed the three-trip pairs to
  `asmtest_amd_decode_stitched` against the `LOOP` bytes and assert the
  reconstructed `insns` total and `!truncated` — mirroring how
  `test_amd_reconstruction` (line 270) validates `amd_replay`
  host-independently.

A failure prints the standard `FAIL:` line from the file's `CHECK` macro; a
pass adds new `ok` lines to `make docker-hwtrace` on every host including this
one.

**Tests.** This task *is* the test — the deliverable is `test_ss_btf_pairing`
itself; see **Code / assertions.** above. There is no separate harness.

**Docs.** None (test-only).

**Done when.**
- `make docker-hwtrace` exits 0 and the new `CHECK` descriptions appear in
  its output on this macOS host's Docker (i.e. the test does NOT skip).
- Deliberately breaking the reversal (comment it out locally) makes the
  oldest/newest-order assertion fail — proving the test has teeth — then
  restore it.

### T5 — Live parity test riding the privileged MSR lane  (S, depends on: T3)

**Goal.** On bare-metal x86-64 Linux with `/dev/cpu/N/msr`, the BTF tier's
live stream is byte-identical to the per-instruction stepper's; everywhere
else the test self-skips with a printed reason.

**Steps.**
1. Add `static void test_ss_btf_live(void)` and
   `static void test_ss_btf_loop(void)` to
   [examples/test_hwtrace.c](../../../examples/test_hwtrace.c) after
   `test_amd_msr` (line 1230), and call them in `main` right after the
   existing `test_amd_msr()` call (find it with `grep -n "test_amd_msr()"
   examples/test_hwtrace.c`).
2. Update the `docker-hwtrace-msr` comment in
   [mk/docker.mk](../../../mk/docker.mk) (lines 548–553) to name the
   in-process BTF tier as a second consumer of the lane. No rule change: the
   lane already runs `make hwtrace-test` under `--privileged`.
3. `make docker-hwtrace-msr` here (expect the SKIP), then on an AMD dev box
   (expect live passes).

**Code / assertions.**
- `test_ss_btf_live`: skip-print unless `asmtest_ss_btf_available()`
  (pattern: `test_amd_msr` lines 1232–1236, reason text
  `needs bare-metal x86-64 + /dev/cpu/N/msr + CAP_SYS_ADMIN; hypervisors mask
  DEBUGCTL.BTF`). Copy the `ROUTINE` mmap/`mprotect` setup from
  `test_singlestep_live` (lines 2099–2107); drive `fn(20,22)` through a
  static thunk (the `msr_run_loop` shape, line 1219). Assert `rc ==
  ASMTEST_HW_OK`, result 42, and honest coverage
  (`insns_total > 0 || truncated`); when `!truncated`, assert the exact
  5-instruction stream `[0x0,0x3,0x6,0xc,0x11]` and the `{0, 0x11}` block
  pair — the same `EXPECT` array as line 2124, which IS the cross-backend
  parity claim.
- `test_ss_btf_loop`: the differentiator — a 20-trip `LOOP` run takes 19
  back-edges, past any 16-entry LBR window; when `!truncated` assert
  `insns_total == 62` (1 + 20×3 + 1, the same 62-step count the W2 plan
  validated) and always assert honesty (`62 || truncated`).
- Both tests must tolerate `truncated` (a context switch mid-region is
  legitimate and flagged, not a bug) — but assert that a truncated run never
  reports the full parity stream as complete.

**Tests.** This task *is* the test — the deliverables are `test_ss_btf_live`
and `test_ss_btf_loop`; see **Code / assertions.** above. On this host they
self-skip; live assertions run only on the bare-metal MSR lane.

**Docs.** None here (T6).

**Done when.**
- This host: `make docker-hwtrace-msr` exits 0 printing
  `# SKIP in-process BTF: …` (the VM masks DEBUGCTL.BTF — the probe, not a
  compile guard, decides).
- Bare-metal AMD box (Zen 5 9950X or Zen 2 4900HS — BTF is baseline AMD64,
  both qualify): `make docker-hwtrace-msr` exits 0 with the live `CHECK`
  lines passing; run it 20× in a loop and confirm any `truncated=1` runs
  still pass (honesty, not flakes).
- `make hwtrace-test` directly on this macOS host still exits 0 (the Darwin
  stub returns 0 → skip).

### T6 — Documentation: guide section, changelog, plan-status corrections  (S, depends on: T3, T5)

**Goal.** A reader of the public tracing guide can find, gate, and correctly
distrust this tier; the two source plans stop calling W3 "forward-look".

**Steps.**
1. [docs/guides/tracing/native-tracing.md](../../../docs/guides/tracing/native-tracing.md):
   add a subsection under `## Single-step tier` (line 346) titled
   "In-process BTF block-step (branch-granular, privileged)": what it is (one
   trap per taken branch, no fork, no 16-entry ceiling), the gates
   (bare-metal x86-64 Linux, `msr` module + `CAP_SYS_ADMIN`, i.e.
   `docker-hwtrace-msr`), the leaf-routine contract, the honest-truncation
   belts (context-switch delta, lost-trap desync, buffer overflow), and a
   short `asmtest_ss_btf_available()` / `asmtest_ss_btf_trace()` snippet.
   Position it in the cascade: below the shipped ptrace block-step (which is
   rootless and switch-proof), above per-instruction in-process TF (which is
   unprivileged but per-insn cost).
2. [CHANGELOG.md](../../../CHANGELOG.md): one entry under `## [Unreleased]` /
   `### Added` describing the tier and its gates (match the existing entry
   style — bold lead, GitHub blob links for internal docs).
3. [docs/internal/plans/zen2-singlestep-trace-plan.md](../plans/zen2-singlestep-trace-plan.md):
   amend the three W3 status sites — the "stays forward-look" sentence at
   lines 154–155, the Phase-5 status bullet at lines 311–312, and the W3
   paragraph at lines 509–521 — from "needs a kernel helper / uapi patch" to:
   landed as the raw-MSR pinned-envelope tier with per-trap re-arm and honest
   truncation; the robust general (context-switch-proof) form remains
   kernel-coupled and already ships as `PTRACE_SINGLEBLOCK`; no non-ptrace
   block-step uapi exists upstream (link this doc).
4. [docs/internal/plans/amd-tracing-plan.md](../plans/amd-tracing-plan.md):
   in Improvement Phase 2's "Why it lives in the W2 ptrace backend" paragraph
   (lines 1052–1062), append one sentence: the EFLAGS-only claim stands, but
   an in-process form via `/dev/cpu/N/msr` has since landed for the pinned
   leaf envelope (link this doc). Do not touch the enum argument — the tier
   is a standalone entry point, not a fifth `asmtest_trace_backend_t` member,
   exactly like `asmtest_amd_msr_trace`.
5. `make docker-docs` — the Sphinx build is `-W` (warnings fail); internal
   docs are excluded from publishing, and any published-page link into
   `docs/internal/**` must be a GitHub blob URL (the CHANGELOG entry style
   already does this).

**Code.** None.

**Tests.** `make docker-docs` exits 0; `make fmt-check` unaffected.

**Docs.** This task IS the docs.

**Done when.**
- `make docker-docs` exits 0.
- `grep -n "forward-look" docs/internal/plans/zen2-singlestep-trace-plan.md`
  no longer matches the W3 sites amended above.
- The changelog entry renders under `## [Unreleased]` / `### Added`.

## Task order & parallelism

- **T1** and **T2** are independent of each other — two people can start
  concurrently.
- **T3** needs both (T1's scanner, T2's probe/arming scaffolding).
- **T4** and **T5** both follow T3 and are independent of each other.
- **T6** last (it documents shipped behavior).

Critical path: `T1 → T3 → T5 → T6` (T2 overlaps T1; T4 overlaps T5).

## Constraints & gates

- **Hardware gate (legitimate self-skip).** Bare-metal x86-64 Linux only:
  hypervisors — GitHub-hosted runners, Docker Desktop's VM on this macOS host
  — mask `DEBUGCTL.BTF`, and the T2 functional probe (not a build flag) is
  what detects that. Per CLAUDE.md this is a real gate: record it in the skip
  string and self-skip. Live validation happens on the AMD dev boxes via
  `make docker-hwtrace-msr`; this Darwin host can only validate the
  self-skip, the pure T4 tests, and compilation.
- **Privilege gate.** `/dev/cpu/N/msr` needs `CAP_SYS_ADMIN` + the host `msr`
  module — the existing `--privileged` `docker-hwtrace-msr` lane
  ([mk/docker.mk](../../../mk/docker.mk) 548–558). No new lane, no new
  container dependency, nothing to add to
  [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt)
  (this feature adds zero third-party code).
- **No kernel helper — a recorded decision, not an omission.** The research
  below shows a robust, context-switch-proof in-process BTF needs the kernel
  to own `TIF_BLOCKSTEP`. That form already exists in-tree as
  `PTRACE_SINGLEBLOCK` (shipped, global position: AMD Part III P2 is LANDED).
  An out-of-tree module is rejected: it cannot be validated in any Docker
  lane (loading a module is a host-kernel change, the same class of gate as
  hardware), and it would duplicate a shipped capability. The raw-MSR tier
  covers only the no-fork niche, honestly.
- **Honesty invariant.** Never emit partial as complete: every unprovable
  window (context switch observed, lost-trap desync, ambiguity, overflow,
  zero in-region stops) sets `truncated`. On AMD validation runs, remember
  the binding global position: `truncated=0` where escalation must fire is a
  regression signal.
- **Formatting/CI.** `make fmt` after every C edit; `make docker-docs` for
  the guide; commit style per repo memory (commit to main and push).

## Research notes (verified 2026-07-17)

Externally verified for this doc; do not re-derive:

- **MSR layout.** `MSR_IA32_DEBUGCTLMSR = 0x000001d9`; `DEBUGCTLMSR_BTF =
  (1UL<<1)` (bit 1); `DEBUGCTLMSR_LBR` is bit 0. AMD APM Vol 2 §13.1.4
  assigns the same bit (`DebugCtl[1]=BTF`) — one code path serves both
  vendors.
  <https://raw.githubusercontent.com/torvalds/linux/master/arch/x86/include/asm/msr-index.h>
- **Linux does NOT preserve a user-written BTF across context switch.**
  `__switch_to_xtra()` touches BTF only inside
  `if ((tifp & _TIF_BLOCKSTEP || tifn & _TIF_BLOCKSTEP) && …)`: a thread
  without `TIF_BLOCKSTEP` is never re-armed after being switched out
  (DEBUGCTL is physical per-CPU state, not saved per-thread), and a
  co-scheduled `TIF_BLOCKSTEP` task's switch actively **clears** a foreign
  BTF. This is why belt (3) truncates on any observed switch.
  <https://raw.githubusercontent.com/torvalds/linux/master/arch/x86/kernel/process.c>
- **The only in-tree TIF_BLOCKSTEP writer is ptrace.** `set_task_blockstep()`
  in `arch/x86/kernel/step.c` (reached via `user_enable_block_step()` ←
  `PTRACE_SINGLEBLOCK`, on a ptrace-stopped tracee, relying on
  `ptrace_freeze_traced()`). No prctl, no perf attr, no procfs knob, and no
  upstream uapi proposal for non-ptrace block-step was found
  (evidence-of-absence from targeted searches, not proof).
  <https://raw.githubusercontent.com/torvalds/linux/master/arch/x86/kernel/step.c>,
  <https://man7.org/linux/man-pages/man2/ptrace.2.html>
- **BTF is a hardware one-shot.** Intel SDM Vol 3B: the processor **clears
  BTF** when it raises the branch-trap `#DB`; the debugger must re-set it
  before resuming. Hence the per-trap `pwrite` re-arm in the T3 handler.
  Kernel commentary corroborates ("#DB clears DEBUGCTLMSR_BTF"; the 2020
  x86/debug fixes existed because a kernel #DB could eat a user-set BTF).
  <https://xem.github.io/minix86/manual/intel-x86-and-64-manual-vol3/o_fe12b1e2a880e0ce-587.html>,
  <https://www.intel.com/content/dam/support/us/en/documents/processors/pentium4/sb/253669.pdf>,
  <https://lkml.kernel.org/lkml/20201027091504.712183781@infradead.org/T/>
- **DEBUGCTL is not generally context-switched**, confirmed independently by
  the Feb-2025 KVM DEBUGCTL series (which also notes KVM suppresses guest
  BTF — the hypervisor-masking behavior the T2 probe detects).
  <https://lists.openwall.net/linux-kernel/2025/02/27/2241>
- **Why `msr_lbr.c` gets away without any of this.** Its LBR capture reads
  frozen state after the fact — no #DB delivery, no cross-switch persistence
  needed. BTF structurally needs per-branch signal delivery AND persistence
  AND re-arming, so the msr_lbr precedent transfers the *access route*
  (`/dev/cpu/N/msr`, pinning, privileged lane), not the *robustness*. Note
  `sched_setaffinity` pinning blocks migration, **not** preemption.
- **Bottom line encoded in this design.** Raw MSR BTF is viable only if the
  region never context-switches and userspace owns the trap/re-arm loop —
  exactly the T3 envelope with belts; anything stronger requires the kernel,
  which the shipped ptrace tier already provides.

## Out of scope

- The **out-of-process** block-step trio and its correctness hardening —
  [ptrace-blockstep-tracer-correctness.md](ptrace-blockstep-tracer-correctness.md).
- AMD LBR capture/docs work, the Zen 4+ floor correction, and the
  `asmtest_amd_freeze_available` retirement —
  [amd-branchsnap-lbr-docs.md](amd-branchsnap-lbr-docs.md); IBS backend
  honesty — [amd-ibs-backend-honesty.md](amd-ibs-backend-honesty.md).
- Whole-window/zero-config scoped-tracing composition that might one day
  cascade into this tier —
  [managed-wholewindow-compose.md](managed-wholewindow-compose.md),
  [zeroconfig-scoped-tracing-hardening.md](zeroconfig-scoped-tracing-hardening.md).
- Any Intel PT arm ([intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md))
  — PT is the continuous-trace answer where it exists; this tier exists
  precisely where it does not.
- Language-binding wrappers for the new symbols: deliberately none, matching
  the allowlisted `asmtest_amd_msr_trace` posture (T2 step 5); revisit only
  if a binding consumer appears.
