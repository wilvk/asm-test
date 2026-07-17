---
# Reachable via inline links from the published scoped-tracing guide, not the
# toctree — `orphan` tells Sphinx that is intentional (no toc.not_included warning
# under -W). The plan set it summarizes lives under docs/internal/plans/ (excluded from the
# site), so those links point at GitHub blobs, matching the repo-wide convention.
orphan: true
---

# Scoped in-process tracing — implementation summary

This document records what was **built and tested** for the scoped-tracing plan set
([scoped-inprocess-tracing-plan.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/plans/scoped-inprocess-tracing-plan.md) and its
three slices — [core](https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/plans/scoped-tracing-core-plan.md),
[bindings](https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/plans/scoped-tracing-bindings-plan.md),
[managed](https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/plans/scoped-tracing-managed-plan.md)), and what remains **forward-look**
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
re-run byte-identical (regression).

The **PT / AMD LBR / CoreSight** active-capture block is now `__thread` too, so those
backends are per-thread as well (each scoping thread owns its perf fd + rings; a
cross-thread close is impossible). Validated live for **AMD LBR** on this Zen 5:
`test_concurrent_amd` (two threads each open their own AMD perf fd concurrently —
deterministic, since the process-global slot would have refused the second) passes in
the capped `make docker-hwtrace-amd` lane. The **PT / CoreSight** per-thread *code* is
in place but the live per-thread-AUX validation needs the silicon (Intel PT board /
AArch64 CoreSight board). Separately, the AMD end path now enforces the
**never-empty-yet-complete honesty invariant** (a branch window that reconstructs to
zero in-region instructions is flagged `truncated`).

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

**Core test results:** `make hwtrace-test` → **201 passed, 0 failed** (up from 137;
+9 for the §D3 bundled-helper discovery + dual-path assertions, +9 for the §Z0/§Z1
region-free whole-window scope); `make codeimage-test` → 12 passed; all on any x86-64
Linux, no hardware.

## Zero-config (capstone) slice — the empty-ctor form, §Z0/§Z1 WEAK tier (DONE) + forward-look

The aspirational `using (new AsmTrace())` — no `NativeCode`, no `[base,len)` — now works
end-to-end on any x86-64 Linux via the single-step **WEAK** tier, per
[scoped-tracing-zeroconfig-plan.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/scoped-tracing-zeroconfig-plan.md).

- **§Z0 region-free arm surface (DONE).** New C entry points
  `asmtest_hwtrace_begin_window` / `_end_window` / `_render_window`
  ([src/hwtrace.c](../src/hwtrace.c)) over a whole-window frame mode in
  `asmtest_ss_begin_window` ([src/ss_backend.c](../src/ss_backend.c)): the frame carries
  no `[base,len)`, so the SIGTRAP handler records the **absolute** RIP of every
  instruction the thread runs in the window (no in-range filter) into the shipped bounded
  ring — overflow honestly flags `truncated`. The `.NET` reference shim gains the
  parameterless `new AsmTrace()` ctor + `SkipReason` ([bindings/dotnet/hwtrace/HwTrace.cs](../bindings/dotnet/hwtrace/HwTrace.cs)).
- **§Z1 WEAK tier (DONE).** `asmtest_hwtrace_render_window` disassembles the recorded
  absolute addresses from **live self memory** (valid for non-moving native code); moving
  managed bytes route to the shipped `asmtest_hwtrace_render_versioned` (§Z3, forward-look).
- **§Z4 default + §Z5 scaffold (DONE).** `end_window` flags `truncated` when the handle
  does not resolve on the closing thread (the cross-thread-hop honesty default); the empty
  ctor self-skips with an actionable `SkipReason` (never throws) where no faithful backend
  exists.
- **Tests:** `test_wholewindow_singlestep` (checks 119–127: region-free arm, absolute-address
  capture, live-memory render finding `ret`, cross-thread `truncated`) in `make docker-hwtrace`
  (201/0); the `.NET` `AsmTrace()` case in `make docker-hwtrace-dotnet` (33/0). Both validated
  on this AMD host.
