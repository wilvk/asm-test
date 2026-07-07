# Clean-room testing

The dlopen bindings (Python, Ruby, Lua, Node, Java, .NET) ship a **bundled
native library** and load it at run time. A test that merely observes "the
import worked" proves less than it looks like it proves: on a developer machine
or a hosted CI runner, the load can be satisfied by something *other* than the
bundled payload and still print "ok". Clean-room testing makes "install fresh,
no override" mean what it says — the bundled library is asserted to be the
**only** thing that could have satisfied the load.

This page is the operator's guide to the lanes; the design history lives in the
[macOS clean-room & portability plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/macos-clean-test-plan.md)
(`docs/plans/macos-clean-test-plan.md` — the plans tree is not part of the
built docs site).

## What leaks it catches

A fresh install's load can silently resolve through any of these vectors:

- **an `ASMTEST_*` override** — `ASMTEST_LIB`, `ASMTEST_MANIFEST`, or a tier
  override (`ASMTEST_HWTRACE_LIB`, `ASMTEST_DRAPP_LIB`, `ASMTEST_DR_LIB`,
  `ASMTEST_DRCLIENT`, `DYNAMORIO_HOME`, …) left set in the environment;
- **a leaked dev `build/` tree** — several loaders' candidate chains fall
  through to `<repo>/build/` when run from inside a checkout;
- **a Homebrew or `/usr/local` copy** — on macOS an *unset*
  `DYLD_FALLBACK_LIBRARY_PATH` reverts to dyld's built-in default, which
  includes `/usr/local/lib`, so a brew `libasmtest`/`libunicorn` there can
  satisfy a bare-leaf-name load;
- **a dynamic-loader override** — `DYLD_LIBRARY_PATH`/`DYLD_INSERT_LIBRARIES`
  on macOS, `LD_LIBRARY_PATH`/`LD_PRELOAD` on Linux.

## How it works

Three small pieces compose every lane:

