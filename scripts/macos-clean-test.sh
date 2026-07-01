#!/bin/sh
# macos-clean-test.sh — Track A of the macOS clean-room & portability plan.
#
# For every binding whose toolchain is present, package it, install it FRESH into
# a throwaway prefix, load it with every ASMTEST_*/DYLD_*/LD_* override scrubbed
# and the cwd outside the checkout (scripts/clean-env.sh), then ASSERT the native
# library it actually resolved lives under that fresh install — never a leaked
# dev build/ tree, a Homebrew dylib, or /usr/local (scripts/assert-clean-path.sh).
# So "install fresh, no ASMTEST_LIB" is proven, not trusted. Bindings whose
# toolchain is absent self-skip; a real leak fails the run.
#
# Driven by `make macos-clean-test`. See docs/plans/macos-clean-test-plan.md.
set -u

REPO=${ASMTEST_REPO_ROOT:-$(pwd)}
REPO=$(cd "$REPO" && pwd -P)
export ASMTEST_REPO_ROOT="$REPO"
PLAT="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"
EMU_SLOT="$REPO/build/dist/native/$PLAT/libasmtest_emu.dylib"
CLEAN_ENV="$REPO/scripts/clean-env.sh"
ASSERT="$REPO/scripts/assert-clean-path.sh"

[ -f "$EMU_SLOT" ] || {
    echo "macos-clean-test: no staged payload at $EMU_SLOT — run 'make package-libs' first" >&2
    exit 1
}

WORK="$(mktemp -d 2>/dev/null || mktemp -d -t asmtest-cleanroom)"
trap 'rm -rf "$WORK"' EXIT

fail=0
summary=""
record()  { summary="${summary}  ${1}\t${2}\t${3}\n"; }
pass_b()  { echo "  PASS $1: $2";        record "$1" PASS "$2"; }
skip_b()  { echo "  SKIP $1: $2";        record "$1" SKIP "$2"; }
fail_b()  { echo "  FAIL $1: $2" >&2;    record "$1" FAIL "$2"; fail=1; }

# assert_path <binding> <resolved-path> <install-prefix>
assert_path() {
    _out="$(sh "$ASSERT" "$2" "$3" 2>&1)"
    if [ $? -eq 0 ]; then pass_b "$1" "$_out"; else fail_b "$1" "$_out"; fi
}

echo "macos-clean-test: clean-room install tests on $PLAT"
echo "  repo=$REPO"
echo "  work=$WORK"
echo

# ---- ruby -----------------------------------------------------------------
ruby_clean_test() {
    RUBY="$(command -v ruby 2>/dev/null)" || { skip_b ruby "no ruby toolchain"; return; }
    command -v gem >/dev/null 2>&1        || { skip_b ruby "no gem"; return; }
    ( cd "$REPO" && make -s ruby-package ) >/dev/null 2>&1 \
        || { fail_b ruby "make ruby-package failed"; return; }
    gem_file="$(ls "$REPO"/build/dist/ruby/asmtest-*.gem 2>/dev/null | head -1)"
    [ -n "$gem_file" ] || { fail_b ruby "no gem built"; return; }
    dest="$WORK/ruby"
    "$RUBY" -S gem install --local --install-dir "$dest" --no-document "$gem_file" >/dev/null 2>&1 \
        || { fail_b ruby "gem install failed"; return; }
    path="$(
        . "$CLEAN_ENV" 1>&2
        GEM_HOME="$dest" GEM_PATH="$dest" "$RUBY" -e "require 'asmtest'; print Asmtest.library_path" 2>/dev/null
    )"
    assert_path ruby "$path" "$dest"
}

# ---- python (needs delocate to self-contain the local wheel) --------------
python_clean_test() {
    PY="$(command -v python3 2>/dev/null)" || { skip_b python "no python3 toolchain"; return; }
    "$PY" -c "import delocate" 2>/dev/null \
        || { skip_b python "no delocate (pip install delocate); release.yml covers the python clean-room"; return; }
    ( cd "$REPO" && make -s python-package ) >/dev/null 2>&1 \
        || { fail_b python "make python-package failed"; return; }
    raw="$(ls "$REPO"/build/dist/python/*.whl 2>/dev/null | head -1)"
    [ -n "$raw" ] || { fail_b python "no wheel built"; return; }
    wh="$WORK/wheelhouse"; mkdir -p "$wh"
    "$PY" -m delocate.cmd.delocate_wheel -w "$wh" "$raw" >/dev/null 2>&1 \
        || { fail_b python "delocate-wheel failed"; return; }
    whl="$(ls "$wh"/*.whl 2>/dev/null | head -1)"
    "$PY" -m venv "$WORK/pyvenv" >/dev/null 2>&1 || { fail_b python "venv failed"; return; }
    "$WORK/pyvenv/bin/pip" install -q "$whl" >/dev/null 2>&1 || { fail_b python "pip install failed"; return; }
    path="$(
        . "$CLEAN_ENV" 1>&2
        "$WORK/pyvenv/bin/python" -m asmtest --where 2>/dev/null | awk '$1=="core"{print $2}'
    )"
    assert_path python "$path" "$WORK/pyvenv"
}

