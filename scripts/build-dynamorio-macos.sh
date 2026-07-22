#!/bin/sh
# build-dynamorio-macos.sh — build libdynamorio.dylib from the pinned source fork
# for the macOS x86-64 drtrace tier (macos-dynamorio-fork-build.md FB1).
#
# DynamoRIO has never published a macOS release asset (all 455 releases scanned,
# 0 Mac/Darwin/dylib files), so unlike the Linux tier — which fetches the pinned
# release tarball via scripts/fetch-dynamorio.sh — the macOS tier builds a pinned
# GIT-COMMIT of the wilvk/dynamorio source fork, exactly the way
# scripts/fetch-libdft.sh pins libdft64: a moved branch cannot swap the runtime
# out from under the manifest. Darwin-x86-64-only: everywhere else this prints a
# skip and exits 0 (the tier self-skips; it never hard-fails a Linux build).
#
# The caller-named prefix IS the CMake build tree. Staging a copied subset would
# be unsound: the build-tree cmake config (cmake/DynamoRIOTarget64.cmake) records
# ABSOLUTE paths into the build tree, so a relocated copy would silently resolve
# libraries from wherever the build ran (or break once that dir is cleaned).
# Upstream supports pointing DynamoRIO_DIR at a build dir, and the build tree
# already carries the exact DYNAMORIO_HOME shape the drtrace tier consumes:
# lib64/release/libdynamorio.dylib, cmake/, include/, ext/. Idempotent: a prefix
# already built at the pinned commit is reused.
#
# Prints the resulting DYNAMORIO_HOME on stdout (logs go to stderr) so a
# Makefile / CI step can:  DYNAMORIO_HOME=$(scripts/build-dynamorio-macos.sh)
#
# Usage: scripts/build-dynamorio-macos.sh [prefix]   (default under build/)
# Override DR_FORK_VERSION (the manifest key) / DR_FORK_URL to bump;
# DR_FORK_CACHE to relocate the source checkout.
set -eu

DR_FORK_VERSION="${DR_FORK_VERSION:-bbbcc40b8}"
DR_FORK_URL="${DR_FORK_URL:-https://github.com/wilvk/dynamorio.git}"
prog=$(basename "$0")
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib-thirdparty.sh"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DR_FORK_CACHE="${DR_FORK_CACHE:-$root/build/dynamorio-fork}"
src="$DR_FORK_CACHE/dynamorio"
# Build flavor (macos-dynamorio-signal-chaining.md SC2): DR_MACOS_BUILD_TYPE=Debug
# opt-in builds a DEBUG+INTERNAL runtime — DR internal asserts print file:line
# where a release build executes a silent ud2 trap — into a separate default
# prefix. The pinned release flow is byte-identical when the variable is unset.
DR_MACOS_BUILD_TYPE="${DR_MACOS_BUILD_TYPE:-Release}"
case "$DR_MACOS_BUILD_TYPE" in
Release)
    cmake_flavor="-DDEBUG=OFF"
    libsub="release"
    default_prefix="$root/build/dynamorio-macos"
    ;;
Debug)
    cmake_flavor="-DDEBUG=ON -DINTERNAL=ON"
    libsub="debug"
    default_prefix="$root/build/dynamorio-macos-debug"
    ;;
*)
    echo "$prog: ERROR: DR_MACOS_BUILD_TYPE must be Release or Debug (got $DR_MACOS_BUILD_TYPE)" >&2
    exit 1
    ;;
esac
prefix="${1:-$default_prefix}"

log() { echo "$prog: $*" >&2; }

# Darwin-x86-64 gate. The product is a Mach-O dylib no Linux/Docker lane can
# run, so the substrate is a macOS x86-64 host (the arm64 DR port is upstream
# i#5383, open); everywhere else the tier self-skips.
os=$(uname -s)
arch=$(uname -m)
if [ "$os" != "Darwin" ]; then
    log "SKIP: Darwin-only (host is $os) — the macOS drtrace tier builds a Mach-O dylib"
    exit 0
