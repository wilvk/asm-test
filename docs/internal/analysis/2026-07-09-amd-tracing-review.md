# AMD Tracing Review — Improvement Recommendations

> **SUPERSEDED (2026-07-09, same day).** This was the first-pass review; it was
> re-verified and expanded the same day into
> [2026-07-09-amd-tracing-review-f1-f47.md](2026-07-09-amd-tracing-review-f1-f47.md)
> (the F1–F47 adversarially re-checked edition — the authoritative one; published
> as the orphan page `docs/amd_tracing_review.md` until its 2026-07-22 move here). Kept for history. **Note two findings here
> have since gone stale:** §4.4 ("the auto-escalation path doesn't try the MSR-direct path")
> was closed when the MSR-direct rung landed in `asmtest_trace_call_auto`
> ([src/trace_auto.c](../../../src/trace_auto.c), `37118ec`, 2026-07-10), and §3.3's
> wrong-path-spec concern is handled at the source in `asmtest_amd_msr_decode_entry`.
> **Update 2026-07-21:** "two findings" is now an undercount — §1.1 and §2.1 have
> also since closed in-tree (the shared `ASMTEST_AMD_REDUCED_FILTER` definition and
> real prototypes now live in `src/amd_backend.h`).

Review date: 2026-07-09

## Summary

The AMD tracing subsystem is **remarkably thorough**. It ships five distinct AMD capture paths — sampled LBR (Tier-A/B), deterministic BPF boundary snapshot, MSR-direct LBR, statistical whole-window survey, and the hardware-free single-step/block-step fallback — all self-skipping cleanly and producing parity `asmtest_trace_t` offsets. The code comments and plan docs are unusually detailed.

That said, there are concrete improvements available. Organized by category: correctness / robustness gaps, code quality / maintainability, performance, and missing features.

---

## 1. Correctness / Robustness

### 1.1 `ASMTEST_AMD_REDUCED_FILTER` is duplicated and can diverge

The reduced branch-filter macro is defined identically in both
[hwtrace.c](../../../src/hwtrace.c) (line 595) and
[branchsnap.c](../../../src/branchsnap.c) (line 52), with a comment "kept in
sync." This is a textual-duplication hazard — if one changes and the other
doesn't, the sampled and snapshot paths silently use different LBR filters.

**Recommendation:** Move the definition to a shared internal header (e.g.
`src/amd_internal.h` or `include/asmtest_trace.h`) and include it from both TUs.

### 1.2 Alignment / strict-aliasing violations in perf ring parsing

In [hwtrace.c](../../../src/hwtrace.c) (line 828) the code casts a `uint8_t *`
buffer into `uint64_t *` and `struct perf_branch_entry *`:

```c
uint64_t nr = *(uint64_t *)body;
struct perf_branch_entry *e =
    (struct perf_branch_entry *)(body + sizeof(uint64_t));
```

The `buf` is `malloc`'d (8-byte-aligned) but `body` is at an arbitrary offset
within it (header + variable-length record), so the cast can misalign on
architectures (or compilers) that enforce alignment. The same pattern appears in
the survey drain (line 1066) and the BEGIN/END split (line 1200).

**Recommendation:** Use `memcpy` into a local `uint64_t nr; memcpy(&nr, body,
8);` instead of a pointer cast. This is also the pattern the kernel's
`perf_event__parse_*` helpers prefer. The branch entry array can stay as-is if
the record's body is guaranteed 8-byte-aligned by the kernel (it is, via the
record header size rounding), but a brief assert or comment would document the
invariant.

### 1.3 `amd_lbr_v2_present()` in `msr_lbr.c` duplicates `/proc/cpuinfo` parsing

[msr_lbr.c](../../../src/msr_lbr.c) (line 63) and
[amd_backend.c](../../../src/amd_backend.c) (line 86) each parse `/proc/cpuinfo`
for AMD CPU flags independently. Three TUs now read the same file for the same
purpose.

**Recommendation:** Factor a shared `asmtest_amd_has_cpu_flag(const char *flag)`
utility into `amd_backend.c` (or a shared header) and use it from `msr_lbr.c`
and the snapshot availability probe. Reduces `/proc/cpuinfo` open/parse to one
cached call path.

### 1.4 Freeze-absent Tier-A exit check scans the full branch array linearly

The freeze gate in [hwtrace.c](../../../src/hwtrace.c) (line 919) loops over
`best_nr` entries to detect a region-exit branch. On a freeze-absent Zen 4 part
this is the *common path* (not a degrade). The loop is fine for 16 entries but
it's scanning a structure that the richest-window scan at line 842 already
partially classified — the in-region count is known.

