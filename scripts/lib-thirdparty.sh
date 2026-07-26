# lib-thirdparty.sh — POSIX helpers for verifying fetched/built third-party engines
# against scripts/third-party-digests.txt (repo-review B5). Sourced, not executed:
#     . "$(dirname "$0")/lib-thirdparty.sh"
# Resolves the manifest relative to this file's directory (override with TP_MANIFEST).

_tp_dir=$(CDPATH= cd -- "$(dirname -- "${0:-.}")" && pwd)
TP_MANIFEST="${TP_MANIFEST:-$_tp_dir/third-party-digests.txt}"

# tp_digest <kind> <name> <version> — echo the pinned "<algo>:<value>", or exit 1
# (with nothing on stdout) when the manifest has no matching line.
tp_digest() {
    awk -v k="$1" -v n="$2" -v v="$3" '
        /^[[:space:]]*#/ || NF < 4 { next }
        $1==k && $2==n && $3==v { print $4; found=1; exit }
        END { if (!found) exit 1 }
    ' "$TP_MANIFEST" 2>/dev/null
}

# tp_cmake_compat — echo the extra cmake argument a MODERN cmake needs in order
# to configure our pinned engine sources, or nothing.
#
# Capstone 5.0.1 and Keystone 0.9.2 both declare a cmake_minimum_required() below
# 3.5. CMake 4.0 REMOVED compatibility with those, so a stock cmake >= 4 refuses
# to configure either one:
#
#   CMake Error at CMakeLists.txt:4 (cmake_minimum_required):
#     Compatibility with CMake < 3.5 has been removed from CMake.
#
# Bumping the engines' own CMakeLists is not available to us: both are pinned to
# an immutable upstream commit (B5) and verified against it before configure, so
# editing the tree would defeat the check that makes the build trustworthy. The
# supported escape hatch is CMAKE_POLICY_VERSION_MINIMUM, which tells cmake to
# configure as though the project had asked for 3.5 — it changes only policy
# defaults, not the pinned sources.
#
# Emitted only for cmake >= 3.31, where the variable was introduced; on older
# cmake it is both unnecessary and unrecognised (a "manually-specified variables
# were not used" warning we would rather not print).
tp_cmake_compat() {
    _tp_ver=$(cmake --version 2>/dev/null | sed -n '1s/.*version \([0-9][0-9.]*\).*/\1/p')
    [ -n "${_tp_ver:-}" ] || return 0
    _tp_maj=${_tp_ver%%.*}
    _tp_rest=${_tp_ver#*.}
    _tp_min=${_tp_rest%%.*}
    case "$_tp_min" in *[!0-9]*|'') _tp_min=0 ;; esac
    if [ "$_tp_maj" -gt 3 ] || { [ "$_tp_maj" -eq 3 ] && [ "$_tp_min" -ge 31 ]; }; then
        echo "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
    fi
}

# tp_sha256 <file> — print the file's raw SHA-256 hex, portably (Linux/macOS).
tp_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

# tp_fetch_lock <lockdir> <witness> — serialize concurrent fetches of one cache.
#
# Why this exists: mk/desktop.mk declares ImGui's six sources with a GROUPED
# target (`&:`) precisely so one fetch produces all of them. Grouped targets
# need GNU make 4.3+, and Apple ships 3.81 as /usr/bin/make — where the syntax
# silently degrades to six INDEPENDENT targets carrying the same recipe. A
# parallel build then runs six copies of the fetch at once, they share one
# scratch path, and they clobber each other's download: the loser hashes a file
# another process already deleted and reports it as an integrity failure. The
# digest check is doing its job; the input was just corrupted locally.
#
# Returns 0 = lock held, the caller must fetch and then `rmdir` the lockdir.
# Returns 1 = another process published <witness>; reuse it. That is the ONLY
#             meaning of 1, because both callers turn it straight into "reusing
#             cached" — so reporting it for any other reason makes them print a
#             cache hit and then die on their own missing-file check, naming the
#             wrong cause. Every other failure aborts here instead, loudly.
# mkdir is the mutex because it is atomic on every POSIX filesystem and needs no
# flock(1), which macOS does not ship.
tp_fetch_lock() {
    _lock="$1"
    _witness="$2"
    _waited=0
    _absent=0
    while ! _err=$(mkdir "$_lock" 2>&1); do
        # mkdir failing is not one condition but two, and they want opposite
        # responses: EEXIST means someone holds the lock and waiting is right,
        # while an unwritable cache dir, a full disk or a stray FILE at the lock
        # path never heals no matter how long we wait.
        #
        # "Is the directory there?" cannot separate them on its own — the holder
        # can release between our mkdir and the test, leaving us looking at a
        # hard error that is really just a lost race. What DOES separate them is
        # persistence: the race resolves on the next attempt, the broken path
        # repeats forever. So retry immediately and let repetition decide.
        if [ -d "$_lock" ]; then
            _absent=0 # ordinary contention
        else
            _absent=$((_absent + 1))
            if [ "$_absent" -ge 3 ]; then
                echo "tp_fetch_lock: cannot create $_lock: $_err" >&2
                exit 1
            fi
            continue # a released lock: try again at once, spend no budget
        fi
        if [ -e "$_witness" ]; then
            return 1
        fi
        _waited=$((_waited + 1))
        if [ "$_waited" -gt 300 ]; then
            # A crashed holder (SIGKILL, OOM, a closed laptop) leaves the
            # directory behind forever. Break the lock rather than hang a build
            # on one nobody holds.
            echo "tp_fetch_lock: stale lock $_lock (waited ${_waited}s) — taking it" >&2
            rm -rf "$_lock"
            # rm+mkdir is NOT atomic, so several waiters can reach here at once
            # and only one wins. Losing does not mean the fetch is done — it
            # means another waiter is now a LIVE holder, so serve out a fresh
            # budget waiting on IT. (Returning 1 here was the bug: it claimed a
            # cache hit that did not exist and killed every loser.)
            if mkdir "$_lock" 2>/dev/null; then
                break
            fi
            _waited=0
        fi
        sleep 1
    done
    # We hold it. Someone may have finished between our last check and the mkdir.
    [ -e "$_witness" ] && { rmdir "$_lock" 2>/dev/null || :; return 1; }
    return 0
}
