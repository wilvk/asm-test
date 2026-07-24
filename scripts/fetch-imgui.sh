#!/bin/sh
# fetch-imgui.sh — fetch a pinned Dear ImGui release into a local cache so the
# desktop GUI (docs/internal/gui/03-desktop-shell.md) can compile ImGui into its
# two binaries without a system-wide install. Mirrors fetch-dynamorio.sh.
#
# Downloads the GitHub source-archive tarball, extracts it under a cache dir, and
# prints the resulting IMGUI_HOME on stdout so a Makefile can:
#     IMGUI_HOME=$(scripts/fetch-imgui.sh)
# It also captures ImGui's license text into licenses/ on first fetch (for
# THIRD-PARTY-LICENSES). Idempotent: a present, complete cache is reused. Any OS.
#
# Override IMGUI_VERSION / IMGUI_URL to bump; IMGUI_CACHE to relocate the cache.
set -eu

IMGUI_VERSION="${IMGUI_VERSION:-1.91.9}"
IMGUI_URL="${IMGUI_URL:-https://github.com/ocornut/imgui/archive/refs/tags/v${IMGUI_VERSION}.tar.gz}"
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib-thirdparty.sh"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IMGUI_CACHE="${IMGUI_CACHE:-$root/build/imgui}"
home="$IMGUI_CACHE/imgui-$IMGUI_VERSION"

log() { echo "fetch-imgui: $*" >&2; }

if [ ! -e "$home/imgui.cpp" ]; then
    log "fetching Dear ImGui $IMGUI_VERSION"
    mkdir -p "$IMGUI_CACHE"
    tmp="$IMGUI_CACHE/.imgui.tar.gz"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$IMGUI_URL" -o "$tmp"
    else
        wget -qO "$tmp" "$IMGUI_URL"
    fi
    # Integrity pin (B5): ImGui is compiled INTO the user-facing desktop binaries,
    # so refuse to use it unless it matches the digest recorded in the manifest —
    # a moved/swapped release asset must fail loudly, not ship silently.
    want=$(tp_digest tarball-sha256 imgui "$IMGUI_VERSION") || {
        log "ERROR: no pinned digest for imgui $IMGUI_VERSION in $TP_MANIFEST"
        log "       (add one by hand — refusing to use an unpinned download)"
        rm -f "$tmp"; exit 1
    }
    got="sha256:$(tp_sha256 "$tmp")"
    if [ "$got" != "$want" ]; then
        log "ERROR: Dear ImGui $IMGUI_VERSION integrity check FAILED"
        log "       expected $want"
        log "       got      $got"
        rm -f "$tmp"; exit 1
    fi
    log "verified Dear ImGui $IMGUI_VERSION ($got)"
    rm -rf "$home"
    ( cd "$IMGUI_CACHE" && tar -xzf "$tmp" )
    # The tarball's top dir is imgui-<ver>; normalize if it differs.
    [ -d "$home" ] || { d=$(cd "$IMGUI_CACHE" && ls -d imgui-*/ 2>/dev/null | head -1); [ -n "$d" ] && mv "$IMGUI_CACHE/$d" "$home"; }
    rm -f "$tmp"
    log "extracted to $home"
else
    log "reusing cached $home"
fi

# Capture the upstream license text once, at the pinned version, for the NOTICE.
lic="$root/licenses/DearImGui-LICENSE.txt"
[ -f "$lic" ] || { cp "$home/LICENSE.txt" "$lic" && log "captured license -> $(basename "$lic")"; }

[ -f "$home/imgui.cpp" ] || { log "ERROR: imgui.cpp missing under $home"; exit 1; }
[ -f "$home/backends/imgui_impl_glfw.cpp" ] || { log "ERROR: backends missing under $home"; exit 1; }
echo "$home"
