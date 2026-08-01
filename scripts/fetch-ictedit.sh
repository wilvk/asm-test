#!/bin/sh
# fetch-ictedit.sh — goossens ImGuiColorTextEdit + TextDiff
# (docs/internal/archive/gui/17-interaction-testing-and-editor.md T2): a real code editor
# for the Author door + a side-by-side diff. master f67e5bc, MIT (+ bundled dtl,
# BSD-3). Every commit since 2025-06 needs ImGui 1.92 for 2 SDL3-IME lines;
# doc-11's compile-verified mitigation wraps exactly those two assignments
# (PlatformImeData.WantTextInput / .ViewportId) in `#if IMGUI_VERSION_NUM >= 19200`,
# restoring the 1.91.9 pin. This wrapper applies that guard to the fetched
# TextEditor.cpp (idempotent). Both .cpp include imgui_internal.h -> compile-probe.
set -eu
here="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
sha=f67e5bc0a3ba1016bdebae44db60acbdeeb098c6
ADDON_NAME=ictedit
ADDON_VERSION=f67e5bc
ADDON_TARBALL_URL="https://github.com/goossens/ImGuiColorTextEdit/archive/$sha.tar.gz"
ADDON_TARBALL_KEEP="TextEditor.cpp TextEditor.h TextDiff.cpp TextDiff.h dtl.h LICENSE"
ADDON_LICENSE_URL="https://raw.githubusercontent.com/goossens/ImGuiColorTextEdit/$sha/LICENSE"
ADDON_LICENSE_DEST=ImGuiColorTextEdit-LICENSE.txt
export ADDON_NAME ADDON_VERSION ADDON_TARBALL_URL ADDON_TARBALL_KEEP \
       ADDON_LICENSE_URL ADDON_LICENSE_DEST
# Fetch (call, not exec — we post-process the result); fetch-addon.sh prints the
# install dir on stdout.
home="$("$here/fetch-addon.sh")"
ed="$home/TextEditor.cpp"
# The 1.92-only guard, applied once (idempotent — skip if already present).
if ! grep -q 'IMGUI_VERSION_NUM >= 19200' "$ed"; then
    perl -i -pe 's{^(\s*context->PlatformImeData\.WantTextInput = true;)$}{#if IMGUI_VERSION_NUM >= 19200\n$1\n#endif}; s{^(\s*context->PlatformImeData\.ViewportId = .*;)$}{#if IMGUI_VERSION_NUM >= 19200\n$1\n#endif};' "$ed"
fi
echo "$home"
