# Packaging the bindings

This page is the release guide for the ten language bindings. It documents, per
ecosystem, the **package manifest**, how the **prebuilt native libraries** are
bundled, and the **command** that assembles a publishable artifact. Each `make
<lang>-package` target assembles a real artifact that **exposes the library
module** (not the conformance runner) and **bundles every native slot present in
`build/dist/native/`** — the host slot locally, or all four when a release has
downloaded the CI `native-all` payload. The **cross-platform native payloads** a
multi-platform release needs are built in CI by the
[`payloads` matrix](#ci-builds-the-cross-platform-native-payloads), which runs the
native staging on each target OS/arch and uploads the result. What still stays out
of this repo: the credentialed release workflow that downloads those payloads and
uploads each package to its registry.

## The native-library split

A binding reaches the framework one of two ways, and that decides what its
package ships:

- **dlopen bindings** — Python, Ruby, Lua, Node, Java, .NET. They open the shared
  library at run time (`ctypes`, `Fiddle`, LuaJIT `ffi`, `koffi`, FFM, P/Invoke),
  so their package **bundles the prebuilt `libasmtest_emu`** under that fixed name.
  That lib is the **full superset** (capture trampoline + opaque-handle FFI +
  emulator + the Keystone in-line assembler + the Capstone disassembler), so a
  fresh install has **both optional tiers working out of the box** —
  `asm_available()` and `disas_available()` return true. Alongside it the
  payload vendors the three native dependencies (**Unicorn, Keystone, Capstone**)
  with the lib's runtime search path rewritten to its own directory (`$ORIGIN` on
  Linux, `@loader_path` on macOS), plus a **`THIRD-PARTY-LICENSES/`** notice — so
  the package is **self-contained** (no system libunicorn/libkeystone/libcapstone
  needed). Each ecosystem has a conventional native-payload location:

  | Language | Native payload location | Registry |
  |---|---|---|
  | Python | `asmtest/_libs/` (wheel `package_data`) | PyPI (wheel per platform tag) |
  | Ruby | `native/<plat>/` (gem files) | RubyGems |
  | Lua | `native/<plat>/` (rock `build.install.lib`) | LuaRocks |
  | Node | `native/<plat>/` (package `files`) | npm |
  | Java | `src/main/resources/native/<os>-<arch>/` (JAR) | Maven Central |
  | .NET | `runtimes/<rid>/native/` (NuGet) | NuGet |

  The `libasmtest_emu` copy is loaded by absolute path, so its install-name/soname
  does not matter; the vendored deps load via its rewritten `$ORIGIN`/`@loader_path`
  rpath. Building the payload therefore needs the full native toolchain at
  *package* time (`make deps DEPS_ARGS=--asm` plus `scripts/build-keystone.sh` +
  `scripts/build-capstone.sh`); the per-binding `make <lang>-package` targets just
  consume the staged `build/dist/native/` tree and do not rebuild it.

  **Licensing.** asm-test's own source is MIT, but because the payload conveys the
  GPL-2.0 engines (Unicorn, Keystone) as binaries dynamically linked into
  `libasmtest_emu`, **each package as distributed is effectively GPL-2.0** (MIT is
  GPL-compatible; Capstone is BSD-3-Clause). Every dlopen package therefore
  declares a compound SPDX expression (`MIT AND GPL-2.0-only AND BSD-3-Clause`) and
  ships `THIRD-PARTY-LICENSES/` with each dependency's verbatim license at the
  exact version vendored (captured by the build scripts into
  `$PREFIX/share/licenses/<dep>-<ver>/`) plus a `NOTICE` recording versions. Before
  a real publish, complete the GPL compliance checklist in
  [the implementation plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/fully-featured-packages-plan.md):
  confirm
  GPL-2.0-only vs -or-later, archive the corresponding source, and surface the
  written offer.

- **link bindings** — Rust, Zig, C++, Go. The native library is linked (or the
  build script builds/locates it), so the package is **source** and the consumer
  builds or installs `libasmtest` (`make install-shared`) — nothing prebuilt is
  bundled. Their "package" is a source distribution: a crate, a Zig package, a
  CMake/header tarball, a Go module.

## Versioning

Every package pins its version to `ASMTEST_VERSION` (`1.0.0`); the layout
manifest (`asmtest_abi.json`, bundled into the Python wheel) carries the same
version, so a mismatched native lib is detectable at load. Bump all package
manifests together with the C `ASMTEST_VERSION` on release.

## Staging the native libraries

```sh
make package-libs      # build the host's shared libs into build/dist/native/<plat>/
```

`<plat>` is `<os>-<arch>` (e.g. `darwin-arm64`, `linux-x86_64`). This stages the
unversioned `libasmtest.{so,dylib}` and `libasmtest_emu.{so,dylib}` the dlopen
bindings look up. Each `make <lang>-package` below re-stages into that language's
payload location and runs its packer, emitting under `build/dist/<lang>/`.

## CI builds the cross-platform native payloads

`make package-libs` stages only the **build host's** slot, so a release built on
one machine would ship a single-platform payload. The CI `payloads` matrix closes
that gap with no Windows/extra hardware beyond the runners the `test` job already
uses:

- **`payloads (<os>)`** runs `make deps DEPS_ARGS=--emu` then `make package-libs`
  on each of `ubuntu-latest` (linux-x86_64), `ubuntu-24.04-arm` (linux-aarch64),
  and `macos-latest` (darwin-arm64), uploading each `build/dist/native/<plat>/`
  as a `native-payload-<os>` artifact. The scarce/slow Intel-macOS corner
  (`darwin-x86_64`) builds nightly + on dispatch in `payloads (macos-13, nightly)`,
  mirroring how `test-macos-x86` gates that runner off the per-push path.
