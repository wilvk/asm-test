#!/bin/sh
# fetch-sde.sh — fetch a pinned Intel Software Development Emulator (SDE) release
# into a local cache so the future/absent-ISA test lane can run without a
# system-wide install.
#
# SDE emulates ISA extensions the host CPU lacks (APX r16-r31, AVX10.2, AMX,
# AVX-512-on-AVX2) for the WHOLE process, so running an UNMODIFIED suite binary
# under `sde64 -future` gives future-ISA routines the full register/flag/memory/
# ABI assertion battery on any x86-64 host. This downloads the prebuilt SDE
# tarball, refuses it unless its SHA-256 matches scripts/third-party-digests.txt,
# extracts it under a cache dir, captures the (proprietary) license verbatim into
# licenses/, and prints the resulting SDE_HOME on stdout so a Makefile can:
#     SDE_HOME=$(scripts/fetch-sde.sh)
# Idempotent: a present, complete cache is reused. Linux x86-64 only (SDE is an
# x86-only emulator; the pinned kit is the lin (Linux) build).
#
# SDE is proprietary freeware (Intel Simplified Software License); it is a
# TEST/ORACLE-only dependency, never linked into any shipped artifact.
#
# Override SDE_VERSION to bump. The mirror id (915934) and build date
# (2026-03-15) in the URL are RELEASE-SPECIFIC, so a version bump must ALSO
# override SDE_URL whole (there is no version->URL formula on Intel's mirror);
# SDE_CACHE relocates the cache.
set -eu

SDE_VERSION="${SDE_VERSION:-10.8.0}"
SDE_URL="${SDE_URL:-https://downloadmirror.intel.com/915934/sde-external-${SDE_VERSION}-2026-03-15-lin.tar.xz}"
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib-thirdparty.sh"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDE_CACHE="${SDE_CACHE:-$root/build/sde}"
home="$SDE_CACHE/sde-external-$SDE_VERSION-2026-03-15-lin"

log() { echo "fetch-sde: $*" >&2; }

if [ ! -x "$home/sde64" ]; then
    log "fetching Intel SDE $SDE_VERSION"
    mkdir -p "$SDE_CACHE"
    tmp="$SDE_CACHE/.sde.tar.xz"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$SDE_URL" -o "$tmp"
    else
        wget -qO "$tmp" "$SDE_URL"
    fi
    # Integrity pin (B5): refuse to use the download unless it matches the digest
    # recorded in the manifest — a moved/swapped release asset must fail loudly.
    want=$(tp_digest tarball-sha256 intel-sde "$SDE_VERSION") || {
        log "ERROR: no pinned digest for intel-sde $SDE_VERSION in $TP_MANIFEST"
        log "       (add one via scripts/refresh-thirdparty-digests.sh — refusing to use an unpinned download)"
        rm -f "$tmp"; exit 1
    }
    got="sha256:$(tp_sha256 "$tmp")"
    if [ "$got" != "$want" ]; then
        log "ERROR: Intel SDE $SDE_VERSION integrity check FAILED"
        log "       expected $want"
        log "       got      $got"
        rm -f "$tmp"; exit 1
    fi
    log "verified Intel SDE $SDE_VERSION ($got)"
    rm -rf "$home"
    ( cd "$SDE_CACHE" && tar -xJf "$tmp" )
    rm -f "$tmp"
    log "extracted to $home"
else
    log "reusing cached $home"
fi

# Capture the upstream license verbatim once, at the pinned version. SDE's
# license is a PDF plus companions (not a single .txt), and the Intel Simplified
# Software License permits redistribution WITHOUT modification with the notice
# reproduced — so vendor the directory verbatim: LICENSE.pdf, the third-party
# programs notice, and the bundled Pin/XED "pin licensing" subdirectory.
licdir="$root/licenses/intel-sde-$SDE_VERSION"
if [ ! -d "$licdir" ]; then
    if [ -d "$home/Licenses" ]; then
        mkdir -p "$licdir/Licenses"
        [ -f "$home/Licenses/LICENSE.pdf" ] && \
            cp "$home/Licenses/LICENSE.pdf" "$licdir/Licenses/"
        [ -f "$home/Licenses/third-party-programs.txt" ] && \
            cp "$home/Licenses/third-party-programs.txt" "$licdir/Licenses/"
        [ -d "$home/Licenses/pin licensing" ] && \
            cp -R "$home/Licenses/pin licensing" "$licdir/Licenses/"
        log "captured license dir -> licenses/intel-sde-$SDE_VERSION/"
    else
        log "WARNING: $home/Licenses not found; license not captured"
    fi
fi

[ -x "$home/sde64" ] || { log "ERROR: sde64 missing under $home"; exit 1; }
echo "$home"
