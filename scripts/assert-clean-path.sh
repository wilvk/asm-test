#!/bin/sh
# assert-clean-path.sh <resolved-native-lib-path> [install-prefix]
#
# The clean-room guard. Exit 0 if <resolved-native-lib-path> is a legitimately
# bundled resolution; exit non-zero with a "LEAK:" reason if the load resolved
# through one of the vectors a fresh "no ASMTEST_LIB" install must NEVER satisfy
# a load through:
#   - the dev checkout (its build/ tree or any in-repo copy)  [ASMTEST_REPO_ROOT]
#   - a Homebrew dylib   (/opt/homebrew, $HOMEBREW_PREFIX)
#   - /usr/local         (Intel Homebrew + the /usr/local/lib dyld fallback)
#
# If [install-prefix] is given, the path must ALSO live under it — or under a
# system temp dir, since the Java jar extracts its bundled payload to one before
# loading. Used by scripts/clean-room-test.sh and directly as the guard's own
# sanity-check (feed it a build/ path and confirm it is rejected).
#
# See docs/plans/macos-clean-test-plan.md (Track A).

p=$1
prefix=$2

case "$p" in
    "" | unavailable* )
        echo "LEAK?: no library path resolved ('$p') — the loader reported nothing" >&2
        exit 2 ;;
esac

# Absolute, symlink-resolved form (portable: no dependency on realpath(1), which
# is absent on older macOS). Runs in the command-substitution subshell, so the
# cd never affects this script's own cwd.
rp=$(
    d=$(dirname "$p")
    b=$(basename "$p")
    if cd "$d" 2>/dev/null; then printf '%s/%s' "$(pwd -P)" "$b"; else printf '%s' "$p"; fi
)

fail() { echo "LEAK: $1 -> $rp" >&2; exit 1; }

# 1. Inside the dev checkout (covers build/ and any in-tree copy). A clean install
#    lives in a throwaway prefix OUTSIDE the checkout, so this is never a false
#    positive; a fall-through to the developer's build/ is exactly what it catches.
if [ -n "$ASMTEST_REPO_ROOT" ]; then
    rr=$(cd "$ASMTEST_REPO_ROOT" 2>/dev/null && pwd -P)
    case "$rp" in "$rr"/*) fail "resolved inside the dev checkout ($rr)";; esac
else
    case "$rp" in */build/*) fail "resolved through a build/ tree";; esac
fi

# 2. Homebrew.
case "$rp" in
    /opt/homebrew/*) fail "resolved through Homebrew (/opt/homebrew)";;
esac
if [ -n "$HOMEBREW_PREFIX" ]; then
    hb=$(cd "$HOMEBREW_PREFIX" 2>/dev/null && pwd -P)
    [ -n "$hb" ] && case "$rp" in "$hb"/*) fail "resolved through Homebrew ($hb)";; esac
fi

# 3. /usr/local (Intel Homebrew prefix + the /usr/local/lib dyld fallback dir).
case "$rp" in
    /usr/local/*) fail "resolved through /usr/local";;
esac

# 4. Positive check: under the install prefix, or a system temp dir (Java's jar
#    extracts its bundled payload to one before loading).
if [ -n "$prefix" ]; then
    pp=$(cd "$prefix" 2>/dev/null && pwd -P)
    case "$rp" in
        "$pp"/*) : ;;
        /var/folders/* | /private/var/folders/* | /tmp/* | /private/tmp/* ) : ;;
        "$TMPDIR"* ) : ;;
        *) fail "resolved outside the install prefix ($pp) and not a temp extraction";;
    esac
fi

echo "clean: $rp"
exit 0
