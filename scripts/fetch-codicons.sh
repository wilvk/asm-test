#!/bin/sh
# fetch-codicons.sh — the VS Code Codicons icon font (gui 13-foundation-moves.md
# F3): step-into/over, watch, breakpoint, etc. CC-BY-4.0 (font). Merged over the
# monospace font at [ICON_MIN_CI, ICON_MAX_CI]. Files shape.
set -eu
ver=0.0.35
base="https://github.com/microsoft/vscode-codicons/raw/$ver"
ADDON_NAME=codicons
ADDON_VERSION=$ver
ADDON_FILES="$base/dist/codicon.ttf codicon.ttf codicons $ver"
ADDON_LICENSE_URL="$base/LICENSE"
ADDON_LICENSE_DEST=Codicons-LICENSE.txt
export ADDON_NAME ADDON_VERSION ADDON_FILES ADDON_LICENSE_URL ADDON_LICENSE_DEST
exec "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/fetch-addon.sh"
