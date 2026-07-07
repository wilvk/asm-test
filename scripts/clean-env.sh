#!/bin/sh
# clean-env.sh — SOURCE this before a fresh-install smoke to make "installed
# clean, no override" mean what it says. It hardens the current shell so the
# ONLY thing that can satisfy a native-library load is the bundled payload of
# the package under test — never a leaked dev build/ tree, a Homebrew dylib, or
# an ASMTEST_* / DYLD_* / LD_* override.
#
#   . scripts/clean-env.sh && <install-and-smoke-that-prints-the-resolved-path>
#
# Used by `make clean-room-test` (scripts/clean-room-test.sh) and intended for
# the release.yml per-binding smokes. Portable across macOS and Linux; POSIX sh.
# See docs/internal/plans/macos-clean-test-plan.md (Track A).

# 1. Native-library / manifest overrides: none may pre-satisfy the load.
unset ASMTEST_LIB ASMTEST_MANIFEST ASMTEST_CORPUS_LIB \
      ASMTEST_HWTRACE_LIB ASMTEST_DRAPP_LIB ASMTEST_DR_LIB \
      ASMTEST_DRCLIENT DYNAMORIO_HOME 2>/dev/null || true

# 2. Dynamic-loader search overrides.
#    macOS: DYLD_FALLBACK_LIBRARY_PATH, when UNSET, reverts to dyld's built-in
#    default `$HOME/lib:/usr/local/lib:/lib:/usr/lib` — which INCLUDES
#    /usr/local/lib, where a Homebrew libunicorn / libasmtest could still satisfy
#    a bare-leaf-name load. Unsetting is therefore NOT enough; pin it to the
#    system dir so only the package's own absolute / @loader_path payload resolves.
unset DYLD_LIBRARY_PATH DYLD_INSERT_LIBRARIES 2>/dev/null || true
DYLD_FALLBACK_LIBRARY_PATH=/usr/lib
export DYLD_FALLBACK_LIBRARY_PATH
#    Linux counterparts (unset restores the compiled-in default, which is fine).
unset LD_LIBRARY_PATH LD_PRELOAD 2>/dev/null || true

# 3. PATH: drop Homebrew and /usr/local so no brew tool — nor its lib dir via a
#    PATH-adjacent search — can leak in. Callers that need an interpreter must
#    invoke it by the absolute path they resolved BEFORE sourcing this (that is
#    exactly what scripts/clean-room-test.sh does).
_ce_path=""
_ce_oldifs=$IFS
IFS=:
for _ce_d in $PATH; do
    case "$_ce_d" in
        /opt/homebrew/*|/usr/local/bin|/usr/local/sbin|*/homebrew/*|"") : ;; # drop
        *) _ce_path="${_ce_path:+$_ce_path:}$_ce_d" ;;
    esac
done
IFS=$_ce_oldifs
PATH="$_ce_path"
export PATH
unset _ce_path _ce_oldifs _ce_d

# 4. Work outside any checkout so a _REPO_ROOT/build/ fall-through cannot resolve.
_ce_tmp="$(mktemp -d 2>/dev/null || mktemp -d -t asmtest-cleanroom)"
if [ -n "$_ce_tmp" ] && cd "$_ce_tmp"; then
    :
else
    echo "clean-env: WARNING could not cd to a scratch dir; cwd unchanged" >&2
fi
unset _ce_tmp

echo "clean-env: scrubbed ASMTEST_*/DYLD_*/LD_*; DYLD_FALLBACK_LIBRARY_PATH=$DYLD_FALLBACK_LIBRARY_PATH; cwd=$(pwd)"
