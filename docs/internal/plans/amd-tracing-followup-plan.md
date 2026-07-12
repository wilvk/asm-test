# asm-test — AMD tracing improvements (10-item follow-up): implementation plan

A focused follow-up to [amd-tracing-plan.md](amd-tracing-plan.md) that lands ten
concrete improvements to the AMD branch-record trace tier
([src/amd_backend.c](../../../src/amd_backend.c),
[src/hwtrace.c](../../../src/hwtrace.c),
[src/branchsnap.c](../../../src/branchsnap.c),
[src/msr_lbr.c](../../../src/msr_lbr.c),
[src/trace_auto.c](../../../src/trace_auto.c)). The items come from a fresh drill-down
against the source of record — the gating chain and capture paths above, cross-read
with the [tracing decision matrix](../analysis/tracing-decision-matrix.md) and the
F1–F47 audit in [../../amd_tracing_review.md](../../amd_tracing_review.md) — and every
finding here was **re-verified against the current tree** (the `src/` anchors hold at
`58d8263` — the intervening commits are docs-only) before being written down, not carried
forward from the audit.

> Status: **Nine items remain *planned*; Phase 7 (IBS-Op survey fallback) is
> SUPERSEDED + LANDED 2026-07-12** via
> [zen2-ibs-tracing-plan.md](zen2-ibs-tracing-plan.md) (its Phase 4 shipped the same
> integration — including this plan's `ASMTEST_FORCE_IBS_SURVEY` env — on the
> standalone `asmtest_ibs_*` engine instead of 7a–7d's in-`amd_backend.c` shape).
> Grounding that reorders priorities versus the 2026-07-09 audit: development runs on
> **two AMD hosts** (both recorded under [benchmarks/boxes/](../../../benchmarks/boxes/)) —
> a **Zen 5 Ryzen 9 9950X** box (`AuthenticAMD`, `amd_lbr_v2` + `perfmon_v2` + `ibs`,
> Linux 6.17) with `perf_event_paranoid=4`, and a **Zen 2 Ryzen 9 4900HS** (no branch
> stack, `paranoid=2`, Linux 6.14) — plus generic/Intel CI runners. So the audit's
> *"dev host is Zen 2, every branch-stack path self-skips"* framing is **superseded on
> the Zen 5 box** (the LBR-v2 paths run live under `make docker-hwtrace-amd`) while
> remaining literally true on the Zen 2 one, which is where the IBS lane was
> empirically built and validated. Already landed (context, out of scope here): the MSR spec-filter,
> the MSR-direct cascade rung, and the internal `amd_backend.h` header (`37118ec`), and
> the partial-never-complete honesty invariant (`07a9e7d`). House rule in force
> throughout: **no untested hardware code** — a path that cannot self-validate on its
> silicon returns `available()→0` rather than emitting an unproven trace, so every item
> below carries either a host-independent synthetic test or a self-skipping cap lane.

---

## Landing order

Phases are numbered 1–10 to match the item numbers in the drill-down (and the review's
F-findings) for traceability; **land them in the wave order below**, which is sequenced
by risk, dependency, and how much of each is testable on the generic CI host.

| Wave | Phases | Theme | Why this order | CI-testable? |
|---|---|---|---|---|
| **1** | 2, 8, 9, 4 | Quick wins — memory-safety clamp, perf, hygiene, logging | Low effort, low blast radius, high value; mostly host-independent | Mostly yes |
| **2** | 3 | Observability — `EPERM` vs `EUNAVAIL` + paranoid reader | Standalone diagnostics; the errno it threads also enriches Phase 4's log lines (each independently shippable) | Yes (invariants) + cap lane for the positive `EPERM` |
| **3** | 1 | ABI-guard the options struct (10-binding flag day) | High blast radius; do it deliberately, after the cheap wins | Yes (ASan, single-step) |
| **4** | 5 → 6 | Multi-exit deterministic snapshot; then the cascade-comment fix it enables | 6 is comment-only and only correct *after* 5 | Exit-enum: yes · live snapshot: cap lane |
| **5** | 7 (7a–7d) | IBS-Op survey fallback — **superseded + landed** via [zen2-ibs-tracing-plan.md](zen2-ibs-tracing-plan.md) | Largest / most speculative; self-skips by default | Decoder + probe: yes · capture: unprivileged (`swfilt`) |
| **6** | 10 | Document the AMD LBR window-reach levers | Naturally documents Phase 5's snapshot + the levers | Docs build only |

---

## Phase 1 — ABI-guard the hwtrace options struct (`include/asmtest_hwtrace.h`, all 10 bindings) *(planned)*

**Goal.** Make growing `asmtest_hwtrace_options_t` permanently safe: a caller compiled against an
older, smaller struct must never be read out of bounds by the library's init copy. Today
[src/hwtrace.c:471](../../../src/hwtrace.c) does `g_opts = *opts;` — a full
`sizeof g_opts` (48-byte) struct copy — while the struct has already grown twice
(`lbr_period`, `branch_filter` appended). Two mirrors already hand-pad to 48 bytes and
cite *"`g_opts = *opts` reads 8 bytes OOB"* in comments
([bindings/java/HwTrace.java](../../../bindings/java/HwTrace.java),
[bindings/ruby/hwtrace.rb](../../../bindings/ruby/hwtrace.rb)) — this replaces that
fragile per-binding hand-padding with a real size negotiation.

**Work.**
- **Prepend** `size_t struct_size` as the **first** field of `asmtest_hwtrace_options_t`
  ([include/asmtest_hwtrace.h:68](../../../include/asmtest_hwtrace.h)). Prepend, not
  append: a size negotiator must sit at an offset every version agrees on, and offset 0 is
  trivially agreed by all — as in `sched_attr` (leading `size`) and `perf_event_attr`
  (`size` at a fixed offset directly after its immutable `type`). Appending is
  self-defeating: you cannot locate a trailing field without already knowing the total
  size, the exact unknown.
- Replace the blind copy at [src/hwtrace.c:469-471](../../../src/hwtrace.c) with a
  clamped, zero-tail copy (recommended **reject-0** semantics):
  ```c
  size_t n = opts->struct_size;
  if (n == 0 || n < offsetof(asmtest_hwtrace_options_t, backend) + sizeof(int))
      return ASMTEST_HW_EINVAL;              /* caller did not self-describe */
  if (!asmtest_hwtrace_available(opts->backend))
      return ASMTEST_HW_EUNAVAIL;
  if (n > sizeof g_opts) n = sizeof g_opts;  /* newer caller: ignore unknown tail */
  memset(&g_opts, 0, sizeof g_opts);          /* zero the tail → C defaults */
  memcpy(&g_opts, opts, n);
  g_opts.struct_size = sizeof g_opts;         /* normalize */
  ```
  Documented lower-churn alternative: map `n==0` to the legacy size
  `sizeof g_opts - sizeof(size_t)` (keeps existing zero-fill callers working, still
  memory-safe, but can silently drop a trailing AMD field). **Never** map `0 → sizeof g_opts`
  (that re-introduces the OOB for a legacy `INTEL_PT=0` caller).
- **Set `struct_size` in every caller.** In-repo C: [src/trace_auto.c:170-173](../../../src/trace_auto.c)
  and ~27 sites in [examples/test_hwtrace.c](../../../examples/test_hwtrace.c) /
  [examples/test_branchsnap.c](../../../examples/test_branchsnap.c) — add a local
  `#define INIT_OPTS(o,b) do{ memset(&(o),0,sizeof(o)); (o).struct_size=sizeof(o); (o).backend=(b);}while(0)`
  to cut churn and prevent a missed site.
- **Every binding mirror** must add the field *and* set it (layout-from-header bindings
  still must set it):
  [python](../../../bindings/python/asmtest/hwtrace.py) (`c_size_t` first in `_Options._fields_`),
  [cpp](../../../bindings/cpp/asmtest_hwtrace.hpp) (`opts.struct_size = sizeof(opts)`),
  [rust](../../../bindings/rust/src/hwtrace.rs) (`struct_size: usize` first),
  [go](../../../bindings/go/hwtrace.go) (first cgo field **and** fix the pre-existing bug
  at `hwtrace.go:438-446` that leaves `lbr_period`/`branch_filter` uninitialized),
  [node](../../../bindings/node/hwtrace.js) (`koffi.sizeof`),
  [java](../../../bindings/java/HwTrace.java) (`JAVA_LONG` first in `OPTIONS_LAYOUT`, 48→56 bytes; update the offset comment),
  [dotnet](../../../bindings/dotnet/hwtrace/HwTrace.cs) (`UIntPtr StructSize` first, both init sites),
  [ruby](../../../bindings/ruby/hwtrace.rb) (48→56-byte pack; update offsets),
  [lua](../../../bindings/lua/hwtrace.lua) (`ffi.sizeof`),
  [zig](../../../bindings/zig/src/hwtrace.zig) (`@sizeOf`).

**Acceptance.** A host-independent C unit test allocates *exactly* a legacy-sized buffer
(`sizeof(asmtest_hwtrace_options_t) - sizeof(size_t)`), sets `struct_size` to that legacy
size and `backend=SINGLESTEP`, and calls `asmtest_hwtrace_init` under **ASan/UBSan** with
no read past the allocation; `struct_size==0` → `EINVAL`; a pretend-future `struct_size =
sizeof+64` returns OK without over-reading. Every `make docker-hwtrace-<lang>` still
marshals + inits and traces the single-step backend. `scripts/check-bindings-parity.sh`
(symbol-based) stays green with no allow-list edit.

## Phase 2 — Overflow-safe `nr` clamp at the sampled branch-stack parse (`src/hwtrace.c`) *(planned)*

**Goal.** Close a latent OOB read in the Tier-B sampled decode. At
[src/hwtrace.c:822](../../../src/hwtrace.c) the per-sample gate is only `nr > 0 &&` before
computing `nr * sizeof(struct perf_branch_entry) <= h.size` — a `uint64_t` multiply that
can wrap under `h.size`, after which the tally loop at
[src/hwtrace.c:835](../../../src/hwtrace.c) reads `e[i]` for `i < nr` unbounded. The two
sibling survey drains at [src/hwtrace.c:1070](../../../src/hwtrace.c) and
[src/hwtrace.c:1219](../../../src/hwtrace.c) already carry the `nr <= 64` guard; this site was missed.

**Work.** Add `nr <= 64` **before** the multiply (short-circuit), matching the siblings
verbatim:
```c
if (nr > 0 && nr <= 64 &&
    sizeof h + sizeof(uint64_t) + nr * sizeof(struct perf_branch_entry) <= h.size) {
```
Use the constant `64` (not `amd_depth`, which is not computed until
[src/hwtrace.c:862](../../../src/hwtrace.c)) to keep the three parse sites byte-identical. Real AMD
LBR is ≤16-deep, so a well-formed sample is never rejected; an `nr>64` record is corrupt
and correctly dropped (the surrounding `lost`/near-full logic already flags truncation
honestly).

**Acceptance.** Ideally, extract a shared `static amd_parse_sample()` helper used by all
three sites and add a host-independent unit test that feeds a hand-built
`PERF_RECORD_SAMPLE` whose `nr` is a crafted overflow value, asserting the record is
rejected under ASan. Minimum bar: the one-token clamp makes site 822 identical to the
already-tested logic at 1070/1219; `make docker-hwtrace-amd` shows no regression in normal
Tier-B decoding. (The overflow is not reproducible from real hardware — the kernel never
emits `nr>16` — so the synthetic ASan test is the real guard.)

## Phase 3 — Distinguish `EPERM` from `EUNAVAIL`; expose a rich status + paranoid reader (`src/hwtrace.c`) *(planned)*

**Goal.** On this very host (`perf_event_paranoid=4`) a permission denial and a hardware
absence both surface as `available()==0` / `EUNAVAIL`, separable only by string-parsing
`skip_reason`. `amd_branch_probe` already tags `AMD_NOPERM` internally
([src/hwtrace.c:238-257](../../../src/hwtrace.c)) but `available()` collapses it. Surface
`EPERM` distinctly **without** changing the `available()→{0,1}` ABI or any capture entry's
return code.

**Work.**
- Append (only) `#define ASMTEST_HW_EPERM (-9)` after `EDECODE=-8`
  ([include/asmtest_hwtrace.h:57](../../../include/asmtest_hwtrace.h)); `-4/-7` stay
  intentionally skipped, mirroring `asmtest_drtrace.h`.
- Add `asmtest_hwtrace_status_t {available; code; stage; perf_event_paranoid; probe_errno;
  reason[160];}` and `int asmtest_hwtrace_status(backend, *out)` plus
  `int asmtest_hwtrace_perf_event_paranoid(void)` (reads
  `/proc/sys/kernel/perf_event_paranoid`, `INT_MIN` if absent). Use documented `int`
  `stage` values, **not** a new typedef enum (no binding-mirrored type).
- Capture errno at the failing probe open: `amd_branch_probe(int *out_errno)` and a
  `perf_permitted_e(backend, int *out_errno)` clone (keep `perf_permitted` as a thin
  `return perf_permitted_e(b,NULL)`); classify `errno==EACCES||EPERM → EPERM`, `EINVAL →
  EUNAVAIL`. Factor a single `hw_classify()` that both `status()` and `skip_reason()`
  reuse (dedup); leave `available()` byte-unchanged.
- Enumerate every backend's `EPERM`-able point (Intel PT / AMD sampled / CoreSight via
  `perf_permitted`; snapshot via BPF load; MSR via `open_msr`; survey via perf_open) — see
  the drill-down's edge-case list; the snapshot/MSR/survey paths are **not** in the backend
  enum, so `status()` documents them as a known gap (errno out-params are a follow-up).
