# Intel PT whole-window capture substrate: STRONG tier ladder, inline ctor, and live smoke — implementation

> **Sources.** Actioned from
> [scoped-tracing-zeroconfig-plan.md](../plans/scoped-tracing-zeroconfig-plan.md)
> (§Z1.2 ZC-STRONG, §Z2 ZC-Z2-LIVE),
> [asmtrace-inline-using-plan.md](../archive/plans/asmtrace-inline-using-plan.md) (R2), and
> [hardware-trace-plan.md](../plans/hardware-trace-plan.md) (Phase 1 PT capture,
> Phase 2 corrections). Written 2026-07-17. If this doc and a source disagree,
> this doc wins (sources may be stale); if the CODE and this doc disagree,
> re-verify before implementing.

## Why this work exists

On bare-metal Intel Linux, `using (new AsmTrace())` — and its explicit sibling
`new AsmTrace(HwBackend.IntelPt)` — should capture a *quiet and complete*
whole-window trace via Intel PT instead of the intrusive single-step WEAK tier.
Today `asmtest_hwtrace_begin_window` returns `EUNAVAIL` for every
non-single-step backend, the .NET `IntelPt` ctor branch self-skips as
"forward-look (not wired)", and the real libipt decode body has never been run
against a real AUX stream. This doc wires the one native PT capture arm (perf
AUX `intel_pt`, `pid==0` self-trace), the WEAK/STRONG/CEILING auto-selection
ladder, the native `pt_begin_window`/`pt_end_window` pair the .NET inline ctor
uses, and the hardware-gated live smoke that validates all of it on silicon.

**Ownership (binding, from the doc-set conflict resolution):** this doc owns
the `begin_window` PT arm for the *self-trace* path, the shared AUX
open/mmap/drain/decode helpers, and the ladder.
[intel-pt-attach-foreign-pid.md](intel-pt-attach-foreign-pid.md) extends the
*same* helpers for foreign pids — there must never be a second, parallel PT
arming implementation.

## What already exists (verified 2026-07-17)

