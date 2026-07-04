#!/bin/sh
# refresh-thirdparty-digests.sh — regenerate scripts/third-party-digests.txt (B5).
#
# Run this after bumping a pinned engine version (DR_VERSION in fetch-dynamorio.sh,
# or the version default in build-keystone.sh / build-capstone.sh). It re-reads
# those versions, resolves each git tag to its immutable commit and fetches the
# DynamoRIO release-asset SHA-256 from GitHub, and rewrites the manifest. Review the
# diff before committing — this is the trust anchor, so a surprising change matters.
#
# Needs: git, curl, jq. Network required.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
man="$root/scripts/third-party-digests.txt"

read_ver() { sed -n -E "s/.*$2.*/\1/p" "$root/$1" | head -n1; }

dr_ver=$(read_ver scripts/fetch-dynamorio.sh 'DR_VERSION="\$\{DR_VERSION:-([0-9][0-9.]*)\}"')
ks_ver=$(read_ver scripts/build-keystone.sh  'VERSION="\$\{1:-([0-9][0-9.]*)\}"')
cs_ver=$(read_ver scripts/build-capstone.sh  'VERSION="\$\{1:-([0-9][0-9.]*)\}"')
[ -n "$dr_ver" ] && [ -n "$ks_ver" ] && [ -n "$cs_ver" ] || {
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

ks_commit=$(tag_commit keystone-engine/keystone "$ks_ver")
cs_commit=$(tag_commit capstone-engine/capstone "$cs_ver")
dr_sha=$(dr_digest "$dr_ver")

{
    sed -n '1,/^# Format/p' "$man"                       # keep the header + Format line
    echo "#   tarball-sha256 — SHA-256 of the downloaded release archive (as GitHub records it)"
    echo "#   git-commit     — the immutable commit a version tag must still resolve to"
    printf 'tarball-sha256  dynamorio  %s  %s\n' "$dr_ver" "$dr_sha"
    printf 'git-commit      keystone   %s        commit:%s\n' "$ks_ver" "$ks_commit"
    printf 'git-commit      capstone   %s        commit:%s\n' "$cs_ver" "$cs_commit"
} >"$man.new"
mv "$man.new" "$man"
echo "refresh: wrote $man"