No immediate code change needed; this is a readability note. A comment
`/* O(16), always bounded by depth */` would be sufficient.

### 1.5 Missing error check on `ioctl(PERF_EVENT_IOC_ENABLE)` returns

Throughout [hwtrace.c](../../../src/hwtrace.c) and
[branchsnap.c](../../../src/branchsnap.c), `ioctl(g_fd, PERF_EVENT_IOC_ENABLE,
0)` is fire-and-forget. An `ENABLE` failure on an event whose ring was
successfully mapped would silently produce an empty trace later flagged as
`truncated` — correct but confusing.

**Recommendation:** Check the `ioctl` return for ENABLE (at minimum, log or
return an error from `hwtrace_begin_amd`). DISABLE and RESET failures are
harmless.

---

## 2. Code Quality / Maintainability

### 2.1 Forward declarations of `amd_backend.c` symbols in `hwtrace.c`

[hwtrace.c](../../../src/hwtrace.c) (line 90) has a block of forward
declarations for every `asmtest_amd_*` function. This is fragile — a signature
change in `amd_backend.c` that isn't mirrored in `hwtrace.c` compiles but
produces UB at link time (or worse, silently passes on LP64).

**Recommendation:** Create a small `src/amd_backend.h` internal header with the
real prototypes and `#include` it from both `hwtrace.c` and `amd_backend.c`.
Same pattern as `stealth_helper.h` already does.

### 2.2 Magic numbers for error codes

`amd_backend.c` locally `#define`s `ASMTEST_HW_OK 0`, `ASMTEST_HW_ENOSYS
(-5)`, `ASMTEST_HW_EDECODE (-8)` at the top — repeating constants from
`asmtest_hwtrace.h`. The two aren't cross-checked.

**Recommendation:** Include `asmtest_hwtrace.h` (or at least its error-code
block) instead of redefining. `ss_backend.c` has the same issue.

### 2.3 The `hwtrace_end_amd()` function is 230+ lines

`hwtrace_end_amd` (line 731 of [hwtrace.c](../../../src/hwtrace.c)) does
snapshot drain, ring linearization, two-pass record walk, richest-window
selection, Tier-A→Tier-B escalation, decode dispatch, freeze gate, loss
detection, honesty invariant, and cleanup — all in one function. Breaking it into
subfunctions (e.g. `amd_linearize_ring()`, `amd_select_richest()`,
`amd_escalate_tierb()`) would materially improve auditability without changing
behavior.

### 2.4 `bsnap_on_event` does not validate raw-field bounds

[branchsnap.c](../../../src/branchsnap.c) (line 82) reads
`ev->raw + i * BSNAP_ENTRY_SZ` without checking that
`nr * BSNAP_ENTRY_SZ` fits within `sz - offsetof(bsnap_event, raw)`. The BPF
program should produce well-formed events, but a defensive bounds check guards
against kernel/BPF bugs.

---

## 3. Performance

### 3.1 The data-ring linearization copies the full span unconditionally

At [hwtrace.c](../../../src/hwtrace.c) (line 786) and the survey (line 1050),
the code mallocs + byte-copies the entire ring span for unwrapping. For the 256
KB default ring and a long-running routine this is a non-trivial allocation at
`end()` time.

**Recommendation:** If `tail % dsz + span <= dsz` (no wrap), avoid the copy and
walk the ring in-place. The wrap case still needs linearization. This
optimization is worth ~100 µs on the default ring and pays for itself on every
AMD trace.

### 3.2 Tier-B stitch output buffer is under-sized

At [hwtrace.c](../../../src/hwtrace.c) (line 891):

```c
size_t out_cap = n_samples + (size_t)amd_depth;
```

This allocates `n_samples + 16` entries. With `sample_period=1`, each new window
contributes at most 1 new edge (shift `d=1`), so the stitched length is at most
`16 + (n_samples - 1) = n_samples + 15`. The `+16` is technically sufficient but
only by 1, and with `lbr_period > 1` each window can contribute more. Currently
safe but tight.

Consider `out_cap = n_samples * (size_t)amd_depth` as a generous upper bound, or
document why `n_samples + depth` is sufficient for the `period > 1` case.

### 3.3 MSR-direct path doesn't filter wrong-path specs

[msr_lbr.c](../../../src/msr_lbr.c) (line 175) checks `valid || spec` in the
FROM/TO MSR read, which means it includes speculative wrong-path entries in the
branch array it passes to `asmtest_amd_decode`. The decoder's `amd_replay`
filters them IF `ASMTEST_HAVE_PERF_BR_SPEC` is set, but the MSR path doesn't go
through the perf `spec` bitfield — the MSR's spec bit is in `TO[62]`, not a
struct field.