- [src/hwtrace.c](../../../src/hwtrace.c) — the hardware-trace facade.
  - The **region-keyed PT AUX capture is landed** (Phase 1): `asmtest_hwtrace_try_begin`
    opens a `perf_event_open` on the `intel_pt` PMU (`attr.type` from
    `/sys/bus/event_source/devices/intel_pt/type` via `pmu_type()`
    [:145](../../../src/hwtrace.c#L145), `pid=0`, `disabled=1`,
    `exclude_kernel/hv`), mmaps the data ring, sets
    `aux_offset`/`aux_size` on the mmap page, second-mmaps the AUX area
    (RW linear, RO circular when `opts.snapshot`), then `IOC_RESET` +
    `IOC_ENABLE` ([:1878-1939](../../../src/hwtrace.c#L1878)).
    `asmtest_hwtrace_end` `IOC_DISABLE`s, reads `aux_head`, scans the data
    ring for `PERF_RECORD_AUX`/`PERF_AUX_FLAG_TRUNCATED`
    (`aux_data_ring_truncated`, [:1971](../../../src/hwtrace.c#L1971)),
    decodes via `asmtest_pt_decode`, and applies the truncation policy
    `overflow || rc != OK` ([:2061-2091](../../../src/hwtrace.c#L2061)). This
    is the pattern every new PT helper extracts from — none of it is new
    research.
  - `asmtest_hwtrace_begin_window` ([:2554](../../../src/hwtrace.c#L2554))
    arms **only** `ASMTEST_HWTRACE_SINGLESTEP`; every other backend hits the
    comment "PT / AMD LBR / CoreSight whole-window capture is the
    forward-look STRONG tier" and `return ASMTEST_HW_EUNAVAIL`
    ([:2585-2587](../../../src/hwtrace.c#L2585)). `end_window`
    ([:2594](../../../src/hwtrace.c#L2594)) closes via
    `asmtest_ss_frame_lookup` and flags `truncated` on a cross-thread close.
  - The AMD begin/end split to mirror: `asmtest_hwtrace_sample_begin_amd`
    ([:1599](../../../src/hwtrace.c#L1599), open + `ENABLE`, heap ctx
    `{fd, base_map, base_sz, …}`) / `asmtest_hwtrace_sample_end_amd`
    ([:1671](../../../src/hwtrace.c#L1671), `DISABLE` + drain + free).
  - Init-time cascade `asmtest_hwtrace_resolve`/`asmtest_hwtrace_auto`
    ([:466](../../../src/hwtrace.c#L466), [:487](../../../src/hwtrace.c#L487))
    — availability-probe ordering, but no *window-path* ladder and no
    decode-trust gate.
  - `asmtest_hwtrace_init` refuses a backend whose
    `asmtest_hwtrace_available()` is 0, and for `INTEL_PT` that requires
    `vendor_is("GenuineIntel")` ([:212](../../../src/hwtrace.c#L212)) plus the
    `intel_pt` sysfs PMU node — so on non-Intel hosts **no facade path can
    reach PT code**, by construction (the 2026-07-17 correction in
    [hardware-trace-plan.md](../plans/hardware-trace-plan.md)). Do not fake
    this gate in tests.
- [src/pt_backend.c](../../../src/pt_backend.c) — decode-only today, gated on
  `-DASMTEST_HAVE_LIBIPT` ([:65](../../../src/pt_backend.c#L65)); the `#else`
  block ([:361](../../../src/pt_backend.c#L361)) compiles `ENOSYS` stubs.
  - `asmtest_pt_read_codeimage` ([:50](../../../src/pt_backend.c#L50)) — the
    libipt-independent temporal byte adapter (host-tested by
    `test_pt_image_from_codeimage`).
  - `asmtest_pt_decode` ([:141](../../../src/pt_backend.c#L141)) region-scoped;
    `asmtest_pt_decode_window` ([:224](../../../src/pt_backend.c#L224))
    whole-window over `read_recorder` ([:99](../../../src/pt_backend.c#L99)),
    recording offsets **from the first decoded IP** (`base_ip`, kept local).
  - `asmtest_pt_encode_fixture` ([:305](../../../src/pt_backend.c#L305)) —
    synthesizes a valid PT AUX stream with libipt's own encoder for the
    canonical ROUTINE walk, `taken` selecting the TNT bit.
  - There is **no capture code and no `PERF_EVENT_IOC_SET_FILTER` wiring** in
    this file — the filter exists only in a comment
    ([:210](../../../src/pt_backend.c#L210)).
- [src/codeimage.c](../../../src/codeimage.c) — `asmtest_codeimage_new(pid)`
  ([:319](../../../src/codeimage.c#L319); `pid==0` = self),
  `asmtest_codeimage_track/refresh/now/bytes_at`, availability probe
  `asmtest_codeimage_available` ([:298](../../../src/codeimage.c#L298),
  soft-dirty / `PAGEMAP_SCAN`).
- [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h) — the window
  trio declarations ([:442-459](../../../include/asmtest_hwtrace.h#L442)), the
  scope handle `{idx, gen, arm_tid}` ([:340](../../../include/asmtest_hwtrace.h#L340)),
  the options struct with `aux_size`/`data_size`/`snapshot`
  ([:75-138](../../../include/asmtest_hwtrace.h#L75)), and the policy enum
  ([:220](../../../include/asmtest_hwtrace.h#L220)).
- [src/ss_backend.c](../../../src/ss_backend.c) — `asmtest_ss_begin_window`
  ([:341](../../../src/ss_backend.c#L341) Linux, [:633](../../../src/ss_backend.c#L633)
  macOS variant) — the WEAK-tier window arm; do not change its behavior.
- [examples/test_hwtrace.c](../../../examples/test_hwtrace.c) —
  `test_wholewindow_decode` ([:3160](../../../examples/test_hwtrace.c#L3160))
  drives both real libipt decode bodies over the synthetic fixture (taken AND
  not-taken TNT), green in CI wherever libipt is built; the canonical
  `ROUTINE[]` bytes at [:120](../../../examples/test_hwtrace.c#L120).
- [bindings/dotnet/hwtrace/HwTrace.cs](../../../bindings/dotnet/hwtrace/HwTrace.cs)
  — `enum Kind` ([:1871](../../../bindings/dotnet/hwtrace/HwTrace.cs#L1871)),
  the empty ctor ([:2105](../../../bindings/dotnet/hwtrace/HwTrace.cs#L2105))
  → `ArmWholeWindow` ([:2140](../../../bindings/dotnet/hwtrace/HwTrace.cs#L2140))
  → `AutoInitSingleStep` ([:2184](../../../bindings/dotnet/hwtrace/HwTrace.cs#L2184)),
  the backend-keyed ctor ([:2535](../../../bindings/dotnet/hwtrace/HwTrace.cs#L2535))
  whose non-AMD/non-SingleStep branch self-skips with
  `"{backend} inline whole-window is forward-look (not wired)"`
  ([:2562-2567](../../../bindings/dotnet/hwtrace/HwTrace.cs#L2562)), the
  `AmdSampler` finalizable holder ([:1807](../../../bindings/dotnet/hwtrace/HwTrace.cs#L1807)),
  the `sample_begin_amd` P/Invoke arming pattern
  ([:2583](../../../bindings/dotnet/hwtrace/HwTrace.cs#L2583)), and the
  Dispose `switch (_kind)` ([:3495](../../../bindings/dotnet/hwtrace/HwTrace.cs#L3495)).
- [Dockerfile.hwtrace](../../../Dockerfile.hwtrace) — installs `libipt-dev`
  (line 21) on the `ubuntu:24.04`-based bindings base, so the hwtrace lane
  builds `pt_backend.o` with `-DASMTEST_HAVE_LIBIPT` via the header probe in
  [mk/native-trace.mk:1907](../../../mk/native-trace.mk#L1907). **No new
  dependency is needed anywhere in this doc.**

**Prove the baseline green before touching anything:**

```sh
make docker-hwtrace          # builds Dockerfile.hwtrace, runs hwtrace-test +
                             # codeimage-test + hwtrace-python-test in a plain container
```

Expected: the C harness ends with a TAP summary `1..N` / `# N passed, 0 failed`
(with `# SKIP` lines for PT/AMD live paths on a non-Intel host), and
`test_wholewindow_decode`'s checks pass (libipt is in the image). Also run
`make check` for the framework self-tests and `make help` to see all targets.

## Tasks

### T1 — Extract the shared PT AUX helpers and add the native `pt_begin_window`/`pt_end_window` pair  (M, depends on: none)

**Goal.** One reusable perf-AUX `intel_pt` open/mmap/drain/teardown
implementation in `src/hwtrace.c`, exposed as a public begin/end context pair
mirroring the AMD split, with the existing region-keyed PT path refactored onto
the same helpers (behavior-identical).

**Steps.**

1. In [src/hwtrace.c](../../../src/hwtrace.c), define an internal context and
   three static helpers next to the AMD split
   ([:1590](../../../src/hwtrace.c#L1590)):

   ```c
   typedef struct {
       int fd;
       void *base_map; size_t base_sz;   /* header page + data ring */
       void *aux_map;  size_t aux_sz;    /* AUX (PT trace) ring     */
   } pt_aux_t;
   static int  pt_aux_open(pid_t pid, size_t data_size, size_t aux_size,
                           uint32_t aux_watermark, int snapshot, pt_aux_t *out);
   static int  pt_aux_stop(pt_aux_t *a, uint64_t *head_out, int *overflow_out);
   static void pt_aux_close(pt_aux_t *a);
   ```

   `pt_aux_open` is the existing open sequence at
   [:1878-1939](../../../src/hwtrace.c#L1878) parameterized on `pid` (this doc
   uses only `pid==0` and passes `aux_watermark=0`, the kernel default; the
   `pid` and `aux_watermark` parameters exist so
   [intel-pt-attach-foreign-pid.md](intel-pt-attach-foreign-pid.md) can reuse it
   for `pid>0` with a nonzero wakeup watermark and **without a second open
   arm**): `pmu_type()` → `perf_event_attr` (`size`, `type`,
   `exclude_kernel=1`, `exclude_hv=1`, `disabled=1`, `attr.aux_watermark =
   aux_watermark` when nonzero) → `perf_open(&attr, pid,
   -1, -1, 0)` → data mmap (`1 + 2^n` pages, `PROT_READ|PROT_WRITE`,
   `MAP_SHARED`, pgoff 0) → set `mp->aux_offset = base_sz`,
   `mp->aux_size = aux_sz` (page-aligned, power-of-two — the existing
   `round_pages(…, 64*1024)` already guarantees this) → AUX mmap at
   `(off_t)base_sz` (`PROT_READ|PROT_WRITE` linear; `PROT_READ` circular when
   `snapshot`) → `IOC_RESET` + `IOC_ENABLE`. Any failure unwinds fully and
   returns `ASMTEST_HW_EUNAVAIL`.
2. `pt_aux_stop`: `ioctl(fd, PERF_EVENT_IOC_DISABLE, 0)`; read
   `mp->aux_head`, `__sync_synchronize()` (the acquire the aux_head/aux_tail
   protocol requires — see Research notes); set `*overflow_out` from a
   parameterized `aux_data_ring_truncated` (refactor
   [:1971](../../../src/hwtrace.c#L1971) to take `(void *base_map, size_t
   base_sz)` instead of globals) plus the `head >= aux_sz` clamp heuristic from
   [:2071-2074](../../../src/hwtrace.c#L2071).
3. Re-express the region-keyed path (`try_begin`'s PT branch and `end`'s
   drain/teardown) over these helpers, keeping the globals `g_fd`,
   `g_base_map`, `g_aux_map` as a `pt_aux_t g_pt` (or thin wrappers) so
   behavior is byte-identical. Run `make docker-hwtrace` after this step —
   everything must stay green before any new surface is added.
4. Add the public pair to
   [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h) directly
   after the §D4 stitch block, and implement in `src/hwtrace.c`:

   ```c
   /* STRONG-tier whole-window PT capture, begin/end split (the .NET
    * `using (new AsmTrace(HwBackend.IntelPt))` shape and the facade's own
    * begin_window PT arm both drive this ONE pair). begin opens a per-thread
    * (pid==0) perf AUX intel_pt event with NO address filter and ENABLEs it;
    * end DISABLEs, drains the linear AUX ring, decodes through
    * asmtest_pt_decode_window against `img` as of `when` (img == NULL: the
    * ctx's own self code-image, refreshed at close), fills `trace` with
    * ABSOLUTE addresses, and frees the ctx. Self-skips EUNAVAIL off
    * bare-metal Intel PT / without perf permission. */
   int asmtest_hwtrace_pt_begin_window(void **ctx_out);
   int asmtest_hwtrace_pt_end_window(void *ctx, asmtest_codeimage_t *img,
                                     uint64_t when, asmtest_trace_t *trace);
   ```

5. `make fmt` (clang-format is CI-gated via `fmt-check`), then
   `make docker-hwtrace`.

**Code.** The ctx is heap-allocated (`calloc`), holding `pt_aux_t aux`, an
owned `asmtest_codeimage_t *img` created with `asmtest_codeimage_new(0)`
([src/codeimage.c:319](../../../src/codeimage.c#L319)) when the codeimage
substrate is available (NULL otherwise — decode then requires the caller's
`img`), and `int arm_tid` from `SYS_gettid`. Guard the whole pair in the exact order
`sample_begin_amd` uses ([src/hwtrace.c:1600](../../../src/hwtrace.c#L1600)):
`asmtest_hwtrace_pt_begin_window` validates its argument FIRST — a NULL
`ctx_out` returns `ASMTEST_HW_EINVAL` before anything else (mirroring
`sample_begin_amd`'s `ctx_out == NULL` check) — then sets `*ctx_out = NULL` and
checks `asmtest_hwtrace_available(ASMTEST_HWTRACE_INTEL_PT)`, returning
`ASMTEST_HW_EUNAVAIL` on 0 (this is what makes the off-Intel path a clean
self-skip). Argument validation precedes the availability gate, so the `(NULL)`
call returns `EINVAL` on EVERY host — including a no-PT dev box, where the
`Tests` self-skip check depends on it — while the availability gate is reached
only with a valid `ctx_out`. `pt_end_window` with `img == NULL` and no
ctx-owned image returns `ASMTEST_HW_EDECODE` after teardown (never leaks the
fd/mmaps); `pt_end_window` with `trace == NULL` is a legal drain-less release —
teardown only, no decode, returns `ASMTEST_HW_OK` — the shape T4's finalizer
relies on to free a leaked scope, mirroring the AMD sibling's documented
"ips may be null" drain-less-release contract
([HwTrace.cs:561-562](../../../bindings/dotnet/hwtrace/HwTrace.cs#L561)).
Sizes: default `data_size` 8 KiB,
`aux_size` 64 KiB (the shipped defaults in the options struct,
[asmtest_hwtrace.h:96-102](../../../include/asmtest_hwtrace.h#L96)); take them
from `g_opts` when the tier is inited, else use the defaults so the pair also
works pre-`init` for the inline ctor.

**Tests.** Extend [examples/test_hwtrace.c](../../../examples/test_hwtrace.c)
with `test_pt_window_pair_selfskip` (register it in `main` next to
`test_wholewindow_decode`): asserts `asmtest_hwtrace_pt_begin_window(NULL)`
returns `ASMTEST_HW_EINVAL`; on a host where
`asmtest_hwtrace_available(ASMTEST_HWTRACE_INTEL_PT)` is 0 it asserts begin
returns `ASMTEST_HW_EUNAVAIL` with `*ctx_out == NULL` and prints
`# SKIP pt window pair: no Intel PT on this host` for the live half. Failure
looks like a `not ok` TAP line from `CHECK`; pass keeps the summary at
`0 failed`. The live-capture half of the pair is validated by T5 (hardware
gate). Run: `make docker-hwtrace`.

**Docs.** Internal-only at this stage (the pair is not yet reachable from any
binding); user-facing docs land with T4/T5.

**Done when.**

- `make docker-hwtrace` is green with the refactor in place (no count drops,
  no new failures).
- `grep -n "pt_aux_open" src/hwtrace.c` shows the PT open sequence defined
  once and called from both the region-keyed path and `pt_begin_window`, and no
  `perf_open` keyed to `pmu_type(ASMTEST_HWTRACE_INTEL_PT)` exists outside it —
  the "one PT arm" invariant. (A plain `grep -n "perf_event_open" src/hwtrace.c`
  is NOT the right check: it matches only the file-header comment and the
  `SYS_perf_event_open` syscall wrapper at `:234`, neither of which is the PT
  open sequence, and the sequence itself calls the `perf_open` wrapper, not
  `perf_event_open`.)
- On any host without the `intel_pt` PMU (via `make docker-hwtrace`), the new
  test prints its `# SKIP` and passes.
- `make fmt-check` passes.

### T2 — Arm the STRONG tier behind `asmtest_hwtrace_begin_window`/`_end_window`  (M, depends on: T1)

**Goal.** When the tier is inited with `backend == ASMTEST_HWTRACE_INTEL_PT`,
`begin_window` arms a real region-free PT capture and `end_window` drains and
decodes it through `asmtest_pt_decode_window`, filling the trace with ABSOLUTE
addresses and honest truncation.

**Steps.**

1. Give `asmtest_pt_decode_window` an out-param for the offset origin. In
   [src/pt_backend.c](../../../src/pt_backend.c), change the signature (real
   body [:224](../../../src/pt_backend.c#L224) AND the `ENOSYS` stub
   [:375](../../../src/pt_backend.c#L375)) to:

   ```c
   int asmtest_pt_decode_window(const uint8_t *aux, size_t aux_len,
                                const asmtest_codeimage_t *img, uint64_t when,
                                asmtest_trace_t *trace, uint64_t *base_ip_out);
   ```

   `base_ip_out` (NULL allowed) receives the first decoded IP. This entry has
   NO declaration in `include/` — only file-local `extern` prototypes — so ALSO
   update the 5-arg prototype at
   [examples/test_hwtrace.c:113-115](../../../examples/test_hwtrace.c#L113) to
   the 6-arg form, or that translation unit fails to compile. Then update the
   two existing callers in `test_wholewindow_decode`
   ([examples/test_hwtrace.c:3265](../../../examples/test_hwtrace.c#L3265) and
   [:3298](../../../examples/test_hwtrace.c#L3298), the (B) half) to pass NULL —
   their offset assertions are unchanged.
2. In `asmtest_hwtrace_begin_window`
   ([src/hwtrace.c:2554](../../../src/hwtrace.c#L2554)), replace the
   unconditional `EUNAVAIL` fallthrough with an `INTEL_PT` arm: call
   `asmtest_hwtrace_pt_begin_window`; on `OK`, store the ctx in a single
   process-global PT-window slot (`static void *g_pt_window; static uint32_t
   g_pt_window_gen; static int g_pt_window_tid;` — one active PT window at a
   time, `ASMTEST_HW_ESTATE` if busy, matching the region path's single
   `g_fd` slot precedent) and fill the handle with a reserved sentinel:
   `out->idx = 0xfffffffeu` (distinct from the `0xffffffffu` invalid
   sentinel), `out->gen = ++g_pt_window_gen`, `out->arm_tid = gettid`. Any
   other non-single-step backend keeps the `EUNAVAIL` self-skip; while you are
   editing that fallthrough block, reword its retained comment at
   [src/hwtrace.c:2585-2586](../../../src/hwtrace.c#L2585) to drop the refuted
   "Zen 3+" live floor for "Zen 4+" (the AMD LBR live floor is Zen 4 — see the
   ladder note in T3 step 2 — not Zen 3).
   Then fix the now-stale public doc-comments in
   [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h): the
   `begin_window` header at
   [:436-441](../../../include/asmtest_hwtrace.h#L436) currently promises
   "ASMTEST_HW_EUNAVAIL on a non-single-step backend (whole-window HW trace is
   forward-look)" — after this task `INTEL_PT` ARMS, so reword it to say PT now
   arms and only the OTHER non-single-step backends remain `EUNAVAIL`; and the
   forward-look block just above at
   [:428-433](../../../include/asmtest_hwtrace.h#L428) says PT/CEILING
   "begin_window self-skips to ASMTEST_HW_EUNAVAIL on them here (bare-metal
   Intel PT / Zen 3+)" — PT no longer self-skips, and its "Zen 3+" must become
   "Zen 4+".
3. In `asmtest_hwtrace_end_window`
   ([:2594](../../../src/hwtrace.c#L2594)), route `handle.idx == 0xfffffffeu`
   to the PT teardown BEFORE the `asmtest_ss_frame_lookup` path: verify
   `handle.gen`/`handle.arm_tid` match the slot and the current tid (mismatch
   → flag `trace->truncated = true`, still drain + free — the §Z4
   false-truncated-over-false-complete default, same shape the SS path
   implements); refresh the ctx's image (`asmtest_codeimage_refresh`) and take
   `when = asmtest_codeimage_now(img)`; call `asmtest_hwtrace_pt_end_window`.
4. Inside `pt_end_window` (T1), after decode: re-base the appended entries to
   ABSOLUTE addresses so the window ABI holds (the header documents
   "insns[] will hold ABSOLUTE addresses",
   [asmtest_hwtrace.h:436-441](../../../include/asmtest_hwtrace.h#L436)):
   snapshot `trace->insns_len` before decode, and add `base_ip` (from
   `base_ip_out`) to `trace->insns[i]` for the appended range (and the
   appended `blocks[]` range). Truncation: `overflow || rc != ASMTEST_HW_OK`
   → `trace->truncated = true` — the exact ~2-line policy
   [hardware-trace-plan.md](../plans/hardware-trace-plan.md) names.
5. Expose the window's byte source so callers can feed it: add
   `asmtest_codeimage_t *asmtest_hwtrace_window_image(void);` returning the
   active PT window ctx's image (NULL when no PT window is armed / the WEAK
   tier is armed). A C caller `asmtest_codeimage_track`s its exec region after
   `begin_window`; the .NET side already tracks via `JitMethodMap` and passes
   its own image (T4).
6. `make fmt` then `make docker-hwtrace`.

**Code.** Keep the single-step window path **byte-identical** — the PT arm is
a new branch keyed on `g_opts.backend`, and the existing tests
(`test_wholewindow_singlestep`, `test_wholewindow_ss_descend`,
`test_zeroctor_scope_hygiene`, `test_asynchop_flag`,
`test_crossthread_handle_collision`) are the regression net. `render_window`
([:2625](../../../src/hwtrace.c#L2625)) needs no change: PT window traces carry
absolute addresses like the WEAK tier, and moving/managed bytes route to
`asmtest_hwtrace_render_versioned` as documented.

**Tests.** On non-Intel hosts this arm is unreachable through the facade —
`asmtest_hwtrace_init(INTEL_PT)` refuses at the `available()` gate, and per
the Phase 2 correction in
[hardware-trace-plan.md](../plans/hardware-trace-plan.md) a facade test that
subverts that gate tests a mock, not the facade. So T2's CI surface is: (a)
the full existing suite stays green (`make docker-hwtrace`); (b)
`test_wholewindow_decode` still passes with the new `base_ip_out` argument;
(c) a new assertion in `test_pt_window_pair_selfskip` that
`asmtest_hwtrace_window_image()` returns NULL when no PT window is armed. The
live end-to-end proof is T5 — say so in the test's `# SKIP` line. This split
(synthetic decode in CI, capture on silicon) is the plan's own posture
([scoped-tracing-zeroconfig-plan.md](../plans/scoped-tracing-zeroconfig-plan.md)
§Z2 "Build-gate honesty").

**Docs.** Append a `### Changed` bullet under `## [Unreleased]` in
[CHANGELOG.md](../../../CHANGELOG.md): `asmtest_pt_decode_window` gained a
`base_ip_out` parameter (source-incompatible for direct C callers of the
decode entry; the facade and bindings are unaffected). User-facing tier docs
land with T5.

**Done when.**

- `make docker-hwtrace` green; no existing test regresses.
- `begin_window` under an inited single-step tier behaves exactly as before
  (existing tests prove it).
- On any host without an inited PT tier, `begin_window` under no init still
  returns `ESTATE`, and under a single-step init the WEAK tier still arms.
- Code review confirms exactly one PT arming implementation (T1's helpers)
  serves both the region-keyed path and the window path.

### T3 — The WEAK/STRONG/CEILING window ladder + runtime decode-trust probe  (S, depends on: T2)

**Goal.** A window-path auto-selector that returns STRONG (`INTEL_PT`) only
when the PT substrate is present AND the whole-window decode proves itself on
the synthetic fixture at runtime; otherwise WEAK (`SINGLESTEP`); with CEILING
(`AMD_LBR`) reported as the explicit sampled complement, never a silent
downgrade of the exact default.

**Steps.**

1. In [src/pt_backend.c](../../../src/pt_backend.c), add
   `int asmtest_hwtrace_pt_window_trusted(void)` (declare it in
   [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h) near
   `asmtest_hwtrace_resolve`): cached (`static int cached = -1;`), returns 1
   only if ALL of: `asmtest_pt_decoder_present()`,
   `asmtest_codeimage_available()`, and an in-process replay of the §Z2
   fixture round-trips — mmap an anonymous RW page, copy the 18 canonical
   ROUTINE bytes `{0x48,0x89,0xf8, 0x48,0x01,0xf0,
   0x48,0x3d,0x64,0x00,0x00,0x00, 0x7e,0x03, 0x48,0xff,0xc8, 0xc3}` (the same
   bytes as [examples/test_hwtrace.c:120](../../../examples/test_hwtrace.c#L120)),
   track them in a scratch `asmtest_codeimage_new(0)` image, encode the taken
   AND not-taken streams via `asmtest_pt_encode_fixture`, decode each through
   `asmtest_pt_decode_window`, and compare against the two expected walks
   ({0,3,6,0xc,0x11}/blocks {0,0x11}; {0,3,6,0xc,0xe,0x11}/blocks {0,0xe},
   re-based by `base_ip_out`). In the `!ASMTEST_HAVE_LIBIPT` block, stub it to
   return 0. This makes "§Z2's fixture is green" a *runtime* predicate, not a
   CI folk memory.
2. In [src/hwtrace.c](../../../src/hwtrace.c), add
   `int asmtest_hwtrace_window_auto(void)` beside `asmtest_hwtrace_auto`
   ([:487](../../../src/hwtrace.c#L487)): returns
   `ASMTEST_HWTRACE_INTEL_PT` when `asmtest_hwtrace_available(INTEL_PT)` &&
   `asmtest_hwtrace_pt_window_trusted()`; else
   `ASMTEST_HWTRACE_SINGLESTEP` when available; else
   `ASMTEST_HW_EUNAVAIL`. Document in its header comment that the CEILING
   tier (AMD LBR) is deliberately NOT returned here: the exact whole-window
   contract cannot be met by a sampled survey, so on AMD the ladder lands on
   WEAK for `begin_window`, and callers wanting the quiet sampled complement
   use the explicit `asmtest_hwtrace_sample_begin_amd` entry points /
   `new AsmTrace(HwBackend.AmdLbr)` (the landed §Z1.3 design change; live AMD
   LBR floor is **Zen 4+**).
3. .NET: in `ArmWholeWindow`
   ([HwTrace.cs:2140](../../../bindings/dotnet/hwtrace/HwTrace.cs#L2140)), add
   a P/Invoke for `asmtest_hwtrace_window_auto` next to the existing externs
   ([:563](../../../bindings/dotnet/hwtrace/HwTrace.cs#L563)); when it
   returns `IntelPt`, auto-init the `IntelPt` tier (generalize
   `AutoInitSingleStep` [:2184](../../../bindings/dotnet/hwtrace/HwTrace.cs#L2184)
   into `AutoInitWindowBackend(HwBackend b)` under the same `TierLock`) and
   proceed to `begin_window` — the PT arm from T2 then arms; the Dispose
   `Kind.WholeWindow` branch works unchanged because `end_window` routes the
   sentinel handle natively. Otherwise behavior is exactly today's
   (single-step). Update `WholeWindowSkipReason`/`DegradationNote` to name the
   PT probe outcome ("intel_pt present but decode untrusted" vs "no intel_pt
   PMU") using `asmtest_hwtrace_status(INTEL_PT)`.
4. `make fmt`, `make docker-hwtrace`, `make docker-hwtrace-bindings` (or at
   minimum `hwtrace-dotnet-test` in the dotnet image via
   `make docker-hwtrace-dotnet-example`'s image — the .NET self-test target is
   `hwtrace-dotnet-test`, [mk/native-trace.mk:2575](../../../mk/native-trace.mk#L2575)).

**Code.** No new capture code — the ladder is probes + dispatch. Keep the
trust probe allocation-light and run-once (it executes at first
`begin_window`/ctor arm on Intel hosts only in practice; on libipt-less hosts
it is a constant 0).

**Tests.** In `test_hwtrace.c`, add `test_window_ladder`: asserts
`asmtest_hwtrace_window_auto()` never returns a backend whose
`asmtest_hwtrace_available()` is 0; on a non-Intel host asserts the return is
not `INTEL_PT` (prints the resolved tier); where libipt is built asserts
`asmtest_hwtrace_pt_window_trusted()` returns 1 **iff**
`asmtest_codeimage_available()` (the fixture round-trip must succeed in the
hwtrace image — a probe failure there is a real decoder regression, same
signal as `test_wholewindow_decode`). Failure: `not ok` on the trust-probe
check in `docker-hwtrace`. The .NET side: `hwtrace-dotnet-test` still passes,
and on any host without the `intel_pt` PMU the empty ctor still arms
single-step (`Armed == true`, no PT).

**Docs.** Update
[docs/guides/tracing/scoped-tracing.md](../../guides/tracing/scoped-tracing.md)
(tier table: STRONG is now wired and auto-selected on trusted Intel PT hosts)
and [docs/scoped-tracing-implementation.md](../../scoped-tracing-implementation.md)
(the Docker-can/can't matrix row for the STRONG tier). CHANGELOG `### Added`:
window-path PT tier + ladder.

**Done when.**

- `make docker-hwtrace` green, including the new ladder test (trust probe
  returns 1 in the libipt-equipped image).
- On any host without the `intel_pt` PMU: `asmtest_hwtrace_window_auto()`
  returns `SINGLESTEP` and the empty ctor behaves exactly as before.
- The refuted "Zen 3+" floor appears nowhere in the new text — CEILING
  availability is written as Zen 4+.

### T4 — Wire the .NET inline ctor: `new AsmTrace(HwBackend.IntelPt)`  (M, depends on: T1; ladder consult from T3)

**Goal.** Replace the `IntelPt` self-skip branch in the backend-keyed ctor
with real arming through the native pair, a new Dispose kind, and a
finalizable ctx holder — decode-on-close filling `Addresses` with
`IsStatistical == false`.

**Steps.**

1. In [bindings/dotnet/hwtrace/HwTrace.cs](../../../bindings/dotnet/hwtrace/HwTrace.cs),
   add P/Invokes beside the AMD pair
   ([:563-566](../../../bindings/dotnet/hwtrace/HwTrace.cs#L563)):

   ```csharp
   [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_pt_begin_window(out IntPtr ctx);
   [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_pt_end_window(
       IntPtr ctx, IntPtr img, ulong when, IntPtr trace);
   ```

2. Add `PtWindow` to `enum Kind`
   ([:1871](../../../bindings/dotnet/hwtrace/HwTrace.cs#L1871)) and a
   `PtWindowCtx` finalizable holder mirroring `AmdSampler`
   ([:1807](../../../bindings/dotnet/hwtrace/HwTrace.cs#L1807)): `End(img,
   when, trace)` P/Invokes `pt_end_window` once (idempotent,
   `GC.SuppressFinalize`); the finalizer releases a leaked scope's fd/mmaps
   with `pt_end_window(ctx, IntPtr.Zero, 0, IntPtr.Zero)`.
3. In the backend-keyed ctor, replace the self-skip at
   [:2562-2567](../../../bindings/dotnet/hwtrace/HwTrace.cs#L2562) with an
   `IntelPt` branch ABOVE the residual guard (which keeps self-skipping
   `CoreSight`): set `_kind = Kind.PtWindow`; gate on
   `HwTrace.Available(HwBackend.IntelPt)` (self-skip with the reason from
   `HwTrace.SkipReason(HwBackend.IntelPt)` when false); create the
   `JitMethodMap(trackBytes: true)` + rundown BEFORE arming (copy the AMD
   branch's order [:2575-2578](../../../bindings/dotnet/hwtrace/HwTrace.cs#L2575)
   — the map must see methods JIT'd inside the window, and the rundown's
   socket I/O must not run under capture); pre-allocate the retained trace
   handle (`asmtest_trace_new`, same sizing as `ArmWholeWindow`
   [:2159](../../../bindings/dotnet/hwtrace/HwTrace.cs#L2159)); call
   `pt_begin_window`; on `OK` wrap the ctx in `PtWindowCtx`,
   `Thread.BeginThreadAffinity()` (the perf event is per-OS-thread — same
   caveat as AMD), `Armed = true`; on failure set a directive `SkipReason`
   (`$"Intel PT window did not arm (rc={rc})"`).
4. Add a `case Kind.PtWindow:` to the Dispose switch
   ([:3495](../../../bindings/dotnet/hwtrace/HwTrace.cs#L3495)) modeled on
   `Kind.AmdWindow` + the whole-window address readback in
   `Kind.WholeWindow` ([:3558](../../../bindings/dotnet/hwtrace/HwTrace.cs#L3558)):
   `_map.Stop()`; take `img = _map.ImageHandle`, `when = _map.ImageNow`
   (falls back to `IntPtr.Zero` → the ctx's own self image); call
   `_ptWinCtx.End(img, when, tr.Handle)`; `Thread.EndThreadAffinity()`; read
   `Truncated` and the ABSOLUTE `Addresses` off the trace
   (`asmtest_emu_trace_insns_len/_insn_at` — same loop as the OOP-inline
   branch); `IsStatistical` stays `false`; `AttributeAddresses(img, when)`;
   `DisablePerfMap` when requested; free the trace.
5. `make fmt` is C-only — for C# match the file's existing style. Build+run:
   `make docker-hwtrace-dotnet-example` image path or the dotnet lane your
   change touches; the binding self-test target is `hwtrace-dotnet-test`.

**Code.** Do NOT touch the `Kind.AmdWindow` or `Kind.WholeWindow` teardown
semantics; the PT branch is additive. The ctor path must honor the shim
invariants: never throws, `Armed=false` + `SkipReason` on every failure,
teardown of `_map`/rundown on a failed arm (the AMD branch shows the exact
order).

**Tests.** `hwtrace-dotnet-test`
([mk/native-trace.mk:2575](../../../mk/native-trace.mk#L2575), runs
`HwTraceProgram.cs`): add a case constructing
`new AsmTrace(HwBackend.IntelPt)` on any host and asserting the invariant
envelope — on a non-PT host: `Armed == false`, `SkipReason` non-empty and
naming the PT gate (not the old "forward-look (not wired)" text), `Dispose()`
does not throw, and a leaked scope does not crash the finalizer thread (GC +
`WaitForPendingFinalizers`). On PT silicon the same case flips to
`Armed == true`, `Addresses.Length > 0`, `IsStatistical == false` — the T5
runner exercises it. Failure: a non-zero exit from `dotnet run`; pass: the
program's summary line with 0 failures. Run in Docker:
`docker run --rm asmtest-dotnet make hwtrace-dotnet-test` after
`make docker-dotnet` (see [mk/docker.mk:478](../../../mk/docker.mk#L478) for
the image recipe).

**Docs.** Update the inline-`using` conformance table in
[docs/guides/tracing/scoped-tracing.md](../../guides/tracing/scoped-tracing.md)
and the hardware-trace tier section of
[docs/guides/tracing/native-tracing.md](../../guides/tracing/native-tracing.md)
(IntelPt inline: wired, silicon-gated live). CHANGELOG `### Added` bullet.
Also update [asmtrace-inline-using-plan.md](../archive/plans/asmtrace-inline-using-plan.md)'s
status line (R2 no longer "the sole remainder" once landed).

**Done when.**

- `hwtrace-dotnet-test` green on any host without the `intel_pt` PMU with the
  new self-skip case (skip text names the PT availability gate).
- `grep -n "forward-look (not wired)" bindings/dotnet/hwtrace/HwTrace.cs`
  shows the message only for the remaining genuinely-unwired backend
  (CoreSight).
- On PT silicon (T5's runner), the inline ctor arms and fills `Addresses`
  (recorded as part of T5's acceptance).

### T5 — Live self-JIT PT smoke + capture-side filter exercise + gated lane  (M, depends on: T2; exercises T3/T4)

**Goal.** A self-skipping live test that, on bare-metal Intel PT, arms the
region-free capture, decodes the REAL AUX stream through
`asmtest_pt_decode_window`, and exercises `PERF_EVENT_IOC_SET_FILTER`, the
anonymous-JIT decode-time-filter fallback, PSB cadence, and AUX-ring
truncation — plus a make target that turns the skip into a failure on a host
that claims PT.

**Steps.**

1. Add `test_pt_live_selfjit` to
   [examples/test_hwtrace.c](../../../examples/test_hwtrace.c), guarded like
   the other live paths (`#if defined(__linux__) && defined(__x86_64__)`),
   first probing `asmtest_hwtrace_available(ASMTEST_HWTRACE_INTEL_PT)` and
   printing the specific reason from `asmtest_hwtrace_skip_reason` on skip
   (`# SKIP pt live: no intel_pt PMU (needs bare-metal Intel; absent on
   AMD/VM)` on this dev box). Body, all through the PUBLIC surface:
   a. "Self-JIT" a routine: `asmtest_hwtrace_exec_alloc` the canonical
      ROUTINE bytes (W^X, icache-flushed), init the tier with
      `backend = ASMTEST_HWTRACE_INTEL_PT`.
   b. `asmtest_hwtrace_begin_window` → `asmtest_codeimage_track` the exec
      region on `asmtest_hwtrace_window_image()` → call the routine with
      taken args (20,22) → `end_window` → assert the decoded trace COVERS the
      taken walk re-based at the exec address, `truncated == false`, and the
      not-taken control (a second window, args 200,1) covers `0xe` — the same
      discriminator `test_wholewindow_decode` uses, now against real silicon
      packets (PSB cadence, timing packets, real enable/disable events all
      flow through `drain_events`).
   c. Truncation: a third window with `opts.aux_size` forced tiny (re-init
      with `aux_size = 4096`) around a long hot loop must come back
      `truncated == true` (the `PERF_AUX_FLAG_TRUNCATED` / head-clamp path on
      a real ring — the aux_head wrap behavior the synthetic fixture cannot
      reach).
   d. Capture-side filter: open a raw side event with T1's `pt_aux_open`
      pattern via a small test-local helper is NOT the way — instead add the
      minimal public knob this needs:
      `int asmtest_hwtrace_pt_set_filter(void *ctx, const char *filter);`
      (T1 pair, thin `ioctl(fd, PERF_EVENT_IOC_SET_FILTER, str)` wrapper,
      callable between begin and the traced call). The test builds the string
      `"filter 0x%lx/0x%x@%s"` for a known function in the test binary's own
      file-backed text (e.g. the address of a local `noinline` helper,
      object path from `/proc/self/exe`, `<start>` given as the symbol's
      OBJECT-RELATIVE offset — perf filter addresses are object-relative; if
      the kernel rejects that interpretation, retry with the runtime vaddr
      and record which form the kernel accepted in the test output — the
      ambiguity is a known research caveat, see Research notes), asserts the
      ioctl returns 0, and asserts the decoded stream then contains the
      filtered function and not the unfiltered sibling.
   e. Anon-JIT constraint: the same `SET_FILTER` string with
      `@/proc/self/exe` replaced by an ANONYMOUS exec_alloc region must FAIL
      (kernel matches filters only on file-backed VMAs; per-task file filters
      require a regular file) — assert the ioctl errors, then assert the
      decode-time path still recovers the region's instructions (the shipped
      fallback: no capture filter, full-stream decode) — this is the honest
      "anonymous JIT pages cannot be address-filtered" exercise.
   f. Read `/sys/bus/event_source/devices/intel_pt/nr_addr_filters` and print
      it (`# pt nr_addr_filters: N`) — recorded, not asserted (hardware
      varies).
2. Add the lane in [mk/native-trace.mk](../../../mk/native-trace.mk) next to
   `hwtrace-test` ([:2127](../../../mk/native-trace.mk#L2127)):

   ```make
   .PHONY: hwtrace-pt-live
   hwtrace-pt-live: $(BUILD)/test_hwtrace
   	@echo "== hwtrace-pt-live (REQUIRES bare-metal Intel PT) =="
   	ASMTEST_REQUIRE_PT=1 $(BUILD)/test_hwtrace
   ```

   In the test, `ASMTEST_REQUIRE_PT=1` converts the availability skip into a
   `CHECK` failure — so the target self-skips nowhere: on a runner that is
   *supposed* to have PT, a silently-hidden PMU is a red build, while plain
   `hwtrace-test` keeps the clean `# SKIP` everywhere else. Also run the .NET
   inline case there when the dotnet SDK is present (documented as manual:
   `make hwtrace-dotnet-test` on the same box).
3. Wire `hwtrace-pt-live` into `make help` text
   ([Makefile:117](../../../Makefile#L117) area) as
   `hwtrace-pt-live  live Intel PT whole-window smoke (bare-metal Intel + CAP_PERFMON/paranoid<0; fails rather than skips)`.
4. `make fmt`; `make docker-hwtrace` (the new test must `# SKIP` cleanly in
   the container); on PT silicon run `make hwtrace-pt-live`.

**Code.** No Docker lane can run this — the gate is hardware (no `intel_pt`
PMU in VMs/containers, nor on any of the non-PT dev boxes), which is one of the
two legitimate self-skip gates under the CLAUDE.md dependency rule. The runner itself
(provisioning a bare-metal Intel box, trusted-branch gating, `CAP_PERFMON`) is
[self-hosted-ci-runners.md](self-hosted-ci-runners.md)'s scope; this task
delivers the test + target that the runner will invoke, and records the gate.

**Tests.** This task IS the test. Failure modes to expect on silicon and their
meanings: decode yields zero instructions → the enable-event drain or PSB sync
regressed (`drain_events`, [src/pt_backend.c:131](../../../src/pt_backend.c#L131));
taken/not-taken walks identical → TNT following broke; `truncated` never set
with the tiny ring → the AUX drain/flag scan regressed. On any host without
the `intel_pt` PMU the whole test prints one `# SKIP` line and the count is
unchanged.

**Docs.** Update
[docs/guides/tracing/native-tracing.md](../../guides/tracing/native-tracing.md)
hardware-trace tier section: the PT whole-window path is wired and
live-validated (record the validation host once run), unprivileged AUX sizing
note (perf tool convention: 128 KiB unprivileged vs 4 MiB privileged — a
tool default, not a kernel cap; the tier's 64 KiB default AUX ring is safely
under it). Update the per-phase status blocks in
[scoped-tracing-zeroconfig-plan.md](../plans/scoped-tracing-zeroconfig-plan.md)
(§Z1 STRONG, §Z2 live half) and
[hardware-trace-plan.md](../plans/hardware-trace-plan.md) once the smoke has
actually run on silicon — not before (verify-before-declaring-done).
CHANGELOG `### Added` bullet for the lane.

**Done when.**

- `make docker-hwtrace` green with the new test present (`# SKIP pt live: …`
  in the container output — the lane self-skips cleanly on any host without the
  `intel_pt` PMU and in every container).
- `make hwtrace-pt-live` exists, appears in `make help`, and FAILS (not
  skips) when `ASMTEST_REQUIRE_PT=1` finds no PMU.
- On a bare-metal Intel PT box with `perf_event_paranoid < 0` or
  `CAP_PERFMON`: `make hwtrace-pt-live` passes all live checks, and
  `make hwtrace-dotnet-test` shows the inline `IntelPt` ctor armed. Until
  such a box exists, record the gate in the plan-status updates instead of
  claiming validation.

## Task order & parallelism

- **Critical path:** T1 → T2 → T3 → T5 (live validation exercises everything).
- **T4** depends only on T1 (the native pair) and can proceed in parallel
  with T2/T3 by a second person; its ladder consult (empty-ctor PT arm) is
  T3's step 3 and should merge after T3.
- **T5** is written after T2 but its silicon RUN is gated on hardware access
  and may land as a self-skipping test well before the box exists.

```
T1 ──> T2 ──> T3 ──┐
  └──> T4 ─────────┼──> T5 (live run: hardware-gated)
```

## Constraints & gates

- **Hardware gate (real, self-skip allowed):** live PT capture needs
  bare-metal Intel silicon with the `intel_pt` PMU exposed and
  `perf_event_paranoid < 0` or `CAP_PERFMON`. None of the reachable dev/cold-run
  boxes expose it, and for DIFFERENT reasons — so acceptance bullets below are
  written host-neutrally ("any host without the `intel_pt` PMU"), never
  "on AMD": the AMD Linux boxes (Zen 5 / Zen 2) lack the PMU on vendor grounds
  (`vendor_is("GenuineIntel")` is false), while the Intel macOS box
  (Core i7-8559U) has no Linux `perf_event_open` at all — and inside
  `docker-hwtrace` on it the vendor reads `GenuineIntel` yet the `intel_pt`
  sysfs node is absent, so PT there skips on the missing PMU node, not on
  vendor. VMs, Docker, and GitHub-hosted runners hide the PMU. Every live path
  must self-skip with the specific reason via `asmtest_hwtrace_skip_reason`, and
  `hwtrace-pt-live` converts that skip to a failure only where PT is claimed.
- **No new dependencies:** libipt is already pinned in the lane
  ([Dockerfile.hwtrace](../../../Dockerfile.hwtrace) `libipt-dev` on
  `ubuntu:24.04` = libipt v2.0.6). Everything else is kernel UAPI. Per
  CLAUDE.md, nothing in this doc may be "solved" by a self-skip except the
  hardware gate above.
- **One PT arm:** any reviewer finding a second `perf_event_open` on the
  `intel_pt` PMU outside T1's helpers should reject the change —
  [intel-pt-attach-foreign-pid.md](intel-pt-attach-foreign-pid.md) builds on
  these helpers.
- **License:** libipt is BSD; no new obligations.
- **Runner security:** a self-hosted PT runner executes code with lowered
  `perf_event_paranoid` — trusted-branch/maintainer-approved runs only
  (accepted in [hardware-trace-plan.md](../plans/hardware-trace-plan.md) CI
  notes; provisioning owned by
  [self-hosted-ci-runners.md](self-hosted-ci-runners.md)).
- **When the gate blocks validation, record:** which task landed
  wiring-complete-but-unvalidated, the exact command to run on silicon
  (`make hwtrace-pt-live`), and do NOT update the plan status blocks to
  "validated" until it has run.

## Research notes (verified 2026-07-17)

- **libipt pin.** `Dockerfile.hwtrace` installs Ubuntu noble's `libipt-dev`
  2.0.6-1build1 (libipt v2.0.6) on the `ubuntu:24.04` bindings base —
  <https://packages.ubuntu.com/noble/libipt-dev>.
- **PMU type.** Dynamic PMUs export an integer at
  `/sys/bus/event_source/devices/intel_pt/type` for `perf_event_attr.type`;
  config bits are described under `.../intel_pt/format` —
  <https://man7.org/linux/man-pages/man2/perf_event_open.2.html> (man2
  perf_event_open) and perf-intel-pt(1)
  <https://man7.org/linux/man-pages/man1/perf-intel-pt.1.html>.
- **AUX mmap rules (kernel v6.8 enforcement, `perf_mmap`,
  kernel/events/core.c:6466-6506).** Two mmaps on one fd: data area first at
  pgoff 0 (1 + 2^n pages; page 0 is `struct perf_event_mmap_page`); then set
  `aux_offset`/`aux_size` and mmap the AUX area. The kernel requires the data
  area already mapped, `aux_offset >= data_offset+data_size` (page-aligned),
  `aux_offset == vm_pgoff << PAGE_SHIFT`, and `aux_size == vma_size` =
  power-of-two pages —
  <https://raw.githubusercontent.com/torvalds/linux/v6.8/kernel/events/core.c>,
  man2.
- **RW vs RO AUX.** A `PROT_WRITE` AUX mapping sets `RING_BUFFER_WRITABLE`;
  `rb_alloc_aux` computes `overwrite = !(flags & RING_BUFFER_WRITABLE)`
  (ring_buffer.c:674) — an RO map is overwrite/snapshot mode, where
  `aux_tail` is not consulted (ring_buffer.c:421-425) and the consumer must
  disable measurement while reading —
  <https://raw.githubusercontent.com/torvalds/linux/v6.8/kernel/events/ring_buffer.c>,
  man2. perf-intel-pt(1): full-trace mode does not overwrite data the user
  has not collected (indicated by advancing `aux_tail`); snapshot mode
  overwrites.
- **Wrap protocol.** `aux_head`/`aux_tail` have the same semantics and
  ordering rules as `data_{head,tail}` (uapi perf_event.h:737-741): read
  `aux_head`, acquire barrier, copy `[aux_tail, aux_head)` modulo `aux_size`
  (two memcpys across a wrap), release-store `aux_tail = head` —
  <https://raw.githubusercontent.com/torvalds/linux/v6.8/include/uapi/linux/perf_event.h>.
  The shipped linear drain (fresh ring per window, `[0, head)`) never wraps
  by construction; T5's tiny-ring case exercises the truncation signal
  instead.
- **PERF_RECORD_AUX (=11).** Body `u64 aux_offset, aux_size, flags`
  (perf_event.h:1064-1079); flags: `PERF_AUX_FLAG_TRUNCATED` 0x01,
  `OVERWRITE` 0x02, `PARTIAL` 0x04, `COLLISION` 0x08 (perf_event.h:1261-1264).
- **ioctls (perf_event.h:551-557).** `PERF_EVENT_IOC_ENABLE` `_IO('$',0)`,
  `DISABLE` `_IO('$',1)`, `RESET` `_IO('$',3)`, `SET_FILTER`
  `_IOW('$',6,char*)`. Flow: `attr.disabled=1` → open → both mmaps →
  optional `SET_FILTER` → `ENABLE` → run → `DISABLE` before reading.
- **Address filters (kernel/events/core.c:10760-10776).** String =
  `"ACTION RANGE_SPEC"`, ACTION ∈ `filter|start|stop`; object-file spec
  `<start>[/<size>]@</path/to/object>`; zero/absent size = single address,
  not valid for `filter`. Constraints: the `@file` must be a regular file
  (S_ISREG, core.c:10933); file-based filters are per-task-event only
  (core.c:10914); filters match only file-backed VMAs by inode
  (`perf_addr_filter_match`, core.c:8816-8825) — **anonymous/JIT pages
  cannot be address-filtered** (the decode-time fallback is mandatory, not
  optional). Per-PMU count at
  `/sys/bus/event_source/devices/intel_pt/nr_addr_filters` (core.c:11362).
  *Unverified:* whether `<start>` in the `@file` form is a file offset or a
  runtime vaddr — T5 tries the object-relative offset first and records the
  accepted form.
- **128 KiB "rule".** Not a kernel cap: kernel accounting is
  `perf_event_mlock_kb` (516 KiB default) + `RLIMIT_MEMLOCK`; 128 KiB is the
  perf *tool's* default auxtrace mmap size for unprivileged users (4 MiB
  privileged) — perf-intel-pt(1).
- **aux_watermark** (since 4.1) sets how much data triggers a
  `PERF_RECORD_AUX`; kernel default is half the buffer — man2. Irrelevant to
  the stop-then-drain shape here; matters for
  [intel-pt-attach-foreign-pid.md](intel-pt-attach-foreign-pid.md)'s live
  draining.
- **Foreign pid** (`pid > 0`) needs `CAP_PERFMON` (since 5.9) or a ptrace
  access check — man2. Out of scope here (self-trace only).
- **libipt v2.0.6 pt_insn API** (verified against the v2.0.6 tag header
  <https://raw.githubusercontent.com/intel/libipt/v2.0.6/libipt/include/intel-pt.h.in>;
  workflow per
  <https://raw.githubusercontent.com/intel/libipt/master/doc/howto_libipt.md>):
  zero `pt_config`, `begin/end` = a linearized AUX snapshot (de-wrap before
  decode); optionally set `.cpu` + `pt_cpu_errata` (the capture side may fill
  it from silicon; the shipped decode leaves it zeroed = no errata
  workarounds); `pt_insn_sync_forward` → `-pte_eos` when no PSB remains
  (:1011); `pt_insn_next` (:2187) with pending events drained via
  `pt_insn_event` on `pts_event_pending` (=1<<0, :1094) — already implemented
  in `drain_events`; `-pte_nomap` = IP outside the image (:141,159);
  `pt_image_add_file(image, filename, offset, size, asid, vaddr)`
  (:1817-1821) exists but the tree uses `pt_image_set_callback` instead —
  keep the callback form.

## Out of scope

- **Foreign-pid PT attach** (opening the event on `pid > 0`, live draining,
  `aux_watermark` tuning, CAP_PERFMON acquisition):
  [intel-pt-attach-foreign-pid.md](intel-pt-attach-foreign-pid.md) — it
  extends T1's helpers.
- **The live managed §Z3 compose and async-hop stitching escalation** (live
  .NET runtime over the PT window, per-hop slices):
  [managed-wholewindow-compose.md](managed-wholewindow-compose.md).
- **Replaying PT windows into the dataflow tier:**
  [dataflow-pt-replay-tier.md](dataflow-pt-replay-tier.md).
- **ARM CoreSight live decode** (the other hardware backend):
  [coresight-live-decode.md](coresight-live-decode.md).
- **Provisioning/gating the bare-metal Intel runner:**
  [self-hosted-ci-runners.md](self-hosted-ci-runners.md).
- **AMD LBR documentation/floor corrections (Zen 4+) and the silicon-gated
  capture arm:** [amd-branchsnap-lbr-docs.md](amd-branchsnap-lbr-docs.md).