fi
if [ "$arch" != "x86_64" ]; then
    log "SKIP: x86-64-only (host is $arch) — upstream arm64 macOS DR is an open port (i#5383)"
    exit 0
fi

# Integrity pin (B5): refuse any checkout whose HEAD is not the manifest commit.
want=$(tp_digest git-commit dynamorio-fork "$DR_FORK_VERSION") || {
    log "ERROR: no pinned commit for dynamorio-fork $DR_FORK_VERSION in $TP_MANIFEST"
    log "       (add one via scripts/refresh-thirdparty-digests.sh — refusing an unpinned build)"
    exit 1
}
commit="${want#commit:}"

stamp="$prefix/.asmtest-dr-fork-commit"
dylib="$prefix/lib64/$libsub/libdynamorio.dylib"
if [ -f "$dylib" ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$commit" ]; then
    log "reusing $prefix (built at $commit)"
    echo "$prefix"
    exit 0
fi

# Fetch (or reuse) the pinned commit — fetch-libdft.sh's shape: fetch-by-SHA
# with a full-clone fallback, then assert HEAD against the manifest.
if [ -d "$src/.git" ] && [ "$(git -C "$src" rev-parse HEAD 2>/dev/null)" = "$commit" ]; then
    log "reusing cached source $src ($commit)"
else
    log "fetching dynamorio fork $commit"
    rm -rf "$src"
    mkdir -p "$DR_FORK_CACHE"
    if git init -q "$src" \
       && git -C "$src" remote add origin "$DR_FORK_URL" \
       && git -C "$src" fetch -q --depth 1 origin "$commit" \
       && git -C "$src" checkout -q --detach FETCH_HEAD; then
        :
    else
        log "fetch-by-SHA unavailable; falling back to full clone"
        rm -rf "$src"
        git clone -q "$DR_FORK_URL" "$src"
        git -C "$src" checkout -q --detach "$commit"
    fi
    got=$(git -C "$src" rev-parse HEAD)
    if [ "$got" != "$commit" ]; then
        log "ERROR: dynamorio fork integrity check FAILED"
        log "       expected $commit"
        log "       got      $got"
        rm -rf "$src"
        exit 1
    fi
    log "verified dynamorio fork ($got)"
fi
# Required submodules (elfutils/libipt/zlib); idempotent when already present.
git -C "$src" submodule update --init --depth 1 >&2

# Capture the fork's license once, for provenance (BSD-3 + subcomponents; also
# vendored in-repo — this keeps a fresh checkout self-healing, fetch-libdft-style).
lic="$root/licenses/DynamoRIO-fork.txt"
[ -f "$lic" ] || { cp "$src/License.txt" "$lic" && log "captured license -> $(basename "$lic")"; }

# A stale build tree (different commit, or moved source) is wiped, not reused:
# CMakeCache pins absolute source paths and DR does not promise incremental
# correctness across versions.
rm -rf "$prefix"
mkdir -p "$prefix"

log "configuring (cmake $cmake_flavor -DBUILD_TESTS=OFF)"
# shellcheck disable=SC2086 — cmake_flavor is deliberately word-split
cmake -S "$src" -B "$prefix" $cmake_flavor -DBUILD_TESTS=OFF >&2

# dynamorio is the runtime; drmgr/drreg/drx are the BSD-clean extensions the
# asm-test drclient sub-build links (drclient/CMakeLists.txt) — built here so
# the staged home can serve `use_DynamoRIO_extension` too.
jobs=$(sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
log "building dynamorio drmgr drreg drx (-j$jobs)"
cmake --build "$prefix" --target dynamorio drmgr drreg drx -j "$jobs" >&2

[ -f "$dylib" ] || {
    log "ERROR: build completed but $dylib is missing"
    exit 1
}
case $(lipo -info "$dylib" 2>/dev/null) in
*x86_64*) ;;
*)
    log "ERROR: $dylib is not x86_64 ($(lipo -info "$dylib" 2>/dev/null || echo unreadable))"
    exit 1
    ;;
esac

printf '%s\n' "$commit" > "$stamp"
log "done: DYNAMORIO_HOME=$prefix"
echo "$prefix"
