# Scoped in-process tracing — implementation summary

This document records what was **built and tested** for the scoped-tracing plan set
([scoped-inprocess-tracing-plan.md](plans/scoped-inprocess-tracing-plan.md) and its
three slices — [core](plans/scoped-tracing-core-plan.md),
[bindings](plans/scoped-tracing-bindings-plan.md),
[managed](plans/scoped-tracing-managed-plan.md)), and what remains **forward-look**
(gated on hardware / managed runtimes the CI host lacks — this dev box is AMD, with no
Intel PT and no bare-metal perf privilege in containers).

The developer-visible target the plans set out to deliver — *package import + a scope*
around a region, yielding the assembly that executed inside it — is implemented across
**all ten language bindings** and validated on any x86-64 Linux via the single-step
backend (no PMU, no perf_event, no privilege, no decoder beyond Capstone).

```csharp
using (new AsmTrace())      // the scope — auto-named, arms lazily
{
    HotPath(data);          // its executed asm is captured + rendered on close
}
```

## Shared C / decode core

All new symbols compile into the existing `libasmtest_hwtrace` objects (no new object
files, no new pkg-config knob) and are declared in
[include/asmtest_hwtrace.h](../include/asmtest_hwtrace.h).

### §0 — cheap C-layer fixes + shared render-on-close (DONE, host-tested)

| Symbol | Role |
|---|---|
| `asmtest_hwtrace_try_begin(name)` | error-returning begin: `ESTATE` on a busy slot / `EINVAL` on an unregistered name; `begin` is now a thin status-discarding wrapper (shipped `void` ABI unchanged) |
| `asmtest_hwtrace_arm_tid()` | the OS tid that armed the active capture (thread-scope assert) |
| `asmtest_hwtrace_render(name, buf, buflen)` | render recorded offsets → Capstone disassembly text; snprintf size-then-allocate; truncated/over-capacity rendered as a labelled prefix |
| `register_region` idempotent-by-name | a repeat registration refreshes the slot in place, so a looped/sprinkled scope reuses one slot; the 32-entry table counts distinct sites |

`end` compares the closing thread against the arming thread and flags `truncated` on a
mismatch — the never-emit-partial-as-complete backstop. Tests: `test_try_begin_busy`,
`test_arm_tid_mismatch`, `test_render_singlestep`, `test_register_idempotent`.

### §1 — per-thread single-step state (DONE for single-step; PT/AMD forward-look)

[src/ss_backend.c](../src/ss_backend.c) is rewritten from a single process-global slot
to a **per-thread TLS range stack**: nested `begin`/`end` on one thread compose
(innermost region is a subset of the outer), and concurrent scopes on different threads
are independent — fixing the flaky-crash class the Go binding hit. Correctness
mechanics per the plan:

- Handler-touched TLS (`tls_frames`, `tls_depth`) is forced to the **initial-exec**
  model (the general-dynamic first-touch of a `__thread` var in a `dlopen`'d `.so` can
  route through `__tls_get_addr` and lazily `malloc` — not async-signal-safe).
- The 512 KiB ordered-RIP buffers stay heap-`malloc`'d; only pointers/offsets live in
  the small (8-frame) TLS stack.
- `g_armed` stays a process-global non-TLS atomic (a coarse belt sparing the unarmed
  early-return from touching TLS); the real per-thread gate is the range-stack depth.
- The process-wide SIGTRAP disposition is gated by an explicit **arm-refcount**
  (install on 0→1, restore on 1→0) so a second concurrent `begin` can't overwrite the
  saved disposition with asm-test's own handler.
- The region registry is guarded by a **process-global mutex** (register/find is a
  read-modify-write; the signal handler never touches it, so no async-signal constraint).

Handle-keyed additive trio: `asmtest_hwtrace_begin_scope` / `_render_scope` /
`_render_versioned` (per-thread scope handle `{idx, gen}`; a stale/closed handle is
rejected). Tests: `test_nested_singlestep`, `test_concurrent_singlestep`,
`test_concurrent_samename`, `test_render_versioned`; `test_singlestep_live`/`_loop`
re-run byte-identical (regression). The **PT/CoreSight/AMD per-thread AUX-ring
migration is forward-look** (validates only on bare-metal Intel PT), so those backends
keep the process-global slot.

### §2 — recorder-backed image callback (host-testable adapter DONE; live PT forward-look)

[src/pt_backend.c](../src/pt_backend.c) gains `asmtest_pt_read_codeimage(img, when, ip,
buf, size)` — a libipt-independent adapter that serves the bytes live at `(ip, when)`
from the self/foreign code-image recorder (the temporal-bytes rule). The libipt
`read_recorder` callback + whole-window `asmtest_pt_decode_window` (a distinct callback
/ record policy selected by mode) build under libipt. Host test
`test_pt_image_from_codeimage` drives the adapter at two `when` values with **no PT
hardware and no libipt**; end-to-end libipt decode is forward-look on real Intel PT.

### §3 — whole-window completeness (host-testable halves DONE)

