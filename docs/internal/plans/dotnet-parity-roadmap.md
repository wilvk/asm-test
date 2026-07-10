# asm-test — dotnet-parity roadmap (the nine bindings vs the .NET reference): implementation plan

> **Context (2026-07-08).** The `.NET` binding is the reference/lead shim: it carries the
> managed scoped-tracing tier (`AsmTrace`, `AsmStitchedTrace`) and wraps a family of
> native-trace tier symbols the other nine bindings do not. This roadmap enumerates the
> concrete per-binding work to close that gap — the "temporal" gaps the parity gate
> deliberately `ALL`-exempts, **not** the structural FFI limits it exempts per-binding.
> It complements the [trace-parity-matrix analysis](../analysis/trace-parity-matrix.md)
> (which backend runs where) and consumes the same source of record: the tier headers +
> [scripts/check-bindings-parity.sh](../../../scripts/check-bindings-parity.sh).

> **Status: partially landed (roadmap).** Phase 1 and part of Phase 2 have landed (see the
> status legend below and CHANGELOG.md); the rest is forward-look. The formal parity gate is
> **green** — `bindings-parity: OK — 114 tier symbols x 10 bindings in sync` — but green means only
> that every binding either wraps each tier symbol *or carries a documented exemption* in
> [scripts/bindings-parity-allow.txt](../../../scripts/bindings-parity-allow.txt). The
> `ALL`-exemptions the gate consumes are exactly the .NET-lead surface below, so a green
> gate is **not** feature-parity with .NET. This roadmap is the plan to retire those
> exemptions where a binding should grow the capability, and to state plainly where it
> should not.
>
> Status legend: **Phase 1 LANDED** (2026-07-08, `afc6ee4`) — the mechanical Cluster 1
> rollout. **Phase 2 STARTED** — the §Z1 in-process whole-window trio, out-of-process stealth
> + crash-proof whole-window capture, version-aware render, and the §D0.4 async-hop merge have
> LANDED in Node/Java (`c2327bc` ff.; see CHANGELOG.md); the .NET deep live-JIT per-method
> resolution (E3) remains forward-look. **Phase 3 planned**
> (grow-a-use, per binding). The clean-path validation is PT-hardware-gated (this host is AMD).

## Why this is not "wrap 13 symbols × 9 bindings"

The symbols .NET leads on split into two kinds, and the split sets a hard ceiling on what
"parity" can even mean per binding:

- **Generic C primitives** — they operate on a native `[base, len)` leaf or an
  `asmtest_trace_t` handle. Any binding can wrap them; it is FFI plumbing.
