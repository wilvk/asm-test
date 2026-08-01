# Addon supply chain & the D2 amendment — implementation

> **Sources.** Actioned from [11-imgui-addons.md](11-imgui-addons.md):
> **Governance** ("amend D2 first") and the closing paragraph of **Sequencing**
> ("Every addon lands with: `fetch-<name>.sh`, a sha256 row, a `licenses/`
> capture, and a compile-check gate"). Written 2026-07-26 against HEAD
> `27cd43e`. Doc 11 is a research/planning document; this is its first
> implementation brief. If this doc and doc 11 disagree, this doc wins (11 may
> be stale); if the CODE and this doc disagree, re-verify before implementing —
> **doc 11's own rule: the code wins, re-verify, then fix the doc in the same
> change.**
>
> Read [\_conventions.md](../../implementations/_conventions.md) first (the
> version-pinning dependency rule is the spine of this doc); shared decisions
> D1–D11 live in this directory's [README](../../gui/README.md).
>
> **Implemented 2026-07-27 — doc complete (3/3).** T1 (D2 amendment) landed
> earlier via the README. T2: `scripts/fetch-addon.sh` (files + tarball modes,
> reusing `lib-thirdparty.sh`), `scripts/README-addons.md` (the how-to), and
> `tests/fetch-addon-test.sh` (+ `make addon-fetch-test`) — plus a **carry-through
> fix to `scripts/refresh-thirdparty-digests.sh`** so a digest regen no longer
> silently un-pins the hand-added rows (it was already dropping imgui/json/linmath/
> pin-3.20/libdft64/maven/libipt/binfmt — a latent B5 hazard this surfaced). T3:
> `desktop/test/addon_compile_probe.cpp` + `make desktop-addon-compile-check`
> (compiles `imgui_internal.h` with the exact desktop flags at the current pin;
> each internal-header addon appends `-DASMDESK_HAVE_… -I…` to `ADDON_PROBE_FLAGS`).
> Both targets pass; both are in `make help`.

## Why this work exists — read this before any other addon doc

Every addon the plan recommends (docs 13–17) is blocked on two things that do
not exist yet:

1. **Decision D2 forbids it.** D2 ([README](../../gui/README.md) and
   [03-desktop-shell.md](03-desktop-shell.md)) pins the desktop app to "Dear
   ImGui + nlohmann/json, nothing else". Adding ImPlot, a memory editor, a file
   dialog, fonts, etc. violates D2 as written. Doc 11's first instruction is
   *amend D2* — not with ad-hoc exceptions per addon, but with one **addon
   admission rule** that any future addon is checked against.
2. **There is no reusable way to pull an addon.** The three existing bundled
   third-party fetches — [fetch-imgui.sh](../../../../scripts/fetch-imgui.sh)
   (tarball), [fetch-json.sh](../../../../scripts/fetch-json.sh) and
   [fetch-linmath.sh](../../../../scripts/fetch-linmath.sh) (single header) — each
   hard-code one dependency. The plan expects ~10 more, most of them one or two
   headers pinned at a commit sha. Copy-pasting a 90-line script per addon is
   the wrong shape.

This doc does exactly those two things and nothing else: it **amends D2** and
**builds the shared scaffolding** every subsequent addon doc reuses. It ships no
addon. It is the critical-path prerequisite (Track G in doc 11's sequencing —
"blocks all"); land it first.

## What already exists (verified 2026-07-26)

- **The dependency rule** is repo policy, not new:
  [\_conventions.md](../../implementations/_conventions.md) and
  [CLAUDE.md](../../../../CLAUDE.md) already require *pin the version, add it where
  the work runs*. This doc applies that rule to ImGui addons — it does not
  invent a new policy.
- **The integrity manifest** —
  [scripts/third-party-digests.txt](../../../../scripts/third-party-digests.txt) —
  and its format: `<kind>  <name>  <version>  <algo>:<value>`, with kinds
  `tarball-sha256` and `git-commit`. Every fetch script verifies its download
  against a row here via `tp_digest` and **fails on a missing or mismatched
  row** (`scripts/lib-thirdparty.sh`; see `fetch-imgui.sh:43`). This is the
  mechanism the plan's rule 2 ("pinned tarball + SHA-256 row") is already built
  on — reuse it.
- **The shared fetch helpers** — `scripts/lib-thirdparty.sh` exposes
  `tp_digest <kind> <name> <version>`, `tp_sha256 <file>`, and
  `tp_fetch_lock`. Every fetch script sources it. **Read it once**; you will
  call `tp_digest`/`tp_sha256` verbatim.
- **The two fetch shapes to mirror**:
  [fetch-imgui.sh](../../../../scripts/fetch-imgui.sh) is the **tarball** shape
  (download `.tar.gz`, verify, extract, publish with one `mv`, capture license);
  [fetch-linmath.sh](../../../../scripts/fetch-linmath.sh) is the **single-header
  at a commit sha** shape (download one raw file, verify, drop it in place —
  no tar step). Most addons here are the linmath shape.
- **The license index** — [licenses/](../../../../licenses/) holds verbatim license
  texts and [licenses/README.md](../../../../licenses/README.md) is a table keyed
  by file / component / version / SPDX with a **bundled vs test-lane-only**
  distinction already drawn (Pin/SDE/libdft64 are marked "never bundled"). New
  MIT/zlib/BSD/OFL addons are *bundled* rows; `imgui_test_engine` (doc 17) is
  the *test-lane-only* exception.
- **The imgui pin the compile-gate must protect** — `mk/desktop.mk:14`
  `IMGUI_VERSION ?= 1.91.9`, built with `DESKTOP_CXXFLAGS`
  (`mk/desktop.mk:27`: `-std=c++17 -Wall -Wextra -O2 -g -MMD -MP …`). Five of
  the recommended addons `#include "imgui_internal.h"` and are only known-good
  at *this* pin.

## Tasks

### T1 — Amend D2 with the addon-admission rule  (S, depends on: none)

> **Landing 2026-07-26 (concurrent).** A parallel change has amended D2 in this
> directory's [README.md](../../gui/README.md) with exactly the five-point admission rule
> below (see the "Amended 2026-07-26" block under D2, and the doc-11 row's
> "planning · G done"). Before implementing, **verify it is on `main`**; if
> present, T1 is done — reconcile the D2 wording in
> [03-desktop-shell.md](03-desktop-shell.md) if that half was missed, then move
> to T2/T3 (the scaffolding, which had not landed at authoring time).

**Goal.** Replace D2's "no third-party dep beyond pinned ImGui + nlohmann/json"
blanket ban with an **admission rule**: a checklist any candidate addon must
pass. After this task, adding an addon that satisfies the rule is in-policy;
adding one that does not is still forbidden. This unblocks docs 13–17.

**Steps.**
1. Edit **D2 in this directory's [README.md](../../gui/README.md)** (the "Binding shared
   decisions" list). Keep the app-backend sentence (GLFW + OpenGL3, headless
   null pattern) unchanged; extend the dependency clause with the five-point
   admission rule below. State that the rule is specified in full in *this*
   document so the README stays a one-line pointer, not a spec.
