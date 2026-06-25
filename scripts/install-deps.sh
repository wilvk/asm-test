#!/bin/sh
#
# scripts/install-deps.sh — install asm-test's OPTIONAL build dependencies
# across platforms by detecting the system package manager.
#
# The core build (`make test`) needs only `make` and a C compiler (cc/gcc/
# clang), which also assembles the GAS .s sources and ships gcov — so this
# script only covers the optional, feature-gated tools:
#
#   nasm         NASM backend      make ASM_SYNTAX=nasm ...
#   pkg-config   install/consume   make install ; pkg-config asmtest
#   unicorn      emulator tier     make emu-test
#   clang-tidy   static analysis   make tidy
#   valgrind     routine memcheck  make valgrind   (Linux/x86-64)
#
# Usage:
#   scripts/install-deps.sh [--all] [--nasm] [--emu] [--pkgconfig] [--tidy]
#                           [--valgrind] [--dry-run] [--help]
#
# With no selection flag it installs everything above (a full dev setup).
# Detects (in order): apt-get, dnf, yum, pacman, zypper, apk, brew. On Linux it
# uses sudo when not already root; brew never runs under sudo. Package names are
# best-effort and a few are distro-version-dependent (noted inline below).

set -eu

prog=install-deps

usage() {
    cat <<EOF
$prog — install asm-test's optional build dependencies cross-platform.

Usage: scripts/install-deps.sh [options]

Selection (default: all of them):
  --all          nasm + pkg-config + unicorn + keystone + clang-tidy + valgrind
  --nasm         NASM backend          (make ASM_SYNTAX=nasm ...)
  --emu          emulator tier         (make emu-test) — unicorn + pkg-config
  --asm          in-line assembler     (make asm-test) — keystone + unicorn + pkg-config
  --pkgconfig    install/consume lib   (make install ; pkg-config asmtest)
  --tidy         static analysis       (make tidy)
  --valgrind     routine memcheck      (make valgrind) — Linux/x86-64

Other:
  --dry-run, -n  print the commands instead of running them
  --help, -h     show this help

The core build (make test) needs none of these — only make + a C compiler.
EOF
}

want_nasm=0
want_pkgconfig=0
want_unicorn=0
want_keystone=0
want_tidy=0
want_valgrind=0
dry_run=0
selected=0

while [ $# -gt 0 ]; do
    case "$1" in
        --all)        want_nasm=1; want_pkgconfig=1; want_unicorn=1; want_keystone=1; want_tidy=1; want_valgrind=1; selected=1 ;;
        --nasm)       want_nasm=1; selected=1 ;;
        --emu)        want_unicorn=1; want_pkgconfig=1; selected=1 ;;
        --asm)        want_keystone=1; want_unicorn=1; want_pkgconfig=1; selected=1 ;;
        --pkgconfig|--pkg-config) want_pkgconfig=1; selected=1 ;;
        --tidy)       want_tidy=1; selected=1 ;;
        --valgrind)   want_valgrind=1; selected=1 ;;
        --dry-run|-n) dry_run=1 ;;
        --help|-h)    usage; exit 0 ;;
        *) echo "$prog: unknown option: $1" >&2; echo >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# No selection flag => full dev setup.
if [ "$selected" -eq 0 ]; then
    want_nasm=1; want_pkgconfig=1; want_unicorn=1; want_keystone=1; want_tidy=1; want_valgrind=1
fi

# Detect the package manager. Native Linux managers take precedence over a
# possible Linuxbrew install; on macOS only brew matches.
PM=""
for c in apt-get dnf yum pacman zypper apk brew; do
    if command -v "$c" >/dev/null 2>&1; then PM="$c"; break; fi
done
if [ -z "$PM" ]; then
    echo "$prog: no supported package manager found" >&2
    echo "  (looked for: apt-get dnf yum pacman zypper apk brew)" >&2
    echo "  install the tools manually: nasm, pkg-config, libunicorn, libkeystone, clang-tidy" >&2
    exit 1
fi

