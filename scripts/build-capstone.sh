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
git clone --depth 1 --branch "$VERSION" https://github.com/capstone-engine/capstone.git "$work/capstone"

mkdir -p "$work/capstone/build"
cd "$work/capstone/build"
# Capstone builds every architecture by default, so a plain Release shared build
# covers the guests we decode (x86-64, AArch64, ARM, RISC-V). Skip the tests and
# the cstool CLI — we only need the library + headers + capstone.pc.
cmake -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=ON \
      -DCAPSTONE_BUILD_TESTS=OFF \
      -DCAPSTONE_BUILD_CSTOOL=OFF \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
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

# Refresh the loader cache so the freshly installed lib is found (Linux).
command -v ldconfig >/dev/null 2>&1 && $SUDO ldconfig || true

echo "$prog: done"
