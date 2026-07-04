# asm-test — Scoped-tracing shared C/decode core: implementation plan

The **shared** half of the [scoped in-process tracing
plan](scoped-inprocess-tracing-plan.md) — the C-layer work that is built **once**
and reused by all ten language shims, so each per-language track (owned by the
[bindings slice](scoped-tracing-bindings-plan.md) and the [managed
slice](scoped-tracing-managed-plan.md)) stays thin and mechanical.

It covers new-item **3, 4, 5, 6** from the umbrella's
[what-is-new list](scoped-inprocess-tracing-plan.md#what-already-ships-vs-what-is-new):
the two cheap C-layer fixes (§0), the shared render-on-close path (§0.3), per-thread
hwtrace state (§1), the libipt decode-against-self-code-image glue (§2), and the
whole-window completeness refinements — Q2 noise attribution and Q3 snapshot drain
(§3).

> Status legend: **planned** unless noted. Update this file as sub-phases land.

This plan touches only shared C in `src/` + `include/`, its Make objects in
[mk/native-trace.mk](../../mk/native-trace.mk), and its C self-tests in
[examples/test_hwtrace.c](../../examples/test_hwtrace.c) /
[examples/test_codeimage.c](../../examples/test_codeimage.c). No binding code
changes here; the bindings consume the new symbols.

---

## Why a shared core (not per-binding)

The analysis is explicit that almost all the load-bearing machinery is already
shared C and the per-binding delta is "small and repetitive," and it names four
things that must be built once:
[the lifecycle](../analysis/scoped-inprocess-tracing.md#one-shared-core-thin-per-language-shims)
(exists), the **libipt decode-against-self-code-image glue** (the single
highest-leverage shared investment), **per-thread hwtrace state** (second-highest),
and the **two cheap C-layer fixes**. Two more shared gaps it calls out —
**render-on-close** ("a *shared* gap, not a per-language one … belongs in (or just
above) the C core, not re-implemented ten times") and the **arming-thread assert**
("today every binding independently proposes this check") — round out this slice.

Doing them in C means every shim inherits the fix for free, and the existing
region-scoped decoders already do the hard part (dropping out-of-region
instructions at decode — [src/pt_backend.c:108](../../src/pt_backend.c#L108),
[src/ss_backend.c:99](../../src/ss_backend.c#L99)).

---

## §0 — The two cheap C-layer fixes + shared render-on-close *(planned; lands first)*

**Goal.** A small set of self-contained changes that de-risk *every* binding and
unblock the bindings slice's emit-on-close. Cheap enough to land before any shim
work. §0.1–§0.3 are the two C-layer fixes plus render-on-close; §0.4 makes region
registration idempotent by name — the prerequisite that lets a scope object safely
register on **every** construction.

### §0.1 `begin` returns an error when a slot is active

**Today.** `asmtest_hwtrace_begin` silently no-ops on a busy slot —
[src/hwtrace.c:656-658](../../src/hwtrace.c#L656): `if (!g_inited || g_fd >= 0 ||
g_active != NULL) return;` — and also silently no-ops when the name isn't a
registered region (`find_region` returns `NULL`,
[src/hwtrace.c:413](../../src/hwtrace.c#L413), used at
[:659-661](../../src/hwtrace.c#L659)). Every binding today must reinvent a nesting
guard because the C layer gives no signal.

**Change.** Add an error-returning entry point without breaking the shipped `void`
ABI:

- Introduce `int asmtest_hwtrace_try_begin(const char *name)` in
  [include/asmtest_hwtrace.h](../../include/asmtest_hwtrace.h#L152) (next to
  `asmtest_hwtrace_begin`/`end`), returning `0` on success and a negative
  `ASMTEST_HW_*` code otherwise. **There is no literal `EBUSY`/`ENOENT` constant** in
  the header — reuse the existing set
  ([include/asmtest_hwtrace.h:46-52](../../include/asmtest_hwtrace.h#L46)): a **busy
  slot** returns `ASMTEST_HW_ESTATE` (the exact code `asmtest_hwtrace_init` already
  returns for the same wrong-state condition, [src/hwtrace.c:385](../../src/hwtrace.c#L385)),
  and an **unregistered name** returns `ASMTEST_HW_EINVAL` (the bad-argument code,
  [:378](../../src/hwtrace.c#L378) / [:401](../../src/hwtrace.c#L401)).
  `asmtest_hwtrace_auto` already returns these negative codes
  ([src/hwtrace.c:327](../../src/hwtrace.c#L327)). Under §1 the busy-slot case becomes
  legal nesting; the "cannot start" return is then reserved for a **full range stack**
  (`ASMTEST_HW_EFULL`) — see §1 Compatibility.
- Keep `void asmtest_hwtrace_begin(const char *name)` as a thin wrapper that calls
  `try_begin` and discards the result — **no ABI break** for the ten shipped
  shims. The implementation split happens in `asmtest_hwtrace_begin`
  ([src/hwtrace.c:654](../../src/hwtrace.c#L654)).
- The same three dispatch cases (single-step [:663](../../src/hwtrace.c#L663), AMD
  [:669](../../src/hwtrace.c#L669), PT/CS [:674](../../src/hwtrace.c#L674)) must
  each surface their own failure through the new return path (e.g. `perf_open`
  failing at [:684](../../src/hwtrace.c#L684)).

### §0.2 Record the arming thread id in `begin`, assert it in `end`

**Today.** Neither `asmtest_hwtrace_begin` nor `asmtest_ss_begin` captures a thread
id; the "single region, single thread" contract
([src/ss_backend.c:56](../../src/ss_backend.c#L56)) is documented but **not
enforced** — confirmed absent. The analysis makes a same-thread mismatch flag a
**required** posture for every shim (the thread-scope caveat).

**Change.**

- Add a `g_arm_tid` to the active-capture state block
  ([src/hwtrace.c:351-358](../../src/hwtrace.c#L351), alongside `g_fd`/`g_active`),
  set from `syscall(SYS_gettid)` in `begin` (all three backends), cleared in `end`.
  Use the **raw syscall, not the bare `gettid()` wrapper**: that wrapper needs
  `_GNU_SOURCE` (glibc ≥ 2.30), `src/hwtrace.c` does not define it, and under
  `-Werror` a bare `gettid()` is an implicit-declaration build failure on the
  **default** lane. `syscall(SYS_gettid)` matches the file's existing idiom
  (`syscall(SYS_perf_event_open)`, [src/hwtrace.c:173](../../src/hwtrace.c#L173)) and
  needs no new include in `hwtrace.c`.
- Do the capture/compare **once in `asmtest_hwtrace_begin`/`_end`**, which already
  wrap `asmtest_ss_begin`/`_end` — `src/ss_backend.c` is a separate TU with no
  visibility into `hwtrace.c` statics and its `asmtest_ss_end(void)`
  ([src/ss_backend.c:207](../../src/ss_backend.c#L207)) takes no args, so putting the
  check there would force a duplicate tid or a forbidden signature change (add
  `<unistd.h>` + `<sys/syscall.h>` there only if a direct `asmtest_ss_*` caller must
  also be guarded). On a closing-thread mismatch set `trace->truncated = true`
  ([include/asmtest_trace.h:59](../../include/asmtest_trace.h#L59)) rather than
  emitting a partial trace as complete. This is the C half of the Go
  `LockOSThread` / .NET `AsyncLocal` story: the shim can't always prevent the hop,
  but the core will never mislabel it. The flag deliberately errs toward
  **false-truncated over false-complete**; once §1 moves `g_arm_tid` into TLS the
  compare reads the closing thread's own slot (an unset arming tid on a cross-thread
  close still flags `truncated`).
- Expose the arming tid via a read accessor (`int asmtest_hwtrace_arm_tid(void)`)
  so a shim can *also* assert in its own idiom before close.

### §0.3 Shared render-on-close path

**Today — this is a *shared* gap across all ten bindings, not a .NET exception.**
The shared C `end` reconstructs the packet stream into `asmtest_trace_t` *offsets*
for every backend (`asmtest_pt_decode`/`asmtest_cs_decode`,
[src/hwtrace.c:809-811](../../src/hwtrace.c#L809)), but turning those offsets into
disassembly *text* on scope close is unwired in **every** binding — including .NET,
whose scope is `begin`/`try`/`finally`-`end` with no render
([bindings/dotnet/hwtrace/HwTrace.cs:602](../../bindings/dotnet/hwtrace/HwTrace.cs#L602)).
That is exactly why it belongs in the C core: the rendering primitives already
exist (Capstone via `asmtest_disas`, [src/disasm.c:89](../../src/disasm.c#L89)) and
the offsets already sit on `asmtest_trace_t` — only the glue is missing, once.

**Change.** Add one C helper that turns a closed region's recorded offsets into
rendered text, with a **pinned, ABI-stable** signature (bindings hard-depend on it,
so it cannot stay an "or"):

- `int asmtest_hwtrace_render(const char *name, char *buf, size_t buflen)` in
  [include/asmtest_hwtrace.h](../../include/asmtest_hwtrace.h), implemented in
  `src/hwtrace.c`, walking the named region's `asmtest_trace_t` insn offsets and
  calling `asmtest_disas` ([src/disasm.c:89](../../src/disasm.c#L89) — the canonical
  spelling; `emu_disas` in the analysis is a one-line forward to it) against
  `[base, base+len)`. **`snprintf` semantics:** on success it writes up to `buflen-1`
  bytes + NUL and **returns the non-negative total length that would be written** (so
  a caller passes `buf=NULL, buflen=0` to size, then allocates). **Error convention:**
  a name miss (`find_region` NULL) or unavailable decoder (Capstone not compiled in)
  returns a **negative** `ASMTEST_HW_*` code — distinct from the non-negative length,
  so the size-then-allocate idiom is unambiguous. It is the one shape every binding
  can marshal (`char[]`/`byte[]`/`[*]u8`) without a callback FFI. A convenience
  `FILE*` wrapper may be layered *above* it later, but the buffer form is the
  installed primitive.
- **Precondition — the region needs an instruction buffer.** `render` walks the region's
  `asmtest_trace_t.insns[0..insns_len)`
  ([include/asmtest_trace.h:47-49](../../include/asmtest_trace.h#L47)), so the shim must
  register the region with an allocated instruction buffer (`insns != NULL`,
  `insns_cap > 0`); a blocks-only (coverage) trace renders empty text. For the bounded
  native-leaf case the shim sizes `insns_cap` to the routine. For the unbounded
  whole-window / managed case (§2 / Managed slice) the executed count can exceed any fixed
  `insns_cap`, so render must honour `insns_total > insns_cap`
  ([:50](../../include/asmtest_trace.h#L50)) / `trace->truncated`
  ([:59](../../include/asmtest_trace.h#L59)) and label the text a prefix — never present a
  capped stream as complete.
- **This primitive is region-scoped and version-blind:** it renders the live bytes at
  `[base, len)` for a single-owner named region. Concurrent same-name scopes (§1) and
  temporally-recompiled managed code (§D3/§D4, where the executed bytes have since
  tiered/moved) need a **version-/handle-aware** render — the §1 per-scope selection
  and a `(img, when)`-parameterised variant — **not** this primitive. Pinning this
  ABI here must not foreclose that variant.
- Default sink policy for the empty-scope case lives in the shims but is **pinned here
  so ten shims don't diverge:** the empty ctor renders to **stdout**; a file
  (`asmtrace-<member>.txt`) is used only on an explicit `sink:`, and then
  **tid-suffixed** (or `O_APPEND` with a per-scope header line) so concurrent
  same-named scopes (§1) neither clobber nor interleave. The **decode + Capstone
  render** itself is this one C path.
- Must respect the region-scoped model: it renders exactly the in-region offsets
  the decoders already filtered to; whole-window rendering (Core §3 / Managed slice)
  is a separate mode.

### §0.4 Region registration is idempotent by name

**Today.** `asmtest_hwtrace_register_region` **appends unconditionally** —
`hw_region_t *r = &g_regions[g_nregions++]` ([src/hwtrace.c:404](../../src/hwtrace.c#L404)) —
with no dedup, and `find_region` returns the **first** name match
([:413](../../src/hwtrace.c#L413)); the table is fixed at `MAX_REGIONS == 32`
([:337](../../src/hwtrace.c#L337)) and `g_nregions` resets only at init/shutdown. The
shipped model registers a region **once** at setup and `begin`s it many times.

**Why the scope object breaks that.** A self-naming scope registers on **every**
construction under a call-site-constant auto-name (`[CallerMemberName]`+line,
`std::source_location`, …). So a `using (new AsmTrace())` in a loop, or more than 32
scope sites over process lifetime, would either exhaust the 32-slot table
(`register_region` returns `ASMTEST_HW_EFULL`) or, if the shim swallows that, resolve
a **stale** earlier duplicate — `find_region`/`render` alias the first registration's
`asmtest_trace_t`. This is a *shared* correctness gap: fixing it once in the C core is
what makes the bindings slice's register-then-begin pattern safe for all ten shims.

**Change.** Make `asmtest_hwtrace_register_region` **idempotent by name**: on a name
that already has a slot, return that slot (refreshing its `[base, len)`) instead of
appending. Repeated entry of the same auto-named scope then reuses one slot and never
grows `g_nregions`, so looped/sprinkled scopes are safe and the 32-entry ceiling
counts **distinct** scope sites, not entries. A shim has no C-API way to dedup itself —
`find_region` is `static` ([:413](../../src/hwtrace.c#L413)) — so the dedup **must**
live in the core. Behaviour for distinct names is unchanged.

**Thread-safety note (becomes load-bearing under §1).** This idempotent lookup is a
find-then-refresh-or-append **read-modify-write** over the process-global
`g_regions`/`g_nregions`, and `find_region` scans the same array — **neither is
synchronised today** (the registry is a plain `static` array with no mutex/atomic,
confirmed in the tree). That is safe under §0's single-thread MVP, but §1 blesses
concurrent scopes on multiple threads, at which point two first-entries racing on
`g_nregions++` corrupt the table. §1 therefore adds a **registry mutex** (see §1
Changes); the RMW introduced here is written to run under it. Per-thread *capture*
state (§1, TLS) and shared *registry* state (this lock) are distinct concerns — TLS
does not make the registry safe.

**§0 tests.** Extend [examples/test_hwtrace.c](../../examples/test_hwtrace.c)
(dispatched from `main` at [:2174](../../examples/test_hwtrace.c#L2174)):

- `test_try_begin_busy` — with one region active, assert `try_begin` on a busy slot
  returns `ASMTEST_HW_ESTATE`, and `try_begin` on an unregistered name returns
  `ASMTEST_HW_EINVAL`; assert the legacy `void` `begin` still no-ops (ABI unchanged).
  **At §0 there is no range stack yet** (that is a §1 construct — the single
  process-global busy guard `g_active != NULL` is all that exists,
  [src/hwtrace.c:656](../../src/hwtrace.c#L656)), so **provoke the busy path with a
  second `begin` while a slot is active** — the only §0 mechanism. **§1 then rewrites
  this test:** once a second same-thread `begin` *composes* (returns `0`), the refusal
  moves to a **full per-thread range stack** returning `ASMTEST_HW_EFULL`, so the §1
  version fills the stack to its depth bound and asserts `EFULL` instead (see §1
  Compatibility). Runs on **any host** (single-step backend, no hardware needed).
- `test_arm_tid_mismatch` — arm on the main thread, close from a spawned thread,
  assert `truncated` is set. Any host.
- `test_render_singlestep` — single-step-trace a known native leaf, call
  `asmtest_hwtrace_render`, assert the text matches a ground-truth `asmtest_disas`
  of the same bytes (reuse the `test_singlestep_live` fixture at
  [:408](../../examples/test_hwtrace.c#L408)). Any x86-64 Linux.
- `test_register_idempotent` — register the same name twice; assert the second call
  returns the same slot and does **not** advance the region count (§0.4), and that
  registering past `MAX_REGIONS` *distinct* names returns `ASMTEST_HW_EFULL`. Any host.

**§0 docs.** Update
[docs/guides/tracing/hardware-tracing.md](../guides/tracing/hardware-tracing.md)
and the API surface in
[docs/reference/api-reference.md](../reference/api-reference.md) for the three new
symbols; note the `void begin` → `try_begin` relationship in
[docs/guides/tracing/native-tracing.md](../guides/tracing/native-tracing.md).

**§0 effort.** ~3–4 days (§0.4 adds the register-dedup fix). No hardware needed —
validated on any x86-64 Linux via the single-step backend.

---

## §1 — Per-thread hwtrace state *(planned; analysis phase C, before the managed bindings)*

**Goal.** Replace the process-global single capture slot with per-thread state,
lifting the no-nesting / no-concurrency / no-multi-binding MVP limit
([include/asmtest_hwtrace.h:149-151](../../include/asmtest_hwtrace.h#L149),
[src/hwtrace.c:351](../../src/hwtrace.c#L351)) for all ten bindings at once. This
is the header's own named next step ("give each scoping thread its own per-thread
event + AUX ring").

**Why it fits.** PT per-thread mode supports exactly this (an exact thread list,
no inheritance); the decoder already range-filters, so nesting on one thread is
"nearly free" — attribute the one AUX stream to several nested ranges at decode, or
refcount enable/disable across the nest. Single-step needs a TLS range stack.

**Changes.**

- **PT/CoreSight (`src/hwtrace.c`).** Move the active-capture block
  ([:353-358](../../src/hwtrace.c#L353): `g_fd`, `g_base_map`, `g_base_sz`,
  `g_aux_map`, `g_aux_sz`, `g_active`, and the new `g_arm_tid` from §0.2) into a
  `__thread`/`_Thread_local` struct, one perf fd + AUX ring per scoping thread.
  Replace the busy guard ([:656-658](../../src/hwtrace.c#L656)) with a per-thread
  refcount + a small fixed range stack so nested `begin`s on one thread compose
  (innermost range wins at decode). The perf event is already per-thread
  (`pid == 0`, [:684](../../src/hwtrace.c#L684)) so no privilege change.
- **Single-step (`src/ss_backend.c`).** Move the **per-capture** state
  `g_base`/`g_base_ip`/`g_len`/`g_trace`/`g_stream`/`g_stream_len`/`g_overflow`
  ([:59-66](../../src/ss_backend.c#L59)) into TLS (the `g_armed` guard is handled
  separately — see below), and replace the single
  `[g_base_ip, g_base_ip+g_len)` test in the handler
  ([:99-104](../../src/ss_backend.c#L99)) with an async-signal-safe **range stack**
  (a fixed-size TLS array, no allocation in the handler — the analysis is explicit:
  "never `malloc` in the handler"). **Async-signal-safety of the TLS *access* is a
  separate requirement from the range-stack *contents*:** this code builds into a
  `dlopen`'d shared library (`shared-hwtrace`,
  [mk/native-trace.mk:653](../../mk/native-trace.mk#L653)), where the default
  general-dynamic TLS model routes the **first** per-thread access through
  `__tls_get_addr`, which can lazily `malloc` the block — *not* async-signal-safe, and
  the handler's first act is to read `g_armed` ([:88-93](../../src/ss_backend.c#L88)).
  So the handler-touched TLS must be forced to the **initial-exec** model
  (`__attribute__((tls_model("initial-exec")))` — a fixed thread-pointer offset with no
  lazy allocation and no `__tls_get_addr` call at all, accepting the resulting
  static-TLS-surplus constraint on the FFI `dlopen`). **initial-exec is mandatory for
  *every* TLS object the handler dereferences — the range stack and the per-capture
  state:** an *armed* thread's handler reads the range stack on its
  hot path, and general-dynamic `__tls_get_addr` is not async-signal-safe **even for an
  already-faulted-in block** (glibc's lock-free fast path is an optimisation, not a spec
  guarantee, and a SIGTRAP landing mid-`__tls_get_addr` in normal code re-enters it).
  **The `g_armed` "am I stepping" guard is deliberately *not* one of those TLS objects
  (resolving the storage-class question one way):** keep it a **process-global non-TLS
  atomic** (`volatile sig_atomic_t`, as today, [:58](../../src/ss_backend.c#L58)) so an
  **unarmed** thread's handler early-returns without touching TLS at all; a thread's own
  "I am stepping" is then implied by its range-stack depth (`> 0`), itself initial-exec
  TLS. That is a *belt* on the unarmed path, **not** a substitute for initial-exec on the
  range stack an armed thread must still read. (The unqualified "same discipline as the
  existing `g_stream` write" is insufficient — `g_stream` is safe today only because it
  is a plain `static`.) **Bound the initial-exec footprint:** it draws on the small,
  shared static-TLS surplus (~1–2 KiB; exhaustion fails a later `dlopen` with "cannot
  allocate memory in static TLS block"), so the range stack stays **small and fixed** —
  cap it at a shallow depth (e.g. 8 frames) of offsets/pointers only; the 512 KiB
  ordered-RIP buffer stays **heap-`malloc`'d as today** ([:120](../../src/ss_backend.c#L120)),
  never TLS. Where a host still exhausts the surplus, the escape levers are the
  `glibc.rtld.optional_static_tls` tunable or linking the `.so` at startup as a
  `DT_NEEDED` dependency rather than late `dlopen`. **The SIGTRAP disposition stays
  process-wide:** `g_old_sa`/`g_installed` remain process-global `static`s (**not** moved
  to TLS), installed once ([:129](../../src/ss_backend.c#L129)). Today `asmtest_ss_begin`
  **saves-and-installs unconditionally** ([:124-134](../../src/ss_backend.c#L124)) and
  `asmtest_ss_end` restores unconditionally under `g_installed`
  ([:213-216](../../src/ss_backend.c#L214)), with no cross-thread refcount, so §1 must add
  an explicit **process-wide arm-refcount** (incremented in `ss_begin`, decremented in
  `ss_end`) that gates **both sides**: save the caller's original disposition into
  `g_old_sa` and install `ss_on_sigtrap` only on the **0→1** transition, and restore only
  on the **1→0** transition. Gating the restore alone is a bug — a second concurrent
  `ss_begin` would otherwise overwrite `g_old_sa` with asm-test's *own* just-installed
  handler, so the count-0 restore would reinstate that instead of the caller's original.
  The process-global non-TLS `g_armed` guard plus per-thread range-stack state then make
  concurrent scopes on different threads safe.
- **AMD (`src/amd_backend.c` / `hwtrace.c`).** The AMD path shares the `hwtrace.c`
  slot; moving that slot to TLS covers it. Tier-A/Tier-B stitching
  ([src/amd_backend.c:152](../../src/amd_backend.c#L152),
  [src/hwtrace.c:603](../../src/hwtrace.c#L603)) is per-region and unaffected.
- **Registry synchronization (`src/hwtrace.c`).** Moving *capture* state to TLS is
  necessary but not sufficient: the **region registry** (`g_regions`/`g_nregions`,
  [:346-347](../../src/hwtrace.c#L346)) stays process-global and shared, and §0.4's
  idempotent `register_region` is a find-then-append **read-modify-write** over it while
  `find_region` scans it ([:413](../../src/hwtrace.c#L413)) — both unsynchronised today.
  Once this section blesses concurrent multi-thread scopes, that is a data race (two
  first-entries racing `g_nregions++`), so **guard the registry with a process-global
  mutex** (a plain `pthread_mutex` critical section around register/find — registration
  is off the hot capture path, so lock cost is irrelevant; the single-step handler never
  touches the registry, so no async-signal-safety constraint applies to this lock).
- **Per-scope trace ownership + render/close selection.** Moving the *capture* slot
  to TLS is not enough: the region table and its `asmtest_trace_t` stay
  process-global and name-keyed, so two threads entering the **same** auto-named
  scope resolve the same `r->trace` — a silent cross-thread ownership swap, and
  `render(name)` has no defined "which thread's slice." Give each active scope a
  **per-thread (or per-scope-handle) trace slot** and key `end`/`render` on that
  handle, not the bare name: `render` returns the **calling thread's
  most-recently-closed** slice. Auto-names that can run concurrently on one site must
  be tid/counter-disambiguated by the shim. This is the concurrency half of the
  render contract §0.3 deferred — **and §1 owns it.** Pin the handle type and three
  additive entry points (no ABI change to §0.3's name-keyed `render`, which the bindings
  slice keeps consuming; single-owner scopes are unaffected):

  ```c
  /* Opaque per-scope capture handle: an index into this thread's TLS range stack,
   * tagged with a generation counter so a stale/closed handle is rejected. */
  typedef struct { uint32_t idx; uint32_t gen; } asmtest_hwtrace_scope_t;

  /* Handle-producing begin: register-then-begin under `name` (§0.4 idempotent),
   * push a range-stack frame, return its handle in `*out`. Same negative ASMTEST_HW_*
   * returns as try_begin (§0.1); ASMTEST_HW_EFULL when this thread's stack is full. */
  int asmtest_hwtrace_begin_scope(const char *name, asmtest_hwtrace_scope_t *out);

  /* Handle-keyed render — the calling thread's slice for `handle`, version-blind
   * (live bytes at [base,len)). snprintf-style size-then-allocate; negative
   * ASMTEST_HW_* on a stale/unknown handle or unavailable decoder. */
  int asmtest_hwtrace_render_scope(asmtest_hwtrace_scope_t handle,
                                   char *buf, size_t buflen);

  /* Version-aware render — decode `trace`'s offsets against code-image `img` as of
   * `when` (asmtest_codeimage_now), for §D3/§D4's tiered/moved managed bytes. Same
   * snprintf/negative-code convention. */
  int asmtest_hwtrace_render_versioned(asmtest_codeimage_t *img, uint64_t when,
                                       const asmtest_trace_t *trace,
                                       char *buf, size_t buflen);
  ```

  `end`/`render_scope` key on the **handle**, not the bare name, so `render_scope`
  returns the handle's own slice with no "which thread's slice" ambiguity; auto-names
  that can run concurrently on one site are disambiguated by the handle, not by inventing
  a unique name per entry. `try_begin`/`end` (§0.1) stay unchanged for the name-keyed
  single-owner path the bindings slice ships against; the handle-keyed trio is purely
  additive, so §1 does **not** break that ABI.

**Compatibility.** The shipped single-region API must behave identically when only
one thread/one region is used. After §1 a second same-thread `try_begin` **composes**
(nested range) and returns `0`; the "cannot start" return (`ASMTEST_HW_ESTATE` from
§0.1) is **redefined** to fire only when this thread's fixed range stack is **full**,
for which the natural code is `ASMTEST_HW_EFULL`. Because that flips §0's
`test_try_begin_busy` (a second same-thread `begin` no longer refuses), **§1 rewrites
that test**: it provokes the error by **filling the range stack to its depth bound**
(see §0 tests) and asserts `EFULL`, replacing the §0 version's `ESTATE`-via-second-`begin`.
The `try_begin` wrapper itself (§0.1) is unchanged across the transition; only the test's
provocation mechanism and asserted code move (`ESTATE` → `EFULL`).

**§1 tests.** In [examples/test_hwtrace.c](../../examples/test_hwtrace.c):

- `test_nested_singlestep` — two nested `begin`/`end` pairs on one thread over a
  known native routine; assert the inner region's offsets are a subset and the
  outer region still closes correctly. Any x86-64 Linux.
- `test_concurrent_singlestep` — two threads each scope a *different* native leaf
  concurrently; assert each gets its own complete trace and neither trips the other
  (the previous single-slot behaviour would have dropped one). Any x86-64 Linux.
  This is the regression test for the flaky-crash class the Go binding hit.
- `test_concurrent_samename` — two threads scope the **same** auto-named site
  concurrently; assert each thread's `render` returns its own slice (no cross-thread
  aliasing), proving the per-scope trace ownership above. Any x86-64 Linux.
- Keep a `test_singlestep_live`/`test_singlestep_loop` re-run
  ([:408](../../examples/test_hwtrace.c#L408), [:463](../../examples/test_hwtrace.c#L463))
  to prove the single-region path is byte-identical after the TLS migration.

**§1 docs.** Rewrite the MVP-limitation paragraph in
[include/asmtest_hwtrace.h:149-151](../../include/asmtest_hwtrace.h#L149) and the
matching notes in
[docs/guides/tracing/hardware-tracing.md](../guides/tracing/hardware-tracing.md)
from "single process-global slot" to "per-thread, nesting-safe"; update
[docs/reference/features.md](../reference/features.md) and
[docs/reference/portability.md](../reference/portability.md).

**§1 effort.** ~4–6 days. Single-step + AMD reconstruction halves are validated on
any x86-64 Linux; PT per-thread AUX rings validate only on bare-metal Intel PT
(self-skips elsewhere, as the hardware-trace plan already accepts).

---

## §2 — libipt decode-against-self-code-image glue *(planned, forward-look; analysis phase C)*

**Goal.** The remaining new decoder piece: feed the self (`pid == 0`) code-image
recorder's bytes into libipt's image callback so an in-process PT capture of the
*whole window* (not just a pre-registered native range) decodes against the JIT's
own live bytes. This is the **same** glue
[hardware-trace-plan Phase 2](hardware-trace-plan.md#phase-2---attach-to-foreign-jit-tracing-byte-source-recorder-done-pt-attach-decode-forward-look)
needs; building it here unblocks the clean managed path (PT/LBR) for every binding
at once.

**Why it is new.** The recorder and Capstone rendering already exist, and libipt's
image callback is **already wired** — but it is **region-scoped, not
recorder-backed**. [src/pt_backend.c:93](../../src/pt_backend.c#L93) installs
`pt_image_set_callback(image, read_region, &ctx)`, and `read_region`
([:42](../../src/pt_backend.c#L42)) returns `-pte_nomap` for any IP outside
`[base, base+len)` ([:47](../../src/pt_backend.c#L47)), so the decoder stops at the
first out-of-region instruction ([:128-137](../../src/pt_backend.c#L128)). The new
work is to **back that existing callback with the code-image recorder** so it
returns bytes for *any* executed address, and to hand libipt the **full** executed
image set — recorder-tracked JIT pages **plus** the file-backed DSOs enumerable
from `/proc/self/maps`.

**Changes.**

- **Image-source adapter (`src/pt_backend.c`).** Replace/augment the fixed
  `read_region` callback ([:42](../../src/pt_backend.c#L42), registered at
  [:93](../../src/pt_backend.c#L93)) with one backed by
  `asmtest_codeimage_bytes_at(img, addr, when, &out, &out_len)`
  ([include/asmtest_codeimage.h:110-112](../../include/asmtest_codeimage.h#L110)),
  keyed to the trace position (`when`) so the temporal-bytes rule holds — the
  version live *during* the window, per
  [the analysis's correctness rule](../analysis/scoped-inprocess-tracing.md#byte-sources-are-orthogonal-to-all-of-the-above).
  For file-backed regions with no recorder entry, fall back to reading the mapped
  file (resolve via `asmtest_proc_region_by_addr`,
  [include/asmtest_ptrace.h:291](../../include/asmtest_ptrace.h#L291)). Note the
  `asmtest_proc_*` helpers substitute the pid literally into their path
  (`/proc/<pid>/maps`, `/tmp/perf-<pid>.map`), so the self case must pass `getpid()`,
  **not** `0` (unlike `asmtest_codeimage_new(0)`, which maps `0`→self).
- **Mode interaction — a second decode mode, not a mutation of the first.** The shipped
  region-scoped decode *relies on* `read_region` returning `-pte_nomap` at the boundary to
  stop the walk ([src/pt_backend.c:47](../../src/pt_backend.c#L47),
  [:128-137](../../src/pt_backend.c#L128)); a recorder-backed callback that serves bytes
  for *any* address removes that stop, so the whole-window path must also lift the
  **record-side** IP filter ([:108-109](../../src/pt_backend.c#L108), which today records
  only in-region IPs). Keep the two as **distinct** callbacks / record policies selected by
  mode — the region-scoped decode the bindings slice consumes must stay **byte-identical**
  after §2 (make that a regression assert, like §1's).
- **Self-recorder wiring (`src/hwtrace.c`).** In the arm path, create a self
  code-image timeline (`asmtest_codeimage_new(0)`,
  [include/asmtest_codeimage.h:81](../../include/asmtest_codeimage.h#L81)) and
  `asmtest_codeimage_track` ([:90-91](../../include/asmtest_codeimage.h#L90)) the
  JIT ranges; drive `asmtest_codeimage_refresh`
  ([:97](../../include/asmtest_codeimage.h#L97)) at region boundaries so a new
  version is snapshotted on change. (The recorder already feeds the *out-of-process*
  stepper's `_versioned` path; this points the same recorder at self.)
- **Capture-side address filter (`src/pt_backend.c:129-135`).** The documented pending
  capture-side fix (a prose comment at [:133-134](../../src/pt_backend.c#L133), **not** a
  literal `TODO` token) would program `PERF_EVENT_IOC_SET_FILTER` so the CPU emits packets
  only for the region. **Critical constraint (correcting the earlier "structural Q2/Q3
  fix" framing):** perf userspace address filters are resolved by scanning the task's VMAs
  for a **file-backed** match (inode + offset — the `start[/size]@<file>` grammar); a
  fileless filter is a *kernel*-address filter, and these PT events set `exclude_kernel =
  1` ([src/hwtrace.c:681](../../src/hwtrace.c#L681)). The regions this facility traces are
  **anonymous** — the native-leaf `exec_alloc` buffer is `MAP_ANONYMOUS`
  ([src/hwtrace.c:848](../../src/hwtrace.c#L848)) and JIT code pages are anonymous too — so
  the hardware filter **cannot target them**; it helps only for **file-backed DSOs**. So
  for the anonymous native-leaf and whole-window JIT cases the real Q2/Q3 mechanism is the
  **decode-time range filtering that already ships**
  ([src/pt_backend.c:108-116](../../src/pt_backend.c#L108)) — which cuts the *decoded*
  stream but **not** capture bandwidth, so the "orders-of-magnitude bandwidth win" applies
  only where the traced code is file-backed (or is first re-mapped from a `memfd`, which
  this plan does not currently propose). Even for file-backed regions, **Intel PT exposes
  only a small, CPU-dependent number of address-range filters**
  (`/sys/bus/event_source/devices/intel_pt/caps/num_address_ranges`, aka the perf
  `nr_addr_filters` limit), so wide or many-region windows exceed the hardware budget and
  fall back to decode-time filtering anyway. The whole capture-side-filter path needs PT
  hardware to validate, so it ships gated behind the same self-skip as the rest of the PT
  capture path.

**Validation posture (mirrors the hardware-trace plan).** The host-testable half
exercises the **recorder-backed image callback (`bytes_at`) adapter directly** —
feed known bytes at two `when` values and assert the adapter returns the
version-correct bytes and that `asmtest_disas` of them matches ground truth. It does
**not** drive libipt end-to-end: unlike AMD/CS, Intel PT has **no** decoder-independent
reconstruction sibling (`asmtest_pt_decode` consumes a raw PT packet stream, and there
is **no** synthetic-PT-packet fixture or PT encoder in the tree), so a genuine
host-side libipt decode would first need a synthetic PT packet stream / libipt's
encoder — a separately-budgeted sub-task (below). The **live PT capture** half
self-skips off bare-metal Intel PT. Per the project's "no untested hardware code" rule,
the live path is written but gated, and the gate is exercised on every host.

**§2 tests.** In [examples/test_hwtrace.c](../../examples/test_hwtrace.c):

- `test_pt_image_from_codeimage` (host-testable) — build an `asmtest_codeimage`
  over an in-process buffer with two versions of the bytes at one address, drive the
  new recorder-backed image adapter (the `bytes_at` callback libipt would call)
  **directly** at two `when` values, assert each returns the version live then (the
  temporal-bytes rule) and that `asmtest_disas` of the returned bytes matches ground
  truth. No PT hardware, no libipt packet stream. (End-to-end libipt decode needs a
  synthetic PT fixture or real Intel PT — see Validation posture.)
- Extend `test_codeimage` ([examples/test_codeimage.c](../../examples/test_codeimage.c),
  target `codeimage-test`, [mk/native-trace.mk:247](../../mk/native-trace.mk#L247))
  with a `bytes_at`-through-decoder round-trip.
- A live PT whole-window smoke that self-skips off Intel PT (asserts the skip
  reason on this AMD dev host), matching the existing `hwtrace-test` posture.

**§2 docs.** Extend
[docs/guides/tracing/hardware-tracing.md](../guides/tracing/hardware-tracing.md)
with the whole-window (image-callback) decode mode and its temporal-bytes rule;
cross-link [hardware-trace-plan Phase 2](hardware-trace-plan.md) as the shared
build; update [docs/analysis/trace-parity-matrix.md](../analysis/trace-parity-matrix.md)
with the new decode mode's parity status.

**§2 effort.** Recorder-backed `bytes_at` adapter + direct tests ~3–4 days
(host-testable; add ~1–2 days if an end-to-end host decode needs a synthetic PT packet
fixture / libipt-enc rather than the direct adapter test); the capture-side address
filter + live whole-window decode a further ~3–5 days **on PT hardware**, forward-look
until a bare-metal Intel PT host is available (same gate as the hardware-trace plan).

---

## §3 — Whole-window completeness: noise attribution (Q2) + snapshot drain (Q3) *(planned)*

**Goal.** Make the empty-scope *whole-window* mode usable, not just honest. §2 backs
the decoder with the recorder (so whole-window decodes at all); this sub-phase adds
the analysis's remaining Q2 and Q3 buildable refinements that §2 does not cover —
so the "you get the runtime too" noise is *labelled* rather than silently mixed, and
a long window does not simply overflow. It is deliberately split from §2 because its
most valuable pieces (symbolize-and-bucket, the AMD reconstruction test) are
**host-testable with no PT hardware**, unlike §2's live capture.

### §3.1 Q2 noise attribution — split and label the runtime slices

The analysis's Q2 lists three refinements; §2's capture-side address filter is (a).
This sub-phase builds (b) and (c):

- **(b) Emission-event slicing.** Use the recorder's eBPF emission detector — the
  `PROT_EXEC`-edge events from `asmtest_codeimage_watch_bpf` /
  `asmtest_codeimage_poll_bpf` / `asmtest_codeimage_next`
  ([include/asmtest_codeimage.h:151](../../include/asmtest_codeimage.h#L151),
  [:157](../../include/asmtest_codeimage.h#L157),
  [:161](../../include/asmtest_codeimage.h#L161)) — to timestamp *when* a method's
  bytes appeared, so the "JIT compiling `HotPath`" slice can be split from the
  "`HotPath` running" slice in the decoded stream (correlate each decoded IP's trace
  position against the recorder version timeline via
  `asmtest_codeimage_now`, [:102](../../include/asmtest_codeimage.h#L102)). The eBPF
  detector is **build- and privilege-gated** (needs libbpf + clang + bpftool at build,
  `CAP_BPF` + kernel BTF at runtime); where it is unavailable, emission-slicing falls
  back to the coarser **soft-dirty version timeline** (`asmtest_codeimage_now`
  correlation), and `test_emission_slice`'s eBPF assertions self-skip when
  `bpf_available() == 0`.
- **(c) Symbolize-and-bucket.** Bucket every decoded IP against `/proc/self/maps` and
  the perf-map so noise is labelled ("31k insns in RyuJIT, 2k in GC, 7k in `HotPath`")
  rather than silently mixed. **The two cited helpers return *extents*, not names** —
  `asmtest_proc_region_by_addr`
  ([include/asmtest_ptrace.h:291](../../include/asmtest_ptrace.h#L291)) discards the
  maps pathname (its `sscanf` reads only `start-end perms`), and
  `asmtest_proc_perfmap_symbol` ([:303](../../include/asmtest_ptrace.h#L303)) is a
  *forward* name→region lookup — so the **label** half needs a **new address→name
  reverse resolver** built here: a `/proc/self/maps` reader that keeps the pathname
  field, and a perf-map range search that returns the containing JIT symbol. **The
  bucket-by-IP mechanics stay host-testable** (the bucketer takes an IP list, not a
  live PT capture); the label half rides the new resolver — together the one
  whole-window piece with real CI coverage.

### §3.2 Q3 snapshot drain — lift the bandwidth ceiling

`end()` today decodes only the **linear** ring `[0, aux_head)`
([src/hwtrace.c:799](../../src/hwtrace.c#L799)); the circular-ring walk is a named
follow-up ([:797-798](../../src/hwtrace.c#L797)). Two buildable drains:

- **PT `aux_tail` circular walk.** For `snapshot` mode (the PROT_READ circular AUX
  ring, [src/hwtrace.c:705-706](../../src/hwtrace.c#L705)), walk from `aux_tail`
  around the ring in `end()` so a long window keeps its **tail** (flag `truncated`),
  instead of decoding only the linear head. Needs PT hardware to validate live.
- **AMD `data_tail` mid-capture drain.** The data ring is "never drained mid-capture
  (`data_tail` only advances at [end])" ([src/hwtrace.c:578](../../src/hwtrace.c#L578),
  consume at [:641](../../src/hwtrace.c#L641)); advancing `data_tail` from a consumer
  thread *while the region runs* converts the ceiling from ring capacity to sustained
  consumption. **Honest caveat (from the analysis):** the PMI-per-branch cost still
  grows with the region, so a long window trends toward stepper-like slowdown —
  stitching/draining extends the *window*, not the bandwidth economics. Reconstruction
  is host-testable with synthetic samples (the `test_amd_stitch` pattern,
  [examples/test_hwtrace.c:323](../../examples/test_hwtrace.c#L323)); live drain needs
  Zen 3+.

**§3 tests.**

- `test_symbolize_bucket` (host-testable, **CI-runnable**) — feed a synthetic IP list
  spanning two `/proc/self/maps` regions + a perf-map entry, assert the bucket counts
  and the labels resolved via the new address→name reverse resolver (§3.1(c)). No
  hardware.
- `test_emission_slice` — over the `test_codeimage` fixture
  ([examples/test_codeimage.c](../../examples/test_codeimage.c)), assert an IP inside
  a range whose bytes appeared *after* trace position T is attributed to the
  "compiling" slice, not the "running" slice.
- `test_amd_drain_reconstruction` (host-testable) — extend `test_amd_stitch`
  ([:323](../../examples/test_hwtrace.c#L323)) with a synthetic multi-sample stream
  larger than one ring, assert the drained sequence is gapless. Live PT `aux_tail`
  drain self-skips off Intel PT.

**§3 docs.** Document the whole-window noise labels and the snapshot/drain ceiling in
[docs/guides/tracing/hardware-tracing.md](../guides/tracing/hardware-tracing.md); note
in [docs/reference/troubleshooting.md](../reference/troubleshooting.md) that a noisy
empty-scope trace is expected and how to read the bucket labels.

**§3 effort.** Symbolize/bucket (c) ~2–3 days (host-testable); emission-event slicing
(b) ~2–3 days; the PT `aux_tail` + AMD `data_tail` drains ~3–4 days (AMD
reconstruction host-testable; PT live forward-look on Intel PT).

---

## Build & CI wiring (all sub-phases)

- New symbols compile into the existing native-trace objects — `hwtrace.o`,
  `pt_backend.o` ([mk/native-trace.mk:189](../../mk/native-trace.mk#L189)),
  `ss_backend.o` ([:196-200](../../mk/native-trace.mk#L196)),
  `amd_backend.o` ([:194-195](../../mk/native-trace.mk#L194)), `codeimage.o` — and
  flow into `HWTRACE_OBJS` ([:226-231](../../mk/native-trace.mk#L226)), the
  `build/pic/` tree ([:615-638](../../mk/native-trace.mk#L615)), and
  `shared-hwtrace` ([:653-669](../../mk/native-trace.mk#L653)). No new object files,
  no new pkg-config knob.
- If `asmtest_hwtrace_try_begin`/`_render`/`_arm_tid` are installed public symbols,
  add them to `install-shared-hwtrace`
  ([mk/native-trace.mk:677-688](../../mk/native-trace.mk#L677)) — which already copies
  `include/asmtest_hwtrace.h`, so header-declared symbols need no extra header-install
  step (review item **K6**).
- CI: the new `test_*` cases run inside the existing `hwtrace` job
  ([.github/workflows/ci.yml:247](../../.github/workflows/ci.yml)) via
  `make hwtrace-test` and the `codeimage` job (`:281`) via `make docker-hwtrace-codeimage`,
  which runs `codeimage-test` inside the eBPF-capable container — no new job. The
  per-binding lanes that exercise the new symbols are the bindings slice's concern
  (`hwtrace-bindings`, `:268`).

---

## Risks and open points

- **Per-thread migration is invasive and must be a no-op for existing callers.**
  The single-region API is shipped and CI-gated across ten bindings; §1's
  regression tests (`test_singlestep_live` re-run, byte-identical) are the guard.
- **Async-signal-safety of the single-step range stack — and of the TLS access
  itself.** The handler ([src/ss_backend.c:88](../../src/ss_backend.c#L88)) runs in
  signal context; the range stack must be a fixed TLS array with no allocation, no
  locks. Beyond the array *contents*, the TLS *access model* matters: in the `dlopen`'d
  shared library the general-dynamic first-touch of a `__thread` var can route through
  `__tls_get_addr` and lazily allocate, so the handler-touched state must be
  `tls_model("initial-exec")` — **mandatory for the range stack and per-capture state**,
  since `__tls_get_addr` is not async-signal-safe even for an already-allocated block;
  keeping `g_armed` a non-TLS atomic only spares the *unarmed* early-return path, it does
  **not** substitute for initial-exec on the range stack the *armed* handler reads. The
  process-wide SIGTRAP disposition needs an explicit arm-refcount so the restore fires
  only when the last armed thread leaves (§1). Separately, the process-global **region
  registry** needs a mutex once §1 allows concurrent scopes (§1 Changes) — a distinct
  lock from the TLS work, and off the signal path.
- **§2's live half needs PT hardware** (this dev host is AMD — no PT, ever). It
  ships self-skipping and hardware-gated; only the reconstruction half is
  CI-validated, exactly as [hardware-trace-plan](hardware-trace-plan.md) accepts
  for its own PT capture.
- **ABI stability.** `begin`/`end` stay `void` and behaviourally identical for the
  ten shipped shims; all new capability is additive (`try_begin`, `render`, `arm_tid`,
  and the §1 handle-keyed trio `begin_scope`/`render_scope`/`render_versioned` plus the
  §D4 `stitch` merge helper). No existing symbol changes signature.

## Sources

Design rationale, the four-qualification analysis (thread-scope, runtime-noise,
bandwidth, nesting), and the temporal-bytes correctness rule:
[the scoped `using` analysis](../analysis/scoped-inprocess-tracing.md#can-the-four-qualifications-be-fixed-in-code).
Shared decode/recorder background:
[hardware-trace-plan.md](hardware-trace-plan.md),
[jit-runtime-tracing.md](../analysis/jit-runtime-tracing.md).