1. [`scripts/clean-env.sh`](https://github.com/wilvk/asm-test/blob/main/scripts/clean-env.sh)
   — a sourceable POSIX-sh scrub: unsets every `ASMTEST_*`/`DYLD_*`/`LD_*`
   override, **pins** `DYLD_FALLBACK_LIBRARY_PATH=/usr/lib` (unsetting is not
   enough — see above), strips Homebrew and `/usr/local` from `PATH`, and
   `cd`s to a scratch dir outside any checkout. Callers resolve interpreters
   to absolute paths *before* sourcing it.
2. [`scripts/assert-clean-path.sh`](https://github.com/wilvk/asm-test/blob/main/scripts/assert-clean-path.sh)
   — the guard. Each binding reports the absolute path it actually loaded
   (`python -m asmtest --where`, Ruby/Lua `library_path`, Node/Java
   `libraryPath()`, .NET `Emu.LibraryPath`); the guard rejects anything under
   the checkout, Homebrew, or `/usr/local`, and (when given a prefix) requires
   the path to live under the fresh install.
3. [`scripts/clean-room-test.sh`](https://github.com/wilvk/asm-test/blob/main/scripts/clean-room-test.sh)
   — the driver: packages each dlopen binding, installs it **fresh** into a
   throwaway prefix, runs the smoke under the scrub, and asserts the resolved
   path. Bindings whose toolchain is absent self-skip; a real leak fails the
   run.

The link bindings (C++/Rust/Go/Zig) ship source and link `libasmtest`
themselves — there is no bundled payload to leak-check, so they are out of
scope by design.

## Running it locally

```sh
make clean-room-test      # any host (Linux or macOS): package, install fresh,
                          # smoke under the scrub, assert the resolved path
make macos-clean-test     # darwin alias of the same target
make docker-clean-room    # the ruby/node/java/dotnet/lua lanes, each in its
                          # own isolated image (make docker-clean-<lang> for one)
```

The Docker lanes run with `CLEANROOM_ONLY=<lang>`, so a self-skip **fails** the
lane — a missing toolchain can't be mistaken for a pass.

Sanity-check the guard itself by making it fail on purpose:

```sh
export ASMTEST_LIB=$PWD/build/libasmtest_emu.dylib   # (or .so)
make clean-room-test      # the scrub unsets this; feed the path straight to
                          # scripts/assert-clean-path.sh to see it rejected
```

## Where CI runs it

- **Every push** — the `clean-room` job in
  [`ci.yml`](https://github.com/wilvk/asm-test/blob/main/.github/workflows/ci.yml)
  runs `make docker-clean-<lang>` for ruby/node/java/dotnet/lua.
- **The release pipeline** — every per-binding smoke in
  [`release.yml`](https://github.com/wilvk/asm-test/blob/main/.github/workflows/release.yml)
  installs the just-built artifact fresh and runs its load under
  `scripts/clean-env.sh` (Track E); the Python job additionally asserts the
  native-trace tiers resolve from inside the repaired wheel.
- **Static Mach-O assertions** (Track B) — `make package-libs-verify-macho`
  runs on the Linux collector job over every `darwin-*` payload: correct arch
  slice, `@rpath`/`@loader_path` install names (no `/Users`, `/opt/homebrew`,
  `/usr/local` baked in), and a min-OS load command.

## The macOS VM lanes (Tracks C/D — written per plan, unvalidated)

Hosted `macos-*` runners are fresh-per-job but **not pristine** — they ship
Xcode CLT and Homebrew, so a binding that accidentally depends on either passes
there and fails on a bare user Mac. Two additional lanes exist for that class;
both were **written to the plan's spec but have not yet been executed** (no
Apple-Silicon/tart or bare-metal-KVM host was available where they were
authored), so treat a first run as a shakedown:

| Lane | Target | Host needed | Entry point |
|---|---|---|---|
| **Track C** — tart ephemeral VM | vanilla **arm64** macOS (no Xcode, no brew) | Apple Silicon + [tart](https://github.com/cirruslabs/tart) + sshpass | `make osx-vm-test` ([`scripts/osx-vm.sh`](https://github.com/wilvk/asm-test/blob/main/scripts/osx-vm.sh)) |
| **Track D** — Docker-OSX | vanilla **x86-64** macOS userland | bare-metal Linux + `/dev/kvm` + sshpass | `make docker-osx-bindings` ([`scripts/docker-osx-bindings.sh`](https://github.com/wilvk/asm-test/blob/main/scripts/docker-osx-bindings.sh)) |

Both lanes copy the working tree — with **host-staged** packages (`make
packages package-libs`; the guests are toolchain-free on purpose) — into the
guest over SSH and run `clean-room-test.sh` with
`ASMTEST_CLEANROOM_PREBUILT=1`, so the guest only installs, smokes, and
asserts. Bindings whose runtime doesn't exist in a vanilla image self-skip;
that is the expected shape of a green run.

Notes the plan calls out:

- **EULA**: macOS's license permits up to 2 macOS VMs **on Apple hardware**, so
  tart-on-Mac (Track C) is above board; Docker-OSX on non-Apple hosts
  (Track D) is EULA-gray — it is an opt-in, self-hosted-only lane.
- **Track D is not a duplicate** of the per-push Rosetta CI leg: that proves
  the x86-64 *ABI* under Rosetta on Apple Silicon; this proves a *clean-room
  x86 dlopen* on a vanilla Intel-macOS userland.
- Neither lane is wired into hosted CI: Track C/D need self-hosted runners
  (`[self-hosted, macOS, arm64]` / `[self-hosted, linux, kvm]`), which this
  repo does not currently register. Wire them behind `workflow_dispatch` once
  a runner exists and the lanes have been validated.

## Related pages

- [Packaging the bindings](reference/packaging.md) — what each package bundles
  and how the native payloads are staged.
- [Portability & assemblers](reference/portability.md) — the OS/arch matrix the
  clean rooms exist to protect.
- [CI & Docker](reference/ci.md) — the full job list and the local Docker
  lanes.
