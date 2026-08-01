#!/bin/sh
# fetch-imzoomslider.sh — the ImZoomSlider pan/zoom control
# (docs/internal/archive/gui/14-quick-wins.md T5): one self-contained 245-line header
# from the ImGuizmo repo (MIT), vendored ALONE — no gizmo code is built. Thin
# wrapper over fetch-addon.sh (header shape). See scripts/README-addons.md.
#
# Include recipe at the use site is mandatory (doc 11 correction — it is NOT
# public-API-only): #define IMGUI_DEFINE_MATH_OPERATORS, then imgui.h, then
# imgui_internal.h, then ImZoomSlider.h — and PushID each instance (its label is
# a fixed internal "ImZoomSlider"). It is in the compile-probe (12 T3).
set -eu
sha=dc25afb98bc3ebe00dfc9a23ba7235fead2ccb1d
base="https://raw.githubusercontent.com/CedricGuillemet/ImGuizmo/$sha"
ADDON_NAME=imzoomslider
ADDON_VERSION=$sha
ADDON_FILES="$base/src/ImZoomSlider.h ImZoomSlider.h imzoomslider $sha"
ADDON_LICENSE_URL="$base/LICENSE"
ADDON_LICENSE_DEST=ImGuizmo-LICENSE.txt
export ADDON_NAME ADDON_VERSION ADDON_FILES ADDON_LICENSE_URL ADDON_LICENSE_DEST
exec "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/fetch-addon.sh"
