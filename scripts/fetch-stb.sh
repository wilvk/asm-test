#!/bin/sh
# fetch-stb.sh — fetch the pinned stb_image_write.h single header so the desktop
# GUI's --shot screenshot mode can encode PNGs without a system-wide install.
# Mirrors fetch-linmath.sh (same single-header, pinned-commit, hash-the-file
# shape) rather than inventing a second pattern.
#
# Downloads the single header AT A PINNED COMMIT (stb ships no tagged releases —
# the commit is the immutable pin), hashes it directly (no tar step), and drops
# it at build/stb/<ver>/stb_image_write.h so `#include "stb_image_write.h"`
# resolves with a single -I on that dir. Prints STB_HOME on stdout so a Makefile
# can:
#     STB_HOME=$(scripts/fetch-stb.sh)
# Idempotent: a present header is reused. Any OS.
#
# stb is dual-licensed MIT / public domain; licenses/stb-LICENSE.txt carries the
# verbatim LICENSE from the pinned commit, exactly as fetch-linmath.sh commits
# linmath.h's by hand.
#
# This header is compiled into asmtest-desktop ONLY — never asmtest-viewer,
# which stays permissively distributable with no third-party image code (D4).
#
# Override STB_VERSION / STB_COMMIT / STB_URL to bump; STB_CACHE to relocate the
# cache. On a bump: set the new commit, run this, copy the printed "got" digest
# into scripts/third-party-digests.txt (name "stb-image-write").
set -eu

# STB_VERSION is the short-commit pin token used in the cache path and the digest
# row; STB_COMMIT is the immutable full commit the URL fetches. The header at
# this commit is stb_image_write v1.16.
STB_VERSION="${STB_VERSION:-f75e8d1}"
STB_COMMIT="${STB_COMMIT:-f75e8d1cad7d90d72ef7a4661f1b994ef78b4e31}"
STB_URL="${STB_URL:-https://raw.githubusercontent.com/nothings/stb/${STB_COMMIT}/stb_image_write.h}"
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib-thirdparty.sh"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
STB_CACHE="${STB_CACHE:-$root/build/stb}"
home="$STB_CACHE/$STB_VERSION"
hdr="$home/stb_image_write.h"

log() { echo "fetch-stb: $*" >&2; }

if [ ! -e "$hdr" ]; then
    log "fetching stb_image_write.h @ $STB_COMMIT"
    mkdir -p "$home"
    tmp="$home/.stb_image_write.h"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$STB_URL" -o "$tmp"
    else
        wget -qO "$tmp" "$STB_URL"
    fi
    # Integrity pin (B5): this header is compiled INTO a shipped binary, so
    # refuse an unpinned/altered download rather than encode screenshots with it
    # silently — the same discipline fetch-linmath.sh applies to linmath.h.
    want=$(tp_digest tarball-sha256 stb-image-write "$STB_VERSION") || {
        log "ERROR: no pinned digest for stb-image-write $STB_VERSION in $TP_MANIFEST"
        log "       (add one by hand — refusing to use an unpinned download)"
        rm -f "$tmp"; exit 1
    }
    got="sha256:$(tp_sha256 "$tmp")"
    if [ "$got" != "$want" ]; then
        log "ERROR: stb_image_write.h $STB_VERSION integrity check FAILED"
        log "       expected $want"
        log "       got      $got"
        rm -f "$tmp"; exit 1
    fi
    log "verified stb_image_write.h $STB_VERSION ($got)"
    mv "$tmp" "$hdr"
    log "installed $hdr"
else
    log "reusing cached $hdr"
fi

[ -f "$hdr" ] || { log "ERROR: stb_image_write.h missing under $home"; exit 1; }
echo "$home"
