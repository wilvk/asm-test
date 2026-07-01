#!/bin/sh
# package-native.sh — finish a native payload slot for a fully-featured package.
#
# Two modes:
#
#   package-native.sh <slot_dir> <emu_lib_filename>
#     The emulator superset (libasmtest_emu — emu + Keystone + Capstone). Asserts it
#     exports the optional asm + disas entry points, vendors Unicorn/Keystone/Capstone
#     next to it with a self-referential runtime search path ($ORIGIN / @loader_path),
#     and assembles THIRD-PARTY-LICENSES/.
#
#   package-native.sh --tier <role> <slot_dir> <tier_lib_filename>
#     An optional native-trace tier staged into the SAME slot (Linux-only):
#       role=hwtrace  libasmtest_hwtrace — asserts asmtest_hwtrace_available; vendors
#                     any linked decoder (libipt / OpenCSD / libbpf) when the lib was
#                     built with them (decoder-free builds have none — self-contained).
#       role=drtrace  libasmtest_drapp   — asserts asmtest_dr_available; vendors the
#                     DynamoRIO runtime (libdynamorio) and rpaths BOTH drapp and the
#                     co-staged libasmtest_drclient so they resolve it in-package.
#     Tier mode does NOT (re)write licenses — the caller re-runs collect-licenses.sh
#     over the finished slot once every lib is staged, so the NOTICE covers them all.
#
# Python uses auditwheel/delocate instead and skips this.
set -eu

prog=$(basename "$0")

tier=
if [ "${1:-}" = "--tier" ]; then
    tier="${2:?usage: $prog --tier <role> <slot_dir> <lib>}"
    shift 2
fi
slot="${1:?usage: $prog [--tier <role>] <slot_dir> <lib>}"
lib_name="${2:?usage: $prog [--tier <role>] <slot_dir> <lib>}"
lib="$slot/$lib_name"
PREFIX="${ASMTEST_DEP_PREFIX:-/usr/local}"
# Repo root: this script lives in scripts/.
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

[ -f "$lib" ] || { echo "$prog: $lib not found (stage it first)" >&2; exit 1; }

uname_s=$(uname -s)

# ---- symbol table of the staged lib (for the export assertion) -------------
lib_syms() { # lib_syms <path>
    _s=""
    if command -v nm >/dev/null 2>&1; then
        _s=$(nm -g "$1" 2>/dev/null || nm "$1" 2>/dev/null || true)
    fi
    if [ -z "$_s" ] && [ "$uname_s" = "Darwin" ] && command -v otool >/dev/null 2>&1; then
        _s=$(otool -IV "$1" 2>/dev/null || true)
    fi
    printf '%s\n' "$_s"
}

assert_exports() { # assert_exports <path> <sym>...
    _p="$1"; shift
    _syms=$(lib_syms "$_p")
    for _sym in "$@"; do
        if ! printf '%s\n' "$_syms" | grep -q "$_sym"; then
            echo "$prog: $(basename "$_p") does NOT export '$_sym'" >&2
            exit 1
        fi
    done
}

# ---- vendor a set of deps next to the lib and self-reference them -----------
# Copies each DT_NEEDED / linked dependency whose soname matches one of the given
# substrings into the slot and rewrites the search path to the slot dir. On Linux
# every staged lib (the given one plus any extra rpath targets) gets rpath $ORIGIN.
copy_in() { # copy_in <abs_path>
    [ -f "$1" ] || return 1
    cp -L "$1" "$slot/" && echo "  vendored $(basename "$1")"
}

vendor_and_rpath() { # vendor_and_rpath <lib_path> <dep-substr...>  (extra rpath targets read from $RPATH_EXTRA)
    _lib="$1"; shift
    if [ "$uname_s" = "Darwin" ]; then
        command -v install_name_tool >/dev/null 2>&1 || {
            echo "$prog: install_name_tool not found (Xcode CLT)" >&2; exit 1; }
        install_name_tool -id "@loader_path/$(basename "$_lib")" "$_lib" 2>/dev/null || true
        for d in "$@"; do
            for p in $(otool -L "$_lib" | awk 'NR>1{print $1}' | grep -i "$d" || true); do
                real="$p"
                case "$p" in @*) real="$PREFIX/lib/$(basename "$p")" ;; esac
                base=$(basename "$p")
                if copy_in "$real"; then
                    install_name_tool -id "@loader_path/$base" "$slot/$base" 2>/dev/null || true
                    install_name_tool -change "$p" "@loader_path/$base" "$_lib"
                fi
            done
        done
    else
        command -v patchelf >/dev/null 2>&1 || {
            echo "$prog: patchelf not found — install it (apt-get install patchelf, or pip install patchelf)" >&2; exit 1; }
        for d in "$@"; do
            for p in $(ldd "$_lib" 2>/dev/null | awk -v n="$d" 'index($0,n){print $3}'); do
                copy_in "$p" || true
            done
        done
        patchelf --set-rpath '$ORIGIN' "$_lib"
        for so in "$slot"/lib*.so*; do
            [ -f "$so" ] || continue
            [ "$so" = "$_lib" ] && continue
            patchelf --set-rpath '$ORIGIN' "$so" 2>/dev/null || true
        done
    fi
}

# ===========================================================================
# Tier mode: one native-trace tier lib, no license rewrite.
# ===========================================================================
if [ -n "$tier" ]; then
    case "$tier" in
        hwtrace)
            assert_exports "$lib" asmtest_hwtrace_available
            echo "$prog: ok — $lib_name exports asmtest_hwtrace_available"
            # Decoder-free builds link no external decoder; a "full" build links
            # libipt / OpenCSD / libbpf — vendor whichever are present.
            vendor_and_rpath "$lib" ipt opencsd bpf
            ;;
        drtrace)
            assert_exports "$lib" asmtest_dr_available
            echo "$prog: ok — $lib_name exports asmtest_dr_available"
            # libasmtest_drapp dlopens libdynamorio at run time; the co-staged
            # libasmtest_drclient (loaded by DR) links it too. Vendor it once and
            # rpath every staged lib to $ORIGIN so both resolve it in-package.
            vendor_and_rpath "$lib" dynamorio
            ;;
        *)
            echo "$prog: unknown tier role '$tier'" >&2; exit 1 ;;
    esac
    echo "$prog: completed tier $tier in $slot"
    exit 0
fi

# ===========================================================================
# Emulator mode (default): the superset lib + its three engines + licenses.
# ===========================================================================
# asmtest_emu_call_asm6 = the Keystone in-line assembler; emu_disas = the Capstone
# disassembler. Both must be present in the superset lib.
assert_exports "$lib" asmtest_emu_call_asm6 emu_disas
echo "$prog: ok — $lib_name exports the asm + disas entry points"

vendor_and_rpath "$lib" unicorn keystone capstone

"$root/scripts/collect-licenses.sh" "$slot/THIRD-PARTY-LICENSES"

echo "$prog: completed $slot"
