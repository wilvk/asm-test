#!/bin/bash
# build-manylinux-wheel.sh — build the self-contained manylinux_2_28 asm-test Python
# wheel inside a quay.io/pypa/manylinux_2_28_<arch> container (distribution-packaging.md
# T5). The published Linux wheels must install on distros OLDER than the ubuntu-latest
# build host; manylinux_2_28 (AlmaLinux 8, glibc 2.28, GCC 14) is the recorded floor.
#
# That image ships NONE of the native engines this framework links — dnf/EPEL have no
# unicorn-devel and no libipt-devel — so this script builds the four functional engines
# (unicorn, keystone, capstone, libipt) from the SAME pinned sources the rest of the tree
# uses, at the versions Ubuntu noble's apt provides (unicorn 2.1.4, keystone 0.9.2,
# capstone 5.0.1, libipt 2.0.6). DynamoRIO is a prebuilt Linux tarball (glibc-2.28
# compatible), fetched by the tree's own script. libopencsd is intentionally skipped (see
# the note in build_deps: it is a dead link-only dep today). The result: a manylinux_2_28
# wheel whose tiers behave identically to the apt-built ubuntu-latest wheel, just with a
# much lower glibc floor.
#
# Phases (arg 1):
#   deps  — the expensive, cacheable half: dnf packages + the five source engines + the
#           DynamoRIO fetch. Dockerfile.manylinux-wheel runs this as a cached layer.
#   wheel — `make python-package` + `auditwheel repair` (the fast, iterate-often half).
#   all   — deps then wheel (default; what release.yml's container leg runs top to bottom).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
# shellcheck source=scripts/lib-thirdparty.sh
. "$here/lib-thirdparty.sh"

phase="${1:-all}"

# Pin the DynamoRIO cache to an absolute, layer-cacheable path: the default lives under
# build/, which does not exist yet at `deps` time (before the full tree is COPY'd).
export DR_CACHE="${DR_CACHE:-/opt/dr-cache}"
# manylinux ships its interpreters under /opt/python (no system python3 on PATH); cp312
# matches release.yml. The wheel is py3-none (pure ctypes, no extension), so the exact
# minor version is immaterial to the tag — it just needs `build` + `auditwheel`.
PY=/opt/python/cp312-cp312/bin/python
export PATH="/opt/python/cp312-cp312/bin:/usr/local/bin:$PATH"
export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"

# fetch + verify a pinned GitHub source tarball against third-party-digests.txt, extract
# stripped into /tmp/<name>-src, and echo that dir.
fetch_src() { # <name> <version> <url>
    _name="$1"; _ver="$2"; _url="$3"
    _want=$(tp_digest tarball-sha256 "$_name" "$_ver") || {
        echo "build-manylinux-wheel: no pinned digest for $_name $_ver" >&2; exit 1; }
    _tmp="/tmp/$_name.tar.gz"; _dst="/tmp/$_name-src"
    curl -fsSL "$_url" -o "$_tmp"
    _got="sha256:$(tp_sha256 "$_tmp")"
    [ "$_got" = "$_want" ] || {
        echo "build-manylinux-wheel: $_name digest mismatch: $_got != $_want" >&2; exit 1; }
    rm -rf "$_dst"; mkdir -p "$_dst"; tar -xzf "$_tmp" -C "$_dst" --strip-components=1; rm -f "$_tmp"
    echo "$_dst"
}