- Add `ALL asmtest_hwtrace_status` and `ALL asmtest_hwtrace_perf_event_paranoid` to
  [scripts/bindings-parity-allow.txt](../../../scripts/bindings-parity-allow.txt) (both are
  TIER_HEADER symbols; additive diagnostics wrapped opportunistically like `skip_reason`).

**Acceptance.** Host-independent: `status(SINGLESTEP).code==OK`; the invariant
`st.available == (available(b)?1:0) == (st.code==OK)` for all four backends;
`status(INTEL_PT).reason == skip_reason(INTEL_PT)` (locks the dedup); `st.perf_event_paranoid
== asmtest_hwtrace_perf_event_paranoid()`. Positive `EPERM` on the cap lane: extend
`make docker-hwtrace-amd` to run once with `perf_event_paranoid` raised (or `CAP_PERFMON`
dropped) asserting `status(AMD_LBR).code==ASMTEST_HW_EPERM`, and once permitted asserting
`OK`. Parity gate passes after the allow-list lands; the `rc in {OK,EUNAVAIL}` binding
assertions stay green (no capture-entry code changed).

## Phase 4 — Env-gated debug logging for the AMD/hwtrace tier (`src/debug.{c,h}`) *(planned)*

**Goal.** With this many stacked gates (vendor → PMU → `perfmon_v2` → kernel → caps →
freeze bit), a self-skip is opaque. Add zero-overhead-when-off logging that explains the
skip, the backend/mode chosen, the Tier-A vs Tier-B pick, and every truncation reason.
There is no logging in `src/` today; precedent is
[src/codeimage.c](../../../src/codeimage.c)'s `ASMTEST_CODEIMAGE_DEBUG`.

