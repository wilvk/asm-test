# Using asm-test in your project

There are two ways to consume the framework from a project of your own. The
**static library** is the primary, recommended path; the single-header
amalgamation is a lighter-weight alternative. To run your suites in CI with
either, see [Using asm-test in your CI](../guides/ci-integration.md) — a
GitHub Action, a GitLab template, and the raw recipe.

## 1. Static library + pkg-config (recommended)

Install the headers, `libasmtest.a`, and an `asmtest.pc` pkg-config file:

```sh
make install                      # to /usr/local by default
make install PREFIX=$HOME/.local  # to a custom prefix
make DESTDIR=/tmp/stage install   # staged install (for packaging)
```

The headers install under `…/include/asmtest/`. A consumer then builds against
the library through pkg-config — including the routine under test, which is
assembled with the same `#include "asm.h"` shim (also installed). The `.s`
source is preprocessed (that's what makes the shim portable), so the assemble
step needs `-x assembler-with-cpp`:

```sh
cc $(pkg-config --cflags asmtest) -x assembler-with-cpp -c my_routine.s -o my_routine.o
cc $(pkg-config --cflags asmtest) -c my_tests.c -o my_tests.o
cc my_tests.o my_routine.o $(pkg-config --libs asmtest) -o my_tests
```

`make uninstall` reverses the install.

| Variable | Purpose | Default |
|---|---|---|
| `PREFIX` | Install prefix | `/usr/local` |
| `DESTDIR` | Staging root prepended to `PREFIX` (packaging) | empty |

### The emulator tier via pkg-config

The core `asmtest` module covers capture + runner. If your suites also use the
[emulator tier](../guides/emulator.md), install the shared superset and consume
the second module, `asmtest-emu`:

```sh
make install-shared-emu PREFIX=$HOME/.local   # libasmtest_emu + asmtest-emu.pc
cc $(pkg-config --cflags asmtest-emu) -c my_emu_tests.c -o my_emu_tests.o
cc my_emu_tests.o my_routine.o $(pkg-config --libs asmtest-emu) -o my_emu_tests
```

(Plain `make install` installs the static core only — `asmtest-emu.pc` is
written by `make install-shared-emu`, whose lib is the full superset and so
needs libunicorn + libkeystone + libcapstone at build time; see
[Installation](../getting-started/installation.md).)

## 2. Single-header amalgamation

```sh
make amalgamate        # writes asmtest_single.h
```

Include `asmtest_single.h` for the API. In **exactly one** translation unit,
`#define ASMTEST_IMPLEMENTATION` before including it to emit the runtime:

```c
#define ASMTEST_IMPLEMENTATION
#include "asmtest_single.h"
```

All other translation units just `#include "asmtest_single.h"`.

:::{warning}
The register/flags capture trampoline is **assembly** (`capture.s`) and cannot
live in a C header. So even with the single header you must assemble and link
that trampoline (or just link `libasmtest.a`). The amalgamation covers the C
surface only; the optional [emulator tier](../guides/emulator.md) is **not** included.
:::

## Version macros

The installed header exposes compile-time version information:

| Macro | Example | Use |
|---|---|---|
| `ASMTEST_VERSION` | `"1.1.0"` | human-readable version string |
| `ASMTEST_VERSION_NUM` | integer | numeric compares in `#if` guards |

See the [Changelog](../project/changelog.md) for what changed between releases.

## Requirements recap

Consuming the library needs a C compiler and (for the pkg-config path)
`pkg-config`. Your routines under test assemble with the same compiler via the
installed `asm.h`. See [Installation](../getting-started/installation.md) for the full dependency
matrix and the `make deps` helper.
