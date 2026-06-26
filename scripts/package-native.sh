#!/bin/sh
# package-native.sh — finish a native payload slot for a fully-featured package.
#
# Given a staged slot directory that already holds the core lib and the superset
# emulator lib (libasmtest_emu_full, copied in under the libasmtest_emu name), this:
#
#   1. asserts the emu lib actually exports the optional asm + disas entry points
#      (so a release can never silently ship the lean lib in the full slot),
#   2. vendors the three native dependencies (Unicorn, Keystone, Capstone) next to
#      it and rewrites the lib's runtime search path to its own directory
#      ($ORIGIN on Linux, @loader_path on macOS) so it loads with no system install,
#   3. assembles a version-matched THIRD-PARTY-LICENSES/ dir (asm-test's MIT text
#      + each dep's license at the version pkg-config reports + a NOTICE manifest).
#
# Python uses auditwheel/delocate instead and skips this. Usage:
#   scripts/package-native.sh <slot_dir> <emu_lib_filename>
set -eu

prog=$(basename "$0")
slot="${1:?usage: $prog <slot_dir> <emu_lib_filename>}"
emu="${2:?usage: $prog <slot_dir> <emu_lib_filename>}"
lib="$slot/$emu"
PREFIX="${ASMTEST_DEP_PREFIX:-/usr/local}"
# Repo root: this script lives in scripts/.
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

[ -f "$lib" ] || { echo "$prog: $lib not found (stage it first)" >&2; exit 1; }

uname_s=$(uname -s)

# ---- 1. symbol assertion ---------------------------------------------------
# asmtest_emu_call_asm6 = the Keystone in-line assembler; emu_disas = the Capstone
# disassembler. Both must be present in the superset lib.
syms=""
if command -v nm >/dev/null 2>&1; then
    syms=$(nm -g "$lib" 2>/dev/null || nm "$lib" 2>/dev/null || true)
fi
if [ -z "$syms" ] && [ "$uname_s" = "Darwin" ] && command -v otool >/dev/null 2>&1; then
    syms=$(otool -IV "$lib" 2>/dev/null || true)
fi
for sym in asmtest_emu_call_asm6 emu_disas; do
    if ! printf '%s\n' "$syms" | grep -q "$sym"; then
        echo "$prog: $emu does NOT export '$sym' — not a full (superset) lib?" >&2
        exit 1
    fi
done
echo "$prog: ok — $emu exports the asm + disas entry points"

# ---- 2. vendor the native deps + patch the search path ---------------------
# Logical dep -> the substring its shared object's name carries.
deps="unicorn keystone capstone"

copy_in() { # copy_in <abs_path>
    [ -f "$1" ] || return 1
    cp -L "$1" "$slot/" && echo "  vendored $(basename "$1")"
}

if [ "$uname_s" = "Darwin" ]; then
    command -v install_name_tool >/dev/null 2>&1 || {
        echo "$prog: install_name_tool not found (Xcode CLT)" >&2; exit 1; }
    install_name_tool -id "@loader_path/$emu" "$lib" 2>/dev/null || true
    for d in $deps; do
        # Each dependency line in otool -L: "<path> (compatibility ...)".
        for p in $(otool -L "$lib" | awk 'NR>1{print $1}' | grep -i "$d" || true); do
            real="$p"
            # @rpath/@loader_path install names: resolve against the dep prefix.
            case "$p" in
                @*) real="$PREFIX/lib/$(basename "$p")" ;;
            esac
            base=$(basename "$p")
            if copy_in "$real"; then
                install_name_tool -id "@loader_path/$base" "$slot/$base" 2>/dev/null || true
                install_name_tool -change "$p" "@loader_path/$base" "$lib"
            fi
        done
    done
else
    command -v patchelf >/dev/null 2>&1 || {
        echo "$prog: patchelf not found — install it (apt-get install patchelf)" >&2; exit 1; }
    for d in $deps; do
        for p in $(ldd "$lib" 2>/dev/null | awk -v n="$d" 'index($0,n){print $3}'); do
            copy_in "$p" || true
        done
    done
    patchelf --set-rpath '$ORIGIN' "$lib"
    for so in "$slot"/lib*.so*; do
        [ -f "$so" ] || continue
        [ "$so" = "$lib" ] && continue
        patchelf --set-rpath '$ORIGIN' "$so" 2>/dev/null || true
    done
fi

# ---- 3. THIRD-PARTY-LICENSES ----------------------------------------------
"$root/scripts/collect-licenses.sh" "$slot/THIRD-PARTY-LICENSES"

echo "$prog: completed $slot"