**Work.**
- New internal `src/debug.h` / `src/debug.c` (not in `include/`, mirroring
  `amd_backend.h`): `asmtest_hwtrace_debug_enabled()` (getenv-once cached, honoring
  `ASMTEST_HWTRACE_DEBUG` tier-wide with `ASMTEST_AMD_DEBUG` as an AMD alias) and a
  `printf`-attributed `asmtest_hwtrace_debugf(fn, fmt, ...)`, wrapped in an
  `ASMTEST_HWDBG(...)` macro that prefixes `[asmtest hwtrace] <func>: `.
- `#include "debug.h"` in `hwtrace.c` / `amd_backend.c` / `branchsnap.c` / `msr_lbr.c`;
  add `src/debug.c` to [mk/native-trace.mk](../../../mk/native-trace.mk)'s object list **and
  every link list that pulls the hwtrace objects** (a missed list is a build-visible link
  error).
- Log sites (`key=val`, one line): `amd_branch_probe` rc/errno; each `available()` self-skip
  with decoder/cpu verdict; `hwtrace_begin_amd` snapshot-vs-sampled choice + `exit_off`/
  `nexit`/`filter`; `hwtrace_end_amd` `n_samples`/`best_nr`/`best_inregion`/`lost`, the
  Tier-A/Tier-B branch, and a paired line at **each** of the 6 truncation sites; `amd_replay`'s
  5 desync causes; `branchsnap` begin/end stages; `msr_trace` open/read/short-read outcomes;
  survey period/nips/truncated; PT/CS arm+decode.
