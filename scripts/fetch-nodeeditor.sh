#!/bin/sh
# fetch-nodeeditor.sh — imgui-node-editor (docs/internal/gui/
# 15-plotting-and-graph-nav.md T2/T3): the graph pan/zoom canvas. PIN master sha
# 021aa0ea — the last release v0.9.3 FAILS to compile on 1.91.9 (operator==
# redefinition); this master sha's 4 TUs compile clean (doc-11, reproduced). The
# standalone imgui_canvas (T2's de-risk) and the full node editor (T3) both live
# here. imgui_canvas.h / imgui_node_editor_internal.h / imgui_extra_math.h include
# imgui_internal.h -> compile-probe. Tarball shape; MIT.
set -eu
sha=021aa0ea4da13fed864bafb2a92d4c5205076866
ADDON_NAME=imgui-node-editor
ADDON_VERSION=021aa0ea
ADDON_TARBALL_URL="https://github.com/thedmd/imgui-node-editor/archive/$sha.tar.gz"
# Keep the 4 TUs + all headers/inls they need + LICENSE. crude_json is the
# vendored settings serialiser the full editor pulls; the canvas alone needs only
# imgui_canvas.* + imgui_extra_math.* but keeping the set lets T3 reuse this fetch.
ADDON_TARBALL_KEEP="imgui_node_editor.cpp imgui_node_editor_api.cpp imgui_canvas.cpp crude_json.cpp imgui_node_editor.h imgui_node_editor_internal.h imgui_canvas.h crude_json.h imgui_bezier_math.h imgui_extra_math.h imgui_bezier_math.inl imgui_extra_math.inl imgui_node_editor_internal.inl LICENSE"
ADDON_LICENSE_URL="https://raw.githubusercontent.com/thedmd/imgui-node-editor/$sha/LICENSE"
ADDON_LICENSE_DEST=imgui-node-editor-LICENSE.txt
export ADDON_NAME ADDON_VERSION ADDON_TARBALL_URL ADDON_TARBALL_KEEP \
       ADDON_LICENSE_URL ADDON_LICENSE_DEST
exec "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/fetch-addon.sh"
