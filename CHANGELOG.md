# Changelog

All notable changes to asm-test are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims
to follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Java binding publishable to Maven Central (distribution-packaging.md T6).**
  `make java-package` now runs a real `mvn package` against `bindings/java/pom.xml`
  (the same POM Central publishes) instead of raw `javac` + `jar cf`, emitting the
  binding jar plus matching `-sources`/`-javadoc` jars. A new tag-gated, secret-guarded
  `maven` job in `release.yml` `mvn deploy`s them — GPG-signed — to the Central Portal
  staging, no-opping unless both `MAVEN_CENTRAL_TOKEN` and `MAVEN_GPG_KEY` are set
  (mirroring the crates/npm publishes). Maven is a version-pinned Apache tarball in the
  java docker image; `make docker-java-package` proves the whole build locally with no
  credentials, and `docs/reference/releasing.md` carries the Central + LuaRocks runbooks.

- **Cross-system benchmark legs on real Windows and Intel macOS.** The deterministic
  golden gate now runs on every OS the framework targets: a per-push
  `benchmarks-windows` leg (`windows-latest`, mingw/MSYS2) builds the PE benchmark
  producers and runs `make win64-bench-check` + `win64-bench-report` on a genuine
  Windows kernel, and a nightly `benchmarks-macos-x86` leg (`macos-15-intel`)
  produces the Intel-macOS report. `benchmarks-compare` merges all five OS × arch
  reports.

- **Nightly auto-commit of per-box benchmark records.** On the nightly schedule
  (and manual dispatch) each benchmark leg records its per-box history and a new
  `benchmarks-record` job commits `benchmarks/boxes/gh-**` back to `main` as
  `github-actions[bot]` (a `GITHUB_TOKEN` push, so it never re-triggers CI). Golden
  emu counts stay human-reviewed — never auto-committed — so a real count drift
  fails a leg's `bench-check` instead of being laundered into history. New
  `make win64-bench-record` persists the Windows box record.

- **Ambient stitched operations (.NET, opt-in, Intel PT).** `AsmAmbientStitchedTrace`
  follows one logical operation across `await` / thread hops with **zero calls in the
  body** — an `AsyncLocal` value-changed handler opens a per-thread `intel_pt` slice
  when the flow lands on a thread and decodes-at-disable when it leaves, then stitches
  the slices at close (`op.Hops` / `op.Path`). PT-only by construction; off Intel PT it
  self-skips and runs the body uninstrumented. Built on the new per-tid PT hop capture
  primitive (`asmtest_hwtrace_pt_hop_open`/`_hop_close`) on the one shared `intel_pt`
  perf-AUX arm. The default whole-window capture is unchanged (it stays the honest
  per-thread window that flags `truncated` on a cross-thread hop — never auto-stitch).

- **Runtime-enabled jitdump byte recovery on an already-running process** — turn jitdump
  emission on in a live foreign runtime and recover a JIT method's recorded bytes with
  `asmtest_jitdump_find`, with **no launch flag** on the target. For **CoreCLR**
  (`make docker-hwtrace-jit-dotnet-attach-jitdump`) the harness sends `EnablePerfMap(All)`
  over the runtime's diagnostics IPC socket (the documented `DOTNET_IPC_V1` wire, hand-rolled
  in C — no NuGet), so an already-JITted `Program::Add` is rundown-emitted into
  `/tmp/jit-<pid>.dump` even though the victim was launched without `DOTNET_PerfMapEnabled`.
  For **HotSpot** (`make docker-hwtrace-jit-java-attach-jitdump`) it loads a new in-tree
  attach-capable JVMTI jitdump agent (`examples/jvmti_jitdump_agent.c`, test-support only,
  never shipped) into the running JVM with `jcmd JVMTI.agent_load`; on attach the agent
  replays every already-compiled method via `GenerateEvents(COMPILED_METHOD_LOAD)` into the
  dump. **Correction:** the linux-tools `libperf-jvmti.so` exports only `Agent_OnLoad` (no
  `Agent_OnAttach`), so HotSpot refuses to load it via `jcmd` — it is `-agentpath`-only and
  cannot serve the attach case; hence the bespoke agent. V8/Node has no runtime-enable path
  (`--perf-prof` is wired once at isolate init), so there is deliberately no Node attach lane.
  Both lanes run on any host with the runtime — **no Intel PT / hardware gate**.

- **Opt-in safe-managed whole-window policy.** `ASMTEST_WHOLEWINDOW_SAFE_MANAGED=1`
  makes `asmtest_hwtrace_begin_window` **refuse** an in-process `EFLAGS.TF`
  whole-window arm when a managed runtime (CoreCLR / JVM / Mono) lives in the process,
  returning the new distinct `ASMTEST_HW_EMANAGED` status instead of single-stepping
  code whose `SIGTRAP` disposition the runtime's signal layer owns. The .NET empty-ctor
  `new AsmTrace()` builds on it to route a managed window to Intel PT where the silicon
  exists, else the §D3 out-of-process stepper, else an honest self-skip — **never**
  in-process TF (`ww.Route` reports the chosen route). Default (env unset / no
  `safeManaged`) is byte-identical to today; `asmtest_hwtrace_managed_runtime_present()`
  exposes the probe.

- **Native trace-point → IL / bytecode / source-line attribution.** A captured native
  offset of a managed method now resolves to a **source line**, a **.NET IL offset**, or a
  **JVM bytecode index** — not just a method name — from feeds the runtimes already emit.
  `asmtest_jitdump_debug_find` (`include/asmtest_ptrace.h`) recovers the per-address
  `(line, column, file)` table from a jitdump's `JIT_CODE_DEBUG_INFO` records the byte
  reader skips, and `asmtest_jitdump_debug_line_map` bridges it into the shipped
  `emu_line_map_t` (works today for **V8** `node --perf-prof`). A widened,
  backend-neutral schema (`asmtest_srcmap_*`, `asmtest_srcreg_*`, `include/asmtest_trace.h`)
  carries `offset → {kind, value, file, column}` with enclosing-point lookup and a
  version-keyed registry stamped on the code-image capture sequence, so a method re-JIT'd at
  a reused address resolves against the body live when the trace ran. For **CoreCLR**, a new
  `IlToNativeMap` EventListener (`bindings/dotnet/hwtrace/HwTrace.cs`) subscribes the
  `MethodILToNativeMap` JIT keyword (0x20000) in-process — no launch knob, no Intel PT — and
  resolves an address to `(method, nativeOffset, ilOffset)`. For **HotSpot**, a new
  `make docker-hwtrace-jit-java-bci` lane loads an in-tree JVMTI agent
  (`examples/jvmti_bci_agent.c`, test-support only, never shipped) that captures
  `CompiledMethodLoad`'s address→bytecode-index map into an `asmtest_srcreg` and proves a
  native address resolves to a real bytecode index. The V8/HotSpot/CoreCLR jitdump debug
  lanes (`make docker-hwtrace-jit-jitdump`, `-jit-java-jitdump`, `-jit-dotnet-jitdump`) print
  the per-method attribution table and CHECK the reader against each real encoder. Attribution
  covers **JIT-compiled code only**: interpreted code keeps its bytecode index in VM state
  the native PC stream cannot see — see
  [il-bytecode-attribution.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/analysis/il-bytecode-attribution.md).

- **libdft64 differential taint oracle** (`make docker-taint-oracle`). The shipped
  DynamoRIO in-band taint client had only an offline emulator/Capstone forward-slice
  as an independent check. This lane cross-validates it against a **second, live,
  independently-implemented byte-level taint engine** — [libdft64][libdft64] on Intel
  Pin — by running the *same* seed/sink fixtures (`examples/taint_fixtures.h`) through
  both and asserting **byte-for-byte sink agreement** on the general-purpose /
  integer-memory subset both cover (branch-condition, call-argument, and mem-copy-length
  sinks), with passing negative controls. A pinned, digest-gated **Pin 3.20** kit
  (libdft64's only tested pin) + git-commit-pinned **libdft64** are fetched at build
  time and are **test/oracle-only** — never linked into `libasmtest` or any shipped
  binding. libdft64's documented blind spots (SIMD is `basic SSE/AVX`, rules unverified;
  no eflags; no ZMM; no implicit flow; no x87/ternary) are enumerated as **named skips**,
  never a blanket pass — see [data-flow-capture.md][dfc] (even libdft punts on SIMD). The
  lane is CI-gated (the `taint-oracle` job) and self-skips only on non-x86 hosts (Pin is
  x86-64 gcc-linux).

[libdft64]: https://github.com/AngoraFuzzer/libdft64
[dfc]: https://github.com/wilvk/asm-test/blob/main/docs/internal/analysis/data-flow-capture.md

