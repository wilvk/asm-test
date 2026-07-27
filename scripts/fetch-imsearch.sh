#!/bin/sh
# fetch-imsearch.sh — ImSearch (docs/internal/gui/16-live-feedback-and-filtering.md
# T2): client-side filtering that wraps existing Selectable/TreeNode draws via
# callbacks. 3 files, C++11; imsearch.cpp includes imgui_internal.h -> the
# compile-probe. Default branch is `main` (there is no master). Thin wrapper over
# fetch-addon.sh (files shape). Compile-verified on 1.91.9b-docking.
set -eu
sha=7596ac5cfcaf473dc5d7716b8449bb40c61a5300 # full commit, for the raw URL
ver=7596ac5                                   # short token: cache path + digest key
base="https://raw.githubusercontent.com/GuusKemperman/ImSearch/$sha"
ADDON_NAME=imsearch
ADDON_VERSION=$ver
ADDON_FILES="$base/imsearch.h imsearch.h imsearch-h $ver
$base/imsearch.cpp imsearch.cpp imsearch-cpp $ver
$base/imsearch_internal.h imsearch_internal.h imsearch-ih $ver"
ADDON_LICENSE_URL="$base/LICENSE"
ADDON_LICENSE_DEST=ImSearch-LICENSE.txt
export ADDON_NAME ADDON_VERSION ADDON_FILES ADDON_LICENSE_URL ADDON_LICENSE_DEST
exec "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/fetch-addon.sh"
