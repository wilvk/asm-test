# Changelog

All notable changes to asm-test are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims
to follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Clean-room install test — every bundled binding, on Linux and macOS, in CI.**
  `make clean-room-test` (any host) / `make macos-clean-test` (darwin alias) packages
  each binding that ships a native payload, installs it **fresh** into a throwaway
  prefix, loads it with every `ASMTEST_*`/`DYLD_*`/`LD_*` override scrubbed and the
  cwd outside the checkout, then **asserts the native library it actually resolved
  lives under that fresh install** — never a leaked dev `build/` tree, a Homebrew
  dylib, or `/usr/local`. So "install fresh, no `ASMTEST_LIB`" is *proven*, not
  trusted: the prior per-binding smokes only checked a tier was *available*, which a
  leaked `build/` or Homebrew dylib also satisfies. Bindings whose toolchain is absent
  self-skip; a real leak fails the run.
  - **All six dlopen bindings** are covered — **Python, Ruby, Node, Java, Lua, and
    .NET**. Each core loader gained a resolved-path accessor: `library_path`
    (Ruby/Lua), `libraryPath()` (Node/Java), `Emu.LibraryPath` (.NET, via
    `Process.Modules` so it reports the real loaded path however P/Invoke resolved the
    name), and Python's existing `python -m asmtest --where`. The **link** bindings
    (C++/Rust/Go/Zig) ship source and link `libasmtest` themselves — no bundled payload
    to leak-check — so they are intentionally out of scope.
  - **Verified in Docker** per language: `make docker-clean-<lang>` builds the binding's
    isolated image and runs the clean-room test in it with `CLEANROOM_ONLY=<lang>` — so a
    self-skip **fails** the lane (a missing toolchain can't pass vacuously); `make
    docker-clean-room` runs the set. A new **`clean-room` CI job** (matrix over
    Ruby/Node/Java/.NET/Lua) gates every push, complementing the conformance `bindings`
    job (which loads the dev `build/` tree). Python's clean-room stays in the existing
    release.yml python job (which asserts on the repaired wheel — self-containing the
    wheel needs `auditwheel`/`build` the lean test image omits).
  - New reusable pieces: [`scripts/clean-env.sh`](https://github.com/wilvk/asm-test/blob/main/scripts/clean-env.sh) — a sourceable
    env scrubber that **pins** `DYLD_FALLBACK_LIBRARY_PATH` to `/usr/lib` rather than
    unsetting it (unsetting reverts to a dyld default that *includes* `/usr/local/lib`,
    where a Homebrew copy could still satisfy a bare-leaf load);
    [`scripts/assert-clean-path.sh`](https://github.com/wilvk/asm-test/blob/main/scripts/assert-clean-path.sh) — the leak guard
    (rejects the checkout, `/opt/homebrew`, `$HOMEBREW_PREFIX`, `/usr/local`; allows a
    temp extraction, e.g. the jar's); and
    [`scripts/clean-room-test.sh`](https://github.com/wilvk/asm-test/blob/main/scripts/clean-room-test.sh) — the cross-platform
    per-binding orchestrator (the first reusable *local* one; the release.yml smokes can
    call it next, per the plan's Track E).
    ([macOS clean-test plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/macos-clean-test-plan.md), Track A)
- **The native-trace tiers now ship *inside* the packages.** Both optional tiers —
  DynamoRIO (`libasmtest_drapp` + `libasmtest_drclient` + the pinned `libdynamorio`)
  and hardware trace (`libasmtest_hwtrace`) — are staged into the Linux payload slots
  by `make package-libs`, so a fresh `pip install` / gem / npm / nupkg / jar / rock runs
  `NativeTrace` / `HwTrace` on a capable host with **no manual `make shared-*` and no
  `DYNAMORIO_HOME`**, exactly as the emulator/Keystone/Capstone tiers already do. drtrace
  is `linux-x86_64` only (DynamoRIO auto-fetched via
  [`scripts/fetch-dynamorio.sh`](https://github.com/wilvk/asm-test/blob/main/scripts/fetch-dynamorio.sh)); hwtrace bundles on every
  Linux slot (single-step + ptrace always; the Intel PT / AMD / CoreSight decoders
  self-skip off the hardware they need). macOS/arm64 slots simply omit the Linux-only
  tier and the wrapper self-skips (`available()` → false) — **no API or `available()`
  behavior change**, bundling only removes the build step.
  - Every binding's `drtrace`/`hwtrace` loader learned a **bundled-package candidate**
    (env override → bundled slot → dev `build/` → system) and a **`library_path()`**
    self-report (`python -m asmtest --where`, and the equivalent accessor in the Go /
    Rust / Ruby / Node / Java / .NET / Lua / Zig wrappers) so a clean-room test can
    assert the tier resolved from the package, not a leaked checkout.
  - A **package-bundled `libdynamorio` self-locates next to `libasmtest_drapp`** (via
    `dladdr`), so the DynamoRIO tier works with zero configuration — `dlopen` does not
    consult a library's own `RUNPATH`, so drapp finds its sibling explicitly.
  - Licensing unchanged in character: DynamoRIO (BSD-3-Clause core), and the "full"
    hwtrace's libipt/OpenCSD/libbpf, are all permissive — `collect-licenses.sh` emits
    each only when the lib is actually staged, adding no copyleft beyond the existing
    Unicorn/Keystone GPL-2.0. The four **source-distributed** bindings (Rust/Zig/C++/Go)
    ship no binary payload, so their consumers build `shared-drtrace`/`shared-hwtrace`
    themselves (documented, not bundled). ([bundle-native-trace-tiers
    plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/bundle-native-trace-tiers-plan.md))
- **Native runtime tracing (two optional tiers).** A third execution tier that
  traces code running *natively, in-process*, complementing the Unicorn emulator
  trace. Both fill the same engine-neutral `asmtest_trace_t` shape (now extracted
  into `include/asmtest_trace.h` + `src/trace.c`, shared by all backends) and the
  Capstone annotation layer renders any backend's offsets. ([Native runtime
  tracing](https://github.com/wilvk/asm-test/blob/main/docs/native-tracing.md))
  - **DynamoRIO in-process tier** (`asmtest_drtrace.h`, `libasmtest_drapp` +
    CMake-built `libasmtest_drclient.so`): `dr_app_*` in-process attach with an
    enforced lifecycle state machine, begin/end region markers, basic-block and
    instruction coverage, and host-native W^X executable-memory allocation
    (`asmtest_exec_alloc` / `asmtest_asm_exec_native`). Uses DynamoRIO's BSD core
    API only — no drmgr/drwrap, so no LGPL-2.1 obligation. Native-trace wrappers
    for **every** language binding — Python (`asmtest.drtrace`), C++, Rust, Go,
    Node, Java, .NET, Ruby, Lua, and Zig — each exposing the same
    `NativeTrace`/`NativeCode` surface and dlopen-loading `libasmtest_drapp` at
    run time, so the core binding never link-depends on DynamoRIO and each wrapper
    self-skips (`available()` → false) when the tier is absent. Targets
    `drtrace-test`, `shared-drtrace`, `drtrace-client`, `drtrace-<lang>-test`,
    `drtrace-bindings-test`, `docker-drtrace`, and `docker-drtrace-bindings`
    (container lanes with DynamoRIO installed). Gated on `DYNAMORIO_HOME`;
    self-skips when absent. All wrappers are verified against a real in-process
    DynamoRIO in Docker: C++/Ruby/Java/Lua/Zig/Rust/Go trace live; Node and .NET
    self-skip there (in-process DynamoRIO can't take over a JIT/GC runtime's
    threads — the managed-runtime limitation, where Intel PT is the recommended
    backend). Linux x86-64.
  - **Hardware-trace tier** (`asmtest_hwtrace.h`, `libasmtest_hwtrace`): four
    backends behind one API, one `available()` gating chain, and one
    `asmtest_trace_t` sink. **Intel PT** capture via `perf_event_open` + libipt
    decode with branch-boundary block normalization; **AMD LBR** (Zen 3 BRS / Zen 4
    LbrExtV2, 16-deep, exact within window then `truncated`); **ARM CoreSight**
    (OpenCSD) scaffold; and **single-step** (`EFLAGS.TF` → `#DB`/`SIGTRAP`), the
    portable backend that records the same exact/complete offsets on **any x86-64
    Linux** host (Intel, any-Zen AMD, VM, CI, plain container) with no PMU,
    perf_event, privilege, or decoder library. `asmtest_hwtrace_available()` encodes
    the full detect-and-skip chain; the PT/AMD/CoreSight backends self-skip off the
    bare-metal hardware they need (the common case). Targets `hwtrace-test`,
    `shared-hwtrace`, `hwtrace-<lang>-test`, `hwtrace-bindings-test`,
    `docker-hwtrace`, and `docker-hwtrace-bindings` (plain unprivileged container
    lanes); auto-detects libipt/OpenCSD via pkg-config.
  - **Hardware-tier backend auto-selection.** `asmtest_hwtrace_resolve(policy, out,
    cap)` returns the host's available backends most-faithful first (Intel PT > AMD
    LBR > single-step > CoreSight); `asmtest_hwtrace_auto(policy)` returns the single
    best pick ready to `init` (or `ASMTEST_HW_EUNAVAIL`). `policy` is
    `ASMTEST_HWTRACE_BEST` or `ASMTEST_HWTRACE_CEILING_FREE` (drops the one
    fixed-window backend, AMD LBR — what a caller re-resolves under after a trace
    comes back `truncated`). On any x86-64 Linux host the cascade is non-empty
    (single-step is the floor), so `auto()` never fails there. Exposed through the C
    API **and every language wrapper** — Python, C++, Rust, Go, Node, Java, .NET,
    Ruby, Lua, Zig — each surfacing `resolve`/`auto` (C++ uses `auto_select`, since
    `auto` is a keyword) with `BEST`/`CEILING_FREE` policy constants, plus a
    per-binding self-test of the selection invariants and a live auto-picked trace.
    Scope is the hardware tier's own backends; a cross-tier fall to DynamoRIO/the
    emulator stays a deliberate, fidelity-aware caller decision.
  - **Cross-tier trace orchestration.** `asmtest_trace_resolve(policy, out, cap)` /
    `asmtest_trace_auto(policy, &choice)`
    ([asmtest_trace_auto.h](https://github.com/wilvk/asm-test/blob/main/include/asmtest_trace_auto.h),
    `src/trace_auto.c`) are the front-end **over all three tiers**, not just the
    hardware backends: they walk the full descending-fidelity cascade — Intel PT →
    AMD LBR → **DynamoRIO** → single-step → CoreSight → **emulator** (DynamoRIO ranks
    above single-step because its code cache runs at native speed while single-step
    pays a per-instruction kernel round-trip) — and return `asmtest_trace_choice_t`
    descriptors `{tier, backend, fidelity}`. It calls `asmtest_hwtrace_available()`
    directly and **dlopen-probes** `libasmtest_drapp` (via `$ASMTEST_DRAPP_LIB`) for
    the DynamoRIO tier, so it hard-links neither the DynamoRIO nor the emulator
    library — the three stay decoupled. The `policy` bitmask composes
    `ASMTEST_TRACE_BEST`, `ASMTEST_TRACE_CEILING_FREE` (drop AMD LBR; re-resolve under
    it after `truncated`), and `ASMTEST_TRACE_NATIVE_ONLY` — **the flag that forbids
    the native→emulator fidelity crossing**: under it the emulator floor is dropped,
    so a host with no native tier resolves to `ASMTEST_HW_EUNAVAIL` rather than
    silently downgrading real-CPU execution to an isolated guest. Shipped in
    `libasmtest_hwtrace` and exposed through **every language wrapper** (Python/Rust/
    Go/Lua/Ruby `resolve_tiers`/`auto_tier`, camelCase `resolveTiers`/`autoTier` for
    C++/Node/Java/.NET/Zig, `ResolveTiers`/`AutoTier` for Go), each with a per-binding
    self-test of the cross-tier invariants. This is the cross-tier front-end the
    [trace parity matrix](https://github.com/wilvk/asm-test/blob/main/docs/analysis/trace-parity-matrix.md)
    flagged as the remaining gap.
  - **Out-of-process single-step backend (W2).** `asmtest_ptrace_trace_call(code,
    len, args, nargs, &result, trace)`
    ([asmtest_ptrace.h](https://github.com/wilvk/asm-test/blob/main/include/asmtest_ptrace.h),
    `src/ptrace_backend.c`) is the out-of-process sibling of the in-process
    `EFLAGS.TF` stepper: a tracer **parent** `PTRACE_SINGLESTEP`s a forked tracee
    that runs the registered routine, reads the program counter from the child's
    register file at each stop, and reconstructs the **same exact offsets** in the
    parent — ordered
    in-region instruction offsets and the identical single-entry/ends-at-branch block
    partition the in-process stepper, Unicorn, DynamoRIO, and Intel PT produce — with
    no shared memory (the parent observes every step) and no library or privilege
    beyond ptrace of one's own child. Because it touches none of the tracee's signal
    disposition or code cache, it is the exact path for a **JIT/GC managed runtime**
    (JVM/.NET/Node) and the recommended managed-runtime backend on **AMD** (no Intel
    PT), and is the only single-step form possible on **AArch64** (whose
    `MDSCR_EL1.SS` is kernel-only). It runs on **Linux x86-64 and AArch64** off one
    body: the AArch64 arm reads the program counter + integer return register via
    `PTRACE_GETREGSET`/`NT_PRSTATUS` (AArch64 has no `PTRACE_GETREGS`) and decodes block
    lengths with `ASMTEST_ARCH_ARM64` Capstone, while the fork/SIGSTOP/step/wait flow and
    the SysV/AAPCS64 register-arg call are shared. Built into
    `libasmtest_hwtrace`; `make hwtrace-test` exercises it live — including in a
    **plain unprivileged container** — asserting byte-for-byte parity with the
    in-process stepper plus a 62-instruction loop (no depth ceiling). This lands the
    Linux x86-64 front of the [Zen 2 single-step plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/zen2-singlestep-trace-plan.md)
    Phase 5 (W2). `asmtest_ptrace_trace_attached(pid, base, len, &result, trace)`
    extends it to the **foreign-process** case — the building block for tracing a
    managed runtime: it traces a region in a **separate, already-running process you
    have attached to externally** (the caller owns the `PTRACE_ATTACH`/`DETACH`
    policy), single-stepping the target from its current stop and reading the region
    bytes **from the target via `process_vm_readv`** (so the tracer does not share the
    target's memory). A live test attaches to a child that never called
    `PTRACE_TRACEME` and reconstructs the same `[0,3,6,c,11]` stream out of band.
    Two **region resolvers** turn the attach primitive into "point it at a running
    process": `asmtest_proc_region_by_addr(pid, addr, &base, &len)` finds the
    executable mapping containing `addr` in `/proc/<pid>/maps` (one interior address →
    the whole region to trace), and `asmtest_proc_perfmap_symbol(pid, name, &base,
    &len)` parses `/tmp/perf-<pid>.map` — the text format V8/Node, .NET, and OpenJDK
    (+perf-map-agent) write so `perf` can symbolize **generated code** — to recover a
    JIT method's `(base, len)` by name. A live test discovers a foreign process's
    region from `/proc/<pid>/maps` using only an interior address and traces *that*
    region (no hardcoded base). `asmtest_ptrace_run_to(pid, addr)` closes the
    **uncontrolled-timing** gap that the rest of the flow left open: `trace_attached`
    needs the target stopped *at* the method entry, but a real managed runtime calls a
    JIT method on its own schedule, so you cannot attach at the right instant. `run_to`
    plants a software breakpoint at `addr` (`PTRACE_POKETEXT` — `int3` on x86-64, `brk`
    on AArch64 — which patches an r-x text page the way a debugger does), `PTRACE_CONT`s
    until the program *itself* next calls in, then removes the breakpoint and rewinds the
    PC, leaving the target stopped exactly at `addr` for `trace_attached` (also hardened
    to record the entry instruction from either stop convention). A live test now drives
    the **complete real-JIT flow with no cooperative go-flag** — a child publishes a
    perf-map and calls its routine in a loop, the tracer resolves it by name, attaches,
    `run_to`s the entry, and traces that invocation to the exact `[0,3,6,c,11]` stream —
    completing the managed-runtime flow: resolve → attach → `run_to` → `trace_attached` →
    detach. **Call-depth awareness** removes the last "leaf only" restriction: `trace_call`
    and `trace_attached` previously treated the first step out of the region as the return,
    so a routine that called a **runtime helper** (GC barrier, allocation, PLT stub — the
    norm for real JIT methods) truncated at the first call. The stepper now decodes the
    region-exit instruction (`asmtest_disas_is_call`, Capstone `CS_GRP_CALL`) and runs a
    **call**-out to its return address at **native speed** (a breakpoint-cont over the
    callee, not a per-instruction step), resuming recording after it; only a genuine
    return ends the trace. The region's own instructions are recorded, the helper skipped,
    the real return still found (`test_ptrace_callout`); without Capstone it falls back to
    the prior leaf-only behaviour. **Real-runtime validation lanes** point the whole
    pipeline at a live JIT (not a fixture) via one argv-driven harness
    (`examples/jit_trace.c`): `make docker-hwtrace-jit` traces **Node.js (V8)** —
    `node --perf-basic-prof --no-turbo-inlining` on a hot function, resolving the method
    from V8's real perf-map, attaching to the live multi-threaded GC'd runtime, `run_to`ing
    the entry, and single-stepping one invocation to recover the **actual TurboFan code**
    for `(a+b)|0`; `make docker-hwtrace-jit-dotnet` traces **.NET (CoreCLR)** —
    `Program::Add` → `lea eax,[rdi+rsi]; ret` (with `DOTNET_TieredCompilation=0` for a
    stable address), tracing .NET's **W^X** code heap **as-shipped** via the hardware-
    breakpoint fallback below; and `make docker-hwtrace-jit-java` traces **OpenJDK
    (HotSpot)** — `Hot.asmtjit` → `lea eax,[rsi+rdx]` inside the real C2 nmethod (entry
    barrier and stack-bang and all), JIT'd once via `-XX:-TieredCompilation` and kept a
    standalone callable body with `-XX:CompileCommand=dontinline`. HotSpot needs two
    wrinkles the others don't, both handled in the harness: it does not stream a perf-map,
    so the lane drives `jcmd <pid> Compiler.perfmap` to materialize one for the live
    process; and the `java` launcher runs Java `main()` on a *secondary* OS thread (not the
    primordial one V8/CoreCLR use), so the harness picks the spinning loop thread by CPU
    delta and `PTRACE_ATTACH`es exactly it — a software-breakpoint trap on an un-traced
    thread is fatal. A watchdog makes a re-tiered/moved address self-skip rather
    than hang, so the lanes never flake (resolve + attach are asserted against the runtime's
    real output; the trace is asserted-or-skipped). **Hardware-breakpoint `run_to`:**
    `run_until` (behind `run_to` and the call-out step-over) defaults to a software `int3`
    but transparently falls back to an **x86-64 hardware execution breakpoint** (DR0/DR7 via
    `PTRACE_POKEUSER`) when `PTRACE_POKETEXT` is refused — i.e. on a W^X JIT code heap whose
    executable page is not writable (.NET's default double-maps it; POKETEXT fails `EIO`).
    The hardware breakpoint writes no code (so it traces W^X as-shipped) and is per-thread
    (so it never traps a sibling runtime thread, unlike a process-wide `int3`); software
    stays the default and `ASMTEST_PTRACE_HW_BP` forces the hardware path (used to validate
    it deterministically on ordinary memory — `test_ptrace_callout`). AArch64 hardware
    breakpoints (`NT_ARM_HW_BKPT`) are a follow-on. A third lane,
    `make docker-hwtrace-jit-jitdump`, validates the **binary jitdump** byte source against
    real output: `node --perf-prof` writes a real `jit-<pid>.dump`, and the lane recovers a
    method's **recorded code bytes** with `asmtest_jitdump_find` and checks them three ways
    — the address agrees with V8's own perf-map, the bytes disassemble to real x86-64, and
    they match the live code at that address (jitdump's temporal-capture guarantee) — the
    first validation of `asmtest_jitdump_find` against a real jitdump rather than a
    synthetic fixture. A **second jitdump producer** lane, `make
    docker-hwtrace-jit-java-jitdump`, validates the reader against a jitdump from a
    *different* runtime **and** encoder: OpenJDK **HotSpot** has no native jitdump, so the
    lane loads the perf project's JVMTI agent (`libperf-jvmti.so`, from `linux-tools`) with
    `-agentpath`, which records every C2 method to a real jitdump. It names methods in JVM
    descriptor form (`LHot;asmtjit(II)I`, unlike V8's symbol) and interleaves
    debug/unwinding records the reader must skip, so it exercises `asmtest_jitdump_find` on
    a genuinely independent encoder; the recovered bytes are checked against the live code
    and against HotSpot's own `jcmd Compiler.perfmap` address (two independent HotSpot
    outputs). A **third jitdump producer** lane, `make docker-hwtrace-jit-dotnet-jitdump`,
    covers **.NET CoreCLR** — which, unlike HotSpot, writes a real `/tmp/jit-<pid>.dump`
    **natively** (no agent) under `DOTNET_PerfMapEnabled=1`, naming the method identically in
    the perf-map and the jitdump. So it shares the same `trace_jitdump` path as V8 (one
    routine parameterized by runtime): recover `Program::Add`'s recorded bytes (`lea
    eax,[rdi+rsi]; ret`) and validate them four ways — disassemble, match the live code, and
    agree with CoreCLR's own perf-map address/size. The binary jitdump is now validated
    against **all three** managed runtimes (V8, HotSpot, CoreCLR). `asmtest_jitdump_find(path,
    pid, name, &entry, bytes,
    cap, &len)` reads the richer **binary jitdump** image (`jit-<pid>.dump` — CoreCLR,
    HotSpot, V8; what `perf inject --jit` consumes), resolving a method to its
    `(code_addr, code_size)` **and its recorded native code bytes** (which the text
    perf-map cannot give). Because each `JIT_CODE_LOAD` record is timestamped, a method
    re-emitted at a reused address (tiered/OSR recompilation) resolves to the **latest**
    body — the temporal same-address-different-bytes problem; endianness is
    auto-detected and non-`LOAD` records skipped. (The text perf-map remains the
    portable lowest common denominator for JITs that only emit symbols.) The full
    foreign-process toolkit — `available`/`skip_reason`, `trace_call`,
    `trace_attached`, `run_to`, `region_by_addr`, `perfmap_symbol`, `jitdump_find` — is exposed
    through **every language wrapper** (a `Ptrace` class / module surfacing the same
    methods, idiomatic per language), each with a per-binding self-test of the
    live-testable subset (out-of-process `trace_call` parity, `/proc/maps` and
    perf-map resolution, and a binary-jitdump round-trip), and `asmtest_ptrace.h` is
    now covered by the binding function-surface parity gate. The `/proc` + jitdump
    **code-region readers** (`asmtest_proc_*`, `asmtest_jitdump_find`) are pure file
    parsing, so they build and run on **any Linux arch** and are **validated live on
    AArch64** (in a `linux/arm64` container, where the perfmap and jitdump tests pass).
    The single-step **trace capture** on AArch64 awaits a real host:
    `asmtest_ptrace_available()` is a cached, hang-proof self-probe (bounded `WNOHANG`
    polling) that returns 0 under qemu-user — which does not emulate the ptrace
    tracer/tracee relationship at all — so the stepper self-skips on emulation just as
    the PT/CoreSight tiers self-skip off their hardware; the AArch64 fixtures are
    decode- and execute-validated under qemu, with only the live single-step stream
    pending Apple-Silicon / Linux-ARM64 / Windows-on-ARM hardware.
  - **Time-aware code-image recorder (`asmtest_codeimage`).** A userspace
    `PERF_RECORD_TEXT_POKE`: it records a **timestamped timeline** of a process's code
    regions so `asmtest_codeimage_bytes_at(img, addr, when, …)` returns the bytes that
    were live at trace-position `when` — the correct answer for a JIT whose code is
    patched, freed, or has its address reused mid-trace, where a single late
    `process_vm_readv` snapshot returns the **wrong** bytes (the temporal problem the
    [JIT-runtime-tracing analysis](https://github.com/wilvk/asm-test/blob/main/docs/analysis/jit-runtime-tracing.md)
    calls "the innovative, buildable core", approach #2). Change detection is **soft-dirty**
    (`/proc/<pid>/clear_refs` to arm + the soft-dirty PTE bit to detect, read via the
    `PAGEMAP_SCAN` ioctl where available, else by parsing `/proc/<pid>/pagemap`), which
    works **cross-process** — the foreign-JIT case — needing only permission to read the
    target. The W2 stepper consumes it: `asmtest_ptrace_trace_attached_versioned(pid,
    base, len, img, when, &result, trace)` decodes a foreign region's blocks against the
    time-correct bytes instead of a live snapshot (the existing `trace_attached` is the
    `img == NULL` case, unchanged). An **optional eBPF emission detector** (a CO-RE
    program on `mprotect`/`mmap`/`memfd_create`, filtered to the target PID namespace via
    `bpf_get_ns_current_pid_tgid`, events drained from a `bpf_ringbuf`) tells the recorder
    *when* code appears so it snapshots on the `PROT_EXEC` edge instead of polling; it is
    built only when `clang`+`libbpf`+`bpftool` are present (`-DASMTEST_HAVE_LIBBPF`) and
    self-skips otherwise, with the userspace soft-dirty path as the always-available
    fallback. Validated live: the same-address-different-bytes temporal proof and the
    versioned W2 trace run in `make hwtrace-test` / `make codeimage-test` on any x86-64
    Linux host (no privilege); the eBPF detector is validated in `make
    docker-hwtrace-codeimage` (a `--cap-add=BPF,PERFMON` container — **not** privileged),
    observing a real `mprotect(PROT_EXEC)` emission edge. Exposed across **all ten**
    language bindings (a `CodeImage` wrapper) and covered by the binding
    function-surface parity gate. ([asmtest_codeimage.h](https://github.com/wilvk/asm-test/blob/main/include/asmtest_codeimage.h),
    `src/codeimage.c`, `bpf/codeimage.bpf.c`)
  - **ARM CoreSight reconstruction core (host-validated).** `src/cs_backend.c` is now
    split like the AMD backend: its decoder-independent **reconstruction core**
    `asmtest_cs_reconstruct(arch, ranges, n, base, len, trace)` turns the ordered
    instruction *ranges* an ETM/ETE decoder emits (`OCSD_GEN_TRC_ELEM_INSTR_RANGE`)
    into the same instruction-offset stream and single-entry/ends-at-branch block
    partition the Intel PT backend produces. It is **host-validated without a
    CoreSight board** (`examples/test_hwtrace.c` `test_cs_reconstruction`, the
    analogue of the AMD synthetic-branch-stack test), asserting byte-for-byte parity
    with the PT/AMD/single-step backends over the shared fixture. The remaining half
    — the live OpenCSD decode tree (`ocsd_create_dcd_tree` + ETMv4/ETE decoder +
    memory accessor feeding ranges to the core) — needs libopencsd *and* a real
    AArch64 CoreSight board to write and validate, so per the project's no-untested-
    hardware-code rule it is not yet implemented; `asmtest_cs_decoder_present()` still
    returns 0 and the tier self-skips on every host, but the half the board glue will
    feed is now proven (CoreSight advances from bare scaffold to validated
    reconstruction).
  - **AMD LBR Tier-B stitching (host-validated).** AMD's branch stack is 16 deep, so
    a single-snapshot (Tier-A) reconstruction sets `truncated` past 16 taken
    branches. Tier-B lifts that ceiling: `asmtest_amd_stitch(samples, nrs, n, out,
    cap, &gap)` splices the overlapping windows `sample_period=1` emits (one per
    taken branch, consecutive windows overlapping by 15 edges) into one gapless
    taken-branch sequence — for each window it takes the smallest shift that still
    overlaps the accumulated tail and appends only the new edges, so a loop's
    repeated identical edges stitch correctly; lost overlap (≥ a full window dropped
    to throttling) sets `*gap`. `asmtest_amd_decode_stitched(...)` replays the
    stitched sequence through the shared `amd_replay` loop (factored out of
    `asmtest_amd_decode`) **without** the 16-entry overflow flag. Host-validated
    without hardware (like the Tier-A reconstruction): `test_amd_stitch` synthesizes
    an 18-iteration loop's windows, stitches them, and reconstructs the **complete**
    trace (55 instructions, two blocks, not truncated) where a single Tier-A 16-window
    truncates — plus gap detection. **Now wired into the live capture and Zen 5-validated:**
    `hwtrace_end_amd` collects every branch-stack sample in the perf data ring (time
    order) and, when the richest single window overflowed (`best_nr >= 16`), stitches
    them and decodes past the ceiling; the small-routine path (`best_nr < 16`) is
    unchanged. Completeness is gated on the precise loss signals — a stitch gap OR a
    `PERF_RECORD_LOST`/`PERF_RECORD_THROTTLE` record (the non-overwrite ring drops the
    *newest* samples on overflow and emits `LOST`, the signal the gaplessly-stitching
    survivors cannot otherwise reveal) → honestly `truncated`. On a Zen 5 (Ryzen 9 9950X,
    `make docker-hwtrace-amd`) a 20000-trip loop reconstructs ~290 instructions (≈95
    stitched branches, far past one 16-deep window's ~49) and stays truncated, as the
    perf ring size and `sample_period=1` throttling require; the live path is complete
    only for runs that fit the ring and survive throttling, beyond which DynamoRIO (no
    ceiling) remains the answer. (AMD LBR plan, Phase 5.)

- **Win64 wide-vector (AVX2 256-bit) capture.** The Win64 capture trampoline
  topped out at 128-bit (`xmm`); a routine's full `ymm` result under the Microsoft
  x64 ABI couldn't be inspected past its low 128 bits. New
  `asm_call_capture_vec256_win64` is the Win64 analog of the SysV
  `asm_call_capture_vec256`: it marshals four 256-bit args into `ymm0..3`, calls
  the routine, and captures the whole `ymm0..15` file into a `vec256_t[16]`,
  saving/restoring the callee-saved low 128 of `xmm6..15` (the upper 128 is
  volatile per the ABI) and `vzeroupper`-ing on exit. A `win64_vaddpd_ymm` routine
  + a `test_capture_win64` case assert the full 256-bit return (all four doubles,
  exercising the upper-128 lanes the 128-bit path can't see), self-skipping
  off-AVX2 via a local CPUID/XCR0 probe. Verified on **both** Win64 lanes — the
  native `ms_abi` lane and the PE/Wine lane (Wine runs PE instructions on the host
  CPU, so it is real AVX2). Track D of the
  [post-v1.0 expansion plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/post-v1-expansion-plan.md)
  (the "Win64 wide path" follow-on); AArch64 SVE remains staged, hardware-gated on a
  runner that can execute it.

- **AVX-512 512-bit (`zmm`) capture — across the core, Win64, and all ten bindings.**
  The wide-vector path now reaches 512 bits, validated on real AVX-512 silicon (a Zen 5
  / Ryzen 9 9950X). A new `vec512_t` (64 bytes) and `asm_call_capture_vec512` marshal
  eight `zmm` args into `zmm0..7` and capture the **full `zmm0..31` file** — AVX-512
  doubles the register *count* as well as the width, so the capture is a `vec512_t[32]`
  (vs `vec256_t[16]` for AVX2) — using the EVEX-encoded `vmovdqu64` (required to reach
  `zmm16..31`). It is gated on a real `asmtest_cpu_has_avx512f()` (CPUID + `XCR0` `0xe6`:
  opmask + `ZMM_Hi256` + `Hi16_ZMM`); the `ASM_VCALL512*` macros and every binding
  wrapper self-skip where AVX-512 is absent, so the same suite runs everywhere. Shipped
  in **both** the GAS and NASM trampolines (`src/capture.{s,asm}`) and the **Win64** path
  (`asm_call_capture_vec512_win64`, Microsoft x64 ABI, low-128 `xmm6..15` saved/restored),
  with `ASSERT_VEC512_EQ` + `asmtest_assert_vec512_eq` for the 64-byte lane compare. A
  `vec_add8d` corpus routine (`vaddpd zmm`, 8 packed doubles) and `win64_vaddpd_zmm`
  assert the full 512-bit return — the 8th double lane proves the bits neither the 128-
  nor 256-bit path can see. Exposed across **all ten** language bindings (Python, C++,
  Rust, Go, Node, Java, .NET, Ruby, Lua, Zig) as `capture_vec512`/`cpu_has_avx512f`
  analogs with per-binding parity tests. Closes the AVX-512 half of gap #4 in the
  [post-v1.0 expansion plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/post-v1-expansion-plan.md)
  (its "no AVX-512 silicon" caveat is now lifted on this host); AArch64 SVE remains the
  staged remainder.

- **Publishable packages (Track A): library-exposing artifacts, self-locating
  native libs, and a dry-run release workflow.** The packaging scaffolding now
  produces artifacts a registry could ship. Each `make <lang>-package` exposes the
  reusable **library module** rather than the conformance test runner (the Ruby
  gem ships `asmtest.rb`, npm `asmtest.js`, the rock `asmtest.lua`, the JAR the
  `Asmtest` classes, and the NuGet package `AsmTest.dll` via a new SDK-style
  `asmtest-lib.csproj` / `dotnet pack`), and bundles one native slot per platform
  present in `build/dist/native/` (the host slot locally; all four when a release
  has the CI `native-all`). The **dlopen bindings self-locate their bundled native
  lib** when `ASMTEST_LIB` is unset — Node/Ruby/Lua/Java fall back to
  `native/<os>-<arch>/` next to the module (Java extracts the jar resource to a
  temp file; .NET resolves via the `runtimes/<rid>/native/` RID layout; Python
  already used `_libs/`) — so an installed package works out of the box. A new
  [`release.yml`](https://github.com/wilvk/asm-test/blob/main/.github/workflows/release.yml)
  builds the cross-platform `native-all`, then per binding **packages → installs
  the artifact fresh → smoke-tests the bundled-native load (`ASMTEST_LIB` unset) →
  dry-run publishes** (`twine check`, `npm publish --dry-run`, `cargo publish
  --dry-run`); the live push is gated behind per-ecosystem token secrets, so it
  runs end to end with no credentials. Python wheels are built **per platform**: a
  `setup.py` tags the wheel `py3-none-<platform>` (platlib, since it bundles a
  native lib), and the workflow **repairs** each into a self-contained
  manylinux / macOS wheel — `auditwheel` / `delocate` vendoring libunicorn — so
  `pip install` pulls no system libs. Every package + fresh-install + bundled-load
  smoke was verified in the per-language Docker images (the manylinux wheel checked
  to load with system libunicorn removed). Track A of the
  [post-v1.0 expansion plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/post-v1-expansion-plan.md);
  see [docs/packaging.md](https://github.com/wilvk/asm-test/blob/main/docs/packaging.md).

- **Binding parity, round 2: the new emulator/capture capabilities reach all ten
  bindings.** The Track F mid-execution guards, Track E coverage-guided fuzzing /
  mutation testing, and Track D AVX2 256-bit capture were C-core-only; they now
  have a binding ABI and a wrapper in every language. The C side adds
  opaque-handle FFI in [`src/ffi.c`](https://github.com/wilvk/asm-test/blob/main/src/ffi.c)
  (`emu_watch_t` / `emu_reg_guard_t` and the fuzz/mutation stat structs — alloc +
  by-field accessors; the arming/driver functions take plain pointers, so a
  binding calls them directly), and `fuzz.o` now ships in the emulator shared
  lib so `emu_fuzz_cover1` / `emu_mutation_test1` are reachable. Each binding
  gained the ergonomic surface in its own idiom — **Python** (`ctypes`),
  **C++** (header structs), **Ruby** (`Fiddle`), **Lua** (LuaJIT `ffi`),
  **Node** (`koffi`), **Go** (`cgo`), **Rust** (`#[repr(C)]` + `extern`),
  **Zig** (`@cImport`), **Java** (FFM/Panama), **.NET** (P/Invoke) — e.g.
  `Emulator.watch_writes` / `guard_reg` / `fuzz_cover` / `mutation_test` and a
  `capture_vec256` + `cpu_has_avx2` gate, with the vector path self-skipping
  where AVX2 is absent. Done by hand (a binding-FFI codegen PoC was evaluated and
  reverted — it only covers the mechanical ~20%); each binding's conformance
  runner gained native checks over the same byte-literal routines, verified on
  the host (Python/C++/Ruby) or the Docker matrix (the other seven).

- **Track C disassembly reaches all ten bindings (via a single "full" lib).**
  Capstone disassembly was C-core-only — binding it naïvely would pull Capstone
  into every binding lib, or spawn a combinatorial lib matrix. Instead one
  `libasmtest_emu_full` carries *both* optional native tiers (Keystone assembler
  **and** Capstone disassembler) on top of the emulator (`make shared-emu-full`);
  the lean `libasmtest_emu` stays Unicorn-only, and a binding points `ASMTEST_LIB`
  at the full lib when it wants disassembly (and gets the assembler for free).
  Every binding gained `disas` / `disas_available` in its own idiom — **Python**
  (`ctypes`), **C++** (header, gated `ASMTEST_ENABLE_DISAS`), **Ruby** (`Fiddle`),
  **Lua** (LuaJIT `ffi`), **Node** (`koffi`), **Go** (`cgo` dlsym), **Rust**
  (`dlsym` + `extern`), **Zig** (`@cImport`, free), **Java** (FFM/Panama),
  **.NET** (P/Invoke) — wrapping `emu_disas` (decode one instruction at an offset
  to `"mnemonic operands"`) with a probe that self-skips against the lean lib.
  The per-binding `*-asm-test` checks now drive `libasmtest_emu_full`, so a single
  run exercises CallAsm **and** disas; the `bindings-asm` base image and
  `install-deps --asm` gained Capstone. Each binding's conformance decodes known
  x86-64 bytes (`xor rax, rax` / `ret` / `nop`), verified on the host
  (Python/C++/Ruby) and the Docker matrix (the other seven). Track C of the
  [post-v1.0 expansion plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/post-v1-expansion-plan.md).

- **Win64 runner parity — per-test isolation, `-jN`, and benchmarks.** The Win64
  tier ran every test in one process (`--no-fork`); the POSIX runner's fork-based
  per-test isolation, `-jN` pool, and benchmark mode were gated off. All three
  now work on Win64. With no `fork()`, the runner **re-execs itself per test** —
  a hidden `--asmtest-child=<index>` runs exactly one test and writes its result
  to a temp file the parent reads back — driven through the existing
  `asmtest_win32_run` / `_run_pool` primitives (`CreateProcess` +
  `WaitForSingleObject` / `WaitForMultipleObjects`). Isolation is now the
  **default** (matching POSIX); `--no-fork` selects the in-process facility. A
  crash is contained in the child (caught there, or backstopped by the child's
  death); a hang is killed by the parent's deadline. `--bench` (rdtsc cycles per
  call) runs on Win64 too (a `BENCH` body is trusted, so it runs unguarded).
  `tests/win64/suite_win64.c` gained a `BENCH`, and `make win64-runner-test`
  exercises **all four modes** (isolation, `-jN`, `--no-fork`, `--bench`) under
  Wine; an optional `windows-latest` CI job signs the same suite off on a genuine
  Windows host with no Wine. Track B of the
  [post-v1.0 expansion plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/post-v1-expansion-plan.md).
  See [docs/win64.md](https://github.com/wilvk/asm-test/blob/main/docs/win64.md#the-runner-port).

- **Wide-vector capture — AVX2 256-bit (`ymm`).** Vector capture was strictly
  128-bit (`vec128_t` / `ASM_VCALLn`); a routine's `ymm` result couldn't be
  inspected past its low 128 bits. New `vec256_t` (the 256-bit analog union) and
  `asm_call_capture_vec256` marshal `ymm0..7` args and capture the whole `ymm`
  file into a `vec256_t[16]` (`out[0]` = return), with `ASM_VCALL256n` /
  `ASSERT_VEC256_EQ` and the existing `ASSERT_DEQ`/`FEQ` over the doubled lane
  counts. A runtime **CPUID probe** (`asmtest_cpu_has_avx2` /
  `asmtest_cpu_has_avx512f`, checking both the feature bit and OS `XCR0`
  enablement) makes the path **self-skip** (`SKIP`) on a host without the
  feature instead of executing an unsupported instruction. The trampoline is in
  both backends ([`src/capture.s`](https://github.com/wilvk/asm-test/blob/main/src/capture.s)
  GAS + `src/capture.asm` NASM, `vzeroupper` on exit), `vec256_t` is pinned in
  the manifest and by a `_Static_assert`, and an `vec_add4d` AVX2 example +
  `test_simd` case assert the full 256-bit result (including the upper-128 lane)
  on both backends. Track D of the
  [post-v1.0 expansion plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/post-v1-expansion-plan.md);
  AVX-512 (`zmm`), AArch64 SVE, a Win64 wide path, and binding parity are staged
  follow-ons — and the emulator wide path self-skips because its bundled Unicorn
  exposes YMM/ZMM but does not execute AVX (`UC_ERR_INSN_INVALID`). See
  [docs/floating-point-simd.md](https://github.com/wilvk/asm-test/blob/main/docs/floating-point-simd.md#wide-vectors--avx2-256-bit).

- **Coverage-guided fuzzing & mutation testing in the emulator.** The emulator
  already recorded basic-block coverage but only ever fed a report; it now feeds
  *input generation*, and a mutation tester proves an input set actually catches
  a perturbed routine. Both run a one-int-arg routine **inside** the emulator
  (the instruction cap + fault hooks contain a pathological input or a broken
  mutant), are seedable for reproducibility, and live in a new dependency-free
  [`src/fuzz.c`](https://github.com/wilvk/asm-test/blob/main/src/fuzz.c):
  - `emu_fuzz_cover1` — **coverage-guided generation**: keeps inputs that grow
    the block-coverage union, drawing candidates fresh or by mutating a corpus
    member (the feedback), so it reaches blocks fixed vectors miss (for the
    `classify` example, 5 blocks vs a positive vector's 3).
  - `emu_mutation_test1` — **mutation testing**: flips bits of the routine, runs
    each mutant and the original on an input set, and counts mutants the set
    fails to distinguish (survivors = test-gap). A weak suite over `classify`
    leaves 100 of 192 mutants alive; a path-covering suite leaves only the ~16
    equivalent mutants — a stronger input set demonstrably kills more.
  Reuses the framework's seedable splitmix64 RNG (`asmtest.h`) and the
  emulator's coverage trace; no new dependency. Track E of the
  [post-v1.0 expansion plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/post-v1-expansion-plan.md).
  See [docs/emulator.md](https://github.com/wilvk/asm-test/blob/main/docs/emulator.md#coverage-guided-fuzzing--mutation-testing).

- **Mid-execution guards in the emulator (watchpoints + register invariants).**
  Assert properties *while* a routine runs, not just on its result — introspection
  no ABI-boundary tool can do. Armed on the emu handle and persisting across
  `emu_call_*` until cleared, recording the first violation as data (no host
  crash), x86-64 guest. **Memory-write watchpoints** (`emu_watch_writes` with
  `EMU_WATCH_ONLY` / `EMU_WATCH_NEVER`, `ASSERT_NO_WRITE_VIOLATION` /
  `ASSERT_WRITE_VIOLATION`) catch a *logical* scribble into mapped memory that
  does **not** fault — where a guard page sees nothing — and name the offending
  store (`emu_watch_describe` reuses the Track C disassembler:
  `write to 0x400800 (8 bytes): mov qword ptr [rdi + 0x800], rax  (@0x3)`).
  **Register invariants** (`emu_guard_reg`, `ASSERT_REG_INVARIANT`) assert a
  register holds a value at every basic-block entry — a callee-saved / stack-pointer
  guard that catches mid-routine corruption **even when the value is restored by
  return** (which ABI capture cannot see). Step-bounded assertions need no new
  API: run with `max_insns=N` and inspect `out->regs`. New hooks
  (`UC_HOOK_MEM_WRITE`, a second `UC_HOOK_BLOCK`) in
  [`src/emu.c`](https://github.com/wilvk/asm-test/blob/main/src/emu.c); types,
  arming functions, and assertions in
  [`include/asmtest_emu.h`](https://github.com/wilvk/asm-test/blob/main/include/asmtest_emu.h).
  Track F of the
  [post-v1.0 expansion plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/post-v1-expansion-plan.md).
  See [docs/emulator.md](https://github.com/wilvk/asm-test/blob/main/docs/emulator.md#mid-execution-guards).

- **Disassembly in emulator diagnostics (Capstone).** The emulator records
  faults, traces, and coverage as raw byte offsets — `@0x2f`, never the
  instruction. With [Capstone](https://www.capstone-engine.org/) linked (the
  disassembler counterpart to the Keystone in-line assembler) those offsets now
  carry the instruction at them, across all four guests (x86-64, AArch64,
  RISC-V, ARM32). New helpers in
  [`include/asmtest_emu.h`](https://github.com/wilvk/asm-test/blob/main/include/asmtest_emu.h)
  / `src/disasm.c`: `emu_disas` (one instruction at an offset, with PC-relative
  targets resolved to absolute), `emu_fault_describe` (a fault line that names
  the offending instruction — `read fault accessing 0xdead0000: mov rax, qword
  ptr [rdi]  (@0x0)`), and disassembling counterparts to the reporters —
  `emu_trace_disasm`, `emu_trace_report_disasm`, `emu_coverage_uncovered_disasm`
  (turning `uncovered: 0x2f` into `uncovered: 0x2f  cmp rax, 0`). Optional and
  **auto-detected** (`pkg-config --exists capstone`): every helper degrades to
  bare offsets when Capstone is absent (`emu_disas_available()` reports which),
  so the same call works either way and the core library / shared libs / binding
  images stay Capstone-free — only `build/test_emu` links it, exactly as the
  assembler tier keeps Keystone in its own object. `make deps DEPS_ARGS=--emu`
  now installs `libcapstone-dev`, so the CI `emu` job exercises the annotated
  diagnostics on every matrix OS. RISC-V disassembly needs Capstone ≥ 5 and
  self-skips on older builds. This is Track C of the
  [post-v1.0 expansion plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/post-v1-expansion-plan.md).
  See [docs/emulator.md](https://github.com/wilvk/asm-test/blob/main/docs/emulator.md#disassembly-in-diagnostics-capstone).

- **CI builds the cross-platform native payloads for the bindings.** `make
  package-libs` only ever staged the *build host's* shared libs, so a release
  shipped a single-platform payload. A new `payloads` CI matrix runs the native
  staging on each `{x86-64, AArch64} × {Linux, macOS}` runner (the Intel-macOS
  corner nightly, as `test-macos-x86` does) and uploads each
  `build/dist/native/<os>-<arch>/` as an artifact; a `payloads (collect + verify)`
  job merges them into one tree, runs the new `make package-libs-verify` to assert
  every platform slot carries both the core and the `libasmtest_emu` lib, and
  re-uploads the combined set as a single `native-all` artifact a publish step
  would consume. This is the "multi-platform native payloads" step the packaging
  scaffolding stopped short of — no registry credentials or extra hardware needed.
  See [docs/packaging.md](https://github.com/wilvk/asm-test/blob/main/docs/packaging.md).

- **Static Mach-O verification of the macOS payloads, on Linux.** `make
  package-libs-verify-macho` ([scripts/verify-macho.sh](https://github.com/wilvk/asm-test/blob/main/scripts/verify-macho.sh),
  folded into `package-libs-verify`) catches the most common macOS packaging regressions —
  a wrong/missing arch slice or a leaked absolute install-name/dependency — at build time on
  the Linux release collector, **with no Mac needed**, via `llvm-otool` / `llvm-lipo`. For
  every `build/dist/native/darwin-*/` `.dylib` it asserts: the slot's arch is present
  (`llvm-lipo -archs`), the install-name (`LC_ID_DYLIB`) is `@rpath`/`@loader_path`-relative
  and neither it nor any dependency bakes in `/Users`, `/opt/homebrew`, or `/usr/local`
  (a dev-build or Homebrew leak; system `/usr/lib` and `/System` are fine), and a min-OS load
  command is present (and `<= MACOS_MIN_FLOOR` when that var is set). It self-skips where the
  llvm tools are absent (a dev host), so `package-libs-verify` stays green everywhere; the
  `package-libs-collect` CI job installs `llvm` so it runs there for real. This is Track B of
  the [macOS clean-test plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/macos-clean-test-plan.md)
  — the independent cross-check that `scripts/package-native.sh`'s macOS-side install-name
  rewrites actually produced correct Mach-O.

- **The full emulator surface reaches every binding.** A review found four core
  emulator capabilities that no binding could reach (the FFI lacked an
  opaque-handle wrapper), plus an assembler tier the corpus did not anchor. All
  are now exposed across all **ten** bindings, driven by a widened binding ABI in
  [`src/ffi.c`](https://github.com/wilvk/asm-test/blob/main/src/ffi.c) / `src/emu.c`:
  - **Cross-arch emulator guests** — run raw AArch64 / RISC-V / ARM32 machine-code
    bytes on any host (`Guest`/`GuestEmulator` + per-arch register reads through
    `asmtest_emu_{arm64,riscv,arm}_reg`), not just the x86-64 guest.
  - **Emulator FP / vector args and >2 integer args** — `call_fp` / `call_vec` /
    `call_bytes` over raw bytes, beyond the old two-integer `call2`.
  - **Execution trace / basic-block coverage** — an opaque `Trace` handle
    (`asmtest_emu_trace_*`) recorded by `call_traced`, with `covered(off)`.
  - **Win64 calling convention** — `call_win64`, to test a Win64 routine on a
    System V host.
  Anchored in the shared conformance corpus: new `emu_bytes` / `emu_trace` cases
  (cross-arch int, x86 wide/FP/vector, Win64, two-block coverage) run on every host
  via checked-in pre-assembled byte literals, and the assembler tier is now emitted
  into `corpus.json` and executed by a new `make conformance-asm` build. The C++
  binding also gains the previously missing `sum_via_rbx` / `clear_carry` cases.
  See the
  [binding-parity plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/binding-parity-plan.md).

- **In-line assembler tier (Keystone).** Pass a routine as an *assembly string*
  and run it, instead of only as pre-assembled object code. `asmtest_assemble()`
  (in the new `include/asmtest_assemble.h`) turns text into machine code for the
  emulator's guest set — x86-64 (Intel or AT&T syntax), AArch64, ARM32, and
  RISC-V where the linked Keystone supports it — with errors reported as data and
  output the caller frees via `asmtest_asm_free()`. Bridge wrappers `emu_call_asm`
  / `emu_arm64_call_asm` / `emu_riscv_call_asm` / `emu_arm_call_asm` assemble at
  the emulator's load base (so PC-relative and branch targets resolve) and run
  through the matching `emu_*_call` in one call. Optional and pkg-config gated
  like the emulator tier, and kept **separate from `libasmtest_emu`** so the
  Unicorn-only binding images are unaffected: `make asm-test` (links `libkeystone`
  + `libunicorn`), `make docker-asm`, and a CI `asm` job on both x86-64 and arm64.
  Keystone has no Linux distro package, so `make deps DEPS_ARGS=--asm` points at
  `scripts/build-keystone.sh` (a pinned source build the CI job and Docker image
  use). RISC-V in-line assembly self-skips until a Keystone release ships a
  RISC-V backend (none does yet). See the
  [implementation plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/inline-asm-keystone-plan.md).

- **In-line assembler reaches every binding, with a widened shim.** All **ten**
  bindings now expose the assembler — the original five (.NET, Ruby, Lua, Node,
  Java) plus Python, Go, Rust, C++, and Zig — bound *optionally* so they self-skip
  against the Keystone-free `libasmtest_emu` and pay no cost in the normal binding
  images. The dlopen bindings probe the symbol; Go and Rust resolve it through the
  libc dynamic loader (they statically link the plain lib); C++ and Zig link the
  assembler lib directly. The opaque-handle shim is widened from the original
  Intel-only, two-integer-arg, error-blind `asmtest_emu_call_asm`: the new
  `asmtest_emu_call_asm6` takes **Intel *or* AT&T syntax**, **up to six** integer
  args, and an **instruction cap** (`max_insns`); `asmtest_asm_last_error()`
  surfaces the **Keystone diagnostic** so a failed assemble reports *why* instead
  of a bare false; and `asmtest_asm_bytes()` exposes **multi-arch text→bytes**
  (x86-64/AArch64/RISC-V/ARM32) so a binding can assemble guests its x86-only
  emulator handle can't run. Each binding presents this as a `callAsm`/`assemble`
  pair with a **uniform failure contract** — an assemble error raises/returns the
  diagnostic (it is never a silent miss). The `bindings-asm` CI matrix grows from
  five to **all ten** (`make <lang>-asm-test`), each case now also covering the
  failure path and a multi-arch assemble; the C `make asm-test` suite adds
  `asmtest_emu_call_asm6` / `asmtest_asm_last_error` / `asmtest_asm_bytes`
  coverage. The original `asmtest_emu_call_asm` stays as a thin compatibility
  wrapper.

- **Native Win64 tier (capture).** A Microsoft x64 (“Win64”) capture trampoline
  (`src/capture_win64.asm`) mirrors all eight System V `asm_call_capture*`
  variants on real x86-64 silicon — integer/FP/vector args, the 32-byte shadow
  space, struct return and by-reference struct args, and ABI-preservation over the
  *larger* Win64 callee-saved set (`rdi`/`rsi` plus the callee-saved `xmm6–15`).
  The captured state has a first-class `regs_t` layout in `include/asmtest.h`
  (selected by `-DASMTEST_ABI_WIN64`, LLP64-correct, with `_Static_assert` offset
  pins) and a machine-readable manifest (`make manifest-win64` →
  `asmtest_abi_win64.json`). It runs **with no Windows host**, two ways: the
  native lane via GCC/Clang `__attribute__((ms_abi))` (`make win64-msabi-test`),
  and a real Windows PE built with `nasm -f win64` + MinGW-w64 and run under Wine
  in an isolated image (`Dockerfile.win64`, `make docker-win64`). A new CI `win64`
  job runs both on every push; the capture suite doubles as the native Win64
  conformance check. This is the capture tier (suite runs `--no-fork`); the Win32
  runner port is now underway (see below). See
  [docs/win64.md](https://github.com/wilvk/asm-test/blob/main/docs/win64.md)
  and the [implementation plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/win64-native-tier-plan.md).

- **Native Win64 tier — runner port.** The framework's process-level
  guarantees now have Win32 equivalents for the Win64 tier, each in
  `src/platform_win32.c` (plus the platform-neutral `src/glob_match.c`), compiled
  only for the Win64 target and **verified under Wine**: per-test isolation +
  timeout via `CreateProcess` / `WaitForSingleObject` / `TerminateProcess`
  (`asmtest_win32_run`, classifying OK / CRASH-as-NTSTATUS / TIMEOUT), the `-jN`
  parallel pool via `WaitForMultipleObjects` (`asmtest_win32_run_pool`), the
  guard-page allocator via `VirtualAlloc` + `VirtualProtect(PAGE_NOACCESS)`,
  in-process crash-to-failure via a vectored exception handler +
  `__builtin_longjmp` (`asmtest_win32_guard`, no SEH unwinding), and a portable
  `--filter` glob matcher (`*`, `?`, `[...]` classes, `\` escaping) replacing
  MinGW's missing `fnmatch`. New `make win64-{guard,isolate,pool,filter,seh}-test`
  targets exercise each under Wine and join `make win64-check` / the CI `win64`
  job. A thin platform seam (`src/platform.h`, `ASMTEST_FNMATCH`) wires the
  `--filter` and guard-page paths into `src/asmtest.c` with no POSIX regression.
  The runner itself is then built for Win64: a Win32 `run_one` (the per-test
  facility's vectored handler + watchdog, mapping the recovery reason to
  fail/skip/crash/timeout), `main()` running `--no-fork` with the fork/pipe/poll
  isolation, parallel pool, signal handlers, and SysV-trampoline helpers gated to
  POSIX. `make win64-runner-test` builds `src/asmtest.c` with MinGW and runs a real
  `TEST()` suite (`tests/win64/suite_win64.c`) under Wine: the runner discovers and
  runs the suite, asserts real Win64 captures, and contains a crashing and a
  hanging test as reported failures while surviving. Still POSIX-only: a
  forked/`-jN` mode on Win64 and benchmarks. See
  [docs/win64.md](https://github.com/wilvk/asm-test/blob/main/docs/win64.md).

- **Packaging scaffolding for all ten bindings.** Each binding now has a
  publish-ready registry manifest and a `make <lang>-package` target that
  assembles a distributable bundling the host's prebuilt native libs:
  `asmtest.gemspec` (RubyGems), `asmtest-1.0.0-1.rockspec` (LuaRocks), `pom.xml`
  (Maven), `asmtest.nuspec` (NuGet), `CMakeLists.txt` (a `find_package`-able
  C++ INTERFACE target), `build.zig.zon` (Zig package), plus upgraded
  `pyproject.toml` (wheel `package-data` over a bundled `asmtest/_libs/`),
  `Cargo.toml` (crates.io metadata), and `package.json` (npm `files`). `make
  package-libs` stages the shared libs into `build/dist/native/<plat>/`; the
  dlopen bindings (Python/Ruby/Lua/Node/Java/.NET) bundle `libasmtest_emu`, while
  the link bindings (Rust/Zig/C++/Go) ship as source. A new
  [docs/packaging.md](https://github.com/wilvk/asm-test/blob/main/docs/packaging.md) is the release guide (native-lib split,
  version pinning, per-language commands, the multi-platform caveat). Scaffolding
  only — no registry credentials or cross-OS build matrices.

- **Go binding (Track G).** A `cgo` wrapper in `bindings/go/` over the
  opaque-handle FFI layer — no struct layout mirrored: it declares the
  binding-ABI entry points (`asmtest_corpus_routine`, `asmtest_capture6`/`_fp2` +
  `asmtest_regs_*`, `asmtest_check_abi`, `asmtest_emu_call2` + accessors) and
  links the prebuilt shared libs. Exposes `Regs` (capture / ABI / flags / FP),
  `Emu` + `EmuResult` (faults as data), and Tier-2 `Assert*` helpers over a small
  `TB` interface that `*testing.T` satisfies (so the helpers are themselves
  testable — the suite proves each one bites). `make go-test` runs `go test`;
  [`conformance_test.go`](https://github.com/wilvk/asm-test/blob/main/bindings/go/conformance_test.go) replays the corpus,
  built + run in its own `asmtest-go` image (`make docker-go`) and the `bindings`
  CI matrix. This closes the last language track — **all ten bindings** (Python,
  Rust, C++, Zig, Node, Java, .NET, Ruby, Lua, Go) now ship Tier 1 + Tier 2.

- **Tier-2 idiomatic assertions (all ten bindings).** Optional assertion layers
  over the Tier-1 result objects, with legible failure messages, idiomatic to
  each language: Python (`asmtest.assertions`, raising `AssertionError`), Rust
  (methods on `Regs`/`EmuResult`, panicking), C++ (`asmtest::assert_*` throwing
  `assertion_error`, for GoogleTest/Catch2), Zig (error-union helpers over
  `std.testing`), Node/Ruby/Lua/Java/.NET (throwing/raising `assert_*` helpers in
  the conformance runner), and Go (`Assert*` helpers failing a `*testing.T`). Each
  covers both the pass paths and the failure paths
  (the assertion fails when it should — pytest `raises`, Rust `should_panic`, Zig
  `expectError`, a recording `TB` stub in Go, try/catch elsewhere). `assert_ret`,
  `assert_abi_preserved`,
  `assert_flag`, `assert_fp`, `assert_no_fault`, `assert_reg`, ….

- **Node, Java, .NET, Ruby & Lua bindings (Tracks N/J/D/C).** Five more language
  wrappers, all over a new opaque-handle FFI layer (`src/ffi.c` + emu helpers in
  `emu.c`): `asmtest_regs_new` + `asmtest_capture6` / `_fp2` + `asmtest_regs_*`
  accessors for the capture tier, `asmtest_emu_call2` + `asmtest_emu_*` accessors
  for the emulator, and `asmtest_corpus_routine(name)` for routine addresses — so
  a dynamic binding needs no C struct layout. Bindings: Node (`koffi`), Java
  (FFM/Panama), .NET (P/Invoke), Ruby (stdlib `Fiddle`), Lua (LuaJIT `ffi`); each
  replays the conformance corpus (`make node-test` / `java-test` / `dotnet-test`
  / `ruby-test` / `lua-test`).
- **Isolated per-language Docker images.** Each wrapper is built and tested in
  its **own** image (`bindings/<lang>/Dockerfile` on a shared
  `Dockerfile.bindings-base`), so toolchains never mix. `make docker-<lang>`
  builds + runs one language; `make docker-bindings` does all ten. The CI
  `bindings` job is now a per-language matrix running `make docker-<lang>`.

- **Zig binding (Track Z).** The lowest-ceremony wrapper: `bindings/zig/`
  consumes the C headers directly via `@cImport` — no separate binding layer —
  and replays the conformance corpus (`make zig-test` → `zig build test`,
  `build.zig` targets Zig 0.13.x). Added to the Docker bindings image and the
  `bindings` CI job.

- **Rust binding (Track R).** A no-crates-io crate in `bindings/rust/`:
  `#[repr(C)]` mirrors of `regs_t` and the emulator structs (arch-selected via
  `cfg`) plus `extern "C"` declarations of the binding-ABI entry points, linked
  against the prebuilt shared libs by `build.rs`. Exposes `capture` /
  `capture_fp` / `capture_vec` → `Regs`, `abi_preserved` (native verdict shim),
  and an `Emulator` whose `EmuResult` carries faults as data. `make rust-test`
  runs `cargo test`; `tests/conformance.rs` replays the conformance corpus.
- **C++ binding (Track X).** The C headers now carry `extern "C"` guards (and a
  portable `ASMTEST_STATIC_ASSERT`), so a C++ TU both compiles and links against
  the framework. `bindings/cpp/asmtest.hpp` adds an RAII `Emu`, initializer-list
  `capture*`, vector-lane helpers, and `abi_preserved` / `flag_set` predicates;
  `make cpp-test` runs an example suite that drives the framework from C++. New
  `ASMTEST_NO_MAIN` knob omits the runtime's `main()` for embedding.
- **Docker per-language wrapper testing.** `Dockerfile.bindings` bundles the
  Python, C++, and Rust toolchains plus libunicorn; `make docker-bindings`
  (and `docker-python` / `docker-cpp` / `docker-rust`) build and test every
  wrapper in one reproducible image — verifying a binding on any host, including
  a language not installed locally. A `bindings` CI job runs the same tests
  natively on x86-64 and arm64 Linux.

- **Python binding (Track P).** A pure-ctypes package in `bindings/python/`
  (no `cffi`/compile step) loads the shared library and the `asmtest_abi.json`
  manifest and exposes `capture()` / `capture_fp()` / `capture_vec()` (returning
  a `Regs` snapshot with `ret`, `flags`, `fret`, vector lanes, `abi_preserved`,
  and `flag_set`) plus an `Emulator` context manager whose `EmuResult` surfaces
  faults as data. Struct layout is read from the manifest, so the binding is
  correct for whatever architecture the library was built for. `make
  python-test` builds the shared libs, manifest, corpus, and a routine fixture
  lib, then runs pytest; the suite replays the same `corpus.json` the C
  reference emits and reproduces every case. A new `bindings-python` CI job
  (x86-64 + arm64 Linux) runs it — the reusable per-language CI template
  (bindings plan 0.5), which completes Track 0.

- **Shared libraries + ABI manifest (Track 0).** The first slice of the
  multi-language bindings substrate. `make shared` builds
  `libasmtest.{so,dylib}` (framework runtime + capture trampoline, from `-fPIC`
  objects in a separate `build/pic/` tree) and `make shared-emu` builds
  `libasmtest_emu.{so,dylib}` (adds `emu.o`, links `-lunicorn`), both with
  platform-correct versioned filenames, soname/install-name, and dev symlinks;
  `make install-shared` / `install-shared-emu` install them plus a new
  `asmtest-emu.pc`. `make manifest` emits `asmtest_abi.json` — a machine-readable
  struct layout (sizes, field offsets, host arch, sentinels, flag masks) compiled
  from the real headers via `scripts/gen-manifest.c` — so FFI bindings consume
  offsets instead of hand-transcribing them. `_Static_assert`s in `asmtest.h` /
  `asmtest_emu.h` pin `regs_t` and the emulator register structs to `offsetof`,
  preventing the headers, the trampoline's stores, and the manifest from drifting
  apart. `make install` (static + headers) is unchanged. See
  [docs/plans/multi-language-bindings-plan.md](https://github.com/wilvk/asm-test/blob/main/docs/plans/multi-language-bindings-plan.md).
- **Binding ABI + conformance corpus (Track 0).** Non-jumping verdict shims
  `asmtest_check_abi` / `asmtest_check_flag` *return* a verdict + reason instead
  of `longjmp`-ing into the runner, so an FFI binding can validate a capture with
  no C runner present (the existing `ASSERT_ABI_PRESERVED` / `ASSERT_FLAG_*` now
  delegate to them). `ASMTEST_NO_MAIN` builds the runtime without its `main()`
  for embedding. `make conformance` runs `bindings/conformance/conformance.c` —
  the C reference for a fixed corpus of canonical routines (int / FP / SIMD /
  flags / ABI capture + an x86-64 emulator case), checked against expected
  literals — and emits `corpus.json`, the portable expected-results table every
  language binding must reproduce. The binding-ABI contract symbols are
  designated in the API reference.
- **Parallel execution (Track E).** `-jN` / `--jobs=N` runs up to N tests
  concurrently as forked children (a pool over the existing per-test fork model),
  while output stays in registration order regardless of finish order. Per-test
  timeout and crash containment are unchanged; `--no-fork` forces serial. New
  `expect.sh` self-tests pin the ordering, failure reporting, and crash
  containment under `-j4`.
- **libc-callback example (Track E).** `examples/callback.s` / `.asm` with
  `examples/test_callback.c`: `sum_map(arr, n, fn)` and `count_if(arr, n, pred)`
  call a C function pointer per element, demonstrating an assembly routine
  calling back into C with correct callee-saved/stack-alignment discipline.
- **Valgrind story (Track E).** `make valgrind` runs the example suites under
  memcheck (`--no-fork`) to catch bugs in the routine under test, complementing
  the always-on guard-page allocator; `make docker-valgrind` and the
  `--valgrind` flag of `scripts/install-deps.sh` round it out. Documented
  alongside the guard-page approach in the README.
- **Emulator FP/SIMD (Track C).** The x86-64 emulator guest marshals `double`
  args (`emu_call_fp`) and 128-bit vector args (`emu_call_vec`) into xmm0..7 and
  captures the whole XMM file (`emu_x86_regs_t.xmm[]`). The AArch64 guest gains
  the same (`emu_arm64_call_fp` / `emu_arm64_call_vec`, NEON `v[]`); the RISC-V
  (`emu_riscv_call_fp`, `f[]`) and ARM32 (`emu_arm_call_fp`, `q[]`) guests gain
  scalar FP, with their FP units enabled at open (RISC-V `mstatus.FS`, ARM32
  CPACR + FPEXC). Generic `ASSERT_EMU_VEC128_EQ` works across guests.
- **Emulator assertions (Track C).** `ASSERT_NO_FAULT`, `ASSERT_FAULT`,
  `ASSERT_FAULT_AT`, `ASSERT_EMU_REG_EQ`, `ASSERT_EMU_FP_EQ`, `ASSERT_EMU_VEC_EQ`,
  and coverage `ASSERT_BLOCK_COVERED` / `ASSERT_BLOCKS_AT_LEAST`.
- **Coverage reporting (Track C).** `emu_trace_report`, `emu_coverage_uncovered`
  (lists the blocks a run missed against a universe trace), `emu_trace_lcov`
  (offset-level lcov export), and the `emu_trace_covered` predicate.
- **Emulator vector parity & source-line coverage (Track C, leftovers).**
  `emu_arm_call_vec` marshals 128-bit NEON vectors into ARM32 `q0..q3` and
  captures the whole `q0..q15` file, matching the x86-64/AArch64 vector path.
  Source-line coverage: a caller-supplied `emu_line_map_t` (ascending
  `(offset, line)` rows, produced out-of-band) drives `emu_line_lookup`,
  `emu_trace_source_report`, and `emu_trace_lcov_source`, which report block
  coverage against source lines (hit **and** missed) — no DWARF parsing, no new
  dependency. The RISC-V "V" extension has no counterpart: Unicorn's RISC-V
  guest exposes no vector registers, so it stays scalar-FP (documented in
  `src/emu.c` and `docs/emulator.md`). Closes Track C's open C items.

### Fixed

- **Out-of-process ptrace stepper leaked the tracee when the traced routine
  faulted.** Tracing a routine that takes a real signal (SIGILL/SIGSEGV) — exactly
  what the out-of-process single-step tier exists to trace — hit a break in the
  step loop (`src/ptrace_backend.c`) that returned `ASMTEST_PTRACE_OK` without
  reaping the forked tracee. Because `PTRACE_O_EXITKILL` fires only when the
  *tracer* exits (not when the trace function returns), the child was left stopped
  in signal-delivery and unreaped, so a suite of faulting routines slowly exhausted
  PIDs. The non-SIGTRAP break now `kill(SIGKILL)` + `waitpid`s the tracee like every
  other exit path, still recording the partial trace as `truncated`. New live
  regression `test_ptrace_faulting_no_leak` traces a `ud2` fixture eight times and
  asserts each leaves no child to reap (`waitpid(-1) → ECHILD`); it fails on the old
  code and passes on the fix (`make docker-hwtrace`, 95 tests green).

- **AMD LBR live capture (first verified on real hardware, Zen 5).** Running the
  branch-record capture on an actual AMD LbrExtV2 host (Ryzen 9 9950X, Zen 5 — the
  project's real dev box, long mis-documented as Zen 2) surfaced a capture bug:
  `hwtrace_end_amd` kept the **last** `PERF_SAMPLE_BRANCH_STACK` sample, which for a
  small routine is all post-routine glue branches — it decoded to an empty in-region
  trace yet reported it **complete** (`truncated=0`). It now keeps the sample
  **richest in in-region branches** (the one taken at/just after the routine, whose
  16-deep window still holds its branches) and sets `truncated` when none is found —
  the honest dynamic-fallback signal. A branch-heavy loop now reconstructs exactly
  from the live LbrExtV2 stack; a tiny single-shot routine (too fast for an in-region
  PMU sample) honestly truncates. New live regression `test_amd_live` +
  `make docker-hwtrace-amd` lane (hwtrace image run with `--security-opt
  seccomp=unconfined --cap-add=PERFMON`); the standard `docker-hwtrace` lane is
  unchanged (AMD self-skips without perf). Docs corrected across the trace-parity
  matrix, native-tracing, and the AMD-LBR / single-step plans (dev host is Zen 5
  with `amd_lbr_v2`; AMD LBR live-verified, no longer "unverified on dev").

- **Emulator handle reuse.** Unicorn's translation-block cache is now flushed
  when new code is loaded, so reusing an `emu_t`/guest handle for a different
  routine no longer re-runs the previous routine's stale translation.

## [1.0.0] — 2026-06-24

First tagged release. Captures the complete framework plus the
Track A self-test suite and Track B packaging.

### Added

- **Core framework.** Auto-discovered `TEST(...)` cases, a provided `main()`,
  per-suite `SETUP`/`TEARDOWN`, `SKIP(reason)`, and colored TAP reporting with a
  nonzero exit on failure.
- **Assertions.** `ASSERT_TRUE/FALSE`, signed `ASSERT_EQ/NE/LT/LE/GT/GE`,
  unsigned `ASSERT_UEQ/UNE/ULT/ULE/UGT/UGE`, `ASSERT_STREQ`, `ASSERT_MEM_EQ`
  (hexdump diff), `ASSERT_REG_EQ`, FP `ASSERT_FP_EQ/NEAR` + lane
  `ASSERT_DEQ/DNEAR/FEQ/FNEAR`, and `ASSERT_VEC_EQ`.
- **ABI capture.** Register/flags capture via `ASM_CALLn`, `ASSERT_ABI_PRESERVED`,
  `ASSERT_FLAG_SET/CLEAR`; full call model (`ASM_CALLN`, `ASM_SRET`, `ASM_FCALLn`,
  `ASM_VCALLn`, struct-by-value) across the System V integer/FP/vector paths.
- **Differential / property testing.** `ASSERT_MATCHES_REF{1,2,3}` with a seedable
  splitmix64 RNG (`ASMTEST_SEED`).
- **Robustness & CLI.** Per-test `fork()` isolation with an `alarm()` timeout,
  crash/hang containment, and a runner CLI (`--filter`, `--list`, `--shuffle`/
  `--seed`, `--timeout`, `--no-fork`, `--format=tap|junit`).
- **Benchmark mode.** `BENCH(...)` cases timed in cycles/call via `rdtsc` /
  `cntvct_el0`, run under `--bench`.
- **Portability.** x86-64 and AArch64, Linux and macOS; GAS (default) and NASM
  (`ASM_SYNTAX=nasm`, x86-64) backends. CI covers all four OS/arch combinations.
- **Emulator tier (optional).** Unicorn-backed x86-64, AArch64, RISC-V (RV64),
  and ARM32 guests; Windows x64 ABI on the x86-64 engine; instruction trace and
  basic-block coverage.
- **Framework self-tests (Track A).** `tests/positive.c`, `tests/negative.c`, and
  the `tests/expect.sh` black-box harness, run by `make check` and wired into CI.
- **Packaging (Track B).** `ASMTEST_VERSION` macros; `make lib` builds
  `libasmtest.a`; `make install`/`uninstall` honoring `PREFIX`/`DESTDIR`; an
  `asmtest.pc` pkg-config file; and `make amalgamate` producing the single-header
  `asmtest_single.h`.

[Unreleased]: https://github.com/wilvk/asm-test/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/wilvk/asm-test/releases/tag/v1.0.0
