# Foundation moves: docking repin, 32-bit indices, real fonts, the 1.92 decision — implementation

> **Sources.** Actioned from [11-imgui-addons.md](11-imgui-addons.md):
> **Foundation moves** F1–F4 and ranked recommendations **#1** (docking branch +
> layout manager) and **#2** (fonts/freetype/icons). Written 2026-07-26 against
> HEAD `27cd43e`. If this doc and doc 11 disagree, this doc wins; if the CODE and
> this doc disagree, re-verify before implementing (doc 11 was verified against
> `4f11065`; the tree has moved a few doc commits since — re-check any file:line
> before you touch it).
>
> Read [\_conventions.md](../implementations/_conventions.md) first; D1–D11 live
> in this directory's [README](README.md). **Hard prerequisite:
> [12-addon-supply-chain.md](12-addon-supply-chain.md) must land first** — F1's
> repin is the first thing the T3 compile-gate protects, and F3's fonts use the
> T2 fetch scaffolding.

## Why this work exists

Every richer addon (docs 15–17) "lands better on top of these" (doc 11,
Foundation moves). The four moves are: put the shell on the docking branch so
views can be real panes instead of ever-deeper tabs (F1); widen draw indices so
dense plots/heatmaps don't overflow the null-backend test tier (F2); ship real
monospace fonts + debugger icons so every text-heavy view stops rendering on the
13 px ProggyClean bitmap (F3); and **decide** — now, while the app has zero
`AddFont`/`PushFont` call sites — whether to bump to imgui 1.92.x before building
elaborate font/DPI infrastructure (F4). F1–F3 are parallel; F4 is a decision that
gates *how* F3 is built.

## What already exists (verified 2026-07-26)

- **The pin & flags** — `mk/desktop.mk:14` `IMGUI_VERSION ?= 1.91.9`,
  `mk/desktop.mk:15` `IMGUI_HOME`, `DESKTOP_CXXFLAGS` at `mk/desktop.mk:27`
  (`-std=c++17 -Wall -Wextra -O2 -g -MMD -MP …`). The imgui sources compiled are
  listed at `mk/desktop.mk:36–39` (core + GLFW/OpenGL3 backends).
- **The fetch + integrity path** — `scripts/fetch-imgui.sh` downloads
  `v${IMGUI_VERSION}.tar.gz`, verifies it against the `imgui` row in
  [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt)
  (`tarball-sha256 imgui 1.91.9 sha256:3872a5f9…`), and **hard-fails without a
  matching row** (`fetch-imgui.sh:43`). The switch to the docking branch is
  therefore exactly *one digest row + one `mk` line* — no script change.
- **The shell today** — one fullscreen window, `IniFilename = nullptr`
  (`desktop/src/main.cpp:67` — *no* `.ini` side-effect, so no layout
  persistence), nested exclusive tab bars (`desktop/src/ui/shell.cpp`). The
  Scrubber, ABI x-ray, and 3D overview integration passes added *more* nested
  tabs (README integration notes) — which strengthens the case for real panes.
- **Zero fonts** — no `AddFont*` call site anywhere; a `PushFont(nullptr)`
  placeholder at `desktop/src/ui/learn_door.cpp:149`; `main.cpp` sets no font.
  This is the "cheapest migration moment it will ever have" F4 hinges on.
- **freetype ships in the tarball** — `misc/freetype/imgui_freetype.{cpp,h}` is
  inside the pinned `build/imgui/imgui-1.91.9` tree already; F3 is a `-D` + one
  TU + `-lfreetype`, not a fetch.
- **The headless null-backend test tier** — `desktop-test` builds every view
  against the ImGui `example_null` pattern (no renderer backend; `mk/desktop.mk`
  `desktop-test`). This tier is F2's real constraint: with no backend it never
  sets `RendererHasVtxOffset`, so 16-bit indices cap a dense draw at 64k
  vertices there.
- **The addon compile-gate** — `desktop-addon-compile-check` +
  `addon_compile_probe.cpp` (doc 12 T3). F1 is its first real customer.

## Tasks

### T1 — Repin ImGui to the docking branch (`v1.91.9b-docking`)  (S, depends on: 12)