- **Signal-safety:** every listed site is ordinary thread context (reached via
  `asmtest_hwtrace_try_begin`/`_end`), *not* the single-step `SIGTRAP` handler
  `ss_on_sigtrap` ([src/ss_backend.c:202](../../../src/ss_backend.c)), which stays
  async-signal-safe — add a one-line comment there forbidding `ASMTEST_HWDBG`.

**Acceptance.** Host-independent (a standalone program / forked child so the getenv cache
starts clean): with `ASMTEST_HWTRACE_DEBUG=1` set before the first tier call, captured
stderr contains `[asmtest hwtrace] `; in a fresh process with it unset, stderr is empty
(zero-overhead-when-off); `ASMTEST_AMD_DEBUG` works as an alias. New files pass clang-format
18.1.3 and the `-Werror` native-trace tier (the `printf` attribute is the compile-time
check). Cap lane: `ASMTEST_AMD_DEBUG=1 make docker-hwtrace-amd` emits the `begin_amd` mode
line, an `end_amd` tier line, and ≥1 truncation-reason line on a deliberately-overflowing
fixture. `debug.{c,h}` are internal → no parity allow-list entry.

## Phase 5 — Multi-exit deterministic BPF boundary snapshot (`src/branchsnap.c`, `src/hwtrace.c`) *(planned)*

**Goal.** Make the deterministic snapshot the default for **2..4-exit** routines, not just
single-exit ones — retiring most reliance on the sampled path's fragile richest-window
guess. Today `hwtrace_begin_amd` selects the snapshot only when `amd_nexit == 1`
([src/hwtrace.c:633](../../../src/hwtrace.c)), and `branchsnap.c` plants exactly one HW
execution breakpoint, so a tiny routine leaving via an earlier `ret`/tail-jmp truncates on
the flood.

**Work.**
- **Enumerate all exits.** Add `asmtest_amd_all_exits(base, len, out, cap, *n)` next to
  [src/hwtrace.c:579](../../../src/hwtrace.c) `asmtest_amd_last_exit_off`, reusing the same
  Capstone ret/region-leaving-tail-jmp classification; refactor `last_exit_off` to a thin
  wrapper (`all_exits(base,len,NULL,0,nexit)`) so the existing host-independent exit test
  passes byte-for-byte. Add `#define ASMTEST_AMD_MAX_EXITS 4` (x86 `HBP_NUM`) to
  [src/amd_backend.h](../../../src/amd_backend.h); declare `all_exits` there.
