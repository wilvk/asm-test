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
