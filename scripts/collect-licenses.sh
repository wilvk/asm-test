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
# The payload slot is the dir holding THIRD-PARTY-LICENSES/, so we can tell which
# optional native-trace tier libs (and their vendored deps) were actually staged.
slot=$(dirname "$dest")

mkdir -p "$dest"
notice="$dest/NOTICE"

present() { ls "$slot"/$1 >/dev/null 2>&1; }   # present '<glob>' — is a vendored lib in the slot?

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

# emit_lit — like emit, but with a literal version (deps that ship no pkg-config
# file, e.g. DynamoRIO and libipt). Same "missing text → flag before publish" rule.
emit_lit() { # emit_lit <name> <slug> <version> <spdx> <upstream-url> <repo-license-glob>
    name="$1"; pc="$2"; ver="$3"; spdx="$4"; url="$5"; glob="$6"
    destdir="$dest/$pc-$ver"
    mkdir -p "$destdir"
    copied=0
    for f in $root/$glob; do [ -f "$f" ] && cp "$f" "$destdir/" && copied=1; done
    if [ -d "$PREFIX/share/licenses/$pc-$ver" ]; then
        cp "$PREFIX/share/licenses/$pc-$ver"/* "$destdir/" 2>/dev/null && copied=1
    fi
    if [ "$copied" -eq 1 ]; then have="bundled in $pc-$ver/"; else
        rmdir "$destdir" 2>/dev/null || true
        have="LICENSE TEXT MISSING — fetch from $url before publishing"; fi
    printf '  - %-9s %-10s  %-13s  %s\n' "$name" "$ver" "$spdx" "$have" >> "$notice"
}

emit Unicorn  unicorn  GPL-2.0-only  https://github.com/unicorn-engine/unicorn  "licenses/Unicorn-*"
emit Keystone keystone GPL-2.0-only  https://github.com/keystone-engine/keystone "licenses/Keystone-*"
emit Capstone capstone BSD-3-Clause  https://github.com/capstone-engine/capstone "licenses/Capstone-*"

# Optional native-trace tiers, emitted only when their libs were staged into this
# slot (package-native.sh --tier). All permissive → they add no copyleft beyond the
# Unicorn/Keystone GPL-2.0 the package already carries.
present 'libdynamorio*' && \
  emit_lit DynamoRIO dynamorio "${ASMTEST_DR_VERSION:-unknown}" BSD-3-Clause \
           https://github.com/DynamoRIO/dynamorio "licenses/DynamoRIO-*"
present 'libipt*' && \
  emit_lit libipt libipt "${ASMTEST_LIBIPT_VERSION:-unknown}" BSD-2-Clause \
           https://github.com/intel/libipt "licenses/libipt-*"
present 'libopencsd*' && \
  emit OpenCSD libopencsd BSD-3-Clause https://github.com/Linaro/OpenCSD "licenses/OpenCSD-*"
present 'libbpf*' && \
  emit libbpf libbpf BSD-2-Clause https://github.com/libbpf/libbpf "licenses/libbpf-*"

{
    echo
    echo "Because this package conveys the GPL-2.0 engines Unicorn and Keystone as"
    echo "binaries dynamically linked into libasmtest_emu, the package as distributed"
    echo "is effectively GPL-2.0 (MIT is GPL-compatible; Capstone is BSD-3-Clause). Any"
    echo "native-trace tier libraries listed above (DynamoRIO, libipt, OpenCSD, libbpf)"
    echo "are permissively licensed and add no further copyleft obligation."
    echo
    echo "WRITTEN OFFER (GPL-2.0 section 3b)"
    echo "----------------------------------"
    echo "For three (3) years from the date you received this package, the asm-test"
    echo "maintainers offer to give any third party, for no more than the cost of"
    echo "physically performing source distribution, the complete corresponding"
    echo "machine-readable source code for the GPL-2.0 components listed above, at the"
    echo "exact versions shown. Request it by opening an issue at"
    echo "https://github.com/wilvk/asm-test . The same source is attached to the"
    echo "matching GitHub release as corresponding-source-*.tar.gz (assembled by"
    echo "scripts/fetch-corresponding-source.sh) and is the upstream release tagged"
    echo "with the version shown for each component:"
    echo "  Unicorn   https://github.com/unicorn-engine/unicorn"
    echo "  Keystone  https://github.com/keystone-engine/keystone"
    echo "  Capstone  https://github.com/capstone-engine/capstone"
} >> "$notice"

echo "$prog: wrote $dest (see NOTICE)"
