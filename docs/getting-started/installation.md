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
| `clang-tidy`, `valgrind` | The analysis targets | Optional |

## Get the source

```sh
git clone https://github.com/wilvk/asm-test.git
cd asm-test
```

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
make deps DEPS_ARGS=--emu       # only what `make emu-test` needs (libunicorn)
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