- **Intel SDE future/absent-ISA test lane** (`make docker-sde` / `make sde-test
  SDE_HOME=$(scripts/fetch-sde.sh)`). Assembly that uses an ISA extension the host
  CPU lacks — APX's `r16`-`r31`, AVX10.2, AMX, or AVX-512 on an AVX2-only box — was
  untestable by this framework (the DynamoRIO tier runs on real silicon; the
  Unicorn tier's vendored QEMU 5.0.1 predates AVX TCG). A **pinned, digest-gated
  Intel SDE 10.8.0** (`scripts/fetch-sde.sh` + `Dockerfile.sde`, with APX-capable
  GAS from binutils 2.46.1 and NASM 3.02, all SHA-256-pinned) emulates those
  extensions for the *whole process*, so an **unmodified** suite binary runs under
  `sde64 -future` and gets the full register/flag/memory/ABI assertion battery on
  any x86-64 host, including CI runners. The lane proves SDE is byte-for-byte
  **transparent** to correct baseline code (native vs SDE TAP identical), adds an
  **APX fixture suite** (`examples/apx_basic.s` + `test_apx_basic`, gated on a new
  `asmtest_cpu_has_apx()` CPUID probe) that skips on real pre-APX silicon and runs
  green under emulation, asserts the **AVX-512-on-AVX2 un-skip** (an existing
  `test_simd` capability skip becomes a real execution under `-future`),
  **cross-checks SDE against the Unicorn tier** on overlapping baseline ISA, and
  offers an optional `-mix` **instruction-mix report** (`make sde-mix`) folded into
  the canonical `asmtest_trace_t` shape. SDE is proprietary freeware, fetched +
  digest-verified at build/test time and never bundled into a shipped artifact —
  test-lane only. Documented in
  [the SDE testing guide](https://github.com/wilvk/asm-test/blob/main/docs/guides/tracing/sde-testing.md)
  and
  [the implementation doc](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/pin-sde-future-isa-lane.md).

- **Live whole-window compose lane (.NET).** The `hwtrace-dotnet` self-suite
  gains checks proving the zero-config compose seam (`MethodLoadVerbose` →
  `codeimage_track` → close-time versioned decode): over the in-process WEAK
  single-step tier (`new AsmTrace()`) against a managed method whose **first JIT
  happens inside the window** (genuinely-compiled-in-window, not a pre-warmed
  body); over the crash-proof §D3 out-of-process inline `using`-scope
  (`new AsmTrace(outOfProcess: true)`) against a **resident** method — the first
  suite coverage of that bare inline OOP whole-window ctor; and (self-skipping
  off Intel-PT silicon) the STRONG PT tier — plus a mid-window re-tier
  decode-at-version check. Runs as the named `make docker-hwtrace-dotnet-unwarmed`
  lane. Tasks T1–T5 of the
  [managed-wholewindow-compose implementation doc](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/managed-wholewindow-compose.md).
  Two re-verification findings refine the doc's original premise: (1) forcing a
  stop-the-world `GC.Collect(0)` **on the single-stepped thread inside the
  window** is intermittently fatal — the in-process EFLAGS.TF window dies
  (SIGTRAP) if the runtime spawns a thread in-window, per the repo's own
  degradation note — so it is omitted (the unwarmed JIT itself supplies the
  live-window instruction noise); (2) the inline OOP ctor's **region-free
  `window_stop` stepper single-steps everything**, so a first-call JIT inside
  that window steps the whole compiler and aborts CoreCLR (exit 134) — the
  unwarmed mid-window-JIT compose is therefore proven on the **range-based
  `AsmTrace.Window` factory** (whose stepper runs the JIT at native speed),
  while the inline OOP ctor is for resident (warm) code, matching the
  `crashproof-showdown` example.
- **Data-flow F5: an out-of-band PT + code-image + Unicorn-replay value
  producer** (`make dataflow-pt-test` / `make docker-dataflow-pt`). The
  least-perturbing L0 value tier: it reconstructs an Intel PT trace's executed
  instruction stream (captured with **zero** single-steps), supplies the bytes
  live at trace time from the code-image recorder, and **replays that exact path
  through Unicorn** to derive per-instruction values into the same
  `asmtest_valtrace_t` the shared def-use (L1) and slice (L2) analysis consume —
  byte-identical to the emulator L0 oracle on a deterministic region.
  `src/dataflow_pt.c` opens **no** perf event (it consumes a captured AUX blob +
  code-image); it reuses the block-step tier's purity/replayability verdicts and
  **truncates honestly** on an impure, VEX/EVEX, or nondeterministic region (no
  single-step fallback), a per-step path cross-check catching a divergence. The
  synthetic-AUX **decode→rebase→materialize→replay bridge is validated in CI with
  no PT hardware** (libipt's own encoder, `libipt-dev` added to
  `Dockerfile.dataflow-attach`); **live foreign-pid capture is silicon-gated**
  (bare-metal Intel PT + the `intel-pt-attach-foreign-pid` capture arm) —
  wiring-complete, hardware-unvalidated, with a `make dataflow-pt-live`
  fail-not-skip target for a runner that claims PT. Documented in
  [the native-tracing guide](https://github.com/wilvk/asm-test/blob/main/docs/guides/tracing/native-tracing.md)
  and
  [the F5 implementation doc](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/dataflow-pt-replay-tier.md).
- **Whole-window in-process guards for the zero-config scope (deny regions /
  instruction budget / wall-clock watchdog).** The `using (new AsmTrace())`
  region-free whole-window scope, powered by the in-process `EFLAGS.TF`
  single-step tier, took the descent tier's "step into everything" semantics
  without any of its safety guards: a window over code that reached a blocking
  libc call (a `read` on an empty pipe, a `poll`) stepped the runtime forever,
  bounded only by the capture ring's memory, never by time. The three
  out-of-process descent guards are now ported onto that in-process path — a
  per-frame **instruction budget** (default 4x the ring cap), a process-global
  **deny-region table** with an opt-in blocking-libc default set (a stepped RIP
  inside a denied region ends the capture and the denied call then runs at native
  speed), and a repeating **`ITIMER_REAL`/`SIGALRM` watchdog** (default 10 s;
  breaks a blocked syscall via `EINTR`) — all malloc/lock-free inside the SIGTRAP
  handler. Plain `asmtest_hwtrace_begin_window` gets safe defaults (budget +
  watchdog on, denylist off), so a hung zero-config window is always bounded; the
  new additive `asmtest_hwtrace_begin_window_ex` (with the F27/F36 `struct_size`
  idiom and an `asmtest_hwtrace_window_guards_t` config) configures them per
  window, and `asmtest_hwtrace_window_guard` reports which guard fired
  (`ASMTEST_HW_GUARD_*`) render-on-close style. C-level knobs (parity-exempted);
  the zero-config defaults flow through the `begin_window` every binding already
  wraps.

- **Zero-config whole-window scope docs.** The
  [hardware-tracing guide](https://github.com/wilvk/asm-test/blob/main/docs/guides/tracing/hardware-tracing.md)
  gains a "zero-config whole-window scope (region-free)" section — the C `begin_window` /
  `end_window` / `render_window` surface, the .NET `using (new AsmTrace())` form, the
  WEAK / STRONG / CEILING tier ladder (single-step / Intel PT / AMD LBR Zen 4+), and how
  the STRONG-tier PT decode is validated on a synthetic PT-packet fixture with no
  silicon. The
  [troubleshooting reference](https://github.com/wilvk/asm-test/blob/main/docs/reference/troubleshooting.md)
  gains a `SkipReason` table and a one-time-provisioning table (`perf_event_paranoid` /
  `setcap cap_perfmon` / `--cap-add`, ptrace `CAP_SYS_PTRACE`, eBPF `CAP_BPF`), and both
  it and
  [portability](https://github.com/wilvk/asm-test/blob/main/docs/reference/portability.md)
  record the whole-window facility's Linux-only floor.

- **asmspy runs on AArch64 Linux.** The out-of-process tracer's
  register / single-step / detach reads are lifted behind an architecture shim
  (`cli/asmspy_arch.h`: PC / return / SP / LR / syscall-number accessors over
  `PTRACE_GETREGS` on x86-64 and `PTRACE_GETREGSET(NT_PRSTATUS)` on AArch64), so
  every engine — `--stream`, `--graph`, `--tree`, `--region`, `--log`, `--procs`
  — runs on both arches. The single-step teardown honours AArch64's kernel-owned
  step model (no user trap flag; `svc #0` syscall-instruction guard), the
  call-graph / call-tree frame logic uses AArch64 `bl`-writes-LR semantics
  (frame identity `(entry_lr, sp)`), and `--log` decodes the AArch64 syscall ABI
  (number in `x8`, args in `x0`-`x5`, `*at`-only name table). `--watch` gains an
  AArch64 arm over the `NT_ARM_HW_WATCH` regset (`DBGWCR`/`DBGWVR`/`BAS`
  encoding, pinned by a pure `cli/test_arch.c` unit test on every host); it
  self-skips where the host exposes no watchpoint slots (qemu-user, some
  hypervisors). Validated on the native `ubuntu-24.04-arm` CI runner (a real VM,
  not qemu) alongside the existing x86-64 leg; see
  [the asmspy guide](https://github.com/wilvk/asm-test/blob/main/docs/guides/tracing/asmspy.md).
- **libFuzzer / AFL++ external-engine fuzzing shim (`make docker-fuzz`).** Drive
  an x86-64 guest routine under the emulator with an industrial fuzzer, feeding
  the emulator's basic-block coverage into the engine's feedback channel without
  compiler-instrumenting the guest bytes (they run under Unicorn). A new tested
  seam `emu_cover_hits` (`src/fuzz.c`) reports one input's distinct executed
  block offsets; `examples/fuzz_libfuzzer.c` registers them as SanitizerCoverage
  8-bit counters (+ the PC table clang-18's libFuzzer requires), and
  `examples/fuzz_afl.c` (native persistent-mode forkserver) plus an
  aflpp_driver reuse of the libFuzzer harness write them into AFL++'s
  shared-memory bitmap via a plain-compiled helper (`examples/fuzz_afl_map.c`).
  `Dockerfile.fuzz` (clang 18 + afl++ 4.09c on the bindings base) runs
  `make fuzz-shim-test`, which fails unless **both** engines steer to a planted
  crash — a real test, never a self-skip. Node (per-block) coverage; documented
  in [the fuzzing-shim guide](https://github.com/wilvk/asm-test/blob/main/docs/guides/fuzzing-shim.md).
  `src/capture.s`; `ret` stubs elsewhere, incl. the NASM twin) marshals
  scalable-vector `z0..z7` and predicate `p0..p3` arguments per AAPCS64 and
  captures the whole `z0..z31` / `p0..p15` file into two new max-size containers
  — `svec_t` (256-byte VLmax) and `spred_t` (32-byte PLmax), of which only the
  low `asmtest_sve_vl()` (resp. `/8`) live bytes are written. `ASM_SVCALL_1`/`_2`
  call and **self-skip** without SVE, and `ASSERT_SVEC_EQ`/`ASSERT_SPRED_EQ`
  compare exactly the live VL. A `HWCAP_SVE` runtime probe
  (`asmtest_cpu_has_sve` / `asmtest_sve_vl`) returns 0 everywhere SVE is absent
  (x86-64, and macOS arm64 — Apple silicon has no non-streaming SVE); the ABI
  manifest pins both containers, and the corpus routine `sve_addd` exercises the
  path. The new `make docker-sve-sweep` lane runs the SIMD suite under qemu-user
  at several vector lengths (VQ 1/3/8/16 → VL 16/48/128/256 bytes, including a
  non-power-of-two) to flush out VL-assumption bugs before any SVE silicon is
  available; execution sign-off on real SVE hardware (Graviton3/Grace/A64FX-class)
  remains pending — see
  [aarch64-sve-capture.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/aarch64-sve-capture.md).

- **XED-decoded Intel Pin trace lane (`make docker-pintool` / `make
  pintool-test`).** A pinned, digest-gated Intel Pin 4.2 kit
  (`scripts/fetch-pin.sh`, SHA-256 pinned in `scripts/third-party-digests.txt`)
  drives a Pintool (`pintool/asmtest_pintool.cpp`) that fills the shared
  `asmtest_trace_t` offset model over POSIX shared memory. The lane asserts
  byte-for-byte instruction/block offset parity with both the in-process
  single-step backend and the DynamoRIO backend (Pin ≡ DynamoRIO ≡ single-step),
  and carries an Intel APX (EGPR/REX2) fixture whose bytes Pin's XED decodes on
  any x86-64 host while the pinned DynamoRIO decoder rejects them — the
  decoder-currency gap the tier exists to close
  ([DR #6226](https://github.com/DynamoRIO/dynamorio/issues/6226) is open; the APX
  execution halves are gated on APX silicon). Test-lane only: Pin is
  digest-verified at build/test time and never bundled into a shipped package.

- **Intel Pin probe-mode argument/return capture lane (`make pin-probe-test`, in
  `make docker-pintool`).** A Pin **probe-mode** tool (`pintool/probe_capture.cpp`)
  splices a jump at a named routine's entry/exit and records the SysV integer/FP
  **argument** registers, the **return** register(s) + flags, and up to a **4 KiB**
  cap of a pointed-to buffer — at **native speed** (no code cache) — into
  `at_val_rec_t` records over a POSIX shm channel
  (`include/asmtest_valtrace_shm.h`). A pointer is validated against the target's
  mapped ranges and the read clamped to the cap and the mapping end, so an invalid
  pointer is **refused**, never faulted; a routine too short or non-relocatable to
  probe is reported as an explicit per-target **skip with a reason**. The capture
  is proven by diffing it against the independent out-of-process ptrace stepper on
  the same routine (two producers agree on the arg + return registers). Captured
  buffers may contain secrets — a sensitive artifact. x86-64 Linux, test/oracle
  only (Pin is proprietary freeware, digest-verified at test time and never
  bundled), the same handling DynamoRIO gets; documented in
  [the data-flow tracing guide](https://github.com/wilvk/asm-test/blob/main/docs/guides/tracing/data-flow.md).

- **Real object identity for managed memory def-use on the live-attach tier.** A
  heap snapshot of `{Address, Size, TypeID}` nodes from the runtime's
  GCBulkNode / GCBulkEdge / GCBulkType events, joined with the `MovedReferences2`
  move feed, keys each captured memory record on *(object, offset)* where the
  snapshot has evidence and degrades to the landed address identity where it does
  not (`asmtest_objid_canonicalize`, `src/dataflow_objid.c`; unit suite
  `test_dataflow_objid`). On a live attach (`make docker-gccanon-attach`, new
  `alias` phase) the false def-use edge that address identity forges when a GC
  slides a live object onto a dead object's vacated slot is reproduced under
  address identity and then eliminated by object identity.

- **Weighted cross-ISA cost proxy `BM_MODEL_COST` in the cross-system benchmark.**
  `emu-bench` now emits a `model_cost` row beside each deterministic `insns` row:
  each executed instruction is classified with Capstone (new
  `asmtest_disas_class` → OTHER/MEM/BRANCH/MULDIV) and summed against a fixed
  weight table (1/3/2/8), an honest cross-architecture cost *model* — comparable
  by construction, **not** silicon cycles. `bench-compare` renders it as its own
  *Model cost* matrix (never mixed with raw counts or real cycles). The metric
  needs Capstone: without it the bench emits the `insns` rows alone and every gate
  still passes. Model values depend on the Capstone version, so they are kept out
  of the golden file — `bench-golden-check` filters `model_cost` rows (and fails
  loudly if any `insns` row is missing its model sibling). See
  [cross-system benchmarking](https://github.com/wilvk/asm-test/blob/main/docs/guides/cross-system-benchmarking.md#the-weighted-model-cost-metric-bm_model_cost).

- **Native RISC-V (rv64) host tier — the capture framework now runs *on* a RISC-V
  machine, not just as an emulator guest.** A `regs_t` branch and trampolines
  (`src/capture.s`) for the RV64GC / LP64D psABI: `a0`/`a1` return pair, integer
  callee-saved `s0`–`s11`, and FP callee-saved `fs0`–`fs11` (checked by
  `ASSERT_ABI_PRESERVED` / `ASSERT_ABI_PRESERVED_VEC` after an `_fp`/`_fp_n`
  capture, since rv64gc has no vector file), plus rv64 bodies for every example
  suite and the framework self-tests. Two ISA facts are surfaced honestly rather
  than faked: RISC-V has **no condition-flags register**, so `ASMTEST_NO_FLAGS` is
  set and `ASSERT_FLAG_*` is a compile error on rv64 (flag-only suites self-skip
  with a printed reason); and there is **no 128-bit vector capture** (`ASM_VCALL*`
  self-skips via `asmtest_cpu_has_vec128()` — RVV is a possible future arm). A
  `make docker-riscv64` lane builds a `linux/riscv64` image and runs the core
  suites + self-tests under QEMU binfmt (`make binfmt-riscv64`, pinned
  `tonistiigi/binfmt`), wired into CI as the `test-riscv64` job. The tracing tiers
  stay x86-64/AArch64. See [riscv-native-tier.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/riscv-native-tier.md).

- **Live Intel PT whole-window smoke + a `make hwtrace-pt-live` lane that FAILS rather
  than skips where PT is claimed.** `test_pt_live_selfjit` (`examples/test_hwtrace.c`)
  self-JITs the canonical routine, arms the region-free PT capture, decodes the REAL AUX
  stream through `asmtest_pt_decode_window`, and exercises `PERF_EVENT_IOC_SET_FILTER`
  (via the new `asmtest_hwtrace_pt_set_filter` knob), the anonymous-JIT decode-time
  fallback, and AUX-ring truncation on a 4 KiB ring. Off the `intel_pt` PMU (AMD/VMs/
  containers) it self-skips with the specific reason — one of the two legitimate hardware
  gates — while `make hwtrace-pt-live` sets `ASMTEST_REQUIRE_PT=1` to convert that skip
  into a build failure on a runner that is supposed to expose bare-metal Intel PT. The
  live capture is silicon-gated (no `intel_pt` on the reachable dev boxes); in
  `make docker-hwtrace` the test prints a clean `# SKIP pt live: …` everywhere.
  See [intel-pt-whole-window-substrate.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/intel-pt-whole-window-substrate.md).

- **System-package specs for the C core — Homebrew, Debian, AUR, vcpkg and Conan —
  each built, installed and consumed in a Docker CI lane.** `packaging/` now holds a
  Homebrew formula, a Debian `libasmtest-dev` source package, an AUR `PKGBUILD` +
  `.SRCINFO`, a vcpkg overlay port and a Conan 2 recipe for the MIT static core (lib
  + headers + `asmtest.pc`; the GPL engines stay in the dlopen binding packages only,
  never here). `make docker-syspkg` runs all five lanes and an additive `syspkg` CI
  job runs them as a matrix — each builds the package, runs its native linter
  (`brew audit`/`style`, `lintian`, `namcap`, vcpkg post-build validation), installs
  it, and compiles a pkg-config/CMake consumer against it. The lanes are hermetic on
  the reproducible `make package-source` tarball; per-manager index publication is a
  maintainer step (runbook in
  [releasing.md](https://github.com/wilvk/asm-test/blob/main/docs/reference/releasing.md#system-packages)).
  See [distribution-packaging.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/distribution-packaging.md).

- **`.NET` inline `using (new AsmTrace(HwBackend.IntelPt))` now arms the STRONG whole-window
  Intel PT capture** (`bindings/dotnet/hwtrace/HwTrace.cs`), replacing the reserved
  `"forward-look (not wired)"` self-skip. The backend-keyed ctor gates on
  `HwTrace.Available(IntelPt)` (self-skip names the PT gate off bare-metal Intel PT), sets up
  the `JitMethodMap` + perf-map rundown before arming, and drives the native
  `asmtest_hwtrace_pt_begin_window`/`_end_window` pair via a finalizable `PtWindowCtx` (a
  leaked scope's fd + AUX mappings are reclaimed drain-less by the finalizer) and a new
  `Kind.PtWindow` Dispose that decodes on close against the map's code-image, filling
  `Addresses` with ABSOLUTE addresses and `IsStatistical == false`. `hwtrace-dotnet-test`
  carries the invariant-envelope case on any host; live capture is silicon-gated
  (`make hwtrace-pt-live` + `make hwtrace-dotnet-test` on a bare-metal Intel PT box). Only
  `HwBackend.CoreSight` keeps the forward-look self-skip.
  See [intel-pt-whole-window-substrate.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/intel-pt-whole-window-substrate.md).

- **Whole-window Intel PT STRONG tier wired behind the empty-ctor scope, with a runtime
  WEAK/STRONG decode-trust ladder.** `asmtest_hwtrace_begin_window`/`_end_window`
  (`src/hwtrace.c`) now arm and drain a real region-free Intel PT capture on an inited
  `INTEL_PT` tier — the ONE shared perf-AUX arm (`pt_aux_open`) that also serves the region
  path, exposed as the native `asmtest_hwtrace_pt_begin_window`/`_end_window` pair the .NET
  inline ctor uses. `asmtest_hwtrace_window_auto` auto-selects STRONG only when the
  `intel_pt` PMU is present **and** `asmtest_hwtrace_pt_window_trusted()` proves the
  whole-window decode on the §Z2 synthetic fixture at runtime; else the WEAK single-step
  tier. The CEILING AMD LBR tier is deliberately never auto-selected for the exact
  whole-window contract (a sampled branch survey cannot meet it; live floor Zen 4+) — the
  quiet sampled complement stays explicit (`new AsmTrace(HwBackend.AmdLbr)`). The .NET
  empty-ctor `using (new AsmTrace())` consults the ladder (`AutoInitWindowBackend`), and
  `DegradationNote()` names the PT probe outcome (present-but-untrusted vs no PMU). Live PT
  capture is silicon-gated; the ladder, the runtime trust probe, and the synthetic-fixture
  decode all run in `make docker-hwtrace` (green on this AMD host — no `intel_pt` PMU, so the
  ladder resolves to WEAK and the native PT pair self-skips).
  See [intel-pt-whole-window-substrate.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/intel-pt-whole-window-substrate.md).

- **Reproducible source tarball (`make package-source`) attached to every
  release.** `git archive` of HEAD piped through `gzip -n` emits
  `build/dist/asm-test-<version>.tar.gz` + `SHA256SUMS`, byte-identical for a
  given commit across machines, so its digest is known ahead of the tag. The
  `release.yml` corresponding-source job builds it, uploads it as a dry-run
  artifact, and attaches both files to the tagged GitHub release — the
  digest-pinned source the system-package specs (Homebrew/Debian/AUR/vcpkg/conan)
  and Debian's orig-tarball flow consume.
  See [distribution-packaging.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/distribution-packaging.md).

- **Socket-syscall `sockaddr` contents are decoded** (asmspy `--log`). `connect`/`bind`/
  `sendto` render their `struct sockaddr *` as `{AF_INET, 127.0.0.1:8080}` /
  `{AF_INET6, [::1]:80}` / `{AF_UNIX, "/path"}` (abstract sockets as `"@name"`), and
  `accept`/`accept4`/`recvfrom` decode their OUT pointer on success (raw pointer on
  failure); `socket()`'s domain renders as `AF_INET`/`AF_UNIX`/… An unknown family prints
  `{family=N, len=M}`, never a guessed name. `make docker-cli` cli-smoke PASS.
  See [asmspy-cli-enhancements.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/asmspy-cli-enhancements.md).

- **`ioctl` requests and `fcntl` commands are named** (asmspy `--log`). `ioctl` renders
  `TIOCGWINSZ`-style names, or an honest `_IOC(dir, type, nr, size)` decomposition for an
  unknown request (never a guessed name); `fcntl` renders `F_GETFL`/`F_SETFD`/… with
  correct conditional arity (an arg-less command such as `F_GETFL` shows no third slot).
  `make docker-cli` cli-smoke PASS.
  See [asmspy-cli-enhancements.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/asmspy-cli-enhancements.md).

- **`futex` operations are named** (asmspy `--log`). The op renders as
  `FUTEX_WAIT`/`FUTEX_WAKE_PRIVATE`/… with `FUTEX_PRIVATE_FLAG` and
  `FUTEX_CLOCK_REALTIME` masked off before naming and re-rendered as suffixes,
  never silently dropped; an unknown op keeps its number plus those suffixes.
  `make docker-cli` cli-smoke PASS.
  See [asmspy-cli-enhancements.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/asmspy-cli-enhancements.md).

- **`stat`/`statx` result buffers are decoded** (asmspy `--log`).
  `fstat`/`stat`/`lstat`/`newfstatat` render `{st_mode=S_IFREG|0644, st_size=18}` on
  success (a raw pointer on failure), and `statx` renders its mask-honoring
  `{stx_mode=…, stx_size=…}` (a field the kernel did not fill is omitted, not invented).
  The path decode these calls already had is preserved. `make docker-cli` cli-smoke PASS.
  See [asmspy-cli-enhancements.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/asmspy-cli-enhancements.md).

- **Hot-edges → data-flow drill-in** (asmspy TUI, mode 7). In the frozen hot-edges view,
  arrows select an edge and `Enter` opens a data-flow capture (mode 9) of the function
  containing the edge's `to_addr` (falling back to `from_addr`), reusing the call-graph
  drill-in idiom. The pure decision (`asmspy_edge_drill`) requires a sized function and
  accepts a mid-function landing (drill ≠ rank); it is unit-tested in `test_autoregion`
  (6 checks) so it is covered on every host, not just an AMD IBS box. The ncurses wiring
  is pty-driven (manual-only); the decision logic runs in CI.
  See [asmspy-cli-enhancements.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/asmspy-cli-enhancements.md).

- **Block-step replay record-and-inject for rdtsc/rdtscp/rdrand/rdseed/cpuid, gated per
  block rather than per region.** `src/dataflow_blockstep.c`'s `step_block` now injects
  each site's recorded post-state (read from the T5 DR exec-breakpoint boundary) into the
  Unicorn replay and terminates the block there — the same record-and-inject shape as
  `syscall`/`int 0x80`, minus the producer-local write-set synthesis (Capstone already
  reports the complete architectural write set for all five mnemonics). `region_scan`'s
  `injectable` verdict now admits regions whose only impurities are syscall/int80/HWREC
  (subject to the existing 4-slot DR0-3 cap — a 5th+ distinct site still falls back to
  single-step, reason `hwrec-overflow`), and a region no longer forfeits the replay's
  perturbation win for a hwrec site its real run never reaches (`hw_hits`/`injected` stay 0
  for an unexecuted site while the region still replays). New opts test hook
  `no_hw_record` skips arming the DR breakpoints, reproducing the pre-injection
  fail-closed truncation on demand. `make dataflow-blockstep-test` 191/191 (was 186/186,
  +5), stable across 5 consecutive runs on this Zen 2 host.
  See [dataflow-producer-correctness.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/dataflow-producer-correctness.md).

- **Extents-driven block-step region scan: a caller-vouched list of real instruction
  extents lets `region_scan` skip an embedded constant-pool island instead of desyncing on
  it (BSVS-2).** New `asmtest_blockstep_extent_t` (`{off, len}`, blob-absolute) plus opts
  fields `extents`/`nextents` (`src/dataflow_blockstep.c`) — NULL/0 keeps today's
  whole-region sweep. `region_scan` is split into a per-extent inner sweep
  (`region_scan_extent`, reused for both the implicit whole-region case and each real
  extent) whose verdicts aggregate across extents; bytes outside every extent are never
  decoded, so a data island sitting between two extents costs nothing. `run()` validates
  extents are sorted, non-overlapping, and fully inside `[region_off, code_len)` before any
  tracee is spawned (`DF_BLOCKSTEP_EINVAL` otherwise); the public `is_pure`/`is_replayable`/
  `is_injectable` classifiers stay whole-blob (extents are a `run()`-only capability). New
  fixture `island_sse` (the same constant-pool-island shape as the existing `island`
  fixture, with a legacy-SSE `paddq` in place of `island`'s VEX-128 `vpaddq` so extents can
  actually recover it into the replay path) proves the positive case byte-identical to the
  single-step oracle with stops cut, while desyncing exactly like `island` without extents —
  the negative control. `make dataflow-blockstep-test` 199/199 (was 191/191, +8), stable
  across 5 consecutive runs; `make docker-dataflow-attach` 520/520 across all 8 suites, 0
  skips; `make docker-docs` clean.
  See [dataflow-producer-correctness.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/dataflow-producer-correctness.md).

- **Def-use graph and forward/backward slice surface in the Ruby, Lua, Zig,
  Rust, Go, Java and .NET data-flow bindings (previously producer-only), via a
  by-pointer slice-seed entry point** (`asmtest_slice_forward_seed`/
  `_backward_seed`, `include/asmtest_valtrace.h` — a 72-byte `at_val_rec_t` is
  SysV MEMORY-class and several of these FFIs, Ruby Fiddle chief among them,
  cannot pass it by value). Each binding's `ValueTrace` now exposes
  `defuse()`/`forward_slice(step)`/`backward_slice(step)` alongside the
  existing live-attach producer, so all ten language bindings (with the prior
  Python/C++/Node) share one surface. Round-trip-tested with a hand-built
  r10→r11→r12 register-move chain (`forward_slice(0)`/`backward_slice(2)` both
  `{0,1,2}`) and, over the shared live-attach `df_chain` fixture, the
  **memory** def-use edge these seven could never reach before — `backward_slice(4)`
  and `forward_slice(0)` both equal `{0,1,2,3,4}` (the store at step 1 reached
  through the load at step 2), excluding the trailing `ret`. All seven
  `docker-dataflow-<lang>` lanes green at their new 40/40 (was 36/36), 0 skips.
  See [dataflow-bindings-slice-codeimage.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/dataflow-bindings-slice-codeimage.md).

- **Code-image recorder wrapper and versioned (time-correct) operand decode in
  all ten data-flow language bindings** (`asmtest_codeimage.h`'s `new`/`track`/
  `now`/`bytes_at`/`free`, plus the new `asmtest_dataflow_ptrace_attach_pid_versioned`
  entry point). Each binding gains a `CodeImage` wrapper (Python/Ruby/Lua's
  `available()`/`skip_reason()`/`track()`/`now()`/`bytes_at()`, C++'s RAII
  `CodeImage`, Node's mirroring the existing hwtrace-binding class, Zig/Rust/Go/
  Java/.NET's thin function wrappers) and a `ValueTrace.attach_pid_versioned`
  method; `attach_jit` no longer unconditionally passes `NULL`/`null`/`nil` for
  the versioned-decode `img` argument — a caller with a recorder now gets
  time-correct operand decode across a mid-capture JIT patch/free/reuse
  instead of NULL/live-snapshot bytes. Verified live: each binding tracks a
  recorder over a real victim's published region and decodes an
  `attach_pid_versioned` capture through it (result and step-count assertions
  tied to that run's own arguments, so a stubbed capture cannot pass). All ten
  `docker-dataflow-<lang>` lanes green with the new assertions, 0 skips.
  See [dataflow-bindings-slice-codeimage.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/dataflow-bindings-slice-codeimage.md).

- **Block-step pre-cover: an IBS covered-block table that memoizes the ptrace
  block-step reconstructors' decode.** `asmtest_bs_precover_build`/`_free`
  (`include/asmtest_blockstep_internal.h`, internal — no new public ABI symbol)
  pre-walks each `asmtest_ibs_normalize_blocks`-covered leader's straight-line
  run ONCE and caches the per-instruction facts `classify_branch` would
  otherwise recompute on every `#DB` stop; `blockstep_reconstruct` (shared by
  the region and attached block-step drivers) resolves a cache hit by
  replaying `asmtest_bs_scan_terminator`'s exact decision procedure over the
  cached run — same bytes, same primitives, zero Capstone calls — so a hit is
  provably identical to a fresh scan, and a miss (including a leader that is
  not a real instruction boundary — the hostile case) falls back to the
  shipped path unconditionally. IBS stays statistical: pre-cover only memoizes
  tracer-side decode, never lets coverage skip *recording* anything (the exact
  parity contract in `asmtest_ibs.h`'s INVARIANT stands). A differential over
  the `LOOP_X86` fixture (precover NULL vs. covering the loop head) proves
  byte-identical `insns[]`/`blocks[]`/`truncated`/result while cutting branch-
  probe decode calls; `asmtest_bs_stats`/`_reset` are test hooks that count
  cumulative probe calls and cache hits. See
  [ptrace-blockstep-tracer-correctness.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/ptrace-blockstep-tracer-correctness.md).

- **`ASMTEST_TRACE_IBS_PRECOVER`: an opt-in policy bit that wires the block-step
  pre-cover table above into the cross-tier auto cascade
  (`asmtest_trace_call_auto`, `include/asmtest_trace_auto.h`).** When set and
  IBS-Op is available, the block-step rung forks an isolated warm-up child that
  re-runs the routine for a bounded ~30ms while the parent surveys it out of
  band with `asmtest_ibs_survey_process` (no ptrace, no perturbation), builds a
  pre-cover table from the resulting live histogram, and installs it around the
  one `asmtest_ptrace_trace_call_blockstep` call the rung already made — so a
  trace produced with the bit set is byte-for-byte identical to one produced
  without it; any survey/build failure degrades silently to the plain rung. On
  a live Zen 2 host a 25-iteration loop's block-step branch-probe decode calls
  dropped from 101 to 0 (a real live warm-up survey covering both of the
  routine's basic blocks). Off AMD, or wherever the auto-cascade's fast
  in-process single-step backend already completes the capture before reaching
  block-step, the bit is a proven no-op — never a behavior change. See
  [ptrace-blockstep-tracer-correctness.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/ptrace-blockstep-tracer-correctness.md).

- **In-process, branch-granular single-step (W3): `asmtest_ss_btf_available` /
  `asmtest_ss_btf_trace` (`src/ss_btf.c`), the missing third single-step form.**
  asm-test already had branch-granular stepping out of process
  (`PTRACE_SINGLEBLOCK`) and per-instruction stepping in-process (`EFLAGS.TF`,
  `ss_backend.c`); this arms `DEBUGCTL.BTF` alongside `EFLAGS.TF` over the same
  thread-pinned `/dev/cpu/N/msr` route `asmtest_amd_msr_trace` uses, so a
  taken-branch retiring — not every instruction — is what traps, with **no
  16-entry ceiling** on the reconstructed stream (unlike AMD LBR). Gated by a
  hang-proof functional probe (some hypervisors silently mask `DEBUGCTL.BTF`
  and degrade to per-instruction stepping — the probe catches this, a build
  check cannot); deliberately scoped to a pinned leaf-routine envelope with
  per-trap re-arm (BTF is a hardware one-shot the CPU clears on every `#DB`)
  and honest truncation on any observed context switch (Linux does not
  preserve a user-written BTF across one) — the general, context-switch-proof
  case stays owned by the shipped `PTRACE_SINGLEBLOCK` trio. Rides the
  existing `docker-hwtrace-msr` `--privileged` lane, no new capability. Live-
  verified on a Zen 2 host: the shared `ROUTINE` fixture reproduces the
  single-step baseline's exact `[0,3,6,c,11]` stream and `{0,0x11}` block
  partition byte-for-byte, and a 20-trip loop (19 taken back-edges, past any
  16-entry LBR window) reconstructs all 62 instructions, complete, 10/10
  stable runs. See
  [inproc-btf-block-step.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/inproc-btf-block-step.md).

- **macOS out-of-process single-step tracer (`asmtest_mach_*`), completing the
  W2 foreign-process story on macOS.** `asmtest_mach_trace_call` /
  `_trace_attached` / `_run_to` (`asmtest_mach.h`, `src/mach_backend.c`) mirror
  the Linux `ptrace` out-of-process tracer's exact shape and offsets, but
  through `task_for_pid` + a Mach `EXC_MASK_BREAKPOINT` exception port +
  `thread_set_state` instead — macOS `ptrace` cannot edit `RIP`/`RFLAGS` at
  all. `run_to`'s breakpoint arm falls back from a software `int3` to a
  `DR0`/`DR7` hardware execution breakpoint on W^X code, same as the Linux
  tracer. x86-64 only for now. `make mach-stepper-test` (needs
  `scripts/codesign-debugger.sh`'s ad-hoc self-sign, or root) runs the lane
  live; self-skips (`ASMTEST_MACH_EPERM`) without either.

- **Pure tests for the IBS sample-period rounding/clamp and the additive-ABI
  `struct_size` guard (first coverage).** The `/16` rounding, the `<16 →
  default` clamp, and the `period_jitter` tail-guard had never executed against
  non-default values anywhere; internal `asmtest_ibs_effective_period` /
  `asmtest_ibs_effective_jitter` seams (shared with the live attr fill, so
  tested == shipped) now pin them.

- **Intel Pin vs. DynamoRIO analysis + a four-track umbrella plan — what Pin
  makes possible that the shipped DynamoRIO tier cannot.**
  [2026-07-17-intel-pin-vs-dynamorio.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/analysis/2026-07-17-intel-pin-vs-dynamorio.md)
  and [intel-pin-capabilities-plan.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/intel-pin-capabilities-plan.md).
  Most "Pin advantages" are **maturity, not impossibility** (DR has
  `drrun -attach`, supports Windows, and the taint ground is already held
  in-tree), and the note separates those out. Four items survive as separable
  plan tracks: **PIN-1** an **Intel SDE** lane that runs the existing `TEST()`
  suites under `sde64 -future` so **APX / AVX10.2 / AMX / AVX-512** assembly
  gets full register/flag/memory/ABI assertions **on any x86-64 host including
  CI** — a true impossibility for *both* DR (executes on real silicon) and the
  Unicorn tier (QEMU 5.0.1 predates AVX TCG; `vaddps ymm` → `UC_ERR_INSN_INVALID`
  and VEX-128 is silently mis-run as SSE), converting CLAUDE.md's "specific CPU
  generation" **hardware self-skip into an installable, pinnable dependency**;
  **PIN-2** an **XED**-decoded Pin trace tier for the newest extensions DR's own
  decoder rejects (**APX is open — [DR #6226](https://github.com/DynamoRIO/dynamorio/issues/6226)**;
  the once-broken AVX-512 VNNI is fixed — [DR #5440](https://github.com/DynamoRIO/dynamorio/issues/5440)
  closed 2022-04-25, and the pinned DR post-dates it — so the case rests on APX alone);
  **PIN-3** Pin **probe-mode** arg/return capture (original code runs native, no
  code cache — the `capture-args-returns.md` middle tier DR has no equivalent
  for); and **PIN-4** **libdft64** as an independent taint oracle diffed
  byte-for-byte against the shipped DR taint client — the `ASSERT_MATCHES_REF`
  cross-validation idiom. The reverse gaps are recorded so Pin is not mistaken
  for a superset (x86-only — no AArch64; no in-process no-IPC model; proprietary
  freeware). Every track is **fetched-and-pinned, test/oracle-only, never
  shipped** — DR's exact handling — so none adds a bindings-parity obligation.

- **AMD hardware review + follow-up plan — an adversarial pass over the AMD
  tiers in which four of nine candidate findings were REFUTED and recorded as
  such.** [2026-07-17-amd-hardware-review.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/analysis/2026-07-17-amd-hardware-review.md)
  and [amd-review-followup-plan.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/amd-review-followup-plan.md).
  Every claim resting on kernel or silicon behaviour was checked against
  **fetched primary source at pinned tags** (v6.10/v6.12/v6.14), not recall —
  one verifier caught `master` shifting line numbers mid-review and re-pinned.
  Confirmed: **Zen 3 BRS cannot open** (the probe/capture use
  `PERF_COUNT_HW_BRANCH_INSTRUCTIONS` → `0x00c2`, but `amd_brs_hw_config` demands
  raw `0xc4` *and* `sample_period > lbr_nr(16)`; `EINVAL` is then reported as
  `AMD_NOHW`, so a real Zen 3 owner is told "no AMD branch records") — code fix
  hardware-blocked per the house rule, but three docs assert the working path,
  including a **Phase 0 marked _landed_ that specifies the exact missing arm**;
  `branchsnap`'s synthetic boundary edge inflates the depth check so a complete
  `use == 15` window is spuriously `truncated` → a real re-execution
  (`n_dec = use + 1` vs a check counting hardware slots only); `IBS_MAX_RECORD`
  is **pre-callchain** (112B vs ~1032-1184B — it landed in `68b53850`, callchain
  in `a266b91` two days later and never touched it), leaving the loss heuristic
  ~10× short where `PERF_RECORD_LOST` provably cannot cover the gap →
  `lost==0 && throttled==0` **silent** loss in an honesty-first lane; and
  `asmtest_amd_freeze_available()` is dead (`nm`: one definition, zero undefined
  refs) with a **flatly false string printed to a human**. The organising finding
  is process, not code: **no CI lane exercises AMD silicon** — the one
  AMD-targeted job is named `hwtrace-privileged (PERFMON; AMD-exact self-skips
  off Zen)` and runs on `ubuntu-latest` — so the only gate is a manual checklist
  with **exactly one commit**, timestamped identically to the fix for the bug it
  still calls open, which now instructs treating a `truncated=0` **regression**
  as a known issue and gates on two signals this project measured false five days
  later. Refuted and recorded so they are not re-raised (the Matrix 3
  convention): the MSR TOS-rotation claim (transplants *Intel* architecture —
  LbrExtV2 pins `From[0]/To[0]` by register renaming and hard-codes `hw_idx = 0`
  *because* rotation is impossible; the linear read is **correct**), "IBS opts are
  unreachable" (NULL *is* the designed contract; `SYSTEM_WIDE` is tested live
  under `--cap-add=PERFMON`), the `RipInvalidChk` impact (the affected silicon
  population is empty — Family 10h hardwires `BrnTrgt = 0`, so the existing gate
  rejects it first), and the unread-IBS-regs "gap" (a dated non-goal in
  `data-flow-tracing-plan.md:94`; the original grep was a false negative on
  "address sampling").

- **`asmspy --dataflow --auto` works off AMD now: a portable software-clock
  sampler (`--sampler=ibs|sw`), with the residency hazard owned out loud.**
  Auto-targeting was AMD-IBS-only; `asmtest_swclock_survey_process` (new in
  the IBS lane's backend: `PERF_COUNT_SW_TASK_CLOCK` + `PERF_SAMPLE_IP`, no
  PMU at all) samples any-vendor hosts and VMs under the same out-of-band,
  unprivileged envelope, with availability probed BY DOING and an
  errno-carrying reason (`asmtest_swclock_unavail_reason`) from birth — the
  `--sample` lesson, applied rather than re-learned. The pure ranking half
  (`asmspy_autoregion_rank_ip`) is honest about being the WEAKER rule: an IP
  histogram measures residency, and residency's winner is often the
  entered-once-never-returns shape whose entry breakpoint can never fire —
  test_autoregion #15 pins the two rules DISAGREEING on the same behaviour.
  So the sw path ranks up to 3 candidates and `cmd_dataflow` WALKS them: a
  winner never seen entering is refused at the bounded entry wait and the
  next-ranked candidate is tried, each refusal reported. Proven live in
  `docker-cli-ibs` on the built-to-disagree `auto_victim`: sw picks
  `grind_forever`, the wait refuses it, the walk captures `entered_often`.
  9 new pure checks (31 total); both cli lanes PASS.

- **`cli/asmspy_ghash.h` + `test_ghash.c` — the graph's hash index gets the
  collision test its honest-gap note demanded.** The `--graph` engine's
  open-addressed index shipped with a measured blind spot: a probe loop that
  trusts the hash and skips the key compare emits byte-identical smoke output,
  because ≤7-node graphs in a 128-slot table never collide (on a larger graph
  that mutant silently over-merged an edge). The mechanism now lives in a pure
  header (`asmspy_gh_find`'s eq callback owns the key compare; the engine's
  node/edge lookups supply it) and a unit test brute-forces three keys into one
  slot, so exactly that mutant fails. Mutation-proven 3/3 (measured):
  accept-first-slot → 5 FAILs, idx-not-idx+1 slot encoding → 3 FAILs,
  grow-without-rehash → 1 FAIL. Engine behavior unchanged (`make docker-cli`
  PASS, `--graph` uniqueness e2e included).

- **asmspy TUI symbol picker (modes 2 and 9) gains a `Tab`-cycle sort: address ->
  hot edges -> name.** The picker used to be a flat, address-ordered list with no way
  to find "what's actually running" short of guessing a name to filter by. **Hot
  edges** reuses the exact entry-arrival rule `--dataflow --auto` picks with
  (`asmspy_autoregion_rank`, one AMD IBS-Op window) rather than minting a second
  definition of "hot" — so the picker's ranking and `--auto`'s pick always agree. An
  IBS-less host (or one where perf is locked down) falls back to address order with
  an on-screen reason rather than silently doing nothing. **Name** sorts
  case-insensitively. The row permutation is kept separate from the symtab's own
  address-sorted storage (`asmspy_symtab_at`'s binary search depends on it), mirroring
  the process picker's existing `order[]` indirection.

- **`asmspy --dataflow <pid> --auto [--module=<m>]` — trace what a process is DOING,
  no symbol needed.** Auto-targeting samples the target OUT OF BAND (AMD IBS-Op, no
  ptrace, no perturbation) for 400 ms, ranks the hottest **entry** edge — an edge whose
  target is a function's start is a direct observation of the exact event the data-flow
  producer blocks on, where the intuitive rules fail: the hottest raw edge is usually a
  mid-function loop back-edge no entry breakpoint can catch, and a PC histogram picks
  the functions entered once and never again (`main`, every event loop), which HANGS the
  producer — then hands the winner's `(base, len)` to the existing `--dataflow` engine.
  Entry-edge counts are quantitative (measured: they reproduce a victim's true 8× loop
  trip count and 2× call-site ratio). The ranking is a pure header
  (`cli/asmspy_autoregion.h`, 21-check unit test) so its correctness is covered on every
  host while the AMD-hardware live leg runs in the `docker-cli-ibs` lane. The resolver
  layers the **JIT map over the ELF symtab**, so a hot JIT'd/managed method (perf-map or
  jitdump, real size) can win the pick and names the capture — proven live against a
  perf-map victim whose only entry arrival is an anonymous-mapping function the ELF
  symtab cannot see. An idle target gets an honest refusal ("no function was observed
  being ENTERED"), not a guess; zero-size symbols cannot win on their exact-start-only
  resolution technicality; `--auto --tid` is a usage error (the sampler carries no tid,
  so pinning could only arm a breakpoint on a thread that never arrives);
  `--module=` scopes the pick with the same substring rule as `--tree --module=`.

- **`make docker-cli-ibs` — the lane that actually tests `--sample`, and honest skip
  reasons everywhere.** The plain `docker-cli` lane runs under Docker's default seccomp,
  which blocks `perf_event_open` — so every `--sample` assertion had self-skipped since
  the view landed (a green gate over an untested view, on hosts where IBS works fine).
  The new lane reruns the same image and smoke with `--cap-add=PERFMON` under the
  default profile (CAP_PERFMON bypasses `perf_event_paranoid`; no sysctl change needed),
  so the `--sample` and `--auto` blocks execute their else-branches for real on an AMD
  IBS host. Alongside it, a new public API `asmtest_ibs_unavail_reason()` carries the
  real `perf_event_open` errno out of the backend: `# SKIP --sample:` used to print an
  **empty string** precisely when the substrate probe passed but perf was blocked — the
  one case an operator on their own AMD box needed the reason most. EACCES now says
  paranoid/CAP_PERFMON, EPERM names seccomp, instead of one indistinguishable silence.

- **asmspy region view samples WORKER threads (`--trace`, TUI mode 2) + a new `--tid=<t>`
  filter.** `asmspy_engine_region` used to `PTRACE_ATTACH` the thread-group LEADER and run it
  to the region, so a function that runs on a worker thread was never observed and the view
  reported `ASMSPY_REGION_NEVER_RAN` — structurally blind to exactly the code asmspy exists to
  show, since a **managed method almost never runs on the leader**. It now SEIZEs every thread
  (`PTRACE_O_TRACECLONE`, so a thread spawned mid-run can win a later round) and races them all
  to the entry, sampling **whichever arrives first**. `--trace` gains `--tid=<t>` to pin one
  thread, matching `--stream`/`--graph`/`--tree`/`--dataflow`. The design reuses the data-flow
  tier's oracle-validated race (`dfp_seize_all`/`dfp_run_to_multi`) rather than inventing a
  second one, reimplemented over asmspy's own thread table per the standing precedent that an
  engine stays in `cli/` and leaves `src/ptrace_backend.c` untouched. `--tid` pins via a
  **per-thread hardware execution breakpoint**, not the shared int3: a shared int3 traps every
  thread, and stepping hot non-target threads back over it was measured not to converge.
  `cli_smoke.sh` asserts the worker sample, both `--tid` directions, and target survival past a
  settle; `make docker-cli` → cli-smoke PASS. Known gaps, unchanged: the any-thread entry
  breakpoint is `POKETEXT`-only (a W^X JIT page self-skips; no DR0 fallback), and the TUI has
  no thread picker.
- **CI gate for the DynamoRIO attach tier (`taint-attach`).** Increments 1 and 3-5 — cooperative
  attach, the marker-less interactive nudge, external `drrun -attach` into a running native
  process with taint capture, and K-round attach/capture/detach cycling — had docker lanes but
  **no CI job**, so five increments of capability had no regression gate while the launch-only
  taint tier next door had four. The external-attach image needs `--cap-add=SYS_PTRACE` and its
  comment still described it as the Increment-2 research probe ("a manual diagnostic, not in the
  main gate") — true when written, but Increments 4-5 were later added to the same image, so
  landed capability inherited a probe's CI posture. Both lanes now run on every push. The two
  MANAGED probes stay out by design: they record a reproducible NO-GO, not capability.

- **Data-flow tracing Phase 6 Increment 1 — `libasmtest_dataflow` shared lib + Python
  binding.** `make shared-dataflow` builds the pure analysis pipeline (L0 value sink + L1
  def-use + L2 slice + method identity + GC-move canonicalization + runtime-helper summaries;
  the emu/ptrace/DR producers stay separate tiers) into a dlopen-able shared library — the
  packaging target the language bindings consume. First bindings: **Python** (`asmtest.dataflow`,
  ctypes), **C++** (`bindings/cpp/asmtest_dataflow.hpp`, header-only), and **Node** (koffi,
  `bindings/node/dataflow.js`) all wrap the pure GC-move canonicalizer `asmtest_gcmove_canon`
  and the tiered-re-JIT-aware method resolver `asmtest_method_resolve_pc`; `make
  dataflow-{python,cpp,node}-test` build and run their TAP suites (8 / 16 / 16 checks,
  mirroring the C `test_dataflow_gcmove` / `test_dataflow_method` semantics). Each self-skips
  cleanly when the lib is not built, so none reddens a general binding job. A new `dataflow` CI
  job builds the lib and runs the Python + C++ bindings on every push (the Node binding runs in
  the docker bindings lane). All three host bindings (Python, C++, Node) additionally wrap the full L0->L1->L2 pipeline
  (`ValueTrace`: value-trace build -> def-use -> forward/backward slice), round-trip-validated
  against the C semantics (register move chains, load-after-store through memory, no spurious
  cross-links — which also validate the 13-field at_val_rec_t marshalling). The remaining seven language bindings are later increments.

- **Live GC-move detection feed for the data-flow tier (`GcMoveMap`, .NET).** An in-proc
  `EventListener` on the CoreCLR runtime provider that enables the GCHeapSurvivalAndMovement
  keyword and captures `GCBulkMovedObjectRanges` from a **compacting** GC — the live source
  the pure GC-move canonicalizer (`asmtest_gcmove_canonicalize`) was built to consume.
  Validated via `make docker-hwtrace-dotnet`: an induced compacting gen2 collection is
  captured as 11 events / 20474 moved ranges (suite 169 → 177). Honest scope: in-proc
  `EventListener` surfaces the reliable scalar range **count** but not the manifest
  struct-array `Values` payload, so the concrete `{old_base, new_base, len}` triples that
  drive the canonicalizer end-to-end are **deferred** to a raw EventPipe/nettrace path — the
  keyword, compacting-GC inducement, and listener wiring (the uncertain parts) are proven.

- **Data-flow tracing Phase 5 Increment 1 — DynamoRIO in-band L0 value producer
  (`src/dataflow_dr.c`, `src/dataflow_dr_client.c`).** The in-band, whole-process analog of
  the scoped ptrace L0 producer: a DynamoRIO client instruments a target under real DR and
  captures per-instruction operand values into the same `asmtest_valtrace_t` sink, so the L1
  def-use builder and L2 slicer work unchanged on an in-band capture. Cross-validated against
  the emulator L0 oracle on a shared fixture (in-band def-use edges and forward/backward
  slices equal the oracle's), validated live via `make docker-drtrace` (`dr-valtrace-test`
  14/14; DR is a software DBI engine, so the lane needs no privilege or special hardware).
  Self-skips (exit 0) without `DYNAMORIO_HOME`. Store *values* and RIP-relative/segmented/VSIB
  memory EAs are deferred to a later increment (the current fixture avoids them); DR-side taint
  shadowing and whole-process breadth are also later increments.

- **Data-flow tracing Phase 4 Increment 3 — runtime-helper summary edges
  (`src/dataflow_helpers.c`).** `asmtest_defuse_build_summarized` recognizes a .NET
  runtime-helper call in an L0 value trace (via the Increment-1 method resolver → a helper
  table matched by name/prefix) and collapses the helper run into a **summary node** — emitting
  only its declared input reads and output writes and dropping the body — so caller dataflow
  connects *across* the helper (arg def → summary → return use) without instrumenting CoreCLR
  internals. Supports reg→reg helpers (allocation, generic-dict lookup) and a `MEM_AT_REG`
  write-barrier output. Conservative by construction: an **unrecognized** call is descended
  normally, never given a fabricated edge. Pure, host-independent suite `test_dataflow_helpers`
  (36 checks).

- **Data-flow tracing Phase 4 Increment 2 — GC-move canonicalization
  (`src/dataflow_gcmove.c`).** A pure, host-independent transform (`asmtest_gcmove_t`,
  the shape of EventPipe `GCBulkMovedObjectRanges`: `old_base`/`new_base`/`len`) that remaps
  memory addresses across a heap compaction to a stable canonical identity, so a managed
  value's def-use survives the move without pre/post-move false aliasing — the plan's Phase-4
  exit criterion. Synthetic suite `test_dataflow_gcmove` (26 checks) proves the exact trap:
  a def at the old address and a use at the new address unify into one object, while an
  unrelated object that later reuses the freed old address does **not** alias it. The live
  EventPipe feed is a later increment.

- **IBS-Fetch front-end coverage lane (AMD IBS statistical lane Phase 7).** A second AMD IBS
  producer beside the retired-op edge sampler: `ibs_fetch` (PMU type 10) samples *fetch*
  addresses (front-end / i-cache / ITLB view). A pure, host-independent decoder turns one
  `PERF_SAMPLE_RAW` fetch record into `{fetch_addr, valid, complete, icache_miss, itlb_miss,
  latency}` (unit-tested with synthetic records on every CI host), plus an availability probe
  and a headless fetch-coverage survey with honest throttled/lost provenance, self-skipping
  off IBS/permission. Kept **fully internal** (`src/ibs_backend.h`) — no public
  `asmtest_ibs_*` surface added, so no binding flag day. Verified live on Zen 5.

- **Data-flow tracing Phase 4 Increment 1 — PC→method-identity+version resolver
  (`src/dataflow_method.c`).** A pure, host-independent resolver that labels each step of
  an L0 value trace with its owning method + version from a jitdump/perf-map-shaped
  method-map, correctly handling tiered re-JIT (newest `code_index` wins for an address; a
  re-JIT to a new address is a new version) — the managed-taint prerequisite. Synthetic
  suite `test_dataflow_method` (29 checks, incl. the moved-re-JIT version distinction);
  the hard GC-move canonicalization is deferred to a later increment.

- **`hwtrace-privileged` CI job + AMD hardware-validation doc.** A CI job exercises
  `make docker-hwtrace-privileged` so the `--cap-add=PERFMON` lane can't bitrot (the
  AMD-exact tests self-skip on GitHub's non-AMD runners — honest by design; it lights up
  on a future AMD runner). `docs/internal/amd-hardware-validation.md` documents the manual
  pre-release validation on real Zen 3+/Zen 5 silicon — closing the gap that let the
  `call_auto` LBR truncation bug hide (the exact AMD paths never ran in CI).

- **Size-negotiated hwtrace options ABI + machine-readable status surface + escalation
  mechanism, across all ten bindings (the AMD-followup API flag day — Phases 1, 3, and
  F22/F26/F37).** `asmtest_hwtrace_options_t` now leads with a `size_t struct_size` the
  caller sets (the `INIT_OPTS` idiom, or explicitly after a zero-fill); `asmtest_hwtrace_init`
  copies `min(struct_size, sizeof)` and zero-fills the tail, so an older/newer caller is
  never read out of bounds — and a caller that fails to self-describe (`struct_size == 0` or
  too small to reach `backend`) is rejected with `EINVAL` rather than having a set field
  silently dropped. New `asmtest_hwtrace_status()` (available / code / stage / probe errno /
  `perf_event_paranoid` / reason) and `asmtest_hwtrace_perf_event_paranoid()` distinguish
  **`ASMTEST_HW_EPERM`** (substrate present, permission denied — e.g. AMD LBR on an
  unprivileged `paranoid > 2` host) from `EUNAVAIL` (missing silicon), backed by one shared
  classifier so `status()` and `skip_reason()` cannot drift. `asmtest_trace_choice_t` grew a
  `mechanism` field (`HW_BRANCH` / `TF_STEP` / `MSR_LBR` / `BLOCKSTEP` / `PER_INSN` / `DBI` /
  `EMULATOR` / `STATISTICAL`) plus `ASMTEST_FIDELITY_STATISTICAL`, so `trace_call_auto`
  reports which rung actually won and a statistical result is structurally unmistakable for
  an exact one. All ten wrappers mirror the new layouts and wrap the new calls; the parity
  gate passes with zero allow-list changes. Suite 358 → 383 (ABI guard, status incl. the
  live-EPERM assertion, mechanism).

- **Data-flow tracing gains a live scoped ptrace L0 producer (Phase 3 — real values,
  out of band).** `src/dataflow_ptrace.c` single-steps a routine (fork+`PTRACE_TRACEME`, or
  `PTRACE_SEIZE` attach to a live victim that survives detach) and emits the **same**
  `asmtest_valtrace_t` stream the Phase-0/1 analyzers consume, so def-use + slicing work
  unchanged on live captures — reading each step's registers (`GETREGS`, `GETFPREGS` for
  XMM, `NT_X86_XSTATE` for 256-bit YMM) and the memory its operands touch. Cross-validated
  edge-for-edge and value-for-value against the emulator L0 oracle; RIP-relative effective
  addresses resolve against the next instruction (a bug an adversarial verify caught before
  merge), gs-based and wide-vector operands captured. `dataflow-test` 26 → 36.

- **Data-flow tracing tier, Phases 0–2 (`include/asmtest_valtrace.h`, `make dataflow-test`)
  — the CI-runnable milestone of the data-flow plan.** Phase 0: the shared L0 value-trace
  sink (`asmtest_valtrace_t`: caller-owned buffers, append/stash-wide/truncate discipline
  mirroring `asmtest_trace_t`) plus the Capstone operand read/write-set enumerator
  (explicit register + memory operands with base/index/scale/disp/segment, and the implicit
  ones — `eflags` writes, `rsp` read+write on push/pop — via `cs_regs_access`). Phase 1: the
  L1 def-use graph over a recorded value trace (register moves and load-after-store memory
  edges) and the L2 forward/backward slicer on top of it. Phase 2: the emulator (Unicorn)
  L0 producer — replay a routine under `uc_hook` instrumentation and emit the value trace
  the pure phases analyze; validated live (per-step values, def-use edges, and both slice
  directions asserted against a hand-traced fixture). Pure phases run on every host;
  the emulator cases self-skip without Unicorn. New `mk/dataflow.mk`; suites
  `test_dataflow` / `test_operands` / `test_dataflow_emu` (53 checks). The known
  raw-address aliasing false positive (pre-GC-canonicalization) is asserted AS a false
  positive, per the plan's Phase 4 note.

- **`asmspy` reads binary jitdump files — the bytes-accurate, tiered-recompile-aware JIT
  symbol source (asmspy plan Theme A).** A `jit-<pid>.dump` reader (LE header +
  `JIT_CODE_LOAD` / `JIT_CODE_MOVE` records; unknown record types skipped via
  `total_size`; a truncated in-flight tail ends the parse keeping what's whole) is now
  **tier 1** of the JIT resolve chain — discovered the way perf does (a mapped marker in
  `/proc/<pid>/maps`, then `/tmp` and the target's cwd), parsed ahead of the text perf-map
  (tier 2, the LCD), re-JIT and code-motion aware (newest `code_index` wins, `CODE_MOVE`
  relocates). Same rate-limited refresh-on-miss discipline as the perf-map path. Hardened
  against hostile files (bounded name reads, zero/short `total_size` rejection — fuzzed
  under ASan/UBSan). Also new: `--tree --json/--dot` exports mirroring the `--graph`
  exporters, the extracted call-graph sort comparator (`cli/asmspy_graphsort.h`) with its
  ordering/tiebreak unit test, and a `jitdump_victim` end-to-end smoke.

- **.NET `AsmTrace.Window` captures methods JIT'd *mid-window* — the sibling-thread live
  JIT publish (extensions plan E3), closing the deep-BCL gap.** `JitMethodMap.SetPublishChannel`
  now starts a dedicated, never-stepped publisher thread: the `MethodLoadVerbose` callback
  only enqueues `(base,len)` onto a lock-free queue (publishing inline could fire on the
  single-stepped thread and re-enter the runtime under step — the observed SIGABRT that
  kept this OFF), and the sibling drains it and P/Invokes each record into the shared
  address channel while the window runs. Stop **joins** the publisher before the channel
  is freed (no use-after-free window); the §E1 hybrid keeps live publish off by design
  (it must capture only the surveyed hot slice). New `WholeWindowScope.LiveJitPublished`
  counter; suite grows 161 → 169 checks including a ptrace-free mechanism test (native
  ring-head readback) and a mid-window-JIT integration case (52 records live-published
  in the docker lane).

- **Env-gated debug logging for the hwtrace/AMD tier (`src/debug.{c,h}`, followup Phase 4 /
  F32).** `ASMTEST_HWTRACE_DEBUG=1` (or `ASMTEST_AMD_DEBUG=1`) turns on stderr tier
  diagnostics; unset costs one cached `getenv` per process. Covered by two suite checks
  (silent when off, emits when on).

- **Host-independent synthetic-ring tests for the AMD branch-stack parse (review F43/F44).**
  The `hwtrace_end_amd` ring-parse now has an internal, linkable entry
  (`asmtest_amd_ring_parse_decode`) driven by crafted `PERF_RECORD_SAMPLE` buffers — the
  nr-clamp / LOST / Tier-A-vs-Tier-B logic and `amd_span_decodable`'s dropped-jmp follow
  finally run on every CI host, AMD or not (+368 lines in `examples/test_hwtrace.c`,
  suite 341 → 358).

- **AMD LBR window-reach tuning guide (`docs/guides/tracing/amd-lbr-tuning.md`, review
  F47 / followup Phase 10)** — what bounds a 16-deep LbrExtV2 window, the sizing/splitting
  levers, what `truncated` means and how it is reported, when the statistical IBS lane is
  the better tool, and the privileged-vs-unprivileged lanes including
  `perf_event_paranoid`.

- **`asmspy --sample` + TUI mode 7 — a live statistical hot-edge view, out of band (IBS lane
  Phases 2–3, the flagship deliverable).** Built on the new
  `asmtest_ibs_survey_process(pid, ms, opts, out)`: whole-process IBS-Op coverage that opens
  one perf event + ring per thread of the target (enumerating `/proc/<pid>/task`, with one
  mid-window rescan for threads spawned after start; the residual born-and-died-in-window
  race and the privileged system-wide remedy are documented, not hidden) and merges
  everything into one hot-edge histogram. `asmspy_engine_sample` resolves both endpoints of
  each edge through the existing ELF-symtab → JIT-perf-map chain, so managed Node/.NET/Java
  frames are named. Headless `asmspy --sample <pid> [ms] [--json]` prints the histogram —
  `count  from -> to` with `[misp N%]`/`[ret]` tags and honest `branch/total samples` /
  `throttled` provenance — or machine-readable JSON; TUI menu item **"7) Hot edges (sample)"**
  shows the same table live, pausable + scrollable + Tab-sortable (count / mispredicts).
  Unlike the stream/graph/tree views this **never attaches ptrace and never single-steps** —
  the target runs at full speed — making it the only rich view that is safe on a live JIT,
  exactly the targets single-stepping can crash. Self-skips (`# SKIP`, exit 0) off IBS; new
  busy victim `cli/sample_victim.c` + a `--sample` smoke in `cli/cli_smoke.sh`; the TUI view
  is driven end-to-end through a pty harness. Verified live on Zen 2: both surfaces name the
  victim's hot back-edge without perturbing it.

- **IBS-Op fallback for the AMD whole-window statistical survey (IBS lane Phase 4; fixes the
  AMD review's F6).** On Zen 2 the branch stack (BRS / LbrExtV2) does not exist, so
  `asmtest_hwtrace_sample_window_amd` (and its begin/end split) returned `EUNAVAIL` on the
  one AMD host class that most needs a crash-proof survey. New internal window primitives
  (`asmtest_ibs_window_begin`/`_end`, `src/ibs_backend.h`) arm IBS-Op on the calling thread
  around the caller's window body, reusing the channel/drain/edge-hash machinery; on
  branch-stack `perf_open` failure — or when `ASMTEST_FORCE_IBS_SURVEY` is set, for
  cross-validation on Zen 3+/CI — the survey delegates to IBS and flattens each sampled
  edge's target into the `ips[]` endpoint histogram weighted by count, so the caller's
  bucket-by-method hotness view is unchanged in shape. Purely **STATISTICAL**: a separate
  producer that never feeds the exact `insns[]`/`blocks[]` parity cascade; the branch-stack
  path is byte-identical when the stack is present and the env unset. Covered by
  `test_amd_sample_window_ibs` (self-skips off IBS); verified live on Zen 2 (~468/468
  endpoints in the hot loop, full `hwtrace-test` 341/341).

- **`asmspy` gained three whole-process structure views and a per-thread lens since the
  entry below.** A **call graph** (TUI mode 4, headless `--graph <pid> [n]
  [--sort=invocations|fanout]`): every `call` attributed caller→callee across all threads,
  aggregated per function, with `--json` export (nodes *and* `{caller,callee,count}` edges,
  addresses as `0x` strings) and `--dot` emitting a Graphviz digraph (kind-coloured nodes,
  count-labelled edges) ready for `dot -Tsvg`. A **call tree** (TUI mode 5, headless
  `--tree <pid> [n]`): the same feed with nesting/order preserved, indented by depth, in a
  two-pane TUI. A **process/thread topology** view (headless `--procs`, plus an `F2`
  flat-list ↔ tree toggle in the process picker): the process forest drawn with `├─`/`└─`/`│`
  box glyphs, threads then child processes nested under each process. A **`--tid=<t>`
  filter** for `--stream`/`--tree`/`--graph` seizes and steps *only* that thread, leaving
  the rest of the process at full speed. The call-graph and region (assembly & funcs) TUI
  views now **pause + scroll** like the log views (`space` freezes a stable snapshot,
  arrows/PgUp/PgDn/Home/End move, `Tab` switches pane focus in the region view). And all
  single-step engines resolve **JIT frames through the runtime's perf map**
  (`/tmp/perf-<pid>.map` — Node/V8 `--perf-basic-prof`, .NET `DOTNET_PerfMapEnabled=1`,
  OpenJDK perf-map-agent), refreshed rate-limited on miss so a compiling JIT keeps getting
  named: managed frames render `name [jit]` in the stream/tree and `[JIT]`-tagged internal
  nodes in the graph.

- **Statistical AMD IBS-Op tracing lane (`asmtest_ibs.h`, `src/ibs_backend.c`) — Phases 0–1.**
  A new, self-contained *statistical* trace producer for AMD hosts where every branch-**stack**
  facility is absent (Zen 2 has no BRS / LbrExtV2, so every exact hwtrace backend self-skips and
  the machine falls back to ~1000×-slower single-stepping). IBS-Op (Instruction-Based Sampling)
  is the one branch-tracing facility this silicon has: it tags a retired op per NMI window and,
  for taken branches, delivers both the source (`IbsOpRip`) and target (`IbsBrTarget`) — a
  **statistical `from → to` control-flow edge**, sampled **out of band, against a running thread,
  unprivileged** (the kernel `swfilt` bit makes user-only sampling open at
  `perf_event_paranoid=2`) and **without perturbing the target** — exactly the case the
  single-step views are dangerous on (a live JIT / managed runtime). It needs **no external
  library** (raw `perf_event_open` + a pure decoder). Public surface:
  `asmtest_ibs_available()` / `asmtest_ibs_skip_reason()` (the full AMD/IBS/`BrnTrgt`/`swfilt`
  detect-and-skip chain), `asmtest_ibs_decode_op()` (a **pure, host-independent** decode of one
  IBS-Op `PERF_SAMPLE_RAW` record into an edge — unit-tested with synthetic records on **every**
  CI host, AMD or not), and `asmtest_ibs_survey_pid()` (attach IBS-Op to one thread, drain for
  N ms, return an aggregated hot-edge histogram sorted by count with honest provenance —
  `samples` / `branch_samples` / `lost` / `throttled`). **INVARIANT:** statistical only — it can
  prove a block *was* seen, never that one was *not*, so it never feeds the exact
  `insns[]`/`blocks[]` parity contract; it is a separate diagnostic producer, not a member of the
  exact-trace cascade. Built into `libasmtest_hwtrace`; validated by `make ibs-test` (also folded
  into `make hwtrace-test`) and the containerized `make docker-hwtrace-ibs`; probe binary
  `examples/ibs_probe.c`. The live path is validated on an AMD Ryzen 9 4900HS (Zen 2, kernel
  6.14): the test captures a spin loop's back-edge out of band from a separate thread. Plan:
  `docs/internal/plans/zen2-ibs-tracing-plan.md`. Phases 2–4 landed subsequently — the
  whole-process survey, the `asmspy --sample` view (headless + TUI mode 7), and the
  statistical survey fallback; see their own entries above.

- **`asmspy` — an interactive process tracer (new `cli/` subsystem, Linux x86-64)** — a small
  ncurses front-end over the out-of-process (`ptrace`) tracer: attach to any running process
  and watch it live and out of band. Three live views: **syscalls with data** (a mini `strace`;
  every syscall named from a table generated against the host's own `<sys/syscall.h>`, `read`/
  `write` buffers and path arguments decoded, `read`/`write` file descriptors resolved to
  their path/socket/pipe via `/proc/<pid>/fd` like `strace -y`, decoded strings split into
  their own pane; the syscall stream **and** the whole-process instruction stream follow
  **every thread** of the target — `PTRACE_SEIZE` of all tasks plus `PTRACE_O_TRACECLONE` for
  threads spawned later, each line tagged `[tid]` when more than one is followed, and (for
  syscalls) entry/exit read from `PTRACE_GET_SYSCALL_INFO` so seizing a thread mid-syscall
  never desyncs), a
  chosen **function's assembly** with
  per-instruction execution **heat counts** plus its callees **ranked by call count**
  (resampled each time the target calls it), and a **whole-process live instruction stream**
  (every instruction as it executes, resolved to its function). The two log feeds
  (syscall log, live stream) **pause + scroll back** through their history (`space` to
  freeze, `↑`/`↓`/`PgUp`/`PgDn`/`Home`/`End` to move, `End`/`space` to resume the tail),
  and scrollback survives target exit. The process picker filters
  as you type and sorts by **pid, recent CPU activity, or string-scan density** (`Tab`
  cycles, `r` rescans, `b` navigates back). Every view is also a headless subcommand for
  scripts and CI: `--list [active|scan]`, `--syms <pid> [filter]`, `--log <pid> [n]`,
  `--trace <pid> <sym|0xADDR[:LEN]> [n]` (an explicit `0xADDR:LEN` range reaches stripped
  code or a JIT region no symbol covers), `--stream <pid> [n]`; a negative `n` runs until the
  target exits, and malformed arguments are rejected up front. Built by `make cli` (needs libncurses +
  Capstone; self-skips with guidance) or containerized via `make docker-cli`
  (`Dockerfile.cli`); carries its own `/proc` lister and ELF `.symtab`/`.dynsym` function
  resolver. End-to-end headless smoke (`cli/cli_smoke.sh`, `make cli-smoke`) drives all five
  subcommands against the example victims and is gated in CI (`cli` job). Guide:
  `docs/guides/tracing/asmspy.md`.

- **`asmtest_trace_call_auto` — the auto-escalating, call-owning cross-tier trace, now in
  all ten bindings.** A single entry point that owns the invocation and traces a native
  routine under the fastest exact tier, then **automatically escalates to a ceiling-free tier
  when the trace comes back `truncated`** — walking the ladder *fast HWTRACE backend →
  MSR-direct AMD-LBR rung → BTF block-step → per-instruction single-step* until the capture is
  complete (or the tiers are exhausted, honestly flagged). This closes the "arm → detect
  truncation → re-resolve → re-run" loop that was previously only a documented idiom. Landed
  C-first (`src/trace_auto.c`, the MSR rung folded in later), then wrapped
  in every binding (python/cpp/rust/zig/node/java/dotnet/ruby/lua/go), removing its former
  `ALL` parity exemption. `*used` reports the tier that produced the final trace, so a caller
  can see whether escalation fired. Covered by `test_call_auto*` in the hwtrace suite.

- **`asmtest_hwtrace_arm_tid` wrapped in the remaining seven bindings** (go, java, lua, node,
  ruby, rust, zig) — the §0.2 thread-scope assert accessor (the OS thread id that armed the
  active hardware-trace capture, `-1` if none) was python/cpp/dotnet-only; it is now surfaced
  in all ten bindings with an idiomatic accessor (`HwTrace.armTid()` / `arm_tid` /
  `HwTraceArmTid()` per language), closing its seven per-binding parity exemptions. Each
  wrapper was built and its hwtrace test suite run green in the per-language docker lanes.

- **Whole-window attribution, version-aware render, and async-hop merge in the Node and Java
  bindings** — dotnet-parity Phase 2, the remaining CI-runnable clusters. Wraps six .NET-lead
  C symbols across both bindings:
  - **`CodeImage.renderVersioned(when, trace)`** (`asmtest_hwtrace_render_versioned`) — disassemble
    a trace's absolute addresses against a code-image timeline AS OF a capture sequence, not live
    memory. Version-*aware* (unlike `render_window`): tracking a region as `add` then rewriting it to
    `sub` renders `add` at the old sequence and `sub` at the new. Plus `NativeTrace.appendInsn`
    (wraps `trace_append_insn`, a non-tier symbol) to build such an absolute-address trace.
  - **`HwTrace.regionName` / `symbolizeBuckets` / `attributeWindow`** (`asmtest_hwtrace_region_name`
    / `_symbolize_bucket` / `_attribute_window`) — whole-window noise attribution: reverse-resolve an
    address to its mapped-region name, bucket a list of IPs by JIT symbol (perf-map) or region, and
    attribute a live whole-window capture's absolute addresses to caller-named regions first
    (so two identical-byte leaves in distinct mappings split into separate buckets — what
    symbol/disasm attribution cannot do). `AddrChannel`-free; range classification, no Capstone.
  - **`HwTrace.stitchHandles(hops, …)`** (`asmtest_hwtrace_stitch_handles`) — the §D0.4 async-hop
    merge: order N already-captured hop traces by `seq` and concatenate into one logical trace with
    per-hop slice bounds. Host-independent (pure merge — runs on every lane, arm64 included); the
    hops must outlive the call (shallow-copy, not duplicated). `asmtest_hwtrace_stitch` (the C core)
    stays binding-internal.
  - Struct marshalling is pinned to the exact SysV layouts (`bucket_t` 136 B, `slice_bound_t` 32 B,
    `named_region_t` 80 B) and cross-checked against the dotnet `[StructLayout]`s. Validated in the
    `docker-hwtrace-node` / `-java` lanes against the C oracles (`test_render_versioned`,
    `test_symbolize_bucket`, `test_wholewindow_buckets`, `test_stitch_slices`). All six `ALL`
    allow-list lines stay (seven-eight bindings still don't wrap them); `trace_append_insn` is a
    non-tier symbol (ungated).

- **Crash-proof WHOLE-WINDOW out-of-process capture in the Node and Java bindings** —
  dotnet-parity Phase 2, increment 3, the out-of-process analog of the in-process `window()`
  form (which single-steps the calling thread and is fatal for arbitrary managed code). Wraps
  `asmtest_ptrace_trace_window_call` (`Ptrace.windowCall` / `HwTrace.ptraceTraceWindowCall` —
  fork-internal: a forked child runs the window frame and is stepped, so it asserts
  unconditionally on any ptrace lane) and `asmtest_hwtrace_stealth_trace_windowed`
  (`HwTrace.stealthWindow` — a helper child reverse-attaches and steps the calling thread's
  window body out of band, mirroring dotnet's `AsmTrace.Window`; self-skips on a refused
  reverse-attach), plus the five `asmtest_addr_channel_*` FFI shims behind a new `AddrChannel`
  class. Pre-publish the code regions the window frame calls into (its leaves/methods) on the
  channel; the capture records the frame **plus** every published region as ABSOLUTE addresses
  (classify by range — no Capstone), stepping over everything else. Validated in the
  `docker-hwtrace-node` / `-java` lanes against the C oracle's driver-blob ceremony (a 35-byte
  frame calling two 7-byte leaves): result `m2(7,3)==4`, driver + both leaves recorded in call
  order, complete. The `ALL` allow-list lines for both windowed symbols stay (seven bindings
  still don't wrap them); the addr_channel shims live in a non-tier header (ungated).

- **Crash-proof out-of-process stealth capture (`stealthTrace`) in the Node and Java bindings**
  — dotnet-parity Phase 2, increment 2. `HwTrace.stealthTrace(code, a, b)` (Node) /
  `HwTrace.stealthTrace(NativeCode, long...)` (Java) wrap `asmtest_hwtrace_stealth_trace`: a
  helper child reverse-attaches (`PR_SET_PTRACER` + `PTRACE_SEIZE`) and single-steps the native
  leaf **out of band**, so **no `EFLAGS.TF` is ever armed on the runtime's own (V8 / JVM)
  thread** — the crash-proof counterpart to the in-process `callScoped`/`window` forms, mirroring
  dotnet's `AsmTrace.Method(..., outOfProcess: true)`. The `result` is exact (the helper reads
  the caller's RAX at the `ret`); the instruction stream is best-effort over a live runtime (its
  async signals can truncate the per-instruction walk — honestly reported via `truncated`), so
  the tests assert the exact `[0,3,6,c,11]` stream only when not truncated. Validated in the
  `docker-hwtrace-node` / `-java` lanes; self-skips cleanly where a Yama `ptrace_scope` refuses
  the reverse-attach.

- **AMD LBR Zen 4/5 coverage: slot-efficient branch filtering (#2B), period-spaced stitch
  validation (#2A), and single-exit snapshot-by-default (#3).** Three improvements that
  stretch how much of a routine each 16-deep AMD branch-record window reconstructs, all
  respecting the silicon ceiling and the "never emit corrupt as complete" rule.
  - **#2B slot-efficient branch filtering (opt-in, SCOPE-SAFE).** New
    `asmtest_hwtrace_options_t.branch_filter` (default 0 = `PERF_SAMPLE_BRANCH_ANY`,
    unchanged). Nonzero requests a reduced HW filter (`COND | IND_JUMP | ANY_CALL |
    ANY_RETURN`) that drops only the **direct unconditional `jmp`** — its target is
    statically decodable, so it need not consume a scarce LBR slot — and the reconstructor
    follows it from the region bytes for a **byte-identical** trace over a longer window.
    Dropping direct *call* too was deliberately rejected (an out-of-region-callee return
    strands the pre-call in-region code — a silent-corruption risk). The decoder is
    **unified/no-flag**: `amd_replay` follows a dropped jmp only when one appears
    mid-straight-line-walk, which under the default full filter can never happen (a taken jmp
    is the recorded `from`), so the follow path is provably dead code on the tested default.
    New primitive `asmtest_disas_is_uncond_jump`; the capture retries the full filter on
    `EOPNOTSUPP`/`EINVAL` so the tier stays available. Applies to both the sampled and the
    deterministic-snapshot paths (the statistical WindowHot survey keeps `BRANCH_ANY`).
    Host-independently validated (`test_amd_reduced_filter` F1–F5: dropped-jmp equivalence,
    back-edge-cycle termination, region-exit truncation, chained follow); two independent
    adversarial reviews confirmed the classify/follow logic exhaustive over every x86-64 CTI.
    **Live-validated + reach-measured on Zen 5** (Ryzen 9 9950X, `test_branchsnap`): the
    deterministic snapshot with `branch_filter=1` follows a dropped jmp to its target block on
    real LbrExtV2, and reconstructs **1.86× more executed instructions per 16-deep window**
    (65 vs 35) on a loop whose body has a direct jmp plus a conditional back-edge.
  - **#2A period-spaced Tier-B stitching — host-independent validation + documented caveat.**
    `test_amd_stitch_period_spaced` proves the landed `lbr_period` path stitches
    period-spaced (P=4) windows of a **distinct-edge** path back to the exact sequence, and
    asserts the flip side: a **self-similar loop silently undercounts** under `period>1` (the
    smallest-overlap heuristic can't tell 1 iteration from P) — which is why the default stays
    `lbr_period=0` (period=1, universally exact). **Live-measured on Zen 5**
    (`test_amd_reach_period`): confirms the finding on real hardware — `period=4` reconstructs
    *fewer* instructions than `period=1` (231 vs 297) on a loop, since **every loop is
    edge-self-similar**, so period-spacing's reach benefit is confined to (inherently short)
    distinct-edge paths, not loops.
  - **#3 deterministic snapshot by default for single-exit regions.** `hwtrace_begin_amd`
    now selects the Phase-3 boundary snapshot by **default** on the supporting substrate
    (`amd_lbr_v2` + `perfmon_v2` + Linux ≥ 6.10), but **only when the region has a lone ret**
    (`amd_last_ret_off` now counts rets) — the one exit breakpoint is then guaranteed hit, so
    the common small routine gets deterministic capture with no richest-window guessing.
    Multi-exit routines (which an earlier ret could make the breakpoint miss) keep the sampled
    path; explicit `opts.snapshot` is honored for any region and every arm failure falls
    through to sampling. Validated: `docker-hwtrace-amd` (328 decoder checks green) and
    `docker-hwtrace-codeimage` (branchsnap marker path green on the Ryzen 9 9950X). Only
    `bindings/dotnet` mirrors the new `branch_filter` field (matching the shipped `lbr_period`
    posture); the field is an ABI-safe tail append (struct stays 48 bytes).

- **Whole-window scope (`begin_window`/`end_window`/`render_window`) in the Node and Java
  bindings** — the region-free, empty-ctor `using (new AsmTrace())` §Z1 substrate (Phase 2,
  increment 1 of the dotnet-parity roadmap). `HwTrace.window(fn)` (Node) /
  `HwTrace.window(Runnable)` (Java) arm a single-step capture on the calling thread with NO
  registered region, run the body, disarm, and render the executed **absolute** addresses
  from live memory — returning `{path, truncated, insns[]}`. It is HONEST-BUT-NOISY by design:
  single-stepping the managed runtime records everything between begin and end (the FFI
  dispatch + runtime), so the traced routine's own addresses appear as a *subset*. A single
  V8-dispatched call runs ~100k instructions (captured cleanly, subset verified); a HotSpot +
  FFM call exceeds the single-step whole-window's internal `SS_WINDOW_CAP` (1<<20), so the
  Java capture honestly reports `truncated` (best-effort). Validated in `docker-hwtrace-node`
  / `-java`. The `begin_window`/`end_window`/`render_window` `ALL` exemptions stay in
  `scripts/bindings-parity-allow.txt`, now consumed by the seven bindings that don't wrap them.

- **`call_scoped` — a registry-free traced native call — now in ALL TEN bindings.** The
  Python/Ruby/Node/Java bindings shipped it first; the remaining five (C++, Rust, Zig, Lua,
  Go) now wrap it too. Each wraps `asmtest_hwtrace_call_scoped_ex` +
  `asmtest_hwtrace_render_scope`: arm, call the native leaf, and disarm entirely in native
  code — a tighter window than the `scope` form (whose FFI dispatch of `code.call` is
  stepped, though region-filtered) — returning the call's result, the executed body's
  disassembly, and the truncation bit in one step. Registry-free, so it is safe in a tight
  loop (no `MAX_REGIONS` exhaustion). `HwTrace.call_scoped(code, *args)` (Python/Ruby),
  `HwTrace.callScoped(code, …args)` (Node), `HwTrace.callScoped(code, long…)` (Java),
  `HwTrace::callScoped(code, args…)` (C++), `HwTrace::call_scoped(&code, &[args])` (Rust),
  `HwTrace.callScoped(&code, args)` (Zig), `HwTrace.call_scoped(code, ...)` (Lua),
  `CallScoped(code, args…)` (Go); each returns `{result, path, truncated, rc}` and each is
  validated in its Docker lane (result 42, body renders to `ret` in 5 insn lines, a 40-call
  loop with no exhaustion). Struct-by-value for the 8-byte `asmtest_hwtrace_scope_t` handle
  is native in the five new bindings (C++ POD, Rust `#[repr(C)]`, Zig `callconv(.C)`, LuaJIT
  FFI, cgo) — no packing, unlike the Ruby/Java bridges. With all ten now wrapping the pair,
  the `ALL` exemptions for `call_scoped_ex`/`render_scope` leave
  `scripts/bindings-parity-allow.txt`.

- **§D0.4 async-hop stitching now has a LIVE producer — `AsmStitchedTrace` (.NET).** The
  shipped `asmtest_hwtrace_stitch` merge core previously had no live producer (only
  synthetic-slice host tests). New `asmtest_hwtrace_stitch_handles(traces[], scope_ids,
  seqs, tids, versions, n, out, bounds, nbounds)` (`src/hwtrace.c`) is the binding-facing
  bridge — it merges N already-captured trace *handles* (the slice struct embeds heap
  pointers a binding can't marshal by value). On top of it, the .NET `AsmStitchedTrace`
  carries an `AsyncLocal<scopeId>` across `await`/thread hops and feeds each hop's
  managed-safe lazy-arm capture to the core, so one logical operation traced across a
  real `Task.Run` thread hop stitches its per-thread slices in seq order. Each hop uses
  the new **registry-free** `asmtest_hwtrace_call_scoped_ex` (`[base,len)` direct, no
  `MAX_REGIONS` slot) so a long-running operation with many hops cannot exhaust the fixed
  32-slot region table process-wide. Validated on the **single-step** tier (no Intel PT
  needed): host `test_stitch_handles` / `test_call_scoped_ex` (incl. a 64-call
  no-exhaustion check) and the .NET lane (scope id flows across the hop; two
  different-thread hops merge with correct bounds; 40 operations all capture).

- **FP shim family for the lazy-arm scope — `(double…)->double` methods trace in-process.**
  `asmtest_hwtrace_call_scoped_fp` (`src/ss_backend.c` / `src/hwtrace.c`) dispatches a
  homogeneous double signature through the SysV FP ABI (xmm0..7 args, 0-8 arity). The
  .NET `AsmTrace.Method(...).Invoke` now tries the integer `(long…)->long` shim, then the
  FP family, before falling back out-of-process — so a `double`-signature method is
  captured in-process instead of degrading. Host-tested (`test_call_scoped_fp`) and on
  the .NET lane. See
  [managed-singlestep-lazy-arm-plan.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/plans/managed-singlestep-lazy-arm-plan.md).

- **Managed single-step is now safe by construction — `AsmTrace.Method()` lazy-arms
  only the method body.** New `asmtest_hwtrace_call_scoped(name, fn, args, nargs,
  result, out)` (`src/ss_backend.c` / `src/hwtrace.c`) arms the single-step window,
  calls the target through the SysV integer ABI, and disarms — all in native code —
  so the region filter keeps only the body's offsets and NONE of the caller's or a
  managed runtime's machinery is ever under `EFLAGS.TF`. The .NET `Invoke` no longer
  steps `DynamicInvoke` in-process (the crash surface where an in-window
  `pthread_create` that blocks `SIGTRAP` force-killed the process on slow hosts): it
  marshals through a `(long…)->long` shim table and, for signatures the shims can't
  express, **auto-falls back to the out-of-process stepper with a loud `SkipReason`**
  — never a silent miss. `HwTrace.DegradationNote()` gains the honest managed-window
  warning. Host-tested (`test_call_scoped`, byte-for-byte parity with begin/end) and
  validated on the .NET lane; see
  [managed-singlestep-lazy-arm-plan.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/plans/managed-singlestep-lazy-arm-plan.md).

- **Slow-host crash-avoidance stress lane** (`make hwtrace-dotnet-stress`, CI:
  `docker-hwtrace-dotnet-stress` in the `hwtrace-bindings` job) — the lazy-arm plan's
  "Sharpening 1". The ONE lane that runs with CoreCLR's tiering worker **unpinned**
  (no `DOTNET_TC_BackgroundWorkerTimeoutMs`): it parks past the worker's idle-exit,
  churns tier-up enqueues on the invoking thread (fresh `DynamicMethod`s driven past
  the call-count threshold), and interleaves lazy-arm `Invoke`s — recreating on the
  loaded CI runner the exact environment where the old stepped-`DynamicInvoke` path
  died with exit 133. Surviving with every capture intact is the pass signal.

- **The zig toolchain tarball is now integrity-pinned** — the one third-party fetch
  P2's supply-chain pass left unverified. `DOCKER_SETUP_zig` verifies a per-arch
  sha256 (`ZIG_SHA256_x86_64`/`_aarch64` in `mk/docker.mk`) before extracting, the
  anchors are recorded in `scripts/third-party-digests.txt`, and
  `check-thirdparty-versions.sh` now asserts both anchors exist for the declared
  `ZIG_VERSION` — so a version bump that forgets the digests fails loudly.

- **Example suites are now auto-discovered.** Every `examples/test_foo.c` +
  `examples/foo.s` pair (`foo.asm` under `ASM_SYNTAX=nasm`) links through a
  `test_%` pattern rule — drop the two files in and `make test` picks the suite
  up, with no Makefile edit. Legacy pairs whose routine object doesn't match the
  test name (`test_arith` → `add.o`, `test_capture` → `flags.o`, `test_struct` →
  `structs.o`) keep explicit link rules, and `SUITE_EXCLUDES` lists the
  `test_*.c` files owned by other targets (bench, usecases, demos, the
  emulator/trace tiers). This makes the long-standing docs claim in
  writing-tests.md true instead of correcting it downward.

- **`asm_call_capture_vec256_win64` and `asm_call_capture_vec512_win64` are now
  declared in `asmtest.h`** (under `-DASMTEST_ABI_WIN64`), completing the Win64
  mirror of the System V capture surface. Both existed in
  `src/capture_win64.asm` and were exercised by the Win64 suite, but a consumer
  following the win64 guide had to hand-declare the prototypes; the guide's
  entry-point table now lists `_vec512_win64` too.

- **Wide-arity, mixed-FP, and struct-return capture reachable from all ten
  bindings** (N4 of the 2026-07-04 review — previously the array-form C entry
  points existed but no binding referenced them). Three FFI-friendly shims join
  `asmtest_capture6`/`_fp2`/`_vec_f32` in the opaque-handle layer:
  `asmtest_capture_args` (stack-spilling wide arity), `asmtest_capture_mix`
  (integer + FP register files together), and `asmtest_capture_sret`
  (hidden-pointer struct return). The struct-layout bindings (C++/Rust/Zig/Python)
  call the array forms directly per their existing idiom; the opaque-handle
  bindings (Node/Java/.NET/Ruby/Lua/Go) wrap the shims. Every binding gained
  wide-arity (`sum8`), mixed (`mix_scale`), and struct-return (`make_big`)
  conformance tests — all ten docker lanes green; fixtures registered in the
  corpus name table (no repeat of N7); NASM counterpart included.

- **Docs: [Teaching with asm-test](https://github.com/wilvk/asm-test/blob/main/docs/guides/classroom.md)**
  (the in-repo scope of P5 from the 2026-07-04 review). The instructor recipe the
  primitives always supported but nothing documented: a three-file assignment
  layout (student `.s`, rubric `grade.c`, grade-time `Makefile`), a complete
  GitHub Classroom autograding config (one scored step per rubric item via
  `--filter` + `--fail-if-no-tests`, so a deleted rubric test is a scored zero,
  not a free pass), and instructor notes on hidden tests, timeouts, and grading
  non-x86 courses through the emulator tier. The separate "Use this template"
  assignment repository remains a maintainer action.

- **.NET examples roadmap — the full remaining tail** (11 items from
  [dotnet-examples-roadmap.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/plans/dotnet-examples-roadmap.md),
  all instruction-count-honest, all green in the docker lane). Five new reports:
  `flatprofile` (perf-report parity: self / Overhead % / cumulative %),
  `amplification` (user vs BCL vs native-runtime split + the WEAK-tier factor),
  `runtimegaps` (largest `RuntimeBefore` bursts by the method they precede),
  `footprint` (code working-set pages + jump-distance locality), and
  `runtimebuckets` (the ~1M-insn runtime lump named by module — resolved per 4 KB
  page, not per address, so ~hundreds of `/proc` lookups instead of ~1M). Six new
  example projects: `instructionmix`, `perfannotate`, `loops` (backedge trip
  counts), `descent` (native call-descent tree with self/inclusive counts),
  `descent_dotnet` (out-of-process call descent into a **live CoreCLR** — descends
  `Program::Leaf` twice as nested frames; `jit_dotnet` gained an additive `chain`
  mode), and `codeimage` (one address, two code bodies over logical time). The
  binding gained `HwTrace.SymbolizeBuckets` over the already-exported
  `asmtest_hwtrace_symbolize_bucket` (.NET suite 123 → 126).

- **Consumer-facing CI integration** (P2 of the 2026-07-04 review). A composite
  **GitHub Action** at the repo root (`action.yml`, "Setup asm-test": POSIX-sh
  steps; inputs `version`/`prefix`/`optional-tiers`/`test-command`; exports
  `PKG_CONFIG_PATH` and library paths), an includable **GitLab CI template**
  (`ci/asmtest.gitlab-ci.yml`, `.asmtest-install` + a documented consumer job with
  JUnit wiring), and a [CI integration guide](https://github.com/wilvk/asm-test/blob/main/docs/guides/ci-integration.md)
  covering both plus the raw `make install` fallback. The wrapped install recipe is
  proven end-to-end locally; the Action's `uses:` path needs a real Actions run.

- **macOS clean-room plan — Track E finished, Tracks C/D written.** `release.yml`'s
  seven smoke blocks now `source scripts/clean-env.sh` instead of ad-hoc
  `cd /tmp && env -u` scrubbing (behavior-preserving; interpreters resolved to
  absolute paths before the PATH scrub), and the methodology is documented in
  [docs/clean-room-testing.md](https://github.com/wilvk/asm-test/blob/main/docs/clean-room-testing.md).
  Tracks C (`scripts/osx-vm.sh` + `make osx-vm-test`, tart VM) and D
  (`scripts/docker-osx-bindings.sh` + `make docker-osx-bindings`, Docker-OSX/KVM)
  are written per the plan's spec and clearly banner-marked UNVALIDATED — they need
  Apple-Silicon-tart / bare-metal-KVM hosts this environment lacks.

- **AMD tracing plan Phase 2 & 3 follow-ups — attached block-step + snapshot marker
  routing.** Completes the two sub-items the earlier block-step / snapshot commits left open:
  - **`asmtest_ptrace_trace_attached_blockstep`** — the third public block-step symbol.
    Block-steps a SEPARATE, externally-attached process (one debug exception per taken
    branch, intra-block instructions reconstructed with Capstone), reading foreign bytes via
    `process_vm_readv` and leaving the target stopped past the region for the caller — the
    rootless managed-runtime completeness fallback. Wrapped in all ten bindings; a new
    `test_ptrace_attach_blockstep` asserts the stream is byte-identical to the per-instruction
    attached tracer over a true external attach.
  - **`opts.snapshot` begin/end routing on AMD** — the deterministic boundary LBR snapshot
    (`bpf_get_branch_snapshot` at a region-exit hardware breakpoint) is now reachable through
    the ordinary `begin`/`end` markers, not just the standalone `asmtest_amd_snapshot_trace`.
    The capture split into `asmtest_amd_snapshot_begin`/`_end` (armed single-slot); the AMD
    marker path derives the exit from the region's last `ret` and falls back to the
    `sample_period=1` sampled path when the BPF toolchain/caps/LbrExtV2 substrate is absent.

- **AMD hardware-trace improvements — Phases 0, 4, 5 of the
  [AMD tracing plan](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/amd-tracing-plan.md).**
  Completes the P0/P1 near-term work on the AMD LBR backend, all validated live on the
  Zen 5 dev box (Ryzen 9 9950X, `amd_lbr_v2`) via `make docker-hwtrace-amd`:
  - **Phase 4 — LbrExtV2 speculation-bit filtering.** `amd_replay` now drops a
    `perf_branch_entry` whose `spec == PERF_BR_SPEC_WRONG_PATH` (a speculative,
    never-retired phantom edge) before reconstruction; dropping it is expected, so it does
    not set `truncated`. The `spec` field (Linux ≥ 6.1) is gated behind a
    `-fsyntax-only` struct-member build probe (`ASMTEST_HAVE_PERF_BR_SPEC`), so the filter
    compiles out cleanly on older headers / Zen 3 BRS. `amd_edge_eq` (the stitcher's
    from+to overlap key) is untouched.
  - **Phase 5 — Tier-B stitch hardening.** `asmtest_amd_stitch` gained a
    decodable-distance guard: a smallest-overlap match is accepted only if the adjacency
    it splices is real straight-line code, so a dropped/throttled-sample mis-stitch
    becomes an honest gap instead of a silently-wrong trace. The AMD data ring default
    grew 64 KB → 256 KB to extend gapless stitch reach before the kernel drops samples;
    the `data_size` header comment now documents both backend defaults.
  - **Phase 0 — runtime branch-stack depth.** `asmtest_amd_lbr_depth()` reads the true
    depth from CPUID `0x80000022` EBX[9:4] (`lbr_v2_stack_sz`), replacing the hardcoded 16
    in the Tier-A/Tier-B split, stitch bound, and LOST heuristic. A no-op today (every
    shipping Zen reports 16) that removes the assumption.
  - Phases 6 (Zen 3 BRS period-adjust) and 7 (IBS-Op coverage) remain forward-look — they
    require Zen 3 / Zen 2 silicon the dev box lacks, and the project does not ship
    hardware code it cannot self-validate.

- **Docker-OSX clean-room lane (Track D): containerized `sshpass`, repointed at surviving
  upstream tags.** New `Dockerfile.sshpass` + `make docker-sshpass` build a small
  `asmtest-sshpass` image; `scripts/docker-osx-bindings.sh` now runs every ssh/scp-equivalent
  call through it instead of requiring a host `sshpass` install (and the `sudo` that would
  need), per CLAUDE.md's "add it where the work runs" rule. Separately, `sickcodes/docker-osx`
  deleted every tag but `:latest`/`:master` from Docker Hub in 2024 (`:ventura` and friends now
  404) — `DOCKER_OSX_IMAGE` defaults to `:latest`, and the script gained `DOCKER_OSX_DISK`
  support (`-v <disk>:/image -e IMAGE_PATH=/image`) plus a one-time-install recipe in its
  header, since a virgin `:latest` boots the macOS installer rather than a headless system.
  See [macos-cleanroom-lanes.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/macos-cleanroom-lanes.md).

### Changed

- **Linux Python wheels now build on the `manylinux_2_28` floor (install on older distros).**
  The two Linux legs of the release `python` job build inside
  `quay.io/pypa/manylinux_2_28_{x86_64,aarch64}` (AlmaLinux 8, glibc 2.28) instead of the
  ubuntu-latest glibc, via `scripts/build-manylinux-wheel.sh` — which source-builds the four
  native engines the image lacks (unicorn/keystone/capstone/libipt, pinned; libopencsd is a
  dead link-only dep and skipped) + fetches DynamoRIO, runs `make python-package`, and
  `auditwheel repair --plat manylinux_2_28` with the load-bearing tier `--exclude` list.
  `make docker-python-manylinux` proves it end to end with no credentials: the
  manylinux_2_28 wheel installs and imports (asm + disas) on a clean AlmaLinux 8.
  (distribution-packaging.md T5.)

- **The out-of-process whole-window stepper block-steps where `PTRACE_SINGLEBLOCK` is
  functional (~4–10× fewer stops).** The §D3 stealth whole-window helper now drives
  `asmtest_ptrace_trace_attached_window_stop_blockstep` (one `#DB` per taken branch)
  instead of one stop per instruction, degrading to the exact per-instruction stepper on a
  `DEBUGCTL.BTF`-masking hypervisor (`asmtest_ptrace_blockstep_available()` false). The
  output is **byte-identical** either way — a cost upgrade, not a fidelity one — which is
  what makes a managed whole-window affordable out of process. `ASMTEST_STEALTH_NO_BLOCKSTEP=1`
  forces the per-instruction stepper.

- **Registry-publish pipeline moved toward keyless publishing (scaffolding; gated on
  registry setup + a real tag).** `release.yml` now publishes PyPI via a dedicated
  OIDC Trusted Publishing job (`pypa/gh-action-pypi-publish`, collecting every matrix
  leg's wheel — the action is Linux-only) and crates.io via `rust-lang/crates-io-auth-action`,
  both with job-scoped `id-token: write` and no stored token; npm publishes with
  `--provenance`. `bindings/java/pom.xml` gains the Maven Central metadata + dormant
  source/javadoc/gpg/central-publishing plugins (activated only by a real `mvn deploy`;
  `make java-package` still uses javac + jar). The manylinux wheel floor is recorded as
  `manylinux_2_28`. All of these are **credential/registry-gated** — a trusted-publisher
  registration (PyPI/crates.io), `MAVEN_*` secrets + a namespace/PGP key (Maven Central),
  and a CI dispatch (manylinux) must land before they go live; see
  [releasing.md](https://github.com/wilvk/asm-test/blob/main/docs/reference/releasing.md).

- **`asmtest_pt_decode_window` gained a trailing `uint64_t *base_ip_out` parameter**
  (`src/pt_backend.c`) reporting the first decoded IP, so the whole-window PT drain can
  re-base its recorded offsets to ABSOLUTE addresses. Source-incompatible for a direct C
  caller of this internal decode entry (pass `NULL` to keep the prior offset-origin
  behavior); the facade (`asmtest_hwtrace_begin_window`/`_end_window`) and every language
  binding are unaffected.
  See [intel-pt-whole-window-substrate.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/intel-pt-whole-window-substrate.md).

- **Internal plan docs reconciled against the code; four completed plans archived.** An audit
  of all 20 active plans against the source, Makefile lanes, CI, and git history found ~30
  stale status markers whose drift was **entirely one-directional — every one under-reported**,
  marking shipped and tested work as "planned" or "forward-look"; nothing claimed landed was
  missing. The mechanism was visible in the artifacts: status was recorded by appending a dated
  block while section headers and tables kept their original marker. The markers are corrected
  (provenance preserved), and the four complete/closed plans move to
  `docs/internal/archive/plans/` per the repo convention: live-attach data-flow (7/7),
  DynamoRIO taint tier (9/9, band-gated at ~11x bare), Zen2 IBS (8/8), and the managed-attach
  safepoint spike (closed NO-GO). Two corrections are load-bearing rather than cosmetic:
  `data-flow-tracing-plan.md`'s Phase-5 stub told readers the taint tier stopped at Increment 3
  when all nine had landed; and **F4's blocker was retired** — both it and Phase 4 stated that
  live GC-move canonicalization needed an out-of-process EventPipe consumer ("its own lift"),
  but that was an assumption and it was disproved: an in-process `MovedReferences2` profiler
  delivers the exact `{old,new,len}` triples at a suspended-EE GC fence, is proven to coexist
  with DynamoRIO, and already ships in the taint tier. F4 is now wiring a proven feed to a
  landed transform, and is the recommended next milestone in that plan.

- **AMD-LBR reconstruction fills the entry-block prologue (fidelity fix).** On a live AMD
  host a too-fast tiny routine's frozen branch stack can carry spurious mid-routine *landing*
  edges, so `amd_replay` anchored at the landing offset and skipped the entry prologue
  `[base_ip, landing)` — a complete-reported reconstruction of a small routine undercounted
  its retired instructions (e.g. `insns=4` vs the block-step baseline `5`). `amd_entry_fill`
  now prepends the clean straight-line prologue all-or-nothing (a branch/ret/overshoot in
  that run honestly truncates instead), with a symmetric trailing fill; anti-fabrication
  tests confirm no phantom instructions. New `test_amd_live_smallroutine` hard-asserts
  complete⇒full-count on a live AMD host. Verified across 3 privileged runs + a 30-iteration
  loop: every complete reconstruction now yields the full count, and the batch-3 case-(b)
  escalation invariant is preserved 30/30. (The residual case-(a) advisory from the Zen 5
  findings doc is resolved.)

- **Multi-exit deterministic BPF boundary snapshot, default-on (followup Phase 5 / F13).**
  `hwtrace_begin_amd` now plants one hardware breakpoint per region exit (1–4 exits, one
  debug register each) via `asmtest_amd_all_exits` + `asmtest_amd_snapshot_begin_multi`, so
  whichever `ret`/tail-call a multi-exit routine leaves through hits a boundary — the old
  single-exit gate missed earlier exits and truncated. A BPF-side drop counter drives an
  honest truncated-on-drop contract (F13: a dropped ring record marks the result truncated,
  never silently complete — verified with a 1670-drop overflow fixture). >4 exits or any arm
  failure falls through to the sampled path unchanged. First live-validated on Zen 5 via the
  new privileged docker lane.

- **First-class privileged hardware-capture docker lane (`make docker-hwtrace-privileged`).**
  Runs `hwtrace-test ibs-test` under `--cap-add=PERFMON` alone (no `--privileged`, no
  `SYS_ADMIN`, default seccomp) — the first live validation of the exact AMD LBR (LbrExtV2)
  and IBS capture paths on the Zen 5 dev box: the previously-skipping AMD/IBS live tests
  (LBR capture, Tier-B stitch, per-thread concurrent fds, `sample_window`, IBS
  `survey_pid`/`survey_process`) all run and pass (test_hwtrace 389/389, test_ibs 23/23).

- **`make BUILD=<abs> test` / `usecases` / `valgrind` now work.** The suite-loop recipes ran
  `./$$t` where `$$t` already holds a `$(BUILD)/`-prefixed path, which broke under an
  absolute `BUILD` override (`.//tmp/...`); dropping the `./` prefix (the path always
  contains a slash) completes the earlier out-of-tree-build fix.

- **`jit_trace`'s JIT lanes prefer the byte-identical block-step rung (review F18).** The
  no-descent lanes select `asmtest_ptrace_trace_attached_blockstep` when
  `asmtest_ptrace_blockstep_available()`, falling back to the per-instruction stepper
  otherwise; the `*-descend` lanes intentionally stay per-instruction (block-step has no
  descent parameter). Verified byte-identical on a live V8 target: the ASLR-normalized
  disasm stream from block-step matches the pre-change single-step stream exactly.

- **`asmtest_amd_snapshot_end` drains the BPF ring without blocking (followup Phase 8 /
  F15).** `ring_buffer__poll(rb, 200)` epoll-waited 200 ms on the no-hit /
  honest-truncation path (the common case) before draining; `ring_buffer__consume` reads
  the producer position directly and returns at once — every record is already committed
  by the time the events are disabled.

- **One cached `amd_lbr_v2` cpuinfo probe (followup Phase 9 / F35/F11).** The duplicated
  `/proc/cpuinfo` parse in `amd_backend.c` and `msr_lbr.c` now shares a single internal
  cached probe.

- **CI / tooling hardening.** The documentation site now builds warnings-as-errors in a
  dedicated `docs` CI job (`sphinx -W`), so a broken cross-reference fails the PR in-repo
  instead of only reddening Read the Docs after merge. The `format` gate is pinned to
  `clang-format-18` (matching `make docker-fmt`'s `ubuntu:24.04`), so it no longer risks
  flagging the whole canonically-formatted tree as drift the day `ubuntu-latest` advances past
  24.04. `-Werror` now guards the `hwtrace` and `cli` jobs (previously only the base
  `test`/`check`), catching warnings in the newest, highest-churn code. A new `make fix-perms`
  target reclaims root-owned `build/` artifacts a `docker-*` lane can leave behind (which
  otherwise break `make clean`). `.dockerignore` now excludes every root `Dockerfile*` and the
  actual `bindings/Dockerfile.lang` (the stale `bindings/*/Dockerfile` glob matched nothing).

- **Internal working docs moved under `docs/internal/`** — `docs/plans/`,
  `docs/analysis/`, `docs/reviews/`, and `docs/archive/` are now
  `docs/internal/{plans,analysis,reviews,archive}`, with one archive rule
  (done plan / fully-actioned review → `archive/`; see
  `docs/internal/README.md`). The four completed scoped-tracing plans and the
  fully-actioned 2026-07-04 review moved to `archive/` accordingly, every
  in-repo reference (docs, comments, workflows, this changelog) was repointed —
  including a dozen references left stale by the earlier archiving commit — and
  the Sphinx `exclude_patterns`/docs-gate now exclude `internal/**` wholesale.

- **Docs/README accuracy pass from a full docs-vs-code review.** The
  entry-point pages no longer claim the language packages are published
  (nothing is on a public registry yet — the release pipeline is ready but
  uncredentialed); the README slimmed to pitch + highlights + links (the
  capability list's single source is now `docs/reference/features.md`) and its
  DynamoRIO link points at the tracing guide; `--bench-format=text|json` and
  `--help` joined the runner/benchmark flag tables; `installation.md` gained
  Keystone/Capstone rows and the real `--emu` dependency set;
  `integration.md` shows the `-x assembler-with-cpp` assemble step and the
  `asmtest-emu` pkg-config module; `api-reference.md` gained
  `ASSERT_ABI_PRESERVED_VEC`/`asmtest_check_abi_vec`; java.md's JDK
  requirement, rust.md's shipped Tier-2 asserts, Zig's raw-`@cImport` status,
  and the single-step tier's macOS support are stated consistently; and
  CONTRIBUTING gained "Adding an example suite" and "Building the docs"
  sections plus a per-language lib-setup cheatsheet in the bindings overview.

- **CI: the pinned Keystone/Capstone source builds are now cached** (K1 of the
  2026-07-04 review — the ~20-identical-multi-minute-LLVM-compiles-per-push item).
  Host builds cache via `actions/cache` + a new `scripts/thirdparty-cache.sh`
  (exact cmake-installed file set, keyed on OS/arch + the pinned versions); docker
  builds of `asmtest-bindings-base` cache via buildx `type=gha` behind a new
  overridable `DOCKER_BASE_BUILD` in `mk/docker.mk`. ci.yml only — `release.yml`
  deliberately stays cache-free. Non-fatal by design on any cache miss or backend
  outage; the warm-cache path still needs a real Actions run to confirm.

- **Docs: ["asm-test vs. alternatives"](https://github.com/wilvk/asm-test/blob/main/docs/reference/comparison.md)**
  (P4 of the 2026-07-04 review). A maintained comparison against the four workflows
  people actually use instead — a C unit framework with `.s` files linked in
  (cmocka/Criterion/Unity/gtest), raw Unicorn scripting, qemu+gdb, and
  asmUnit-style in-asm macros — including an honest "when the alternative is the
  better choice" for each, a "what asm-test does not try to be" calibration list,
  and a capability matrix. Linked from the README reference funnel and the docs
  index "Where to start".

- **Call-descent built-in default denylist — `asmtest_descent_use_default_denylist`**
  (the one unshipped Phase-5 deliverable of the
  [call-descent plan](https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/plans/call-descent-plan.md)).
  Arms the L3 `DESCEND_ALL` safety set the plan promised: at trace start the backend
  populates the handle's deny pool from the tracee — the dynamic linker's executable
  mappings (the lazy-binding PLT resolver) and `[vdso]`/`[vsyscall]`, managed-runtime
  GC/JIT modules by mapping name (CoreCLR/Mono/HotSpot/ART/V8/BoehmGC), and, on the
  fork path (tracee shares the tracer's layout), dlsym-resolved entry points of the
  classic blocking libc/pthread calls as one-byte deny regions. Denied callees are
  stepped over and recorded as edges; caller-supplied deny regions/callbacks compose.
  Wrapped in all ten bindings (parity gate green, 99 symbols × 10); a new fork-path
  fixture asserts a call landing exactly on `poll` becomes an edge, not a frame
  (hwtrace suite 259 → 260).

- **Emulator snapshot/restore — `emu_snapshot` / `emu_restore` / `emu_snapshot_free`**
  (E5 of the 2026-07-04 review). Captures the full register context
  (`uc_context_save`) plus the extents, permissions, and contents of every mapped
  region; restore reinstates the bytes **and the mapping set itself** (a region
  mapped after the snapshot is unmapped again). Mapped memory deliberately persists
  across `emu_call_*` — so fuzz/mutation sweeps previously ran each candidate
  against memory dirtied by its predecessors; bracketing the sweep with
  snapshot/restore makes killed/survived classification independent of handle
  history. Handle-level arming (watchpoints, register guards, preloads, the fuzz
  corpus) survives a restore by design. Emu suite 50 → 52.

- **`ASM_MIXCALL` — mixed integer + FP argument capture** (A6 of the 2026-07-04
  review). The canonical ptr+len+scalar shape gets a first-class macro:
  `ASM_MIXCALL(&r, fn, (buf, n), (0.5))` marshals each parenthesized group into its
  register file via the existing `asm_call_capture_fp` — no more hand-built compound
  literals (the repo's own `test_structparam.c` hand-roll is converted). Covered by a
  new `mix_scale` example routine (x86-64 + AArch64 + NASM bodies; shared with the
  bindings' mixed-capture fixtures) and the strict-c11/C++ header-portability gate.

- **FP reference models — `ASSERT_MATCHES_FREF{1,2,3}`** (A7). Differential testing
  now covers the FP surface where rounding/NaN/lane bugs live: double tuples from an
  `asmtest_fgen_fn` generator run through the FP register file
  (`asm_call_capture_fp`) and the C model, judged by ULP distance (`ulps = 0` =
  bit-exact; NaN matches only NaN). Example property tests pin `fp_add`/`fp_mul`
  against C models over dyadic rationals and a specials table (±0, ±inf, NaN,
  DBL_MIN/MAX).

- **Failing-input shrinking in `ASSERT_MATCHES_REF*`** (E7). On a mismatch the
  tuple is greedily shrunk toward `0` / `±1` / `LONG_MAX` / `LONG_MIN` (else halved
  toward zero) while the disagreement persists, so the report leads with the
  boundary value that triggers the bug — `shrinks to [0, 1]` — alongside the
  original draw. Deterministic, bounded, and self-tested (the negative suite's
  mismatch now asserts the exact shrunk tuple).

- **Runner flow control — `--fail-fast`, `--repeat=N`, `--shard=K/N`** (R5 of the
  [2026-07-04 review](https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/reviews/2026-07-04-repo-review.md)).
  `--fail-fast` stops dispatching at the first failing test (forces the serial path;
  the TAP plan moves to the end of the stream so it covers exactly what ran).
  `--repeat=N` block-replicates the selection N times — with `--shuffle`/`--seed`,
  the flake-hunting loop. `--shard=K/N` runs the K-th of N round-robin slices of the
  filtered selection, so N CI jobs can split one suite with no test lost or
  duplicated (self-tested: shards 1/2 + 2/2 reassemble `--list` exactly). Self-tests
  43 → 49.

### Fixed

- **asmspy `cli-smoke`: deterministic unknown-arity arg-decode assertion (de-flake).**
  The `--log` syscall arg-decode smoke (`cli/cli_smoke.sh`) asserts that at least
  one syscall in a 400-event window renders the honest unknown-arity form (`…`)
  rather than a fabricated arity-of-three. The only undescribed syscall the victim's
  stream produced was the *incidental* `restart_syscall` the kernel emits when a
  signal interrupts a blocking call — nondeterministic, and it flaked to zero
  matches under some kernels (measured 1-of-N on Docker-Desktop's LinuxKit kernel),
  so `make docker-cli` failed at that step. `cli/argdecode_victim.c` now makes one
  DELIBERATELY-undescribed syscall (`sysinfo`, absent from asmspy's `arg_shape`
  table), so the `…` rendering appears every iteration; the assertion keys off that
  specific call — a strengthening, not a weakening. `make docker-cli` cli-smoke
  PASS restored end-to-end.

- **Five red `main` CI jobs unbroken — regressions the individual lanes that
  introduced them did not catch.** Each surfaced on the shared `ci.yml` matrix
  after an unrelated lane landed; all five were failing on every recent push.
  - **`cli` (both `ubuntu-latest` and `ubuntu-24.04-arm`): a `-Werror` build
    break in `cli/asmspy_engine.c`.** Two `#include` lines carried comments whose
    body contained `*/` (`/* … TIOC*/FIO* … */`, `/* … S_IF*/STATX_* … */`), which
    closes the block comment early and leaves the tail as `error: extra tokens at
    end of #include directive [-Werror]`. Only the WERROR CI leg (`make WERROR=1
    cli-smoke`) is gated on it, so the non-WERROR `docker-cli` path stayed green and
    the break went unnoticed. Reworded both comments (`TIOC*, FIO*` / `S_IF*,
    STATX_*`); `make docker-cli` now builds and the smoke passes end-to-end.
  - **`test (riscv64 container)`: an x86-only opcode in a supposedly-portable asm
    stub.** `asmtest_sve_rdvl` in `src/capture.s` (added with the SVE trampolines)
    zeroed its return via `xorl %eax, %eax` in a catch-all `#else` that also covers
    RV64 — `src/capture.s:2786: Error: unrecognized opcode`. Split the arm to match
    the file's own 4-way pattern (`#elif __x86_64__` → `xorl`, `#elif __riscv` → `li
    a0, 0`, `#else` → `#error`); the riscv64 container builds and runs green again.
  - **`dataflow (analysis lib + bindings)`: a legitimate new hardware self-skip
    not on the gate's by-name allowlist.** Chaining F5's PT replay suite into
    `dataflow-test` added a `# SKIP pt live replay: no intel_pt PMU …` line, which
    tripped the anti-vacuity gate (it allowed only the BTF block-step skip). Extended
    the allowlist to permit the Intel-PT skip by name — a host gate exactly like the
    BTF one — with a matching `::notice`.
  - **`dataflow (F4 GC-move canon)`: a stale exact-count gate.** The lane grew from
    37 to 43 assertions when the object-identity alias phase (`1..6`) landed, but the
    gate still asserted `-ne 37`. Updated to 43 with the phase noted in the message.
  - **`benchmarks (macos-latest)`: Capstone `@rpath` dylib not found at runtime
    (one of two macOS issues; partial).** The pinned Capstone build installs to
    `/usr/local` and its dylib install-name is `@rpath/libcapstone.5.dylib`; recent
    macOS dropped `/usr/local/lib` from dyld's default fallback search, so `emu-bench`
    aborted (`Library not loaded: @rpath/libcapstone.5.dylib`). Set
    `DYLD_FALLBACK_LIBRARY_PATH=/usr/local/lib` on the benchmarks job (ignored on
    Linux); the CI run confirmed this resolves the `bench-check` abort. It then
    surfaced a **separate, pre-existing** failure the abort had masked: `bench-report`
    cannot link `build/asmfeatures` on the **Apple-Silicon** `macos-latest` runner —
    `Undefined symbols for architecture arm64: _catch_mach_exception_raise*`. Those
    MIG callbacks are defined in `src/mach_backend.c`, whose body is x86_64-only (the
    Mach out-of-process stepper was ported to Intel macOS only), while the MIG server
    `mach_excServer.o` references them unconditionally. Porting the Mach tier to
    arm64-macOS (or excluding it from the arm64 `asmfeatures` link) is a follow-on for
    a macOS-capable agent — tracked against macos-oop-mach-stepper / benchmarks-ci-followups.

- **macOS (Intel) native build correctness (fourth pass): the asmspy CLI lane +
  two ungated example lanes**, surfaced by building the lanes *outside* the
  nightly `test-macos-x86` contract on a macOS 14.7.5 / Intel host — `make cli`,
  `make cli-smoke`, `make WERROR=1 codeimage-test`, `make WERROR=1 build/jit_trace`
  — none of which a Linux or Docker-on-Mac (Linux) lane exercises. A tree-wide
  `make WERROR=1` sweep of every other macOS-buildable native lane
  (test/check/emu-test/asm-test/usecases/usecases-emu/dataflow-test/dataflow-pt-test/
  hwtrace-test and the C/C++/Ruby/Python binding lanes) was already clean; these
  three lanes were the gap.
  - `mk/cli.mk` gated `cli` / `cli-smoke` on **architecture only**
    (`x86_64 aarch64 arm64`), not OS. asmspy is a Linux-only out-of-process tracer
    (ptrace / `process_vm_readv` / `personality` / `/proc` / `<linux/futex.h>` /
    `<sys/user.h>` / the glibc extension `pthread_timedjoin_np`, plus `<sys/prctl.h>`
    in every victim), so on macOS-**x86_64** the arch gate passed and the build fell
    through and hard-failed — `cli/asmspy.c` at undeclared `process_vm_readv` /
    `pthread_timedjoin_np`, and the whole `cli/` tree at `<elf.h>` / `<linux/futex.h>` /
    `<sys/prctl.h>`. Per-file include guards can't fix this (`cli/asmspy_engine.c`
    alone carries ~473 Linux-only ptrace/`user_regs_struct`/`SYS_*` references);
    macOS's single-step tracer is the separate Mach-exception tier
    (`src/mach_backend.c`, `make mach-stepper-test`). Added a Linux **OS gate**
    checked *before* the arch gate to both targets, mirroring the existing arch-gate
    self-skip idiom, so `make cli` / `cli-smoke` now print `# SKIP … this is an OS
    gate` on non-Linux instead of a compile cascade. `make docker-cli` (Linux
    in-container, `UNAME_S=Linux`) falls through the gate and builds asmspy + drives
    the cli-smoke sequence exactly as before — the gate is host-OS-keyed, not
    container-keyed (verified: asmspy built and the smoke ran its full sequence).
  - `examples/test_codeimage.c`: the `BLOB_A` / `BLOB_B` file-scope `static const`
    routines are referenced only inside the two `#if defined(__linux__)` bodies, so
    off Linux they drew `-Werror,-Wunused-const-variable` — and the ungated
    `codeimage-test` lane compiles this C TU under `-Werror` (the test itself is
    *designed* to compile everywhere and self-skip at runtime via its `#else` stub).
    Guarded the two definitions with `#if defined(__linux__)` to match their use.
  - `examples/jit_trace.c`: `static int checks, failures;` and the `CHECK` macro are
    used only from the `#if defined(__linux__) && defined(__x86_64__)` body (the
    `#else` is a self-skip stub `main` that reports neither), so off that target
    (macOS, and Linux-arm64) they drew `-Werror,-Wunused-variable`. Guarded both with
    the same condition as their callers, honouring the file's own compile-and-skip
    design.

- **macOS (Intel) native build + binding self-skip correctness (third pass)**,
  surfaced by building the binding conformance corpus and the per-language
  `dataflow-*` / `hwtrace-*` lanes natively on a macOS 14.7.5 / Intel host — a
  surface no Linux or Docker-on-Mac (Linux) lane exercises (`make python-test`,
  `make WERROR=1 dataflow-test`, `make dataflow-cpp-test` / `-python-test` /
  `-ruby-test`, `make hwtrace-python-test`):
  - `bindings/conformance/conformance.c` included `<sys/mman.h>` / `<unistd.h>`
    only under `#if defined(__linux__)`, but the `CL_HAVE` `ptrace_descent`
    fixture that calls `mmap` / `mprotect` / `munmap` is gated on ARCH
    (x86-64 / aarch64), not OS — so it compiles on macOS and hit
    implicit-function-declaration errors there, breaking `make python-test`.
    Broadened the include guard to `__linux__ || __APPLE__`, matching
    `src/hwtrace.c`'s `asmtest_hwtrace_exec_alloc` W^X path.
  - `examples/test_dataflow_ptrace.c`: nine `static const` fixtures
    (`df_chain_v2` + the call-out / overflow set) used only inside the
    `__linux__ && __x86_64__` block drew `-Werror,-Wunused-const-variable` under
    `make WERROR=1 dataflow-test` on macOS. Guarded their definitions to match
    their use (completing the earlier `#else`-stub fix to the same file).
  - `bindings/dataflow_victim.c` — compiled by every `dataflow-<lang>` lane,
    which run on macOS — unconditionally included `<sys/prctl.h>` and called
    `prctl(PR_SET_PTRACER, …)` (a Linux-only Yama trace opt-in), so the shared
    victim failed to build on macOS (`'sys/prctl.h' file not found`). Guarded
    both under `__linux__`; the live-attach lanes self-skip off Linux regardless.
  - `bindings/python/tests/test_hwtrace.py`'s
    `test_window_region_free_whole_window` still hard-asserted `w.armed` — the
    one binding missed when the second pass guarded the C++/Ruby/Lua/Zig/Rust
    window tests. Now guards the arming-dependent checks on `armed` and notes the
    honest self-skip, matching node's and cpp's shape.
  - `bindings/ruby/dataflow.rb` used Ruby-3.0 endless-method syntax
    (`def steps = …`) that fails to parse on Ruby 2.6, violating the binding's
    own `required_ruby_version >= 2.6` (`asmtest.gemspec`) — and stock macOS
    ships Ruby 2.6. Rewrote as classic single-line defs (`def steps; …; end`),
    matching every sibling ruby file.

- **macOS (Intel) native build + whole-window self-skip correctness (second
  pass)**, surfaced by building the wider native tier set on a macOS 14.7.5 /
  Intel host (`make hwtrace-test`, `make dataflow-test`, `make WERROR=1
  hwtrace-test`, `make hwtrace-cpp-test hwtrace-ruby-test`):
  - `asmtest_hwtrace_pt_hop_open`'s non-Linux `#else` returned `ASMTEST_HW_ENOSYS`
    while the tier's classifier reports Intel PT as `ASMTEST_HW_EUNAVAIL` off
    libipt — so `test_pt_hop_surface`'s self-skip failed on macOS. Now returns
    `EUNAVAIL` (the same fix already applied to `pt_begin_window` /
    `pt_attach_begin`; the per-tid PT hop pair had reintroduced it).
  - `examples/test_dataflow_ptrace.c` called five `test_window_*` functions
    unconditionally in `main`, but their definitions live inside the
    `__linux__ && __x86_64__` guard — no non-Linux stubs, unlike every sibling
    test. Added the missing `#else` stubs.
  - `examples/test_dataflow_blockstep.c` used Linux-only `memfd_create`
    unconditionally (the F2 `sc_pread` fixture), breaking the macOS compile even
    though the whole suite runtime-self-skips off Linux via
    `asmtest_dataflow_blockstep_probe()`. Added a compile-only non-Linux stub
    (the caller is unreachable there — the fixture's own `fd < 0` SKIP covers it).
  - `examples/test_hwtrace.c`'s `map_exec` helper drew an unused-function
    `-Werror` under `make WERROR=1 hwtrace-test` on macOS: every caller sits
    inside a Linux guard. Guarded the definition to match its callers, exactly
    like the adjacent `frame_insns_eq`.
  - The region-free `§Z1` whole-window scope is Linux/x86-64-only (`begin_window`
    self-skips on macOS single-step, where the region-based tier still works).
    The C++, Ruby, Lua, Zig, and Rust binding tests hard-asserted `w.armed`,
    failing on macOS; they now guard the arming-dependent checks on `armed` and
    note the honest self-skip — matching node's already-correct shape and the C
    `test_wholewindow_singlestep` skip.

- **macOS (Intel) native build + PT self-skip correctness**, surfaced by
  validating the out-of-process Mach single-step tier natively on a macOS 14.7.5
  / Intel host (`make mach-stepper-test`, 25/25 live):
  - `examples/test_hwtrace.c` included `<unistd.h>` only under
    `#if defined(__linux__)`, so the portable `test_pt_attach_selfskip` (it calls
    `getpid()` on every host) failed to compile on macOS; the include moved to the
    unconditional POSIX block.
  - `asmtest_hwtrace_pt_begin_window` / `asmtest_hwtrace_pt_attach_begin` returned
    `ASMTEST_HW_ENOSYS` from their non-Linux `#else` arms, but the tier's single
    availability classifier reports Intel PT as `ASMTEST_HW_EUNAVAIL` on any host
    without libipt — so the PT `begin()` self-skip envelope diverged from the
    `status`/`skip_reason` contract on macOS. Both `#else` arms now return
    `EUNAVAIL`, matching the classifier and the Linux `!available` path.
  - `tests/glob_parity.c` compared `asmtest_glob_match` (pinned to glibc
    `fnmatch`) against the *host* `fnmatch` on undefined-behavior patterns
    (unterminated `[`, trailing `\`), which BSD/macOS `fnmatch` resolves
    differently — failing `make check` 11/44 on macOS. The divergent cases now
    assert the glibc-pinned contract directly, cross-checking the host `fnmatch`
    only under `__GLIBC__`; well-defined cases keep the live host differential
    everywhere.

- parallel runner (`-jN`): a non-EINTR `poll()` failure no longer abandons the
  run and reports never-run tests as passed; the scheduler degrades to
  blocking reaps.

- `--filter` on Win64: the portable glob matcher now matches POSIX `fnmatch`
  on unterminated `[`, backslash escapes inside classes, and trailing
  backslashes.

- guard-page allocators return NULL instead of a guard-page pointer for
  sizes within a page of `SIZE_MAX`.

- emulator fuzzing: corpus nudge no longer has signed-overflow UB at
  `LONG_MIN`/`LONG_MAX` range extremes.

- Zig conformance: vec256/vec512 capture tests no longer under-fill the
  8-slot vargs array.

- Win64 `--no-fork`: a fault on a non-test thread no longer hijacks the test
  thread's recovery stack; it takes the normal unhandled-exception path.

- docs: the emulator guide no longer claims `--emu` installs only libunicorn.

- **Cross-alias register def-use edges resolved.** `asmtest_defuse_build` (the
  shared, tier-neutral last-writer builder in `src/dataflow.c`) keyed its
  register axis on the raw Capstone id, so a write to one GP sub-register
  alias and a later read of another — `mov eax, ...` then a read of `ax`,
  `mov r8d, ...` then a read of `r8` — produced no def-use edge at all, even
  though the value trace correctly captured both. This is the shared
  builder's counterpart to `dfp_alias_shape`
  (`src/dataflow_ptrace.c`, added for the F6 gap barrier): a new `reg_slice`
  helper canonicalizes a Capstone GP register id to its 64-bit container plus
  a byte offset/length, and `apply_write`/`emit_read` now key a mappable
  register per CONTAINER BYTE — exactly as memory is already keyed per
  address byte — so a partial-overlap write/read resolves to the right last
  writer instead of missing the edge. `AH`/`BH`/`CH`/`DH` stay pinned to byte
  offset 1 of their container (not offset 0, which is `AL`/`BL`/`CL`/`DL`'s
  own byte): a write to `ah` reaches a later `ah`/`ax`/`eax`/`rax` read but
  never a later `al` read, which is the discriminator against a
  container-collapsing implementation that folds by container alone and
  ignores the byte offset (proven by temporarily mutating `reg_slice` that
  way and observing the new synthetic fixture fail, then restoring it). A
  32-bit GP write (`eax`, `r8d`, …) additionally marks the FULL 8-byte
  container as written, not just its own 4 bytes — x86-64 defines a 32-bit
  write as implicitly zero-extending bits 32-63, unlike a 16/8-bit write,
  which leaves the untouched bytes exactly as they were — so a later
  full-width read resolves its upper-half producer to that same write
  instead of a stale one from before the zero-extension (also proven by
  mutation: reverting the widened write range makes a dedicated fixture
  fabricate exactly that phantom edge).
  Vector registers, segment selectors, `EFLAGS`, and `RIP` fall through to
  the pre-existing raw-id keying unchanged (none of them alias with anything
  else, so raw-id keying was already exact for them). Two new live fixtures
  in `examples/test_dataflow_ptrace.c` exercise the windowed gap barrier
  end-to-end through this change: a glue excursion that clobbers a
  sub-register alias of a register the survey recorded, and — closing F6
  known-limit (4) — a glue excursion that clobbers a whole XMM register the
  survey recorded, the first fixture anywhere to exercise the barrier's
  vector path at all.

- **The scoped `ptrace` dataflow producer's call-out step-over can no longer
  fabricate a def-use edge across a stepped-over helper.** `dfp_step_loop`
  (`src/dataflow_ptrace.c`) runs a call-out at native speed and records nothing
  over it — correct for cost, but a helper that clobbers a location the region
  already wrote (and a later in-region read relies on) previously left the
  read's edge pointing at the stale in-region writer instead of the elided
  helper, silently wrong at a passing `rc`. Every scoped entry point
  (`_run`, `attach`, `attach_pid*`, `attach_jit`) now feeds a `dfp_riskset`
  (mirroring the windowed survey's existing gap barrier), and the call-out
  branch snapshots it immediately before the native run and diffs it after:
  a synthetic GAP step is appended carrying exactly what changed (register
  alias-sliced, memory per byte), so a post-call read correctly resolves to
  the barrier instead of the stale writer. A risk-set cap hit is **deferred**
  in scoped mode — it only promotes to `truncated` at the first real gap
  (and is discarded on a gap-free exit), so a region that never calls out is
  never falsely flagged. Precision, not a blanket invalidation, is
  load-bearing here (F6 measured that a blanket shadow deletes true
  cross-gap edges): a helper that touches nothing at risk appends no record
  for that location, even though the gap step itself is still present (the
  call/ret round trip through any helper unavoidably moves `rsp`, which was
  already at risk from the call's own write).

- **`make install` / `make install-shared-hwtrace` now ship `asmtest_ibs.h`** —
  the hardware-tracing guide's documented `#include <asmtest_ibs.h>` could not
  previously compile against an installed package (it was the only
  guide-referenced header missing from all three install lists).
  `scripts/clean-room-test.sh` gained a header-install compile check (a fresh
  `make install` + `cc -fsyntax-only` against every guide-referenced header)
  so this omission class cannot silently recur.

- **The ptrace block-step reconstructors now mark the capture truncated when a
  block contains a `rep`-prefixed string op.** A `rep movs/stos/…` retires once
  per iteration under per-instruction stepping (RIP parks on it) but a static
  block-step reconstructor records it exactly once, so the block-step stream
  silently under-counted it. New `asmtest_disas_is_rep_string` lets
  `bs_record_run` and `window_block_walk` downgrade such a block to
  `BS_AMBIGUOUS` (honest truncation), bounding the "byte-identical to
  per-instruction stepping" promise accordingly.

- **The ptrace block-step reconstructors no longer record never-executed
  instructions when the traced code contains an application `int3`.** A JVM
  safepoint poll or .NET breakpoint inside a block-stepped region was misread as
  a BTF `#DB` block completion, so the region, attached, and windowed drivers
  fabricated the instructions after it with `truncated=false`. They now classify
  the trap via `si_code` (SI_KERNEL / TRAP_HWBKPT), record the executed run up to
  and including the trap byte, mark the capture truncated, and forward the
  signal — the region (owned) driver via `PTRACE_CONT`, the attached (foreign)
  driver by leaving the target in its SIGTRAP delivery-stop for the caller, and
  the windowed driver by handing off to the per-instruction window loop, which
  runs the frame to its window end at native speed (`run_until_sig`) and
  recovers `*result` there instead of discarding the signal.

- **No per-instruction ptrace loop in `src/ptrace_backend.c` swallows an
  application SIGTRAP any more.** `run_until` (the call-out step-over primitive,
  now `run_until_sig` plus a 2-arg wrapper), the per-instruction region driver
  (`asmtest_ptrace_trace_call`), the foreign attached driver
  (`asmtest_ptrace_trace_attached`), the windowed per-instruction loop shared by
  `asmtest_ptrace_trace_attached_windowed[_window_stop]`, the fork-owned window
  driver (`asmtest_ptrace_trace_window_call`), and call descent
  (`asmtest_ptrace_trace_call_ex`/`_attached_ex`) each either deliver an
  application `int3`/breakpoint via `PTRACE_CONT` (owned tracee) or end honestly
  with the target left at its SIGTRAP delivery-stop (foreign) — never
  `PTRACE_SINGLESTEP`/`PTRACE_SINGLEBLOCK` with the signal attached (measured
  fatal: the re-armed trap fires inside a masked handler). `bs_sigtrap_is_app`
  (the `si_code` classifier introduced for the block-step drivers) is now a
  file-wide helper shared by every loop, on both x86-64 and AArch64.

- **The call-out step-over is now depth-aware (code review finding #19's real
  fix).** `run_until` (now `run_until_sp`, with `run_until_sig`/`run_until` kept
  as thin wrappers) previously resumed the trace at the FIRST arrival at a
  call-out's return-address breakpoint, so a stepped-over helper that called
  BACK into the traced region (a callback, or a tiering/OSR stub re-invoking the
  method) hit its own return-address breakpoint from a deeper stack frame first
  and hijacked the resume into that nested invocation. `classify_region_exit`
  (shared by all four region drivers — per-instruction, block-step, attached
  per-instruction, attached block-step) now also passes the callee-entry stack
  pointer, and `run_until_sp` rejects a same-address hit at the wrong depth: it
  steps past the premature hit at native cost (a single-step over a software
  breakpoint, or a bare `PTRACE_CONT` for a hardware one — `EFLAGS.RF` keeps the
  CPU from re-trapping on it) and keeps waiting for the matching depth. A new
  differential fixture — a region that calls a helper which calls back into the
  region's own entry exactly once before returning — proves the trace now
  resumes at the true, outer completion instead of the inner one.

- **`asmtest_ibs.h` no longer describes the shipped system-wide capture flag as
  a future phase** — the `survey_process` residual-race note now names the
  `ASMTEST_IBS_OPT_SYSTEM_WIDE` flag directly.

- **The pure IBS-Op decoder now validates the record's own caps word (BrnTrgt)
  before trusting the branch-target register.** Two 68-byte record shapes are
  length-identical (`BRNTRGT=0/OPDATA4=1` vs `BRNTRGT=1/OPDATA4=0`) and only the
  caps word disambiguates reg[7]; the decoder previously trusted length alone and
  could misread an `IbsOpData4` value as a branch destination. The RipInvalid
  read is now gated on caps bit 7, and `asmtest_ibs_available()` requires CPUID
  IBSFFV (EAX[0]) so it cannot disagree with the caps the kernel samples with.

- **IBS ring-loss heuristic now bounds the callchain worst-case record** (was
  112 bytes, ~10× short — silent sample loss with `lost==0 && throttled==0`);
  `ibs_fill_attr` pins `sample_max_stack` so the bound is sound, and the
  internal window lane no longer opens with callchain (no in-tree consumer, and
  a callchain stream can overrun the single end-of-window drain).

- **`ASMTEST_IBS_OPT_CALLCHAIN` is documented as consumer-less**: it enables
  kernel-side capture only; nothing in the tree decodes the stack (the drain
  parses past it to reach RAW), and the window lane ignores it.

- **`ibs_probe` and the `ibs-test` live skips now attempt a real perf open and
  report the real refusal reason instead of claiming AVAILABLE from the
  CPUID/sysfs substrate probe alone.** On a locked-down AMD host (perf blocked by
  `perf_event_paranoid`/seccomp) the substrate is present but no sampling can
  open — `ibs_probe` prints `substrate present but sampling is BLOCKED — <reason>`
  (Op and Fetch lanes) and the five `test_ibs` EUNAVAIL skips print the real
  `asmtest_ibs_unavail_reason()` instead of a hardcoded guess. The AMD
  manual-validation checklist no longer inverts the `call_auto` regression
  signal: post-`5d8e0d2` a `truncated=0` where escalation must fire is a
  **regression**, not a known finding.

- **The guides and the public header no longer claim AMD LBR live capture works
  on Zen 3.** The live-capture floor is **Zen 4+** (LbrExtV2) — Zen 3 BRS exists
  in silicon but this tree cannot open it (the generic `sample_period=1` open is
  rejected by the kernel's `amd_brs_hw_config`; the raw-`0xc4` arm is a
  hardware-gated follow-up). Swept every "Zen 3+" floor claim in the tracing
  guides, `asmtest_hwtrace.h`, and the AMD backend comments to "Zen 4+", each
  with the one-line cannot-open explanation.

- **The dead AMD freeze-on-PMI probe (`asmtest_amd_freeze_available`) and its
  false PRESENT/ABSENT diagnostic are removed.** The probe had zero live
  consumers after `5d8e0d2` replaced the freeze-conditional window-trust gate
  with an unconditional exit-presence check that runs on every part
  (`asmtest_amd_ring_parse_decode`). `test_hwtrace` printed a trust statement
  ("PRESENT (single-window Tier-A trusted)" / "ABSENT (…)") that was false in
  both branches — Tier-A completeness is exit-anchored regardless of the freeze
  bit. The freeze test is retired; the snapshot-substrate/depth probe checks stay
  (renamed `test_amd_snapshot_substrate_probe`).

- **The AMD deterministic boundary snapshot no longer flags a provably complete
  15-branch window as `truncated`.** The depth-ceiling check in
  `asmtest_amd_decode_reach` counted the total decode-array length, but
  `branchsnap.c` prepends a synthetic boundary edge (a deterministic completion,
  not a captured hardware slot), so a full 15-hardware-slot window (15 + 1
  synthetic = 16) tripped the 16-deep ceiling and escalated to a needless real
  re-execution of the routine under test (`src/trace_auto.c`). The new
  `asmtest_amd_decode_reach_hw` gates truncation on the hardware slot count; the
  `asmtest_amd_decode` / `asmtest_amd_decode_reach` wrappers pass `hw_nbr == nbr`
  so every other caller is byte-identical.

- **`make cli` / `make cli-smoke` on arm64 now self-skip like the other tiers
  instead of dumping raw compile errors.** asmspy's single-step engines are
  x86-64-hardcoded (`rip`/`eflags`-TF/`orig_rax`), so on aarch64 the build died
  mid-compile with `SYS_open undeclared` / `no member named 'rip'` — or worse,
  fell into the missing-dependency branch and advised installing libncurses-dev,
  which cannot fix an architecture. A `uname -m` gate (checked before
  `CLI_MISSING`, for exactly that reason) now prints an honest `# SKIP` naming
  the open ARM64-abstraction plan row and exits 0. Measured in a real
  linux/arm64 container: skip + rc 0 both targets; x86-64 unchanged.
  a Yama/seccomp skip.** The victim called the region once and `_exit(0)`'d, so on
  a slow host the child finished and died before the parent's `PTRACE_SEIZE`
  landed (3/3 GitHub runs today), and the resulting `ESRCH` surfaced as
  `# SKIP … PTRACE_SEIZE unavailable here (yama/seccomp)` — a double lie, since
  SEIZE worked for every other attach test in the same job — which the lane's
  anti-vacuity gate rightly turned into a hard failure. The victim now LOOPS the
  region at the same 2 ms cadence as every other attach victim in the file, so
  the attach always finds a live process and a fresh entry. Proven discriminating
  in the docker lane: the once-and-exit victim plus a deliberate 200 ms
  pre-attach sleep reproduces the exact CI skip; the looping victim passes under
  the same handicap. The gate's stale bookkeeping was recalibrated in the same
  change: the 8 suites total 389 on bare metal (the comment said 257), the VM
  runs ~293, and the floor moved 230 → 285 — preserving the original tightness
  (the smallest suite vanishing still trips it).

- **`cli/asmspy.c`'s new picker-sort code failed the clang-format gate.** The
  Tab-cycle sort landed verified by `make docker-cli` (build + smoke) but not by
  `make fmt-check`; the format job caught 7 violations. Mechanical reflow, plus
  one comment hoisted above its `if` so the formatter keeps the condition on one
  line.

- **Data-flow `--dataflow`'s call-out step-over lied about WHY it truncated, and two
  test suites carried assertions that could not fail.** Three small, independently
  diagnosed defects closed together:
  - **`dataflow_ptrace.c`'s call-out step-over conflated a BOUND with a FAILURE**
    (the sibling site to the `--max` fix: `9d55611`/`0129b1e`). Hitting the whole-run
    step backstop mid-call-out and `dfp_run_to` actually failing shared one `||` and
    one `DF_PTRACE_ETRACE`, so a region that simply ran a lot of call-outs surfaced as
    *"ptrace/attach failure (permission? ptrace_scope? … W^X JIT page)"* — sending an
    operator to Yama/seccomp for a budget, not a bug. The backstop is now its own
    branch (`DF_PTRACE_OK`, `truncated=true`, same shape `--max` already got right);
    `dfp_run_to` failing (the callee exited, faulted, or its return byte could not be
    trapped) keeps `DF_PTRACE_ETRACE`. The 2^20-step backstop is now also overridable
    via `ASMTEST_DF_STEP_BACKSTOP` (mirroring `ASMTEST_DF_ENTRY_WAIT_MS`), which is
    what makes the bound reachable in a test at all — a real 2^20-step fixture was
    exactly the kind of "never exercised, needs 1M hits" gap this codebase already
    flags elsewhere. A new attach-based test (`test_callout_step_backstop`, needs the
    attach path's exact `pre_positioned` entry so the trip point is deterministic
    rather than a coin flip on the fork prologue's step parity) proves it: mutation
    (reverting the split) turns the check back into ETRACE.
  - **`test_branchsnap.c`'s multi-exit test asserted `covered(t, 0)` as its entry
    evidence — vacuously.** `amd_replay` appends block 0 unconditionally
    (`amd_backend.c:267`), so `covered(t, 0)` is always true by construction; the
    check was carried entirely by the `ni > 0` conjunct beside it, same fact the
    Phase 9 tail-`jmp` tests in the same file had already found and correctly
    stopped relying on. `snap_default_run` now asserts the PATH-SPECIFIC block
    instead (`covered(want_off) && !covered(other_off)`, the two exits' own `mov`
    blocks) — real evidence that the default-on snapshot captured the exit that
    actually ran, not just "some" data regardless of which path executed.
  - **`test_dataflow_blockstep.c` re-declared `asmtest_blockstep_info_t` with no
    layout guard.** The tier ships no header by design (keeps the producer off the
    public ABI), so the suite hand-copies the struct — exactly the skew that cost
    F6's sibling telemetry struct 3 green checks before a `sizeof`+`offsetof` guard
    caught it. `asmtest_dataflow_blockstep_info_layout()` (mirroring
    `asmtest_dataflow_ptrace_win_info_layout`) now lets the suite check its copy
    against the producer's real layout before trusting any `info.*` field.

  All three were filed as open follow-ups
  ([2026-07-17-dataflow-tier-open-followups.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/analysis/2026-07-17-dataflow-tier-open-followups.md))
  after the same day's F1/F2/F6/F7 batch landed, deliberately deferred out of that
  diff to avoid scope creep. Verified: `make docker-dataflow-attach` (126+118 checks,
  0 skips, 0 failures) and `make dataflow-blockstep-test` natively on the Zen 5 dev
  box (119/119). `test_branchsnap.c`'s live leg needs the BPF toolchain
  (clang/libbpf-dev), absent on this host and gated behind a `sudo` password this
  session could not supply — verified by compilation + the ENOSYS stub path only.

- **`asmspy --dataflow` on a symbol that is not running HUNG instead of erroring.** The
  producer's step backstop counts single-steps, and a region that never arrives burns
  zero steps — so the blocking wait never advanced (measured: rc=124, killed by
  `timeout`, where `--trace` on the identical target answered "never executed" in 4 s).
  The entry wait is now bounded by a `CLOCK_MONOTONIC` deadline (default 10 s,
  `ASMTEST_DF_ENTRY_WAIT_MS` overrides, 0 restores the old unbounded wait) and reports
  *"<sym> not seen entering in pid N (waited M ms)"* — an outcome, not a failure: the
  symbol resolved and the tracer worked; the code just is not being called right now.
  The unwind re-establishes the all-running invariant (restore the entry byte, rewind a
  thread stopped at `base+1`, continue), which also fixes a latent hang on the
  never-exercised step-backstop disarm path.

- **`asmspy --dataflow --max=<n>` failed for every `n` below the region's step count —
  and blamed ptrace for it.** The truncation branch did everything right (partial trace
  appended, `truncated:true`) and then returned the generic ptrace-failure code, so a
  valid cap surfaced as *"ptrace/attach failure (permission? ptrace_scope?…)"*. The flag
  worked only when it did nothing (measured: `--max=3` rc=1, `--max=200` rc=0 on an
  83-step region). It now returns OK with the truncated partial trace; the smoke asserts
  the EXACT step count per cap, so "cap ignored" cannot pass either.

- **Three `asmspy --trace` honesty defects: a bound that wasn't, a diagnosis thrown
  away, and a documented self-skip that never happened.** (1) The entry wait's idle
  window reset on EVERY waitpid event — a target that stops more often than the window
  (a 1 Hz timer, a chatty clone) reset the budget forever and `--trace` blocked
  indefinitely; a 30 s `CLOCK_MONOTONIC` wall bound now sits alongside the idle rule,
  checked unconditionally. (2) The entry race's four distinct outcomes were bare
  integers collapsed by a bare `break`, so "the region never ran", "the target exited",
  and "the entry could not be armed" all rendered as *"never executed"*; the outcomes
  are now named and each maps to its own answer ("pid N exited before <sym> was seen
  executing" for an exit, an attach/ETRACE report for an unarmable entry). (3) That fix
  makes `asmspy.h`'s promised W^X/JIT self-skip real: an entry page refusing the
  breakpoint now reports *"possibly a W^X JIT page refusing the entry breakpoint"*
  instead of the confidently-wrong *"never executed"* — verified against an unmappable
  explicit range, the same failure shape a genuinely W^X page produces.

- **`asmtest_trace_call_auto` could report a window-overflowing AMD-LBR capture as
  complete, so escalation never fired (a real Zen 5 silicon finding).** The Tier-A
  completeness check in `asmtest_amd_ring_parse_decode` — trust a single sampled window as
  complete only if it contains the region-exit branch — was gated behind
  `!asmtest_amd_freeze_available()` and thus skipped on freeze-capable parts (Zen 5). With
  `sample_period=1` the capture picks the *richest-in-region* window, often an arbitrary
  mid-run fragment that never held the exit, so a 25-back-edge loop reconstructed a 4-edge
  fragment and reported `truncated=0` — `trace_call_auto` returned it as complete instead
  of escalating to block-step (and `test_call_auto` passed vacuously). The exit-presence
  requirement now runs on every part; combined with the existing overflow flag it is the
  airtight "complete iff a non-overflowed exit-anchored window exists" invariant. Verified
  deterministic across 16 privileged AMD runs; `test_call_auto` case (b) hardened to fail
  hard on a fragment-reported-complete. Surfaced only because the new
  `docker-hwtrace-privileged` lane runs the exact AMD paths live.

- **The shared `libasmtest_hwtrace` shipped with an undefined `asmtest_ibs_window_end`.**
  The Zen-2 F6 IBS survey fallback made `hwtrace.c` call the IBS window primitives, but
  the shared-lib link recipe never included `ibs_backend.o` — every binding's dlopen
  failed on every host (the static test binaries link `HWTRACE_OBJS`, which carries it,
  masking the gap in `hwtrace-test`). `pic/ibs_backend.o` is now compiled and linked, and
  tracked by the knob-flip rebuild sentinel.

- **A corrupt/huge `nr` in an AMD branch-stack sample could drive the ring parse out of
  bounds (review F5/F7, followup Phase 2).** The sampled-branch count from the perf ring
  is now clamped (`nr <= 64`, comfortably above the 32-deep hardware maximum) *before*
  the `nr * sizeof(perf_branch_entry)` size check can wrap, and a short-tail sample too
  small to hold the 8-byte `nr` itself is rejected instead of read. Exercised by the new
  synthetic-ring tests on every host.

- **`asmtest_trace_call_auto` could return `ASMTEST_HW_OK` with an *empty* trace (review
  F24).** Each escalation rung's reset discards the prior rung's partial capture, but
  `ran` kept reading 1 from that earlier rung — so a rung that reset and then failed at
  runtime (seccomp/`ptrace_scope`, ENOMEM) reported a successful empty trace. `ran` is
  now cleared at every reset site and re-earned only when a rung actually commits; the
  legitimate truncated-but-OK partial (no downstream rung runs) is preserved.

- **`asmspy`'s single-step engines could kill a V8/Node target seconds *after* a clean
  detach.** V8 worker threads park in blocking futex syscalls; a `PTRACE_SINGLESTEP` that
  completes across a syscall defers its `#DB` debug exception until the syscall returns, so a
  parked worker carried a *queued* trap through detach — when its futex later woke, the trap
  fired with no tracer attached and terminated the whole process (reproduced: ~1 detach in
  2–6 fatal on an 11-thread V8 target). Two prior defenses missed it: the two-phase detach
  orders resumes, and the trap-flag clear was gated on a read-back TF bit that a
  kernel-forced TF masks out of `GETREGS`. `detach_threads` now clears TF unconditionally
  and **drains the pending step** — each stopped thread is single-stepped once more to
  consume its queued `#DB` while we are still the tracer (skipping threads poised on a
  syscall instruction so the drain cannot block); the `PTRACE_SYSCALL` engines skip both
  phases, since draining them would inject step state into a target that had none.
  30 + 25 consecutive attach/trace/detach cycles on the 11-thread V8 target now survive.

- **`asmspy` swallowed a target's own `int3` breakpoints (and could have killed it
  re-injecting them).** The single-step engines treated every `SIGTRAP` stop as their own
  step, so an application-executed `int3` (a JIT/debugger breakpoint, e.g. V8's
  `IMMEDIATE_CRASH`) was mis-decoded and silently dropped, breaking the app's own breakpoint
  logic. Stops are now split by `si_code` (`PTRACE_GETSIGINFO`): only `SI_KERNEL` (an
  executed `int3` on x86) and `TRAP_HWBKPT` are delivered back to the target — via
  `PTRACE_CONT`, never `SINGLESTEP`, because re-arming the trap flag fires a `#DB` inside
  the (SIGTRAP-masked) handler and the kernel force-kills the target. Everything else
  (`TRAP_TRACE`, `TRAP_BRKPT` from a step completing across a syscall, the `SI_USER` exec
  trap) is still absorbed. New `int3_victim` + smoke prove a self-breakpointing target
  survives tracing with its handler intact.

- **Fork-based tracers aborted the whole trace when an unrelated signal interrupted the
  post-fork handshake.** `trace_call`, `trace_call_blockstep`, `trace_window_call`, and
  `trace_call_descend` waited for the child's initial `raise(SIGSTOP)` with a bare
  `waitpid` that treated `EINTR` as a failed handshake (rc=`ETRACE`, zero frames). A host
  runtime's repeating timer — or the descent stale-alarm test's deliberate 200 µs `SIGALRM`
  storm, which failed ~70 % of runs on a fast box — could land in that window. All four
  handshake sites now retry across `EINTR` exactly as the step loop always has; a genuine
  child death still surfaces. The stale-alarm test passes 20/20.

- **Four defects found by a deep multi-agent audit of the whole tree** (each survived
  double adversarial verification; the newest subsystem — `asmspy` — and the AMD/LBR, PT,
  single-step, orchestration and FFI-binding layers came back clean). (1) **Reused-handle
  determinism leak in two emulator guests.** The x86 and arm64 setups zero the GP + vector
  register file before every call so a routine that reads a register the caller did not set
  gets a deterministic `0`; the **RISC-V** (`emu_riscv_setup`) and **ARM32** (`emu_arm_setup`)
  setups omitted it, so a long-lived handle (how every binding holds it) leaked the previous
  call's callee-saved / FP-lane state into the next call and returned a stale result with
  `ok=true`. Both now zero registers (x1..x31 + f0..f31 / r0..r12 + d0..d31 + condition
  flags) like the other two guests. (2) **In-process stealth stepper could busy-hang
  forever.** `asmtest_hwtrace_stealth_trace`'s `while (!sc->ready)` spin only checked for
  early helper death under `if (use_exec)`; in the in-process fork fallback (common under the
  ptrace-restricted container/CI posture this project targets) a helper killed by
  seccomp/OOM/watchdog before publishing `ready` left the caller spinning at 100% CPU. The
  `waitpid(WNOHANG)` death check now runs unconditionally, matching the two windowed spins.
  (3) **Block-step tracer leaked its owned tracee on overflow.**
  `asmtest_ptrace_trace_call_blockstep` broke out on a `blockstep_reconstruct` failure
  (stream full / undecodable insn / no in-region terminator) with `rc` still OK, so the
  post-loop cleanup — which only reaps on `rc != OK` — left the forked child alive,
  ptrace-stopped and unreaped; repeated calls could exhaust PIDs. It now `kill`+`waitpid`s on
  that path like the other overflow breaks. (4) **DynamoRIO recording stack could pop a live
  region on deep nesting.** The client's `on_begin` pushed only while `depth < MAX_DEPTH` but
  `on_end` always decremented, so nesting past 16 distinctly-named regions desynced the
  per-thread stack and silently dropped coverage with `truncated` left `0`. `on_begin` now
  tracks the true nesting depth unconditionally (matching the app side) and flags the trace
  `truncated` when a region falls outside the storable window — upholding the never-present-a-
  partial-trace-as-complete invariant.

- **Native-trace "honesty" gaps — three places a partial capture could escape without its
  `truncated` flag.** The framework's core invariant is that an incomplete trace is never
  presented as complete; a review found three leaks and they are now closed. (1) The
  whole-window single-step loops (`asmtest_ptrace_trace_attached_windowed` and the fork-internal
  `asmtest_ptrace_trace_window_call`) treated *any* loop exit as clean — so a window whose tracee
  died or `exit()`ed before reaching the return address was reported complete; they now flag the
  stream truncated unless the one clean terminator (`pc == win_ret`, or the async `*stop`) was
  reached. (2) The AMD MSR-direct LBR path (`asmtest_amd_msr_trace`) returned a partial branch
  stack as complete when a mid-stack MSR read failed; a short read now sets `truncated`. Both
  err toward false-truncated over false-complete.

- **`asmtest_hwtrace_arm_tid()` reported a stale thread id after a single-step scope closed.**
  The accessor's documented contract is "the OS thread id that armed the active capture, or `-1`
  when none is active", but the single-step `end()` path (the default, most-portable backend,
  freshly wired into eight bindings) returned without clearing it — so it kept reading the last
  arming tid instead of `-1`. It now resets like the PT / AMD / whole-window paths already do; a
  regression assertion covers it.

- **`asmspy --trace` silently produced nothing for a function that runs only on a worker
  thread.** The region engine attaches only the thread-group leader (unlike the whole-process
  syscall/stream engines, which SEIZE every thread), so a function executing on another thread
  was never single-stepped and the command exited cleanly with zero output. It now reports the
  region was never observed executing and points at `--stream` (which follows all threads).

- **`asmspy` ptrace-lifecycle hardening.** A job-control group-stop (`^Z` / `SIGSTOP` / tty
  stop) is now handled with `PTRACE_LISTEN` instead of being resumed, so a traced target can
  actually be suspended while watched. On OOM while seizing threads, an already-seized thread is
  now detached rather than left stranded seize-stopped. The ELF section-header walk in the
  symbol resolver now strides by `e_shentsize` (not `sizeof(Elf64_Shdr)`), so a non-standard
  object with a larger entsize resolves correctly instead of reading misaligned headers.

- **Latent AMD-LBR test flake in nine bindings' auto-select hwtrace test.** Each binding
  mirrors the C reference `test_auto_resolve_traces_live`: pick `auto(BEST)`, trace a tiny
  five-instruction routine, assert the result. On a privileged AMD Zen 3+ host `auto` picks
  AMD LBR, which honestly *truncates* a too-fast-to-sample single-shot routine (so `covered(0)`
  is false) — the C reference and .NET already asserted `covered(0) || truncated`, but the fix
  was never ported, so rust/cpp/python/lua/ruby/zig/node/java/go still asserted only
  `covered(0)` and would fail on such a host. All nine now assert the honest invariant.

- **The hwtrace options struct under-allocated the AMD-LBR fields in all seven FFI-mirroring
  bindings (8-byte OOB read in `asmtest_hwtrace_init`).** When `lbr_period`/`branch_filter` were
  appended to `asmtest_hwtrace_options_t` (Zen 4/5 LBR work), only the .NET binding's struct was
  updated; every other binding that hand-mirrors the struct still described the old 40-byte layout —
  Node `koffi.struct`, Java `OPTIONS_LAYOUT`, Python `ctypes.Structure`, Rust `#[repr(C)]`, Ruby's
  Fiddle packer, Go's cgo typedef, and Lua's `ffi.cdef`. `HwTrace.init` passed a 40-byte buffer, and
  `asmtest_hwtrace_init`'s `g_opts = *opts` copies the full 48 bytes — reading 8 bytes past the
  buffer on every init. Harmless for the SINGLESTEP backend (it ignores those fields), but for an
  `AMD_LBR` init the garbage read could seed a spurious sample period / reduced branch filter,
  silently altering capture. All seven now mirror the 48-byte C layout (verified: `koffi.sizeof ==
  48`, `ctypes.sizeof == 48`; the rust/ruby/go/lua `docker-hwtrace-<lang>` lanes green). C++
  (`#include "asmtest_hwtrace.h"`) and Zig (`@cImport`) use the real header and were never affected.
  Surfaced by the adversarial review of the whole-window attribution work.

- **Node binding: 64-bit trace-call results above 2^53 were silently rounded.** Every
  fork/attach/stealth trace entry in the Node binding read the routine's return (its RAX at the
  `ret`) as `Number(readBigInt64LE(...))`, which rounds any value past `Number.MAX_SAFE_INTEGER`
  through the double mantissa — so a routine returning a full 64-bit hash/id/pointer came back
  wrong, contradicting the documented "BigInt out of safe range" contract and the OOP capture
  forms' exact-result guarantee. Added a `_safeInt` helper (Number when it fits the safe-integer
  range, else the exact BigInt) and applied it to all twelve result reads (`callScoped`,
  `stealthTrace`, `windowCall`, `stealthWindow`, `traceCall`/`traceCallBlockstep`/`traceCallEx`,
  and the `traceAttached*` family). Surfaced by the adversarial review of the whole-window work;
  regression-tested with a leaf returning `0x0102030405060708`.

- **Stealth stepper seized the wrong thread on a managed runtime (`getpid` → `SYS_gettid`).**
  `asmtest_hwtrace_stealth_trace` reverse-attached the helper to `getpid()` (the process leader),
  but on HotSpot the thread invoking the region is a JVM-created thread whose tid ≠ pid — so the
  helper single-stepped the wrong (idle primordial) thread and the `run_to` breakpoint fired on
  the **untraced** calling thread, killing the JVM with a fatal SIGTRAP (exit 133). Node and
  CoreCLR were unaffected only because their calling thread happens to be the leader (tid == pid).
  Fixed to seize `(pid_t)syscall(SYS_gettid)` — the calling thread — matching what the windowed
  variant `asmtest_hwtrace_stealth_trace_windowed` already did. Surfaced while adding the Java
  `stealthTrace` wrapper; after the fix Java captures a complete, exact stealth trace.

- **bindings-parity gate restored to green.** The block-step / whole-window / snapshot
  commits added eight tier symbols wrapped only in the .NET binding, leaving the
  `check-bindings-parity` CI gate failing with 75 missing (binding, symbol) pairs. The
  BTF block-step pair (`asmtest_ptrace_blockstep_available`,
  `asmtest_ptrace_trace_call_blockstep`) — siblings of the universally-wrapped
  `asmtest_ptrace_trace_call` — is now genuinely wrapped in all ten bindings, each with
  a self-skipping parity test asserting the block-step stream is byte-identical to the
  single-step stream. The managed-tier / C-level symbols (the §Z1 whole-window trio,
  §3.1(c) `attribute_window`, §D3 `trace_attached_windowed`, and the AMD boundary
  snapshot) carry reasoned allow-list exemptions following the file's existing
  conventions (the .NET tier keeps its real window-trio wraps).

- **`test_descent_stale_alarm_flag` no longer flakes on loaded CI runners.** The test
  spams the tracer with a 200 µs `SIGALRM` storm to prove a stale L3 watchdog flag +
  EINTRs cannot abort a healthy L2 descent — but it left the descent's real-time
  deadline at the 2 s default, which a loaded 2-core runner can legitimately exceed
  under 5000 interrupts/sec (a *correct* truncation, misread as the regression). The
  descent under test now carries an explicit 60 s deadline, so only the stale-flag
  bug it guards can fail it; the EINTR pressure is unchanged. (`asmtest_hwtrace_call_scoped`
  also joins the parity allow-list under the same dotnet-only posture as the
  window trio, restoring the gate the lazy-arm commit tripped.)

- **The scoped/windowed data-flow producers resolve r8d–r15b sub-register
  aliases.** `gp_value` (the register-file value reader in both
  `src/dataflow_ptrace.c` and `src/dataflow_blockstep.c`) and `dfp_alias_shape`
  (the F6 gap barrier's alias-slice classifier) had cases folding `eax/ax/al/ah`
  etc. to their 64-bit container but none for `r8d/r8w/r8b` .. `r15d/r15w/r15b` —
  a step that wrote one of those aliases produced a def-use record with no
  captured value (`value_valid` stayed false), and the gap barrier could not
  decide whether glue at risk had changed such a location (`truncated`). Both
  now fold every GP sub-register alias Capstone can emit on x86-64 to its
  container, exactly like the existing high-byte/32/16/8-bit cases.

- **The block-step tier no longer compares or records architecturally
  undefined EFLAGS bits as if silicon defined them.** A new explicit
  mnemonic(+count)-keyed table in `src/dataflow_blockstep.c`
  (`dfb_undef_flags`) masks the undefined bits an instruction leaves out of
  both the coherence canary (`regs_coherent`, accumulated per replayed
  instruction and reset per block) and every captured EFLAGS write record
  (`finalize_step`), on both the single-step oracle and the block-step+replay
  paths — preserving their byte-identical property by construction, since
  both flow through the same shared classification. Covers
  `and/or/xor/test` (AF), `mul/imul` (SF/ZF/AF/PF), `div/idiv` (all six),
  `bsf/bsr` (CF/OF/SF/AF/PF), count-dependent `shl/shr/sal/sar` and
  `rol/ror/rcl/rcr`, and `bt/bts/btr/btc` (OF/SF/AF/PF); an instruction
  outside the table that touches flags at all is treated as fully
  flag-defining, matching every other x86 arithmetic instruction. New test
  hooks `no_undef_mask` (disables both mask sites — the negative control) and
  `inject_flag_bit` (forces a chosen bit to disagree right before the canary)
  land with the tier's first opts-struct layout guard
  (`asmtest_dataflow_blockstep_opts_layout`). A dedicated `xor eax,eax`
  fixture — the AF-undefined case the tier's primary oracle fixture
  deliberately avoided — proves AF reads 0 in every post-xor EFLAGS record on
  both paths while the trace stays byte-identical, and the canary
  discrimination checks prove the mask, not luck, is what tolerates it (an
  injected AF divergence is tolerated; the same injection with
  `no_undef_mask` set is caught).

## [1.1.0] — 2026-07-06

### Fixed

- **Review-driven defect sweep (2026-07-02).** Resolved the full backlog from the
  [code-level review](https://github.com/wilvk/asm-test/blob/main/docs/internal/analysis/2026-07-02-code-review.md) (54 findings) and the
  still-open [2026-07-01](https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/reviews/2026-07-01-repo-review.md) /
  [2026-07-02](https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/reviews/2026-07-02-repo-review.md) repo-review items, with a
  per-batch implementation note under [`docs/summaries/`](https://github.com/wilvk/asm-test/tree/main/docs/summaries/). Highlights:
  AArch64 callee-saved `d8`–`d15` ABI checking + a corrected `vm.s`/`structparam.s`;
  `SKIP()` in SETUP/TEARDOWN reported as skip; JUnit XML made well-formed and no longer
  preceded by test stdout; hardware-trace truncation contract honored across the
  single-step / AMD-LBR / Intel-PT / code-image backends (block partition matches
  Unicorn/PT/DR); emulator SysV/AArch64 stack-and-register argument marshaling and a
  deterministic register reset per call on a reused handle; ptrace signal-forwarding,
  jitdump-truncation and tracee-reaping fixes; memory-safety and 64-bit-precision fixes
  across the Rust/Python/Node/Lua/C++/Java bindings; and Win64 runner teardown/DF/watchdog
  fixes. Build/CI: knob-aware object identity (`SAN`/`COV`/`ASM_SYNTAX`), header-prerequisite
  and PIC-object completeness, a `check-version` + third-party-version CI gate, publish
  tokens scoped to their step, the GPL corresponding-source release step, and a
  self-sufficient BTF-less eBPF fallback header. Added `.mailmap`.

### Added

- **Scoped in-process tracing for .NET — the managed tier (§Z0–§Z5, §D0).** The
  zero-config scope construct over the single-step hardware-trace tier:
  `using (new AsmTrace()) { … }` captures whatever the thread executes (no region,
  no `HwTrace.Init` — the ctor auto-inits the portable backend and self-skips with
  an honest `SkipReason` where it cannot run). `byMethod: true` labels the captured
  window by managed method via an in-process `MethodLoadVerbose` listener
  (`JitMethodMap`), and `withRundown: true` also names warm + ReadyToRun BCL
  methods through a dependency-free `DOTNET_IPC_V1` jitdump rundown over the
  runtime's own diagnostics socket (no NuGet package, no launch knob). Results are
  data-first (`Addresses`, `Methods`, `Disassembly`, `AsmMethod.Assembly/.Tier`),
  with `renderPath: true` as the rendered opt-in. Labelling decodes against the
  code-image **version live in the window** (the map feeds
  `asmtest_codeimage_track` per method load), so bodies that re-tier/move after
  the scope still render the bytes that ran.
- **Named-method form — `AsmTrace.Method(delegate)` (§D0.3).** Trace one managed
  method's own JIT'd body: resolution via `PrepareMethod` + the listener (jitdump
  rundown fallback for warm/R2R bodies), a region + step-over capture with exact
  offsets, and `Invoke(args…)` as the library-owned non-inlinable call site.
  `outOfProcess: true` (§D3) routes `Invoke` through the concealed ptrace-stealth
  stepper — a bundled helper reverse-attaches and steps the body out of band, so
  the calling thread is never armed with `EFLAGS.TF`.
- **Honest-degradation surface.** `HwTrace.DegradationNote()` composes the tier
  ladder (Intel PT → AMD LBR → single-step → CoreSight, each with its skip
  reason, plus the ptrace fallback); cross-thread closes and overflows flag
  `Truncated` (native OS-tid assert + a complementary managed-thread guard);
  `Disas.IsCall/IsBranch/IsRet/TryCallTarget` classify live instructions
  structurally. The packable `AsmTest` NuGet now ships `AsmTrace` and the whole
  hwtrace wrapper alongside the bundled native payload.
- **Eleven runnable .NET examples** under `examples/dotnet/` (whole-window,
  region, methods, rundown, assemblies, annotated, tiers, hotspots, coverage,
  callgraph, ptrace_native — plus the out-of-process `ptrace_dotnet` attach
  demo), each split Program/Report, wired into `make hwtrace-dotnet-example`
  and `make dev-dotnet`. Validated on .NET 8 **and** .NET 9 (no diagnostics-IPC
  or `MethodLoadVerbose` drift).
- **Single-step native-trace tier: macOS-Intel front-end.** The exact, unprivileged
  EFLAGS.TF (`#DB` → `SIGTRAP`) single-step backend now runs in-process on x86-64
  **macOS**, not just Linux — the first Phase-5 front-end a Linux CI host (or
  Docker-on-Mac, whose containers are Linux) cannot exercise. XNU delivers the
  single-step trap as a BSD `SIGTRAP`, so re-asserting `TF` in the saved thread state
  re-arms stepping across `sigreturn` exactly as on Linux; the only platform deltas are
  the feature-test macro (`_DARWIN_C_SOURCE`) and the mcontext field access
  (`uc_mcontext->__ss.__rip`/`__rflags` vs. `gregs[REG_RIP]`/`[REG_EFL]`), both isolated
  behind shims in `src/ss_backend.c`. `asmtest_hwtrace_available(SINGLESTEP)` now returns
  1 on x86-64 Darwin and the whole `asmtest_hwtrace_*` facade (region table, `init`/
  `register`, `begin`/`end`/`begin_scope`/`render_scope`) drives it; the `src/hwtrace.c`
  gate `HWTRACE_LIFECYCLE` is a superset of `__linux__`, so the Linux path is unchanged
  (verified: `make hwtrace-test` 61 pass on macOS; `make docker-hwtrace` 178 pass on
  Linux). The binding-facing W^X executable-memory helper
  (`asmtest_hwtrace_exec_alloc`/`_exec_free`, `src/hwtrace.c`) now runs on x86-64 Darwin
  too — its `PROT_NONE`→RW→RX `mmap`/`mprotect` path is plain POSIX and identical to the
  Linux one — so the per-binding single-step lanes are reachable natively on macOS, not
  just the C suite (verified on this host: `make hwtrace-{python,cpp,ruby}-test` pass,
  with the Linux-only ptrace/codeimage backends self-skipping). Off-platform hosts
  self-skip with "single-step backend is x86-64 Linux/macOS only (Windows/AArch64
  planned)".
- **Scoped in-process tracing (the `using`/RAII/`with` model).** A cooperative,
  developer-ergonomics face of the tracing machinery: bracket a region of a program's
  own code with a scope construct — `using (new AsmTrace())` in C#, RAII in C++/Rust,
  `with` in Python, `defer` in Go/Zig, a block/try-with-resources elsewhere — and get
  back the assembly that executed inside it, rendered on scope close. Implemented across
  **all ten language bindings** over a small shared C/decode core (error-returning
  `asmtest_hwtrace_try_begin`, arming-thread assert, `asmtest_hwtrace_render`,
  idempotent-by-name region registration, per-thread single-step state, a recorder-backed
  image adapter, symbolize-and-bucket, and the `asmtest_hwtrace_stitch` async-hop merge
  core). Linux-only; self-skips to a recorded no-op where no faithful backend is
  available. See [docs/scoped-tracing-implementation.md](https://github.com/wilvk/asm-test/blob/main/docs/scoped-tracing-implementation.md)
  and [docs/internal/archive/plans/scoped-inprocess-tracing-plan.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/plans/scoped-inprocess-tracing-plan.md).
  - **§Z0/§Z1 the aspirational empty-ctor form — `using (new AsmTrace())`.** A region-free
    whole-window scope with **no `NativeCode` and no `[base,len)`**: new C entry points
    `asmtest_hwtrace_begin_window`/`_end_window`/`_render_window` over a whole-window frame
    mode in `asmtest_ss_begin_window` (the single-step handler records ABSOLUTE RIPs into
    the bounded ring, overflow → `truncated`), rendered from live self memory. The .NET
    reference shim gains the parameterless `new AsmTrace()` ctor + `SkipReason` (honest
    self-skip). This is the single-step **WEAK** tier — native-leaf only, on any x86-64
    Linux (`test_wholewindow_singlestep`, `make docker-hwtrace` → 201/0; `.NET`
    `make docker-hwtrace-dotnet` → 33/0). The STRONG whole-window PT / AMD LBR tiers,
    arbitrary-managed-method capture, and the other nine binding shims remain forward-look.
    See [docs/internal/plans/scoped-tracing-zeroconfig-plan.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/scoped-tracing-zeroconfig-plan.md).
  - **§D3 concealed ptrace-stealth stepper — now a bundled standalone binary.** The
    hardware-free scope path (Zen 2 / Docker-on-Mac) reverse-attaches a helper to the
    caller (`PR_SET_PTRACER` + `PTRACE_SEIZE`) and single-steps the region out of band.
    Its stepping body + discovery moved to `src/stealth_helper.c` so the same code runs
    either as an in-process forked child (the fallback) or as the standalone
    **`asmtest-stealth-helper`** binary — a real separate process the managed packages can
    ship — which the caller discovers via a dladdr-sibling lookup (mirroring the
    DynamoRIO payload) or the `ASMTEST_STEALTH_HELPER` override, handing the shared trace
    over a memfd. New `$(BUILD)/asmtest-stealth-helper` build target +
    `install-stealth-helper`; `test_ptrace_scoped_stealth` asserts **both** paths
    reconstruct byte-identical offsets on any ptrace-capable Linux. The helper is
    **bundled into every managed package payload** (NuGet `runtimes/<rid>/native`,
    npm/Maven/gem/rock, the Python wheel `_libs/`) beside `libasmtest_hwtrace`,
    `$ORIGIN`-rpath'd so it resolves the co-vendored Capstone in-package, and asserted
    present + rpath'd (and not leaked into a darwin slot) by a fail-closed
    `package-libs-verify` gate. Only the live-JIT cross-process address channel (needs a
    running managed runtime) remains forward-look.
- **Call descent for the out-of-process ptrace tracer.** The single-step tracer
  (`asmtest_ptrace.h`) can now optionally FOLLOW the calls a traced region makes instead of
  only stepping over them, at four opt-in levels (`asmtest_descent_t`): `OFF` (today's
  behaviour), `RECORD_EDGES` (record each `call-site → callee` edge, still step over),
  `DESCEND_KNOWN` (single-step **into** resolvable callees — an allow-set of method regions
  or an optional resolver callback — stepping over the rest), and `DESCEND_ALL` (into
  everything, **default off**, denylist + instruction-budget + real-time-watchdog gated).
  The flat `asmtest_trace_t` is unchanged — it is always frame 0, byte-identical across all
  levels; descent records into a **separate opaque handle** read through scalar accessors
  (edges + nested per-callee frames), so `asmtest_trace_t` stays ABI-frozen and every binding
  adds accessor calls, not a struct layout. New entry points `asmtest_ptrace_trace_call_ex` /
  `_trace_attached_ex` / `_trace_attached_versioned_ex` thread the handle through the existing
  loops; the non-`_ex` symbols are unchanged (`descent == NULL`).
  - The descender is a return-address **shadow stack** with an exact pop predicate
    (`PC == ret_addr && SP == caller-pre-call-SP && the just-stepped insn is a return`) plus
    an SP-sweep for non-local exits (`longjmp`/unwind/`sigreturn`), same-region recursion as a
    distinct frame (with a recursion + `max_depth` cap), per-instruction byte windows via
    `process_vm_readv`, benign-signal forwarding on the live path, and a backend-owned
    `ITIMER_REAL`/`SIGALRM` watchdog so a blocked syscall in a descended callee self-truncates
    rather than hanging. AArch64 gained a `NT_ARM_HW_BREAK` hardware-breakpoint step-over path
    (the W^X JIT-heap fallback x86-64 already had). L3 is documented as **best-effort /
    expected-to-perturb** on a live managed runtime (the cross-thread lock-inversion deadlock
    vector is not fully mitigable) — see [analysis/jit-runtime-tracing.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/analysis/jit-runtime-tracing.md).
  - Surfaced in **all ten language bindings** (a `Descent` wrapper + descending `trace_call_ex`,
    with idempotent free and the per-FFI address/upcall hazards handled), pinned by a new
    `ptrace_descent` conformance-corpus tier and the header-grep parity gate; the resolver
    callback ships to the six upcall-safe FFIs (Python/Go/Node/Java/.NET/Lua) and Rust/Ruby/
    C++/Zig expose the allow-set only. New `jit_trace *-descend` / `*-descend-all` demo lanes
    (`make docker-hwtrace-jit-dotnet-bcl-descend`, …). See [docs/native-tracing.md](https://github.com/wilvk/asm-test/blob/main/docs/native-tracing.md)
    ("Call descent levels") and [docs/internal/archive/plans/call-descent-plan.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/plans/call-descent-plan.md).

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
    ([macOS clean-test plan](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/macos-clean-test-plan.md), Track A)
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
    plan](https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/plans/bundle-native-trace-tiers-plan.md))
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
    [trace parity matrix](https://github.com/wilvk/asm-test/blob/main/docs/internal/analysis/trace-parity-matrix.md)
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
    Linux x86-64 front of the [Zen 2 single-step plan](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/zen2-singlestep-trace-plan.md)
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
    [JIT-runtime-tracing analysis](https://github.com/wilvk/asm-test/blob/main/docs/internal/analysis/jit-runtime-tracing.md)
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
  [post-v1.0 expansion plan](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/post-v1-expansion-plan.md)
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
  [post-v1.0 expansion plan](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/post-v1-expansion-plan.md)
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
  [post-v1.0 expansion plan](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/post-v1-expansion-plan.md);
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

- **Track C disassembly reaches all ten bindings (via one superset lib).**
  Capstone disassembly was C-core-only — binding it naïvely would pull Capstone
  into every binding lib, or spawn a combinatorial lib matrix. Instead a single
  `libasmtest_emu` is the superset: `make shared-emu` links the emulator
  (`-lunicorn`) plus *both* optional native tiers (Keystone assembler **and**
  Capstone disassembler) into that one lib, so any binding that points
  `ASMTEST_LIB` at it gets disassembly and the assembler with no extra flag.
  Every binding gained `disas` / `disas_available` in its own idiom — **Python**
  (`ctypes`), **C++** (header, gated `ASMTEST_ENABLE_DISAS`), **Ruby** (`Fiddle`),
  **Lua** (LuaJIT `ffi`), **Node** (`koffi`), **Go** (`cgo` dlsym), **Rust**
  (`dlsym` + `extern`), **Zig** (`@cImport`, free), **Java** (FFM/Panama),
  **.NET** (P/Invoke) — wrapping `emu_disas` (decode one instruction at an offset
  to `"mnemonic operands"`) with a probe that self-skips against an older lib
  that lacks the tier.
  The per-binding `*-asm-test` checks now drive `libasmtest_emu`, so a single
  run exercises CallAsm **and** disas; the `bindings-asm` base image and
  `install-deps --asm` gained Capstone. Each binding's conformance decodes known
  x86-64 bytes (`xor rax, rax` / `ret` / `nop`), verified on the host
  (Python/C++/Ruby) and the Docker matrix (the other seven). Track C of the
  [post-v1.0 expansion plan](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/post-v1-expansion-plan.md).

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
  [post-v1.0 expansion plan](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/post-v1-expansion-plan.md).
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
  [post-v1.0 expansion plan](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/post-v1-expansion-plan.md);
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
  [post-v1.0 expansion plan](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/post-v1-expansion-plan.md).
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
  [post-v1.0 expansion plan](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/post-v1-expansion-plan.md).
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
  [post-v1.0 expansion plan](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/post-v1-expansion-plan.md).
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
  the [macOS clean-test plan](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/macos-clean-test-plan.md)
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
  [binding-parity plan](https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/plans/binding-parity-plan.md).

- **In-line assembler tier (Keystone).** Pass a routine as an *assembly string*
  and run it, instead of only as pre-assembled object code. `asmtest_assemble()`
  (in the new `include/asmtest_assemble.h`) turns text into machine code for the
  emulator's guest set — x86-64 (Intel or AT&T syntax), AArch64, ARM32, and
  RISC-V where the linked Keystone supports it — with errors reported as data and
  output the caller frees via `asmtest_asm_free()`. Bridge wrappers `emu_call_asm`
  / `emu_arm64_call_asm` / `emu_riscv_call_asm` / `emu_arm_call_asm` assemble at
  the emulator's load base (so PC-relative and branch targets resolve) and run
  through the matching `emu_*_call` in one call. Optional and pkg-config gated
  like the emulator tier, and folded into the superset `libasmtest_emu` (built by
  `make shared-emu`, which links `libkeystone` + `libcapstone` + `libunicorn`):
  `make asm-test` builds the standalone in-line-assembler suite, with
  `make docker-asm` and a CI `asm` job on both x86-64 and arm64.
  Keystone has no Linux distro package, so `make deps DEPS_ARGS=--asm` points at
  `scripts/build-keystone.sh` (a pinned source build the CI job and Docker image
  use). RISC-V in-line assembly self-skips until a Keystone release ships a
  RISC-V backend (none does yet). See the
  [implementation plan](https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/plans/inline-asm-keystone-plan.md).

- **In-line assembler reaches every binding, with a widened shim.** All **ten**
  bindings now expose the assembler — the original five (.NET, Ruby, Lua, Node,
  Java) plus Python, Go, Rust, C++, and Zig — bound *optionally* so they self-skip
  against an older lib that lacks the assembler and pay no cost in the normal
  binding images. The dlopen bindings probe the symbol; Go and Rust resolve it
  through the libc dynamic loader (they statically link the plain lib); C++ and
  Zig link the assembler-carrying `libasmtest_emu` directly. The opaque-handle shim is widened from the original
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
  and the [implementation plan](https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/plans/win64-native-tier-plan.md).

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
  [docs/internal/archive/plans/multi-language-bindings-plan.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/plans/multi-language-bindings-plan.md).
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

### Changed

- **Consolidated the AMD native-tracing docs (design-doc curation).** The AMD tracing
  story was split across four overlapping files; the three genuinely-AMD ones — the AMD
  LBR snapshot backend plan, the improvement analysis, and the improvement
  implementation plan — are merged into a single [`docs/internal/plans/amd-tracing-plan.md`](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/amd-tracing-plan.md)
  with three parts (shipped LBR backend / improvement analysis / improvement roadmap),
  collapsing two duplicated "governing constraint" + "implementation status" preambles
  into one. All ~9 references (the `src/amd_backend.c` header comment and the sibling
  plans / parity matrix) repoint to the merged file; the vendor-neutral single-step
  ("Zen 2") plan stays separate. The `docs/internal/analysis/trace-parity-matrix.md` overlap the
  review also flagged was assessed and **left intact**: its matrices are
  trace-specialized (not restatements of `docs/features.md`) and its "Matrix N"
  numbering is cited from `src/trace_auto.c` / `include/asmtest_trace_auto.h`, so
  trimming would lose detail and dangle those code references. Addresses review
  finding #15.

### Fixed

- **`DRAPP_KEYSTONE` is now part of `drtrace_app.o`'s build identity.** The app-side
  object was compiled with `$(DRAPP_KS_DEF)` (`-DASMTEST_HAVE_KEYSTONE`, gated by the
  `DRAPP_KEYSTONE` knob) but that flag was not among the rule's prerequisites, so
  flipping the knob between sub-makes in the **same** `build/` tree reused the stale
  object. This is not hypothetical: the per-binding native-trace lanes invoke
  `$(MAKE) shared-drtrace … DRAPP_KEYSTONE=0` precisely because a Keystone-enabled
  drapp `.so` has unresolved `emu_*` symbols and won't `dlopen` — so a reused
  Keystone-on object silently breaks exactly those lanes (CI only escaped it by running
  each in a clean tree). Both `drtrace_app.o` rules (static + PIC) now depend on a
  `$(BUILD)/.drapp-flags` sentinel that records the full compile-flag string and is
  rewritten only when it changes (`cmp` guard, so no spurious rebuilds), folding
  Keystone — and by extension `SAN`/`COV` via `CFLAGS` — into the object's identity.
  Verified: no rebuild when nothing changes, a rebuild the moment the Keystone define
  or any `CFLAGS` knob flips. (The related `make -j` race on the aggregate
  `drtrace-bindings-test` targets — concurrent sub-makes sharing one `build/` — is a
  separate concern and not addressed here.)

- **Version sync now covers the C header, not just the binding manifests.**
  `include/asmtest.h` pins the version in four macros (`ASMTEST_VERSION_MAJOR/MINOR/
  PATCH` + the `ASMTEST_VERSION` string), and `scripts/amalgamate.sh` derives the
  single-header version *from that header* — but `scripts/sync-version.sh` /
  `make check-version` only touched the nine binding manifests. So a version bump plus
  `make sync-version && make check-version` passed **green** while `ASMTEST_VERSION`,
  `ASMTEST_VERSION_NUM`, and the amalgamated `asmtest_single.h` all stayed at the old
  version — an unchecked second source of truth. `sync-version.sh` now writes and
  checks the four header macros too (splitting `VERSION` into numeric MAJOR/MINOR/PATCH,
  and requiring an exact 3-part numeric semver). Verified by round-tripping a bump: the
  header, `ASMTEST_VERSION_NUM`, and the amalgamation all follow, and `check-version`
  fails on a stale header.

- **RNG `asmtest_rng_range` divided by zero on ranges wider than `LONG_MAX`.**
  `asmtest_rng_range(rng, LONG_MIN, LONG_MAX)` — a natural draw in differential /
  property testing — computed the span as `(uint64_t)(hi - lo) + 1`, where `hi - lo`
  signed-overflows `long` (UB) and the wrap makes `(uint64_t)(hi-lo)+1` evaluate to
  `0`, so the following `% span` was an integer division by zero → **SIGFPE** (a hard
  crash, or a spurious "crashed by signal 8" under `--fork`). The span is now computed
  entirely in `uint64_t` (wraps to 0 only for the full 2^64 width, which is special-
  cased to "every value is in range"), and the offset is added in `uint64_t` too so a
  large offset can't overflow the `lo + …` back through signed. New self-tests
  `posit.rng_range_full_width_no_sigfpe` / `_wide_in_bounds` / `_narrow_in_bounds`.

- **Signed-overflow UB in the floating-point ULP-distance helpers.** `fp_ulp_distance`
  / `fp_ulp_distance_f` computed the gap between the sign-magnitude-mapped keys with a
  signed `ia - ib`, which overflows `int64_t`/`int32_t` for far-apart operands (e.g.
  `ASSERT_DNEAR(-DBL_MAX, DBL_MAX, …)`, ~1.84e19 ULPs). It yielded the right magnitude
  on two's-complement hardware but is UB — and **halted this repo's own `make sanitize`
  (UBSan, `halt_on_error=1`) lane**, so it was self-inconsistent. The larger-minus-
  smaller gap is now taken in `uint64_t`/`uint32_t` (bit-identical result, no UB; the
  signed comparison that picks the larger key is unchanged, so `-0.0`/`+0.0` still
  collapse to a zero-ULP distance). New self-test `posit.near_full_range_no_overflow`.

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

[Unreleased]: https://github.com/wilvk/asm-test/compare/v1.1.0...HEAD
[1.1.0]: https://github.com/wilvk/asm-test/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/wilvk/asm-test/releases/tag/v1.0.0
