#!/bin/sh
# thirdparty-cache.sh — K1 (2026-07-04 repo review): CI cache wrapper for the
# pinned Keystone/Capstone source builds. Keystone is a trimmed LLVM, so the
# uncached builds cost multiple minutes in ~20 jobs per push; both engines are
# version-pinned (scripts/third-party-digests.txt), so the exact installed file
# set can be cached and restored byte-identically.
#
# How: scripts/build-<comp>.sh records cmake's install_manifest.txt (+ the
# license dir) when ASMTEST_TP_MANIFEST_DIR is set. `save` tars EXACTLY those
# files (relative to /); `restore` untars them back into / and refreshes the
# loader cache, after which the build script's pkg-config idempotence check
# makes the from-source build a no-op. Nothing about prefixes, rpaths, or
# install names changes versus an uncached run — the restored files are the
# ones a previous run installed.
#
# Usage (CI pairs this with actions/cache on $ASMTEST_TP_CACHE_DIR/<comp>):
#   scripts/thirdparty-cache.sh cached-build <keystone|capstone>...
#       restore -> ./scripts/build-<comp>.sh -> save, per component
#   scripts/thirdparty-cache.sh restore <comp>...   |   ... save <comp>...
#
# NON-FATAL BY DESIGN: cache mechanics never fail the build — a cold cache is
# a plain from-source build, a broken tarball falls back to building, a failed
# save only warns. Only the underlying build script's own failure propagates.
set -u

prog=$(basename "$0")
REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
CACHE_ROOT=${ASMTEST_TP_CACHE_DIR:-$HOME/.cache/asmtest-thirdparty}

# sudo only when not root and available (CI runners are non-root with sudo) —
# mirrors scripts/build-keystone.sh / build-capstone.sh.
SUDO=""
[ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1 && SUDO="sudo"

usage() {
    echo "usage: $prog <restore|save|cached-build> <keystone|capstone>..." >&2
    exit 2
}

restore_one() {
    _tarball="$CACHE_ROOT/$1/$1.tar.gz"
    if [ ! -f "$_tarball" ]; then
        echo "$prog: $1: cache cold ($_tarball absent) — building from source"
        return 0
    fi
    if $SUDO tar -xzpf "$_tarball" -C /; then
        command -v ldconfig >/dev/null 2>&1 && $SUDO ldconfig || true
        echo "$prog: $1: restored the pinned install from cache"
    else
        echo "$prog: WARNING: $1: cache restore failed — building from source" >&2
    fi
    return 0
}

save_one() {
    _dir="$CACHE_ROOT/$1"
    _tarball="$_dir/$1.tar.gz"
    if [ -f "$_tarball" ]; then
        echo "$prog: $1: cache tarball already present (restored); nothing to save"
        return 0
    fi
    _list=$(mktemp)
    # Manifests hold absolute paths; store them /-relative so create and
    # extract both anchor at -C / (portable across GNU tar and bsdtar).
    cat "$_dir/manifests/"*.list 2>/dev/null \
        | sed -e '/^[[:space:]]*$/d' -e 's|^/||' > "$_list"
    if [ ! -s "$_list" ]; then
        echo "$prog: $1: no install manifest (build skipped or failed) — nothing to cache"
        rm -f "$_list"
        return 0
    fi
    mkdir -p "$_dir"
    if tar -czf "$_tarball" -C / -T "$_list"; then
        echo "$prog: $1: cached $(wc -l < "$_list" | tr -d ' ') installed files"
    else
        echo "$prog: WARNING: $1: cache save failed — removing the partial tarball" >&2
        rm -f "$_tarball"
    fi
    rm -f "$_list"
    return 0
}

cmd=${1:-}
[ $# -ge 2 ] || usage
shift

for comp in "$@"; do
    [ -x "$REPO/scripts/build-$comp.sh" ] || {
        echo "$prog: unknown component '$comp' (no scripts/build-$comp.sh)" >&2; exit 2; }
done

rc=0
for comp in "$@"; do
    case "$cmd" in
        restore) restore_one "$comp" ;;
        save)    save_one "$comp" ;;
        cached-build)
            restore_one "$comp"
            # The build script self-skips (pkg-config) when the restore already
            # satisfied it; its real failures DO propagate.
            ASMTEST_TP_MANIFEST_DIR="$CACHE_ROOT/$comp/manifests" \
                "$REPO/scripts/build-$comp.sh" || { rc=$?; break; }
            save_one "$comp"
            ;;
        *) usage ;;
    esac
done
exit $rc