# ---- node -----------------------------------------------------------------
node_clean_test() {
    NODE="$(command -v node 2>/dev/null)" || { skip_b node "no node toolchain"; return; }
    command -v npm >/dev/null 2>&1        || { skip_b node "no npm"; return; }
    ( cd "$REPO" && make -s node-package ) >/dev/null 2>&1 \
        || { fail_b node "make node-package failed"; return; }
    tgz="$(ls "$REPO"/build/dist/node/asmtest-*.tgz 2>/dev/null | head -1)"
    [ -n "$tgz" ] || { fail_b node "no tarball built"; return; }
    proj="$WORK/node"; mkdir -p "$proj"
    ( cd "$proj" && npm init -y >/dev/null 2>&1 && npm install "$tgz" >/dev/null 2>&1 ) \
        || { fail_b node "npm install failed"; return; }
    path="$(
        . "$CLEAN_ENV" 1>&2
        cd "$proj" && "$NODE" -e "process.stdout.write(require('asmtest').libraryPath())" 2>/dev/null
    )"
    assert_path node "$path" "$proj"
}

# ---- lua ------------------------------------------------------------------
lua_clean_test() {
    LUAJIT="$(command -v luajit 2>/dev/null)" || { skip_b lua "no luajit toolchain"; return; }
    ( cd "$REPO" && make -s lua-package ) >/dev/null 2>&1 \
        || { fail_b lua "make lua-package failed"; return; }
    dest="$WORK/lua"; mkdir -p "$dest"
    cp "$REPO/bindings/lua/asmtest.lua" "$dest/" 2>/dev/null || { fail_b lua "no asmtest.lua"; return; }
    cp -R "$REPO/bindings/lua/native" "$dest/" 2>/dev/null || { fail_b lua "no bundled native/"; return; }
    path="$(
        . "$CLEAN_ENV" 1>&2
        "$LUAJIT" -e "package.path='$dest/?.lua;'..package.path; io.write(require('asmtest').library_path())" 2>/dev/null
    )"
    assert_path lua "$path" "$dest"
}

# ---- java (macOS ships a JRE-less `java` stub that errors on -version) -----
java_clean_test() {
    JAVA="$(command -v java 2>/dev/null)"; JAVAC="$(command -v javac 2>/dev/null)"
    { [ -n "$JAVA" ] && [ -n "$JAVAC" ]; } || { skip_b java "no java/javac toolchain"; return; }
    "$JAVA" -version >/dev/null 2>&1 || { skip_b java "no JRE (macOS java stub only — install a JDK 21)"; return; }
    ( cd "$REPO" && make -s java-package ) >/dev/null 2>&1 \
        || { fail_b java "make java-package failed"; return; }
    jar="$(ls "$REPO"/build/dist/java/asmtest-*.jar 2>/dev/null | head -1)"
    [ -n "$jar" ] || { fail_b java "no jar built"; return; }
    smoke="$WORK/java"; mkdir -p "$smoke"
    printf 'public class Where{public static void main(String[] a){System.out.print(Asmtest.libraryPath());}}' > "$smoke/Where.java"
    "$JAVAC" --release 21 --enable-preview -cp "$jar" -d "$smoke" "$smoke/Where.java" 2>/dev/null \
        || { fail_b java "javac failed (needs JDK 21)"; return; }
    path="$(
        . "$CLEAN_ENV" 1>&2
        "$JAVA" --enable-preview --enable-native-access=ALL-UNNAMED -cp "$jar:$smoke" Where 2>/dev/null
    )"
    assert_path java "$path" "$smoke"
}

ruby_clean_test
python_clean_test
node_clean_test
lua_clean_test
java_clean_test

echo
echo "macos-clean-test summary ($PLAT):"
printf "$summary" | while IFS='	' read -r b s m; do printf "  %-8s %-5s %s\n" "$b" "$s" "$m"; done
echo
if [ "$fail" -ne 0 ]; then
    echo "macos-clean-test: FAILED — a binding resolved its native lib OUTSIDE its fresh install (see LEAK above)."
    exit 1
fi
echo "macos-clean-test: OK — every present binding resolved its native lib from its fresh install."
