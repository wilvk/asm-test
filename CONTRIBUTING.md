# Contributing to asm-test

Thanks for your interest in improving asm-test. This guide covers the local
workflow, how the build is organized, and the conventions PRs are expected to
follow. The full design rationale lives in [DESIGN.md](DESIGN.md); the user-facing
docs are under [docs/](docs/) (and published via Read the Docs).

## Getting started

The core build needs nothing beyond `make` and a C compiler (`cc` — gcc or
clang, which also assembles the GAS `.s` sources):

```sh
make            # build + run the example suites (default target)
make check      # framework self-tests (assertions, runner, crash/hang containment)
make help       # the full target list, grouped by area, with the available knobs
```

`make help` is the source of truth for targets. Common knobs:

| Knob              | Effect                                                        |
| ----------------- | ------------------------------------------------------------ |
| `ASM_SYNTAX=nasm` | use the NASM (Intel-syntax) backend instead of GAS (x86-64)  |
| `WERROR=1`        | treat warnings as errors (CI sets this; see below)           |
| `CSTD=c11`        | override the pinned C standard (default `gnu11`)             |
| `SAN=1`           | build with AddressSanitizer + UndefinedBehaviorSanitizer     |
| `COV=1`           | build with gcov/llvm-cov coverage instrumentation            |
| `PREFIX=…`        | install prefix for `make install`                            |

## Run the CI lanes locally (Docker)

Per the repo convention, prefer the `docker-*` Makefile targets over installing
the optional toolchain on the host — they reproduce the **Linux** half of the CI
matrix in a container (the macOS jobs need a Mac). Highlights:

```sh
make docker-test      # example suites + self-tests (the `test` job)
make docker-nasm      # NASM backend
make docker-emu       # emulator tier (libunicorn)
make docker-ci        # the whole x86-64 Linux matrix end to end
make docker-bindings  # build + run every language image
```

Pass `DOCKER_PLATFORM=linux/arm64` to exercise the aarch64 lane. See the
[CI guide](docs/reference/ci.md) for the full mapping of jobs to targets.

## How the build is organized

The top-level [Makefile](Makefile) holds the core variables, knobs, and the
native build/test rules. Large self-contained target groups are split into
[mk/](mk/) by concern and `include`d in place (so they share every variable):

- `mk/docker.mk` — Docker CI lanes
- `mk/win64.mk` — native Win64 tier (cross-compile + Wine)
- `mk/native-trace.mk` — DynamoRIO + hardware (Intel PT / CoreSight) trace tiers
- `mk/bindings.mk` — conformance corpus, per-language binding tests, packaging
- `mk/docs.mk` — Sphinx / Read the Docs

Edit a target where it lives; edit knobs/shared variables in the parent Makefile.

## Adding or changing a language binding

Every binding drives the C library through the same FFI entry points and must
reproduce the **cross-language conformance corpus** — the single source of truth
that pins each canonical routine's expected result:

```sh
make conformance     # (re)generate bindings/conformance/corpus.json
make <lang>-test     # e.g. python-test, rust-test, go-test (see `make help`)
make docker-bindings # all bindings in their containers
```

When you add a C entry point to a **native-trace tier** (`asmtest_hwtrace.h` /
`asmtest_drtrace.h`), wrap it in every binding: a gate enforces that every binding
exposes every tier symbol, so the contract can't reach nine bindings and miss the
tenth.

```sh
make check-bindings-parity   # gate: fail if any binding is missing a tier symbol
make bindings-parity-report  # the symbol x binding coverage matrix
```

Record a deliberate omission in [scripts/bindings-parity-allow.txt](scripts/bindings-parity-allow.txt)
with a reason (stale exemptions fail the gate, so the list stays honest).

See [docs/bindings/index.md](docs/bindings/index.md) for the shared model and the per-language
pages, and [docs/archive/plans/binding-parity-plan.md](docs/archive/plans/binding-parity-plan.md)
for the parity checklist a new binding is held to. Keep the public surface and the
conformance coverage at parity with the existing bindings.

## Before you open a PR

- **Build clean with warnings as errors:** `make WERROR=1 test && make WERROR=1 check`
  (CI runs the gating jobs this way over a controlled gcc/clang toolchain).
- **Run the relevant tier(s)** for what you touched — the emulator suite
  (`make emu-test`), a binding (`make <lang>-test`), etc.
- **Update the docs** under [docs/](docs/) and the [CHANGELOG.md](CHANGELOG.md)
  `[Unreleased]` section when you change behavior or the public API.
- **Keep portability:** the framework targets x86-64 and AArch64, Linux and
  macOS. New assembly routines carry both an x86-64 and an AArch64 body (see the
  `ASM_FUNC` macros in [include/asm.h](include/asm.h)); add the NASM
  (Intel-syntax) counterpart for x86-64 example routines.

## Commit & PR conventions

- **Conventional Commits** with a scope, matching the existing history, e.g.
  `feat(trace): …`, `fix(rust): …`, `docs(ci): …`, `build: …`, `test(emu): …`.
- Keep each PR focused; explain the *why* in the description.
- CI must be green. The matrix is broad (4 native targets, NASM, emulator,
  DynamoRIO, Windows, sanitizers, valgrind, clang-tidy, and 10 bindings) — if a
  lane you can't run locally fails, say so in the PR rather than forcing it.

## License

asm-test's own source is **MIT** (see [LICENSE](LICENSE)). By contributing you
agree your contribution is licensed under the same terms. Note that the
*published language packages* bundle the GPL-2.0 Unicorn/Keystone/Capstone
engines and are therefore effectively GPL-2.0 as distributed — the asm-test
source itself stays MIT.
