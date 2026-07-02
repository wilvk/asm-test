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
check_group "DynamoRIO" \
    'mk/bindings.mk|DR_VERSION \?= ([0-9][0-9.]*)' \
    'mk/docker.mk|DR_VERSION \?= ([0-9][0-9.]*)' \
    '.github/workflows/ci.yml|DR_VERSION: ([0-9][0-9.]*)' \
    'scripts/fetch-dynamorio.sh|DR_VERSION="\$\{DR_VERSION:-([0-9][0-9.]*)\}"' \
    'Dockerfile.drtrace|ARG DR_VERSION=([0-9][0-9.]*)' \
    'Dockerfile.drtrace-lang|ARG DR_VERSION=([0-9][0-9.]*)'

# Keystone (GPL-2.0): the version built/vendored must equal the one whose source
# the release attaches for GPL §3.
check_group "Keystone" \
    'scripts/build-keystone.sh|VERSION="\$\{1:-([0-9][0-9.]*)\}"' \
    'scripts/fetch-corresponding-source.sh|KEYSTONE_VERSION="\$\{KEYSTONE_VERSION:-([0-9][0-9.]*)\}"'

# Capstone (BSD): same corresponding-source consistency.
check_group "Capstone" \
    'scripts/build-capstone.sh|VERSION="\$\{1:-([0-9][0-9.]*)\}"' \
    'scripts/fetch-corresponding-source.sh|CAPSTONE_VERSION="\$\{CAPSTONE_VERSION:-([0-9][0-9.]*)\}"'

if [ "$fail" -ne 0 ]; then
    echo "third-party version drift detected — reconcile the versions above." >&2
    exit 1
fi
echo "OK: all pinned third-party versions are consistent."