- **Managed-JIT capabilities** — tracing the runtime's *own live JIT output* and
  stitching a logical operation across `await`/continuation thread hops. These exist only
  inside a managed JIT runtime. The reference constructs that embody them —
  `AsmTrace.Method(Delegate)` (JIT'd managed-method resolution via `JitMethodMap` /
  `MethodLoadVerbose`, [HwTrace.cs:1500](../../../bindings/dotnet/hwtrace/HwTrace.cs#L1500)
  / [:2113](../../../bindings/dotnet/hwtrace/HwTrace.cs#L2113)), the `DiagnosticsIpc`
  pre-arm rundown ([:2361](../../../bindings/dotnet/hwtrace/HwTrace.cs#L2361)), and the
  `AsyncLocal<ScopeId>` hop hook behind `AsmStitchedTrace`
  ([:2917](../../../bindings/dotnet/hwtrace/HwTrace.cs#L2917)) — have **no analog** in
  C++, Rust, Zig, Go, or Lua. Those runtimes AOT-compile or interpret; there is no live
  JIT method stream to follow.

**Consequence:** managed-capability parity is only *definable* for **Node** and **Java**
(the other two managed-JIT runtimes — the [managed
slice](scoped-tracing-managed-plan.md)'s §D1/§D2). For the six non-managed bindings,
"dotnet parity" means the **generic-primitive surface** over native leaves, and several of
those primitives are low-value there. The roadmap is therefore phased by value density,
not by symbol count.

## The feature clusters

Symbol contracts are in [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h);
line anchors below are current as of 2026-07-08.

| Cluster | Symbols (header line) | Capability | Portable to |
|---|---|---|---|
| **1 · Registry-free scoped call** | `call_scoped_ex` ([:273](../../../include/asmtest_hwtrace.h#L273)), `render_scope` ([:282](../../../include/asmtest_hwtrace.h#L282)); also `begin_scope` ([:234](../../../include/asmtest_hwtrace.h#L234)), `call_scoped` ([:249](../../../include/asmtest_hwtrace.h#L249)), `call_scoped_fp` ([:259](../../../include/asmtest_hwtrace.h#L259)) | Arm → call native leaf → disarm entirely in native code; `_ex` is registry-free so no `MAX_REGIONS` exhaustion in a tight loop | **All** — the ergonomic win; already in python/node/java/ruby |
| **2 · Whole-window empty scope (§Z1)** | `begin_window` ([:318](../../../include/asmtest_hwtrace.h#L318)), `end_window` ([:326](../../../include/asmtest_hwtrace.h#L326)), `render_window` ([:334](../../../include/asmtest_hwtrace.h#L334)) | `using (new AsmTrace())` — capture whatever runs, no region; records ABSOLUTE addresses | All (native leaf, single-step); managed value needs PT/ptrace |
| **3 · Version render + noise attribution** | `render_versioned` ([:289](../../../include/asmtest_hwtrace.h#L289)), `symbolize_bucket` ([:465](../../../include/asmtest_hwtrace.h#L465)) | Decode moved/tiered bytes against a code-image; bucket whole-window IPs by JIT symbol / mapped region | All wrap-able; value is managed-runtime noise attribution |
| **4 · Async-hop stitching (§D4/§D0.4)** | `stitch_handles` ([:384](../../../include/asmtest_hwtrace.h#L384)) | Merge per-thread slices of one logical operation in `seq` order | Generic merge is portable; **the producer (per-runtime hop hook) is managed-only** |
| **5 · Out-of-process stepper (§D3)** | `stealth_trace` ([:413](../../../include/asmtest_hwtrace.h#L413)) | Exact single-step of a native leaf via a reverse-attached helper — no TF armed on the caller's own thread; runs on no-PT hosts (Zen 2, Docker-on-Mac) | All (`run_region` is a **non-capturing** callback) |
| **0 · Diagnostic** | `arm_tid` ([:189](../../../include/asmtest_hwtrace.h#L189)) | OS tid that armed the active capture — a managed-thread-level single-region assert | All (trivial) |

**Two symbols are deliberately *not* binding targets.** `asmtest_hwtrace_stitch`
([:371](../../../include/asmtest_hwtrace.h#L371)) passes `asmtest_hwtrace_slice_t` **by
value with embedded heap pointers a binding cannot marshal** — the header ships
`stitch_handles` (opaque trace handles + blittable scalar arrays) as the binding-facing
form. `asmtest_hwtrace_begin_scope` / `call_scoped` (registry form) are **superseded by
`_ex`** for a native leaf (same capture, no fixed-table slot consumed); .NET wraps them
for `AsmTrace.Method`'s *named-region managed* path, where a native binding gains nothing
over `_ex`.

## FFI constraints that set per-binding difficulty

- **Struct-by-value.** `render_scope`, `end_window`, and `render_window` take the 8-byte
  `asmtest_hwtrace_scope_t` handle ([:217](../../../include/asmtest_hwtrace.h#L217)) **by
  value**. Native in the compiled / `cdef` bindings (cpp, rust, zig, lua/LuaJIT, go);
  Fiddle (ruby) has no struct-by-value and packs it as a `LONG_LONG`
  (`idx | gen<<32`, ABI-identical on SysV x86-64) — the pattern already shipped in
  commit `19d5646`. koffi (node) and FFM (java) pass it by value directly / as a packed
  `JAVA_LONG`.
- **Capturing upcalls (structural — NOT a roadmap item).** `descent_set_resolver` /
  `descent_set_denylist` need a GC-safe *capturing* upcall, which cpp (dlopen), rust
  (`extern "C" fn`), zig (`std.DynLib`), and ruby (Fiddle) cannot host — hence their
  standing exemptions. These stay allow-set-only; they are not part of dotnet-parity.
- **`stealth_trace`'s callback is non-capturing** (`void (*run_region)(void *)` + a
  `void *arg`), so it is *not* blocked by the capturing-upcall limit — every binding that
  hosts an ordinary C callback can wrap it.

## Per-binding state → to-do

| Binding | FFI | Has today | Next (ordered) | Effort | Notes |
|---|---|---|---|---|---|
| **python** | ctypes | Cluster 1 (via `_ex`), `arm_tid`, `dr_under_dynamorio` | grow-a-use: window trio, `stealth_trace`, `symbolize_bucket` | ~2–3d (mostly low-value) | Closest. **Leads .NET** on `dr_under_dynamorio`. No live JIT → not a managed target |
| **ruby** | Fiddle | Cluster 1 (`_ex`+`render_scope`+`call_scoped`) | optional generics; `arm_tid` | ~2–3d | Handle packed `LONG_LONG`; capturing upcall blocked (descent stays exempt) |
| **node** | koffi | Cluster 1, §D1 `using`-scope, **§Z1 window trio ✅ `c2327bc`**, **`stealth_trace` ✅ (2026-07-09)** | **§D1 managed**: `AsyncLocalStorage` hop hook, `render_versioned`, V8-jitdump resolution | ~4–6d | **Managed target.** `worker_threads` hops escape (disclosed gap) |
| **java** | FFM/Panama | Cluster 1, §D2 `AsmTrace` t-w-r, **§Z1 window trio ✅ `c2327bc`**, **`stealth_trace` ✅ (2026-07-09)** | **§D2 managed**: JVMTI hop hook, `libperf-jvmti` jitdump resolution | ~4–6d | **Managed target.** `libperf-jvmti.so` is an external build dep |
| **cpp** | dlopen | Cluster 1 ✅, `arm_tid` | ~~Cluster 1~~ ✅ `afc6ee4`; then optional generics | done | Struct-by-value trivial (real C decls); capturing upcall blocked |
| **rust** | libloading | Cluster 1 ✅ | ~~Cluster 1~~ ✅ `afc6ee4`; then optional generics | done | `#[repr(C)]` by value trivial; capturing upcall blocked |
| **zig** | std.DynLib | Cluster 1 ✅ | ~~Cluster 1~~ ✅ `afc6ee4`; then optional generics | done | `extern struct` by value trivial; capturing upcall blocked |
| **lua** | LuaJIT FFI | Cluster 1 ✅, descent upcalls | ~~Cluster 1~~ ✅ `afc6ee4`; then optional generics | done | FFI callbacks OK; `cdef` struct by value trivial |
| **go** | cgo | Cluster 1 ✅, descent upcalls | ~~Cluster 1~~ ✅ `afc6ee4`; then optional generics | done | Async-hop N/A; render pinned with `LockOSThread` (handle-keyed, capturing-thread TLS) |

## Phased sequencing

### Phase 1 — finish Cluster 1 in the five that lack it (mechanical, highest ROI) — ✅ LANDED `afc6ee4` (2026-07-08)

> **Landed 2026-07-08 (`afc6ee4`).** All five bindings (cpp, rust, zig, lua, go) now wrap
> `call_scoped_ex` + `render_scope`, each with the canonical `add2` test + 40-call loop,
> green in every `docker-hwtrace-<lang>` lane. The two `ALL` exemptions were removed from
> [bindings-parity-allow.txt](../../../scripts/bindings-parity-allow.txt); the gate is green
> (103 × 10). A go `runtime.LockOSThread` pin (handle-keyed render must run on the capturing
> thread) and a cpp RAII trace guard (free on a throwing render) were surfaced by an
> adversarial review pass. **`call_scoped_ex` + `render_scope` are now wrapped in all ten
> bindings.** Next: Phase 2 (Node/Java managed tier).

**Bindings:** cpp, rust, zig, lua, go. **~3–5d total** (~0.5–1d each — the scope construct
already ships in every binding via the [bindings
slice](../archive/plans/scoped-tracing-bindings-plan.md); this adds only the two symbols).

Mirror commit `19d5646` (which added the same to ruby/node/java, itself mirroring the
python `8941860`): wrap `asmtest_hwtrace_call_scoped_ex` + `asmtest_hwtrace_render_scope`,
exposing a `callScoped(code, args…) → {result, path, truncated}` in each binding's idiom.
Struct-by-value is native in all five, so **no Fiddle-style packing** is needed. Add the
canonical test per binding: the `add2` leaf (result 42, body renders to `ret` with 5 insn
lines) plus a 40-call registry-free loop proving no `MAX_REGIONS` exhaustion, validated in
each `docker-hwtrace-<lang>` lane.

**Gate effect:** retires the five-binding `ALL`-exemption consumption on
`call_scoped_ex` / `render_scope` in
[bindings-parity-allow.txt](../../../scripts/bindings-parity-allow.txt) — after this the
matrix shows those two symbols wrapped in all ten bindings, and the two `ALL` lines are
removed (stale exemptions fail the gate, so they *must* be removed).

### Phase 2 — the real dotnet-capability parity: Node §D1 + Java §D2

> **Increment 1 LANDED (2026-07-08, `c2327bc`).** The §Z1 in-process whole-window trio
> (`begin_window`/`end_window`/`render_window`) now ships in Node (`HwTrace.window(fn)`) and
> Java (`HwTrace.window(Runnable)`) — the empty-ctor `using (new AsmTrace())` substrate,
> validated over a native leaf in each Docker lane. It is honest-but-noisy: single-stepping
> the runtime records the FFI dispatch + runtime, so the traced routine's addresses are a
> *subset* (a V8 call ~100k insns, captured cleanly and subset-verified; a HotSpot+FFM call
> exceeds the internal `SS_WINDOW_CAP` 1<<20 and honestly truncates). **This form is SAFE
> only for a tight native-leaf body** — arming single-step on the managed thread for an
> arbitrary managed block is the SIGTRAP footgun; the doc comments route that case to the
> out-of-process path in [managed-wholewindow-oop-plan.md](managed-wholewindow-oop-plan.md)
> (the §D3 whole-window channel, `asmtest_ptrace_trace_window_call`). Remaining Phase-2 work
> (async-hop, JIT-address resolution) is below.
>
> **Increment 2 LANDED (2026-07-09).** The §D3 out-of-process stealth stepper
> (`asmtest_hwtrace_stealth_trace`) now ships in Node (`HwTrace.stealthTrace(code, a, b)`) and
> Java (`HwTrace.stealthTrace(NativeCode, long...)`) — the **crash-proof** counterpart to
> `callScoped`/`window`, mirroring dotnet's `AsmTrace.Method(..., outOfProcess: true)`. A helper
> child reverse-attaches and single-steps the leaf out of band, so **no `EFLAGS.TF` is armed on
> the runtime's own thread** (the in-process single-step footgun the §Z1 `window()` form warns
> about). The `result` is EXACT (read from the caller's RAX at the `ret`); the instruction STREAM
> is **best-effort over a live runtime** — single-stepping the runtime thread can be interrupted
> by its async signals, so the wrapper honestly reports `truncated` with a partial `offsets` (the
> same posture dotnet takes: its `outOfProcess` test asserts result + armed, not stream
> completeness). Live-validated on the host (yama `ptrace_scope=1` + `PR_SET_PTRACER`) and in the
> `docker-hwtrace-node` / `-java` lanes; self-skips cleanly where the reverse-attach is refused.
> The `ALL asmtest_hwtrace_stealth_trace` allow-list line **stays** (seven bindings still don't
> wrap it, so it is not stale).
>
> **C-backend bug fixed en route (`getpid` → `SYS_gettid`).** Wiring Java's wrapper surfaced a
> real latent defect: `asmtest_hwtrace_stealth_trace` seized `getpid()` (the process leader), but
> HotSpot runs Java `main()` on a JVM thread whose tid ≠ pid — so the helper stepped the wrong
> (idle) thread and the `run_to` breakpoint fired on the **untraced** calling thread → fatal
> SIGTRAP (exit 133). Node and CoreCLR were unaffected only because their calling thread *is* the
> leader (tid == pid). One-line fix in [src/hwtrace.c](../../../src/hwtrace.c): seize
> `(pid_t)syscall(SYS_gettid)` (the calling thread), matching what
> `asmtest_hwtrace_stealth_trace_windowed` already did. After the fix Java stealth returns a
> **complete, exact** trace (`[0,3,6,c,11]`, not truncated) on both the forked-child and the
> bundled exec'd-helper paths. (The stream is still best-effort in general — Node's exec'd helper
> can truncate at insn 1 when V8's async signals hit the non-SIGTRAP break; the result stays
> exact.)
>
> **Increment 3 LANDED (2026-07-09).** The §D3 WHOLE-WINDOW OOP capture now ships in Node and
> Java — the out-of-process analog of the in-process `window()` (the SIGTRAP footgun). Wraps
> `asmtest_ptrace_trace_window_call` (`Ptrace.windowCall` / `HwTrace.ptraceTraceWindowCall` —
> fork-internal, asserts unconditionally) and `asmtest_hwtrace_stealth_trace_windowed`
> (`HwTrace.stealthWindow` — reverse-attach, mirroring dotnet's `AsmTrace.Window`; self-skips on
> a refused attach), plus the five `asmtest_addr_channel_*` shims behind a new `AddrChannel` class
> (pre-publish the leaves the window frame calls into; the capture records the frame + published
> regions as ABSOLUTE addresses, classify by range). Validated in `docker-hwtrace-node` / `-java`
> against the C oracle's 35-byte driver frame calling two 7-byte leaves (result `m2(7,3)==4`;
> driver + both leaves in call order; complete). The two `ALL` windowed allow-list lines stay
> (seven bindings still don't wrap them); the addr_channel shims are in a non-tier header
> (ungated).
>
> **Increment 4 LANDED (2026-07-09).** The remaining CI-runnable Phase-2 clusters now ship in Node
> and Java (six .NET-lead symbols): version-aware render (`CodeImage.renderVersioned` +
> `NativeTrace.appendInsn`); whole-window noise attribution (`HwTrace.regionName` /
> `symbolizeBuckets` / `attributeWindow` — the named-region path splits identical-byte leaves that
> symbol/disasm attribution cannot); and the §D0.4 **async-hop merge** (`HwTrace.stitchHandles` —
> order N captured hop traces by `seq`, host-independent pure merge, validated with a fake-hook
> harness scripting the hops out of seq order). Struct marshalling pinned to the exact SysV layouts
> (`bucket_t` 136, `slice_bound_t` 32, `named_region_t` 80), cross-checked vs the dotnet
> `[StructLayout]`s. Validated in `docker-hwtrace-node` / `-java` against the C oracles
> (`test_render_versioned` / `test_symbolize_bucket` / `test_wholewindow_buckets` /
> `test_stitch_slices`). All six `ALL` allow-list lines stay (7-8 bindings still don't wrap them).
> **What remains is genuinely forward-look**: the LIVE async-hop *producer* (Node
> `AsyncLocalStorage` / Java JVMTI hop hooks) and runtime JIT-address resolution (V8 jitdump /
> `libperf-jvmti`) — runtime-specific, no CI coverage today; the merge/attribution *substrate* they
> feed is now wrapped and host-tested. The remaining ALL-exempt symbols are hardware-gated (AMD
> LBR/MSR survey) or superseded (registry `call_scoped`, `begin_scope`).

**Bindings:** node, java only. **~8–12d** (§D1 ~4–6d, §D2 ~4–6d, per the [managed
slice](scoped-tracing-managed-plan.md#effort--risks)). This is the only place
"parity with .NET" is genuinely *definable* — the two runtimes with a live JIT.

Per binding, the managed capability is: the per-runtime async-hop hook (Node
`AsyncLocalStorage` / `async_hooks`; Java JVMTI or executor bytecode agent — `ScopedValue`
only *propagates*, it is not a value-changed hop signal), JIT-method address resolution
(Node V8 jitdump; Java `libperf-jvmti.so` → `jit-<pid>.dump` read in-process via the
shipped `asmtest_jitdump_find`), the whole-window trio, `stitch_handles` fed by the hook,
and `stealth_trace` routing so a no-PT host silently steps out of band instead of arming
single-step against the runtime (**forbidden** — it fights the runtime's SIGTRAP/JIT).

**Validation reality:** the clean in-process PT path is **forward-look** (needs bare-metal
Intel PT; the dev host is AMD). The **ptrace-stealth path is the CI-runnable exactness
check** on any ptrace-capable Linux, and the §D4 merge is covered host-side by
`test_stitch_slices` / `test_stitch_handles`. The live per-runtime hop hook itself ships
with **no CI coverage** (disclosed gap) — add a fake-hook harness driving the merge from a
scripted hop sequence to cover the hook→merge seam.

### Phase 3 — optional generic primitives across native bindings (grow-a-use)

**Symbols:** `begin_window` trio, `stealth_trace`, `symbolize_bucket`, `render_versioned`,
`stitch_handles`. **Decide per binding; do not bulk-build.**

For a native leaf these are mostly low-value — the existing `region()` / `scope` already
covers native tracing, and `_ex` (Phase 1) already gives the loop-safe property.
Legitimate grow-a-use triggers: a binding wants a no-PT-host exact stepper
(`stealth_trace`), an empty-scope form over multiple native leaves with per-leaf
attribution (`begin_window` + `symbolize_bucket`), or a multi-region native merge
(`stitch_handles`). Wrap on demand, drop the matching allow-list line, add the test.

### Side items — decide, likely skip

- **`arm_tid`** to the seven lacking it (rust, zig, node, java, ruby, lua, go): only if a
  binding wants the tighter managed-thread assert. Otherwise keep the exemption — they
  rely on the C `asmtest_hwtrace_end()` cross-thread `truncated` backstop, as the
  allow-list documents.
- **`begin_scope` / `call_scoped` (registry form) / `call_scoped_fp`**: superseded by
  `_ex` for native bindings. Only worth it for a named-region managed-method form;
  `call_scoped_fp` only if a binding traces `double→double` leaves.
- **Reverse gap:** python wraps `asmtest_dr_under_dynamorio` and .NET does not — the one
  symbol where a binding leads the reference. For strict two-way parity, add a one-line
  P/Invoke to the .NET binding; or leave it python-only (it is a managed-host-gate
  diagnostic with no .NET caller today).

## The ceiling (state it in docs, do not treat as a gap)

After every phase, the six non-managed bindings (C++, Rust, Zig, Go, Lua, Python, Ruby)
reach **primitive-surface** parity but never **managed-capability** parity — there is no
live JIT to trace, no `AsyncLocal`-equivalent hop to follow. That is the nature of the
runtimes, not an open item. Full dotnet-equivalence is a **Node + Java** story only, and
even there the clean whole-window PT decode is forward-look on hardware this project does
not yet have in CI.

## Tests

- **Phase 1:** per-binding `add2`-leaf scope test + 40-call no-exhaustion loop in each
  `docker-hwtrace-<lang>` lane (the `19d5646` shape). Assert `render_scope` produces the
  same body offsets as the callback `region()` form; assert clean self-skip off Linux /
  non-single-step backends.
- **Phase 2:** extend the `hwtrace-node-test` / `hwtrace-java-test` lanes with a
  `using`/try-with-resources scope over a native leaf (runs today) and an opt-in async-hop
  case asserting slices stitch by `ScopeId` (self-skips off a PT/ptrace host). The §D3
  ptrace-stealth exactness check and the §D4 host-side merge tests are the automated
  protection; the live hook is the disclosed forward-look gap.
- **Phase 3:** per-symbol test as each is grown; no new corpus entries — the scoped forms
  replay the existing [conformance corpus](../../../bindings/conformance/).

## Build & CI

Source-only additions to the existing per-binding trees, consuming the shared-core symbols
via `libasmtest_hwtrace`; no new shared lib or Make object. Phase 1 lands in the existing
`docker-hwtrace-<lang>` fan-out lanes unchanged. Phase 2's ptrace-stealth and merge tests
gate on ordinary CI (a `--cap-add=SYS_PTRACE` Docker lane, no PT hardware); the clean PT
lanes self-skip. After each phase, run `scripts/check-bindings-parity.sh` and remove the
now-stale allow-list lines (stale exemptions fail the gate).

## Effort & risks

- **Phase 1 ~3–5d, Phase 2 ~8–12d, Phase 3 on demand.** Phase 1 is low-risk mechanical
  propagation. Phase 2 is the real engineering — the async-hop redesign is a *model
  change* (thread window → stitched logical operation), gated behind explicit opt-in, and
  must never emit a partial trace as complete (the §0.2 arming-thread assert is the
  backstop).
- **Managed single-step is forbidden** — Phase 2 must route managed-code capture to
  PT/LBR or the §D3 ptrace helper, never `src/ss_backend.c` against the runtime.
- **Address resolution is runtime-version-fragile** off .NET 8 / current V8 / current
  JVMs; the self code-image recorder is the version-independent fallback.
- **`libperf-jvmti.so` (Java) is an external build dependency** often absent from distro
  packages, writing to a randomized `$HOME/.debug/jit/*/` path §D2 must discover.

## Sources

- Matrix + gate: [scripts/check-bindings-parity.sh](../../../scripts/check-bindings-parity.sh),
  [scripts/bindings-parity-allow.txt](../../../scripts/bindings-parity-allow.txt)
  (`--report` prints the symbol × binding matrix).
- Header contracts:
  [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h) §1/§Z1/§D3/§D4/§3.1(c).
- .NET reference: [bindings/dotnet/hwtrace/HwTrace.cs](../../../bindings/dotnet/hwtrace/HwTrace.cs)
  (`AsmTrace`, `AsmStitchedTrace`, `JitMethodMap`, `DiagnosticsIpc`).
- Managed tier: [scoped-tracing-managed-plan.md](scoped-tracing-managed-plan.md)
  (§D0–§D4). Scope construct (already shipped everywhere):
  [scoped-tracing-bindings-plan.md](../archive/plans/scoped-tracing-bindings-plan.md).
- Backend/host reach: [trace-parity-matrix.md](../analysis/trace-parity-matrix.md).
- Landed follower commits: `8941860` (python `call_scoped`), `19d5646`
  (ruby/node/java `call_scoped`).