**Recommendation:** When `TO[62]` is set (spec) AND `TO[63]` (valid) is NOT set,
the entry is a speculative-only record. Filter these before passing to
`asmtest_amd_decode`, or at minimum document the spec-filtering invariant.
Currently it's _probably_ inert because `amd_decode` calls `amd_replay`, which
filters `spec == WRONG_PATH` only when the struct field is present — the MSR
path constructs a `perf_branch_entry` with zeroed flags, so the filter is
inert = phantom edges leak through.

---

## 4. Missing Features / Forward-look Items Ready to Land

### 4.1 Default-on deterministic snapshot for single-exit regions (partially done)

`hwtrace_begin_amd` (line 636 of [hwtrace.c](../../../src/hwtrace.c)) already
defaults to the deterministic snapshot when
`asmtest_amd_snapshot_available() && amd_nret == 1`. But it only counts
`ret`-class instructions and misses routines that exit via a **tail-call** (`jmp`
to another function). Adding `asmtest_disas_is_uncond_jump()` as a second
exit-class detection would capture more routines automatically.

This extends the "single-exit" heuristic without changing the
fallback-to-sampled safety net.

### 4.2 AMD LBR `lbr_period` option is undocumented in user-facing docs

The `lbr_period` and `branch_filter` fields in `asmtest_hwtrace_options_t`
([asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h), line 86) have
extensive header-doc comments but no mention in the published Sphinx docs. Users
looking at `native-tracing.md` wouldn't know these levers exist.

### 4.3 No `asmtest_amd_decode` error propagation from Tier-B

At [hwtrace.c](../../../src/hwtrace.c) (line 901),
`asmtest_amd_decode_stitched` returns `ASMTEST_HW_EDECODE` on bad arguments, but
the return value is discarded:

```c
asmtest_amd_decode_stitched(out, st, r->base, r->len,
                            r->trace, gap || lost);
done = 1;
```

If the stitched decode fails (e.g. `r->trace` was somehow NULL at this point —
it's checked earlier, but defensively), the trace is left empty and `done = 1`
prevents the Tier-A fallback.

**Recommendation:** Check the return value and fall back to Tier-A if it fails.

### 4.4 The auto-escalation path (`trace_auto.c`) doesn't try the MSR-direct or BPF-snapshot paths

The `CASCADE[]` in [trace_auto.c](../../../src/trace_auto.c) (line 60) lists
`AMD_LBR` as a single cascade entry. When the sampled AMD path truncates,
`asmtest_trace_call_auto` escalates to block-step/single-step (line 181) but
never tries the BPF snapshot or MSR-direct path, which could complete the trace
without the block-step overhead.

**Recommendation:** After a truncated AMD LBR result, try
`asmtest_amd_snapshot_trace` (if available) or `asmtest_amd_msr_trace` (if
available) before falling to the block-step tier. This is the biggest single
improvement opportunity — it keeps the fast AMD HW path and avoids the ~1000x
block-step slowdown for routines that simply overflowed the sampled window but
fit within one frozen snapshot.

---

## 5. Priority Ranking

| # | Improvement | Impact | Effort | Category |
|---|-------------|--------|--------|----------|
| 1 | Auto-escalation tries snapshot/MSR before block-step (§4.4) | **High** — avoids 1000x slowdown | Medium | Performance / Feature |
| 2 | Shared `ASMTEST_AMD_REDUCED_FILTER` header (§1.1) | **Medium** — prevents silent divergence | Low | Correctness |
| 3 | Internal `amd_backend.h` header (§2.1) | **Medium** — prevents ABI mismatch | Low | Maintainability |
| 4 | MSR path spec-filtering (§3.3) | **Medium** — phantom edges in traces | Low | Correctness |
| 5 | Reuse error codes from `asmtest_hwtrace.h` (§2.2) | **Low** — hygiene | Trivial | Maintainability |
| 6 | Ring linearization skip-copy optimization (§3.1) | **Low** — microseconds at `end()` | Low | Performance |
| 7 | Tier-B decode return-value check (§4.3) | **Low** — defensive | Trivial | Correctness |
| 8 | Split `hwtrace_end_amd` into subfunctions (§2.3) | **Low** — readability | Medium | Maintainability |
| 9 | `ioctl(ENABLE)` error checking (§1.5) | **Low** — observability | Trivial | Robustness |
| 10 | Tail-call exit detection for snapshot heuristic (§4.1) | **Low** — incremental coverage | Low | Feature |