- **Multi-breakpoint begin.** In [src/branchsnap.c](../../../src/branchsnap.c) turn the
  single `int bfd` / `struct bpf_link *link` into `bfd[ASMTEST_AMD_MAX_EXITS]` /
  `link[ASMTEST_AMD_MAX_EXITS]` (+ `int nbp`); add
  `asmtest_amd_snapshot_begin_multi(base, len, exit_offs[], nexit, filter)` that opens one
  `HW_BREAKPOINT_X` per exit into a distinct DR (the LBR `lfd` is a PMU *counter*, not a DR,
  so all 4 debug registers are free), attaches the **same** BPF prog to each (N `bpf_link`s,
  one shared 256 KB ringbuf), and enables all. Make the single-exit `snapshot_begin` a
  wrapper over it. **Require all N exits or fall back to sampling** (a partially-covered set
  could silently miss the taken exit); update `bsnap_teardown` to loop-destroy. `bsnap_on_event`
  / `bsnap_drain` / `snapshot_end` need **no change** — all breakpoints feed one ring and the
  richest-in-region selection already works across multiple hits.
- **Default-on for 1..4 exits.** In `hwtrace_begin_amd` (rewrite
  [src/hwtrace.c:631-642](../../../src/hwtrace.c)) enumerate once; select the snapshot when
  `asmtest_amd_snapshot_available() && nexit_total ∈ [1,4]`; `>4` exits stay sampled;
  explicit `opts.snapshot` with `>4` keeps the legacy last-exit best-effort. Update the
  comment block.

**Acceptance.** Host-independent: extend `test_amd_tailcall_exit` with `all_exits` cases —
a 3-`ret` region (`n==3`, ascending offsets, return==last); a 5-exit region with `cap=4`
(`n==5` total, `written==4`, return==true last, so `begin_amd` skips it); `ret+tail-jmp`
(`n==2`); classification equals `last_exit_off`. Cap lane (Zen 4/5 + `CAP_BPF` +
`CAP_PERFMON`, the codeimage image): a two-`ret` fixture in
[examples/test_branchsnap.c](../../../examples/test_branchsnap.c) driven with `opts.snapshot`
**unset** (proving default-on) once down each path — entry block covered and `!truncated`
on both. DR-slot arbitration self-skips honestly to sampled where a debugger holds a DR.

## Phase 6 — Cascade rung: no new code; correct the now-stale MSR-rung comment (`src/trace_auto.c`) *(planned, depends on Phase 5)*

**Goal.** Confirm and document that no explicit BPF-snapshot rung belongs in
`asmtest_trace_call_auto` — the snapshot is already effectively rung 1 via
`hwtrace_begin_amd`, and Phase 5 extends that default to 2..4 exits, which is exactly the
multi-exit tiny-routine case a rung would have targeted. A re-run rung would only duplicate
a completed capture or re-attempt a deterministically-failed one.

**Work.** Comment-only:
- Reword the rung-1b comment at [src/trace_auto.c:192-205](../../../src/trace_auto.c): its
  claim that the snapshot *"must skip"* every multi-exit region becomes false once Phase 5
  lands. New wording: the MSR rung is the backstop for regions with **more exits than the 4
  debug registers** (`>ASMTEST_AMD_MAX_EXITS`) or any multi-exit region on a host without
  `CAP_BPF`; the 1..4-exit case is completed in rung 1 by the multi-exit snapshot.
- Add a short rationale comment after [src/trace_auto.c:190](../../../src/trace_auto.c)
  recording the rejected rung and the correct order (snapshot in rung 1: `CAP_BPF+PERFMON`,
  no extra run; MSR rung 1b: `CAP_SYS_ADMIN`, a second in-process run; both share the
  16-branch ceiling and are excluded under `CEILING_FREE` in lock-step — verify the rung-1
  `hp` and rung-1b `!(policy & CEILING_FREE)` guards stay aligned).

**Acceptance.** No runtime behavior changes. Regression on the cap lane: a 2..4-exit tiny
routine is completed by rung 1 (`used->backend == AMD_LBR`, `!truncated`) and does **not**
reach the ~1000× block-step; assert "covered OR truncated" per the AMD-LBR truncation
lesson. A doc check confirms `trace_auto.c` no longer claims the snapshot skips all
multi-exit regions.

## Phase 7 — IBS-Op survey fallback (`src/hwtrace.c`, `src/amd_backend.c`) *(SUPERSEDED + LANDED 2026-07-12)*

> **Superseded by [zen2-ibs-tracing-plan.md](zen2-ibs-tracing-plan.md), whose Phase 4
> landed this integration 2026-07-12.** The shipped shape differs from 7a–7d below: the
> probe/decoder/capture live in the standalone `asmtest_ibs_*` engine
> (`include/asmtest_ibs.h`, `src/ibs_backend.c`) rather than `amd_backend.c`, with internal
> window primitives (`asmtest_ibs_window_begin`/`_end`) giving `sample_window_amd` and the
> begin/end split their fallback; this plan's `ASMTEST_FORCE_IBS_SURVEY` env and the
> "survey-only, never parity" invariant shipped as specced. Also note 7a–7b's privilege
> framing was corrected empirically: with the kernel `swfilt` bit, user-only IBS opens
> **unprivileged at `paranoid=2`** and `exclude_kernel` works (no cap lane needed for the
> user-only path). Kept below for the record.