- **Forward-look:** the STRONG whole-window PT / CEILING AMD LBR capture tiers (bare-metal
  Intel PT / Zen 3+), §Z2 live decode, §Z3 arbitrary-managed-method capture (needs a live
  runtime + MethodLoadVerbose), the §Z4 stitching escalation, and the other nine binding
  shims (mechanical mirrors of the .NET reference). All ship self-skipping and gated.

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
- **§D3 the concealed out-of-process ptrace-stealth stepper + standalone-binary
  bundling** — implemented + CI-runnable. `asmtest_hwtrace_stealth_trace`
  reverse-attaches to the caller (`prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY)` +
  `PTRACE_SEIZE`), drives it to the region entry (`run_to`), and single-steps the
  region out of band while the caller runs it (reusing the shipped
  `asmtest_ptrace_trace_attached` W2 tracer against a shared shadow trace). The
  stepping body + bundled-binary discovery moved to
  [src/stealth_helper.c](../src/stealth_helper.c) so the SAME code runs either as an
  in-process forked child (the fallback) or as the **standalone
  `asmtest-stealth-helper` binary** — a real separate process the managed packages can
  ship — discovered via a dladdr-sibling lookup (mirroring `drtrace_app.c`) or the
  `ASMTEST_STEALTH_HELPER` override, with the shared scratch handed over a memfd (it
  survives `execv`, unlike the fork path's anonymous mapping). Build/install:
  `$(BUILD)/asmtest-stealth-helper` + `install-stealth-helper` in
  [mk/native-trace.mk](../mk/native-trace.mk). Host test `test_ptrace_scoped_stealth`
  (checks 158–166) asserts **both** paths reconstruct the identical `[0,3,6,c,11]`
  offsets on any ptrace-capable Linux; it self-skips (`EUNAVAIL`) where Yama refuses
  the reverse attach, and an `alarm()` watchdog in the helper means it never hangs CI.
  This is the hardware-free scope path for Zen 2 / Docker-on-Mac.
- **§D3 package embedding** — the built standalone helper is now bundled into every
  native payload slot beside `libasmtest_hwtrace` and `$ORIGIN`-rpath'd so it resolves
  the co-vendored Capstone in-package ([scripts/package-native.sh](../scripts/package-native.sh)),
  copied into every managed payload (NuGet `runtimes/<rid>/native`, npm/Maven/gem/rock
  via `emu_lib_slots`, the Python wheel `_libs/`), and asserted present + rpath'd (and
  not leaked into a darwin slot) by a fail-closed `package-libs-verify` gate
  ([mk/bindings.mk](../mk/bindings.mk)). Clean-room-verified: a full `package-libs` slot
  passes the complete-set gate, `ldd asmtest-stealth-helper` resolves the in-slot
  `libcapstone.so.5`, and a `dlopen` of the bundled `libasmtest_hwtrace.so` discovers the
  helper as its dladdr sibling with the environment scrubbed.
- **Forward-look:** §D0 (.NET `MethodLoadVerbose` address resolution, `AsyncLocal`
  async-hop), the §D1/§D2 per-runtime async-hop hooks, and — for §D3 — the cross-process
  address channel for a live JIT's `MethodLoadVerbose` addresses. These need managed
  runtimes or PT hardware to validate, and are sequenced last exactly as the plan states.

## Build fix (incidental, unblocks the binding test lanes)

The `hwtrace-rust-test` / `hwtrace-go-test` recipes list `$(CORPUS_LIB)` as a
prerequisite, but `mk/bindings.mk` (which defines it) is included *after*
`mk/native-trace.mk`, so the prerequisite expanded to **empty** and the corpus fixture
lib was never built — cargo/cgo then failed to link `-lasmtest_corpus`. Fixed by
hoisting the `CORPUS_LIB` definition into `mk/native-trace.mk` (identical `:=`, so
`bindings.mk` redefines it harmlessly). See [mk/native-trace.mk](../mk/native-trace.mk).

## What Docker can and cannot validate here

Containers share the host kernel + CPU, so the dividing line is the **host's silicon**,
not Docker. This dev host is an **AMD Ryzen 9 9950X (Zen 5)**: `intel_pt` PMU absent,
kernel BTF present. Everything reachable on that hardware is now implemented and
validated in a capped Docker lane; the rest is marked with the architecture / OS it
needs.

### Validated in Docker on this AMD host

