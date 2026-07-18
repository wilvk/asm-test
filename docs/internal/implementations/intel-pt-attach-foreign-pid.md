# Intel PT attach-to-foreign-PID capture, facade dispatch, and the hypervisor/EPT frontier — implementation

> **Sources.** Actioned from
> [hardware-trace-plan.md](../plans/hardware-trace-plan.md) (items HWT-PT-ATTACH,
> HWT-PT-WIRE, HWT-HV-EPT) and
> [jit-runtime-tracing.md](../analysis/jit-runtime-tracing.md). Written
> 2026-07-17. If this doc and a source disagree, this doc wins (sources may be
> stale); if the CODE and this doc disagree, re-verify before implementing.

## Why this work exists

asm-test can already **decode** an Intel PT trace of a foreign JIT's code back
into an ordered instruction/block stream — that entry point
(`asmtest_pt_decode_window`) is written and CI-validated against a synthetic
packet stream — but nothing **produces** a real PT trace from a *running*
process. Phase 1 only ever opens a PT event on `pid=0` (the calling thread). This
doc adds the missing producer: open `perf_event_open` on the `intel_pt` PMU
against a **foreign pid**, pair it with a temporal code-image recorder for that
process, drain the AUX ring honestly, and hand the result to the existing decode.
The user-visible outcome is: attach to a live .NET / JVM / V8 process and get a
deterministic disassembled trace of a JIT-generated routine, out of band, with no
in-process agent and no code-cache collision.

## What already exists (verified 2026-07-17)

This doc builds directly on the self-trace PT substrate. That substrate — the
shared perf-AUX helpers, the `begin_window` PT arm for `pid==0`, the
WEAK/STRONG/CEILING ladder, and the native `pt_begin_window`/`pt_end_window`
pair — is owned by
[intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md); **do
not reimplement any of it here.** In particular that doc's T1 extracts
`pt_aux_open(pid_t pid, size_t data_size, size_t aux_size, uint32_t
aux_watermark, int snapshot, pt_aux_t *out)` and parameterizes it on `pid` *and*
`aux_watermark` *specifically so this doc can reuse it for `pid>0` with a prompt
wakeup watermark and no second open arm* — that is the contract this doc
consumes.

Verified landed surface this doc touches:

