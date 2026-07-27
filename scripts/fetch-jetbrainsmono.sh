#!/bin/sh
# fetch-jetbrainsmono.sh — JetBrains Mono Regular TTF (gui 13-foundation-moves.md
# F3): the real monospace face for hex/registers/disasm. OFL-1.1. Loads via
# stb_truetype (no freetype needed), so it works on every lane. Files shape.
set -eu
ver=v2.304
base="https://github.com/JetBrains/JetBrainsMono/raw/$ver"
ADDON_NAME=jetbrainsmono
ADDON_VERSION=$ver
ADDON_FILES="$base/fonts/ttf/JetBrainsMono-Regular.ttf JetBrainsMono-Regular.ttf jetbrainsmono $ver"
ADDON_LICENSE_URL="$base/OFL.txt"
ADDON_LICENSE_DEST=JetBrainsMono-OFL.txt
export ADDON_NAME ADDON_VERSION ADDON_FILES ADDON_LICENSE_URL ADDON_LICENSE_DEST
exec "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/fetch-addon.sh"
