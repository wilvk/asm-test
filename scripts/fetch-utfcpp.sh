#!/bin/sh
# fetch-utfcpp.sh — utfcpp v4.0.6 (Boost Software License 1.0): the header-only
# UTF-8 library ImGuiTextSelect (docs/internal/archive/gui/14-quick-wins.md T6) includes
# as <utf8.h>. We keep just source/ (utf8.h + the utf8/ headers it pulls); the
# include dir is $(UTFCPP_HOME)/source. Thin wrapper over fetch-addon.sh.
set -eu
ADDON_NAME=utfcpp
ADDON_VERSION=4.0.6
ADDON_TARBALL_URL="https://github.com/nemtrif/utfcpp/archive/refs/tags/v4.0.6.tar.gz"
ADDON_TARBALL_KEEP="source"
ADDON_LICENSE_URL="https://raw.githubusercontent.com/nemtrif/utfcpp/v4.0.6/LICENSE"
ADDON_LICENSE_DEST=utfcpp-LICENSE.txt
export ADDON_NAME ADDON_VERSION ADDON_TARBALL_URL ADDON_TARBALL_KEEP \
       ADDON_LICENSE_URL ADDON_LICENSE_DEST
exec "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/fetch-addon.sh"