2. Edit the **matching D2 statement in
   [03-desktop-shell.md](03-desktop-shell.md)** so the two do not drift (grep
   `03-desktop-shell.md` for "no third-party" / "nlohmann" and reconcile the
   wording; the two must say the same thing).
3. Write the admission rule as a numbered list in this doc's *Reference: the
   addon-admission rule* section below (it already is — T1 is the act of making
   D2 point at it and reconciling the two READMEs, not re-authoring the rule).
4. Add a one-line `## [Unreleased] Changed` CHANGELOG entry only if D2 is
   user-visible — it is **not** (internal-docs decision), so **no CHANGELOG
   entry**; note that in the commit body instead.

**Docs.** The D2 edit *is* the doc change. No code.

**Done when.** D2 in `gui/README.md` and in `03-desktop-shell.md` both point at
the admission rule; a reader who knows only D2 can tell whether a proposed addon
is admissible; `grep -rn "no third-party dep" docs/internal/gui` finds no
surviving blanket ban.

### T2 — Reusable addon fetch/pin/license scaffolding  (M, depends on: T1)

**Goal.** One documented, tested way to vendor an ImGui addon, so docs 13–17
each add *a pinned row + a thin script*, never a bespoke 90-line fetcher. Two
addon shapes are covered: **single-header/few-headers at a commit sha** (the
common case) and **release-tarball at a tag** (ImPlot, FileDialog, TextSelect,
the docking branch).

**Steps.**
1. Add a generic **`scripts/fetch-addon.sh`** that fetches N pinned files for
   one addon into `build/addons/<name>-<ver>/`, verifies **each** file against
   its digest row, and prints the install dir on stdout — the same contract
   `fetch-imgui.sh`/`fetch-linmath.sh` print (`IMGUI_HOME` on stdout for a
   Makefile to capture). Drive it by env like the siblings do:
   `ADDON_NAME`, `ADDON_VERSION` (the pin token used in the cache path **and**
   the digest row), and either `ADDON_FILES` (space-separated `url|relpath`
   pairs, for the header shape) or `ADDON_TARBALL_URL` (for the tarball shape).
   Reuse `lib-thirdparty.sh` verbatim — do not re-implement hashing:

