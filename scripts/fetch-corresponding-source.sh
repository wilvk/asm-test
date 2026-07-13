#!/bin/sh
# fetch-corresponding-source.sh — assemble the GPL corresponding-source archive.
#
# Downloads the upstream source of the GPL-2.0 / BSD native dependencies asm-test
# redistributes, at the exact versions shipped, so a release can attach it
# (satisfying GPL-2.0 section 3) and back the written offer in every package's
# THIRD-PARTY-LICENSES/NOTICE. Keystone and Capstone are pinned (the build scripts
# clone these tags); Unicorn is a distro/brew binary whose version is recorded
# per-package, so the release job passes the actual version(s) shipped.
#
# Usage: scripts/fetch-corresponding-source.sh <outdir>
#   env: KEYSTONE_VERSION (0.9.2), CAPSTONE_VERSION (5.0.1),
#        UNICORN_VERSIONS (space-separated; default 2.1.4)
set -eu

prog=$(basename "$0")
out="${1:?usage: $prog <outdir>}"
KEYSTONE_VERSION="${KEYSTONE_VERSION:-0.9.2}"
CAPSTONE_VERSION="${CAPSTONE_VERSION:-5.0.1}"
UNICORN_VERSIONS="${UNICORN_VERSIONS:-2.1.4}"

mkdir -p "$out"

fetch() { # fetch <name> <github-repo> <tag>
    url="https://github.com/$2/archive/refs/tags/$3.tar.gz"
    f="$out/corresponding-source-$1-$3.tar.gz"
    echo "$prog: $1 $3 -> $(basename "$f")"
    curl -fsSL "$url" -o "$f"
}

fetch keystone keystone-engine/keystone "$KEYSTONE_VERSION"
fetch capstone  capstone-engine/capstone "$CAPSTONE_VERSION"
for v in $UNICORN_VERSIONS; do
    fetch unicorn unicorn-engine/unicorn "$v"
done

# --- LGPL-2.1 DynamoRIO extensions (drwrap, umbra) --------------------------
# Assembled only when a build opted into linking these into the drtrace client and a
# package ships libdrwrap/libumbra (default drtrace builds are BSD-clean). LGPL-2.1 §6
# requires the complete corresponding source of the relinkable library. Enable with
# INCLUDE_LGPL_SOURCE=1:
#   DR_VERSION        DynamoRIO release (its ext/drwrap IS drwrap's source); defaults to
#                     the fetch-dynamorio.sh pin. Fetched from the cronbuild-<ver> tag.
#   DRMEMORY_VERSION  the Dr. Memory tag whose bundled drmf matches the umbra shipped.
#                     REQUIRED (no default) — shipping the wrong/absent umbra source is
#                     worse than failing loudly. NOTE: umbra lives in a drmf submodule,
#                     so the tag tarball may need submodule source added by the release
#                     job; verify before publishing.
lgpl_source=0
if [ "${INCLUDE_LGPL_SOURCE:-0}" = "1" ]; then
    DR_VERSION="${DR_VERSION:-11.91.20630}"
    : "${DRMEMORY_VERSION:?INCLUDE_LGPL_SOURCE=1 requires DRMEMORY_VERSION (the Dr. Memory tag whose drmf matches the bundled umbra)}"
    fetch dynamorio DynamoRIO/dynamorio "cronbuild-$DR_VERSION"
    fetch drmemory  DynamoRIO/drmemory  "$DRMEMORY_VERSION"
    lgpl_source=1
fi

{
    echo "Corresponding source (GPL-2.0 section 3) for the native dependencies the"
    echo "asm-test packages redistribute, at the versions shipped:"
    echo
    echo "  keystone $KEYSTONE_VERSION  (GPL-2.0-only)"
    echo "  capstone $CAPSTONE_VERSION  (BSD-3-Clause)"
    for v in $UNICORN_VERSIONS; do echo "  unicorn  $v  (GPL-2.0-only)"; done
    if [ "$lgpl_source" = 1 ]; then
        echo
        echo "Corresponding source (LGPL-2.1 section 6) for the DynamoRIO extensions this"
        echo "package additionally bundles, at the versions shipped:"
        echo
        echo "  dynamorio $DR_VERSION  (LGPL-2.1-only; drwrap lives in ext/drwrap)"
        echo "  drmemory  $DRMEMORY_VERSION  (LGPL-2.1-only; umbra lives in the Dr. Memory Framework / drmf)"
        echo
        echo "  NOTE: umbra is a drmf submodule of the drmemory repo; confirm the archive"
        echo "  above includes the drmf source (add it if the tag tarball omits submodules)"
        echo "  before publishing."
    fi
    echo
    echo "Each archive is the upstream release tagged with that version. asm-test's"
    echo "own source (MIT) is the tagged repository at github.com/wilvk/asm-test."
} > "$out/SOURCES.txt"

echo "$prog: wrote $out (see SOURCES.txt)"
