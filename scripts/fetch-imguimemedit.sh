#!/bin/sh
# fetch-imguimemedit.sh — imgui_memory_editor.h (docs/internal/archive/gui/14-quick-wins.md
# T4): the single 836-line header from ocornut's own imgui_club (MIT, PUBLIC API
# only — no imgui_internal.h, so it is not in the repin compile-probe). Thin
# wrapper over fetch-addon.sh (header shape). See scripts/README-addons.md.
set -eu
sha=a436e793fe44a2c8e827bfcbf138fcbe11940476
base="https://raw.githubusercontent.com/ocornut/imgui_club/$sha"
ADDON_NAME=imgui_club
ADDON_VERSION=$sha
ADDON_FILES="$base/imgui_memory_editor/imgui_memory_editor.h imgui_memory_editor.h imgui_club $sha"
ADDON_LICENSE_URL="$base/LICENSE.txt"
ADDON_LICENSE_DEST=imgui_club-LICENSE.txt
export ADDON_NAME ADDON_VERSION ADDON_FILES ADDON_LICENSE_URL ADDON_LICENSE_DEST
exec "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/fetch-addon.sh"
