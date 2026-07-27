#!/bin/sh
# fetch-iconfontcppheaders.sh — IconFontCppHeaders' IconsCodicons.h (gui
# 13-foundation-moves.md F3): the pure #define header giving ICON_CI_* macros +
# the ICON_MIN_CI/ICON_MAX_CI merge range for Codicons. zlib, version-agnostic.
set -eu
sha=210b5a399a64270674560d633638952d1e8d804d
ver=210b5a3
base="https://raw.githubusercontent.com/juliettef/IconFontCppHeaders/$sha"
ADDON_NAME=iconfontcppheaders
ADDON_VERSION=$ver
ADDON_FILES="$base/IconsCodicons.h IconsCodicons.h iconfontcppheaders $ver"
ADDON_LICENSE_URL="$base/licence.txt"
ADDON_LICENSE_DEST=IconFontCppHeaders-LICENSE.txt
export ADDON_NAME ADDON_VERSION ADDON_FILES ADDON_LICENSE_URL ADDON_LICENSE_DEST
exec "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/fetch-addon.sh"
