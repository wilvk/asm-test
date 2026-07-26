#!/bin/sh
# fetch-linmath.sh — fetch the pinned linmath.h single header into a local cache
# so the desktop GUI's 3D spacetime overview (docs/internal/gui/10-spacetime-3d-
# overview.md T4) can do its camera math (mat4x4_perspective / _look_at / _mul)
# without a system-wide install. Mirrors fetch-json.sh / fetch-imgui.sh.
#
# Downloads the single header linmath.h AT A PINNED COMMIT (linmath.h has no
# tagged releases — the commit is the immutable pin), hashes it directly (no tar
# step), and drops it at build/linmath/<ver>/linmath.h so `#include "linmath.h"`
# resolves with a single -I on that dir. Prints LINMATH_HOME on stdout so a
# Makefile can:
#     LINMATH_HOME=$(scripts/fetch-linmath.sh)
# Idempotent: a present header is reused. Any OS.
#
# The header carries no embedded license text, so the WTFPL notice is committed
# by hand at licenses/linmath-LICENSE.txt (verbatim LICENCE from the pinned
# commit), exactly as fetch-json.sh commits nlohmann/json's by hand.
#
# Override LINMATH_VERSION / LINMATH_COMMIT / LINMATH_URL to bump; LINMATH_CACHE
# to relocate the cache. On a bump: set the new commit, run this, copy the printed
# "got" digest into scripts/third-party-digests.txt (name "linmath").
set -eu

# LINMATH_VERSION is the short-commit pin token used in the cache path and the
# digest row; LINMATH_COMMIT is the immutable full commit the URL fetches.
LINMATH_VERSION="${LINMATH_VERSION:-26211bb}"
LINMATH_COMMIT="${LINMATH_COMMIT:-26211bb27f5f500ad513ff52986fec4158de2a15}"
LINMATH_URL="${LINMATH_URL:-https://raw.githubusercontent.com/datenwolf/linmath.h/${LINMATH_COMMIT}/linmath.h}"
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib-thirdparty.sh"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LINMATH_CACHE="${LINMATH_CACHE:-$root/build/linmath}"
home="$LINMATH_CACHE/$LINMATH_VERSION"
hdr="$home/linmath.h"

log() { echo "fetch-linmath: $*" >&2; }

if [ ! -e "$hdr" ]; then
    log "fetching linmath.h @ $LINMATH_COMMIT"
    mkdir -p "$home"
    tmp="$home/.linmath.h"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$LINMATH_URL" -o "$tmp"
    else
        wget -qO "$tmp" "$LINMATH_URL"
    fi
    # Integrity pin (B5): linmath.h is compiled INTO the desktop binaries, so
    # refuse an unpinned/altered header rather than ship it silently — the same
    # discipline fetch-json.sh applies to the nlohmann/json single header.
    want=$(tp_digest tarball-sha256 linmath "$LINMATH_VERSION") || {
        log "ERROR: no pinned digest for linmath $LINMATH_VERSION in $TP_MANIFEST"
        log "       (add one by hand — refusing to use an unpinned download)"
        rm -f "$tmp"; exit 1
    }
    got="sha256:$(tp_sha256 "$tmp")"
    if [ "$got" != "$want" ]; then
        log "ERROR: linmath.h $LINMATH_VERSION integrity check FAILED"
        log "       expected $want"
        log "       got      $got"
        rm -f "$tmp"; exit 1
    fi
    log "verified linmath.h $LINMATH_VERSION ($got)"
    mv "$tmp" "$hdr"
    log "installed $hdr"
else
    log "reusing cached $hdr"
fi

[ -f "$hdr" ] || { log "ERROR: linmath.h missing under $home"; exit 1; }
echo "$home"
