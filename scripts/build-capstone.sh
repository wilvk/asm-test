#!/bin/sh
# build-capstone.sh — build + install the Capstone disassembly engine from source.
#
# Capstone (the disassembler behind the emulator tier's `disas`) is built from a
# pinned release here — exactly the way scripts/build-keystone.sh builds the
# Keystone assembler — so the optional native tiers never depend on a distro/brew
# package, and a published package vendors a known, fixed version. Idempotent: if
# pkg-config already finds capstone, it does nothing. Needs git, cmake, a C
# compiler, and make.
#
# Usage: scripts/build-capstone.sh [version]   (default version below)
set -eu

VERSION="${1:-5.0.1}"
PREFIX="${CAPSTONE_PREFIX:-/usr/local}"
prog=$(basename "$0")
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib-thirdparty.sh"

# Already present? (the in-tree builds find it via pkg-config.)
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists capstone 2>/dev/null; then
    echo "$prog: capstone already installed ($(pkg-config --modversion capstone)), skipping"
    exit 0
fi

# sudo only when not root and available (CI runners are non-root with sudo).
SUDO=""
[ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1 && SUDO="sudo"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

echo "$prog: building capstone $VERSION -> $PREFIX"
# Integrity pin (B5): pin the mutable tag to its immutable commit and assert HEAD.
want=$(tp_digest git-commit capstone "$VERSION") || {
    echo "$prog: ERROR: no pinned commit for capstone $VERSION in the digest manifest" >&2
    echo "$prog:        regenerate it with scripts/refresh-thirdparty-digests.sh" >&2
    exit 1
}
commit="${want#commit:}"
git clone --depth 1 --branch "$VERSION" https://github.com/capstone-engine/capstone.git "$work/capstone"
got=$(cd "$work/capstone" && git rev-parse HEAD)
if [ "$got" != "$commit" ]; then
    echo "$prog: ERROR: capstone $VERSION resolved to $got, expected pinned $commit (moved tag?)" >&2
    exit 1
fi

mkdir -p "$work/capstone/build"
cd "$work/capstone/build"
# Capstone builds every architecture by default, so a plain Release shared build
# covers the guests we decode (x86-64, AArch64, ARM, RISC-V). Skip the tests and
# the cstool CLI — we only need the library + headers + capstone.pc.
#
# Darwin: CMake's default gives the dylib an `@rpath/libcapstone.N.dylib` install
# name, so every consumer linked with a plain `-L$PREFIX/lib -lcapstone` (the
# pkg-config flags the tree uses) aborts at load with "no LC_RPATH's found".
# Bake the absolute install-name directory instead — the way Homebrew ships
# dylibs — so plain links work. A no-op on non-Apple platforms.
cmake -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=ON \
      -DCAPSTONE_BUILD_TESTS=OFF \
      -DCAPSTONE_BUILD_CSTOOL=OFF \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_INSTALL_NAME_DIR="$PREFIX/lib" \
      ..
make "-j$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
$SUDO make install

# Record the license at the exact version built, so packaging can vendor a
# version-matched THIRD-PARTY-LICENSES notice (mirrors build-keystone.sh).
licdir="$PREFIX/share/licenses/capstone-$VERSION"
$SUDO mkdir -p "$licdir"
for f in LICENSE.TXT LICENSE_LLVM.TXT; do
    [ -f "$work/capstone/$f" ] && $SUDO cp "$work/capstone/$f" "$licdir/"
done

# K1 (CI cache): record the exact installed file set — cmake's install manifest
# plus the license dir — so scripts/thirdparty-cache.sh can tar precisely these
# files into a CI cache and restore them next run. No-op unless the caller sets
# ASMTEST_TP_MANIFEST_DIR (CI does; local builds don't need it).
if [ -n "${ASMTEST_TP_MANIFEST_DIR:-}" ]; then
    mkdir -p "$ASMTEST_TP_MANIFEST_DIR"
    { cat install_manifest.txt; echo; find "$licdir" -type f 2>/dev/null; } \
        | sed '/^[[:space:]]*$/d' > "$ASMTEST_TP_MANIFEST_DIR/capstone-$VERSION.list"
fi

# Refresh the loader cache so the freshly installed lib is found (Linux).
command -v ldconfig >/dev/null 2>&1 && $SUDO ldconfig || true

echo "$prog: done"
