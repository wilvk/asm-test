#!/bin/sh
# Propagate the single source of truth — the repo-root VERSION file — into every
# language binding manifest, so a version bump touches one file instead of nine.
#
#   scripts/sync-version.sh          rewrite every manifest to match VERSION
#   scripts/sync-version.sh --check  verify they all match; non-zero exit if not
#                                    (used by `make check-version` in CI)
#
# Each manifest pins the version a little differently; the per-file edits below
# are deliberately anchored so they touch only the package's own version field.
set -eu

cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
VERSION="$(cat VERSION | tr -d '[:space:]')"
[ -n "$VERSION" ] || { echo "sync-version: VERSION file is empty" >&2; exit 2; }

# The C header (include/asmtest.h) pins the version in four macros and is a SECOND
# source of truth: scripts/amalgamate.sh derives the single-header version from it.
# It needs the numeric MAJOR/MINOR/PATCH split, so require an exact 3-part numeric
# semver here (a pre-release suffix couldn't populate the numeric macros anyway).
MAJOR=${VERSION%%.*}; rest=${VERSION#*.}; MINOR=${rest%%.*}; PATCH=${rest#*.}
if [ "$MAJOR.$MINOR.$PATCH" != "$VERSION" ]; then
  echo "sync-version: VERSION '$VERSION' is not MAJOR.MINOR.PATCH" >&2; exit 2
fi
case "$MAJOR$MINOR$PATCH" in
  *[!0-9]*) echo "sync-version: VERSION '$VERSION' has non-numeric parts" >&2; exit 2;;
esac

CHECK=0
[ "${1:-}" = "--check" ] && CHECK=1

status=0

# replace_in FILE SED_EXPR  — apply SED_EXPR (which must rewrite the version to
# $VERSION) in place, portably (BSD + GNU sed) via a temp file.
replace_in() {
  f="$1"; expr="$2"
  tmp="$f.tmp.$$"
  sed -E "$expr" "$f" > "$tmp"
  mv "$tmp" "$f"
}

# check_has FILE GREP_EXPR  — pass if the file already contains the expected
# version-bearing line; otherwise report drift.
check_has() {
  f="$1"; pat="$2"
  if grep -Eq "$pat" "$f"; then
    return 0
  fi
  echo "sync-version: $f is not at $VERSION (run: make sync-version)" >&2
  status=1
}

# Each entry: FILE | sed rewrite expr | grep verify pat (verify uses $VERSION).
# python: pyproject.toml -> version = "X"
PY=bindings/python/pyproject.toml
# rust: Cargo.toml [package] version (column-0 `version = "..."`)
RS=bindings/rust/Cargo.toml
# node: package.json -> "version": "X"
ND=bindings/node/package.json
# ruby: gemspec -> s.version = "X"
RB=bindings/ruby/asmtest.gemspec
# java: pom.xml -> the project <version> (2-space indented, not modelVersion)
JV=bindings/java/pom.xml
# dotnet: csproj -> <Version>X</Version>
DN=bindings/dotnet/asmtest-lib.csproj
# zig: build.zig.zon -> .version = "X"
ZG=bindings/zig/build.zig.zon
# lua: rockspec carries the version in BOTH its body and its filename
LUA_DIR=bindings/lua
# C header: ASMTEST_VERSION_{MAJOR,MINOR,PATCH} (numeric) + ASMTEST_VERSION (string)
HDR=include/asmtest.h

if [ "$CHECK" -eq 1 ]; then
  check_has "$PY" "^version = \"$VERSION\"$"
  check_has "$RS" "^version = \"$VERSION\"$"
  check_has "$ND" "^  \"version\": \"$VERSION\","
  check_has "$RB" "^  s\.version  *= \"$VERSION\""
  check_has "$JV" "^  <version>$VERSION</version>$"
  check_has "$DN" "<Version>$VERSION</Version>"
  check_has "$ZG" "^    \.version = \"$VERSION\","
  check_has "$HDR" "^#define ASMTEST_VERSION_MAJOR $MAJOR$"
  check_has "$HDR" "^#define ASMTEST_VERSION_MINOR $MINOR$"
  check_has "$HDR" "^#define ASMTEST_VERSION_PATCH $PATCH$"
  check_has "$HDR" "^#define ASMTEST_VERSION \"$VERSION\"$"
  rock="$LUA_DIR/asmtest-$VERSION-1.rockspec"
  if [ -f "$rock" ] && grep -Eq "^version = \"$VERSION-1\"$" "$rock"; then
    :
  else
    echo "sync-version: $LUA_DIR rockspec is not at $VERSION (run: make sync-version)" >&2
    status=1
  fi
  [ "$status" -eq 0 ] && echo "sync-version: all manifests at $VERSION"
  exit $status
fi

replace_in "$PY" "s|^version = \".*\"$|version = \"$VERSION\"|"
replace_in "$RS" "s|^version = \".*\"$|version = \"$VERSION\"|"
replace_in "$ND" "s|^(  \"version\": )\".*\",|\1\"$VERSION\",|"
replace_in "$RB" "s|^(  s\.version  *= )\".*\"|\1\"$VERSION\"|"
replace_in "$JV" "s|^  <version>.*</version>$|  <version>$VERSION</version>|"
replace_in "$DN" "s|<Version>.*</Version>|<Version>$VERSION</Version>|"
replace_in "$ZG" "s|^(    \.version = )\".*\",|\1\"$VERSION\",|"
replace_in "$HDR" "s|^#define ASMTEST_VERSION_MAJOR .*|#define ASMTEST_VERSION_MAJOR $MAJOR|"
replace_in "$HDR" "s|^#define ASMTEST_VERSION_MINOR .*|#define ASMTEST_VERSION_MINOR $MINOR|"
replace_in "$HDR" "s|^#define ASMTEST_VERSION_PATCH .*|#define ASMTEST_VERSION_PATCH $PATCH|"
replace_in "$HDR" "s|^#define ASMTEST_VERSION \".*\"|#define ASMTEST_VERSION \"$VERSION\"|"

# Lua rockspec: the filename encodes name-version-revision, so rename it when the
# version changes, then rewrite the `version` field inside.
oldrock=$(ls "$LUA_DIR"/asmtest-*.rockspec 2>/dev/null | head -n1 || true)
newrock="$LUA_DIR/asmtest-$VERSION-1.rockspec"
if [ -n "$oldrock" ] && [ "$oldrock" != "$newrock" ]; then
  if command -v git >/dev/null 2>&1 && git ls-files --error-unmatch "$oldrock" >/dev/null 2>&1; then
    git mv "$oldrock" "$newrock"
  else
    mv "$oldrock" "$newrock"
  fi
fi
replace_in "$newrock" "s|^version = \".*\"$|version = \"$VERSION-1\"|"

echo "sync-version: wrote $VERSION into all binding manifests"