- **`payloads (collect + verify)`** downloads every `native-payload-*` artifact,
  merges them into one `build/dist/native/` tree, runs `make package-libs-verify`
  to assert each platform slot carries **both** the core and the emulator lib, and
  re-uploads the combined tree as a single `native-all` artifact. A real publish
  job downloads `native-all` and feeds each `<plat>/` into the matching binding
  package (or each ecosystem's prebuild tool) before pushing to the registry.

```sh
make package-libs-verify   # check a collected build/dist/native/ tree locally
```

## Per-language

```sh
make python-package    # py3-none-<plat> wheel -> build/dist/python/  (needs `pip install build`)
make rust-package      # crate  -> bindings/rust/target/package/  (cargo package)
make zig-package       # source tarball -> build/dist/zig/
make cpp-package       # header + CMake tarball -> build/dist/cpp/
make node-package      # npm tarball -> build/dist/node/   (npm pack)
make java-package      # jar -> build/dist/java/           (javac + jar)
make dotnet-package    # nupkg (AsmTest.dll + runtimes/<rid>/native) -> build/dist/dotnet/  (dotnet pack)
make ruby-package      # gem -> build/dist/ruby/           (gem build)
make lua-package       # rock source -> build/dist/lua/    (luarocks pack/make)
make go-package        # module check (Go modules publish from the tagged repo)
```

The plain `<lang>-package` targets above are **payload-consumers**: the dlopen
ones (python/ruby/node/java/dotnet/lua) expect `build/dist/native/` to already be
staged (by `make package-libs` locally, or a CI-downloaded `native-all` tree) and
fail fast via `native-payload-check` if it is not. This keeps a release host able
to package a downloaded multi-platform tree without the full toolchain.

For a **one-shot local build**, use the `-full` aliases — each runs every step
needed to emit a complete package on this host, staging the native payload first:

```sh
make dotnet-package-full   # = package-libs (build + stage + vendor deps) then dotnet-package
make ruby-package-full     # likewise for ruby/node/java/lua
make python-package-full   # python-package already stages, so this just forwards
```

The dlopen `-full` aliases run `package-libs`, so they need the full native
toolchain — **libunicorn + libkeystone + libcapstone** (headers and libs) plus
**patchelf** (Linux) / `install_name_tool` (macOS) for dep vendoring. Bootstrap
it with `make deps DEPS_ARGS=--asm` (and `apt-get install patchelf` on Linux).
The link/source `-full` aliases (rust/zig/cpp/go) just forward to the plain
target — those packages bundle no native payload.

`make packages` runs them all, so it needs every toolchain installed — like
`make docker-bindings` for the test side, prefer building one language at a time,
or use each binding's Docker image (`make docker-<lang>`), where the toolchain is
already present.

## What remains before a first publish

The scaffolding stops short of a credentialed, multi-platform release:

1. **Cross-platform native libs.** *Built in CI* — the `payloads` matrix above
   stages `make package-libs` on every target OS/arch and the collect job emits a
   verified `native-all` artifact spanning all four platforms. Each `make
   <lang>-package` now **consumes the collected tree**: the dlopen packers bundle
   one native slot per platform present in `build/dist/native/` (so locally they
   ship the host slot; in a release that downloads `native-all`, all four). What
   remains is the *publish-side* wiring: a release job that downloads `native-all`
   into `build/dist/native/` and runs the packers (or hands the payloads to each
   ecosystem's prebuild tool — `cibuildwheel`, `prebuildify`, Maven classifiers,
   NuGet RIDs).
2. **Wiring the library modules into each packer.** *Done* — every `make
   <lang>-package` now exposes the reusable **library module**, not the
   conformance runner: the Ruby gem ships `asmtest.rb`, the npm package `asmtest.js`
   (its `main`), the rock `asmtest.lua`, the JAR the `Asmtest` classes (compiled
   from `Asmtest.java`), and the NuGet package the `AsmTest.dll` library assembly
   (built from `Asmtest.cs` by `asmtest-lib.csproj`) — joining the
   already-library-shaped Python wheel, Rust crate, C++ header, and Go module. Each
   binding's `conformance.*` test runner is no longer shipped.
3. **A release workflow.** *Dry-run done* — [`.github/workflows/release.yml`](../.github/workflows/release.yml)
   builds the cross-platform `native-all`, then for each binding packages it,
   **installs the artifact fresh and runs a smoke test** that the bundled native
   lib resolves with `ASMTEST_LIB` unset (so an installed package works
   out of the box), and dry-run publishes (`twine check`, `npm publish --dry-run`,
   `cargo publish --dry-run`). It runs end to end on a fork with no credentials.
   Coverage:
   - **dlopen bindings** (Python, Node, Ruby, Lua, Java, .NET) install-test on a
     `[ubuntu, macos]` matrix, so the bundled-native resolution is exercised on
     both `linux-x86_64` and `darwin-arm64`.
   - **Python** is per-platform: a matrix builds the `py3-none-<plat>` wheel on
     each runner and **repairs** it into a self-contained manylinux / macOS wheel
     (`auditwheel` / `delocate`, vendoring libunicorn), so `pip install` needs no
     system libs.
   - **link bindings** (Go, C++, Zig, Rust) ship source, so the check is that the
     published source is *consumable* — the cgo module vets+builds, a C++ consumer
     compiles+links+runs against the packaged header, the Zig package builds, and
     `cargo publish --dry-run` packs the crate.

   What remains: the **live publish** is gated `if: …env.<TOKEN> != ''` per
   ecosystem (and runs once, from the Linux leg) — add `PYPI_TOKEN` / `NPM_TOKEN` /
   `RUBYGEMS_API_KEY` (and the rest) as repo secrets and tag a release to push for
   real.