build_deps() {
    # gcc-c++ (unicorn is C++), pkgconf (the tree probes unicorn via pkg-config), patchelf
    # (make package-libs rpaths the vendored deps). cmake/make/git/curl already ship in the
    # manylinux image.
    dnf -y install epel-release
    dnf -y install gcc-c++ pkgconf-pkg-config patchelf

    # keystone 0.9.2 bundles a 2016-era LLVM whose CMakeLists both declares
    # cmake_minimum_required < 3.5 AND sets removed policies to OLD (CMP0051) — BOTH are
    # hard errors under the manylinux image's CMake 4.x. Pin a 3.x cmake (which still
    # honors those) ahead of it on PATH for every engine build; pip's cmake wheel is
    # glibc-2.17-based, so it runs fine on this AlmaLinux 8 (and never ships in the wheel).
    "$PY" -m pip install --quiet "cmake==3.27.9"
    command -v cmake; cmake --version | head -1

    # Force CMAKE_INSTALL_LIBDIR=lib on every cmake engine: AlmaLinux's cmake defaults to
    # lib64, but the framework link lines and PKG_CONFIG_PATH below expect /usr/local/lib.

    # 1. Unicorn 2.1.4 (emulator tier) — installs libunicorn.so + unicorn.pc.
    d=$(fetch_src unicorn 2.1.4 "https://github.com/unicorn-engine/unicorn/archive/refs/tags/2.1.4.tar.gz")
    cmake -S "$d" -B "$d/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=ON -DUNICORN_BUILD_TESTS=OFF
    cmake --build "$d/build" -j"$(nproc)"
    cmake --install "$d/build"

    # 2/3. Keystone + Capstone — the tree's own pinned (git-commit) source builders.
    "$here/build-keystone.sh"
    "$here/build-capstone.sh"

    # 4. libipt 2.0.6 (Intel PT decode) — installs intel-pt.h on the default include path
    #    + libipt.so, which is exactly how the hwtrace tier probes/links it (-lipt).
    d=$(fetch_src libipt 2.0.6 "https://github.com/intel/libipt/archive/refs/tags/v2.0.6.tar.gz")
    cmake -S "$d" -B "$d/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DCMAKE_INSTALL_LIBDIR=lib -DPTUNIT=OFF -DPTDUMP=OFF -DPTXED=OFF
    cmake --build "$d/build" -j"$(nproc)"
    cmake --install "$d/build"

    # NOTE — libopencsd (ARM CoreSight decode) is deliberately NOT built here. It is a
    # LINK-only dependency in the current tree with no functional effect: src/cs_backend.c
    # references zero OpenCSD symbols and includes no OpenCSD header — the live decode tree
    # is board-gated and unwritten (coresight-live-decode.md T2-T5), so
    # asmtest_cs_decoder_present() returns 0 whether or not -DASMTEST_HAVE_OPENCSD is set.
    # The CoreSight tier therefore self-skips IDENTICALLY with or without libopencsd, so
    # omitting it here is not a capability gap vs the apt-built ubuntu wheel (which links a
    # never-called -lopencsd_c_api). AlmaLinux 8 has no opencsd dnf package and upstream
    # OpenCSD ships no pkg-config .pc (Debian's libopencsd.pc is a packaging add-on), so a
    # source build here would be link-cosmetic only. When the live decode lands (needing a
    # real CoreSight board) this build should add a source-built libopencsd + a hand-written
    # .pc; until then, matching the tree's actual behaviour beats matching a dead link flag.

    # keystone/capstone (built via the shared scripts with no LIBDIR override) land in
    # /usr/local/lib64 on AlmaLinux; consolidate into /usr/local/lib so the tree's
    # -L/usr/local/lib link lines and pkg-config both resolve them (unicorn/libipt already
    # forced lib above).
    if [ -d /usr/local/lib64 ]; then cp -an /usr/local/lib64/. /usr/local/lib/ 2>/dev/null || true; fi
    # Register /usr/local/lib in the linker cache so `auditwheel repair` can locate
    # libunicorn.so.* (the emu lib's DT_NEEDED) to vendor it into the wheel.
    echo /usr/local/lib > /etc/ld.so.conf.d/asmtest-local.conf
    ldconfig || true
    echo "build-manylinux-wheel: engines installed:"
    for e in unicorn keystone capstone; do
        pkg-config --exists "$e" && echo "  $e $(pkg-config --modversion "$e")" || echo "  $e: pkg-config MISSING"
    done
    "${CC:-cc}" -E -include intel-pt.h -xc /dev/null >/dev/null 2>&1 \
        && echo "  libipt header OK" || echo "  libipt header MISSING"

    # 6. DynamoRIO (drtrace tier) — the tree's pinned fetch into the cacheable DR_CACHE.
    "$here/fetch-dynamorio.sh" >/dev/null
    echo "build-manylinux-wheel: DynamoRIO staged in $DR_CACHE"
}

build_wheel() {
    "$PY" -m pip install --quiet --upgrade build auditwheel
    make -C "$root" python-package
    rm -rf "$root/wheelhouse"; mkdir -p "$root/wheelhouse"
    # Mirror release.yml's exclude list exactly: the native-trace tier libs are already
    # self-contained ($ORIGIN rpath, libdynamorio co-located) and dlopen'd at run time, so
    # auditwheel must NOT relocate/rename them — renaming libdynamorio breaks drapp's
    # dladdr sibling lookup — nor fail the manylinux policy on the prebuilt DynamoRIO lib.
    # --plat pins the recorded floor (manylinux_2_28): building ON glibc 2.28 means the
    # wheel can never need newer, so this always succeeds, and it stops auditwheel from
    # auto-assigning a different tag (e.g. a lower 2_17 that would slip the tag check).
    "$PY" -m auditwheel repair --plat "manylinux_2_28_$(uname -m)" \
        --exclude libasmtest_hwtrace.so --exclude libasmtest_drapp.so \
        --exclude libasmtest_drclient.so --exclude libdynamorio.so \
        "$root"/build/dist/python/*.whl -w "$root/wheelhouse"
    echo "build-manylinux-wheel: repaired wheel(s):"
    ls -l "$root/wheelhouse"
}

case "$phase" in
    deps)  build_deps ;;
    wheel) build_wheel ;;
    all)   build_deps; build_wheel ;;
    *) echo "usage: $0 [deps|wheel|all]" >&2; exit 2 ;;
esac
