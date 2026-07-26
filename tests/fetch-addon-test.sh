#!/bin/sh
# fetch-addon-test.sh — exercise scripts/fetch-addon.sh end to end against a
# real pinned artifact, so the addon supply chain (docs/internal/gui/
# 12-addon-supply-chain.md) has a regression test that does not wait for a real
# addon. Uses the already-pinned `linmath` header as the fixture: fetch-addon.sh
# in FILES mode must produce a byte-identical file to the canonical
# fetch-linmath.sh, and must REFUSE an unpinned download (the B5 integrity gate).
# Network required (the supply chain is a network of pinned downloads).
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

fail() { echo "fetch-addon-test: FAIL — $*" >&2; exit 1; }

sha=26211bb27f5f500ad513ff52986fec4158de2a15
url="https://raw.githubusercontent.com/datenwolf/linmath.h/$sha/linmath.h"
probe="build/addons/linmath-probe-26211bb"
rm -rf "$probe"

# 1) happy path: fetch + verify against the pinned `linmath` digest row.
home=$(ADDON_NAME=linmath-probe ADDON_VERSION=26211bb \
       ADDON_FILES="$url linmath.h linmath 26211bb" \
       sh scripts/fetch-addon.sh) || fail "happy-path fetch exited non-zero"
[ -f "$home/linmath.h" ] || fail "no linmath.h under $home"

# 2) byte-identical to the canonical single-purpose fetcher's output.
canon=$(sh scripts/fetch-linmath.sh) || fail "fetch-linmath.sh exited non-zero"
cmp "$home/linmath.h" "$canon/linmath.h" || fail "fetched header differs from fetch-linmath.sh"

# 3) idempotent: a second run reuses the cache and still succeeds.
home2=$(ADDON_NAME=linmath-probe ADDON_VERSION=26211bb \
        ADDON_FILES="$url linmath.h linmath 26211bb" \
        sh scripts/fetch-addon.sh) || fail "second (cached) fetch exited non-zero"
[ "$home2" = "$home" ] || fail "cache path changed between runs"

# 4) refuse-unpinned: an unknown digest name must exit non-zero and say so.
if ADDON_NAME=nope ADDON_VERSION=zzz \
     ADDON_FILES="$url linmath.h no-such-digest zzz" \
     sh scripts/fetch-addon.sh >/dev/null 2>"build/addons/.refuse.err"; then
    fail "unpinned download was NOT refused"
fi
grep -qi 'no pinned digest' "build/addons/.refuse.err" || fail "refusal did not name the missing digest"

# 5) missing-required-env must fail loudly, not silently no-op.
if ADDON_NAME=only sh scripts/fetch-addon.sh >/dev/null 2>&1; then
    fail "fetch-addon.sh ran with neither ADDON_FILES nor ADDON_TARBALL_URL"
fi

rm -rf "$probe" build/addons/nope-zzz build/addons/.refuse.err
echo "fetch-addon-test: all checks passed"