| Item | Lane | Needs |
|---|---|---|
| Core §1 per-thread **AMD LBR** capture (`test_concurrent_amd`) + the AMD honesty invariant | `make docker-hwtrace-amd` | AMD Zen 3+ + `--cap-add=PERFMON` (this box) |
| §D3 reverse-attach ptrace-stealth stepper **+ standalone `asmtest-stealth-helper` binary** (both paths, `test_ptrace_scoped_stealth` checks 158–166) | `make hwtrace-test` / any ptrace lane | any ptrace-capable Linux |
| §D3 helper **package embedding** — full `package-libs` slot, complete-set `package-libs-verify`, in-slot Capstone resolution, dladdr-sibling discovery from the bundled `.so` | `make package-libs && make package-libs-verify` (bindings-base image) | x86-64 Linux + emu toolchain (Capstone/patchelf) |
| Core §0/§1-single-step/§2-adapter/§3-symbolize/§D4-merge + all 10 binding scopes | `make docker-hwtrace` + per-binding lanes | any x86-64 Linux |
| eBPF code-emission detector | `make docker-hwtrace-codeimage` | `CAP_BPF` + kernel BTF (present) |
| Managed-JIT tracing via the ptrace/W2 path (.NET/Node/Java) | `make docker-hwtrace-jit-{dotnet,node,java}` | a live runtime (in the image) |

### Remaining — blocked on architecture / OS (Docker cannot substitute)

| Item | Requires |
|---|---|
| Core §2 live whole-window libipt decode + capture-side address filter | **bare-metal Intel PT** (no `intel_pt` on AMD; a synthetic PT-packet fixture is the only non-hardware route) |
| Core §1 per-thread PT/CoreSight **AUX** live validation | **bare-metal Intel PT** / an **AArch64 CoreSight board** (the per-thread *code* is done) |
| Core §3.1(b) emission **slicing** (trace-position ↔ emission-timestamp correlation) | **Intel PT** whole-window decode for per-IP positions (the eBPF detector itself is done) |
| Core §3.2 **live** AMD `data_tail` mid-capture drain | **AMD Zen 3+** — deferred: live capture is PMU-variable (no deterministic assertion); reconstruction is tested |
| Core §3.2 PT `aux_tail` circular drain | **bare-metal Intel PT** |
| Single-step **Windows** front-end (VEH) | a **Windows** host (the macOS-Intel front-end has since landed — see the note below) |
| §D0 live managed-JIT capability (`MethodLoadVerbose`, `AsyncLocal` hook) + §D1/§D2 async-hop hooks | a **live .NET/Node/JVM** runtime (Docker-reachable, but large; the ptrace tracing path already works) + **Intel PT** for cross-thread async-hop stitching |
| §D3 **cross-process address channel** — feeding the helper a live JIT's `MethodLoadVerbose` addresses over the process boundary (the standalone binary, build/install, dladdr discovery, **and per-ecosystem package embedding** are all done above) | a **live .NET/Node/JVM** runtime |

### Validated natively on a macOS-Intel host (not Docker — Docker-on-Mac is Linux)

The single-step tier's **macOS-Intel front-end** is the one Phase-5 front that a Linux
CI host (or Docker-on-Mac, whose containers are Linux) *cannot* exercise: it needs a
real x86-64 Darwin userspace. It shares the in-process EFLAGS.TF stepper with Linux —
XNU delivers the `#DB` as a BSD `SIGTRAP`, so re-asserting `TF` in the saved thread
state re-arms stepping across `sigreturn` — with only the feature-test macro
(`_DARWIN_C_SOURCE`) and the mcontext field access (`uc_mcontext->__ss.__rip`/`__rflags`)
differing, both isolated behind shims in [src/ss_backend.c](../src/ss_backend.c). The
[src/hwtrace.c](../src/hwtrace.c) facade (region table + mutex, `init`/`register`,
`begin`/`end`/`begin_scope`/`render_scope`, arm-tid backstop) is gated by
`HWTRACE_LIFECYCLE` (a superset of `__linux__`, so the Linux path is byte-for-byte
unchanged) with every perf/PT/AMD block kept behind a narrower `__linux__` guard.

| Item | Lane | Needs |
|---|---|---|
| Single-step live capture + 62-insn loop (no depth ceiling) + `try_begin` nesting/compose/EFULL + `begin_scope`/`render_scope` | `make hwtrace-test` (native, no Docker) | an **x86-64 macOS** host — **61 pass** on this box |

Per the project's no-untested-hardware-code rule, every remaining live path ships
**self-skipping and gated**, and the gate is exercised on every host.
