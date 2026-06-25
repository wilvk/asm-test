#!/bin/sh
# build-keystone.sh — build + install the Keystone assembler engine from source.
#
# Keystone (the assembler behind `make asm-test`) is not packaged by most Linux
# distros — `libkeystone-dev` does not exist on Ubuntu — so the in-line-asm tier
# builds it from a pinned release here. Idempotent: if pkg-config already finds
# keystone, it does nothing. Needs git, cmake, a C++ compiler, and make.
#
# Usage: scripts/build-keystone.sh [version]   (default version below)
set -eu

VERSION="${1:-0.9.2}"
PREFIX="${KEYSTONE_PREFIX:-/usr/local}"
prog=$(basename "$0")

# Already present? (the in-tree builds find it via pkg-config.)
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists keystone 2>/dev/null; then
    echo "$prog: keystone already installed ($(pkg-config --modversion keystone)), skipping"
    exit 0
fi

# sudo only when not root and available (CI runners are non-root with sudo).
SUDO=""
[ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1 && SUDO="sudo"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

echo "$prog: building keystone $VERSION -> $PREFIX"
git clone --depth 1 --branch "$VERSION" https://github.com/keystone-engine/keystone.git "$work/keystone"

mkdir -p "$work/keystone/build"
cd "$work/keystone/build"
cmake -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=ON \
      -DLLVM_TARGETS_TO_BUILD="AArch64;ARM;X86" \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      ..
make "-j$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
$SUDO make install

# Refresh the loader cache so the freshly installed lib is found (Linux).
command -v ldconfig >/dev/null 2>&1 && $SUDO ldconfig || true

echo "$prog: done"
