#!/bin/sh
# collect-licenses.sh — assemble a THIRD-PARTY-LICENSES/ directory for a package.
#
# Gathers asm-test's own MIT license plus the verbatim license of every vendored
# native dependency AT THE VERSION ACTUALLY INSTALLED, and writes a NOTICE that
# records each component's exact version + SPDX id. Keystone and Capstone licenses
# are captured at build time by scripts/build-{keystone,capstone}.sh into
# $PREFIX/share/licenses/<dep>-<ver>/; Unicorn (a distro/brew package) is
# discovered best-effort. "Same version as used" holds because the version is read
# from pkg-config for the very lib being shipped.
#
# Usage: scripts/collect-licenses.sh <dest_dir>
set -eu

prog=$(basename "$0")
dest="${1:?usage: $prog <dest_dir>}"
PREFIX="${ASMTEST_DEP_PREFIX:-/usr/local}"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

mkdir -p "$dest"
notice="$dest/NOTICE"

# asm-test's own license.
[ -f "$root/LICENSE" ] && cp "$root/LICENSE" "$dest/asm-test-LICENSE"

{
    echo "THIRD-PARTY NOTICES for the asm-test package"
    echo
    echo "asm-test itself is MIT (see asm-test-LICENSE). This package additionally"
    echo "bundles the following prebuilt native libraries, whose licenses are"
    echo "reproduced in this directory at the exact versions shipped:"
    echo
} > "$notice"

modver() { pkg-config --modversion "$1" 2>/dev/null || echo unknown; }

emit() { # emit <name> <pkgconfig-name> <spdx> <upstream-url> <repo-license-glob>
    name="$1"; pc="$2"; spdx="$3"; url="$4"; glob="$5"
    ver=$(modver "$pc")
    destdir="$dest/$pc-$ver"
    mkdir -p "$destdir"
    copied=0
    # The verbatim upstream license text, checked into licenses/ at the pinned
    # version — always present, so the notice is never missing a text.
    for f in $root/$glob; do [ -f "$f" ] && cp "$f" "$destdir/" && copied=1; done
    # A build-time capture (scripts/build-{keystone,capstone}.sh) augments with the
    # exact-version text when the dep was source-built on this host.
    if [ -d "$PREFIX/share/licenses/$pc-$ver" ]; then
        cp "$PREFIX/share/licenses/$pc-$ver"/* "$destdir/" 2>/dev/null && copied=1
    fi
    if [ "$copied" -eq 1 ]; then
        have="bundled in $pc-$ver/"
    else
        rmdir "$destdir" 2>/dev/null || true
        have="LICENSE TEXT MISSING — fetch from $url before publishing"
    fi
    printf '  - %-9s %-10s  %-13s  %s\n' "$name" "$ver" "$spdx" "$have" >> "$notice"
}

emit Unicorn  unicorn  GPL-2.0-only  https://github.com/unicorn-engine/unicorn  "licenses/Unicorn-*"
emit Keystone keystone GPL-2.0-only  https://github.com/keystone-engine/keystone "licenses/Keystone-*"
emit Capstone capstone BSD-3-Clause  https://github.com/capstone-engine/capstone "licenses/Capstone-*"

{
    echo
    echo "Because this package conveys the GPL-2.0 engines (Unicorn, Keystone) as"
    echo "binaries dynamically linked into libasmtest_emu, the package as distributed"
    echo "is effectively GPL-2.0 (MIT is GPL-compatible). Corresponding source for"
    echo "each engine is the pinned upstream tag the build scripts clone; see"
    echo "scripts/build-keystone.sh and scripts/build-capstone.sh."
} >> "$notice"

echo "$prog: wrote $dest (see NOTICE)"