**Goal.** The statistical whole-window survey
([src/hwtrace.c:976](../../../src/hwtrace.c)) has one hardware source — the LBR/BRS branch
stack — and returns `EUNAVAIL` without `CAP_PERFMON`. This host reports `ibs`. Add **AMD
IBS-Op** as a second source *under the survey tier only*, ordered strictly after
branch-stack, self-skipping by default (so the Zen 5 box and all CI are byte-identical to
today). IBS never feeds `asmtest_amd_decode` / `insns[]` / the fidelity cascade — it stays
survey-only, exactly as [amd-tracing-plan.md](amd-tracing-plan.md) requires.

**Work (four sub-phases).**

### 7a — Probe + attr builder
- Cached CPUID caps probe `asmtest_amd_ibs_brntrgt()` (Fn`8000_001B` EAX[0]=IBSFFV &&
  EAX[5]=BrnTrgt) in [src/amd_backend.c](../../../src/amd_backend.c), next to
  `asmtest_amd_freeze_available`; declare in `amd_backend.h`.
- `ibs_op_pmu_type()` reading `/sys/bus/event_source/devices/ibs_op/type`; `ibs_op_fill_attr()`
  building the attr with **no** `exclude_kernel`/`exclude_hv` (the IBS PMU has no user filter
  and `EINVAL`s those bits), `config = IbsOpCntCtl(bit 19)`, `sample_type = PERF_SAMPLE_IP |
  PERF_SAMPLE_RAW`.
- Public `asmtest_amd_ibs_op_available()` (vendor + node + BrnTrgt cap + a disabled-open
  permission probe). Extend `skip_reason`'s AMD arm to append the IBS verdict.

### 7b — Pure raw decoder (host-independent, the correctness anchor)
- `ibs_decode_op_raw(raw, rawlen, *endpoint)`: validate `rawlen` is a `u64` multiple ≥ the
  fixed register count (never index OOB), require a **retired taken** branch
  (`IbsOpData` bits), return the `BrTarget` word as the endpoint. Plus `ibs_sample_body()`
  to peel the `[u64 ip][u32 raw_size][raw]` framing with the same bound-before-index
  discipline as the branch-stack drain. Expose non-static for the test seam.

### 7c — Integration
- In `sample_window_amd`/`sample_begin_amd`, on branch-stack `perf_open` failure (or when
  `ASMTEST_FORCE_IBS_SURVEY` is set and IBS is available) delegate to a `sample_window_ibs()`
  that drains via 7b into the same `ips[]` endpoint histogram, preserving `*truncated =
  (lost || n==0)`. Add `int is_ibs` to the ctx for the begin/end split; factor one shared
  `ibs_drain()`.

### 7d — Validation + self-test gate
- Land the pure `test_ibs_decode_raw()` and `test_ibs_op_available()` in CI (no hardware).
- Add a **live self-test inside `asmtest_amd_ibs_op_available()`** (one-shot, cached, AMD+IBS
  only): arm IBS over a tiny fixed hot loop, require a strong majority of endpoints in-region,
  and cache `0` (self-skip the whole lane) if the kernel raw layout differs from 7b's
  constants — converting the one hardware-truth uncertainty (bit/word positions) into a safe
  self-skip, never garbage endpoints.
- Add a `docker-hwtrace-amd-ibs` cap lane and update
  [amd-tracing-plan.md](amd-tracing-plan.md) Phase 7 for what landed vs stays forward-look.

**Acceptance.** `test_ibs_decode_raw` (synthetic `u64 w[8]`, retired-taken → endpoint;
non-taken / zero-target / short `rawlen` → 0) and `test_ibs_op_available` (returns 0, no
crash, on the CI host) pass everywhere. On the Zen 5 box `ASMTEST_FORCE_IBS_SURVEY=1` over
the existing hot-loop survey test puts the majority of endpoints in-loop (live decode
cross-validated against branch-stack). With the env unset and branch-stack available,
behavior is byte-identical to today. The true Zen-2 (branch-stack-absent) production path is
called out as **forward-look**.

## Phase 8 — `ring_buffer__consume` instead of a 200 ms `poll` in snapshot end() (`src/branchsnap.c`) *(planned)*

**Goal.** Remove a fixed ~200 ms wall stall on every no-hit/truncated snapshot region. The
exit breakpoint fires synchronously during `run_fn` and the BPF program submits immediately,
so by `snapshot_end` the records are committed; the `ring_buffer__poll(rb, 200)` at
[src/branchsnap.c:225](../../../src/branchsnap.c) only ever burns its full timeout when the
ring is empty — now the common path since the snapshot is default-on.

**Work.** One-line swap to non-blocking `ring_buffer__consume(g_bsnap.rb)` (the
disable-before-drain guard at [src/branchsnap.c:223-224](../../../src/branchsnap.c) already prevents any
later record; `consume` reads the producer position directly, drains all queued records, and
returns at once). `consume` has been in libbpf since 0.4.0 and the only image building this
TU carries libbpf 1.3.0. Optional: fix the two "poll" comments.

