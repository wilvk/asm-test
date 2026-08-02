# Defect — the live 3D scene freezes its worldlines while the terrain grows

> **RESOLVED 2026-07-30, same day as filing — `55fc624` "re-upload 3D
> worldlines as a live capture grows".** The fix took the doc's own "Cleaner"
> recommendation below: `SceneFrame` gained a `gen` field
> (`shell.cpp:900`, `f.gen = r.event_count();`) and the host now gates on
> `scene_needs_traj_upload(f.key, f.gen, ...)` (`gl_scene_host.cpp:68`) instead
> of `key` alone. Kept for the file:line context and the reusable "identity vs.
> state" analysis, not because anything below is still open — re-verified
> 2026-07-31 while cutting
> [../gui/43-faithful-city-roadmap.md](../../gui/43-faithful-city-roadmap.md), whose
> Phase C previously assumed this was still an open dependency.

Review date: 2026-07-30. Severity: **medium** (live-capture only; visual
staleness, no crash and no data loss). Scope: the GL upload gate of the 3D
spacetime overview under a growing `asmspy --serve` capture. Found during the
[computer-as-city 3D ideation](../../analysis/2026-07-30-computer-as-city-3d.md); filed
separately so it is not lost behind that larger design.

## Summary

On a **live, growing** capture the 3D scene re-uploads the **terrain height
field** every batch but **never re-uploads the trajectory tubes or the
convergence arcs** after the first batch. The landscape grows; the worldlines and
cross-thread arcs freeze at their first-batch geometry. A replayed (static) file
is unaffected.

## Evidence (verified against HEAD `49cfeea`)

The GL host re-uploads on two different gates
([`desktop/src/ui/gl_scene_host.cpp:64-79`](../../../../desktop/src/ui/gl_scene_host.cpp#L64)):

```cpp
// trajectory + arcs: gated on the recording IDENTITY key
if (f.key != up_key_ || !have_upload_) {
    scene_.set_trajectories(*f.traj, f.terr->proj);
    scene_.set_convergences(...);
    up_key_ = f.key;
    up_t_ = f.slice_t + 1; // force the terrain upload below
}
// terrain: gated on the sliced playhead t
if (f.slice_t != up_t_) {
    scene_.set_terrain(*f.slice);
    up_t_ = f.slice_t;
}
```

But `f.key` is the recording's **identity**, not its **state**
([`desktop/src/ui/scene_host.h:37`](../../../../desktop/src/ui/scene_host.h#L37) —
`uint64_t key; // recording identity`), and it is built from the basename alone
([`desktop/src/ui/shell.cpp:896`](../../../../desktop/src/ui/shell.cpp#L896)):

```cpp
f.key = std::hash<std::string>{}(a.id);   // a.id = the recording basename
```

A live capture keeps the **same basename** as it grows
([`shell_sync_live_tab`](../../../../desktop/src/ui/shell.cpp#L192) mirrors the
growing recording as one tab — "the growing recording is not a second code path,
it is the same model"). So `f.key` is invariant across the per-batch re-weave,
`f.key != up_key_` is false after batch 1, and `set_trajectories` /
`set_convergences` are never called again — while `set_terrain` keeps firing
because `f.slice_t` advances with the playhead.

The in-code comment at `gl_scene_host.cpp:63-66` states the assumption outright —
*"the trajectory + arcs depend only on the recording, so a scrub re-uploads the
height field alone."* That is correct for a **static** recording (scrubbing does
not change the worldline) and wrong for a **growing** one (growth adds worldline
vertices, but the identity key does not change).

## Why it is easy to miss

- The 2D live views (observer deck, timeline, scrubber) and the terrain itself
  all update correctly — only the 3D worldline/arcs are stale, and the 3D pane is
  often not the focused pane during a live capture.
- `shell_sync_live_tab` already does the right thing upstream: it re-decodes the
  streams, rebuilds `sv.traj`, and forces a lazy 3D re-weave each batch
  ([`shell.cpp:247-262`](../../../../desktop/src/ui/shell.cpp#L247)) preserving the
  camera. The fresh, grown `TrajectorySet` reaches the GL host in `f.traj` — the
  host just never uploads it.

## Fix

Fold the recording's **growth signal** into the upload key so the trajectory/arc
buffers re-upload when the capture grows. `draw_scene_overview` already holds the
`Recording& r`, and `s.live_built_events` already tracks the monotonic
`event_count()` of the last-built state
([`shell.cpp:272`](../../../../desktop/src/ui/shell.cpp#L272)). Minimal change:

```cpp
// shell.cpp draw_scene_overview — key on identity AND size, not identity alone
f.key = std::hash<std::string>{}(a.id) ^ (r.event_count() * 0x9e3779b97f4a7c15ULL);
```

Cleaner is a dedicated `uint64_t gen` field on `SceneFrame` (bumped from
`r.event_count()`), leaving `key` as pure identity and letting the host gate on
`key != up_key_ || gen != up_gen_`. For the common append case, prefer
`glBufferSubData` over the current free+regen in `set_trajectories` so a large
growing capture does not reallocate every batch.

**Scope note.** This fixes the GL **upload** only. The upstream cost — the
whole-recording re-decode + re-weave each batch (`shell_sync_live_tab` calls
`decode_streams` / `build_terrain` over the entire grown recording) — is a
separate live-scaling concern, not this defect.

## Test

`desktop/test/` cannot exercise real GL, but the gate is testable at the seam:
build two `SceneFrame`s with the same `a.id` and different `r.event_count()` (a
grown `TrajectorySet`) and assert the host's decision to re-upload flips — i.e.
factor the `key != up_key_ || gen != up_gen_` decision into a pure predicate and
unit-test it, mirroring how the model halves are golden-tested. A `desktop-ui-test`
null-backend smoke can additionally drive two frames and assert no assertion/leak.