- [src/hwtrace.c](../../../src/hwtrace.c) — the hardware-trace facade.
  - **Every `perf_open` call site passes `pid=0`** (the self-trace of the calling
    thread): confirmed at [:260](../../../src/hwtrace.c#L260),
    [:302](../../../src/hwtrace.c#L302), [:834](../../../src/hwtrace.c#L834),
    [:838](../../../src/hwtrace.c#L838), [:1355](../../../src/hwtrace.c#L1355),
    [:1616](../../../src/hwtrace.c#L1616), [:1890](../../../src/hwtrace.c#L1890).
    No foreign-pid open exists anywhere in the tree.
  - The region-scoped PT open/mmap sequence to mirror lives at
    [:1878-1939](../../../src/hwtrace.c#L1878): `pmu_type()`
    ([:145](../../../src/hwtrace.c#L145), reads
    `/sys/bus/event_source/devices/intel_pt/type`) → `perf_event_attr`
    (`exclude_kernel=1`, `exclude_hv=1`, `disabled=1`) → `perf_open(&attr, 0, -1,
    -1, 0)` → data mmap → set `mp->aux_offset`/`mp->aux_size` → AUX mmap
    (`PROT_READ|PROT_WRITE` linear, `PROT_READ` circular when `opts.snapshot`) →
    `IOC_RESET` + `IOC_ENABLE`.
  - `aux_data_ring_truncated()` ([:1971](../../../src/hwtrace.c#L1971)) scans the
    data ring for `PERF_RECORD_AUX` records and sets truncated on
    `PERF_AUX_FLAG_TRUNCATED`. The **truncation policy** to mirror is the two-line
    `if (r->trace != NULL && (overflow || rc != ASMTEST_HW_OK)) r->trace->truncated
    = true;` at [:2082](../../../src/hwtrace.c#L2082).
  - `asmtest_hwtrace_begin_window` self-skips every non-single-step backend at
    [:2585-2587](../../../src/hwtrace.c#L2585) (`return ASMTEST_HW_EUNAVAIL`).
  - `asmtest_hwtrace_init` refuses a backend whose `asmtest_hwtrace_available()`
    is 0 ([:594-595](../../../src/hwtrace.c#L594)); for `INTEL_PT`, `available()`
    requires `vendor_is("GenuineIntel")` ([:212](../../../src/hwtrace.c#L212))
    **and** the `intel_pt` sysfs PMU node. On the AMD dev boxes no facade path can
    reach PT code — this is by construction, and tests must not fake the gate.
- [src/pt_backend.c](../../../src/pt_backend.c) — decode-only, gated on
  `-DASMTEST_HAVE_LIBIPT` ([:65](../../../src/pt_backend.c#L65); the `#else` at
  [:361](../../../src/pt_backend.c#L361) compiles `ENOSYS` stubs).
  `asmtest_pt_decode_window` ([:224](../../../src/pt_backend.c#L224) real body,
  [:375](../../../src/pt_backend.c#L375) stub) decodes an AUX blob against a
  code-image recorder as of `when`, recording offsets from the first decoded IP;
  `asmtest_pt_read_codeimage` ([:50](../../../src/pt_backend.c#L50)) and
  `read_recorder` ([:99](../../../src/pt_backend.c#L99)) are the temporal byte
  adapters; `asmtest_pt_encode_fixture` ([:305](../../../src/pt_backend.c#L305))
  is the userspace fixture generator. **There is no capture code and no
  `PERF_EVENT_IOC_SET_FILTER` wiring in this file** — the filter exists only in a
  comment.
- [src/codeimage.c](../../../src/codeimage.c) /
  [include/asmtest_codeimage.h](../../../include/asmtest_codeimage.h) — the
  time-aware code-image recorder. `asmtest_codeimage_new(pid)` (`pid==0` = self,
  a foreign pid tracks another process via soft-dirty / `PAGEMAP_SCAN` +
  `process_vm_readv`), `asmtest_codeimage_now()`
  ([:102](../../../include/asmtest_codeimage.h#L102), the monotonic capture
  sequence), and `asmtest_codeimage_bytes_at(img, addr, when, …)`
  ([:110](../../../include/asmtest_codeimage.h#L110)) — the `when` source
  `asmtest_pt_decode_window` consumes.
- [src/ptrace_backend.c:134](../../../src/ptrace_backend.c#L134)
  `asmtest_jitdump_find(path, pid, name, …)` — the binary jitdump reader that
  recovers a method's recorded code bytes from `/tmp/jit-<pid>.dump` (path=NULL
  resolves that name). **Landed and validated.**
- **Runtime-jitdump byte-recovery lanes already exist — but only via
  *launch-time* enablement.**
  [examples/jit_trace.c](../../../examples/jit_trace.c)'s `trace_jitdump`
  ([:429](../../../examples/jit_trace.c#L429)) drives them:
  [mk/native-trace.mk](../../../mk/native-trace.mk) `hwtrace-jit-jitdump`
  ([:2226](../../../mk/native-trace.mk#L2226), V8 via `node --perf-prof`),
  `hwtrace-jit-dotnet-jitdump` ([:2256](../../../mk/native-trace.mk#L2256),
  CoreCLR via the launch env `DOTNET_PerfMapEnabled=1`), and
  `hwtrace-jit-java-jitdump` ([:2316](../../../mk/native-trace.mk#L2316), HotSpot
  via the `PERF_JVMTI` agent probed at
  [:2214](../../../mk/native-trace.mk#L2214)). All three set the flag **at
  process start**. Enabling jitdump on an *already-running foreign* process (the
  attach case) is the new slice below.
- [examples/attachprof_probe/attacher/Program.cs](../../../examples/attachprof_probe/attacher/Program.cs)
  — the .NET **diagnostic-port attach** pattern already in-tree:
  `new DiagnosticsClient(pid).AttachProfiler(...)` over the
  `dotnet-diagnostic-<pid>` socket, targeting an already-running process. Its
  [attacher.csproj](../../../examples/attachprof_probe/attacher/attacher.csproj)
  references `Microsoft.Diagnostics.NETCore.Client` 0.2.510501 on `net8.0`, and
  [Dockerfile.attachprof-probe](../../../Dockerfile.attachprof-probe) installs
  `dotnet-sdk-8.0`. This is the harness `EnablePerfMap` extends.
- [Dockerfile.hwtrace](../../../Dockerfile.hwtrace) installs `libipt-dev` (line
  21) on the `ubuntu:24.04`-based bindings base
  ([Dockerfile.bindings-base:15](../../../Dockerfile.bindings-base#L15) `ARG
  BASE=ubuntu:24.04`), so the hwtrace lane already builds `pt_backend.o` with
  `-DASMTEST_HAVE_LIBIPT` (libipt v2.0.6). **No new decoder dependency is needed
  for the PT half of this doc.**

**Prove the baseline green before touching anything:**

```sh
make docker-hwtrace                 # hwtrace-test + codeimage-test in a plain container
make docker-hwtrace-jit-dotnet-jitdump   # launch-time .NET jitdump byte recovery
make check                          # framework self-tests
```

Expected: `hwtrace-test` ends with a TAP `# N passed, 0 failed` and `# SKIP`
lines for the PT/AMD live paths on this AMD host; `test_wholewindow_decode` is
green (libipt is in the image); the jitdump lane prints
`jitdump: asmtest_jitdump_find recovered a real JIT method's ...`. Run `make
help` to see every target.

## Tasks

### T1 — Foreign-pid PT AUX capture context  (M, depends on: intel-pt-whole-window-substrate.md#T1)

**Goal.** Open, enable, and tear down an `intel_pt` perf-AUX capture against a
*foreign* `pid>0`, reusing the substrate's `pt_aux_open` helper with no parallel
open sequence.

**Steps.**

1. In [src/hwtrace.c](../../../src/hwtrace.c), add a foreign-attach capture
   context and its lifecycle next to the AMD begin/end split, built on the
   substrate's `pt_aux_open`/`pt_aux_stop`/`pt_aux_close` helpers (see
   [intel-pt-whole-window-substrate.md#T1](intel-pt-whole-window-substrate.md)).
   Do **not** copy the open sequence — call `pt_aux_open(pid, data_size,
   aux_size, /*aux_watermark=*/aux_size/4, /*snapshot=*/0, &ctx->aux)` with a
   real `pid` (step 2 explains the watermark).

   ```c
   typedef struct {
       pid_t   pid;
       pt_aux_t aux;                 /* fd + data ring + AUX ring (substrate T1) */
       asmtest_codeimage_t *img;     /* foreign code-image recorder (T2)         */
       int     have_filter;          /* an object-file hardware filter was set    */
   } pt_attach_t;

   int  asmtest_hwtrace_pt_attach_begin(pid_t pid, const char *obj_hint,
                                        pt_attach_t **out);
   int  asmtest_hwtrace_pt_attach_poll (pt_attach_t *a, int timeout_ms,
                                        int *truncated_out);
   int  asmtest_hwtrace_pt_attach_end  (pt_attach_t *a, uint64_t when,
                                        asmtest_trace_t *trace);   /* → T4       */
   ```

2. `asmtest_hwtrace_pt_attach_begin`: first
   `if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_INTEL_PT)) return
   ASMTEST_HW_EUNAVAIL;` — this is what makes the off-Intel path a clean
   self-skip, exactly as the AMD sampler and the substrate's window pair do.
   Then call the substrate helper — **do not populate a `perf_event_attr` or
   call `perf_open` here; there is exactly one Intel PT open sequence in the
   tree and it lives in `pt_aux_open`.** Pass a nonzero **`aux_watermark`** of a
   small fraction of `aux_size` (e.g. `aux_size/4`) so the kernel raises a
   `PERF_RECORD_AUX` / `poll()` wakeup promptly while the foreign target runs
   rather than only when the ring is half full (the kernel default the
   self-trace path gets from `aux_watermark=0`):

   ```c
   int rc = pt_aux_open(pid, data_size, aux_size,
                        /*aux_watermark=*/aux_size / 4,
                        /*snapshot=*/0, &ctx->aux);   /* RW-linear, per-task */
   ```

   `pt_aux_open` already opens the event `cpu=-1` (a **per-task** event, which
   the foreign attach and the step-4 address filter both require) and maps the
   AUX ring RW-linear when `snapshot==0`, so the drain is a straight
   `[aux_tail, aux_head)` copy. Extending `pt_aux_open` with the `aux_watermark`
   parameter is owned by
   [intel-pt-whole-window-substrate.md#T1](intel-pt-whole-window-substrate.md);
   this doc only consumes it.
3. When `pt_aux_open` returns nonzero it has already unwound fully; classify the
   still-set `errno` for the caller's message: `EACCES`/`EPERM` → the process
   lacks `CAP_PERFMON` (Linux 5.9+) or same-uid ptrace access to the target —
   return `ASMTEST_HW_EUNAVAIL` with a message naming `CAP_PERFMON`, mirroring
   the AMD `NOPERM` branch at [:271](../../../src/hwtrace.c#L271). Do **not**
   attempt to lower a host sysctl. (`pt_aux_open` preserves `errno` from the
   failing `perf_open`/`mmap` on its unwind path — the substrate helper does not
   clobber it before returning; if a future refactor makes that unsafe, thread
   an `int *errno_out` through the helper rather than re-opening here.)
4. If `obj_hint` names a real backing object file for the region of interest,
   set a hardware address filter with `ioctl(fd, PERF_EVENT_IOC_SET_FILTER,
   "filter <off>/<size>@<obj>")` and record `have_filter=1`. **Critical
   constraint from the kernel (see Research notes): hardware address filters
   match only file-backed VMAs by inode — anonymous JIT pages cannot be
   address-filtered.** So for a foreign JIT the filter covers only the loader /
   `.so` regions; the JIT-generated code itself is captured whole and
   IP-post-filtered at decode. When `obj_hint` is NULL or the region is
   anonymous, skip the ioctl and leave `have_filter=0` (whole-thread capture,
   software post-filter in T4).
5. Enable with `IOC_RESET` + `IOC_ENABLE`. Run `make fmt` then `make
   docker-hwtrace` — everything must stay green (the new code compiles into the
   TU but self-skips on the AMD lane).

**Code.** The ctx is heap-allocated (`calloc`); `attach_end` and every error
path in `attach_begin` must `pt_aux_close` and `free` so no fd/mmap ever leaks
(match the region path's full unwind at
[:1917-1931](../../../src/hwtrace.c#L1917)). Sizes default to the shipped
options: `data_size` 8 KiB, `aux_size` 64 KiB
([asmtest_hwtrace.h:96](../../../include/asmtest_hwtrace.h#L96)); a foreign
long-running target wants a larger `aux_size` — take it from `g_opts.aux_size`
when the tier is inited, else the default.

**Tests.** Add `test_pt_attach_selfskip` to
[examples/test_hwtrace.c](../../../examples/test_hwtrace.c) (register in `main`
next to `test_wholewindow_decode`): assert `asmtest_hwtrace_pt_attach_begin(pid,
NULL, NULL)` returns `ASMTEST_HW_EINVAL` (NULL out); assert that where
`asmtest_hwtrace_available(ASMTEST_HWTRACE_INTEL_PT)` is 0 (this AMD host)
`attach_begin(getpid(), NULL, &a)` returns `ASMTEST_HW_EUNAVAIL` with `a ==
NULL`, and print `# SKIP pt attach: no Intel PT on this host`. A failure is a
`not ok` TAP line; a pass keeps `0 failed`. The live-capture half is validated
only under the hardware gate (T4's live lane).

**Docs.** Internal-only — the capture is not yet reachable from any binding
(that arrives in T4). No changelog entry until T4.

**Done when.**

- `make docker-hwtrace` green; `grep -n "perf_open(&.*, pid" src/hwtrace.c` shows
  the foreign open routed through `pt_aux_open`, not a hand-rolled sequence.
- On this AMD host `test_pt_attach_selfskip` prints its `# SKIP` and passes.
- `make fmt-check` passes.

### T2 — Pair the capture with a live foreign code-image recorder  (S, depends on: T1)

**Goal.** During a foreign attach, maintain a temporal code-image of the
target's executable memory so the decode in T4 has the exact bytes live at each
trace position.

**Steps.**

1. In `asmtest_hwtrace_pt_attach_begin`, after the perf event is open, create
   `a->img = asmtest_codeimage_new(pid)` for the **foreign** pid (not `pid==0`).
   If the codeimage substrate is unavailable
   ([asmtest_codeimage_available](../../../src/codeimage.c), soft-dirty /
   `PAGEMAP_SCAN` absent) leave `a->img = NULL` — decode then requires a
   caller-supplied image in T4.
2. Track the region(s) of interest with `asmtest_codeimage_track(a->img, base,
   len)` where known; otherwise rely on the optional eBPF emission detector
   (`asmtest_codeimage_watch_bpf` /
   [poll_bpf](../../../include/asmtest_codeimage.h#L157)) to snapshot on the
   `PROT_EXEC` edge. This is exactly the cross-process recorder the analysis doc
   calls the "buildable core"
   ([jit-runtime-tracing.md §2](../analysis/jit-runtime-tracing.md)); it is
   already live-validated on x86-64 — this task only *pairs* it with the foreign
   PT capture, it does not modify the recorder.
3. In `asmtest_hwtrace_pt_attach_poll`, after each drained AUX chunk call
   `asmtest_codeimage_refresh(a->img)` so a page patched *after* it was made
   executable is re-snapshotted before the trace position that reads it — the
   write-after-protect race the analysis doc flags. Capture the current
   `asmtest_codeimage_now(a->img)` as the `when` to stamp that chunk against.

**Code.** No decoder change — `asmtest_pt_decode_window` already takes `(img,
when)`. The pairing is purely lifecycle: own `a->img`, refresh it on the drain
cadence, free it in `attach_end`. Because the recorder needs `process_vm_readv`,
the foreign attach requires `CAP_SYS_PTRACE` (or the target opting in via
`PR_SET_PTRACER_ANY`, the pattern the
[docker-hwtrace-attach-demo](../../../mk/docker.mk) lane already uses) *in
addition to* `CAP_PERFMON` for the PT event.

**Tests.** Extend `test_pt_attach_selfskip`: on this host assert that when the
codeimage substrate *is* available, `attach_begin` still self-skips on the perf
gate (PT unavailable) and frees any `img` it created — i.e. no leak when only
half the stack is present. The recorder's own temporal correctness
(same-address-different-bytes) is already covered by
`asmtest_pt_read_codeimage`'s host test at
[examples/test_hwtrace.c:3117](../../../examples/test_hwtrace.c#L3117); do not
duplicate it.

**Docs.** Internal-only; folded into the T4 user-facing note.

**Done when.**

- `make docker-hwtrace` green; the recorder is created and freed on every attach
  path (verify with a leak-sanitized run of the self-skip test:
  `# no leaks` under the existing ASan-enabled `hwtrace-test` build if present,
  else a manual `attach_begin`→`attach_end` loop asserting stable RSS).

### T3 — Runtime-enabled jitdump byte recovery on a *live* process  (M, depends on: none)

**Goal.** Turn on jitdump emission in an **already-running** foreign process and
recover a JIT method's bytes with the existing `asmtest_jitdump_find`, without
having launched the target with a flag. This is the byte-source half of the
foreign attach and — unlike the PT capture — **needs no Intel PT hardware**, so
it is testable on any Linux host.

**Steps.**

1. **.NET (best path).** Extend the diagnostic-port attacher pattern at
   [examples/attachprof_probe/attacher/Program.cs](../../../examples/attachprof_probe/attacher/Program.cs)
   with a sibling command that calls
   `new DiagnosticsClient(pid).EnablePerfMap(PerfMapType.JitDump)`
   (`Microsoft.Diagnostics.NETCore.Client`, .NET 8+). CoreCLR re-emits every
   already-JITted method on enable, writing `/tmp/jit-<pid>.dump` for the running
   process. Add a make lane `hwtrace-jit-dotnet-attach-jitdump` in
   [mk/native-trace.mk](../../../mk/native-trace.mk) mirroring
   `hwtrace-jit-dotnet-jitdump` ([:2256](../../../mk/native-trace.mk#L2256)) but
   **without** `DOTNET_PerfMapEnabled=1` in the launch env — instead start the
   victim plain, then run the attacher against its pid, then recover
   `Program::Add`'s bytes with `asmtest_jitdump_find(NULL, pid, "Add", …)` and
   validate them against ground truth exactly as the launch-time lane does.
2. **JVM.** Attach the perf JVMTI agent to a running HotSpot with `jcmd <pid>
   JVMTI.agent_load <libperf-jvmti.so>` (the `PERF_JVMTI` path already probed at
   [mk/native-trace.mk:2214](../../../mk/native-trace.mk#L2214)); the agent's
   `CompiledMethodLoad` + a `GenerateEvents(COMPILED_METHOD_LOAD)` replay writes
   the running process's `/tmp/jit-<pid>.dump`. The target must run with
   `-XX:+EnableDynamicAgentLoading` (JEP 451) or the load is refused. Add
   `hwtrace-jit-java-attach-jitdump` mirroring
   `hwtrace-jit-java-jitdump` ([:2316](../../../mk/native-trace.mk#L2316)).
3. Wire docker lanes `docker-hwtrace-jit-dotnet-attach-jitdump` and
   `docker-hwtrace-jit-java-attach-jitdump` in
   [mk/docker.mk](../../../mk/docker.mk) alongside the existing
   `docker-hwtrace-jit-*-jitdump` targets, reusing the `asmtest-dotnet` /
   `asmtest-java` images (no new base image, no privilege — attaching to one's
   own child needs none; the diagnostic socket and `jcmd` both work in a plain
   container).

**Code.** The C side reuses `asmtest_jitdump_find` unchanged — the only new C is
the lane driver in `jit_trace.c` if a distinct `dotnet-attach-jitdump` /
`java-attach-jitdump` mode is cleaner than an env toggle. V8/Node has **no
runtime-enable path** (the `--perf-prof` logger is wired once at isolate init —
see the analysis doc's matrix), so there is deliberately no Node attach lane;
record that as an inherent runtime limitation, not a gap.

**Tests.** The two new make lanes ARE the tests: each asserts
`asmtest_jitdump_find` recovered a real method's bytes from a process that was
**not** launched with the flag, and that they match the live code / perf-map. A
failure prints `# SKIP jitdump (...): ...` (runtime absent) or a hard assertion
mismatch; a pass prints the same
`jitdump: asmtest_jitdump_find recovered ...` line the launch-time lanes emit.
These run green on any host with the runtime installed — **no Intel PT gate**.

**Docs.** Add an `## [Unreleased]` → `### Added` entry to
[CHANGELOG.md](../../../CHANGELOG.md): "Runtime-enabled jitdump byte recovery —
turn on `.NET`/HotSpot jitdump emission in an already-running process
(`DiagnosticsClient.EnablePerfMap(JitDump)` / `jcmd JVMTI.agent_load`) and
recover a method's bytes without a launch flag." Update the runtime-enablement
matrix note in
[jit-runtime-tracing.md](../analysis/jit-runtime-tracing.md) only if it drifts;
prefer leaving the analysis doc as design rationale.

**Done when.**

- `make docker-hwtrace-jit-dotnet-attach-jitdump` and
  `make docker-hwtrace-jit-java-attach-jitdump` both recover a method's bytes
  from a plain-launched victim and pass.
- On a host without the runtime the lanes self-skip cleanly with a printed
  reason.

### T4 — Facade dispatch of `asmtest_pt_decode_window` for the foreign path (HWT-PT-WIRE)  (S, depends on: T1, T2)

**Goal.** Give `asmtest_pt_decode_window` a *production* caller for the
foreign-attach path: `asmtest_hwtrace_pt_attach_end` dispatches the drained
foreign AUX blob + the paired recorder image into the decode and applies the
truncation policy. (The **self-trace** `pid==0` production caller is
`begin_window`/`end_window`, owned by
[intel-pt-whole-window-substrate.md#T2](intel-pt-whole-window-substrate.md) — do
not re-wire it here. This task adds the *foreign* caller, reusing the same decode
entry so there is one decode, not two.)

**Steps.**

1. In `asmtest_hwtrace_pt_attach_poll`, drain the linear AUX ring: read
   `mp->aux_head`, issue the acquire barrier (`__sync_synchronize()`), copy
   `[aux_tail, aux_head)` out of the AUX ring into a linearized buffer (two
   memcpys across the wrap — see Research notes), then release-store `aux_tail =
   aux_head`. Scan the data ring for `PERF_RECORD_AUX` with
   `PERF_AUX_FLAG_TRUNCATED` using a `(base_map, base_sz)`-parameterized
   `aux_data_ring_truncated` (the substrate T1 refactor) and OR the result into
   `*truncated_out`.
2. In `asmtest_hwtrace_pt_attach_end`, after `pt_aux_stop` (`IOC_DISABLE` +
   final drain), decode the accumulated linearized AUX with
   `asmtest_pt_decode_window(aux, aux_len, a->img, when, trace)` (using the
   caller-supplied `img` if `a->img` is NULL). If `have_filter == 0`, apply the
   **software IP post-filter** — drop decoded IPs outside the region of interest,
   since a whole-thread capture without a hardware address filter records the
   loader and runtime too (the fallback the plan calls out for anonymous JIT
   memory).
3. Apply the truncation policy in the two-line shape at
   [src/hwtrace.c:2082](../../../src/hwtrace.c#L2082):

   ```c
   int rc = asmtest_pt_decode_window(aux, aux_len, img, when, trace);
   if (trace != NULL && (overflow || rc != ASMTEST_HW_OK))
       trace->truncated = true;
   ```

4. Expose the attach entry to the .NET binding as an attach-by-pid method
   parallel to the region API (a thin P/Invoke over `attach_begin` / `poll` /
   `end`); keep the decoder out of the binding surface, consistent with the
   plan's report-format reuse — the binding gets offsets + Capstone-rendered
   text, never a libipt type.

**Code.** No change to `asmtest_pt_decode_window` beyond what the substrate T2
already does (the `base_ip_out` out-param); this task only *calls* it from a
foreign producer. The whole dispatch is unreachable on a non-Intel host by the
`available()` gate — that is intended, and it is why the live assertion lives
behind the hardware gate below rather than in a portable unit test.

**Tests.** Two layers:
- **Portable (any host):** a unit test of the software IP post-filter and the
  AUX de-wrap (feed a hand-built ring with a synthetic wrap and assert
  `[aux_tail, aux_head)` linearizes correctly and out-of-region IPs are dropped)
  — these are pure logic, no PMU. Add to
  [examples/test_hwtrace.c](../../../examples/test_hwtrace.c).
- **Hardware-gated:** on a bare-metal Intel PT host, extend the live path so an
  attach to a child running the canonical `ROUTINE` yields `insns` `{0,3,6,0xc,
  0x11}` / blocks `{0,0x11}` — the *same* ground truth
  `test_wholewindow_decode` asserts against the fixture
  ([test_hwtrace.c:3160](../../../examples/test_hwtrace.c#L3160)). This is the
  proof the real-silicon AUX matches the encoder; it self-skips everywhere else.

**Docs.** Add the CHANGELOG `### Added` entry: "Intel PT attach-to-foreign-PID
capture wired to the whole-window decode — `perf_event_open` on a live pid,
paired with the temporal code-image recorder, dispatched into
`asmtest_pt_decode_window` with honest truncation. Bare-metal Intel PT gated."
Note the foreign-attach mode in
[native-tracing.md](../../guides/tracing/native-tracing.md) under the hardware
trace tier.

**Done when.**

- `make docker-hwtrace` green with the portable de-wrap / post-filter tests
  passing; the live-attach assertion `# SKIP`s on this AMD host with a clear
  reason.
- `grep -n "asmtest_pt_decode_window" src/` shows a caller in `src/hwtrace.c`
  (the foreign dispatch) in addition to the substrate's self-trace caller — the
  decode entry is no longer test-only.
- On a bare-metal Intel PT runner (self-hosted, `CAP_PERFMON` + `CAP_SYS_PTRACE`,
  or `perf_event_paranoid ≤ -1`) the live attach reproduces the ground-truth
  offsets.

### T5 — Hypervisor/EPT frontier: research-grade design record (HWT-HV-EPT)  (L, depends on: T4)

**Goal.** Capture, as an internal design record (no shipping code), the
maximum-stealth Phase 2 option: host-side Intel PT of a *guest's* execution via
Xen `vmtrace`, and — separately — DRAKVUF altp2m execute-only EPT views for
hidden breakpoints. Explicitly forward-look; it cannot begin until the PT-attach
slice (T1–T4) ships, and it needs a bare-metal Intel host running Xen that
neither dev box provides.

**Steps.** This task produces **documentation only** — a
`docs/internal/analysis/` design note (or an appendix here) recording the API
surface and the wiring decision, so a future implementor starts from verified
facts rather than re-researching:

1. Record the Xen `vmtrace` surface: available since **Xen 4.15** (2021);
   hypercall `XEN_DOMCTL_vmtrace_op`; the per-vCPU trace buffer mapped to dom0 via
   `XENMEM_acquire_resource(XENMEM_resource_vmtrace_buf)` /
   `xenforeignmemory_map_resource`; libxc wrappers `xc_vmtrace_enable` /
   `_disable` / `_set_option` / `_output_position`; buffer sized at domain create
   via `vmtrace_buf_size`. Requires x86 HVM + Intel VT-x (AMD/PV unsupported) and
   a build with `CONFIG_VMTRACE`. Xen emits **raw PT bytes only** — decode is
   off-host with a PT decoder (the same libipt path this doc already owns).
2. Record the DRAKVUF path: the `ipt` plugin (`-a ipt`, needs Xen ≥ 4.15, up to
   16 vCPUs, domain cfg `vmtrace_buf_kb = 8192`) post-processes with libipt +
   Intel `xed` / modified `ptxed`; the altp2m technique duplicates the code page,
   writes the `0xCC` only into the shadow copy, and marks the breakpoint-bearing
   page **execute-only** in EPT so guest reads trap and switch to a clean view
   (`xc_altp2m_*`). These are **two distinct out-of-VM mechanisms**, not one fused
   technique — state that plainly (global-position discipline: do not imply a
   single PT+altp2m primitive).
3. Record the decoder trade: `libxdc` (nyx-fuzz) claims 15–30× over libipt for
   fuzzing workloads but **builds against Capstone v4**, while this repo pins
   Capstone 5.0.1 ([scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt)) —
   so adopting libxdc would need a separate Capstone-v4 build and cannot reuse
   the pinned Capstone. Recommend staying on libipt unless a measured need
   appears.

**Tests.** None — this task ships no code, so there is no testable surface. The
verification that replaces a test is: the design note cites the primary sources
below and is reviewed against them. State this explicitly in the note.

**Docs.** The note itself is the deliverable; link it from the Phase 2
deliverables list in [hardware-trace-plan.md](../plans/hardware-trace-plan.md).
No CHANGELOG entry (no user-visible surface).

**Done when.**

- The design note exists, cites the Xen/DRAKVUF/libxdc sources, and states the
  Xen 4.15 + Intel-PT + Xen-host gate.
- No source, Makefile, or Dockerfile under the tree references `vmtrace`,
  `drakvuf`, `altp2m`, or `libxdc` — this stays forward-look until the gate is
  met (verify with `grep -ri`).

## Task order & parallelism

- **T3 is fully independent** and needs no PT hardware — one person can land it
  in parallel with everything else, and it is the highest-value slice deliverable
  today (byte recovery from a live foreign runtime, testable on any host).
- **T1 → T2 → T4** is the PT-capture critical path (capture context → recorder
  pairing → decode dispatch). T1 depends on the substrate's `pt_aux_open`
  (`intel-pt-whole-window-substrate.md#T1`) existing first.
- **T5** depends on T4 and is research-grade / doc-only; start it any time but it
  cannot be *validated* without a Xen+PT host.

Critical path: `substrate#T1 → T1 → T2 → T4`. T3 and T5 hang off the side.

## Constraints & gates

- **Bare-metal Intel PT silicon** (Broadwell+/Goldmont+) is the hard gate for
  T1/T2/T4 live capture. Both dev boxes are AMD (Ryzen 9 9950X Zen 5, Ryzen 9
  4900HS Zen 2 — `AuthenticAMD`, no `intel_pt` PMU node); VMs and GitHub runners
  do not expose PT. This is a legitimate hardware self-skip per CLAUDE.md — record
  it and skip, never fake the `available()` gate to force a test.
- **Privilege:** foreign-pid PT needs `CAP_PERFMON` (Linux 5.9+) — `pid>0` fails
  `EACCES`/`EPERM` without it; the paired code-image recorder needs
  `CAP_SYS_PTRACE` (or the target's `PR_SET_PTRACER_ANY`). These are host knobs
  the process cannot grant itself — detect-and-skip, do not lower a sysctl.
- **No new installable dependency for the PT/jitdump halves:** libipt v2.0.6 is
  already in [Dockerfile.hwtrace](../../../Dockerfile.hwtrace); `dotnet-sdk-8.0`
  and `Microsoft.Diagnostics.NETCore.Client` are already in the attach-probe
  image; `libperf-jvmti.so` is already probed. Xen/DRAKVUF/libxdc (T5) are
  hardware+hypervisor gated and stay forward-look, not added to any image.
- **Address filters cannot target JIT pages** — file-backed VMAs only, per-task
  events only. For foreign JIT code the hardware filter is unusable; the software
  IP post-filter (T4) is mandatory, not optional.

## Research notes (verified 2026-07-17)

- **PMU type / open.** Put the integer from
  `/sys/bus/event_source/devices/intel_pt/type` in `perf_event_attr.type`;
  config bits are described under `.../intel_pt/format`.
  [perf_event_open(2)](https://man7.org/linux/man-pages/man2/perf_event_open.2.html),
  [perf-intel-pt(1)](https://man7.org/linux/man-pages/man1/perf-intel-pt.1.html).
- **AUX mmap.** Two mmaps on one fd: the data area at pgoff 0 (`1 + 2^n` pages;
  page 0 is `struct perf_event_mmap_page`), then set `aux_offset` (>
  `data_offset+data_size`) and `aux_size` (page-aligned, power-of-two) and mmap
  the AUX area. `PROT_WRITE` AUX = linear (drain-and-flag); `PROT_READ`-only AUX =
  overwrite/snapshot mode (consumer must disable measurement before reading).
  [perf_event_open(2)],
  [kernel v6.8 core.c](https://raw.githubusercontent.com/torvalds/linux/v6.8/kernel/events/core.c),
  [ring_buffer.c](https://raw.githubusercontent.com/torvalds/linux/v6.8/kernel/events/ring_buffer.c).
- **Wrap protocol.** `aux_head`/`aux_tail` share `data_{head,tail}` semantics:
  read `aux_head`, acquire-barrier, copy `[aux_tail, aux_head)` modulo `aux_size`
  (two memcpys across the wrap — the AUX blob must be a linearized snapshot before
  libipt decode), release-store `aux_tail = head`.
  [perf_event.h](https://raw.githubusercontent.com/torvalds/linux/v6.8/include/uapi/linux/perf_event.h).
- **`PERF_RECORD_AUX` (=11).** Body `u64 aux_offset, aux_size, flags`; flags:
  `PERF_AUX_FLAG_TRUNCATED 0x01`, `OVERWRITE 0x02`, `PARTIAL 0x04`, `COLLISION
  0x08`. The existing `aux_data_ring_truncated`
  ([src/hwtrace.c:1971](../../../src/hwtrace.c#L1971)) already parses exactly this
  layout.
- **Foreign pid.** `pid>0` requires `CAP_PERFMON` (since 5.9) or a
  `PTRACE_MODE_READ_REALCREDS` access check on older kernels; unprivileged
  `paranoid=2` still permits same-uid targets with `exclude_kernel`.
  `aux_watermark` (since 4.1) sets how much data triggers a `PERF_RECORD_AUX` /
  wakeup — set it small for prompt `poll()` drain of a live target (kernel default
  is half the buffer). [perf_event_open(2)].
- **The "128 KiB" figure is not a kernel cap** — it is the `perf` tool's default
  auxtrace mmap size for unprivileged users (4 MiB/page for privileged). Kernel
  accounting is `perf_event_mlock_kb` (516 KiB default) + `RLIMIT_MEMLOCK`.
  [perf-intel-pt(1)].
- **Address filters** (kernel core.c comment): `"ACTION RANGE_SPEC"`,
  `ACTION ∈ filter|start|stop`; object-file form `<start>[/<size>]@</path>`.
  Constraints: `@file` must be a regular file; **file-based filters are
  per-task-events only** (`!ctx->task → EOPNOTSUPP`); filters match only
  file-backed VMAs by inode (`perf_addr_filter_match`) — **anonymous/JIT pages
  cannot be address-filtered**. Per-PMU count at
  `/sys/bus/event_source/devices/intel_pt/nr_addr_filters` (read at runtime).
  [core.c v6.8].
- **libipt v2.0.6** (Ubuntu noble `libipt-dev` 2.0.6-1build1, already installed):
  `pt_insn_alloc_decoder`, windowed image via `pt_image_alloc` +
  `pt_image_set_callback` (the recorder-backed path already in
  `asmtest_pt_decode_window`), loop `pt_insn_sync_forward` → `pt_insn_next`, drain
  `pts_event_pending` via `pt_insn_event`; `-pte_nomap` = IP outside the added
  image window; `-pte_eos` = end of trace.
  [intel-pt.h v2.0.6](https://raw.githubusercontent.com/intel/libipt/v2.0.6/libipt/include/intel-pt.h.in),
  [libipt-dev noble](https://packages.ubuntu.com/noble/libipt-dev).
- **.NET runtime-enable jitdump.**
  `DiagnosticsClient.EnablePerfMap(PerfMapType.JitDump)` /
  `DisablePerfMap` ship in `Microsoft.Diagnostics.NETCore.Client`, .NET 8+; IPC
  Process command `EnablePerfMap=0x0405`, payload `uint32 PerfMapType` where
  `JITDUMP=2`; jitdump lands at `/tmp/jit-<pid>.dump` and re-emits already-JITted
  methods. The env `DOTNET_PerfMapEnabled` is launch-only — hence the diagnostic
  port for attach.
  [dotnet diag IPC](https://github.com/dotnet/diagnostics/blob/main/documentation/design-docs/ipc-protocol.md),
  [NETCore.Client](https://learn.microsoft.com/en-us/dotnet/core/diagnostics/microsoft-diagnostics-netcore-client),
  [runtime-config](https://learn.microsoft.com/en-us/dotnet/core/runtime-config/debugging-profiling).
- **JVM jitdump agent.** The kernel builds `libperf-jvmti.so`
  ([tools/perf/Makefile.perf](https://raw.githubusercontent.com/torvalds/linux/master/tools/perf/Makefile.perf));
  it registers `JVMTI_EVENT_COMPILED_METHOD_LOAD` +
  `JVMTI_EVENT_DYNAMIC_CODE_GENERATED` and emits jitdump records
  ([libjvmti.c](https://raw.githubusercontent.com/torvalds/linux/master/tools/perf/jvmti/libjvmti.c)).
  Attach to a running JVM with `jcmd <pid> JVMTI.agent_load`; a
  `GenerateEvents(COMPILED_METHOD_LOAD)` replays existing nmethods (needs
  `-XX:+EnableDynamicAgentLoading`, JEP 451).
- **jitdump format.** File magic `"JiTD"` `0x4A695444`, version 1;
  `JIT_CODE_LOAD` body carries `pid/tid/vma/code_addr/code_size/code_index`, the
  NUL-terminated name, then the **raw native code bytes** — the bytes
  `asmtest_jitdump_find` recovers.
  [jitdump-specification.txt](https://raw.githubusercontent.com/torvalds/linux/master/tools/perf/Documentation/jitdump-specification.txt).
- **V8/Node has no runtime-enable** — `--perf-prof` is wired once at isolate init;
  CLI/`NODE_OPTIONS` only.
  [flag-definitions.h](https://raw.githubusercontent.com/v8/v8/main/src/flags/flag-definitions.h),
  [linux-perf](https://v8.dev/docs/linux-perf).
- **Xen vmtrace / DRAKVUF / libxdc** (T5): Xen 4.15+ `XEN_DOMCTL_vmtrace_op` +
  `XENMEM_resource_vmtrace_buf` + `xc_vmtrace_*` (Intel VT-x HVM only, raw PT
  bytes decoded off-host);
  [Xen 4.15](https://xenproject.org/2021/04/08/xen-project-hypervisor-4-15),
  [xc_vmtrace patch](https://patchew.org/Xen/20210121212718.2441-1-andrew.cooper3@citrix.com/20210121212718.2441-9-andrew.cooper3@citrix.com/).
  DRAKVUF `ipt` plugin (`-a ipt`, `vmtrace_buf_kb=8192`, ≤16 vCPUs, libipt+ptxed)
  and altp2m execute-only EPT (two distinct mechanisms);
  [DRAKVUF IPT](https://github.com/tklengyel/drakvuf/wiki/Intel-Processor-Trace),
  [altp2m](https://github.com/tklengyel/drakvuf/wiki/Xen-altp2m). `libxdc`
  (15–30× over libipt, fuzzing) needs **Capstone v4** — incompatible with this
  repo's pinned Capstone 5.0.1, so it cannot reuse the pinned build;
  [libxdc](https://github.com/nyx-fuzz/libxdc).

## Out of scope

- The self-trace (`pid==0`) PT arm, the shared `pt_aux_open`/`pt_aux_stop`
  helpers, the WEAK/STRONG/CEILING window ladder, and the native
  `pt_begin_window`/`pt_end_window` pair — all owned by
  [intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md). This
  doc consumes those helpers; it never re-implements a PT arm.
- The time-aware code-image recorder internals (soft-dirty / `PAGEMAP_SCAN` /
  eBPF emission detector) — landed and validated; this doc only *pairs* it with
  the foreign PT capture. Its cross-binding slice surface is
  [dataflow-bindings-slice-codeimage.md](dataflow-bindings-slice-codeimage.md).
- ARM CoreSight live decode of a foreign process —
  [coresight-live-decode.md](coresight-live-decode.md).
- A dataflow/value-replay tier over PT —
  [dataflow-pt-replay-tier.md](dataflow-pt-replay-tier.md).
- Managed whole-window composition and the .NET inline ctor plumbing —
  [managed-wholewindow-compose.md](managed-wholewindow-compose.md) and the
  substrate doc's T4.
- IL/bytecode source attribution of recovered JIT bytes —
  [native-il-bytecode-attribution.md](native-il-bytecode-attribution.md).
