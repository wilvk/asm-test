#!/bin/sh
# check-thirdparty-versions.sh — assert the pinned third-party engine versions
# agree across every place they are declared, so a bump in one file cannot drift
# from the others (repo-review B3/B4). The binaries that get built/bundled and the
# GPL corresponding-source archive attached to a release must reference the SAME
# version; without this gate a mismatch is only discovered when the release
# workflow's packaging steps explode on a real tag.
#
# Run from the repo root: sh scripts/check-thirdparty-versions.sh
set -eu

fail=0

# Print "<label>=<version>" for a single grep hit of an assignment/ARG line.
extract() { # <file> <regex-capturing-the-version>
    sed -n -E "s/.*$2.*/\1/p" "$1" 2>/dev/null | head -n1
}

check_group() { # <name> <expected-nonempty?> then pairs of "file|regex"
    name="$1"; shift
    first=""; firstfile=""
    for spec in "$@"; do
        file="${spec%%|*}"; rx="${spec#*|}"
        got="$(extract "$file" "$rx")"
        if [ -z "$got" ]; then
            echo "  ! $name: could not read version from $file (pattern changed?)"
            fail=1
            continue
        fi
        if [ -z "$first" ]; then
            first="$got"; firstfile="$file"
        elif [ "$got" != "$first" ]; then
            echo "  ! $name: $file has '$got' but $firstfile has '$first'"
            fail=1
        fi
    done
    [ -n "$first" ] && echo "  $name = $first"
}

echo "Checking pinned third-party engine versions are consistent..."

# DynamoRIO (bundled binary + CI test + Dockerfiles + corresponding-source path).
# Every DR image is listed: they all route the fetch through fetch-dynamorio.sh
# (which enforces the manifest digest), and this group keeps their ARG pins from
# drifting apart.
check_group "DynamoRIO" \
    'mk/bindings.mk|DR_VERSION \?= ([0-9][0-9.]*)' \
    'mk/docker.mk|DR_VERSION \?= ([0-9][0-9.]*)' \
    '.github/workflows/ci.yml|DR_VERSION: ([0-9][0-9.]*)' \
    'scripts/fetch-dynamorio.sh|DR_VERSION="\$\{DR_VERSION:-([0-9][0-9.]*)\}"' \
    'Dockerfile.drtrace|ARG DR_VERSION=([0-9][0-9.]*)' \
    'Dockerfile.drtrace-lang|ARG DR_VERSION=([0-9][0-9.]*)' \
    'Dockerfile.drext-probe|ARG DR_VERSION=([0-9][0-9.]*)' \
    'Dockerfile.pintool|ARG DR_VERSION=([0-9][0-9.]*)' \
    'Dockerfile.gcprofiler-probe|ARG DR_VERSION=([0-9][0-9.]*)' \
    'Dockerfile.suspendprof-probe|ARG DR_VERSION=([0-9][0-9.]*)' \
    'Dockerfile.taint-native|ARG DR_VERSION=([0-9][0-9.]*)' \
    'Dockerfile.taint-dotnet|ARG DR_VERSION=([0-9][0-9.]*)' \
    'Dockerfile.taint-attach|ARG DR_VERSION=([0-9][0-9.]*)' \
    'Dockerfile.taint-attach-probe|ARG DR_VERSION=([0-9][0-9.]*)' \
    'Dockerfile.taint-managed-attach-probe|ARG DR_VERSION=([0-9][0-9.]*)' \
    'Dockerfile.taint-oracle|ARG DR_VERSION=([0-9][0-9.]*)'

# manylinux wheel base (K2): the pinned dated tag must agree between the local
# lane's Dockerfile and release.yml's containerised python job — the base that
# builds the published PyPI wheel must never float.
check_group "manylinux_2_28" \
    'Dockerfile.manylinux-wheel|ARG MANYLINUX_TAG=([0-9][0-9.-]*)' \
    '.github/workflows/release.yml|MANYLINUX_TAG=([0-9][0-9.-]*)'

# Keystone (GPL-2.0): the version built/vendored must equal the one whose source
# the release attaches for GPL §3.
check_group "Keystone" \
    'scripts/build-keystone.sh|VERSION="\$\{1:-([0-9][0-9.]*)\}"' \
    'scripts/fetch-corresponding-source.sh|KEYSTONE_VERSION="\$\{KEYSTONE_VERSION:-([0-9][0-9.]*)\}"'

# Capstone (BSD): same corresponding-source consistency.
check_group "Capstone" \
    'scripts/build-capstone.sh|VERSION="\$\{1:-([0-9][0-9.]*)\}"' \
    'scripts/fetch-corresponding-source.sh|CAPSTONE_VERSION="\$\{CAPSTONE_VERSION:-([0-9][0-9.]*)\}"'

# Unicorn (GPL-2.0): the Windows/mingw benchmark producer lane cross-builds it
# from a pinned source archive (Dockerfile.win64). Only one declaration exists
# today, so this just surfaces the version; the manifest check below is the teeth.
check_group "Unicorn" \
    'Dockerfile.win64|ARG UNICORN_VERSION=([0-9][0-9.]*)'

# The integrity manifest (B5) must carry a pinned digest/commit for each version
# above, or a bump would ship unpinned. Assert an entry exists for each declared
# version — a name+version present in scripts/third-party-digests.txt.
manifest="$(dirname "$0")/third-party-digests.txt"
check_manifest() { # <name> <version>
    [ -n "$2" ] || return 0
    if awk -v n="$1" -v v="$2" '!/^[[:space:]]*#/ && $2==n && $3==v {found=1} END{exit !found}' "$manifest"; then
        echo "  manifest: $1 $2 pinned"
    else
        echo "  ! manifest: no pinned digest/commit for $1 $2 in $manifest (run scripts/refresh-thirdparty-digests.sh)"
        fail=1
    fi
}
check_manifest dynamorio "$(extract mk/bindings.mk 'DR_VERSION \?= ([0-9][0-9.]*)')"
check_manifest keystone  "$(extract scripts/build-keystone.sh 'VERSION="\$\{1:-([0-9][0-9.]*)\}"')"
check_manifest capstone  "$(extract scripts/build-capstone.sh 'VERSION="\$\{1:-([0-9][0-9.]*)\}"')"
# The zig toolchain tarball (docker zig lane) is pinned per-arch: both digests must
# anchor the single ZIG_VERSION declared in mk/docker.mk.
check_manifest zig-linux-x86_64  "$(extract mk/docker.mk 'ZIG_VERSION \?= ([0-9][0-9.]*)')"
check_manifest zig-linux-aarch64 "$(extract mk/docker.mk 'ZIG_VERSION \?= ([0-9][0-9.]*)')"
check_manifest unicorn "$(extract Dockerfile.win64 'ARG UNICORN_VERSION=([0-9][0-9.]*)')"

if [ "$fail" -ne 0 ]; then
    echo "third-party version drift detected — reconcile the versions above." >&2
    exit 1
fi
echo "OK: all pinned third-party versions and integrity anchors are consistent."
