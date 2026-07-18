# Installation

asm-test is built from source with `make` and a C compiler. The core has **no
third-party dependencies** — the compiler also assembles the GAS-syntax `.s`
sources. Optional tiers (the NASM backend, the emulator, packaging, the analysis
targets) pull in extra tools you can install on demand.

## Requirements

| | Required for | Notes |
|---|---|---|
| **C compiler** (`cc` — gcc or clang) + `make` | The core framework | Also assembles the `.s` sources |
| x86-64 **or** AArch64 CPU | Running native tests | Both are first-class targets |
| Linux **or** macOS | Everything | ELF and Mach-O handled by `asm.h` |
| `nasm` | The NASM backend (`ASM_SYNTAX=nasm`) | x86-64 only, opt-in |
| `pkg-config` | Installing/consuming `asmtest.pc` | See [Integration](../reference/integration.md) |
| `libunicorn` | The [emulator tier](../guides/emulator.md) (`make emu-test`) | Optional |
| `libkeystone` | The [in-line assembler tier](../reference/features.md#in-line-assembler-optional-keystone) (`make asm-test`) | Optional; no distro package — pinned source build via `scripts/build-keystone.sh` (`make deps DEPS_ARGS=--asm` walks you through it; Homebrew on macOS) |
| `libcapstone` | [Disassembly in diagnostics](../guides/disassembly.md) (`disas_available()`) | Optional; pinned source build via `scripts/build-capstone.sh` — deliberately never installed from a distro/brew package (the `--emu`/`--asm` deps flows point you at the script) |
| `clang-tidy`, `valgrind` | The analysis targets | Optional |

## Get the source

```sh
git clone https://github.com/wilvk/asm-test.git
cd asm-test
```

## Install from a system package manager

The C core — the static `libasmtest.a`, the public headers, and the `asmtest.pc`
pkg-config file (MIT; no third-party engines) — has authored packaging specs for
five managers under
[`packaging/`](https://github.com/wilvk/asm-test/blob/main/packaging/). Each is
built and installed end to end in CI (`make docker-syspkg-<mgr>`), so the recipes
are known-good. **Consumption is gated on two maintainer steps that have not yet
run:** publishing the `v1.1.0` release (the specs pin its source tarball asset)
and submitting each spec to its upstream index. Until both happen for a given
manager, the supported install path is [from source](#get-the-source)
(`make install`).

| Manager | Command (after publication) | Spec |
|---|---|---|
| Homebrew | `brew tap wilvk/asmtest && brew install asmtest` | [`packaging/homebrew/asmtest.rb`](https://github.com/wilvk/asm-test/blob/main/packaging/homebrew/asmtest.rb) |
| Debian / Ubuntu | `apt install libasmtest-dev` | [`packaging/debian/`](https://github.com/wilvk/asm-test/blob/main/packaging/debian/) |
| Arch (AUR) | `makepkg -si` in the AUR checkout | [`packaging/aur/PKGBUILD`](https://github.com/wilvk/asm-test/blob/main/packaging/aur/PKGBUILD) |
| vcpkg | `vcpkg install asmtest --overlay-ports=packaging/vcpkg/ports` | [`packaging/vcpkg/ports/asmtest/`](https://github.com/wilvk/asm-test/blob/main/packaging/vcpkg/ports/asmtest/) |
| Conan | `conan create packaging/conan/recipes/asmtest/all --version=1.1.0` | [`packaging/conan/recipes/asmtest/`](https://github.com/wilvk/asm-test/blob/main/packaging/conan/recipes/asmtest/) |

Each package installs the same layout `make install` does, so a consumer builds
against it with `pkg-config --cflags --libs asmtest`. The submission runbook for
maintainers is in [releasing.md](../reference/releasing.md#system-packages).

## Build and run the bundled suites

```sh
make test
```

This assembles the example routines, compiles the C tests, links one binary per
suite under `build/`, and runs them all. A green summary with a zero exit status
means everything passed. Continue to the [Quick start](quickstart.md) to write
your own.

## Installing the optional tools

The optional toolchain can be installed cross-platform through your system
package manager with a single target:

```sh
make deps                       # full dev setup (nasm, pkg-config, unicorn, …)
make deps DEPS_ARGS=--emu       # what `make emu-test` needs: unicorn + pkg-config (Capstone is source-built — run scripts/build-capstone.sh)
make deps DEPS_ARGS=--asm       # adds the in-line assembler tier (keystone, source-built)
make deps DEPS_ARGS=--dry-run   # print the commands without running them
```

[`scripts/install-deps.sh`](https://github.com/wilvk/asm-test/blob/main/scripts/install-deps.sh)
detects `apt-get` / `dnf` / `yum` / `pacman` / `zypper` / `apk` / `brew` and
uses `sudo` on Linux when you are not root.

:::{tip}
You don't need any of the optional tools to start writing tests. Install them
only when you reach for the feature that needs them — the NASM backend, the
emulator tier, or the packaging/analysis targets.
:::

## Verifying a clean checkout

The framework ships its own black-box self-tests, which double as a smoke test
of your toolchain:

```sh
make check     # run tests/positive.c, tests/negative.c via tests/expect.sh
```

If both `make test` and `make check` pass, your environment is ready.