**Acceptance.** Cap lane (`make docker-hwtrace-codeimage`): all `ok - branchsnap*` lines
still print (hit path, marker path, reduced-filter follow, reach). Add a no-hit timing
sub-test — `begin("x")` then `end("x")` without invoking `fn`, wrap `end()` in a
`CLOCK_MONOTONIC` delta, assert `< ~50 ms` and `truncated==true` (was ~200 ms). Self-skips
where the substrate/caps are absent.

## Phase 9 — Single cached `/proc/cpuinfo` flag probe (`src/amd_backend.c`, `src/msr_lbr.c`) *(planned)*

**Goal.** `/proc/cpuinfo` `flags` is parsed independently in
[src/msr_lbr.c:58](../../../src/msr_lbr.c) and
[src/amd_backend.c:90](../../../src/amd_backend.c) (and `vendor_id` a third time in
`hwtrace.c`). Factor one cached probe; drift here would silently break the byte-identical-trace
invariant.

**Work.** Add `int asmtest_amd_has_cpu_flag(const char *flag)` (caches the first `flags`
line into a `char[4096]`, substring-searches with the exact `" <flag>"` leading-space
semantics) and a **pure** `int asmtest_amd_flags_have(const char *line, const char *flag)`
(no I/O, exposed for a host-independent test, mirroring the `asmtest_amd_msr_decode_entry`
precedent) in [src/amd_backend.c](../../../src/amd_backend.c); declare both in
[src/amd_backend.h](../../../src/amd_backend.h) with `#else` stubs. Collapse
`amd_lbr_v2_present` (msr_lbr.c) and the `snapshot_available` flag block (amd_backend.c) to
call it. Preserve the `char[4096]` first-chunk window and leading-space match exactly — the
hard constraint is *no availability result changes*. Defer the `hwtrace.c` `vendor_is`
consolidation (different field, file-local static, hot gating path).

**Acceptance.** Host-independent `test_amd_cpu_flag()` drives `asmtest_amd_flags_have` on
synthetic lines: present flag → 1, second flag → 1, absent → 0, left-prefix
(`"xamd_lbr_v2"`) → 0, `NULL`/empty → 0. Equivalence: `asmtest_amd_snapshot_available()` and
`asmtest_amd_msr_available()` return their pre-change values on the dev host. `#else` stubs
keep non-x86_64 links clean.

## Phase 10 — Document the AMD LBR window-reach levers (`docs/guides/tracing/`) *(planned)*

**Goal.** `lbr_period`, `branch_filter`, and the AMD meaning of `snapshot` have rich header
docs ([include/asmtest_hwtrace.h:78-112](../../../include/asmtest_hwtrace.h)) but appear in
neither user guide — real reach-extending levers a user can't discover. Implements the
audit's F47.

**Work.** All substantive prose in
[docs/guides/tracing/hardware-tracing.md](../../../docs/guides/tracing/hardware-tracing.md)
(native-tracing.md delegates tier detail to it): (1) give `snapshot` its AMD
deterministic-boundary meaning in the options paragraph and add a forward pointer;
(2) insert a `### Tuning AMD LBR window reach` subsection before the scoped-tracing-primitives
heading, covering `lbr_period` (Tier-B PMI-reduction, must be `<16`, honest stitch-gap
truncation, the **self-similar-loop undercount** caveat — 231 vs 297 insns at period 4 vs 1)
and `branch_filter` (drops direct-uncond-`jmp`, byte-identical, ~1.86× reach per window, the
loop lever), both framed **fidelity-neutral by construction**. Link the internal plan via a
full github blob URL (internal/ is outside the Sphinx tree). Add one cross-ref clause to
[docs/guides/tracing/native-tracing.md](../../../docs/guides/tracing/native-tracing.md). No
`DESIGN.md` edit (its only 'AMD' hits are the System V AMD64 ABI name — no hardware-trace
section). No `features.md` edit either: its Hardware-trace bullet
([docs/reference/features.md:113-120](../../../docs/reference/features.md)) does name AMD
LBR, but per-field tuning knobs sit below a feature catalogue's altitude — it lists no
option knobs at all (not even `aux_size`) — and Matrix 5 is external-deps only. Do **not**
touch the header.

**Acceptance.** `make docker-docs` completes clean under `-W --keep-going` (the intra-page
anchor `#tuning-amd-lbr-window-reach` must match MyST's auto-slug; build against a clean
doctree per the stale-cache gotcha); `make docker-docs-linkcheck` resolves the blob URL and
the anchor; `grep 'lbr_period\|branch_filter' hardware-tracing.md` is now non-empty.

---

## Deliverables (Phases 1–10)

