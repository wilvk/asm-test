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

# tp_sha256 <file> — print the file's raw SHA-256 hex, portably (Linux/macOS).
tp_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}
