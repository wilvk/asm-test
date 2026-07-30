#!/bin/sh
# fetch-appimage-runtime.sh — fetch the pinned AppImage type-2 runtime stub into a
# local cache, for `appimagetool --runtime-file`.
#
# Left to itself, appimagetool downloads this ELF stub (the header prepended to
# every AppImage's squashfs) fresh on EVERY pack, from AppImage/type2-runtime's
# "continuous" tag — a rolling, unpinned build, exactly the kind of unverified
# download this project's B5 discipline (scripts/third-party-digests.txt) exists to
# avoid. This script fetches the same artifact from that project's dated
# `20251108` release instead (an immutable tag) and verifies it, so the AppImage
# lane (Dockerfile.syspkg-appimage) can:
#     RUNTIME=$(scripts/fetch-appimage-runtime.sh)
#     appimagetool --runtime-file "$RUNTIME" AppDir out.AppImage
# Never bundled into anything itself (it's a build tool input, not shipped source),
# so it carries no THIRD-PARTY-LICENSES entry. Linux only; picks the asset matching
# `uname -m`.
#
# Override APPIMAGE_RUNTIME_VERSION to bump; APPIMAGE_RUNTIME_CACHE to relocate.
set -eu

APPIMAGE_RUNTIME_VERSION="${APPIMAGE_RUNTIME_VERSION:-20251108}"
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib-thirdparty.sh"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
APPIMAGE_RUNTIME_CACHE="${APPIMAGE_RUNTIME_CACHE:-$root/build/appimage-runtime}"

prog=fetch-appimage-runtime

case "$(uname -m)" in
    x86_64|amd64)   arch=x86_64 ;;
    aarch64|arm64)  arch=aarch64 ;;
    *) echo "$prog: unsupported arch $(uname -m) (type2-runtime ships x86_64/aarch64 assets we pin)" >&2; exit 1 ;;
esac

runtime="$APPIMAGE_RUNTIME_CACHE/runtime-$arch-$APPIMAGE_RUNTIME_VERSION"

log() { echo "$prog: $*" >&2; }

if [ ! -f "$runtime" ]; then
    log "fetching type2-runtime $APPIMAGE_RUNTIME_VERSION ($arch)"
    mkdir -p "$APPIMAGE_RUNTIME_CACHE"
    url="https://github.com/AppImage/type2-runtime/releases/download/${APPIMAGE_RUNTIME_VERSION}/runtime-${arch}"
    tmp="$APPIMAGE_RUNTIME_CACHE/.runtime.tmp"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$url" -o "$tmp"
    else
        wget -qO "$tmp" "$url"
    fi
    # Integrity pin (B5): a moved/swapped release asset must fail loudly, not run.
    want=$(tp_digest tarball-sha256 "appimage-runtime-$arch" "$APPIMAGE_RUNTIME_VERSION") || {
        log "ERROR: no pinned digest for appimage-runtime-$arch $APPIMAGE_RUNTIME_VERSION in $TP_MANIFEST"
        log "       (add one via scripts/refresh-thirdparty-digests.sh — refusing to use an unpinned download)"
        rm -f "$tmp"; exit 1
    }
    got="sha256:$(tp_sha256 "$tmp")"
    if [ "$got" != "$want" ]; then
        log "ERROR: type2-runtime $APPIMAGE_RUNTIME_VERSION integrity check FAILED"
        log "       expected $want"
        log "       got      $got"
        rm -f "$tmp"; exit 1
    fi
    log "verified type2-runtime $APPIMAGE_RUNTIME_VERSION ($got)"
    mv "$tmp" "$runtime"
    chmod +x "$runtime"
else
    log "reusing cached $runtime"
fi

echo "$runtime"
