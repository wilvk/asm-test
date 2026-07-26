#!/bin/sh
# fetch-addon.sh — one reusable fetcher for pinned Dear ImGui *addons*, so each
# addon (docs/internal/gui/12-addon-supply-chain.md) is a thin `fetch-<name>.sh`
# wrapper that sets a few env vars and `exec`s this, instead of a bespoke 90-line
# script. Mirrors the two existing shapes: fetch-linmath.sh (single header at a
# pinned commit) and fetch-imgui.sh (release tarball at a tag). Any OS.
#
# It downloads N pinned files (or one tarball) into build/addons/<name>-<ver>/,
# VERIFIES each against its row in scripts/third-party-digests.txt (refusing an
# unpinned or altered download, exactly as fetch-imgui.sh does — repo-review B5),
# optionally captures a license under licenses/, and prints the install dir on
# stdout so a Makefile can:
#     ADDON_HOME=$(ADDON_NAME=foo ADDON_VERSION=abc123 ADDON_FILES=... \
#                  scripts/fetch-addon.sh)
# Idempotent: a present, complete cache is reused.
#
# ── Inputs (env) ────────────────────────────────────────────────────────────
#   ADDON_NAME      required. Cache-path token + the license-capture basename.
#   ADDON_VERSION   required. Pin token used in the cache path (build/addons/
#                   <name>-<version>). NOT necessarily a per-file digest key.
#
#   FILES MODE (single/few headers from raw.githubusercontent):
#   ADDON_FILES     one entry per line, whitespace-separated 4-tuple:
#                       <url> <dest-relpath> <digest-name> <digest-version>
#                   Each file is verified against
#                       tp_digest tarball-sha256 <digest-name> <digest-version>
#                   (the manifest's "tarball-sha256" kind is just "sha256 of the
#                   fetched artifact" — linmath.h uses it for a lone header too).
#                   Multi-file addons whose files carry different upstream
#                   versions (e.g. TextSelect + its utfcpp dep) get one line —
#                   and one digest row — each.
#
#   TARBALL MODE (a whole repo/release archive; for multi-file-same-repo addons
#   like ImPlot / node-editor / ImGuiFileDialog / ICTE):
#   ADDON_TARBALL_URL   the .tar.gz to fetch; verified against
#                           tp_digest tarball-sha256 <ADDON_NAME> <ADDON_VERSION>
#   ADDON_TARBALL_KEEP  optional: space/newline list of relpaths (relative to the
#                       archive's single top dir) to publish under <home>/. When
#                       empty the whole extracted top dir is published as <home>.
#
#   LICENSE CAPTURE (optional, either mode):
#   ADDON_LICENSE_URL   fetched once (if absent) to licenses/<ADDON_LICENSE_DEST>.
#   ADDON_LICENSE_DEST  the licenses/ filename. Committed to the repo, so it is
#                       the pin — no digest required (as fetch-json/linmath do).
#
# On a bump: change the version + URLs in the wrapper, run it once (it fails on
# the missing/mismatched digest row and prints the "got" value), paste the digest
# into scripts/third-party-digests.txt.
set -eu

: "${ADDON_NAME:?fetch-addon: ADDON_NAME is required}"
: "${ADDON_VERSION:?fetch-addon: ADDON_VERSION is required}"

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib-thirdparty.sh"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ADDON_CACHE="${ADDON_CACHE:-$root/build/addons}"
home="$ADDON_CACHE/$ADDON_NAME-$ADDON_VERSION"

log() { echo "fetch-addon($ADDON_NAME): $*" >&2; }

# download <url> <dest> — curl or wget, into a per-pid temp then rename.
download() {
    _u="$1"; _d="$2"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$_u" -o "$_d"
    else
        wget -qO "$_d" "$_u"
    fi
}

# verify <file> <digest-name> <digest-version> — B5 integrity gate. Refuses an
# unpinned download (no row) or a mismatch, printing the got value so a bump can
# paste it. Same discipline and messages as fetch-imgui.sh:43.
verify() {
    _f="$1"; _n="$2"; _v="$3"
    _want=$(tp_digest tarball-sha256 "$_n" "$_v") || {
        log "ERROR: no pinned digest for $_n $_v in $TP_MANIFEST"
        log "       got sha256:$(tp_sha256 "$_f")"
        log "       (add that row by hand — refusing to use an unpinned download)"
        rm -f "$_f"; exit 1
    }
    _got="sha256:$(tp_sha256 "$_f")"
    if [ "$_got" != "$_want" ]; then
        log "ERROR: $_n $_v integrity check FAILED"
        log "       expected $_want"
        log "       got      $_got"
        rm -f "$_f"; exit 1
    fi
    log "verified $_n $_v ($_got)"
}

