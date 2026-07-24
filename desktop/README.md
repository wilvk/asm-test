# `desktop/` — the asmtest desktop GUI

A [Dear ImGui](https://github.com/ocornut/imgui) shell over the `.asmtrace`
document model: a viewer (and, in the full build, an author/inspect host) for the
recordings every headless asmspy mode and the Author-mode recorder produce. This
directory is the app skeleton — the two binaries, the build integration, the
document model, and the reuse seam onto the asmspy view-model — implemented from
[docs/internal/gui/03-desktop-shell.md](../docs/internal/gui/03-desktop-shell.md).
The real views (canvas, timelines, slice explorer, the Loom, the live observer
surfaces) land in the sibling docs 04–09.

## Two binaries, one license split

| Binary | What it is | License |
|---|---|---|
| `build/asmtest-desktop` | the **full app** — links the Author-tier engines (Unicorn/Keystone/Capstone) so it can drive the emulator/assemble/valtrace tiers | **GPL-2.0 as a whole**: it links the bundled GPL-2.0 engines, so the combined work is GPL-2.0 |
| `build/asmtest-viewer` | the **render-only viewer** — opens and replays `.asmtrace` recordings with **zero engine dependencies** (built with `-DASMTEST_DESKTOP_RENDER_ONLY=1`) | **permissive**: it links no engine object or lib (`ldd` shows no libunicorn/keystone/capstone), so it stays distributable without the engine copyleft |

This split (plan §D7 / D4) is enforced by the build, not by convention: the
viewer's link line carries no engine object, and Pin/SDE/libdft64 are never
bundled into any desktop artifact.

## Building

Use the Docker lane where possible (no host deps needed):

```
make docker-desktop     # build both binaries + run the headless tests in a container
```

On a host with the toolchain installed:

```
make desktop            # build build/asmtest-desktop (needs GLFW/GL + the engines)
make desktop-render     # build build/asmtest-viewer  (needs GLFW/GL only)
make desktop-test       # headless null-backend tests (only a C++17 compiler)
```

When a dependency is missing, `desktop` / `desktop-render` print the apt line and
the `make docker-desktop` pointer and fail — never a raw compiler error.
`desktop-test` needs no display, no GL and no engines, so it runs anywhere.

Formatting: `make desktop-fmt` reformats `desktop/**` with the repo
[`.clang-format`](../.clang-format); `make desktop-fmt-check` reports drift and is
informational (desktop/ stays out of the CI-gated format set, D8).

## Pinned dependencies

Both are MIT, both are compiled **into** the binaries (bundled), and both are
fetched pinned and digest-verified the way the native engines are — the digests
live in [`scripts/third-party-digests.txt`](../scripts/third-party-digests.txt)
and the license texts under [`licenses/`](../licenses/):

| Dependency | Version | Fetch script | License file |
|---|---|---|---|
| Dear ImGui | 1.91.9 | [`scripts/fetch-imgui.sh`](../scripts/fetch-imgui.sh) | `licenses/DearImGui-LICENSE.txt` |
| nlohmann/json | 3.11.3 | [`scripts/fetch-json.sh`](../scripts/fetch-json.sh) | `licenses/nlohmann-json-LICENSE.MIT` |

The app backends are GLFW + OpenGL3 (`libglfw3-dev` / `libgl1-mesa-dev`, added by
[`Dockerfile.desktop`](../Dockerfile.desktop)); the headless tests use ImGui's
`example_null` pattern and link neither. Nothing under `build/` is committed
(D1) — the fetch scripts repopulate it, and `make docker-desktop` refetches
inside the image.

## Layout

```
desktop/
  src/
    main.cpp            shared entry point for both binaries (compile-time
                        ASMTEST_DESKTOP_RENDER_ONLY guard)
    vm_compat.cpp       compiles the asmspy view-model headers as C++17 so the
                        desktop reuses the TUI's tested inline logic (plan D5)
    doc/
      recording.{h,cpp} .asmtrace NDJSON -> Recording (events by kind + mandatory
                        provenance + the honesty facts a reader must surface)
      workspace.{h,cpp} the SET of open recordings (plan D3)
    ui/
      shell.{h,cpp}     the home screen (three doors), open dialog, and tab strip
                        — backend-free ImGui draws the null backend can drive
  test/
    test_null_render.cpp   ImGui builds + renders headlessly (example_null)
    test_recording.cpp     the loader's reject rules + honesty accounting (D7)
    test_shell.cpp         shell banner behaviour + a 3-frame null render smoke
    test_golden.cpp        opens every committed golden recording (schema gate)
    fixtures/              hand-authored .asmtrace, one per loader rule
```

The `.asmtrace` grammar, kind registry and golden corpus are **not** owned here —
they belong to
[docs/internal/gui/asmtrace-schema.md](../docs/internal/gui/asmtrace-schema.md)
and [`tests/golden-asmtrace/`](../tests/golden-asmtrace/). See the
[GUI implementation docs](../docs/internal/gui/README.md) for the full plan.
