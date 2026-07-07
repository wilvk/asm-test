# Using asm-test in your CI

This guide is for **consumers**: you have a project with assembly routines and
asm-test suites, and you want your own CI to build and run them. (For how this
repo tests *itself*, see [CI & Docker](../reference/ci.md).) Three paths,
easiest first:

- a **GitHub Action** (`uses: wilvk/asm-test@main`) — installs the framework
  and wires up `pkg-config` for the rest of your job;
- an **includable GitLab CI template** with the same shape;
- the raw **`make install`** recipe both of those wrap, for any other CI.

Whichever path you take, your suite's runner emits JUnit XML
(`--format=junit`, see [the runner](runner.md#output-formats)) that GitHub,
GitLab, Jenkins, and friends can ingest natively.

:::{note}
The Action and the GitLab template are new and **syntactically validated but
not yet exercised by an external consumer pipeline** — this repo's own CI
cannot run its own `uses:` line the way your project will. If a step's shape
doesn't survive first contact, the raw recipe at the bottom is the ground
truth they both wrap.
:::

## GitHub Actions

The repo root ships a composite `action.yml` (POSIX-sh steps only — no Node,
no container), so referencing the repo *is* referencing the action:

```yaml
jobs:
  test:
    runs-on: ubuntu-latest        # or macos-latest; both work
    steps:
      - uses: actions/checkout@v5

      # Install asm-test and export PKG_CONFIG_PATH for the rest of the job.
      # The @ref pins the framework version — prefer a tag over @main.
      - uses: wilvk/asm-test@main

      # Build your suite the way the integration guide shows, then run it.
      - run: |
          cc $(pkg-config --cflags asmtest) -c my_tests.c -o my_tests.o
          cc $(pkg-config --cflags asmtest) -x assembler-with-cpp -c my_routine.s -o my_routine.o
          cc my_tests.o my_routine.o $(pkg-config --libs asmtest) -o my_tests
          ./my_tests
```

Or let the action run the suite for you, and opt into the optional tiers:

```yaml
      - uses: wilvk/asm-test@main
        with:
          # version: v1.1.0        # build a different ref than the `uses:` one
          # prefix: /opt/asmtest   # default: $HOME/.asmtest (no sudo needed)
          optional-tiers: "true"   # libunicorn + pinned Keystone/Capstone +
                                   # libasmtest_emu installed into the prefix
          test-command: "make check"
```

Inputs and outputs:

| Input | Default | Meaning |
|---|---|---|
| `version` | *(empty)* | Git ref to build. Empty = the action's own `uses:` ref (recommended — one pin, not two). |
| `prefix` | `$HOME/.asmtest` | `make install PREFIX=...` destination; `PKG_CONFIG_PATH` is exported for later steps. |
| `optional-tiers` | `"false"` | `"true"` additionally installs libunicorn (system package manager), builds the pinned Keystone + Capstone from source, installs `libasmtest_emu`, and exports `LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH`. Slower — Keystone is a trimmed LLVM (pair it with a cache if you use it per-push). |
| `test-command` | *(empty)* | Command run in your workspace after the install, with the environment above already set. |

| Output | Meaning |
|---|---|
| `prefix` | The resolved install prefix. |

## GitLab CI

[`ci/asmtest.gitlab-ci.yml`](https://github.com/wilvk/asm-test/blob/main/ci/asmtest.gitlab-ci.yml)
is an includable template with a hidden `.asmtest-install` job that clones,
installs, and exports `PKG_CONFIG_PATH` in `before_script`:

```yaml
include:
  - remote: 'https://raw.githubusercontent.com/wilvk/asm-test/main/ci/asmtest.gitlab-ci.yml'

test:
  image: gcc:13                    # any Debian/Ubuntu-based image
  extends: .asmtest-install
  variables:
    ASMTEST_VERSION: "main"        # pin a tag/SHA for reproducible CI
  script:
    - cc $(pkg-config --cflags asmtest) -c my_tests.c -o my_tests.o
    - cc $(pkg-config --cflags asmtest) -x assembler-with-cpp -c my_routine.s -o my_routine.o
    - cc my_tests.o my_routine.o $(pkg-config --libs asmtest) -o my_tests
    - ./my_tests --format=junit > report.xml || { cat report.xml; exit 1; }
  artifacts:
    when: always
    reports:
      junit: report.xml            # failures render in the MR widget
```

Notes:

- the template's `before_script` installs `build-essential git pkg-config`
  when the image lacks them (Debian/Ubuntu images; on anything else, install
  the equivalents yourself and the rest works unchanged);
- your own `before_script` would *replace* the template's — extend without
  one, or inline the template's five lines into yours;
- `ASMTEST_PREFIX` defaults to `$CI_PROJECT_DIR/.asmtest`, so you can add a
  GitLab `cache:` entry on that path to skip rebuilding per pipeline.

## Any other CI: the raw recipe

Both wrappers reduce to the [integration guide](../reference/integration.md)'s
install, which works on any Linux/macOS box with `make`, a C compiler, and
`pkg-config`:

```sh
git clone https://github.com/wilvk/asm-test
git -C asm-test checkout --detach "$PINNED_REF"
make -C asm-test install PREFIX="$HOME/.asmtest"
export PKG_CONFIG_PATH="$HOME/.asmtest/lib/pkgconfig:$PKG_CONFIG_PATH"

cc $(pkg-config --cflags asmtest) -c my_tests.c -o my_tests.o
cc $(pkg-config --cflags asmtest) -x assembler-with-cpp -c my_routine.s -o my_routine.o
cc my_tests.o my_routine.o $(pkg-config --libs asmtest) -o my_tests
./my_tests --format=junit > report.xml
```

For the optional tiers add, before the consumer build:

```sh
make -C asm-test deps DEPS_ARGS=--emu       # libunicorn + capstone + pkg-config
asm-test/scripts/build-keystone.sh          # pinned source build (no distro pkg)
asm-test/scripts/build-capstone.sh          # no-op if the distro one is present
make -C asm-test install-shared install-shared-emu PREFIX="$HOME/.asmtest"
export LD_LIBRARY_PATH="$HOME/.asmtest/lib:$LD_LIBRARY_PATH"
```

The exit code is nonzero on any failure, `--format=junit` feeds your CI's
test-report ingestion, and the [runner options](runner.md) — filtering,
`--fail-if-no-tests` (so a typo'd filter can't green-light an empty run),
timeouts, parallelism — behave the same everywhere.
