#!/bin/sh
# fetch-dynamorio.sh — fetch a pinned DynamoRIO release into a local cache so the
# drtrace tier can be built + bundled without a system-wide install.
#
# Downloads the prebuilt DynamoRIO-Linux tarball (the tier needs it: DynamoRIO ships
# no pkg-config and needs find_package — see mk/native-trace.mk), extracts it under
# a cache dir, and prints the resulting DYNAMORIO_HOME on stdout so a Makefile can:
#     DYNAMORIO_HOME=$(scripts/fetch-dynamorio.sh)
# It also captures DynamoRIO's license text into licenses/ (for THIRD-PARTY-LICENSES)
# on first fetch. Idempotent: a present, complete cache is reused. Linux x86-64 only.
#
# Override DR_VERSION / DR_URL to bump; DR_CACHE to relocate the cache.
set -eu

DR_VERSION="${DR_VERSION:-11.91.20630}"
DR_URL="${DR_URL:-https://github.com/DynamoRIO/dynamorio/releases/download/cronbuild-${DR_VERSION}/DynamoRIO-Linux-${DR_VERSION}.tar.gz}"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DR_CACHE="${DR_CACHE:-$root/build/dynamorio}"
home="$DR_CACHE/DynamoRIO-Linux-$DR_VERSION"

log() { echo "fetch-dynamorio: $*" >&2; }

if [ ! -e "$home/lib64/release/libdynamorio.so" ]; then
    log "fetching DynamoRIO $DR_VERSION"
    mkdir -p "$DR_CACHE"
    tmp="$DR_CACHE/.dr.tar.gz"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$DR_URL" -o "$tmp"
    else
        wget -qO "$tmp" "$DR_URL"
    fi
    rm -rf "$home"
    ( cd "$DR_CACHE" && tar -xzf "$tmp" )
    # The tarball's top dir is DynamoRIO-Linux-<ver>; normalize if it differs.
    [ -d "$home" ] || { d=$(cd "$DR_CACHE" && ls -d DynamoRIO-*/ 2>/dev/null | head -1); [ -n "$d" ] && mv "$DR_CACHE/$d" "$home"; }
    rm -f "$tmp"
    log "extracted to $home"
else
    log "reusing cached $home"
fi

# Capture the upstream license text once, at the pinned version, for the NOTICE.
lic="$root/licenses/DynamoRIO-$DR_VERSION.txt"
if [ ! -f "$lic" ]; then
    for c in "$home/License.txt" "$home/license.txt" "$home/LICENSE" "$home/docs/License.txt"; do
        [ -f "$c" ] && cp "$c" "$lic" && log "captured license -> $(basename "$lic")" && break
    done
fi

[ -f "$home/lib64/release/libdynamorio.so" ] || { log "ERROR: libdynamorio.so missing under $home"; exit 1; }
echo "$home"
