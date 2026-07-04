# asm-test — Scoped-tracing per-language shims (native / interpreter / Go tiers): implementation plan

The **thin-shim** half of the [scoped in-process tracing
plan](scoped-inprocess-tracing-plan.md) for the tiers with **no managed-JIT
hazard** — the eight bindings where the developer-visible surface can become
*import + scope* over a *known native leaf* with existing machinery. It delivers
new-item **1 and 2** from the umbrella's
[what-is-new list](scoped-inprocess-tracing-plan.md#what-already-ships-vs-what-is-new):
the guaranteed-cleanup scope construct and the arm hook, per binding.

**Scope of this slice:** the scope *construct* for **C++, Rust, Zig, Python, Ruby,
Lua, Go**, plus **.NET as the reference shim shape**. The **managed-code**
capability (tracing the runtime's own live JIT output, async-hop stitching) for
**Node, JVM, .NET** is the separate [managed slice](scoped-tracing-managed-plan.md);
this slice targets the use every binding already supports — a scope around an
asm-test-generated / P/Invoked **native region**, captured by single-step (any
x86-64 Linux) or available hardware trace.

> Status legend: **planned** unless noted. .NET's `Region(Action)` and the other
> nine `region(name, fn)` helpers already ship (see anchors below); this slice adds
> the *scope object*, *auto-name*, *arm hook*, *thread-id assert*, and *emit-on-close*
> on top of them.

**Hard dependency:** the [shared-core slice §0](scoped-tracing-core-plan.md#0--the-two-cheap-c-layer-fixes--shared-render-on-close-planned-lands-first)
— `asmtest_hwtrace_try_begin` (nesting signal), the arming-thread assert, and the
`asmtest_hwtrace_render` render-on-close path — must land first. This slice
consumes those three symbols in every shim.

---

## The shim shape (one pattern, ten realisations)

The analysis identifies four ergonomic pieces; three of them map to a per-language
idiom, and the fourth (`AsyncLocal` async-hop) is **only** needed in the managed
tier and is therefore out of scope here:

| Piece | Generic capability | This slice's realisation |
|---|---|---|
| guaranteed-cleanup scope | balanced `begin`/`end` even on exception | RAII / `with` / `defer` / block-`ensure` per language |
| arm before first use | no setup call | real module-init where one exists, else lazy-once; **recommended lazy first-scope everywhere** |
| auto-name from call site | no name to invent | compile-time source-location or a one-frame stack walk |
| thread-id assert at close | never emit partial-as-complete | call the shared-core §0.2 assert (or the language's own tid check) |

**Two correctness constraints every shim must honour** (from the analysis's
cross-language pass):

1. **Register-then-begin under the same generated name.** `begin` keys on the
   region name via `find_region` ([src/hwtrace.c:413](../../src/hwtrace.c#L413),
   [:659](../../src/hwtrace.c#L659)) and silently no-ops on a miss, so an auto-named
   scope must `asmtest_hwtrace_register_region` under its generated name *before*
   `begin`. Because the scope object registers on **every** construction, this is only
   safe once **Core §0.4** makes `register_region` idempotent-by-name (repeated names
   reuse one slot, so a looped/sprinkled scope doesn't exhaust the 32-entry table).
   Shims must treat a nonzero `register_region`/`try_begin` return as a **clean
   self-skip**, never a hard failure.
2. **Lazy first-scope arm.** Mere import must not claim the (per-thread, after core
   §1) capture slot or install the single-step SIGTRAP sigaction
   ([src/ss_backend.c:129](../../src/ss_backend.c#L129)). "Arm on import" is
   delivered as an *effect* (arm on first scope entry), which is also strictly
   better for the three languages with no module-init hook.

---

## Reference shim — .NET `AsmTrace` (the canonical shape)

.NET is the analysis's worked example and the shape every other shim mirrors.
Today the binding has **only** the callback form —
`HwTrace.Region(string name, Action body)` at
[bindings/dotnet/hwtrace/HwTrace.cs:602](../../bindings/dotnet/hwtrace/HwTrace.cs#L602)
(begin → `try` → `finally` end) — and **no** `IDisposable` over begin/end (the
existing `IDisposable`/`AutoCloseable` types wrap native *handles*, not the
begin/end pair). `[ModuleInitializer]`/`[CallerMemberName]` are **not used anywhere**
in the binding today (verified). New work:

- **`sealed class AsmTrace : IDisposable`** (new type in
  [bindings/dotnet/hwtrace/HwTrace.cs](../../bindings/dotnet/hwtrace/HwTrace.cs)).
  Ctor: `public AsmTrace(<region>, bool emit = true, [CallerMemberName] string name =
  null, [CallerLineNumber] int line = 0)` (unannotated `string`, not `string?` — the
  .NET projects set `<Nullable>disable</Nullable>`, so a `?` annotation emits CS8632).
  It (a) lazily arms (below), (b) generates the region name from `name`/`line`,
  (c) registers the traced code range under that name — see the **Region source** note
  below — then `asmtest_hwtrace_try_begin` (new shared-core §0.1 P/Invoke — add next to
  `asmtest_hwtrace_begin` at [:201](../../bindings/dotnet/hwtrace/HwTrace.cs#L201)),
  capturing `Environment.CurrentManagedThreadId`.
  `Dispose()`: `asmtest_hwtrace_end` ([:203](../../bindings/dotnet/hwtrace/HwTrace.cs#L203)),
  assert the closing thread matches the ctor's (flag `truncated` on mismatch — the
  **authoritative** check is the native OS-tid §0.2 assert; the ctor's
  `Environment.CurrentManagedThreadId` is a **complementary** managed-level guard, a
  different id space from the OS tid the single-step TF and §0.2 assert key on).
  **Always render** via `asmtest_hwtrace_render` to populate the `Path` property
  (readable after the block — statement form: `AsmTrace t; using (t = new AsmTrace(…,
  emit:false)) {…} … t.Path`); **`emit` gates only whether the rendered text is also
  written to the default sink**, not whether `Path` is produced.
- **Region source (required — the ctor must have a range to register).**
  `asmtest_hwtrace_register_region`
  ([HwTrace.cs:198](../../bindings/dotnet/hwtrace/HwTrace.cs#L198)) binds the auto-name to
  a concrete `[base, len)` code range recorded into an `asmtest_trace_t` handle and
  returns `ASMTEST_HW_EINVAL` on a null argument — so an **empty** ctor has nothing to
  register, and a `begin` under an unregistered name silently no-ops (constraint #1). The
  reference shim therefore takes the traced native region explicitly — either
  **(a)** a code parameter (`AsmTrace(NativeCode code, …)` / `base+len` + an owned trace
  handle the ctor allocates and `Dispose` frees), registering it under the auto-name; or
  **(b)** a pre-registered region the scope only *drives* — `AsmTrace(string regionName,
  …)` that skips `register_region` and calls `try_begin`/`end`/`render` by name. Pick one
  and state it per binding; the "import + scope" surface still holds because the region is
  the *thing being traced*, supplied once, not per-entry config.
- **`[ModuleInitializer] static void Arm()`** — the *ergonomic* arm-on-import: it
  probes availability (`HwTrace.Available`/`Auto`,
  [:470](../../bindings/dotnet/hwtrace/HwTrace.cs#L470)/[:517](../../bindings/dotnet/hwtrace/HwTrace.cs#L517))
  and self-skips cleanly; **actual** slot/SIGTRAP arming stays lazy at first
  `AsmTrace` per the lazy-first-scope rule. (The existing eager `LibHandle` load at
  [:100](../../bindings/dotnet/hwtrace/HwTrace.cs#L100) already handles library
  resolution; `[ModuleInitializer]` only front-loads the availability probe.)
  **Packaging:** a `[ModuleInitializer]` fires for consumers only if its compiland ships
  in the packable library. Today `HwTrace.cs` compiles into the hwtrace **smoke-test exe**
  (`hwtrace.csproj`, `OutputType Exe`), not the packable NuGet lib (`asmtest-lib.csproj`,
  which includes only `Asmtest.cs`) — so add `HwTrace.cs` (or a new `AsmTrace.cs`) to the
  packable compile items, or ship `AsmTrace` in a separate hwtrace package.
  **Analyzer note (CA2255).** A `[ModuleInitializer]` compiled into a *shipped library*
  **does** fire for downstream consumers — it runs on first use of any type in the module,
  in the consumer's process, not only when this assembly is built directly — which is
  exactly what **CA2255** ("`[ModuleInitializer]` should not be used in libraries") flags.
  (CA2255 is **default-enabled only in .NET 10**; on the net8.0 target it fires only under
  a newer SDK / raised `AnalysisLevel` — but a NuGet-published library should treat it as
  live regardless, since a downstream consumer may build at a higher analysis level.)
  That is *why* the initializer here is confined to a cheap, side-effect-free availability
  probe with the real slot/SIGTRAP arming lazy-first-scope: suppress CA2255 with that
  justification and keep the probe trim-/startup-safe. The lazy-first-scope arm makes the
  initializer an *optimisation*, not a correctness dependency — a further reason it is safe
  to ship.
- **Emit contract.** `Dispose` **always renders** to populate `Path` (a ctor can't
  detect assignment); `emit` gates only the **sink write**, and `emit:false` / `sink:` is
  the opt-out for writing (never for producing `Path`). This is the analysis's "assume no
  knowledge" default. The default sink is **stdout** (Core §0.3); a file
  (`asmtrace-<member>.txt`) is used only on an explicit `sink:`, and then tid-suffixed
  so concurrent same-named scopes don't clobber.

The `AsmTrace.Method(HotPath)` named-method form and its `MethodLoadVerbose`
address resolution are the [managed slice](scoped-tracing-managed-plan.md)'s
concern — they need the runtime-event plumbing this slice deliberately avoids.

---

## Per-binding rollout

Ordered per the analysis's phasing —
[(A) zero-hazard first, (B) the Go migration mitigation](../analysis/scoped-inprocess-tracing.md#impact-on-the-plan).
Each row: the shipped `region()` anchor, the construct to add, the arm hook, and
the auto-name mechanism.

| # | Binding | Shipped region helper | Construct to add | Arm hook | Auto-name |
|---|---|---|---|---|---|
| A1 | **Python** | [hwtrace.py:528](../../bindings/python/asmtest/hwtrace.py#L528) `region()` → `_Region` ctx-mgr ([:419](../../bindings/python/asmtest/hwtrace.py#L419)) | **extend** `_Region` to auto-name + emit-on-`__exit__` | real `import`; recommend lazy `_get()` ([:351](../../bindings/python/asmtest/hwtrace.py#L351)) | `sys._getframe(N)` walk at a **pinned depth** (CPython-only; on other impls `inspect.currentframe()` returns `None` — degrade to a synthetic collision-resistant `file:line`/counter label, not a portability path) |
| A2 | **C++** | [asmtest_hwtrace.hpp:677](../../bindings/cpp/asmtest_hwtrace.hpp#L677) `region()` → RAII `RegionScope` ([:501](../../bindings/cpp/asmtest_hwtrace.hpp#L501)) | **extend** `RegionScope` (**`noexcept`** dtor emit + tid assert) | no module-init → Meyers static `api()` ([:289](../../bindings/cpp/asmtest_hwtrace.hpp#L289)) | `__builtin_FUNCTION`+`__builtin_LINE`+`__builtin_FILE` → `file:line` name (**C++17 floor**); `std::source_location` only `#if __cplusplus >= 202002L` |
| A3 | **Zig** | [hwtrace.zig:743](../../bindings/zig/src/hwtrace.zig#L743) `region(name, args, body)` (begin/`defer` end) | add `Scope` returning a `deinit`-able value (**not RAII** — needs explicit `defer scope.deinit()`) or keep callback + emit | lazy `load()` latch ([:511](../../bindings/zig/src/hwtrace.zig#L511)) | `@src()` **passed at the call site** (Zig has no default-arg / caller-location propagation) |
| A4 | **Ruby** | [hwtrace.rb:705](../../bindings/ruby/hwtrace.rb#L705) `region(name)` block + `ensure` | block form + emit; auto-name from `caller_locations` | real `require`; recommend lazy | `caller_locations(1,1)` |
| A5 | **Lua** | [hwtrace.lua:389](../../bindings/lua/hwtrace.lua#L389) `region(name, fn)` + `pcall` | callback + emit (no `<close>` under LuaJIT 5.1) | real `require`; defensive `pcall(ffi.load)` ([:186](../../bindings/lua/hwtrace.lua#L186)) | `debug.getinfo(2)` |
| A6 | **Rust** | [hwtrace.rs:937](../../bindings/rust/src/hwtrace.rs#L937) `region()` → `Drop` guard `Region` ([:712](../../bindings/rust/src/hwtrace.rs#L712)) | **extend** `Region::drop` (emit + tid assert) | lazy `OnceLock` `hw_fns()` ([:490](../../bindings/rust/src/hwtrace.rs#L490)) | `#[track_caller]` + `Location::caller()` |
| B1 | **Go** | [hwtrace.go:826](../../bindings/go/hwtrace.go#L826) `Region(name, fn)`; `Begin`/`End` ([:798](../../bindings/go/hwtrace.go#L798)/[:816](../../bindings/go/hwtrace.go#L816)) | closure + `defer`; emit; **keep** `LockOSThread` ([:808](../../bindings/go/hwtrace.go#L808)) | `func init()` ([:608](../../bindings/go/hwtrace.go#L608)) | `runtime.Caller(1)` → `file:line` (function name needs `runtime.FuncForPC(pc).Name()`) |

*(Node and Java `region()` helpers exist —
[hwtrace.js:439](../../bindings/node/hwtrace.js#L439),
[HwTrace.java:779](../../bindings/java/HwTrace.java#L779) — but their scope
constructs ship in the [managed slice](scoped-tracing-managed-plan.md) alongside
the managed-code capability, since the analysis rates them "medium" in the same
JIT-managed tier as .NET.)*

### The three tiers, and what each proves

- **Native / no-GC (C++, Rust, Zig).** Trivial and safe — `begin == end`
  same-thread holds by construction. Rust and C++ already have true RAII (`Drop` /
  dtor); the only real gap is emit-on-close (shared-core §0.3) and auto-name. **The
  C++ dtor path must be `noexcept`** — flag `truncated` (as the .NET reference does),
  never throw or use an aborting `assert()` from a destructor. **Zig has no
  destructors, so its `Scope` value is *not* RAII** — it needs an explicit `defer
  scope.deinit()` at the call site and shares the callback form's abnormal-exit gap
  (Zig's `defer` is skipped on `panic` — document it). The trace is then simply not
  emitted on the abnormal path, never emitted-partial-as-complete.
- **Refcount / interpreter (Python, Ruby, Lua).** Easy, with real import-time arm
  hooks. **Ruby's OS-thread-migration hazard is Ractors and the M:N scheduler — not
  fibers.** A Ruby **Fiber never migrates OS threads** in any released Ruby: fibers are
  pinned to their creating thread (resuming one elsewhere raises `FiberError`), and the
  Fiber *scheduler* is itself thread-local — so a `region()` whose `begin`/`ensure` sit
  in one fiber close on the arming OS thread **when M:N is off** (fiber pinning is to the
  Ruby *thread*, not the OS thread — under M:N hazard (b) the Ruby thread's own native
  thread can still change mid-scope; fiber migration, Feature #13821, was proposed but
  never merged). The genuine hazards, for both of which the §0.2 tid assert
  is the load-bearing backstop, are: **(a) Ractors** (Ruby **3.0+**) — each runs on its
  own native thread(s), so a scope entered in a different Ractor arms on a different OS
  thread; and **(b) the M:N thread scheduler** (Ruby **3.3+**) — Ruby *threads* are
  multiplexed onto a native-thread pool, so the OS thread under a Ruby thread can change
  across a blocking point mid-scope (off by default for the main Ractor; on for non-main
  Ractors or under `RUBY_MN_THREADS=1`). On the **2.6–2.7 floor** neither exists and the
  tid assert is *defensive-only* — keep it regardless; it is correct and required once a
  supported runtime enables Ractors or M:N.
  Lua's same-thread safety comes from the **single `lua_State`**, not coroutine
  pinning; note that LuaJIT permits **yield-across-`pcall`**, so a `region()` whose
  `fn` yields leaves the marker pair open across the suspension — rely on the tid
  assert if it resumes on another OS thread, and treat a never-resumed coroutine as
  the "not emitted" abnormal-exit case. Still *safer than Ruby*.
- **Non-moving GC + M:N scheduler (Go) — the instructive middle.** Already solved by
  *pinning* the OS thread (`runtime.LockOSThread`,
  [hwtrace.go:808](../../bindings/go/hwtrace.go#L808)) — the fix for the project's
  own resolved **go-full-test flaky-crash** finding (single-step TF is per-thread).
  Go converts .NET's "follow the migration" into "forbid the migration for the
  region." **`go func()` fan-out inside the scope is *silently untraced*** —
  `LockOSThread` confines the trace to the arming OS thread, so work fanned out to
  other goroutines runs elsewhere with no stitch and no signal. The tid assert is a
  backstop **only** for the "`End` ran on the wrong thread" misuse, **not** a
  fan-out detector; the Go thread-hop test must therefore force `End` from a spawned
  goroutine to actually exercise it.

---

## Tests

Every binding already has a `hwtrace-<lang>-test` lane
([mk/native-trace.mk:547-612](../../mk/native-trace.mk#L547), aggregated by the
`hwtrace-bindings-test` rule at [:556](../../mk/native-trace.mk#L556)) run under
`hwtrace_env` and wired into CI (`hwtrace-bindings` job,
[.github/workflows/ci.yml:268](../../.github/workflows/ci.yml)). Extend each with a
scoped-trace case:

- **Per-binding scope test.** Open the new scope over a known native leaf (the same
  routine the existing `region()` test uses), assert it produces the **same**
  offsets as the callback `region()` form (proving the scope object is a faithful
  wrapper), assert emit-on-close renders non-empty text on single-step hosts, and
  assert self-skip is clean where single-step is unavailable (off Linux).
- **Auto-name test.** Assert the generated region name reflects the enclosing
  function/line (per-language stack-walk / compile-time source-location).
- **Thread-hop test** (Ruby, Go especially). Force a close on a different OS thread
  (Ruby Fiber-scheduler; Go `go func()` inside the scope) and assert the result is
  flagged `truncated`, never emitted as complete — the shared-core §0.2 assert
  surfaced through the shim.
- **Conformance.** No new corpus entries needed — the scope traces the same
  `corpus.json` routines ([bindings/conformance/corpus.json](../../bindings/conformance/corpus.json));
  add a scoped-form replay to each per-language conformance consumer
  (`bindings/python/tests/test_conformance.py`, `bindings/rust/tests/conformance.rs`,
  `bindings/go/conformance_test.go`, …) so the scope object is corpus-validated.

---

## Docs

- **Per-language binding pages** — add a "Scoped tracing" section to each of
  [docs/bindings/python.md](../bindings/python.md),
  [cpp.md](../bindings/cpp.md), [zig.md](../bindings/zig.md),
  [ruby.md](../bindings/ruby.md), [lua.md](../bindings/lua.md),
  [rust.md](../bindings/rust.md), [go.md](../bindings/go.md),
  [dotnet.md](../bindings/dotnet.md) showing the *import + scope* form and the
  emit-on-close default, with the per-language auto-name and the thread-hop caveat.
- **Shared guide** — add a "Scoped in-process tracing" section to
  [docs/guides/tracing/native-tracing.md](../guides/tracing/native-tracing.md) (or a
  new `docs/guides/tracing/scoped-tracing.md` added to the tracing `toctree` in
  [docs/guides/tracing/index.md](../guides/tracing/index.md)) covering the model,
  the thread-scope caveat, the "you get the runtime too" noise warning, the
  register-then-begin and lazy-arm rules, and the Linux-only degradation.
- **Reference** — note the new per-binding surface in
  [docs/reference/features.md](../reference/features.md) (support matrix row) and the
  self-skip semantics in [docs/reference/portability.md](../reference/portability.md).

---

## Build & CI

- No new Make objects or shared libs — the shims are source-only additions to the
  existing per-binding trees, consuming the shared-core symbols via
  `libasmtest_hwtrace` ([shared-hwtrace](../../mk/native-trace.mk#L653)).
- The per-language scope tests run in the existing `hwtrace-<lang>-test` recipes
  ([mk/native-trace.mk:559-612](../../mk/native-trace.mk#L559)) and the
  `hwtrace-bindings` / `bindings` CI jobs — no new CI job **for the docker-matrix
  languages**. Docker fan-out lanes (`docker-hwtrace-<lang>`,
  [mk/docker.mk:306-313](../../mk/docker.mk#L306)) pick them up unchanged. **Exception —
  Python:** Python is deliberately excluded from `HWTRACE_DOCKER_LANGS`
  ([mk/docker.mk:220](../../mk/docker.mk#L220)), so its scope test is **not** exercised by
  the `hwtrace-bindings` job (in the `bindings` job the hwtrace tests self-skip, since no
  `shared-hwtrace` lib is on the path). The A1 Python scope test therefore needs explicit
  CI wiring — add `python` to the hwtrace docker matrix, or run `make hwtrace-python-test`
  in a dedicated step — otherwise it runs only locally.

---

## Effort & sequencing

- **Phase A (Python + C++ first, then Zig, Ruby, Lua, Rust):** ~1–1.5 days per
  binding — less for Rust/C++/Python (RAII/ctx-mgr already present; add
  emit+auto-name+tid-assert), a little more for Zig/Ruby/Lua (add the scope shape).
  ~6–8 days total.
- **Phase B (Go):** ~1–2 days — the hard part (`LockOSThread`) is already wired;
  add the scope+emit+auto-name and the `go func()`-escape flag/test.
- **.NET reference shim:** ~1–2 days for the `AsmTrace` construct + `[ModuleInitializer]`
  + `[CallerMemberName]` auto-name (the managed-code parts are the managed slice).

**Depends on** shared-core §0 (all three symbols). **Does not depend on** §1
(per-thread state) or §2 (libipt glue) — those unblock nesting/concurrency and the
managed clean path respectively, and the shims inherit them for free once landed.

## Risks and open points

- **Emit-on-close needs shared-core §0.3.** Until `asmtest_hwtrace_render` lands,
  the shims can only record coverage, not render text — sequence §0.3 first.
- **Auto-name via stack walk has per-language edge cases** (inlining, tail calls,
  release-mode frame elision). Compile-time source-location is robust — Zig `@src()`
  and Rust `#[track_caller]`+`Location::caller()`; **C++ uses the
  `__builtin_FUNCTION`/`__builtin_LINE`/`__builtin_FILE` builtins at the C++17 floor**
  (`std::source_location` needs C++20, above the floor). `__builtin_FUNCTION` alone
  yields only the function name — two scopes in one function alias — so compose it with
  `__builtin_LINE`/`__builtin_FILE` into a `file:line` name. The interpreted stack
  walks (`sys._getframe`, `caller_locations`, `debug.getinfo`, `runtime.Caller`) are
  best-effort — and note Python's `sys._getframe` **and** `inspect.currentframe()` are
  *both* CPython-only (on other implementations `currentframe()` returns `None` rather
  than being a portability path), so the shim must null-check the frame result, document
  when the name falls back to a synthetic label, and make that fallback
  **collision-resistant**
  (e.g. `file:line`, or a monotonic counter suffix) so two distinct *unresolved* sites
  do not alias to one slot under Core §0.4's idempotent-by-name registry. **Name
  *collisions*** (a scope in a loop, or the
  same site running concurrently) are handled by Core §0.4's idempotent `register`
  (repeated names reuse one slot) plus a tid/counter qualifier where the same site can
  run on multiple threads at once (Core §1 render selection) — not by inventing a
  unique name per entry. **Watch the 64-char name ceiling:** `hw_region_t.name` is
  `char[64]` ([src/hwtrace.c:339](../../src/hwtrace.c#L339)) and `register_region`
  `snprintf`s into it ([:405](../../src/hwtrace.c#L405)), so a long absolute-path
  `file:line` auto-name **truncates** and two sites sharing a 64-char prefix would alias
  under §0.4's by-name dedup — derive the auto-name from the **basename** (+ line, + a
  short hash of the full path if needed), not the full path.
- **Zig `defer` is skipped on `panic`; Lua has no `<close>` under LuaJIT 5.1.** The
  scope still closes on the normal path; document the abnormal-exit gap (the trace
  is simply not emitted, never emitted-partial-as-complete).
- **Ruby thread affinity is the real hazard in this slice** — but via **Ractors** (own
  native threads) and the **3.3+ M:N scheduler** (Ruby threads multiplexed across a
  native-thread pool), **not fibers** (which never migrate OS threads — see the
  interpreter-tier note). Either can land `ensure` on another OS thread and disarm TF on
  the wrong one. The tid assert catches it; a full fix is the managed slice's async-hop
  territory.

## Sources

Per-binding feasibility matrix, the four ergonomic pieces, and the tiering:
[the scoped `using` analysis — cross-language section](../analysis/scoped-inprocess-tracing.md#beyond-net--extending-the-scoped-model-to-the-other-nine-bindings).
The Go migration hazard and its resolved fix:
[go-full-test flaky-crash finding](../analysis/scoped-inprocess-tracing.md#the-hard-cases-called-honestly).
