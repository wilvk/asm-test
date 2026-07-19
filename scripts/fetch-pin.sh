#!/bin/sh
# fetch-pin.sh — fetch a pinned Intel Pin kit into a local cache so the pintool
# tier can be built + run without a system-wide install.
#
# Downloads the prebuilt Pin gcc-linux tarball, extracts it under a cache dir,
# and prints the resulting PIN_HOME on stdout so a Makefile can:
#     PIN_HOME=$(scripts/fetch-pin.sh)
# It also captures Pin's license texts into licenses/ (for THIRD-PARTY-LICENSES)
# on first fetch. Idempotent: a present, complete cache is reused. Linux x86-64
# only (the pinned kit is gcc-linux).
#
# Override PIN_VERSION / PIN_URL to bump; PIN_CACHE to relocate the cache.
set -eu

PIN_VERSION="${PIN_VERSION:-4.2-99776-g21d818fa2}"
PIN_URL="${PIN_URL:-https://software.intel.com/sites/landingpage/pintool/downloads/pin-external-${PIN_VERSION}-gcc-linux.tar.gz}"
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib-thirdparty.sh"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PIN_CACHE="${PIN_CACHE:-$root/build/pin}"
home="$PIN_CACHE/pin-external-$PIN_VERSION-gcc-linux"

log() { echo "fetch-pin: $*" >&2; }

if [ ! -x "$home/pin" ]; then
    log "fetching Intel Pin $PIN_VERSION"
    mkdir -p "$PIN_CACHE"
    tmp="$PIN_CACHE/.pin.tar.gz"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$PIN_URL" -o "$tmp"
    else
        wget -qO "$tmp" "$PIN_URL"
    fi
    # Integrity pin (B5): Intel publishes no official hash for this tarball, so
    # the manifest value is a self-computed digest, cross-checked by a second
    # independent computation before it was pinned (see the implementation doc).
    # Refuse to use a download that no longer matches it.
    want=$(tp_digest tarball-sha256 pin "$PIN_VERSION") || {
        log "ERROR: no pinned digest for pin $PIN_VERSION in $TP_MANIFEST"
        log "       (add one via scripts/refresh-thirdparty-digests.sh — refusing to use an unpinned download)"
        rm -f "$tmp"; exit 1
    }
    got="sha256:$(tp_sha256 "$tmp")"
    if [ "$got" != "$want" ]; then
        log "ERROR: Pin $PIN_VERSION integrity check FAILED"
        log "       expected $want"
        log "       got      $got"
        rm -f "$tmp"; exit 1
    fi
    log "verified Pin $PIN_VERSION ($got)"
    rm -rf "$home"
    ( cd "$PIN_CACHE" && tar -xzf "$tmp" )
    # The tarball's top dir is pin-external-<ver>-gcc-linux for the newer kits, but the
    # 3.20 kit (libdft64's tested pin) ships as plain pin-<ver>-gcc-linux with NO
    # `external` segment — so normalize both spellings to $home. The `pin-*/` fallback is
    # only reached for a non-external kit (the external kit's $home already exists after
    # extraction and short-circuits this line), so the 4.2 path is unaffected.
    [ -d "$home" ] || { d=$(cd "$PIN_CACHE" && ls -d pin-external-*/ pin-*/ 2>/dev/null | head -1); [ -n "$d" ] && mv "$PIN_CACHE/$d" "$home"; }
    rm -f "$tmp"
    log "extracted to $home"
else
    log "reusing cached $home"
fi

# Capture the upstream license texts once, at the pinned version, for the NOTICE.
# Test-lane only (see licenses/README.md): Pin is never bundled into a shipped
# package, so this capture exists for provenance, not for collect-licenses.sh.
lic="$root/licenses/Pin-$PIN_VERSION.txt"
# The 4.2 kit names its license licensing/intel-simplified-software-license.txt; the
# 3.20 kit names the same terms licensing/EULA.txt. Try both (first hit wins, so 4.2
# is unaffected) so either kit captures its proprietary license text.
if [ ! -f "$lic" ]; then
    for c in "$home/licensing/intel-simplified-software-license.txt" \
             "$home/licensing/EULA.txt"; do
        [ -f "$c" ] && cp "$c" "$lic" && log "captured license -> $(basename "$lic")" && break
    done
fi
lic3p="$root/licenses/Pin-$PIN_VERSION-third-party.txt"
[ -f "$lic3p" ] || { [ -f "$home/licensing/third-party-programs.txt" ] && cp "$home/licensing/third-party-programs.txt" "$lic3p" && log "captured license -> $(basename "$lic3p")"; }

[ -x "$home/pin" ] || { log "ERROR: pin launcher missing under $home"; exit 1; }
echo "$home"