> **Implemented 2026-07-27 — green (45/45 suites).** Digest independently
> re-verified (`466fdef9…528a5`, matches doc 11) and pinned; `IMGUI_VERSION`
> flipped. **Two code-won-over-doc corrections** the "exactly two lines" estimate
> missed: (1) `mk/desktop.mk`'s grouped fetch rule ran `sh scripts/fetch-imgui.sh`
> **without passing the version**, and the script defaults to plain `1.91.9` on
> its own — so `IMGUI_HOME` pointed at the docking dir while the fetch wrote the
> vanilla dir; fixed to `IMGUI_VERSION=$(IMGUI_VERSION) sh …`. (2) A **host**
> pin-switch needs `rm -rf build/desktop` first: GitHub tarballs preserve the
> upstream 2025 mtime, so incremental make saw the new docking `imgui.cpp` as
> older than the object built earlier from vanilla and kept a stale `imgui.o`
> (which links against the pre-docking `CollapseButton` layout). Docker is immune
> (empty `build/`). Nothing docks yet — T2 (layout manager) is separate.

**Goal.** Swap vanilla `1.91.9` for the **docking branch hotfix
`v1.91.9b-docking`**, same version, so panes become dockable/tearable. This is
the two-line switch; the layout *manager* is T2.

**Why the `b` hotfix, not plain `1.91.9-docking`** (doc 11): `b` fixes asserts
when *loading* `.ini` table settings (reordered/hidden/sorted columns, upstream
#8496/#7934). The app never loads ini today, so plain `1.91.9` never hits it —
but T2's headline feature (layout persistence) makes that crash newly reachable,
and the app has tables (syscalls etc.). Pin `b` from the start.

**Steps.**
1. Add the digest row to
   [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt):
   `tarball-sha256  imgui  1.91.9b-docking  sha256:…`. Doc 11 records
   `466fdef9b18de15f0bb6e288e3d00ffa3d82200ec458ce5e4f724a161d9528a5` **but
   requires re-verification at pin time** — run `scripts/fetch-imgui.sh` with
   `IMGUI_VERSION=1.91.9b-docking` once (it will fail on the missing row and
   print the `got` digest), confirm the printed digest independently, then paste
   it in. Never paste doc 11's value unverified.
2. Change `mk/desktop.mk:14` `IMGUI_VERSION ?= 1.91.9b-docking`. `fetch-imgui.sh`
   already forms the URL as `v${IMGUI_VERSION}.tar.gz`, so no script edit.
3. Rebuild everything: `make desktop && make desktop-test`. The same six imgui
   TUs (`mk/desktop.mk:36–39`) compile; doc 11 verified the **full headless
   desktop suite passes under both tarballs** (it cites 42/42 at `4f11065`; the
   count is higher now — the invariant is "same count, both green", not the
   literal number). Run `make desktop-addon-compile-check` (doc 12 T3) — this is
   its first real trip.
4. Capture the docking-branch `LICENSE.txt` if its text differs (it should not —
   still MIT; `fetch-imgui.sh` only captures on first fetch, so delete
   `licenses/DearImGui-LICENSE.txt` and re-fetch if you want to confirm it is
   byte-identical, then restore).

**Docs.** CHANGELOG `Changed`: "desktop GUI: Dear ImGui pinned to the docking
branch (v1.91.9b-docking) — same version, adds dockable panes." Update D2's
"pinned release" wording in [README](README.md) if it names the plain tag.

**Done when.** `make desktop desktop-test` is green under `1.91.9b-docking`; the
digest row is the independently re-verified sha; `desktop-addon-compile-check`
passes; the app still opens with its current tab UI (nothing docks until T2 —
the branch switch alone changes no behaviour).

### T2 — In-tree layout manager over `DockBuilder`  (M, depends on: T1; 04, 09, 10)

> **Implemented 2026-07-27 (manager + dockspace host) — green (51 suites).**
> `desktop/src/ui/layout.{h,cpp}` is the manager and the **only** `imgui_internal.h`
> consumer in the shell (DockBuilder), as the doc requires — `layout_build` splits
> a dockspace into left/center/right/bottom regions and docks named panes per
> `LayoutPreset` (Replay/Inspect · Author · Live observer); `layout_exists`
> wraps `DockBuilderGetNode` so the shell needs no internal header.
> `test_layout` drives it headless (DockingEnable on) and pins the split tree.
> `main.cpp` enables `ConfigFlags_DockingEnable` + a real `build/desktop-imgui.ini`
> **for the app only** (the null test backends keep their own contexts, docking
> off, file-free — so `test_shell`/`test_golden` are unchanged). `shell.cpp`
> hosts a `DockSpaceOverViewport` (passthru central node) + a **View menu**
> (Reset layout / per-mode presets), all guarded on `DockingEnable`, and builds
> the shipped default on first run unless a layout was persisted.
> Multi-viewports stay OFF. **Remaining T2 scope**: converting the existing
> nested tabs (Scrubber / ABI x-ray / 3D overview / Diff) into the dockable
> **named windows** the manager targets is the follow-on UX refactor — best
> landed with the interaction tests from doc 17 so the tab→pane move is pinned.

**Goal.** A ~200-line in-tree wrapper (doc 11's estimate) that turns the shell
into a real dockspace: a shipped default layout, "reset layout", and per-mode
presets (Learn/Author/Inspect vs replay vs live observer). It hosts the views
that are currently *nested tabs* — the Scrubber (09), ABI x-ray (09), 3D overview
(10), and side-by-side `diff_view` (04) — as **panes**.

**Steps.**
1. New TU **`desktop/src/ui/layout.{h,cpp}`** — the *only* place
   `imgui_internal.h` (for `DockBuilder*`) is included in the shell. Contain all
   internal-header exposure here; `DockBuilder` "is deliberately internal and
   will churn in the announced docking rewrite" (doc 11) — one TU, one blast
   radius. Add `layout.cpp` to `addon_compile_probe.cpp`'s guard set is **not**
   needed (it's first-party, not a fetched addon) — but it *does* use
   `imgui_internal.h`, so note it in the repin-gate comment (doc 12 T3) as a
   first-party internal-header dependent to rebuild on repin.
2. Enable the dockspace in `desktop/src/main.cpp`: set
   `io.ConfigFlags |= ImGuiConfigFlags_DockingEnable`, and **turn on ini
   persistence** — replace `IniFilename = nullptr` (`main.cpp:67`) with a real
   path (e.g. `build/desktop-imgui.ini`, or an XDG path) **only for the app
   binary**; the null-backend test binary keeps `nullptr` (tests must stay
   deterministic and file-free). Gate docking behind `ConfigDockingWithShift` if
   you want drag-to-dock opt-in (doc 11 suggests it as a low-risk default).
3. The manager API (mirror Hello ImGui's `DockingParams` *as a reference only* —
   do **not** adopt Hello ImGui, see doc 11 skips):

```cpp
// desktop/src/ui/layout.h — named-layout wrapper over DockBuilder. The only
// imgui_internal.h consumer in the shell.
namespace asmdesk {
struct LayoutPreset { const char *name; /* opaque: builds the split tree */ };

// Build (or rebuild) the dockspace for `preset` into `dockspace_id`. Idempotent
// per frame; only rebuilds when the preset changed or the user hit "reset".
void layout_apply(ImGuiID dockspace_id, const LayoutPreset &preset);

// Ship a default .ini string compiled in, so first run has a real layout even
// with no persisted file (Save/LoadIniSettingsToMemory).
void layout_load_default();
void layout_reset();                 // "reset layout" menu action
const std::vector<LayoutPreset> &layout_presets();  // Learn/Author/Inspect/…
}
```

4. Rehome the nested-tab views as panes in `desktop/src/ui/shell.cpp`: the
   Scrubber, ABI x-ray, and 3D overview tabs become `ImGui::Begin`-in-dockspace
   panes. **Keep every fidelity placard and every A/B mechanism** exactly as the
   integration passes wired them (README notes: the ABI x-ray locks active-vs-B,
   the 3D overview reaches GL only via `ui/scene_host.h`). Docking changes
   *where* a view lives, never *what* it decides — the builders are untouched.
5. **Multi-viewports stay OFF** (doc 11): dead on Wayland, "tends to be broken"
   on X11 per the official wiki. Do **not** set
   `ImGuiConfigFlags_ViewportsEnable`. macOS-only opt-in is the ceiling, and not
   in this task.

**Tests.**
- `desktop/test/test_layout.cpp` (new, null backend): `layout_apply` for each
  preset produces a stable dock tree (assert node count / the shipped default
  loads); `layout_reset` restores it; presets are distinct. The null backend has
  no window, so drive `DockBuilder` against an offscreen dockspace id the way the
  existing null-render tests drive views.
- `test_shell` (extend): the Scrubber / ABI x-ray / 3D overview panes still
  build their models and draw their placards when hosted as panes, not tabs
  (re-use the existing assertions; only the container changed).
- Persistence: assert the **app** path saves/loads a layout and the **test**
  binary writes no ini (still `nullptr`).

**Docs.** `desktop/README.md`: the layout/preset/reset UX + "multi-viewports are
off by design". CHANGELOG `Added`: dockable panes, per-mode layout presets,
reset-layout.

**Done when.** the shell is a dockspace; Scrubber/ABI-x-ray/3D-overview/diff are
tearable panes that can be shown at once (the survey's "timeline + scrubber +
disasm cannot be shown together" gap is closed); presets switch the layout;
reset restores the shipped default; the app persists layout while tests stay
file-free; every prior fidelity placard still fires.

### T3 — 32-bit draw indices via `IMGUI_USER_CONFIG`  (S, depends on: T1)

**Goal.** Widen `ImDrawIdx` to 32-bit so dense draws (ImPlot heatmaps, node
canvases, the app's own draw-list views) don't overflow **the null-backend
golden-test tier**, which never sets `RendererHasVtxOffset` and so caps 16-bit
index draws at 64k vertices.

**Steps.**
1. Add `desktop/src/imconfig_user.h` (new) containing:
   `#define ImDrawIdx unsigned int`. Inject it via `DESKTOP_CXXFLAGS` with
   `-DIMGUI_USER_CONFIG=\"imconfig_user.h\"` so it reaches **all** imgui + addon
   TUs (append to the flags at `mk/desktop.mk:27`). Do **not** edit any file
   inside the digest-pinned tarball — the user-config header is the supported
   injection point precisely so the pin stays untouched.
2. Rebuild both binaries and the test tier. Confirm the backends
   (`imgui_impl_opengl3.cpp`) still compile — they honour `ImDrawIdx`
   automatically.
3. **Cite the reason correctly in the header comment**: the null-backend test
   tier, *not* "OpenGL 3.0". The live app is usually fine (macOS requests a 3.2
   core context in `main.cpp`; Linux compat contexts resolve to 4.x) — the
   widening is for the headless tier and future dense draws.

**Tests.** `desktop-test` stays green (no behavioural change; wider indices are a
superset). Optionally add a null-render test that builds a >64k-vertex draw list
and asserts it renders without index wraparound — the concrete thing F2 buys.

**Docs.** CHANGELOG `Changed`: "desktop GUI: 32-bit draw indices
(`IMGUI_USER_CONFIG`) so dense plots/heatmaps are safe under the headless test
tier." A comment in `imconfig_user.h` naming the null-backend reason.

**Done when.** `ImDrawIdx` is 32-bit across imgui + addons via the user-config
header (no tarball edit); both binaries and the test tier build; the reason is
documented as the null-backend tier.

### T4 — Real fonts: freetype + JetBrains Mono + Codicons  (M, depends on: T1; F4 decision)

> **Implemented 2026-07-27 — fonts host-verified (52 suites), freetype gated to
> Docker.** Vendored **JetBrains Mono v2.304** (OFL), **Codicons 0.0.35**
> (CC-BY-4.0), and **IconFontCppHeaders `IconsCodicons.h`** (`210b5a3`, zlib) via
> three `fetch-*.sh` wrappers (digests pinned, licenses captured + README rows).
> **Key realization**: the fonts load via **stb_truetype**, so they need no
> freetype — `desktop/src/ui/fonts.cpp` (`load_fonts`) loads JetBrains Mono as the
> default face and merges the Codicons `[ICON_MIN_CI, ICON_MAX_CI]` range, called
> from `main.cpp` with compiled-in paths (`ASMTEST_*_TTF`). **Graceful**: a
> missing TTF returns false and keeps the built-in bitmap (graceful degrade, tested).
> `test_fonts` verifies the atlas builds + a Codicons glyph is present + the
> degrade — all **on the host** (no freetype). The `PushFont(nullptr)` placeholder
> (`learn_door.cpp`) is gone — the default is now the real face. **Freetype** (the
> F3 rasteriser) is the only Docker-gated part: `DESKTOP_FREETYPE=1` (set by
> `Dockerfile.desktop`, which adds `libfreetype-dev`) links imgui's
> `imgui_freetype` into the **app + viewer only** — the null-backend test tree
> stays stb-only, so `make desktop-test` needs no libfreetype on any host.
> Verified: host `desktop-test` + `desktop`/`desktop-render` build green (stb);
> `make docker-desktop` exercises the freetype path.

**Goal.** Replace the 13 px ProggyClean bitmap with a **freetype-rasterised
monospace TTF** for all hex/register/disasm surfaces, and **merge a debugger
icon font** (Codicons: step-into/over, watch, breakpoint) so the
scrubber/observer/patch-bay get real icon labels. Fixes the `PushFont(nullptr)`
placeholder. **Fonts land before any text-editor or visual-polish addon** —
nothing text-heavy looks right on the bitmap font (doc 11 sequencing).

**Read F4 first (T5).** If the team decides to bump to imgui 1.92.x, build this
task's *font loading* against 1.92's dynamic fonts (which delete the glyph-ranges
+ atlas-rebuild-on-DPI bookkeeping). If staying on 1.91.x, build the
glyph-ranges/atlas path below. The rest of T4 (which fonts, which flags, license
captures) is identical either way.

**Steps.**
1. Enable freetype: add `-DIMGUI_ENABLE_FREETYPE` to `DESKTOP_CXXFLAGS`
   (`mk/desktop.mk:27`), compile the **in-tarball** TU
   `$(IMGUI_HOME)/misc/freetype/imgui_freetype.cpp` (add it to `IMGUI_SRCS`,
   `mk/desktop.mk:36`), and link `-lfreetype`. Add `libfreetype-dev` to
   [Dockerfile.desktop](../../../Dockerfile.desktop) **and** document the macOS
   host requirement (`brew install freetype`; the app builds on Darwin since
   `816999f`, pkg-config finds it). Per doc 11's correction: macOS needs
   freetype too, not only the Docker lane.
2. Vendor **JetBrains Mono** (OFL-1.1) via doc 12's `fetch-addon.sh` — the TTF
   is a pinned asset, one digest row, one `licenses/JetBrainsMono-OFL.txt`
   capture. Embed or load it at startup in `main.cpp` (`AddFontFromFileTTF` or
   embed-as-header). This is the app's **first** `AddFont*` call site.
3. Vendor **IconFontCppHeaders** (zlib, pure `#define` headers, version-agnostic)
   via `fetch-addon.sh` and the **Codicons** font (CC-BY-4.0 font, MIT code).
   Merge Codicons into the main font with `MergeMode` so `ICON_*` macros render
   inline in labels. Fact from doc 11 verification: IconFontCppHeaders' Codicons
   support is current (refresh 2026-06-05); FA7 support (unused here) landed
   2025-08.
4. Replace `PushFont(nullptr)` at `desktop/src/ui/learn_door.cpp:149` with the
   real monospace font handle; give the scrubber/observer/patch-bay icon labels
   where doc 11 calls them out.
5. Capture licenses (doc 12 rule 1): FreeType FTL, OFL-1.1 (JetBrains Mono),
   CC-BY-4.0 (Codicons) — three new `licenses/*` files + three README rows,
   all **bundled**.

**Tests.**
- `desktop/test/test_fonts.cpp` (new, null backend): the atlas builds with
  freetype enabled; the monospace font and merged Codicons range are present;
  an `ICON_*` glyph resolves to a non-tofu codepoint. The null backend builds
  the font atlas without a renderer, so this is testable headless.
- Regression: `desktop-test` stays green; no view asserts on a null font.

**Docs.** `desktop/README.md`: fonts, HiDPI note, the freetype host requirement.
CHANGELOG `Added`: real monospace font + debugger icon set.

**Done when.** the app renders hex/registers/disasm in JetBrains Mono via
freetype; Codicons render inline in scrubber/observer/patch-bay labels;
`PushFont(nullptr)` is gone; freetype is in `Dockerfile.desktop` and the macOS
build note; three license captures landed; the font test passes headless.

### T5 — F4: decide the 1.92.x bump timing (a written decision)  (S, depends on: none)

> **Decision 2026-07-27 — DEFER the 1.92.x bump; stay on `v1.91.9b-docking`.**
> Rationale: (1) every addon brief is already written and pinned for 1.91.x — the
> two 1.92-sensitive ones ship at compatible pins (TextSelect **v1.1.6 + utfcpp**,
> ICTE **master + a compile-verified 2-line `#if IMGUI_VERSION_NUM >= 19200`
> guard**) and FileDialog **v0.6.8** works via its own `< 19201` guard, so nothing
> we want is *blocked* by staying. (2) The docking parity pin (T1) is landed and
> green across the full suite. (3) F3 fonts on 1.91.x (glyph-ranges + atlas
> rebuild) is well-understood and bounded; the dynamic-font savings 1.92 offers do
> not outweigh re-verifying ~10 addon pins right now. **Revisit triggers** (any
> one flips the decision): an addon we decide we want becomes 1.92-only with no
> 1.91-compatible pin; we take on serious HiDPI/dynamic-DPI work; or upstream
> abandons the docking-branch parity tag. On a bump, move `imgui_test_engine`'s
> tag (doc 17) in the same commit and re-run `desktop-addon-compile-check`.
> Consequence for **T4**: build the **1.91.x glyph-ranges/atlas** font path, not
> the 1.92 dynamic-font path.

**Goal.** A recorded decision — *bump to 1.92.x now, or stay on 1.91.9(b) for
now* — made **before** T4 builds elaborate font/DPI infrastructure, because the
app has **zero `AddFont`/`PushFont` call sites today, the cheapest migration
moment it will ever have** (doc 11 F4). This is a decision task: its deliverable
is a short decision record and a gate flag, not code.

**Steps.**
1. Write a decision record (append to this doc as a dated *F4 decision* section,
   or a `docs/internal/gui/decisions/` note if one exists) capturing doc 11's
   inputs:
   - 1.92's dynamic fonts delete the glyph-ranges + atlas-rebuild-on-DPI
     bookkeeping that 1.91.x forces T4 to build.
   - Multiple addon HEADs are **already 1.92-only**: ImGuiTextSelect ≥v1.2.0
     (doc 14 pins v1.1.6 to stay on 1.91.9), goossens ICTE master (doc 17 ships a
     2-line guard), ImGuiFileDialog master (doc 14 pins v0.6.8).
   - The docking-branch parity pin (T1) is sound today, but upstream has moved a
     full minor series; the bump cost only grows.
   - **If a bump is plausible within two quarters, do it before T4's font
     infrastructure**, and bump `imgui_test_engine`'s tag (doc 17) in the same
     commit.
2. State the decision and its consequences for T4 (dynamic-font path vs
   glyph-ranges path) and for docs 14/17 (whether their version guards/pins can
   be dropped).
3. If the decision is **bump now**: this task grows a T1-shaped repin to the
   1.92.x docking tag (new digest row + `mk` line + full `desktop-test` +
   `desktop-addon-compile-check` + re-audit every addon pin doc 11 flagged
   1.92-sensitive). If **not yet**: record the trigger conditions that would
   revisit it, and T4 builds the 1.91.x path.

**Docs.** The decision record is the deliverable. If "bump now", CHANGELOG
`Changed` for the version move.

**Done when.** a dated decision exists with its rationale and its consequences
for T4 and docs 14/17; if the decision is to bump, the repin has landed green;
if not, the revisit trigger is written down so it is not silently forgotten.

## Task order & parallelism

`12` → **T1** → {T2, T3, T4} in parallel, with **T5 (F4) decided before T4
starts** (T5 has no code dependency and can be decided at any time, but T4 must
not begin until it is). T1 is the gate for all three; T2/T3/T4 touch disjoint
surfaces (shell/dockspace, flags/user-config, main.cpp/fonts) and can be
different developers. Critical path: `12` → T1 → T4 (fonts unblock docs 15/17's
text-heavy views).

## Constraints & gates

- **Multi-viewports never become baseline** — dead on Wayland, broken on X11
  (doc 11). Not in T2, not later, except a macOS-only opt-in that is out of
  scope here.
- **`imgui_internal.h` is contained** — only `ui/layout.cpp` (T2) uses it in the
  shell; it is listed in the repin-gate comment (doc 12 T3) so a future imgui
  bump rebuilds it.
- **The test tier stays file-free and deterministic** — ini persistence (T2) is
  app-binary-only; the null backend keeps `IniFilename = nullptr`.
- **The pinned tarball is never edited** — F2's `ImDrawIdx` and F3's freetype
  enable go through `DESKTOP_CXXFLAGS`/user-config/`-D`, never a source patch.
- Every view's fidelity placards and A/B mechanisms survive the tab→pane move
  (T2) unchanged.

## Out of scope

- Any ranked addon (ImPlot, memory editor, node editor, etc. — docs 14–17).
- The macOS multi-viewport opt-in.
- HiDPI *auto-scaling* beyond loading a real font at a chosen size (the dynamic
  DPI story is F4/1.92 territory, decided in T5).
