#!/bin/sh
# verify-macho.sh — static Mach-O assertions over a collected darwin payload slot.
#
# Track B of the macOS clean-test plan: catch the most common macOS packaging
# regressions — a wrong/missing arch slice or a leaked absolute install-name/dep —
# at build time, on the Linux release collector, with no macOS needed. The macOS-side
# scripts/package-native.sh sets the install-names with install_name_tool; this is the
# independent cross-check that the collected Mach-O is actually correct.
#
# For every *.dylib in <slot_dir>, assert:
#   1. it carries the slot's expected arch slice (llvm-lipo -archs) — no thin-wrong-arch
#      or silently-missing slice;
#   2. its install-name (LC_ID_DYLIB) is @rpath/@loader_path-relative, and neither it nor
#      any dependency (LC_LOAD_DYLIB) bakes in an absolute /Users, /opt/homebrew, or
#      /usr/local path (a dev-build or Homebrew leak). System libs (/usr/lib, /System) are
#      fine — they exist on every macOS;
#   3. a min-OS load command (LC_BUILD_VERSION / LC_VERSION_MIN_MACOSX) is present — and,
#      when a floor is given, that min-OS is <= the floor (so the lib runs on the oldest
#      macOS the project supports).
#
# Self-skips (exit 0, prints why) when llvm-otool/llvm-lipo are absent, so a dev host
# without LLVM still runs `make package-libs-verify` cleanly. Exits nonzero on any
# violation. Usage: verify-macho.sh <slot_dir> <expected_arch> [min_floor]
set -eu

prog=$(basename "$0")
slot="${1:?usage: $prog <slot_dir> <expected_arch> [min_floor]}"
arch="${2:?usage: $prog <slot_dir> <expected_arch> [min_floor]}"
floor="${3:-}"

# Locate an llvm tool by PATH name, version-suffixed name (llvm-otool-18, as Debian/Ubuntu
# ship it), or under /usr/lib/llvm-*/bin (where the unsuffixed binary actually lives).
find_tool() {
    for c in "$1" $(ls /usr/bin/"$1"-* 2>/dev/null) $(ls /usr/lib/llvm-*/bin/"$1" 2>/dev/null); do
        if command -v "$c" >/dev/null 2>&1 || [ -x "$c" ]; then
            echo "$c"
            return 0
        fi
    done
    return 1
}
OTOOL=$(find_tool llvm-otool || find_tool otool || true)
LIPO=$(find_tool llvm-lipo || find_tool lipo || true)
if [ -z "${OTOOL:-}" ] || [ -z "${LIPO:-}" ]; then
    echo "  SKIP $slot — llvm-otool/llvm-lipo not found (install llvm to run Mach-O checks)"
    exit 0
fi

# ver_le A B -> true if version A <= version B (dotted numeric, via version sort).
ver_le() {
    [ "$1" = "$2" ] && return 0
    [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -1)" = "$1" ]
}

rc=0
nlib=0
# Run from inside the slot so otool prints basenames (not absolute paths that would
# otherwise be mistaken for install-names by the leading-'/' test below).
cd "$slot"
for lib in *.dylib; do
    [ -e "$lib" ] || continue # no-match guard when the glob is literal
    nlib=$((nlib + 1))
    bad=0

    # 1. arch slice present.
    archs=$("$LIPO" -archs "$lib" 2>/dev/null || echo "")
    case " $archs " in
        *" $arch "*) ;;
        *)
            echo "  FAIL $lib: expected arch '$arch' not in slice set [$archs]"
            bad=1
            ;;
    esac

    # 2a. install-name (LC_ID_DYLIB) must be @rpath/@loader_path-relative.
    id=$("$OTOOL" -D "$lib" 2>/dev/null | grep -E '^(@|/)' | head -1 || true)
    case "$id" in
        @*) ;;
        "") ;; # no id (unusual for a dylib) — leave it to the leak scan
        *)
            echo "  FAIL $lib: absolute install-name '$id' (want @rpath/@loader_path)"
            bad=1
            ;;
    esac

    # 2b. no dev-build / Homebrew absolute path baked into the id or any dependency.
    leaks=$("$OTOOL" -L "$lib" 2>/dev/null |
        grep -oE '(/Users/|/opt/homebrew/|/usr/local/)[^ )]*' | sort -u | tr '\n' ' ')
    if [ -n "$leaks" ]; then
        echo "  FAIL $lib: leaked absolute path(s): $leaks"
        bad=1
    fi

    # 3. min-OS load command present (and <= floor if one was given).
    minos=$("$OTOOL" -l "$lib" 2>/dev/null |
        awk '/LC_BUILD_VERSION|LC_VERSION_MIN_MACOSX/{f=1; next}
             f && ($1=="minos" || $1=="version"){print $2; exit}')
    if [ -z "$minos" ]; then
        echo "  FAIL $lib: no min-OS load command (LC_BUILD_VERSION / LC_VERSION_MIN_MACOSX)"
        bad=1
    elif [ -n "$floor" ] && ! ver_le "$minos" "$floor"; then
        echo "  FAIL $lib: min-OS $minos exceeds support floor $floor"
        bad=1
    fi

    if [ "$bad" -eq 0 ]; then
        echo "  ok   $lib   (arch $arch, @rpath/@loader_path, min-OS ${minos:-?})"
    else
        rc=1
    fi
done

if [ "$nlib" -eq 0 ]; then
    echo "  note: no .dylib in $slot"
fi
exit $rc
