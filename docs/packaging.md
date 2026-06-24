# Packaging the bindings

This page is the release guide for the ten language bindings. It documents, per
ecosystem, the **package manifest**, how the **prebuilt native libraries** are
bundled, and the **command** that assembles a publishable artifact. Everything
here is *scaffolding*: the manifests and the `make <lang>-package` targets exist
and assemble an artifact for the **host** platform; a real multi-platform release
repeats the native staging on each target OS/arch (or uses each ecosystem's
standard prebuild tooling) and uploads to the registry with credentials. No
registry credentials or cross-OS build matrices live in this repo.

## The native-library split

A binding reaches the framework one of two ways, and that decides what its
package ships:

- **dlopen bindings** — Python, Ruby, Lua, Node, Java, .NET. They open the shared
  library at run time (`ctypes`, `Fiddle`, LuaJIT `ffi`, `koffi`, FFM, P/Invoke),
  so their package **bundles the prebuilt `libasmtest_emu`** (the emulator
  superset, which also carries the capture trampoline + opaque-handle FFI layer).
  Each ecosystem has a conventional native-payload location:

  | Language | Native payload location | Registry |
  |---|---|---|
  | Python | `asmtest/_libs/` (wheel `package_data`) | PyPI (wheel per platform tag) |
  | Ruby | `native/<plat>/` (gem files) | RubyGems |
  | Lua | `native/<plat>/` (rock `build.install.lib`) | LuaRocks |
  | Node | `native/<plat>/` (package `files`) | npm |
  | Java | `src/main/resources/native/<os>-<arch>/` (JAR) | Maven Central |
  | .NET | `runtimes/<rid>/native/` (NuGet) | NuGet |

  These packages depend on **libunicorn** at run time (the emulator lib links it);
  a capture-only variant could instead bundle `libasmtest`. The `libasmtest_emu`
  copy is loaded by absolute path, so its install-name/soname does not matter.

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

## Per-language

```sh
make python-package    # wheel  -> build/dist/python/   (needs `pip install build`)
make rust-package      # crate  -> bindings/rust/target/package/  (cargo package)
make zig-package       # source tarball -> build/dist/zig/
make cpp-package       # header + CMake tarball -> build/dist/cpp/
make node-package      # npm tarball -> build/dist/node/   (npm pack)
make java-package      # jar -> build/dist/java/           (javac + jar)
make dotnet-package    # nupkg -> build/dist/dotnet/       (dotnet pack)
make ruby-package      # gem -> build/dist/ruby/           (gem build)
make lua-package       # rock source -> build/dist/lua/    (luarocks pack/make)
make go-package        # module check (Go modules publish from the tagged repo)
```

`make packages` runs them all, so it needs every toolchain installed — like
`make docker-bindings` for the test side, prefer building one language at a time,
or use each binding's Docker image (`make docker-<lang>`), where the toolchain is
already present.

## What remains before a first publish

The scaffolding stops short of a credentialed, multi-platform release:

1. **Cross-platform native libs.** `make package-libs` builds only the host's
   libs. Populate the other `native/<plat>/` (or `runtimes/<rid>/native/`) slots
   by running the staging on each OS/arch, or wire each ecosystem's prebuild tool
   (`cibuildwheel`, `prebuildify`, Maven classifiers, NuGet RIDs).
2. **A reusable library module** for the conformance-runner bindings (Node, Java,
   .NET, Ruby, Lua). Their package manifest and native layout are scaffolded
   here, but the published artifact should expose a small library module rather
   than the conformance runner; extracting it is the one remaining per-binding
   step. The library-shaped bindings (Python, Rust, Zig, C++, Go) already publish
   their module as-is.
3. **Registry credentials + a release workflow** (out of scope here; the publish
   commands above are the building blocks).
