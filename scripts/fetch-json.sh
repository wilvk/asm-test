#!/bin/sh
# fetch-json.sh — fetch the pinned nlohmann/json single header into a local cache
# so the desktop GUI (docs/internal/gui/03-desktop-shell.md) can parse .asmtrace
# NDJSON without a system-wide install. Mirrors fetch-dynamorio.sh / fetch-imgui.sh.
#
# Downloads the release asset json.hpp (one file — hashed directly, no tar step)
# into build/nlohmann-json/<ver>/nlohmann/json.hpp so that `#include
# <nlohmann/json.hpp>` resolves with a single -I on the parent. Prints JSON_HOME
# (that parent) on stdout so a Makefile can:
#     JSON_HOME=$(scripts/fetch-json.sh)
# Idempotent: a present header is reused. Any OS.
#
# The single header carries no embedded license text, so the license is committed
# by hand at licenses/nlohmann-json-LICENSE.MIT (verbatim LICENSE.MIT from the tag).
#
# Override JSON_VERSION / JSON_URL to bump; JSON_CACHE to relocate the cache.
set -eu

JSON_VERSION="${JSON_VERSION:-3.11.3}"
JSON_URL="${JSON_URL:-https://github.com/nlohmann/json/releases/download/v${JSON_VERSION}/json.hpp}"
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib-thirdparty.sh"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
JSON_CACHE="${JSON_CACHE:-$root/build/nlohmann-json}"
home="$JSON_CACHE/$JSON_VERSION"
hdr="$home/nlohmann/json.hpp"

log() { echo "fetch-json: $*" >&2; }

if [ ! -e "$hdr" ]; then
    log "fetching nlohmann/json $JSON_VERSION"
    mkdir -p "$home/nlohmann"
    tmp="$home/.json.hpp"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$JSON_URL" -o "$tmp"
    else
        wget -qO "$tmp" "$JSON_URL"
    fi
    # Integrity pin (B5): compiled INTO the user-facing desktop binaries — refuse
    # an unpinned/altered header rather than ship it silently.
    want=$(tp_digest tarball-sha256 nlohmann-json "$JSON_VERSION") || {
        log "ERROR: no pinned digest for nlohmann-json $JSON_VERSION in $TP_MANIFEST"
        log "       (add one by hand — refusing to use an unpinned download)"
        rm -f "$tmp"; exit 1
    }
    got="sha256:$(tp_sha256 "$tmp")"
    if [ "$got" != "$want" ]; then
        log "ERROR: nlohmann/json $JSON_VERSION integrity check FAILED"
        log "       expected $want"
        log "       got      $got"
        rm -f "$tmp"; exit 1
    fi
    log "verified nlohmann/json $JSON_VERSION ($got)"
    mv "$tmp" "$hdr"
    log "installed $hdr"
else
    log "reusing cached $hdr"
fi

[ -f "$hdr" ] || { log "ERROR: json.hpp missing under $home"; exit 1; }
echo "$home"