- **Memory safety:** a size-negotiated `asmtest_hwtrace_options_t` with a clamped init copy
  across the library + all 10 bindings (P1); an overflow-safe `nr` clamp at
  [src/hwtrace.c:822](../../../src/hwtrace.c) matching the guarded sibling sites, optionally
  unified behind a shared `amd_parse_sample()` (P2).
- **Observability:** `ASMTEST_HW_EPERM`, `asmtest_hwtrace_status()`, and
  `asmtest_hwtrace_perf_event_paranoid()` (P3); an `ASMTEST_HWTRACE_DEBUG`/`ASMTEST_AMD_DEBUG`
  logging facility (`src/debug.{c,h}`) across begin/end/probe/truncation sites (P4).
- **Snapshot fidelity:** multi-exit (≤4) deterministic BPF boundary snapshot, default-on,
  with `asmtest_amd_all_exits` + `snapshot_begin_multi` (P5); the corrected cascade rationale
  (P6).
- **New capability:** an IBS-Op survey source with a pure decoder + live self-test gate (P7).
- **Perf/hygiene:** non-blocking snapshot drain (P8); one cached cpuinfo-flag probe (P9).
- **Docs:** the "Tuning AMD LBR window reach" guide subsection (P10).

When the last phase lands, **move this file to `../archive/plans/`** in the same change
(the repo's done-material-never-sits-in-plans/ rule).

## Validation

Grounded in the tier's test harness (all C cases in
[examples/test_hwtrace.c](../../../examples/test_hwtrace.c), run by `make hwtrace-test`; live
cases self-skip via `asmtest_hwtrace_available()`):

- **Host-independent (real execution on the generic/Intel CI host — Capstone-only):** the
  `nr`-overflow parse (P2), the options ABI clamp under ASan (P1), the `status()`/paranoid
  invariants (P3), the debug on/off behavior (P4), `all_exits` enumeration (P5), the IBS
  `ibs_decode_op_raw` + probe (P7a/b), the pure cpuinfo-flag matcher (P9). These follow the
  synthetic-`perf_branch_entry` pattern of `test_amd_reconstruction`/`test_amd_stitch` and get
  genuine coverage where live capture self-skips.
- **Docker cap lanes on the Zen 5 box (`perf_event_paranoid=4`, so a bare run self-skips):**
  live LBR capture, positive `EPERM`, and AMD debug lines via `make docker-hwtrace-amd`
  (`--cap-add=PERFMON --security-opt seccomp=unconfined`); the multi-exit snapshot + the
  no-hit timing sub-test via `make docker-hwtrace-codeimage` (`--cap-add=BPF,PERFMON,SYS_PTRACE`);
  MSR paths via `make docker-hwtrace-msr` (`--privileged`); IBS via a new
  `docker-hwtrace-amd-ibs` lane.
- **Gates:** `scripts/check-bindings-parity.sh` (symbol surface — only P3 needs allow-list
  entries; P1/P4 do not); clang-format 18.1.3 + the `-Werror` native-trace tier (P4); the docs
  build under `-W` (P10).
- **Forward-look:** the true Zen-2 (no branch stack) IBS production path (P7) has no runnable
  lane here — validated by decode + live self-test, capture deferred.

## Risks and open points

- **P1 is a coordinated flag-day ABI change** — prepending `struct_size` reorders the struct,
  so every object/wheel/`.so` built against the old header is binary-incompatible. Safe only
  because the lib + all 10 bindings build in lockstep from one tree; the clamp makes the
  residual cross-version case memory-safe (not correct). Reject-0 semantics require touching
  ~27 C sites + all 10 binding inits — use the `INIT_OPTS` macro and grep every
  `asmtest_hwtrace_init` to avoid a missed site; the manual-layout mirrors (java/dotnet/ruby)
  risk a wrong offset if the `+8` shift is mis-applied.
- **P5 depends on system-wide DR arbitration** — the 4 x86 debug registers are shared with
  gdb hw-watchpoints; require-all-N-else-sampled keeps this honest but a constrained host
  silently degrades to the sampled path (loses the snapshot win, never correctness). Keep
  `bp_len = sizeof(long)` (the working Zen 5 value).
- **P6 must follow P5** — the comment rewording is only true once the multi-exit snapshot is
  default-on; landing it early would mislead a maintainer into re-adding a redundant rung.
- **P7's one hardware-truth uncertainty** is the IBS raw register word/bit layout (varies by
  kernel). Neutralized two ways: the pure 7b test validates the code path with synthetic data;
  the 7d live self-test validates the constants against silicon and self-skips the lane on
  mismatch — a layout drift can never emit garbage endpoints. Guard against silently
  downgrading a capable host's survey from LBR to sparse IBS by gating strictly on the
  branch-stack open failing first.
- **P3's snapshot/MSR/survey `EPERM` paths are not in the backend enum**, so
  `asmtest_hwtrace_status()` cannot cover them today — documented gap; errno out-params on
  `asmtest_amd_snapshot_available`/`asmtest_amd_msr_available` are a follow-up.
- **P4 adds a TU to every hwtrace link list** — a missed list is a build-visible link error,
  not silent.
