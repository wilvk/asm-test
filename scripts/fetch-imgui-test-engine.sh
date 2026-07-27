#!/bin/sh
# fetch-imgui-test-engine.sh — Dear ImGui Test Engine (docs/internal/gui/
# 17-interaction-testing-and-editor.md T1).
#
# TEST-LANE ONLY, fetched-at-build, NEVER bundled or linked into a shipped
# binary — exactly the posture used for Intel Pin and SDE. This is the ONE
# admitted non-MIT dependency in the tree: its licence is the Dear ImGui Test
# Engine License v1.04, and doc 12's admission rule permits it only because it
# never ships. It compiles into the `desktop_ui_test` binary alone; `desktop`
# and `desktop-render` never see it, so the shipped artefacts stay 100% MIT.
#
# Tag v1.91.9 is tag-matched to our Dear ImGui pin (1.91.9b-docking): the engine
# tag MUST move in the same commit as any imgui repin (doc 13 F1/F4).
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# ADDON_VERSION is the digest-row key AND the build/addons/<name>-<ver> dir; the
# archive URL carries the leading `v` that GitHub tags use.
ADDON_NAME=imgui-test-engine
ADDON_VERSION=${ITE_VERSION:-1.91.9}
ADDON_TARBALL_URL="https://codeload.github.com/ocornut/imgui_test_engine/tar.gz/refs/tags/v${ADDON_VERSION}"
# The whole tree is published (no ADDON_TARBALL_KEEP): we need imgui_test_engine/
# {the 8 .cpp, the headers, thirdparty/Str, thirdparty/stb}.
ADDON_LICENSE_URL="https://raw.githubusercontent.com/ocornut/imgui_test_engine/v${ADDON_VERSION}/imgui_test_engine/LICENSE.txt"
ADDON_LICENSE_DEST="DearImGuiTestEngine-LICENSE.txt"
export ADDON_NAME ADDON_VERSION ADDON_TARBALL_URL ADDON_LICENSE_URL ADDON_LICENSE_DEST

exec sh "$here/fetch-addon.sh"
