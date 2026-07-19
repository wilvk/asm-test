#!/bin/sh
# refresh-thirdparty-digests.sh — regenerate scripts/third-party-digests.txt (B5).
#
# Run this after bumping a pinned engine version (DR_VERSION in fetch-dynamorio.sh,
# PIN_VERSION in fetch-pin.sh, ZIG_VERSION in mk/docker.mk, or the version default
# in build-keystone.sh / build-capstone.sh). It re-reads those versions, resolves
# each git tag to its immutable commit, fetches the DynamoRIO release-asset SHA-256
# from GitHub, hashes the Pin and zig tarballs locally (Intel/ziglang.org host them
# directly — no GitHub API), and rewrites the manifest **in full** — every pinned
# component must be re-read here, or a regen silently drops whatever this script
# doesn't know about. Review the diff before committing — this is the trust anchor,
# so a surprising change matters.
#
# Needs: git, curl, jq. Network required.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
man="$root/scripts/third-party-digests.txt"
. "$root/scripts/lib-thirdparty.sh"

read_ver() { sed -n -E "s/.*$2.*/\1/p" "$root/$1" | head -n1; }

dr_ver=$(read_ver scripts/fetch-dynamorio.sh 'DR_VERSION="\$\{DR_VERSION:-([0-9][0-9.]*)\}"')
pin_ver=$(read_ver scripts/fetch-pin.sh      'PIN_VERSION="\$\{PIN_VERSION:-([0-9][0-9A-Za-z.-]*)\}"')
sde_ver=$(read_ver scripts/fetch-sde.sh      'SDE_VERSION="\$\{SDE_VERSION:-([0-9][0-9.]*)\}"')
ks_ver=$(read_ver scripts/build-keystone.sh  'VERSION="\$\{1:-([0-9][0-9.]*)\}"')
cs_ver=$(read_ver scripts/build-capstone.sh  'VERSION="\$\{1:-([0-9][0-9.]*)\}"')
zig_ver=$(read_ver mk/docker.mk              'ZIG_VERSION \?= ([0-9][0-9.]*)')
[ -n "$dr_ver" ] && [ -n "$pin_ver" ] && [ -n "$sde_ver" ] && [ -n "$ks_ver" ] && [ -n "$cs_ver" ] && [ -n "$zig_ver" ] || {
    echo "refresh: could not read one of the pinned versions (patterns changed?)" >&2; exit 1; }

# Immutable commit a tag resolves to (peeled ^{} for annotated tags).
tag_commit() { # <repo> <tag>
    c=$(git ls-remote "https://github.com/$1.git" "refs/tags/$2^{}" 2>/dev/null | cut -f1)
    [ -n "$c" ] || c=$(git ls-remote "https://github.com/$1.git" "refs/tags/$2" 2>/dev/null | cut -f1)
    [ -n "$c" ] || { echo "refresh: cannot resolve $1@$2" >&2; exit 1; }
    echo "$c"
}

# DynamoRIO release-asset SHA-256, as GitHub records it.
dr_digest() { # <version>
    api="https://api.github.com/repos/DynamoRIO/dynamorio/releases/tags/cronbuild-$1"
    d=$(curl -fsSL "$api" | jq -r '.assets[] | select(.name=="DynamoRIO-Linux-'"$1"'.tar.gz") | .digest')
    [ -n "$d" ] && [ "$d" != "null" ] || { echo "refresh: cannot read dynamorio $1 asset digest" >&2; exit 1; }
    echo "$d"
}

# Pin release-asset SHA-256. Intel hosts the tarball directly and publishes no
# official hash (unlike the GitHub-hosted releases above), so this downloads it
# and hashes it locally with the same tp_sha256 the fetch scripts use.
pin_digest() { # <version>
    url="https://software.intel.com/sites/landingpage/pintool/downloads/pin-external-$1-gcc-linux.tar.gz"
    tmp=$(mktemp)
    curl -fsSL "$url" -o "$tmp" || { rm -f "$tmp"; echo "refresh: cannot download pin $1 tarball" >&2; exit 1; }
    d="sha256:$(tp_sha256 "$tmp")"
    rm -f "$tmp"
    echo "$d"
}

# Intel SDE tarball SHA-256. The mirror id + build date live in fetch-sde.sh's
# URL default (release-specific — Intel's mirror has no version->URL formula), so
# read that template and expand ${SDE_VERSION} against the pinned version, then
# download + hash locally with the same tp_sha256 the fetch script uses.
sde_digest() { # <version>
    tmpl=$(sed -n -E 's/^SDE_URL="\$\{SDE_URL:-(.*)\}"$/\1/p' "$root/scripts/fetch-sde.sh" | head -n1)
    [ -n "$tmpl" ] || { echo "refresh: cannot read SDE_URL template from fetch-sde.sh" >&2; exit 1; }
    url=$(SDE_VERSION="$1" eval "printf '%s' \"$tmpl\"")
    tmp=$(mktemp)
    curl -fsSL "$url" -o "$tmp" || { rm -f "$tmp"; echo "refresh: cannot download intel-sde $1 tarball ($url)" >&2; exit 1; }
    d="sha256:$(tp_sha256 "$tmp")"
    rm -f "$tmp"
    echo "$d"
}

# zig release tarball SHA-256, per architecture (ziglang.org hosts directly, no API).
zig_digest() { # <version> <arch>
    url="https://ziglang.org/download/$1/zig-linux-$2-$1.tar.xz"
    tmp=$(mktemp)
    curl -fsSL "$url" -o "$tmp" || { rm -f "$tmp"; echo "refresh: cannot download zig $1 ($2) tarball" >&2; exit 1; }
    d="sha256:$(tp_sha256 "$tmp")"
    rm -f "$tmp"
    echo "$d"
}

ks_commit=$(tag_commit keystone-engine/keystone "$ks_ver")
cs_commit=$(tag_commit capstone-engine/capstone "$cs_ver")
dr_sha=$(dr_digest "$dr_ver")
pin_sha=$(pin_digest "$pin_ver")
sde_sha=$(sde_digest "$sde_ver")
zig_x86_64_sha=$(zig_digest "$zig_ver" x86_64)
zig_aarch64_sha=$(zig_digest "$zig_ver" aarch64)

{
    sed -n '1,/^# Format/p' "$man"                       # keep the header + Format line
    echo "#   tarball-sha256 — SHA-256 of the downloaded release archive (as GitHub records it)"
    echo "#   git-commit     — the immutable commit a version tag must still resolve to"
    printf 'tarball-sha256  dynamorio  %s  %s\n' "$dr_ver" "$dr_sha"
    printf 'tarball-sha256  pin        %s  %s\n' "$pin_ver" "$pin_sha"
    printf 'tarball-sha256  intel-sde  %s  %s\n' "$sde_ver" "$sde_sha"
    printf 'git-commit      keystone   %s        commit:%s\n' "$ks_ver" "$ks_commit"
    printf 'git-commit      capstone   %s        commit:%s\n' "$cs_ver" "$cs_commit"
    printf 'tarball-sha256  zig-linux-x86_64   %s  %s\n' "$zig_ver" "$zig_x86_64_sha"
    printf 'tarball-sha256  zig-linux-aarch64  %s  %s\n' "$zig_ver" "$zig_aarch64_sha"
} >"$man.new"
mv "$man.new" "$man"
echo "refresh: wrote $man"
