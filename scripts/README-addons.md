# Adding a Dear ImGui addon — the supply-chain how-to

This is the operational companion to
[docs/internal/gui/12-addon-supply-chain.md](../docs/internal/gui/12-addon-supply-chain.md)
(which amends decision **D2** with the addon-admission rule). Read the admission
rule there first; this file is the mechanics. Every addon in docs 13–17 is added
the same five-step way, so the per-addon cost is *one thin wrapper + one/two
digest rows + one license row + (if it uses `imgui_internal.h`) one probe line*.

## The five steps

1. **Check the admission rule** (D2, five points: MIT/zlib/BSD/OFL license; a
   pinned tarball/commit + digest; the `imgui_internal.h` compile-gate;
   view-model purity; the honesty filter). If it fails any, it is not
   admissible — see doc 11's *Deliberate skips* before arguing.

2. **Write `scripts/fetch-<name>.sh`** — a thin wrapper over
   [`fetch-addon.sh`](fetch-addon.sh). Two shapes:

   **Header shape** (one or a few files from `raw.githubusercontent.com`, pinned
   at a commit sha — this is the common case; mirrors `fetch-linmath.sh`):

   ```sh
   #!/bin/sh
   set -eu
   sha=021aa0ea...                        # the immutable commit
   base=https://raw.githubusercontent.com/ocornut/imgui_club/$sha
   ADDON_NAME=imgui_club
   ADDON_VERSION=$sha                     # cache-path + digest-version token
   ADDON_FILES="$base/imgui_memory_editor/imgui_memory_editor.h imgui_memory_editor.h imgui_club $sha"
   ADDON_LICENSE_URL=$base/LICENSE
   ADDON_LICENSE_DEST=imgui_club-LICENSE.txt
   export ADDON_NAME ADDON_VERSION ADDON_FILES ADDON_LICENSE_URL ADDON_LICENSE_DEST
   exec "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/fetch-addon.sh"
   ```

   Each `ADDON_FILES` line is a 4-tuple `<url> <dest-relpath> <digest-name>
   <digest-version>`. A two-file addon (e.g. TextSelect + its utfcpp dependency)
   is two lines, and **two digest rows** — one per file, each with its own
   upstream version.

   **Tarball shape** (a whole repo/release archive — for multi-file-same-repo
   addons like ImPlot / imgui-node-editor / ImGuiFileDialog / ICTE; mirrors
   `fetch-imgui.sh`):

   ```sh
   #!/bin/sh
   set -eu
   ADDON_NAME=implot
   ADDON_VERSION=v1.0
   ADDON_TARBALL_URL=https://github.com/epezent/implot/archive/refs/tags/v1.0.tar.gz
   ADDON_TARBALL_KEEP="implot.h implot_internal.h implot.cpp implot_items.cpp LICENSE"
   ADDON_LICENSE_URL=  # (empty: capture from KEEP'd LICENSE by hand, or set a raw URL)
   export ADDON_NAME ADDON_VERSION ADDON_TARBALL_URL ADDON_TARBALL_KEEP
   exec "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/fetch-addon.sh"
   ```

   `ADDON_TARBALL_KEEP` (optional) publishes just those relpaths; omit it to
   publish the whole tree.

3. **Pin the digest(s).** Run the wrapper once. It fails on the missing row and
   prints the `got sha256:…`. Paste a row into
   [`third-party-digests.txt`](third-party-digests.txt):
   `tarball-sha256  <name>  <version>  sha256:<value>` (the `tarball-sha256`
   kind is "sha256 of the fetched artifact" — it is used for lone headers too).
   Re-run; it must now verify. **`refresh-thirdparty-digests.sh` preserves these
   hand-pinned rows** (it does not compute addon hashes; it carries them through
   a full rewrite by `<name> <version>`), so a digest regen never un-pins an
   addon.

4. **Capture the license.** `fetch-addon.sh` copies `ADDON_LICENSE_URL` into
   `licenses/<ADDON_LICENSE_DEST>` on first fetch. Add a row to
   [`../licenses/README.md`](../licenses/README.md), marked **bundled** (compiled
   into the desktop binaries) — the only *test-lane-only, never bundled*
   exception is `imgui_test_engine` (doc 17).

5. **If it includes `imgui_internal.h`, add it to the compile-gate.** Append an
   `#include` (behind its `ASMDESK_HAVE_<ADDON>` guard) to
   [`../desktop/test/addon_compile_probe.cpp`](../desktop/test/addon_compile_probe.cpp)
   and wire the fetch into `mk/desktop.mk`. `make desktop-addon-compile-check`
   then rebuilds it on every imgui repin — a repin that breaks the addon is a
   **landing blocker**, not a runtime surprise. The five known internal-header
   dependents are TextSelect, ImGuiFileDialog, node-editor/imgui_canvas,
   ImGuiNotify, and ImSearch (doc 11's verification appendix).

## Verify

`make addon-fetch-test` exercises `fetch-addon.sh` end to end against the
already-pinned `linmath` header (network required): it must produce a
byte-identical file to `fetch-linmath.sh` and **refuse** an unpinned download.
`make desktop-addon-compile-check` compiles every vendored addon header with the
desktop lane's exact flags at the current imgui pin.
