#!/bin/sh
# fetch-imguinotify.sh — ImGuiNotify (docs/internal/gui/16-live-feedback-and-
# filtering.md T1): non-modal toasts for live-session events. Header-only (like
# memory_editor), but bundles the FontAwesome6 header + the fa-solid-900.ttf it
# needs for glyphs (fetched together). Uses imgui_internal.h -> compile-probe.
# NOTIFY_RENDER_OUTSIDE_MAIN_WINDOW is forced false at the include site (vanilla,
# no multi-viewports). Files shape; Dev branch pin.
set -eu
sha=d00e45f8d6b1e094bc9288d20eb3d2840f6a7d73
ver=d00e45f
base="https://raw.githubusercontent.com/TyomaVader/ImGuiNotify/$sha"
ADDON_NAME=imguinotify
ADDON_VERSION=$ver
ADDON_FILES="$base/unixExample/backends/ImGuiNotify.hpp ImGuiNotify.hpp imguinotify-hpp $ver
$base/unixExample/fonts/IconsFontAwesome6.h IconsFontAwesome6.h iconsfontawesome6 $ver
$base/unixExample/fonts/fa-solid-900.ttf fa-solid-900.ttf fa-solid-900 $ver"
ADDON_LICENSE_URL="$base/LICENSE"
ADDON_LICENSE_DEST=ImGuiNotify-LICENSE.txt
export ADDON_NAME ADDON_VERSION ADDON_FILES ADDON_LICENSE_URL ADDON_LICENSE_DEST
exec "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/fetch-addon.sh"