capture_license() {
    [ -n "${ADDON_LICENSE_URL:-}" ] || return 0
    [ -n "${ADDON_LICENSE_DEST:-}" ] || { log "ERROR: ADDON_LICENSE_URL set without ADDON_LICENSE_DEST"; exit 1; }
    _lic="$root/licenses/$ADDON_LICENSE_DEST"
    [ -f "$_lic" ] && return 0
    log "capturing license -> licenses/$ADDON_LICENSE_DEST"
    if ! download "$ADDON_LICENSE_URL" "$_lic"; then
        rm -f "$_lic"
        log "ERROR: could not fetch the license from $ADDON_LICENSE_URL"
        log "       (fix ADDON_LICENSE_URL in the wrapper, or commit the text by"
        log "        hand as licenses/$ADDON_LICENSE_DEST — D2 requires a capture)"
        exit 1
    fi
}

# ── FILES MODE ──────────────────────────────────────────────────────────────
fetch_files() {
    mkdir -p "$home"
    # Read the 4-tuples. A trailing "complete" witness marks a finished cache so
    # a partial download is never mistaken for a hit.
    printf '%s\n' "$ADDON_FILES" | while IFS= read -r line; do
        # skip blank lines
        [ -n "$(printf '%s' "$line" | tr -d '[:space:]')" ] || continue
        # shellcheck disable=SC2086
        set -- $line
        [ "$#" -eq 4 ] || { log "ERROR: ADDON_FILES line needs 4 fields <url> <relpath> <name> <version>: $line"; exit 1; }
        _url="$1"; _rel="$2"; _dn="$3"; _dv="$4"
        _dest="$home/$_rel"
        if [ -f "$_dest" ] && [ -f "$home/.complete" ]; then continue; fi
        mkdir -p "$(dirname "$_dest")"
        _tmp="$_dest.$$.part"
        log "fetching $_rel"
        download "$_url" "$_tmp"
        verify "$_tmp" "$_dn" "$_dv"
        mv "$_tmp" "$_dest"
    done
    : > "$home/.complete"
}

# ── TARBALL MODE ────────────────────────────────────────────────────────────
fetch_tarball() {
    if [ -f "$home/.complete" ]; then return 0; fi
    mkdir -p "$ADDON_CACHE"
    _tmp="$ADDON_CACHE/.$ADDON_NAME.$$.tar.gz"
    _stage="$ADDON_CACHE/.stage-$ADDON_NAME.$$"
    trap 'rm -rf "$_tmp" "$_stage"' EXIT INT TERM
    log "fetching tarball $ADDON_VERSION"
    download "$ADDON_TARBALL_URL" "$_tmp"
    verify "$_tmp" "$ADDON_NAME" "$ADDON_VERSION"
    rm -rf "$_stage"; mkdir -p "$_stage"
    ( cd "$_stage" && tar -xzf "$_tmp" )
    # A GitHub archive has exactly one top dir (<repo>-<sha>); find it.
    _top=$(cd "$_stage" && ls -d */ 2>/dev/null | head -1)
    [ -n "$_top" ] || { log "ERROR: archive had no top directory"; exit 1; }
    _top="$_stage/${_top%/}"
    rm -rf "$home"; mkdir -p "$home"
    if [ -n "${ADDON_TARBALL_KEEP:-}" ]; then
        printf '%s\n' "$ADDON_TARBALL_KEEP" | while IFS= read -r rel; do
            [ -n "$(printf '%s' "$rel" | tr -d '[:space:]')" ] || continue
            mkdir -p "$home/$(dirname "$rel")"
            cp -R "$_top/$rel" "$home/$rel"
        done
    else
        # publish the whole tree
        ( cd "$_top" && tar -cf - . ) | ( cd "$home" && tar -xf - )
    fi
    : > "$home/.complete"
    rm -rf "$_tmp" "$_stage"
    trap - EXIT INT TERM
}

if [ -n "${ADDON_TARBALL_URL:-}" ]; then
    fetch_tarball
elif [ -n "${ADDON_FILES:-}" ]; then
    fetch_files
else
    log "ERROR: set ADDON_FILES (files mode) or ADDON_TARBALL_URL (tarball mode)"
    exit 1
fi

capture_license
echo "$home"
