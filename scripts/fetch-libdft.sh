#!/bin/sh
# fetch-libdft.sh — fetch the pinned libdft64 checkout into a local cache so the
# differential taint oracle (pin-libdft-taint-oracle.md) can be built against Pin
# without a system-wide install.
#
# libdft64 (AngoraFuzzer fork) is the mature, independently-implemented byte-level
# taint engine this repo cross-validates its DynamoRIO taint client against. It is
# GIT-COMMIT-pinned — exactly the way scripts/build-capstone.sh pins Capstone's tag
# to its immutable commit — so a moved branch cannot swap the oracle engine out from
# under the manifest. Clones the pinned commit under a cache dir, asserts HEAD, and
# prints the resulting LIBDFT_ROOT on stdout so a Makefile / Dockerfile can:
#     LIBDFT_ROOT=$(scripts/fetch-libdft.sh)
# It also captures libdft64's license into licenses/ on first fetch. Idempotent: a
# present checkout already at the pinned commit is reused.
#
# libdft64 is TEST/ORACLE-ONLY here (like Pin): it is never linked into libasmtest or
# any shipped binding — it lives only in the docker-taint-oracle test lane.
#
# Override LIBDFT_VERSION (the manifest key) / LIBDFT_URL to bump; LIBDFT_CACHE to
# relocate the cache (the Docker build points it at /opt so the checkout lands at
# /opt/libdft64).
set -eu

LIBDFT_VERSION="${LIBDFT_VERSION:-20804d5}"
LIBDFT_URL="${LIBDFT_URL:-https://github.com/AngoraFuzzer/libdft64.git}"
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib-thirdparty.sh"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LIBDFT_CACHE="${LIBDFT_CACHE:-$root/build/libdft}"
home="$LIBDFT_CACHE/libdft64"

log() { echo "fetch-libdft: $*" >&2; }

# Integrity pin (B5): the mutable branch is pinned to its immutable commit; refuse
# any checkout whose HEAD does not match — a moved branch must fail loudly, not
# silently swap the oracle engine.
want=$(tp_digest git-commit libdft64 "$LIBDFT_VERSION") || {
    log "ERROR: no pinned commit for libdft64 $LIBDFT_VERSION in $TP_MANIFEST"
    log "       (add one via scripts/refresh-thirdparty-digests.sh — refusing an unpinned checkout)"
    exit 1
}
commit="${want#commit:}"

is_pinned() {
    [ -d "$home/.git" ] || return 1
    got=$(git -C "$home" rev-parse HEAD 2>/dev/null) || return 1
    [ "$got" = "$commit" ]
}

if is_pinned; then
    log "reusing cached $home ($commit)"
else
    log "fetching libdft64 $commit"
    rm -rf "$home"
    mkdir -p "$LIBDFT_CACHE"
    # Fetch just the pinned commit (github.com serves a fetch-by-SHA); fall back to a
    # full clone + checkout if the server refuses an arbitrary-SHA want.
    if git init -q "$home" \
       && git -C "$home" remote add origin "$LIBDFT_URL" \
       && git -C "$home" fetch -q --depth 1 origin "$commit" \
       && git -C "$home" checkout -q --detach FETCH_HEAD; then
        :
    else
        log "fetch-by-SHA unavailable; falling back to full clone"
        rm -rf "$home"
        git clone -q "$LIBDFT_URL" "$home"
        git -C "$home" checkout -q --detach "$commit"
    fi
    got=$(git -C "$home" rev-parse HEAD)
    if [ "$got" != "$commit" ]; then
        log "ERROR: libdft64 integrity check FAILED"
        log "       expected $commit"
        log "       got      $got"
        rm -rf "$home"
        exit 1
    fi
    log "verified libdft64 ($got)"
fi

# Capture the upstream license text once, for provenance. libdft64 inherits the
# original libdft license (permissive); test/oracle-only, so this is recorded but
# never collected into a shipped package (see licenses/README.md).
lic="$root/licenses/libdft64.txt"
if [ ! -f "$lic" ]; then
    for c in "$home/LICENSE" "$home/LICENSE.md" "$home/LICENSE.txt" \
             "$home/COPYING" "$home/COPYING.txt"; do
        [ -f "$c" ] && cp "$c" "$lic" && log "captured license -> $(basename "$lic")" && break
    done
fi

[ -f "$home/src/libdft_api.h" ] || [ -f "$home/src/libdft_api.cpp" ] || [ -d "$home/src" ] || {
    log "ERROR: libdft64 src/ missing under $home (bad checkout?)"
    exit 1
}
echo "$home"
