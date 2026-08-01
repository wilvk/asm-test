#!/bin/sh
# fetch-textselect.sh — ImGuiTextSelect v1.1.6 (docs/internal/archive/gui/14-quick-wins.md
# T6): text selection + copy-out for line-oriented panes. A COMPILED addon
# (textselect.cpp) whose .cpp includes <imgui_internal.h> and <utf8.h> (see
# scripts/fetch-utfcpp.sh for the latter). Thin wrapper over fetch-addon.sh.
#
# PIN v1.1.6, NOT latest — v1.2.0+ dropped pre-1.92 ImGui support (v1.3.2 calls
# ImGui::GetFontBaked() / ImFont::CalcWordWrapPosition(), which do not exist on
# our 1.91.9 pin and do not compile). doc 11's refuted-and-corrected finding.
set -eu
ADDON_NAME=textselect
ADDON_VERSION=1.1.6
ADDON_TARBALL_URL="https://github.com/AidanSun05/ImGuiTextSelect/archive/refs/tags/v1.1.6.tar.gz"
ADDON_TARBALL_KEEP="textselect.hpp textselect.cpp LICENSE.txt"
ADDON_LICENSE_URL="https://raw.githubusercontent.com/AidanSun05/ImGuiTextSelect/v1.1.6/LICENSE.txt"
ADDON_LICENSE_DEST=ImGuiTextSelect-LICENSE.txt
export ADDON_NAME ADDON_VERSION ADDON_TARBALL_URL ADDON_TARBALL_KEEP \
       ADDON_LICENSE_URL ADDON_LICENSE_DEST
exec "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/fetch-addon.sh"