```sh
# scripts/fetch-addon.sh (sketch — mirror fetch-linmath.sh line for line for
# the header shape, fetch-imgui.sh for the tarball shape). Both already do:
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib-thirdparty.sh"
want=$(tp_digest tarball-sha256 "$ADDON_NAME" "$ADDON_VERSION") || {
    echo "fetch-addon: no pinned digest for $ADDON_NAME $ADDON_VERSION" >&2
    exit 1   # refuse an unpinned download — same discipline as fetch-imgui.sh:43
}
got="sha256:$(tp_sha256 "$tmp")"
[ "$got" = "$want" ] || { echo "integrity FAILED: want $want got $got" >&2; exit 1; }
```

   > **Decision to make while implementing (record it in the script header):**
   > a header addon with *two* files (e.g. TextSelect + utfcpp) needs *two*
   > digest rows. Either give each file its own `<name>` row
   > (`textselect`, `utfcpp`) — simplest, matches `linmath` — or hash the set.
   > Prefer **one row per file**; it is what the manifest format already
   > expresses and what `tp_digest` looks up.

2. Add a **`fetch-<name>.sh` thin-wrapper convention**: each addon still gets
   its own named script (so `make` targets and READMEs cite a stable path), but
   it is ~15 lines that set the env vars and `exec scripts/fetch-addon.sh`.
   Document this in the script's header and in
   [licenses/README.md](../../../../licenses/README.md)'s neighbouring prose is
   **not** the right home — put the how-to in a short
   `scripts/README-addons.md` (new) so a doc-13 author copies one file.
