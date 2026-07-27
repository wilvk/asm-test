#!/bin/sh
# fetch-implot.sh — ImPlot v1.0 (docs/internal/gui/15-plotting-and-graph-nav.md
# T1): the plotting chassis. A COMPILED addon (implot.cpp + implot_items.cpp).
# PIN the v1.0 TAG — master is v1.1 WIP tracking ImGui 1.92, and v1.0's ImPlotSpec
# redesign already differs from every pre-2026 example. implot.h is public-API;
# implot_internal.h (pulled by the two .cpp) includes imgui_internal.h. Tarball.
set -eu
ADDON_NAME=implot
ADDON_VERSION=v1.0
ADDON_TARBALL_URL="https://github.com/epezent/implot/archive/refs/tags/v1.0.tar.gz"
ADDON_TARBALL_KEEP="implot.h implot_internal.h implot.cpp implot_items.cpp LICENSE"
ADDON_LICENSE_URL="https://raw.githubusercontent.com/epezent/implot/v1.0/LICENSE"
ADDON_LICENSE_DEST=ImPlot-LICENSE.txt
export ADDON_NAME ADDON_VERSION ADDON_TARBALL_URL ADDON_TARBALL_KEEP \
       ADDON_LICENSE_URL ADDON_LICENSE_DEST
exec "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/fetch-addon.sh"
