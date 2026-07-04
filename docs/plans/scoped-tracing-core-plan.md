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

**Goal.** Three small, self-contained changes that de-risk *every* binding and
unblock the bindings slice's emit-on-close. Cheap enough to land before any shim
work.

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
  `ASMTEST_HW_*` code on a busy slot (`EBUSY`-analog) or unregistered name
  (`ENOENT`-analog). Reuse the existing `ASMTEST_HW_*` negative-code convention
  (`asmtest_hwtrace_auto` already returns them, [src/hwtrace.c:327](../../src/hwtrace.c#L327)).
- Keep `void asmtest_hwtrace_begin(const char *name)` as a thin wrapper that calls
  `try_begin` and discards the result — **no ABI break** for the ten shipped
  shims. The implementation split happens in `asmtest_hwtrace_begin`
  ([src/hwtrace.c:654](../../src/hwtrace.c#L654)).
- The same two dispatch cases (single-step [:663](../../src/hwtrace.c#L663), AMD
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
  set from `gettid()` in `begin` (all three backends), cleared in `end`.
- In `asmtest_hwtrace_end` ([src/hwtrace.c:762](../../src/hwtrace.c#L762)) — and the
  single-step `asmtest_ss_end` ([src/ss_backend.c:207](../../src/ss_backend.c#L207)) —
  compare `gettid()` against `g_arm_tid`; on mismatch set `trace->truncated = true`
  ([include/asmtest_trace.h:59](../../include/asmtest_trace.h#L59)) rather than
  emitting a partial trace as complete. This is the C half of the Go
  `LockOSThread` / .NET `AsyncLocal` story: the shim can't always prevent the hop,
  but the core will never mislabel it.
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
  calling `asmtest_disas` ([src/disasm.c:89](../../src/disasm.c#L89)) against
  `[base, base+len)`. **`snprintf` semantics:** it writes up to `buflen-1` bytes +
  NUL and **returns the total length that would be written** (so a caller passes
  `buf=NULL, buflen=0` to size, then allocates) — the one shape every binding can
  marshal (`char[]`/`byte[]`/`[*]u8`) without a callback FFI. A convenience
  `FILE*` wrapper may be layered *above* it later, but the buffer form is the
  installed primitive.
- Default sink policy for the empty-scope case lives in the shims (stdout or
  `asmtrace-<member>.txt`), but the **decode + Capstone render** is this one C path.
- Must respect the region-scoped model: it renders exactly the in-region offsets
  the decoders already filtered to; whole-window rendering (Core §3 / Managed slice)
  is a separate mode.

**§0 tests.** Extend [examples/test_hwtrace.c](../../examples/test_hwtrace.c)
(dispatched from `main` at [:2174](../../examples/test_hwtrace.c#L2174)):

- `test_try_begin_busy` — register two regions, `try_begin` the second while the
  first is active, assert the negative `EBUSY`-analog; assert `try_begin` on an
  unregistered name returns the `ENOENT`-analog; assert the legacy `void` `begin`
  still no-ops (ABI unchanged). Runs on **any host** (single-step backend, no
  hardware needed).
- `test_arm_tid_mismatch` — arm on the main thread, close from a spawned thread,
  assert `truncated` is set. Any host.
- `test_render_singlestep` — single-step-trace a known native leaf, call
  `asmtest_hwtrace_render`, assert the text matches a ground-truth `asmtest_disas`
  of the same bytes (reuse the `test_singlestep_live` fixture at
  [:408](../../examples/test_hwtrace.c#L408)). Any x86-64 Linux.

**§0 docs.** Update
[docs/guides/tracing/hardware-tracing.md](../guides/tracing/hardware-tracing.md)
and the API surface in
[docs/reference/api-reference.md](../reference/api-reference.md) for the three new
symbols; note the `void begin` → `try_begin` relationship in
[docs/guides/tracing/native-tracing.md](../guides/tracing/native-tracing.md).

**§0 effort.** ~2–3 days. No hardware needed — validated on any x86-64 Linux via
the single-step backend.

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
- **Single-step (`src/ss_backend.c`).** Move `g_armed`/`g_base`/`g_base_ip`/`g_len`/
  `g_trace`/`g_stream`/`g_stream_len`/`g_overflow`/`g_old_sa`/`g_installed`
  ([:58-68](../../src/ss_backend.c#L58)) into TLS, and replace the single
  `[g_base_ip, g_base_ip+g_len)` test in the handler
  ([:99-104](../../src/ss_backend.c#L99)) with an async-signal-safe **range stack**
  (a fixed-size TLS array, no allocation in the handler — the analysis is explicit:
  "never `malloc` in the handler"). The SIGTRAP disposition stays process-wide
  (installed once, [:129](../../src/ss_backend.c#L129); restored when the last
  armed thread leaves, [:214](../../src/ss_backend.c#L214)); per-thread `g_armed`
  makes concurrent scopes on different threads safe.
- **AMD (`src/amd_backend.c` / `hwtrace.c`).** The AMD path shares the `hwtrace.c`
  slot; moving that slot to TLS covers it. Tier-A/Tier-B stitching
  ([src/amd_backend.c:152](../../src/amd_backend.c#L152),
  [src/hwtrace.c:603](../../src/hwtrace.c#L603)) is per-region and unaffected.

**Compatibility.** The shipped single-region API must behave identically when only
one thread/one region is used. `try_begin`'s `EBUSY` (§0.1) is redefined to mean
"this thread's range stack is full," not "the process is busy."

**§1 tests.** In [examples/test_hwtrace.c](../../examples/test_hwtrace.c):

- `test_nested_singlestep` — two nested `begin`/`end` pairs on one thread over a
  known native routine; assert the inner region's offsets are a subset and the
  outer region still closes correctly. Any x86-64 Linux.
- `test_concurrent_singlestep` — two threads each scope a *different* native leaf
  concurrently; assert each gets its own complete trace and neither trips the other
  (the previous single-slot behaviour would have dropped one). Any x86-64 Linux.
  This is the regression test for the flaky-crash class the Go binding hit.
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
  [include/asmtest_ptrace.h:291](../../include/asmtest_ptrace.h#L291)).
- **Self-recorder wiring (`src/hwtrace.c`).** In the arm path, create a self
  code-image timeline (`asmtest_codeimage_new(0)`,
  [include/asmtest_codeimage.h:81](../../include/asmtest_codeimage.h#L81)) and
  `asmtest_codeimage_track` ([:90-91](../../include/asmtest_codeimage.h#L90)) the
  JIT ranges; drive `asmtest_codeimage_refresh`
  ([:97](../../include/asmtest_codeimage.h#L97)) at region boundaries so a new
  version is snapshotted on change. (The recorder already feeds the *out-of-process*
  stepper's `_versioned` path; this points the same recorder at self.)
- **Capture-side address filter (`src/pt_backend.c:129-135`).** The named TODO —
  program `PERF_EVENT_IOC_SET_FILTER` so the CPU emits packets only for the region
  — is the structural fix for both the runtime-noise (Q2) and bandwidth (Q3)
  qualifications. It needs PT hardware to validate, so it ships gated behind the
  same self-skip as the rest of the PT capture path.

**Validation posture (mirrors the hardware-trace plan).** The **reconstruction
half** — feeding a synthetic code image to the decoder and asserting byte-for-byte
parity with the other backends — is host-testable the way
`test_amd_reconstruction`/`test_cs_reconstruction` already are (no hardware). The
**live PT capture** half self-skips off bare-metal Intel PT. Per the project's "no
untested hardware code" rule, the live path is written but gated, and the gate is
exercised on every host.

**§2 tests.** In [examples/test_hwtrace.c](../../examples/test_hwtrace.c):

- `test_pt_image_from_codeimage` (host-testable) — build an `asmtest_codeimage`
  over an in-process buffer with two versions of the bytes at one address, run the
  new image adapter through the decode path at two `when` values, assert each
  decodes against the version live then (the temporal-bytes rule) and that the
  instruction offsets match `asmtest_disas` ground truth. No PT hardware.
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

**§2 effort.** Reconstruction adapter + tests ~3–4 days (host-testable); the
capture-side address filter + live whole-window decode a further ~3–5 days **on PT
hardware**, forward-look until a bare-metal Intel PT host is available (same gate as
the hardware-trace plan).

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
  `asmtest_codeimage_now`, [:102](../../include/asmtest_codeimage.h#L102)).
- **(c) Symbolize-and-bucket.** Bucket every decoded IP against `/proc/self/maps`
  (`asmtest_proc_region_by_addr`,
  [include/asmtest_ptrace.h:291](../../include/asmtest_ptrace.h#L291)) and the
  perf-map (`asmtest_proc_perfmap_symbol`,
  [:303](../../include/asmtest_ptrace.h#L303)), so noise is labelled ("31k insns in
  RyuJIT, 2k in GC, 7k in `HotPath`") rather than silently mixed. **This is
  host-testable** — the bucketer takes an IP list, not a live PT capture — so it is
  the one whole-window piece with real CI coverage.

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
  and labels. No hardware.
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
  ([mk/native-trace.mk:677-694](../../mk/native-trace.mk#L677)) and the header
  install (review item **K6**).
- CI: the new `test_*` cases run inside the existing `hwtrace` job
  ([.github/workflows/ci.yml:247](../../.github/workflows/ci.yml)) via
  `make hwtrace-test` and the `codeimage` job (`:281`) via `make codeimage-test` —
  no new job. The per-binding lanes that exercise the new symbols are the bindings
  slice's concern (`hwtrace-bindings`, `:268`).

---

## Risks and open points

- **Per-thread migration is invasive and must be a no-op for existing callers.**
  The single-region API is shipped and CI-gated across ten bindings; §1's
  regression tests (`test_singlestep_live` re-run, byte-identical) are the guard.
- **Async-signal-safety of the single-step range stack.** The handler
  ([src/ss_backend.c:88](../../src/ss_backend.c#L88)) runs in signal context; the
  range stack must be a fixed TLS array with no allocation, no locks — the same
  discipline the existing `g_stream` write obeys
  ([:99-104](../../src/ss_backend.c#L99)).
- **§2's live half needs PT hardware** (this dev host is AMD — no PT, ever). It
  ships self-skipping and hardware-gated; only the reconstruction half is
  CI-validated, exactly as [hardware-trace-plan](hardware-trace-plan.md) accepts
  for its own PT capture.
- **ABI stability.** `begin`/`end` stay `void` and behaviourally identical for the
  ten shipped shims; all new capability is additive (`try_begin`, `render`,
  `arm_tid`). No existing symbol changes signature.

## Sources

Design rationale, the four-qualification analysis (thread-scope, runtime-noise,
bandwidth, nesting), and the temporal-bytes correctness rule:
[the scoped `using` analysis](../analysis/scoped-inprocess-tracing.md#can-the-four-qualifications-be-fixed-in-code).
Shared decode/recorder background:
[hardware-trace-plan.md](hardware-trace-plan.md),
[jit-runtime-tracing.md](../analysis/jit-runtime-tracing.md).