# Per-manager package names for each logical dependency. A few vary by distro
# release — adjust here if your repo names them differently.
# keystone_pkg is only set where a distro ships the Keystone *assembler* engine
# under an unambiguous name (note: on some repos "keystone" is OpenStack's
# identity service, NOT this). Where it's empty, --asm prints a build-from-source
# note instead of installing the wrong package.
case "$PM" in
    apt-get) nasm_pkg=nasm; pkgconfig_pkg=pkg-config; unicorn_pkg=libunicorn-dev;  keystone_pkg=libkeystone-dev; tidy_pkg=clang-tidy;        valgrind_pkg=valgrind ;;
    dnf|yum) nasm_pkg=nasm; pkgconfig_pkg=pkgconf-pkg-config; unicorn_pkg=unicorn-devel; keystone_pkg=; tidy_pkg=clang-tools-extra; valgrind_pkg=valgrind ;;
    pacman)  nasm_pkg=nasm; pkgconfig_pkg=pkgconf;     unicorn_pkg=unicorn;          keystone_pkg=; tidy_pkg=clang;            valgrind_pkg=valgrind ;;
    zypper)  nasm_pkg=nasm; pkgconfig_pkg=pkg-config;  unicorn_pkg=libunicorn-devel; keystone_pkg=; tidy_pkg=clang-tools;      valgrind_pkg=valgrind ;;
    apk)     nasm_pkg=nasm; pkgconfig_pkg=pkgconf;     unicorn_pkg=unicorn-dev;      keystone_pkg=; tidy_pkg=clang-extra-tools; valgrind_pkg=valgrind ;;
    brew)    nasm_pkg=nasm; pkgconfig_pkg=pkg-config;  unicorn_pkg=unicorn;          keystone_pkg=keystone; tidy_pkg=llvm;     valgrind_pkg= ;; # valgrind unsupported on current macOS
esac

have() { command -v "$1" >/dev/null 2>&1; }
have_unicorn() { have pkg-config && pkg-config --exists unicorn 2>/dev/null; }
have_keystone() { have pkg-config && pkg-config --exists keystone 2>/dev/null; }

pkgs=""
add() { pkgs="$pkgs $1"; }
skip() { echo "$prog: $1 already present, skipping"; }

[ "$want_nasm" -eq 1 ]      && { have nasm && skip nasm || add "$nasm_pkg"; }
[ "$want_pkgconfig" -eq 1 ] && { { have pkg-config || have pkgconf; } && skip pkg-config || add "$pkgconfig_pkg"; }
[ "$want_unicorn" -eq 1 ]   && { have_unicorn && skip unicorn || add "$unicorn_pkg"; }
[ "$want_keystone" -eq 1 ]  && {
    if have_keystone; then skip keystone
    elif [ -z "$keystone_pkg" ]; then
        echo "$prog: keystone has no $PM package; build from source:" >&2
        echo "  https://github.com/keystone-engine/keystone (then re-run make asm-test)" >&2
    else add "$keystone_pkg"; fi
}
[ "$want_tidy" -eq 1 ]      && { have clang-tidy && skip clang-tidy || add "$tidy_pkg"; }
[ "$want_valgrind" -eq 1 ]  && {
    if have valgrind; then skip valgrind
    elif [ -z "$valgrind_pkg" ]; then echo "$prog: valgrind unsupported on $PM (skipping)"
    else add "$valgrind_pkg"; fi
}

# Strip leading space; bail out early if there's nothing left to do.
pkgs=$(echo "$pkgs" | sed 's/^ *//')
if [ -z "$pkgs" ]; then
    echo "$prog: nothing to install (all selected deps already present)"
    exit 0
fi

case "$PM" in
    apt-get) install_cmd="apt-get install -y" ;;
    dnf)     install_cmd="dnf install -y" ;;
    yum)     install_cmd="yum install -y" ;;
    pacman)  install_cmd="pacman -S --needed --noconfirm" ;;
    zypper)  install_cmd="zypper install -y" ;;
    apk)     install_cmd="apk add" ;;
    brew)    install_cmd="brew install" ;;
esac

# Privilege escalation: brew refuses root; the Linux managers need it.
SUDO=""
if [ "$PM" != "brew" ] && [ "$(id -u)" -ne 0 ]; then
    if have sudo; then
        SUDO="sudo"
    elif [ "$dry_run" -eq 0 ]; then
        echo "$prog: $PM needs root and sudo was not found — run as root" >&2
        exit 1
    fi
fi

run() {
    echo "+ $*"
    [ "$dry_run" -eq 1 ] || "$@"
}

echo "$prog: manager=$PM  packages:$pkgs"
[ "$PM" = "apt-get" ] && run $SUDO apt-get update
# Intentionally unquoted: install_cmd and pkgs must word-split into argv.
run $SUDO $install_cmd $pkgs

# clang-tidy from Homebrew ships under the keg-only llvm and isn't on PATH.
if [ "$PM" = "brew" ] && [ "$want_tidy" -eq 1 ]; then
    echo "$prog: note — brew's clang-tidy lives in llvm; add it to PATH, e.g.:"
    echo '  export PATH="$(brew --prefix llvm)/bin:$PATH"'
fi

echo "$prog: done"