§3.1(c) symbolize-and-bucket ships the new address→name **reverse resolver**
`asmtest_hwtrace_region_name` (keeps the `/proc/<pid>/maps` pathname the shipped
helpers discard), a perf-map reverse search, and `asmtest_hwtrace_symbolize_bucket`.
§3.2's AMD `data_tail` drain **reconstruction** is covered by
`test_amd_drain_reconstruction`. Tests: `test_symbolize_bucket`,
`test_amd_drain_reconstruction`. Forward-look: emission-event slicing (§3.1b, eBPF /
soft-dirty at live capture), the live AMD `data_tail` drain (Zen 3+), and the PT
`aux_tail` circular walk (Intel PT).

### §D4 — async-hop stitching merge core (DONE, CI-runnable)

`asmtest_hwtrace_stitch(slices, n, out, bounds, nbounds)` plus the slice/bound ABI
(`asmtest_hwtrace_slice_t`, `asmtest_hwtrace_slice_bound_t`) — a pure ordered
concatenation by `seq` (no decode), host-testable from synthetic pre-decoded slices.
Test: `test_stitch_slices`. This closes the async-hop **merge** test hole; the live
per-runtime tag→merge hooks remain forward-look.

**Core test results:** `make hwtrace-test` → **183 passed, 0 failed** (up from 137);
`make codeimage-test` → 12 passed; all on any x86-64 Linux, no hardware.

## Bindings slice — the scope construct across all ten tiers (DONE)

Each binding gained the shared-core FFI symbols (`try_begin`, `render`) and a scope
construct with **auto-name from the call site**, register-then-begin under that name,
render-on-close, and the thread-scope `truncated` bit. Every per-binding
`hwtrace-<lang>-test` lane's scope case passes green.

| Binding | Construct | Auto-name |
|---|---|---|
| Python | `HwTrace.scope(code)` (`with`) | `sys._getframe` → `basename:line` |
| C++ | `asmtest::ScopedTrace` (RAII, `noexcept` dtor) | `__builtin_FILE`/`__builtin_LINE` default args (C++17) |
| Zig | `HwTrace.scope(code, @src(), emit)` → `Scope` (explicit `defer deinit`) | `@src()` at the call site |
| Ruby | `HwTrace#scope(code) { … }` (block + `ensure`) | `caller_locations` |
| Lua | `HwTrace:scope(code, fn)` (callback + `pcall`) | `debug.getinfo` |
| Rust | `HwTrace::scope(code, emit)` → `ScopedTrace` (`Drop`) | `#[track_caller]` + `Location::caller` |
| Go | `HwTrace.Scope(code, emit, fn)` (closure + `LockOSThread`) | `runtime.Caller` |
| .NET | `new AsmTrace(code)` (`IDisposable`, reference shim) | `[CallerMemberName]`/`[CallerLineNumber]` |
| Node | `HwTrace#scope(code, fn)` (callback; `using` sugar on Node 24+) | stack-frame parse |
| Java | `HwTrace.AsmTrace.scope(code)` (try-with-resources) | `StackWalker` |

Each shim renders exactly the executed instruction lines (verified: `add2(20,22)` → 5
lines; `add2(200,50)` → 6 lines; includes the `ret`), self-skips cleanly where no
backend is available, and honours the two correctness constraints (register-then-begin
under the same name; lazy first-scope arm). Go additionally documents + tests the
`go func()` fan-out gap (fanned-out work runs on another OS thread and is silently
untraced). The .NET `AsmTrace` is the canonical reference shape the plan calls for.

## Managed slice — the testable half (DONE) + forward-look

- **§D4 merge core** and **`asmtest_hwtrace_render_versioned`** (version-aware render
  of a trace's absolute addresses against the code-image version live as of `when`) —
  implemented + host-tested (`test_stitch_slices`, `test_render_versioned`).
- **§D1 Node** and **§D2 Java** scope constructs over a native leaf — implemented +
  tested (the managed-code capability's testable surface today).
- **Forward-look:** §D0 (.NET `MethodLoadVerbose` address resolution, `AsyncLocal`
  async-hop), the §D1/§D2 per-runtime async-hop hooks, and §D3 (the concealed
  out-of-process ptrace-stealth stepper — reverse-attach `PR_SET_PTRACER` +
  `PTRACE_SEIZE` protocol, cross-process address channel, per-ecosystem bundling).
  These need managed runtimes, PT hardware, or a second process to validate, and are
  sequenced last exactly as the plan states.

## Build fix (incidental, unblocks the binding test lanes)

The `hwtrace-rust-test` / `hwtrace-go-test` recipes list `$(CORPUS_LIB)` as a
prerequisite, but `mk/bindings.mk` (which defines it) is included *after*
`mk/native-trace.mk`, so the prerequisite expanded to **empty** and the corpus fixture
lib was never built — cargo/cgo then failed to link `-lasmtest_corpus`. Fixed by
hoisting the `CORPUS_LIB` definition into `mk/native-trace.mk` (identical `:=`, so
`bindings.mk` redefines it harmlessly). See [mk/native-trace.mk](../mk/native-trace.mk).

## Where validation stops

Per the project's no-untested-hardware-code rule, every hardware-gated path ships
**self-skipping and gated**, and the gate is exercised on every host. The forward-look
items above are written where their design is pinned (the §D4 ABI, `render_versioned`,
the whole-window `decode_window`) but their live paths need Intel PT, AMD Zen 3+,
`CAP_BPF`/`CAP_PERFMON`, or a live managed runtime — none available on this AMD dev host
or in the default container sandbox, exactly as the hardware-trace and AMD plans already
accept for their own capture halves.
