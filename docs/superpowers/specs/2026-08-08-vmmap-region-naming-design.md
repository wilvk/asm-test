# The 3D plane names its regions — design

> **Source.** The 2026-08-08 session question: *"for the 3D overview regions, why are
> most 'observed data (unknown)'? is it possible to have more descriptive names? can
> these be shown on the 3d scene with more data also?"*
>
> Authored 2026-08-08 against `7b1ae0ce`. Every `file:line` below was read, and the
> six load-bearing ones re-verified by hand after a 9-agent investigation in which all
> four research slices were adversarially refuted and corrected. If a citation
> disagrees with the code, the code wins — re-verify, then fix this doc in the same
> change.
>
> **Prerequisites: none.** One new producer emission, one new schema row, a viewer
> overlay pass, and four scene surfaces. No engine change, no new dependency, no
> envelope major.

## Why this work exists

The plane has exactly two sources of regions, and only one is a map.

**`codeimage` events** become `Kind::Code` regions labelled `"code@0x…"`
([terrain.cpp:77-107](../../../desktop/src/space/terrain.cpp#L77)). The serve host arms
exactly **one**: the main executable's text span, matched against `/proc/<pid>/exe`
([asmspy.c:3739](../../../cli/asmspy.c#L3739)), clamped to `SERVE_CI_MAX_BYTES`.

**Everything else** comes from `observed_data_spans`
([projection.h:92-115](../../../desktop/src/space/projection.h#L92)), which clusters the
addresses the trace was *seen touching* into page-rounded spans. Every one is
`Kind::Unknown`, labelled `"observed data"`.

So for a real process — libc, ld.so, heap, ninety thread stacks, hundreds of mappings —
one span is named and the rest are inferred from "a load happened here". The `Unknown`
is deliberate honesty, stated in the header itself: *the recording states that these
bytes were touched, and NOTHING about what they are (not an allocation extent: the real
object may start before and end after)*.

**The names already exist and never reach the capture.** The producer reads
`/proc/<pid>/maps` already — `scan_modules` ([asmspy_proc.c:355](../../../cli/asmspy_proc.c#L355))
walks it during `asmspy_symtab_load` (though it keeps only file-backed offset-0 rows; see
*Emission points* for why this feature still needs its own walk). `asmspy --info` emits a `procinfo` event carrying
`modules[]`. But `procinfo` is emitted **only** by `--info`
([asmspy.c:8823](../../../cli/asmspy.c#L8823)), into its own one-event recording for the
Details pane. A capture never carries it, and there is no `regions_from_maps` anywhere
in `desktop/src`.

**The display side is already built and starved.** `region_style`
([projection.cpp:633-649](../../../desktop/src/space/projection.cpp#L633)) has five kinds
with distinct hues — Code, Stack, Heap, Data, Mmap — of which only Code is reachable.
`atlas_labels` ([projection.cpp:704-735](../../../desktop/src/space/projection.cpp#L704))
already paints `Region::label` in place on the floor. `kind_by_cell`
([terrain.cpp:108-125](../../../desktop/src/space/terrain.cpp#L108)) already zones the
terrain by `Region::Kind`. Feed them kinds and names and they light up with no new draw
code.

This is an absent input, not a rendering gap.

## Architecture

### Part 1 — the wire: a new `vmmap` event

Not `procinfo`. That kind is a whole-process snapshot (threads, counters, containment,
children) whose `modules[]` is a list of *loaded objects with symbol counts* — it has no
row for `[heap]`, a thread stack, or an anonymous arena, which is most of what a data
trace touches. Emitting all of it per capture would duplicate the Details pane and carry
eight members of noise.

A focused kind, **serve-only**, emitted after the header and before the first engine
event:

```json
{"k":"vmmap","version":0,"maps_readable":true,
 "spans_total":5618,"spans_truncated":true,
 "spans":[
   {"base":"0x55e3c0a01000","len":933888,"perms":"r-xp","name":"firefox",
    "path":"/usr/lib/firefox/firefox"},
   {"base":"0x55e3c2100000","len":67108864,"perms":"rw-p","name":"[heap]"},
   {"base":"0x7ffd0a000000","len":135168,"perms":"rw-p","name":"[stack]"},
   {"base":"0x7f2b30000000","len":536870912,"perms":"rw-p"}]}
```

Four existing rules are borrowed verbatim rather than invented:

- **`base` is a hex string, `len` a number.** The schema's 64-bit-address rule
  (`procinfo` §, "*A JSON number is a double in many readers, which silently rounds a
  64-bit pointer*"). Addresses are locations; lengths are magnitudes.
- **`maps_readable` gates the whole array**, exactly as `procinfo.code.maps_readable`
  does. `false` means **absent measurement**, never measured zero — the state of every
  process the running user does not own.
- **Rank over the full table, then cap.** The `procinfo` modules rule, whose stated
  rationale is that capping first "dropped libc itself while keeping dozens of
  zero-symbol rows". Rank by executable-first, then by size.
- **`spans_total` rides beside `spans_truncated`.** This *fixes* a stated v1 gap rather
  than copying it: `procinfo.modules` carries no total, so its truncation magnitude is
  unrecoverable. Cap, flag, **and** total — all three.

**Cap: 256 spans.** Measured: 97–118 JSON bytes per span, and a browser-class 5618-row
map is ~550 KB. 256 spans is ~30 KB, against golden recordings of 1.3–5.5 KB. The cap is
mandatory, not a nicety.

#### The body must not use `rec_emitf`

`rec_emitf` formats into a **16 KB stack buffer** and **discards `vsnprintf`'s return**
([asmspy.c:195-204](../../../cli/asmspy.c#L195)):

```c
static void rec_emitf(rec_t *r, const char *kind, const char *fmt, ...) {
    char body[16384];
    ...
    vsnprintf(body, sizeof body, fmt, ap);   /* return discarded */
```

An oversized body therefore emits **syntactically invalid NDJSON with no flag**. Use the
`procinfo` emitter's pattern instead — heap buffer, sticky `overflow`, and a loud refusal
rather than a half-token ([asmspy.c:8612](../../../cli/asmspy.c#L8612)) — or
`graph_record`'s per-row headroom check plus `rec_truncated`
([asmspy.c:1307-1340](../../../cli/asmspy.c#L1307)).

#### Emission points

- **Attach:** beside the symtab load, after the recording header is open.

  **A fresh maps walk, not `scan_modules`.** An earlier draft of this spec said the
  symtab load "already walks the maps" and so this would cost "zero extra passes". That
  is wrong, and the plan corrects it. `scan_modules`
  ([asmspy_proc.c:355](../../../cli/asmspy_proc.c#L355)) does walk the same file, and then
  discards exactly the rows this feature exists to name:

  ```c
  if (path[0] != '/') /* skip [heap],[stack],[vdso],anon */
      continue;
  ...
  if (off != 0) /* only the ELF-header (offset 0) mapping fixes the base */
      continue;
  ```

  plus a dedup by path. It resolves module **bases**; this needs the **table**. The walk
  is one `fopen` and a few hundred `fgets` — cheap (655 µs measured for the largest map
  on this host) — so a second pass is the right answer, not a reason to contort
  `scan_modules` into serving two callers with opposite filters.
- **Refresh:** beside `serve_codeimage_refresh` in the region and dataflow sinks. See
  *Updating over time* below.

**Serve-only, deliberately.** Headless `--record` and `tools/asmtrace_record.c` do not
emit it, so the golden corpus stays byte-stable under Determinism rule 5 — the same
carve-out `37 T4` took. Live maps are full of ASLR'd absolute addresses; goldens pin a
synthetic `0x100000` base.

### Part 2 — the viewer: a naming oracle, not a region source

**This is the load-bearing decision.** The map's rows do **not** become regions. After
`observed_data_spans` runs ([shell.cpp:1337-1339](../../../desktop/src/ui/shell.cpp#L1337)),
a pure overlay pass walks the resulting `Kind::Unknown` / `kObservedDataLabel` spans and
overwrites **`label` and `kind` only** from the `vmmap` span containing them. `base` and
`len` are untouched.

```
observed span 0x7f2b41a0c000 + 8K       (today: Unknown, "observed data")
        │  resolve against vmmap
        ▼
0x7f2b41a00000 + 2.0M  r-xp  libc.so.6
        │
        ▼
Region{ kind = Code, label = "libc.so.6 .text",
        base/len   = the span      (unchanged — what we touched),
        extent     = the mapping   (new metadata — what it is),
        perms, path }
```

`space::Region` was built for exactly this. Its own header says a Region comes "from a
codeimage event (code) **or a /proc/maps snapshot (data/stack/heap/mmap)**"
([types.h:17-23](../../../desktop/src/space/types.h#L17)) — the second half has never had
a producer.

Three properties make this safe, all verified:

1. **The floor provably cannot move.** `layout_fingerprint`
   ([projection.cpp:666-685](../../../desktop/src/space/projection.cpp#L666)) mixes
   `order`, `layout`, `domain_off[]` and `rects[]` — **not `label`, not `kind`**.
   Unchanged bases and lengths give an identical digest, so `layout_reflow_note`
   correctly stays silent. The reader's mental map survives because the floor genuinely
   did not move.
2. **Labels need no invalidation.** `atlas_labels` is recomputed every frame from the
   Projection, so new names appear with zero plumbing.
3. **Invalidation is free anyway.** An in-band `vmmap` lands in `by_kind`, bumps
   `event_count()`, defeats the live early-return at
   [shell.cpp:274-277](../../../desktop/src/ui/shell.cpp#L274), and clears `sv.built`.

Kind resolution is a pure function of `(perms, name)`: `[heap]`→Heap, `[stack…]`→Stack,
file-backed + exec→Code, file-backed otherwise→Data, anonymous→Mmap. **A span the map
does not cover stays `Unknown` / `"observed data"`** — never guessed from size or
neighbours.

`"vmmap"` must be added to `kKnownKinds`
([recording.cpp:42-47](../../../desktop/src/doc/recording.cpp#L42), a
`std::array<const char*, 24>` that becomes 25) in the same change. That array's own
comment records this exact miss for `procinfo`, which made every `--info --json`
recording render as "(1 event(s) of unknown kind, kept)".

### Part 3 — the four scene surfaces

**Names and kind colours cost no draw code** — see *Why this work exists*.

**Pick readout.** [hud.cpp:346](../../../desktop/src/scene3d/hud.cpp#L346) already prints
`r->label.empty() ? st.name : r->label`. It gains name, path, perms, true extent, and
touched-of-extent.

**Region roster.** A new HUD block: each placed region with kind, name, extent and
touched fraction, click-to-frame through the existing `req_goto` / `cam.frame` path. It
states the count it is *not* showing ("6 placed, 1494 mapped-untouched"), so the roster
never reads as the whole address space.

**Two new layers, default OFF** — `occupancy` (touched fraction) and `perms` (w+x
marked, read-only dimmed). Both `LayerGrade::Derived`, both `Group::Structure`. This is
how they avoid competing with the 18 layers already on: seven layers already default off
(`confidence`, `opcode`, `data_relief`, `working_set`, `lifetime`, `data_ribbon`,
`sediment`), precisely the re-lift-and-density-risk set, and these join it.

## Updating over time

**Yes — change-gated, never on a clock.**

Measured volatility across 10 live processes over 30 s: **6 byte-identical**, the rest
drifting 0.04–0.6% of rows. A wall-clock refresh would re-emit ~1500 rows to catch a
change that usually is not there.

`codeimage` is the precedent, and it is four lines
([asmspy.c:3826-3829](../../../cli/asmspy.c#L3826)):

```c
static void serve_codeimage_refresh(serve_session_t *s) {
    if (s->img && asmtest_codeimage_refresh(s->img) > 0)
        serve_codeimage_emit(s);
}
```

`serve_vmmap_refresh` inherits the cadence and the tracer-thread placement, but **not**
the change suppression — that lives in the emitter's `memcmp` against `s->ci_last`, so
vmmap needs its own last-snapshot digest. On a static target the refresh costs one read
and no event.

**Version semantics: stream order, forward-valid.** A `vmmap` describes the address space
as read at its stream position and applies to events **after** it, up to the next
`vmmap`. This is the `df_invocation` delimiter rule, chosen **explicitly** — it is the
*opposite* of `codeimage`, where inferring from wire order is forbidden. The schema
appendix must say which was picked, or a reader will assume the other.

**`version` is an ordinal, not a resolution key.** It counts emissions within a session
(0 at attach, +1 per change-gated refresh) so a reader can say "the map changed twice
during this capture" and a log can name which snapshot it is looking at. Resolution is by
stream position alone, per the rule above. A consumer that sorts by `version` to decide
which map applies to a step is using it wrongly — and the plane does not resolve at all,
it flattens to last-name-wins. If a future consumer genuinely needs per-step resolution,
it must define that separately rather than reinterpreting this field.

**No `when`, no new clock.** `when` is `asmtest_codeimage_now`, ticks only on codeimage
versions, is not session-monotonic (readers must key on `(rbase, when)` together), and
its emitted values are sparse — the library records a version on any dirty page while the
wire dedupes by bytes, so `when` values exist that no event carries. Do not reuse it and
do not invent a second one.

**The plane flattens: last name wins.** It already flattens the codeimage version
timeline ("widest len, latest version",
[terrain.cpp:84-101](../../../desktop/src/space/terrain.cpp#L84)) and `Region::version` is
unread by layout. If time-gating is ever wanted, the in-plane precedent is `churn_step`.

## Fidelity rules — what must never be claimed

1. **Never claim "unchanged" from a lossy canonical form.** Measured: a name-coalesced
   digest reported identical while the raw rows moved +2/−6. Any change-gate must digest
   exactly the payload that would have been emitted.
2. **Never coalesce across the executable bit.** Measured: collapsing 5618 rows to 212 by
   `(lo,hi,name)` fuses executable with non-executable memory in **102 of 212** spans and
   hides a real 256 KiB `PROT_NONE → rw-p` mprotect as merge noise.
3. **Never claim completeness when capped.** Cap, flag, and total — all three.
4. **Never claim an extent is an allocation.** The same refusal `observed_data_spans`
   already makes.
5. **Never claim coverage of followed children.** The engines follow child processes with
   their own address spaces and no sink surfaces that. Root pid only; say so.
6. **Never claim the snapshot is atomic.** Measured: 137 `read()` syscalls for a
   5618-line map, and at both refresh sites the target is not quiesced. A torn read is
   possible — state it rather than design it away.
7. **Never emit outside `[header … end]`.** `LiveSession::feed_line` discards a
   pre-header event as malformed. An attach-time map emitted before the header is
   invisible.
8. **Never route it through `session`/`cmd`/`err`.** Those go to `notes_`, bypass
   `event_count`, and the weave gate never fires.
9. **`maps_readable:false` classifies nothing, and says so.** Silently degrading to
   today's behaviour would make "unreadable" indistinguishable from "genuinely
   unmapped".
10. **Touched-of-extent mixes two grades in one sentence, so word it that way.** The
    extent is exact; the touched part is a lower bound. "touched at least 8K of 64M".

## Risks, ranked

1. **Domain blowup / label extinction** if vmmap spans ever become Projection regions. A
   1 GiB anonymous reservation entering the *compacted* domain squashes the actual
   routine window to its guaranteed one cell, pins `order` at 12 (16.7 M `unproject`
   calls per weave), and makes **every** atlas label vanish under the min-side legibility
   skip — defeating the feature's entire purpose. Mitigated only by the overlay design;
   write the constraint into the code comment so nobody later "improves" it.
2. **Body overflow → corrupt NDJSON.** `rec_emitf`'s 16 KB stack buffer truncates
   silently. Must use the heap-buffer-and-refuse pattern.
3. **Stale region indices.** `SceneFocus::region`
   ([focus.h:53-54](../../../desktop/src/scene3d/focus.h#L53)) and `goto_region_sel`
   ([hud.h:262-264](../../../desktop/src/scene3d/hud.h#L262)) are raw indices into
   `Projection::regions` that survive the live reset and are only *bounds*-checked, never
   identity-checked. **This is a live bug today**, independent of this work: after any
   weave that changes the region set they silently retarget a different region. Fix it
   here — re-resolve by `(base, len)`, clear on no-match — because relabeling makes it
   easier to hit.
4. **Torn map read.** See fidelity rule 6.
5. **Golden churn / ASLR nondeterminism** if the kind ever escapes serve-only.
6. **Emission-window ordering.** The attach-time map must land after the header line.
7. **`unknown kind` noise** until `kKnownKinds` is extended — cosmetic, but the exact
   defect review caught for `procinfo`.

## Explicitly cut (YAGNI)

- **Periodic / wall-clock refresh.** Unjustified by measurement. (Note for the record:
  the serve host is *not* timer-free — `asmspy_engine_sample` already emits one event per
  wall-clock window on the tracer thread — so a timer is *possible*. It is merely
  pointless.)
- **Full-fidelity raw rows.** 5618 spans blows every body buffer.
- **`vmmap` as a region source.** Risk 1. The single most destructive option available.
- **Any `when`/version clock for vmmap.** Biggest invention cost, zero consumer.
- **The name-coalesced digest scheme.** Refuted by fidelity rules 1 and 2.
- **Per-followed-tgid maps.** No sink carries the signal.
- **Emission from headless `--record`.** Keeps golden recordings byte-stable.

## Testing

The strongest available assertion is cheap: **layout invariance.** Build the projection
for the same recording with and without a `vmmap` and the atlas rects must be
*identical* — names and kinds change, the floor does not. One property test pins Part 2's
whole promise.

Beyond it:

- A table test over `(perms, name) → Kind` covering every branch, pure, in the null
  harness.
- An absent `vmmap` and `maps_readable:false` each leave existing recordings
  byte-identical.
- The cap, the truncation flag, and `spans_total` — including that ranking happens before
  capping (an executable row must survive a cap that drops larger anonymous ones).
- A body at and past the cap emits **valid** NDJSON or refuses loudly; never a half-token.
- The change-gate: an unchanged map emits no second event; a changed one does.
- The layer-registry exhaustiveness test already catches the two new rows.
- `is_known_kind("vmmap")`.
- Region-identity re-resolution across a weave that changes the region set (risk 3).
- A `cli-smoke` lane asserting a real serve capture carries a `vmmap` with a `[heap]` and
  a `[stack]` row. Per CLAUDE.md this lane must actually test it, not self-skip.

No new dependencies.

## Post-implementation corrections (2026-08-08)

Landed, then reviewed by 10 agents with every finding verified before report. Three
things this spec got wrong, recorded here because the spec was the thing that was
wrong — not the implementation of it:

1. **"rewrites label/kind ONLY, so it is safe" was an incomplete argument.** It is
   sound for LAYOUT and unsound for the ANCHOR. `resolve_anchor` counts
   `Region::Code` spans and refuses at two or more, so naming a data touch inside an
   r-xp mapping strands every routine-relative path in the recording. `kind` is not
   presentational. The fix is `Region::from_vmmap`, and the lesson is that layout
   invariance was the wrong invariant to prove safety with.
2. **Containment must be full, not base-only.** `observed_data_spans` page-rounds and
   gap-merges, so one span can cover two adjacent kernel mappings — libc's `rw-p` data
   followed by its anonymous `.bss` overflow, present on any glibc host. A straddling
   span now stays unknown.
3. **The two visual layers should not have shipped.** Nothing read
   `SceneLayers::occupancy` or `::perms`; they were two checkboxes that did nothing.
   Removed. A real implementation needs a second per-cell channel beside
   `kind_by_cell` plus shader work — **this remains an open gap**, and the honest
   version of "shown on the 3D scene with more data" that did land is the region
   roster and the pick detail, both textual.

## Explicitly unverified

Carried forward honestly rather than presented as settled:

- **Coalescing by `(name, exec-bit)`** — the compromise that would preserve the
  Code/Data distinction while collapsing the `rw-p`/`---p` alternation — has not been
  measured. Name-only measured 5618→212; name + full perms measured 5618→5509. The middle
  is a guess. **Implementation should measure it before choosing.**
- Volatility was measured on idle-ish desktop processes at 5 s sampling, never on a
  target being single-stepped by this tooling, and never on Firefox.
- No non-C++ reader (language bindings, `cli/cli_smoke.sh` substring scans,
  `cli/test_asmtrace.c`'s `field()` helper) was audited for a `vmmap` field-name
  collision.
