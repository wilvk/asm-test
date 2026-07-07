# asm-test — Call-descent for the out-of-process ptrace tracer: implementation plan

Give the out-of-process single-step tracer (`asmtest_ptrace.h`, `src/ptrace_backend.c`)
**optional levels of descent** into the call-outs it currently steps over at native
speed. Today a traced region's calls to helpers/callees outside `[base, base+len)` —
runtime helpers, GC barriers, PLT stubs, other JIT methods, libc — are run at native
speed to their return and **recorded nowhere** (`classify_region_exit`,
[src/ptrace_backend.c:615](../../../src/ptrace_backend.c#L615); the trace loop at
[src/ptrace_backend.c:855-890](../../../src/ptrace_backend.c#L855-L890)). This plan adds
four levels, each a strict opt-in over the last:

| Level | Name | Behaviour |
|---|---|---|
| 0 | `OFF` | current behaviour: step over call-outs, record nothing (the default) |
| 1 | `RECORD_EDGES` | record each `(call-site → callee)` edge, still step over |
| 2 | `DESCEND_KNOWN` | single-step **into** calls whose target resolves (allow-set / resolver); step over the rest |
| 3 | `DESCEND_ALL` | single-step into **every** call incl. runtime internals; denylist + budget + watchdog gated, **default off** |

The motivating case is the `.NET` BCL lane: `System.Console::WriteLine`'s body is two
stepped-over call-outs (`get_Out` @ `0xb`, virtual `TextWriter.WriteLine` @ `0x1e` —
[docs/native-tracing.md:578-604](../../guides/tracing/native-tracing.md)). Levels 1–3 let a caller record
those edges, descend into `get_Out`, or descend the whole tree.

> Status legend: **planned** unless noted. This plan was pressure-tested by an
> adversarial design review; the correctness mitigations in
> [§Correctness core](#correctness-core-the-shadow-stack) and the honesty constraints in
> [§Level 3 safety](#level-3-safety-and-honesty) are **load-bearing**, not optional
> polish. Update this file as phases land, the way
> [inline-asm-keystone-plan.md](inline-asm-keystone-plan.md) tracks its own.
>
> **Status: complete (2026-07-07).** All phases (0–9) are ✅ DONE — the levels,
> conformance/manifest/parity, all-ten binding wrappers, fixtures, and docs landed
> 2026-07-03; the last gap, Phase 5's built-in default denylist
> (`asmtest_descent_use_default_denylist`), landed 2026-07-07. All six open
> decisions are ratified below. The two remaining limitations (signal-frame SP-pop
> suspension, tail-call keep-open) are documented deferrals, not pending work.

This plan is a **sibling** of the
[zen2-singlestep-trace-plan](../../plans/zen2-singlestep-trace-plan.md) (which shipped the W2
out-of-process stepper this extends) and the
[hardware-trace-plan](../../plans/hardware-trace-plan.md) (the foreign-JIT resolution toolkit
descent consumes). Descent is **Linux x86-64 + AArch64 only** — the two arches the
ptrace tracer supports; RISCV64/ARM32 are disasm-only and never ptrace targets.

---

## Design overview

Three decisions carry the whole design:

1. **`asmtest_trace_t` stays byte-for-byte ABI-frozen.** The canonical trace model
   ([include/asmtest_trace.h:44](../../../include/asmtest_trace.h#L44)) records offsets from
   **one** region base — it structurally cannot represent nested callee frames. All ten
   bindings consume it through the opaque handle + `asmtest_emu_trace_*` accessors (none
   mirror the struct layout), and a machine-readable manifest (`asmtest_abi.json`) pins
   its field offsets. So descent adds a **new, separate opaque handle** rather than
   touching the trace struct.

2. **One new opaque handle, `asmtest_descent_t`, read only through accessor functions.**
   It carries config **in** (level, depth ceiling, instruction budget, allow-set,
   optional resolver/denylist) and records **out** two things: level-1 **edges**
   `(call_site_off, callee_addr, from_frame, depth)` and level-2/3 nested **frames**
   (each a self-contained trace whose offsets are relative to *that* frame's base). This
   matches the established opaque-handle idiom every binding already speaks, so per-binding
   work is *adding scalar accessor calls, not laying out a struct*.

3. **New `_ex` entry points; the three existing symbols become NULL-descent wrappers.**
   `asmtest_ptrace_trace_call_ex` / `_trace_attached_ex` / `_trace_attached_versioned_ex`
   thread the descent handle through the existing fork / attach / versioned loops. The
   current `asmtest_ptrace_trace_call` etc. delegate with `descent == NULL`, reproducing
   today's trace exactly. No existing symbol changes signature.

### Frozen API surface (Phase 0 output)

The design review surfaced **three mutually incompatible edge/frame ABIs** across the
sketches (opaque accessors vs. a caller-supplied edge sink + `on_frame` callback vs.
accessors bolted onto the trace handle) and two `_ex` signatures. **Phase 0 freezes one
surface before any header, test, corpus case, or binding declaration is written.** The
chosen surface (rationale: keeps `asmtest_trace_t` frozen, needs no cross-FFI upcall for
frame delivery, and never hands out a borrowed owning pointer that bindings would
double-free):

```c
/* include/asmtest_ptrace.h — new declarations */

typedef enum {
    ASMTEST_DESCENT_OFF           = 0, /* step over, record nothing (default)        */
    ASMTEST_DESCENT_RECORD_EDGES  = 1, /* record (call_site -> callee), step over    */
    ASMTEST_DESCENT_DESCEND_KNOWN = 2, /* step INTO resolvable calls; else step over */
    ASMTEST_DESCENT_DESCEND_ALL   = 3, /* step INTO every call; denylist+budget gated */
} asmtest_descent_level_t;

typedef struct asmtest_descent asmtest_descent_t;   /* opaque; full type in src/descent.c */

/* Optional level-2/3 resolver: 1 => descend (and, if known, *base_out/*len_out); 0 => step over. */
typedef int (*asmtest_descent_resolver_fn)(uint64_t callee_addr, void *user,
                                           uint64_t *base_out, uint64_t *len_out);
/* Optional level-3 denylist: 1 => refuse descent into callee_addr. */
typedef int (*asmtest_descent_denylist_fn)(uint64_t callee_addr, void *user);

asmtest_descent_t *asmtest_descent_new(asmtest_descent_level_t level);   /* + setters below */
void asmtest_descent_free(asmtest_descent_t *d);                         /* idempotent: NULLs internally */
void asmtest_descent_set_max_depth(asmtest_descent_t *d, uint32_t max_depth);
void asmtest_descent_set_insn_budget(asmtest_descent_t *d, uint64_t budget);      /* 0 => conservative default */
void asmtest_descent_set_watchdog_ms(asmtest_descent_t *d, uint32_t ms);          /* 0 => conservative default */
int  asmtest_descent_allow_region(asmtest_descent_t *d, const void *base, size_t len);  /* L2 allow-set */
int  asmtest_descent_deny_region(asmtest_descent_t *d, const void *base, size_t len);   /* L3 denylist  */
void asmtest_descent_set_resolver(asmtest_descent_t *d, asmtest_descent_resolver_fn fn, void *user);
void asmtest_descent_set_denylist(asmtest_descent_t *d, asmtest_descent_denylist_fn fn, void *user); /* L3 */

/* Read accessors (opaque-handle idiom, one scalar per call — safe for every FFI): */
size_t   asmtest_descent_edges_len(const asmtest_descent_t *d);
uint64_t asmtest_descent_edge_site(const asmtest_descent_t *d, size_t i);   /* call-site off in its frame */
uint64_t asmtest_descent_edge_target(const asmtest_descent_t *d, size_t i); /* absolute callee address    */
uint32_t asmtest_descent_edge_depth(const asmtest_descent_t *d, size_t i);
size_t   asmtest_descent_frames_len(const asmtest_descent_t *d);
uint64_t asmtest_descent_frame_base(const asmtest_descent_t *d, size_t f);
uint64_t asmtest_descent_frame_len(const asmtest_descent_t *d, size_t f);
uint32_t asmtest_descent_frame_depth(const asmtest_descent_t *d, size_t f);
int32_t  asmtest_descent_frame_parent(const asmtest_descent_t *d, size_t f);
size_t   asmtest_descent_frame_insn_count(const asmtest_descent_t *d, size_t f);
uint64_t asmtest_descent_frame_insn_at(const asmtest_descent_t *d, size_t f, size_t i);
size_t   asmtest_descent_frame_block_count(const asmtest_descent_t *d, size_t f);
uint64_t asmtest_descent_frame_block_at(const asmtest_descent_t *d, size_t f, size_t i);
int      asmtest_descent_truncated(const asmtest_descent_t *d);      /* a pool overflowed */
int      asmtest_descent_depth_capped(const asmtest_descent_t *d);   /* stopped by policy, distinct from overflow */

int asmtest_ptrace_trace_call_ex(const void *code, size_t len, const long *args, int nargs,
                                 long *result, asmtest_trace_t *trace, asmtest_descent_t *descent);
int asmtest_ptrace_trace_attached_ex(pid_t pid, const void *base, size_t len,
                                     long *result, asmtest_trace_t *trace, asmtest_descent_t *descent);
int asmtest_ptrace_trace_attached_versioned_ex(pid_t pid, const void *base, size_t len,
                                               struct asmtest_codeimage *img, uint64_t when,
                                               long *result, asmtest_trace_t *trace,
                                               asmtest_descent_t *descent);
```

Per-frame **scalar** accessors (`frame_insn_at` …) are canonical rather than a borrowed
`asmtest_descent_frame_trace()` returning an `asmtest_trace_t*`: the review showed that
handing bindings a borrowed pointer collides with their **owning** `Trace` wrappers
(C++ `~HwTrace` calls `trace_free`; Rust `Drop`; .NET `Dispose`; …) → double-free / UAF.
Frame 0 is always the root registered region, so the descent handle is a superset of the
flat `asmtest_trace_t` view. A convenience borrowed-view accessor **may** be added later
behind a documented non-owning wrapper, but is out of the first cut.

**Descent decision, per call-out** (implemented in the loop, not the header):

```
OFF            -> step over (run_until), record nothing
RECORD_EDGES   -> append edge, step over
DESCEND_KNOWN  -> descend iff depth < max_depth AND (target ∈ allow-set OR resolver()==1)
                          AND budget remains; else append edge + step over
DESCEND_ALL    -> descend iff depth < max_depth AND no denylist hit AND budget remains;
                          else append edge + step over
```

Every non-descended call still records an edge at level ≥ 1, so the edge list is a
complete record of *un-followed* calls even when depth/budget/allow gating declines a
descent.

---

## Correctness core: the shadow stack

Descending changes the loop's termination condition from "the region was exited" to
"control returned to depth 0". That requires an explicit **return-address shadow stack**,
and the adversarial review found the naïve version wrong in several realistic ways. These
mitigations are requirements, not nice-to-haves.

- **`read_pc_ret` must read SP (and AArch64 x30).** Today it fetches only PC + `rax`/`x0`
  ([src/ptrace_backend.c:322-339](../../../src/ptrace_backend.c#L322-L339)). The pop
  predicate is unimplementable without SP (`regs.rsp` / `regs.sp`) and, on AArch64, the
  callee-entry link register (`regs.regs[30]`). **Prerequisite for all of L2/L3.**

- **Exact pop predicate, not a loose inequality.** Pop frame *T* iff
  `PC == frame[T].ret_addr` **AND** `SP == frame[T].sp_at_call + slot` (x86 `slot = 8`;
  AArch64 `slot = 0`, identity is `(entry_lr, sp)`) **AND** the just-stepped instruction
  `is_ret`. Capture `sp_at_call` at the **pre-call** stop (the caller's SP before the
  push), so post-return `SP` compares equal naturally.

- **SP-sweep pop for non-local exits.** `longjmp`, C++ exception unwind, and `sigreturn`
  move SP across **many** frames at once without hitting any `ret_addr`, stranding frames
  (inflating `depth`/`frame_count`, finalizing phantom-open frames as truncated). At every
  out-of-current-frame stop, first pop **all** frames whose `sp_at_call < current_SP`
  regardless of PC, then re-classify PC against the new top. The `ret_addr` match becomes a
  confirmation, not the sole trigger.

- **Same-region recursion is a distinct frame — and fixes a latent level-0 bug.** A
  recursive region *R* calling *R* keeps PC inside `[base, base+len)`, so the exit
  dispatcher never runs, offsets fold into frame 0 duplicated, and `normalize`
  ([src/ptrace_backend.c:452](../../../src/ptrace_backend.c#L452)) emits a spurious block
  boundary at the re-entered offset — wrong **even at level 0 today**. Detect a call whose
  target lands within the current frame's own range (via `is_call` on the last in-frame
  insn) and push a new frame with the same base and a deeper `sp_at_call`. Add a
  per-identical-base recursion cap so unbounded recursion cannot exhaust `ASMTEST_MAX_FRAMES`;
  set a `recursion-collapsed` truncation bit when hit.

- **Forward benign signals; never abort on them.** The fork loop currently **kills** the
  tracee on any non-SIGTRAP ([src/ptrace_backend.c:696-706](../../../src/ptrace_backend.c#L696-L706));
  the attached loop sets overflow and breaks ([:849-853](../../../src/ptrace_backend.c#L849-L853)).
  During descent this is both a correctness bug and unsafe: managed runtimes use
  `SIGSEGV` (GC/null-check safepoints), `SIGURG` (preemption), `SIGPROF` (sampling) as
  routine control signals. Re-inject the signal via the `PTRACE_SINGLESTEP` data arg
  exactly as `run_until` already does ([:548](../../../src/ptrace_backend.c#L548)), and
  suspend SP-pop evaluation while PC is inside the kernel-built signal frame (below all
  `sp_at_call`).

- **Timer-driven watchdog, backend-owned.** A step-cadence check ("every N steps") cannot
  preempt a tracee blocked in a descended `read`/`futex`/`mmap` syscall — `waitpid` blocks
  and CI **hangs**, not truncates. The backend must own a real `CLOCK_MONOTONIC` deadline
  via `timer_create`/non-`SA_RESTART` `SIGALRM` (or a timed wait so a blocked `waitpid`
  returns `EINTR`), independent of the harness `alarm()` that only `jit_trace` installs
  today. On expiry: fork path `kill`+reap; attached path `PTRACE_DETACH`/`CONT` to leave
  the target resumable — **never** `run_until` into runtime code from a deep frame.

- **Tail-call is a documented limitation, handled to preserve forward progress.** A callee
  ending in `jmp` (not `call`) leaves its frame with SP unchanged; do **not** pop at
  `top>0` when `PC != ret_addr` and SP has not risen — record an edge and `run_until` the
  tail-callee's return, keeping the frame open until its real return, so a subsequent
  genuine return cannot match the wrong frame.

> **Deferred in the first cut (2026-07-03) — known limitations, both on the branch-D
> catch-all pop.** The [2026-07-03 call-descent review](../../analysis/2026-07-03-call-descent-review.md)
> surfaced that two of the mitigations above are only *partially* realized:
> - **Signal-frame SP-pop suspension is NOT implemented.** On the live/attached path
>   (`forward_faults=1`) an async managed-runtime signal (a `SIGSEGV` null-check safepoint,
>   `SIGPROF`, `SIGURG`) delivered while inside a descended frame lands the PC on the handler
>   entry — out of the frame's region, SP in the low kernel signal frame (so the SP-sweep
>   does not fire) — and branch D pops the frame, and one step later mis-reads a root return
>   with a garbage value. The fork path forwards benign signals but its controlled fixtures
>   install no handlers, so this bites only live-runtime descent — which is already documented
>   best-effort/expected-to-perturb. Not yet mitigated; do not rely on descended traces across
>   a live safepoint.
> - **Tail-call keep-open is NOT implemented.** A descended method ending in a tail `jmp`
>   pops early instead of following to the tail-callee's return. The first cut adds a
>   *defensive* mitigation only — clearing the parent's pending-call state on every frame
>   push — so a tail-jump degrades to **honest truncation** rather than a mis-parented /
>   corrupted frame tree. Full keep-open (record an edge + `run_until` the real return) is
>   future work.

---

## Phases

Ordering is dependency-driven; **Phase 0 blocks everything.**

### Phase 0 — Freeze the shape *(decision, no code)* — ✅ **DONE (2026-07-03)**

**Goal.** Reconcile the conflicting sketches to the one surface in
[§Frozen API surface](#frozen-api-surface-phase-0-output). **Deliverables:** the frozen
header signatures; the single source filename `src/descent.c` (sibling of `src/trace.c`);
the decision that per-frame scalar accessors are canonical (no borrowed trace pointer); the
resolver is optional and the **allow-set is the universal path**; L3 is **default-off**.
**Acceptance:** this section of the plan is ratified and the [open decisions](#open-decisions-for-the-user)
are answered. **Effort:** 0.5 day.

### Phase 1 — Arch/read prerequisites — ✅ **DONE (2026-07-03)**

Landed: `read_pc_ret` now also returns SP + AArch64 x30 ([src/ptrace_backend.c](../../../src/ptrace_backend.c));
`asmtest_disas_is_ret` + `asmtest_disas_call_target` in [src/disasm.c](../../../src/disasm.c),
declared in the non-parity-gated [asmtest_trace.h](../../../include/asmtest_trace.h); the AArch64
`NT_ARM_HW_BREAK` `set_hw_bp`/`clear_hw_bp` wired into `run_until` as the W^X fallback
(x86-64 debug-register path unchanged). Unit tests `test_disas_queries` cover both arches
cross-arch (11 checks, green on x86-64 live and aarch64-under-qemu). AArch64 object
cross-compiles clean; the live hardware-breakpoint trap awaits a real arm64 host (qemu-user
exposes no debug-register slots, so `set_hw_bp` fails and the caller self-skips to edges-only).

**Goal.** Land the primitives descent's loop needs. **Deliverables:**
- Extend `read_pc_ret` ([src/ptrace_backend.c:322](../../../src/ptrace_backend.c#L322)) to
  also return SP (x86 `regs.rsp` / AArch64 `regs.sp`) and AArch64 entry-LR (`regs.regs[30]`).
- `asmtest_disas_is_ret(arch, code, len, off)` — Capstone `CS_GRP_RET` (x86 `ret`/`retf`,
  AArch64 `ret`), mirroring `asmtest_disas_is_call` ([src/disasm.c:128](../../../src/disasm.c#L128)).
- `asmtest_disas_call_target(arch, code, len, base, off, *target)` — direct-call target
  (x86 `E8` rel32 via `X86_OP_IMM`; AArch64 `bl` imm26); returns "indirect" for
  `call r/m`/`blr` so the loop uses the live post-step PC instead.

- **AArch64 hardware-breakpoint path (`NT_ARM_HW_BREAK`)** — *in scope, per decision.* Add
  an AArch64 `set_hw_bp`/`clear_hw_bp` via `PTRACE_SETREGSET`/`NT_ARM_HW_BREAK` (debug
  breakpoint control + value registers `DBGBCR`/`DBGBVR`), so `run_until`/`run_to` gets the
  same W^X fallback x86-64 already has ([src/ptrace_backend.c:485](../../../src/ptrace_backend.c#L485)).
  This closes the parity gap: L2/L3 descent works on a W^X JIT heap on AArch64 instead of
  degrading to edges-only. Gate on a runtime probe (`ptrace(PTRACE_GETREGSET, …,
  NT_ARM_HW_BREAK)` reporting ≥1 breakpoint slot) and self-skip where the kernel/host has none.

**Acceptance:** unit tests for `is_ret`/`call_target` on both arches; `read_pc_ret` returns
correct SP for a known fixture; the AArch64 hardware breakpoint plants and traps on a real
arm64 host (the arm64 docker lane; self-skip under qemu-user, which does not emulate debug
registers). **Header home & parity treatment of the two disasm queries is an
[open decision](#open-decisions-for-the-user)** — `asmtest_trace.h` (where `is_call` lives,
but **not** in the parity gate's `TIER_HEADERS`), `asmtest_ptrace.h` (parity-enforced →
forces 10-way wrapping), or C-internal. Default: keep them C-internal until a binding
consumes them. **Effort:** 3 days (was 1; +2 for the AArch64 `NT_ARM_HW_BREAK` path).

### Phase 2 — Handle + accessors, no loop changes yet — ✅ **DONE (2026-07-03)**

Landed: [src/descent.c](../../../src/descent.c) (growable edge/frame pools + the ~20 scalar
accessors + the internal mutators the loop drives), [include/asmtest_descent_internal.h](../../../include/asmtest_descent_internal.h)
(private handle layout, not a tier header), the frozen public surface in
[asmtest_ptrace.h](../../../include/asmtest_ptrace.h), and the three `_ex` entry points as
level-0-only wrappers (descent handle reads back empty). Wired into `HWTRACE_OBJS` + the
`pic`/shared-lib lists in [mk/native-trace.mk](../../../mk/native-trace.mk). (`scripts/amalgamate.sh`
covers only the core `asmtest.c` surface, not the hwtrace tier — ptrace_backend.c is already
excluded — so descent.c is correctly *not* added there.) `test_descent_handle` green (8
checks); compiles clean on x86-64 and aarch64. The 10-way binding decls + parity land with
Phase 7 (header intentionally ahead of bindings until then, in one change).

**Goal.** The `asmtest_descent_t` handle exists and reads back empty; `_ex` entry points
exist as `descent==NULL` wrappers so parity/manifest/tests stay green while the loop is
still level-0-only. **Deliverables:** `src/descent.c` (alloc/free/append/dedup + edge &
frame pools + the ~20 read accessors, reusing `trace_append_insn/block` discipline from
[src/trace.c](../../../src/trace.c)); declarations in `include/asmtest_ptrace.h`; wire
`src/descent.c` into `HWTRACE_OBJS` (Makefile) **and** `scripts/amalgamate.sh`'s source
list. **Critical:** the parity gate (`scripts/check-bindings-parity.sh`, which greps
`asmtest_ptrace.h`) turns every new symbol into a required declaration in **all 10
bindings** — so the header, all 10 binding decls, and any
`scripts/bindings-parity-allow.txt` exemptions land in **one** change. Never merge the
header ahead of the bindings. **Acceptance:** `make hwtrace-test` + `make check-bindings-parity`
green with the NULL-descent wrappers. **Effort:** 2 days C + folded into Phase 7's 10-way work.

### Phase 3 — Level 1 `RECORD_EDGES` — ✅ **DONE (2026-07-03)**
### Phase 4 — Level 2 `DESCEND_KNOWN` — ✅ **DONE (2026-07-03)**
### Phase 5 — Level 3 `DESCEND_ALL` + guards — ✅ **DONE (2026-07-03; default denylist 2026-07-07)**

> **2026-07-07 follow-up:** the one Phase-5 deliverable that had not shipped — the
> **built-in default denylist** (GC-entry / JIT-compile / PLT-`ld.so`-resolver /
> known-blocking libc) — is now `asmtest_descent_use_default_denylist()`: populated
> from the tracee at trace start (`/proc/<pid>/maps` module matching for
> ld-linux/ld-musl/ld.so, `[vdso]`/`[vsyscall]`, and the CoreCLR/Mono/HotSpot/ART/
> V8/BoehmGC runtime modules; dlsym-resolved blocking libc/pthread entry points on
> the fork path, where the tracee shares the tracer's layout — an attached foreign
> process gets the mapping-based set only). Wrapped in all ten bindings; the
> fork-path fixture asserts a call landing exactly on `poll` is stepped over as an
> edge, not descended. L3 safety now rests on default-off + budget + watchdog +
> the built-in denylist, as this phase originally promised.

Implemented as one shadow-stack descending loop (`descend_core` in
[src/ptrace_backend.c](../../../src/ptrace_backend.c)) shared by the fork and attached `_ex`
entry points, covering all levels at once. Load-bearing correctness pieces landed: the
pop-phase-first ordering (so a same-region recursion's return — whose address lies inside
the frame — is popped before the in-frame check), the exact `PC==ret_addr && SP==sp_ret &&
last-insn-was-ret` pop predicate with the `SP > sp_ret` non-local sweep, the `sp_ret =
caller-pre-call-SP` derivation (arch-uniform: x86 `ret` and AArch64 epilogue both restore
to it), the `entered` gate, same-region recursion as a distinct frame + recursion/`max_depth`
caps, per-instruction byte windows via `process_vm_readv` (so L3's whole-mapping extent
never needs a huge read), benign-signal forwarding on the live path, the conservative insn
budget, and the **backend-owned ITIMER_REAL/SIGALRM watchdog** (SA_RESTART cleared → a
blocked `waitpid` returns EINTR). Tests: `test_descent_fork` (L1 edges, L2 descend-known +
step-over-unknown, L3 descend-all + denylist + budget, recursion, max_depth, watchdog
self-termination) and `test_descent_attach` (foreign-process L2) — 14 checks, green on
x86-64; ptrace_backend.o cross-compiles clean on aarch64. AArch64 fixtures + the hardware-bp
step-over path land with Phase 8; the conformance corpus fixture with Phase 6.

### Phase 3 — Level 1 `RECORD_EDGES` (detail)

**Goal.** Stop discarding the out-of-region PC at
[src/ptrace_backend.c:856](../../../src/ptrace_backend.c#L856); append edges in the exit
dispatcher on both fork and attached paths, still using `run_until` step-over.
**Deliverables:** edge append (`call_site = stream[n-1]`, `callee = out-of-region PC` or
`call_target` for direct calls); `truncated` on edge-pool overflow. **Acceptance:** new C
test asserts the two `Console::WriteLine`-style edges from a fork fixture; lowest-risk
level, unblocks the `dotnet-bcl` edge assertions. **Effort:** 1 day.

### Phase 4 — Level 2 `DESCEND_KNOWN` + shadow stack + termination

**Goal.** The full descender for resolvable callees. **Deliverables:** the return-address
shadow stack with the exact pop predicate, SP-sweep pop, per-frame byte sourcing (direct
read on fork; `process_vm_readv` on attach; code-image-versioned bytes for re-JIT), the
allow-set + optional resolver gate, `max_depth` ceiling, same-region recursion frame +
cap, benign-signal forwarding, and the unresolvable-extent → record-edge+step-over
fallback so the loop always makes progress. Supersede `classify_region_exit` with a
`descend_step_loop`. **Acceptance:** the four-way fork fixture (below) asserts L2 descends
into the known sibling as a nested frame while an unknown libc call is still stepped over;
`max_depth`, recursion cap, and truncation all covered on x86-64 and AArch64.
**Effort:** 5–6 days (highest complexity).

### Phase 5 — Level 3 `DESCEND_ALL` + guards *(default off)*

**Goal.** Descend into everything, safely bounded and honestly documented. **Deliverables:**
denylist; **conservative default insn budget (~4096, not `PTRACE_STREAM_CAP`=65536)**; the
backend-owned real-time watchdog (blocked-syscall escape); a **default denylist** covering
GC-entry / JIT-compile / PLT-`ld.so`-resolver / known-blocking libc; an explicit opt-in
gate **beyond** the level number; per-step sanity check that PC/`ret_addr` still lie in a
known executable mapping (self-truncate, not crash, when GC/re-JIT moves code); clean
detach-not-`run_until` unwind on live targets. Ships **with** the honesty docs (§below) or
not at all. **Acceptance:** fork-path fixture descends a libc `memcpy` call; a **guarded L3
live lane** (`jit_trace *-descend-all`, per decision) runs against a real runtime with the
default denylist + conservative budget + watchdog and self-skips honestly when it trips a
guard — asserting the guards fire, not that L3 is transparent; with the Phase-1 AArch64
hardware-breakpoint path, AArch64 W^X L3 now descends rather than self-skipping to
edges-only (self-skip remains only where the host reports no debug-register slots).
**Effort:** 4–5 days (was 3–4; +1 for the guarded live-runtime L3 lane + its self-skip
discipline).

### Phase 6 — Conformance + manifest + parity — ✅ **DONE (2026-07-03)**

Added a `ptrace_descent` tier to `run_corpus()` + `emit_corpus()` in
[conformance.c](../../../bindings/conformance/conformance.c) (a `calls_leaf` in-blob
R→sibling-S fixture, x86-64 + AArch64 bytes; self-skips off a single-step host / arch
mismatch), linking the ptrace backend + descent + Capstone into `$(BUILD)/conformance`. The
generated `corpus.json` carries two cases (L1 edges, L2 descend) with a per-case `arch` gate.
Manifest stays structs-only (no `descent_symbols` emitter, per plan). The parity gate rides
the existing header-grep — **`check-bindings-parity.sh` is green: 78 tier symbols × 10
bindings in sync**. L3 kept out of the corpus. All 10 bindings replay L1/L2 or SKIP.

### Phase 6 — Conformance + manifest + parity (detail)

**Goal.** Pin descent behaviour cross-language. **Deliverables:** a new `ptrace_descent`
tier added to `run_corpus()` **and** `emit_corpus()` in
[bindings/conformance/conformance.c](../../../bindings/conformance/conformance.c) (the
generated `corpus.json` is rebuilt by `make conformance` — **never hand-edit it**), plus a
`calls_leaf` two-routine fixture in
[bindings/conformance/corpus_routines.c](../../../bindings/conformance/corpus_routines.c) whose
call target is an **in-blob sibling** (a rel32 call cannot reach libc from an `mmap`'d
page — keeps emitted bytes host-portable on both arches). A **per-binding ptrace self-skip
gate** mirroring `test_ptrace_callout`'s `asmtest_ptrace_available()` so qemu-user / yama /
sandboxed docker report SKIP not FAIL. Keep **L3 out of the corpus**. Manifest: the
current `gen-manifest.c` emits only struct offsets and has no enum/symbol block, and the
parity gate does **not** read the manifest — so descent's symbol enforcement rides the
existing header-grep parity gate, and the manifest stays structs-only (adding a
`descent_symbols` emitter is explicitly **out of scope**). **Acceptance:** `make conformance`
regenerates `corpus.json`; all 10 bindings replay the L1/L2 cases or SKIP. **Effort:** 2 days.

### Phase 7 — Per-binding wrappers + smoke tests (all 10) — ✅ **DONE (2026-07-03)**

A `Descent` wrapper + descending `trace_call_ex` (with an explicit **region** arg — the
fixture's caller region is a sub-range of the allocation) + L1/L2 smoke tests + a
`ptrace_descent` conformance handler landed in **all ten** bindings, with idempotent free and
the per-FFI hazards handled (Node BigInt getters, Lua boxed-`uint64` cdata, Ruby unsigned
mask, .NET `GCHandle`/Java `upcallStub`-Arena/Node `koffi.register`/Lua `ffi.cast`/Go
`runtime.Pinner`/Python `CFUNCTYPE` upcall pinning). The resolver ships to the six upcall-safe
FFIs (Python/Go/Node/Java/.NET/Lua) with a resolver smoke test each; Rust/Ruby/C++/Zig are
allow-set-only and exempt `set_resolver`/`set_denylist` in
[bindings-parity-allow.txt](../../../scripts/bindings-parity-allow.txt). Parity green. Validated
live per binding (descent smoke + conformance replay). **Also corrected a pre-existing
cross-binding drift** surfaced here: commit `10bd4ed` switched the single-step block model to
ends-at-branch (the loop is 3 blocks `{0,0x7,0xf}`, matching the C reference) but the binding
loop smoke tests still asserted 2 — fixed to 3 across all ten (aborting on that stale
assertion had been blocking the descent smoke checks from running in some bindings).

### Phase 7 — Per-binding wrappers + smoke tests (detail)

**Goal.** A `Descent` surface in every binding. Per binding: declare the new native
entry points + accessors via that binding's FFI, add a `Descent` object (level enum,
`allow_region()`, `edges()`, `frames()`, per-frame insn/block views), mirror the existing
trace-handle lifetime discipline with **idempotent free** (NULL after free so
finalizer + explicit free cannot double-free), and one L1 + one L2 smoke test.

| Binding | File(s) to edit | FFI | Binding-specific hazard to fix |
|---|---|---|---|
| Python | `asmtest/_native.py`, `asmtest/hwtrace.py`, `tests/test_hwtrace.py` | ctypes | resolver upcall OK (`CFUNCTYPE`) |
| Go | `hwtrace.go`, `hwtrace_test.go` | cgo + dlopen | `//export` + `runtime.Pinner` if resolver used; moving-GC region ptrs |
| Rust | `src/hwtrace.rs`, `tests/hwtrace.rs` | extern-C fn ptrs | no capturing-closure upcall → **allow-set only** |
| Node | `hwtrace.js`, `test_hwtrace.js` | koffi | **address getters return `BigInt`** (JS `Number` rounds >2^53 ASLR addrs) |
| .NET | `hwtrace/HwTrace.cs`, `HwTraceTest.cs` | P/Invoke | `GCHandle.Alloc` + `GC.KeepAlive` if resolver used |
| Ruby | `hwtrace.rb`, `test_hwtrace.rb` | Fiddle | **mask signed `TYPE_LONG_LONG` → unsigned** for addresses; **allow-set only** |
| Lua | `hwtrace.lua`, `test_hwtrace.lua` | LuaJIT ffi | **address getters return boxed `uint64` cdata**, never `tonumber()` |
| Zig | `src/hwtrace.zig`, `src/hwtrace_test.zig` | std.DynLib | allow-set path |
| Java | `HwTrace.java`, `HwTraceTest.java` | FFM/Panama | keep `upcallStub` Arena open if resolver used |
| C++ | `asmtest_hwtrace.hpp`, `test_hwtrace.cpp` | dlopen/dlsym | **non-owning `FrameView`** holding a `Descent` ref (never `trace_free`) |

Per the scope decision, the **resolver callback ships in the first cut** — offered to the
six upcall-safe FFIs (ctypes/cgo/koffi/FFM/P-Invoke/LuaJIT), each with the per-binding
lifetime pinning in the table's hazard column so the GC can't move/collect the trampoline
mid-single-step (`GCHandle.Alloc`+field on .NET, an open `upcallStub` Arena on Java,
`runtime.Pinner`+kept `//export` on Go, a retained `koffi.register` handle on Node, an
anchored `ffi.cast` on LuaJIT, a kept `CFUNCTYPE` on Python). Rust and Ruby cannot host a
safe upcall, so they expose the **allow-set only** — which remains the universal path and
the one the conformance corpus exercises (**the corpus stays callback-free**). A resolver
smoke test is added only to the six upcall-safe bindings; all ten test the allow-set.
**Acceptance:** each binding's smoke test passes or self-skips; a >2^53 synthetic callee
address round-trips exactly on Lua and Node (pinned by a corpus/parity assertion); the six
resolver-capable bindings prove a resolver upcall fires and is not collected under GC
pressure. **Effort:** 6–7 days (was 5–6; +1 for first-cut resolver pinning across six
bindings and its lifetime tests).

### Phase 8 — Fixtures, real-runtime harness, cascade invariants — ✅ **DONE (2026-07-03)**

`examples/test_hwtrace.c` gained the fork-path descent fixtures (L0-L3, `max_depth`, budget,
recursion, watchdog self-termination), the attached-path descent test, and the **cascade
invariants** (`test_descent_cascade`: frame-0 body byte-identical across L1/L2/L3; higher
levels add only directly-reachable frames) plus the **honest limitation** test (a known region
behind a stepped-over intermediary is NOT recorded). `jit_trace` gained `<mode>-descend` (L2)
/ `<mode>-descend-all` (L3, guarded, asserts the guards fire) for the dotnet / dotnet-bcl /
java lanes, wired to `mk/native-trace.mk` `hwtrace-jit-*-descend[-all]` + `mk/docker.mk`
`docker-hwtrace-jit-*-descend[-all]` targets. C suite green (132/132) on x86-64; the AArch64
descent stream + hardware-breakpoint step-over path and the live `docker-hwtrace-jit-*-descend`
runs await their respective hardware/runtime (the lanes self-skip like the existing jit lanes;
the aarch64 `calls_leaf` fixture is carried in the conformance corpus + compiles clean).

### Phase 8 — Fixtures, real-runtime harness, cascade invariants (detail)

**Goal.** Deterministic C coverage + live proof. **Deliverables:**
- New `examples/test_hwtrace.c` fork-path tests beside `test_ptrace_callout`
  ([~line 1232](../../../examples/test_hwtrace.c)): a two-routine `descent_blob` — region *R*
  calling a known sibling *S*, region *M* calling libc `memcpy` — asserting **level 0**
  (top body only), **level 1** (edge recorded, callee not stepped), **level 2** (descends
  into *S* as a nested frame; the libc call still stepped over), **level 3** (descends the
  libc call too), plus `max_depth`, budget/truncation, recursion, and the AArch64 variant,
  on **both** the software-int3 and hardware-breakpoint step-over paths.
- `jit_trace` modes `dotnet-descend` / `dotnet-bcl-descend` (descend `get_Out` / the
  virtual `WriteLine`) and `java-descend` at **L2** (the default demo lane), plus the
  guarded **`*-descend-all` L3 lane** from Phase 5 that runs with the default denylist +
  budget + watchdog and asserts the guards fire; `mk/native-trace.mk` `hwtrace-jit-*-descend`
  / `hwtrace-jit-*-descend-all` + `mk/docker.mk` `docker-hwtrace-jit-*-descend[-all]` targets,
  all watchdog-bounded and self-skipping honestly like the existing lanes.
- Cascade property checks with **P1 weakened** to the honest form: *frame-0 body is
  byte-identical across all levels; higher levels add only **directly-reachable** frames/edges*.
  The strong `recorded(L2) ⊆ recorded(L3)` claim is **false** — a known region reachable
  only through a stepped-over intermediary is invisible to descent — so a dedicated test
  **pins that limitation** (known region behind an unknown intermediary is *not* recorded).

**Acceptance:** `make hwtrace-test` green on x86-64 and the arm64 docker lane;
`make docker-hwtrace-jit-dotnet-bcl-descend` prints the descended `get_Out` body or
self-skips. **Effort:** 3 days.

### Phase 9 — Docs — ✅ **DONE (2026-07-03)**

Landed: [native-tracing.md](../../guides/tracing/native-tracing.md) "Call descent levels" subsection (4-level
table + edge/frame model + the honesty limits + the `dotnet-bcl-descend` worked-output
pointer); the L3-hazard treatment in [hardware-tracing.md](../../guides/tracing/hardware-tracing.md) and
[analysis/jit-runtime-tracing.md](../../analysis/jit-runtime-tracing.md); the ~27 new symbols in
[api-reference.md](../../reference/api-reference.md); five [glossary.md](../../project/glossary.md) terms (call
descent, call edge, descent level, frame 0, nested frame, shadow stack, step-over vs
step-into); the single-region-invariant reconciliation in [traces.md](../../guides/tracing/traces.md);
per-binding paragraphs ([python.md](../../bindings/python.md), [dotnet.md](../../bindings/dotnet.md));
the [CHANGELOG.md](../../../CHANGELOG.md) entry; and the version bump via
[scripts/sync-version.sh](../../../scripts/sync-version.sh).

### Phase 9 — Docs (detail)

**Goal.** Every doc reflects descent, honestly. **Deliverables** (one section each):
- [docs/native-tracing.md](../../guides/tracing/native-tracing.md) §558-604 — extend the step-over passage
  into a **"Call descent levels"** subsection with the 4-level table, the edge/frame model,
  and a **worked `dotnet-bcl-descend` output** captured from the Phase 8 make target (so it
  lands *last*).
- [docs/hardware-tracing.md](../../guides/tracing/hardware-tracing.md) — managed-runtime descent note + the L3 caveat.
- [docs/api-reference.md](../../reference/api-reference.md) — the ~20 new symbols.
- [docs/glossary.md](../../project/glossary.md) — *descent level*, *call edge*, *nested frame*,
  *shadow stack*, *step-over vs step-into*.
- [docs/traces.md](../../guides/tracing/traces.md) — reconcile the "single-region `asmtest_trace_t` invariant"
  text with nested frames (frame 0 remains the single-region view; descent frames live in
  the separate handle).
- [docs/bindings/*.md](../../bindings/) — one paragraph each (esp. `dotnet.md`, `python.md`).
- [docs/analysis/jit-runtime-tracing.md](../../analysis/jit-runtime-tracing.md) — **why L3 is
  hazardous on a live runtime and when to use it**.
- `CHANGELOG.md`, README/DESIGN cross-links; `scripts/sync-version.sh` version bump.

**Acceptance:** `make docs` builds; the worked example is real captured output. **Effort:** 1.5 days.

---

## Level 3 safety and honesty

The review was blunt: **L3 on a live managed runtime is not "expensive/noisy" — it can
perturb or deadlock the target**, and the plan must default to that truth.

- **Cross-thread lock inversion is a real, not-fully-mitigable deadlock vector.** Attach is
  single-tid ([examples/jit_trace.c:301](../../../examples/jit_trace.c#L301)) — sibling
  CoreCLR/JVM threads run free. Single-stepping (~1000× slower) a helper that holds a lock
  a sibling needs (GC alloc slow-path, JIT-compile helper, `_dl_runtime_resolve` under the
  loader lock, malloc arena) stalls the sibling; a per-thread watchdog cannot break a lock
  the sibling now spins on.
- **GC / re-JIT can move code out from under the stepper**, desyncing cached frame bytes
  and invalidating shadow-stack `ret_addr`s. Require code-image versioning for *any*
  descended frame on the live path, and self-truncate (never crash) on a PC/executable-mapping
  mismatch.
- **Blocking syscalls park `waitpid` indefinitely** — the backend-owned timer is the only
  escape.

**Hard constraints, not prose:** L3 is **default off** behind an explicit opt-in beyond the
level number, and documented as **best-effort / expected-to-perturb-or-deadlock**. Per the
scope decision, L3 **is permitted on a live runtime** — but only behind the opt-in, with the
default denylist (GC/JIT/PLT/`ld.so`/blocking-libc) + conservative instruction budget +
backend-owned watchdog, and with the plain statement that **the bounding is best-effort: the
cross-thread lock-inversion deadlock vector is not fully mitigable** while sibling threads
run free. The safe-by-construction targets remain the **fork path** (controlled
single-threaded callee), a **paused / single-threaded / post-mortem** target, or a live
runtime whose sibling threads the caller has frozen (`PTRACE_SEIZE` the whole thread-group —
itself not free of deadlock risk if a frozen thread holds a runtime lock). The guarded live
`jit_trace *-descend-all` lane exists to exercise this path and is expected to self-skip
(never hang, never corrupt) when a guard trips.

L2's step-over-unknown **is** a sound default and mostly delivers "keep descent out of
internals" — with the residual, documented caveat that L2 still single-steps the full body
of each known method (including its calls up to the step-over point), so the allow-set /
resolver must be **method-exact**, never broad module ranges that re-admit runtime glue.

**AArch64:** today `run_to` is software-`brk`-only ([src/ptrace_backend.c:517](../../../src/ptrace_backend.c#L517)),
so on a W^X JIT heap a callee's return breakpoint cannot be planted. Per the scope decision,
**Phase 1 adds the `NT_ARM_HW_BREAK` hardware-breakpoint path**, giving AArch64 the same W^X
fallback as x86-64 — so L2/L3 descent works on a W^X JIT heap on AArch64. The edges-only
self-skip now applies **only** where the host reports no debug-register breakpoint slots (or
under qemu-user, which does not emulate them); that residual case is asserted distinctly so
the AArch64 CI lane doesn't read as a regression.

---

## Open decisions (for the user)

**Decided (2026-07-01):**

1. ✅ **Resolver ships in the first cut** for the six upcall-safe FFIs (with per-binding
   pinning); Rust/Ruby stay allow-set-only; the corpus stays callback-free. *(Phase 7.)*
2. ✅ **L3 is permitted on live runtimes**, default-off behind an explicit opt-in, guarded by
   denylist + conservative budget + watchdog, and documented as expected-to-perturb — the
   cross-thread deadlock vector is stated as not-fully-mitigable, not designed away. *(Phase 5,
   §Level 3 safety.)*
3. ✅ **The AArch64 `NT_ARM_HW_BREAK` hardware-breakpoint path is in scope** (Phase 1),
   closing the W^X descent-parity gap; edges-only self-skip survives only where no
   debug-register slot exists / under qemu-user.

**Decided (2026-07-03, Phase 0 ratified):**

4. ✅ **`is_ret` / `call_target` live in `asmtest_trace.h`**, next to the existing
   `asmtest_disas_is_call` / `asmtest_disas_is_branch`. That header is *not* in the parity
   gate's `TIER_HEADERS`, so the two new queries carry **no binding/parity cost** (the
   "C-internal" intent) yet stay non-`static` so the Phase-1 unit tests can link them.
5. ✅ **Frame 0 also fills the flat `asmtest_trace_t`** whenever a `trace` is passed —
   byte-identical to today's single-handle behaviour — *and* is mirrored as descent frame 0.
   The descent handle is thus a strict superset of the flat view; existing single-handle
   callers and every current test are unaffected.
6. ✅ **The edge list records only stepped-over (un-followed) calls.** A descended call is
   represented by its nested **frame**, not also duplicated as an edge; the edge list stays
   the complete record of calls the tracer did *not* follow (per §Design overview).

**Phase-0 refinement:** the frozen surface declared the `asmtest_descent_denylist_fn`
typedef but no setter to install it, so Phase 0 adds `asmtest_descent_set_denylist(d, fn,
user)` for symmetry with `asmtest_descent_set_resolver` (L3-only; Rust/Ruby exempt it in
`bindings-parity-allow.txt` like the resolver).

---

## Effort and risk

**Effort.** ~28–32 engineer-days end to end: Phase 0 (0.5) · Phase 1 (3) · Phase 2 (2) ·
Phase 3 (1) · Phase 4 (5–6) · Phase 5 (4–5) · Phase 6 (2) · Phase 7 (6–7) · Phase 8 (3) ·
Phase 9 (1.5). The scope decisions add ~4 days over the recommended-default baseline (+2
AArch64 `NT_ARM_HW_BREAK`, +1 guarded live L3 lane, +1 first-cut resolver pinning). Phases
3→5 are strictly sequential (each level builds on the last); Phase 7 can overlap Phases 3–6
once Phase 2's symbols are frozen.

**Top risks.** (1) Shadow-stack correctness on non-local control flow — mitigated by the
SP-sweep pop + exact triple predicate + the deterministic fork fixtures; (2) **L3 live-runtime
safety — the decision to permit L3 on live runtimes makes this the sharpest risk**: default-off
+ conservative caps + default denylist + watchdog + honesty docs bound it, but cross-thread
lock inversion is **not** eliminated, so the guarded live lane must self-skip cleanly rather
than hang; (3) the atomic 10-way symbol landing tripping the parity gate — mitigated by
landing header + all bindings + allow-list in one change; (4) Lua/Node 64-bit address
truncation — mitigated by boxed-cdata/BigInt getters pinned by a round-trip conformance
assertion; (5) the first-cut resolver upcall being collected/moved by a binding's GC
mid-single-step — mitigated by the per-binding pinning in Phase 7 and its lifetime tests.
