#!/bin/sh
# fetch-imguifiledialog.sh — ImGuiFileDialog v0.6.8 (docs/internal/gui/
# 14-quick-wins.md T7): pure-ImGui open/save (no zenity/osascript, so the
# docker-desktop lane can test it). Works on our 1.91.9 pin via an explicit
# `#if IMGUI_VERSION_NUM < 19201` guard in ImGuiFileDialog.cpp — do NOT track
# master (targets 1.92.3 / ImTextureRef). A COMPILED addon; includes
# imgui_internal.h -> compile-probe. We keep only the 3 core files (no stb
# thumbnails, no bundled dirent — POSIX has <dirent.h>). Tarball shape.
set -eu
ADDON_NAME=imguifiledialog
ADDON_VERSION=0.6.8
ADDON_TARBALL_URL="https://github.com/aiekick/ImGuiFileDialog/archive/refs/tags/v0.6.8.tar.gz"
ADDON_TARBALL_KEEP="ImGuiFileDialog.cpp ImGuiFileDialog.h ImGuiFileDialogConfig.h LICENSE"
ADDON_LICENSE_URL="https://raw.githubusercontent.com/aiekick/ImGuiFileDialog/v0.6.8/LICENSE"
ADDON_LICENSE_DEST=ImGuiFileDialog-LICENSE.txt
export ADDON_NAME ADDON_VERSION ADDON_TARBALL_URL ADDON_TARBALL_KEEP \
       ADDON_LICENSE_URL ADDON_LICENSE_DEST
exec "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/fetch-addon.sh"