3. **License capture is mandatory and automatic.** `fetch-addon.sh` copies the
   addon's `LICENSE*` into `licenses/<Name>-LICENSE.txt` on first fetch (as
   `fetch-imgui.sh` does for `DearImGui-LICENSE.txt`); for a single-header addon
   with no license file (linmath's case), the wrapper commits the text by hand
   and the fetch script asserts its presence. Add the row to
   [licenses/README.md](../../../../licenses/README.md)'s table — **bundled** for
   everything except `imgui_test_engine`.
4. Wire the **digest-refresh helper**: extend
   [scripts/refresh-thirdparty-digests.sh](../../../../scripts/refresh-thirdparty-digests.sh)
   so `make`-ing an addon pin recomputes its row the same way the engines' do
   (verify the addon names are picked up; add them if the script enumerates a
   fixed list).

**Tests.**
- A shell test under the repo's existing script-test convention (grep for how
  `fetch-*.sh` are exercised — e.g. a `make` dry-run or a
  `tests/` shell harness) that runs `fetch-addon.sh` for a **tiny real pinned
  header** (use `linmath` itself as the fixture: point `fetch-addon.sh` at the
  linmath commit and assert it produces a byte-identical file to
  `fetch-linmath.sh` and **fails loudly** when the digest row is deleted). This
  proves the refuse-unpinned path (the plan's rule 2) without waiting for a real
  addon.
- Assert the missing-digest path exits non-zero with the manifest name in the
  message (mirror `fetch-imgui.sh:43`'s behaviour).

**Docs.** `scripts/README-addons.md` (new, the how-to); a `licenses/README.md`
table note that addon rows follow this scheme.

**Done when.** `fetch-addon.sh` fetches + verifies a pinned header and a pinned
tarball; deleting the digest row makes it fail with a non-zero exit and a named
error; the license-capture path drops a `licenses/*-LICENSE.txt`; docs 13–17 can
each add an addon with one thin wrapper + one (or two) digest rows + one license
row, and nothing else.

### T3 — Compile-check gate wired to the imgui repin path  (M, depends on: T2)

**Goal.** The plan's rule 3, made real: **any future ImGui repin rebuilds every
addon that reaches into `imgui_internal.h` before it can land.** Five
recommended addons are internal-header dependents — TextSelect, ImGuiFileDialog,
imgui-node-editor / imgui_canvas, ImGuiNotify, ImSearch — and are only known
good at the frozen `1.91.9` pin (doc 11's verification appendix). Without a
gate, a silent imgui bump ships an addon that no longer compiles.

**Steps.**
1. Add a **`desktop-addon-compile-check`** target to
   [mk/desktop.mk](../../../../mk/desktop.mk) that compiles a tiny
   `desktop/test/addon_compile_probe.cpp` (new) which `#include`s every
   currently-vendored addon header (each behind the same
   `#ifdef ASMDESK_HAVE_<ADDON>` guard the adopting doc introduces, so the probe
   stays green when an addon is not yet vendored) with the **exact**
   `DESKTOP_CXXFLAGS` — no separate flag set, or the check is a fiction. The
   probe needs no `main` and links nothing; a successful compile is the pass.
2. **Make the imgui-fetch target depend on / notify the gate.** The clean way:
   the addon fetch outputs (`build/addons/*`) are already keyed on
   `ADDON_VERSION`, but the *imgui* version is what invalidates them. Add a
   short comment + an order-only prerequisite so that a change to
   `IMGUI_VERSION` (`mk/desktop.mk:14`) forces `desktop-addon-compile-check` to
   re-run in `make desktop-test`. Simplest correct implementation: include the
   probe object in the `desktop-test` object set so it is rebuilt whenever
   `IMGUI_HOME` changes (the grouped imgui rule at `mk/desktop.mk:40` already
   re-fetches on a digest/version change; hang the probe off the same
   prerequisite chain the other `desktop/test/%.o` objects use —
   `mk/desktop.mk:98`).
3. Add a `make help` echo line for the new target (D3 requires user-visible
   targets to be discoverable).
4. Document the contract in `scripts/README-addons.md`: *"Every addon that
   includes `imgui_internal.h` MUST be added to `addon_compile_probe.cpp`. A
   repin that breaks it is a landing blocker, not a runtime surprise."*

**Tests.** `make desktop-addon-compile-check` passes at the current pin with the
probe present (even empty — zero addons vendored yet, all `#ifdef`s off, so it
compiles trivially). The real assertion is procedural and lands with each addon:
doc 13's F1 (the docking repin) is the first event that must trip and satisfy
this gate.

**Docs.** CHANGELOG: none (internal). `scripts/README-addons.md` gets the
contract paragraph.

**Done when.** `make desktop-addon-compile-check` exists, is in `make help`, and
compiles the probe with the real `DESKTOP_CXXFLAGS`; a comment at
`mk/desktop.mk:14` states that bumping `IMGUI_VERSION` re-runs the gate; the
contract is documented so doc-13+ authors add their internal-header addon to the
probe as a matter of course.

## Reference: the addon-admission rule (the amended D2)

Any Dear ImGui addon may be vendored **iff** all five hold. This is the text D2
points at.

1. **License**: MIT / zlib / BSD-class (or OFL/CC-BY for fonts). Captured
   verbatim under [licenses/](../../../../licenses/) with a
   [licenses/README.md](../../../../licenses/README.md) row. **One deliberate
   exception**: `imgui_test_engine` (Test Engine License v1.04) is admissible
   **test-builds-only, fetch-at-build, never vendored** — its fourth free-use
   bullet (public OSI-licensed derivative) covers this repo (doc 17).
2. **Pinned**: a tarball-tag or commit-sha `<name>` row in
   [scripts/third-party-digests.txt](../../../../scripts/third-party-digests.txt)
   and a `fetch-<name>.sh` thin wrapper over `fetch-addon.sh` (T2). Prefer
   release tags; use commit-sha tarballs where the addon publishes none
   (`imgui_club`, `imgui-node-editor` master, ImGuiNotify Dev).
3. **Compile-gated to the imgui pin**: if the addon includes
   `imgui_internal.h`, it is added to `addon_compile_probe.cpp` (T3) so any
   future repin rebuilds it before landing.
4. **View-model purity preserved (D4/D-fidelity)**: addons are *draw-half
   chassis only*. The pure view-models and the golden-text test surfaces stay
   the source of truth — an addon never becomes the place a rule is decided or
   tested.
5. **Fidelity ethos as a selection filter**: nothing that renders statistical
   data as stacks, imposes force-directed layout (banned in
   [04-replay-views.md](04-replay-views.md) and
   [08-observer-views.md](08-observer-views.md)), or hides refusals. Doc 11's
   *Deliberate skips* section records the addons already rejected on these
   grounds — **do not re-litigate them.**

## Task order & parallelism

T1 → T2 → T3, strictly. T1 (the D2 amendment) is the true unblocker and can land
alone in minutes; T2 and T3 are the scaffolding that makes docs 13–17 cheap.
This whole doc is **Track G — it blocks every other addon doc**; nothing in
docs 13–17 should start before T1 lands, and the first addon (doc 13 F1) should
land T2+T3's machinery in anger.

## Constraints & gates

- **No addon ships in this doc.** If you find yourself editing a view, you are
  in the wrong document (that is docs 13–17).
- The imgui pin stays `1.91.9` here — doc 13 F1 moves it to the docking branch
  and is the *first* consumer of the T3 gate, not this doc.
- `desktop-render` (the engine-free render-only viewer, D4) must stay
  permissively distributable: every admitted addon is MIT/zlib/BSD/OFL, so this
  holds by rule 1 — but re-check it when adding the license row.

## Out of scope

- Any specific addon adoption (docs 13–17 own those).
- Changing the engines' pinning scheme (Unicorn/Keystone/Capstone/DynamoRIO are
  untouched).
- The 1.92.x imgui bump decision — that is doc 13 F4, a decision, not part of
  the admission machinery.
