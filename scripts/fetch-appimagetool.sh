#!/bin/sh
# fetch-appimagetool.sh — fetch the pinned appimagetool release into a local cache
# so the desktop AppImage lane (Dockerfile.syspkg-appimage) can package an AppDir
# without a system-wide install.
#
# appimagetool ships no pkg-config/find_package story at all — it is a standalone
# AppImage itself — so this mirrors fetch-dynamorio.sh: download the pinned release
# asset, verify it against scripts/third-party-digests.txt, cache it, and print the
# resulting executable path on stdout:
#     APPIMAGETOOL=$(scripts/fetch-appimagetool.sh)
# A build TOOL only — it is never bundled into the AppImage it produces, so it
# carries no THIRD-PARTY-LICENSES entry (collect-licenses.sh only covers what
# actually ships). Linux only; picks the asset matching `uname -m`.
#
# Override APPIMAGETOOL_VERSION to bump; APPIMAGETOOL_CACHE to relocate the cache.
set -eu

APPIMAGETOOL_VERSION="${APPIMAGETOOL_VERSION:-1.9.1}"
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib-thirdparty.sh"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
APPIMAGETOOL_CACHE="${APPIMAGETOOL_CACHE:-$root/build/appimagetool}"

prog=fetch-appimagetool

case "$(uname -m)" in
    x86_64|amd64)   arch=x86_64 ;;
    aarch64|arm64)  arch=aarch64 ;;
    *) echo "$prog: unsupported arch $(uname -m) (appimagetool ships x86_64/aarch64 releases only)" >&2; exit 1 ;;
esac

tool="$APPIMAGETOOL_CACHE/appimagetool-$arch-$APPIMAGETOOL_VERSION.AppImage"

log() { echo "$prog: $*" >&2; }

if [ ! -x "$tool" ]; then
    log "fetching appimagetool $APPIMAGETOOL_VERSION ($arch)"
    mkdir -p "$APPIMAGETOOL_CACHE"
    url="https://github.com/AppImage/appimagetool/releases/download/${APPIMAGETOOL_VERSION}/appimagetool-${arch}.AppImage"
    tmp="$APPIMAGETOOL_CACHE/.appimagetool.tmp"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$url" -o "$tmp"
    else
        wget -qO "$tmp" "$url"
    fi
    # Integrity pin (B5): a moved/swapped release asset must fail loudly, not run.
    want=$(tp_digest tarball-sha256 "appimagetool-$arch" "$APPIMAGETOOL_VERSION") || {
        log "ERROR: no pinned digest for appimagetool-$arch $APPIMAGETOOL_VERSION in $TP_MANIFEST"
        log "       (add one via scripts/refresh-thirdparty-digests.sh — refusing to use an unpinned download)"
        rm -f "$tmp"; exit 1
    }
    got="sha256:$(tp_sha256 "$tmp")"
    if [ "$got" != "$want" ]; then
        log "ERROR: appimagetool $APPIMAGETOOL_VERSION integrity check FAILED"
        log "       expected $want"
        log "       got      $got"
        rm -f "$tmp"; exit 1
    fi
    log "verified appimagetool $APPIMAGETOOL_VERSION ($got)"
    mv "$tmp" "$tool"
    chmod +x "$tool"
else
    log "reusing cached $tool"
fi

echo "$tool"
