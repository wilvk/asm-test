# asm-test — Fully featured packages: implementation plan

A phased roadmap for making the **published binding packages ship with both
optional native tiers working out of the box** — the in-line assembler
(Keystone) and the disassembler (Capstone) — instead of degrading to capture +
emulator only. As part of this, **Capstone is built from a pinned source release
exactly the way Keystone already is**, so neither optional tier depends on a
distro/brew package being present.

This plan is the *how + in what order*. The headline it establishes: **the
binding code already supports both tiers and already loads a fixed lib name —
so "fully featured" is a packaging-payload change (ship the superset lib and
vendor its three native deps), not a binding rewrite.**

> Status legend: **planned** unless noted. Update this file as phases land, the way
> [expansion-plan.md](expansion-plan.md) and
> [multi-language-bindings-plan.md](multi-language-bindings-plan.md) track theirs.

---

## Goals & non-goals

**Goal.** A consumer who `pip install asmtest` / `gem install asmtest` / `npm
install asmtest` (etc.) on a supported platform gets, with **no `ASMTEST_LIB`,
no system libunicorn/libkeystone/libcapstone, and no source build**:

1. capture + emulator (already shipped today), **plus**
2. the **in-line assembler** — `CallAsm` / `asm_bytes` resolve and run
   (`asm_available()` → true), and
3. the **disassembler** — `disas(bytes, off)` decodes (`disas_available()` →
   true),

with **Capstone built from a pinned source release** (`scripts/build-capstone.sh`,
the counterpart to [`scripts/build-keystone.sh`](https://github.com/wilvk/asm-test/blob/main/scripts/build-keystone.sh))
so the disassembler tier no longer relies on `libcapstone-dev` / `capstone-devel`
/ brew `capstone` existing on the build host.

**Non-goals.**

- **Changing the lean default for the *link* bindings** (Rust, Zig, C++, Go).
  Those ship *source*; the consumer builds/links `libasmtest`. They already gate
  the tiers at build time (`-DASMTEST_ENABLE_ASM` / `-DASMTEST_ENABLE_DISAS`,
  `-Dasm=true`) and are out of scope except for doc updates.
- **A combinatorial lib matrix.** We keep the existing "one superset lib carries
  everything optional" design ([`libasmtest_emu_full`](https://github.com/wilvk/asm-test/blob/main/Makefile));
  packages ship that one lib, not an emu/emu+asm/emu+disas spread.
- **RISC-V in-line assembly.** Released Keystone has no RISC-V backend; that guest
  stays "assemble unsupported," exactly as today. Capstone *does* decode RISC-V,
  so `disas` may cover guests `CallAsm` cannot — no regression either way.
- **The native Win64 tier.** Win64 packaging is tracked in
  [win64-native-tier-plan.md](win64-native-tier-plan.md); this plan is the POSIX
  (`.so`/`.dylib`) dlopen story.
- **Dropping the lean lib entirely.** `make shared-emu` and the lean
  `libasmtest_emu` stay — they're what the Docker binding images and dev loop use.
  Only the *packaged* payload changes.

---

## Where we are today

Two facts set the whole shape of this work:

1. **The packaged lib is the lean one.**
   [`package-libs`](https://github.com/wilvk/asm-test/blob/main/Makefile) depends
   on `shared shared-emu` and stages `libasmtest` + `libasmtest_emu` (Unicorn
   only — deliberately Keystone-free *and* Capstone-free). Every dlopen packer
   (`emu_lib_slots`, `python-package`) bundles `libasmtest_emu.*`. The superset
   [`libasmtest_emu_full`](https://github.com/wilvk/asm-test/blob/main/Makefile)
   (= base + `emu.o` + `fuzz.o` + `assemble.o` + `disasm.o`, linking
   `-lunicorn -lkeystone -lcapstone`) is built only for the local `*-asm-test`
   conformance targets and is **never packaged**.

2. **The binding loaders use a fixed name and already support both tiers.**
   Each dlopen binding loads `libasmtest_emu.{so,dylib}` by name from its bundled
   `native/<plat>/` dir, preferring it over the capture-only `libasmtest`
   ([`_native.py`](https://github.com/wilvk/asm-test/blob/main/bindings/python/asmtest/_native.py),
   [`asmtest.rb`](https://github.com/wilvk/asm-test/blob/main/bindings/ruby/asmtest.rb),
   [`asmtest.js`](https://github.com/wilvk/asm-test/blob/main/bindings/node/asmtest.js)).
   They bind the optional asm/disas symbols defensively and **self-skip** when the
   symbol is absent (e.g. Node's `asm_call_asm6` try/catch). So a lib whose
   *symbol set is a superset* drops straight in under the same name with no
   binding change — the probes simply start returning true.

That makes the feature gap purely a payload choice. But "fully featured" has a
second, harder half: **self-containment**. Today only the Python wheel is
self-contained — its CI step repairs the wheel with `auditwheel` (Linux) /
`delocate` (macOS), which vendors `libunicorn`. The other dlopen packages
(`ruby`, `node`, `lua`, `java`, `dotnet`) bundle only `libasmtest_emu` and
declare a **system** `libunicorn` runtime dependency
([packaging.md](../packaging.md)). Their release smoke test runs
`make deps DEPS_ARGS=--emu` *before* installing, so it proves the load path —
not that the package is self-contained. Shipping the full lib adds **two more**
shared-lib dependencies (`libkeystone`, `libcapstone`) that must travel with the
payload, so vendoring is the load-bearing part of this plan, not an afterthought.

---

## The two design decisions

### D1 — How the full lib reaches the existing fixed-name loaders

**Chosen: stage the superset lib into the payload under the `libasmtest_emu`
slot name.** `package-libs` builds `shared-emu-full` and copies the real
`libasmtest_emu_full` file into `build/dist/native/<plat>/` **renamed** to
`libasmtest_emu.{so,dylib}` (the core `libasmtest` slot is unchanged). Because
the bindings dlopen by **absolute path**, the lib's embedded install-name/soname
is irrelevant ([packaging.md](../packaging.md)), and the superset symbol set
satisfies both the required capture/emu symbols and the optional asm/disas ones.
**Zero binding-code churn**, and the lean lib name the loaders already know keeps
working.

*Rejected alternative:* teach all ten loaders to prefer a new
`libasmtest_emu_full` name with a `libasmtest_emu` fallback. Honest naming, but
ten files of churn across six languages and two static bindings for no functional
gain over D1. We can still adopt it later if the "emu slot is secretly full"
naming proves confusing — it's a pure rename on top of D1.

### D2 — How the three native deps travel with the payload

The full lib has `DT_NEEDED` / load commands for `libunicorn`, `libkeystone`,
`libcapstone`. Each ecosystem vendors them differently:

- **Python** — keep using the repair tools. Once the wheel bundles the full lib,
  `auditwheel repair` / `delocate-wheel` follow its dependencies and vendor
  **all three** transitively (plus their own deps) into the wheel, rewriting
  rpaths. This is the cleanest path and already in the pipeline — it just needs
  Keystone + Capstone present at build time so the deps are traceable.
- **Ruby / Node / Lua / Java / .NET** — no auditwheel equivalent. We add a small
  **vendor-and-patch** step (Phase 2) that copies the three resolved `.so`/`.dylib`
  files into the same `native/<plat>/` dir as the full lib and patches the full
  lib's runtime search path to its own directory (`$ORIGIN` via `patchelf
  --set-rpath` on Linux; `@loader_path` via `install_name_tool -change` /
  `-add_rpath` on macOS) so the loader finds the siblings with no system install.

---

## Phases

### Phase 0 — Capstone from source, at parity with Keystone *(the explicit ask)*

- **`scripts/build-capstone.sh`** — a near-mirror of
  [`build-keystone.sh`](https://github.com/wilvk/asm-test/blob/main/scripts/build-keystone.sh):
  `set -eu`; pinned `VERSION` (default a known-good release, e.g. `5.0.1`);
  idempotent early-exit when `pkg-config --exists capstone`; `git clone --depth 1
  --branch "$VERSION"`; CMake `-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
  -DCAPSTONE_BUILD_TESTS=OFF` with the arch set we decode (X86, ARM, AArch64,
  RISC-V); `make -j`; `sudo make install`; refresh `ldconfig`. Capstone installs
  its own `capstone.pc`, so the existing `pkg-config --exists capstone` gate in
  the Makefile and `install-deps.sh` keeps working unchanged.
- **`install-deps.sh`** — stop pointing `--asm` / `--emu` / `--all` at the distro
  `capstone_pkg`. Mirror the Keystone branch exactly: drop the per-manager
  `capstone_pkg` names (or keep them only as an opt-in override) and, where
  Capstone isn't already present via pkg-config, direct the user to
  `scripts/build-capstone.sh` — the same message shape the
  [keystone branch](https://github.com/wilvk/asm-test/blob/main/scripts/install-deps.sh)
  already prints. Update the usage/`--help` text and the header comment listing
  the optional tools.
- **`Dockerfile.bindings-asm-base`** — it currently `apt-get install`s
  `libcapstone-dev`; replace that with `./scripts/build-capstone.sh` next to the
  existing `./scripts/build-keystone.sh` line, so the image builds both engines
  from pinned source (it already has `cmake`, `git`, `python3`, `g++`, `make`).
- **CI** — anywhere the workflows install Capstone via a package
  ([`ci.yml`](https://github.com/wilvk/asm-test/blob/main/.github/workflows/ci.yml)
  bindings-asm job), switch to the script so dev, Docker, and CI all build the
  same pinned Capstone.

*Exit:* `make shared-emu-full` builds against a source-built Capstone on a host
with **no** `libcapstone-dev`; the disassembler tier no longer has a distro
dependency, matching Keystone.

### Phase 1 — Package the superset lib (feature completeness)

- **`package-libs`** — depend on `shared shared-emu-full` (build keystone +
  capstone + unicorn first). Stage `libasmtest` as today, and stage the full lib
  into the `libasmtest_emu.{so,dylib}` slot per D1. Keep `pkg_emu_name` as the
  slot the packers read so `emu_lib_slots` and `python-package` need no change.
- **`package-libs-verify`** — extend the per-platform check: each `<plat>` slot
  must carry the core lib **and** a `libasmtest_emu` whose symbol table actually
  exports the asm + disas entry points (`nm`/`objdump`/`otool` grep for
  `asmtest_emu_call_asm6` and the disas symbol), so a release can never silently
  ship the lean lib in the full slot.
- **Release/CI native jobs** — the
  [`native` matrix](https://github.com/wilvk/asm-test/blob/main/.github/workflows/release.yml)
  runs `make deps DEPS_ARGS=--emu` then `make package-libs`. Change to
  `make deps DEPS_ARGS=--asm` (pulls keystone + capstone + unicorn, using the
  Phase 0 source builds) so the full lib links on every payload runner
  (`ubuntu-latest`, `ubuntu-24.04-arm`, `macos-latest`). Same change in the
  `ci.yml` `payloads` matrix.

*Exit:* on a runner with `ASMTEST_LIB` unset, every dlopen binding loads the
bundled lib and `asm_available()` / `disas_available()` return **true** — but the
package is not yet self-contained for the non-Python bindings.

### Phase 2 — Self-containment (vendor the native deps)

- **Python** — no packer change. Ensure the `python` release job installs
  Keystone + Capstone (Phase 0 scripts) before `make python-package`, so
  `auditwheel`/`delocate` can trace and vendor all three. Bump the repair to
  expect three vendored libs.
- **Ruby / Node / Lua / Java / .NET** — add a Make helper, e.g.
  `vendor_native_deps`, invoked by `emu_lib_slots` (and the `.NET` RID loop):
  after copying `libasmtest_emu.*` into a `<plat>` dir, resolve the full lib's
  three dependencies (via `pkg-config --variable=libdir` + the soname, or
  `ldd`/`otool -L`), copy them alongside, and patch search paths to the lib's own
  directory (`patchelf --set-rpath '$ORIGIN'` on Linux; `install_name_tool` to
  rewrite each dependency to `@loader_path/<name>` and `-add_rpath @loader_path`
  on macOS). Gate the tool use per-OS; both `patchelf` and `install_name_tool`
  are cheap to add to the binding jobs.
- **package-libs-verify** — extend again: each slot must also contain the three
  vendored deps, and (Linux) the full lib's `RPATH` must be `$ORIGIN` / (macOS)
  its dependent paths must be `@loader_path`-relative, so a slot can't ship a lib
  that still points at a system path.

*Exit:* installing each package on a runner with **no** system
unicorn/keystone/capstone present still loads and runs both tiers.

### Phase 3 — Smoke tests prove the tiers, and docs

- **Release smoke tests** — today they call a no-corpus probe (`cpu_has_avx2`,
  `disasAvailable`) with `ASMTEST_LIB` unset. Strengthen each to (a) **not**
  install the system deps first (so self-containment is actually under test —
  drop the pre-install `make deps DEPS_ARGS=--emu` from the per-binding jobs that
  only need it for the load), and (b) assert `disas_available()` **and**
  `asm_available()` are true, plus one real `disas`/`CallAsm` round-trip where the
  binding's smoke harness can express it. The Java/.NET smokes already print
  `disasAvailable` — flip them from "prints a bool" to "asserts true."
- **Docs** — update [packaging.md](../packaging.md) (the dlopen bullet now ships
  the full lib + vendored unicorn/keystone/capstone; no system runtime dep),
  [bindings.md](../bindings.md) ("the optional tiers are available out of the box
  in the published packages; a lean local build still self-skips"),
  [README.md](../../README.md), and [DESIGN.md](../../DESIGN.md). Add a
  `scripts/build-capstone.sh` mention everywhere `build-keystone.sh` is cited.

*Exit:* a tagged release publishes packages that a clean machine can install and
exercise both optional tiers from, and the smoke matrix would fail if a future
change silently reverted to the lean lib or broke vendoring.

---

## Risks & mitigations

- **Payload size.** The full lib + three vendored deps (Unicorn pulls a chunk of
  LLVM-ish tables; Keystone and Capstone are sizable) materially grows every
  package. *Mitigation:* this is the cost of "fully featured." If it bites,
  reintroduce a lean variant as a separate package (`asmtest-lite`) rather than
  re-lean the default — keep the turnkey path turnkey.
- **macOS rpath fiddliness.** `install_name_tool` rewrites are the classic
  cross-platform footgun. *Mitigation:* `delocate` already does this correctly for
  Python — lean on it there, and keep the hand-rolled macOS patching (Phase 2) to
  the strictly necessary `@loader_path` rewrites, verified by
  `package-libs-verify` and the no-system-deps smoke test.
- **Keystone/Capstone build time in CI.** Two source builds per native runner add
  minutes. *Mitigation:* both scripts are idempotent via pkg-config; cache the
  install prefixes (`/usr/local/lib` + `.pc`) keyed on the pinned versions.
- **License/redistribution.** Vendoring Keystone (LLVM-derived) and Capstone
  (BSD) into published packages means *redistributing* them. *Mitigation:* confirm
  both licenses permit binary redistribution (they do — Capstone BSD-3, Keystone
  GPLv2 for the engine) and add the required notices to each package's metadata
  before any real publish. **This gate must clear before flipping the
  `if: secrets.* != ''` real-publish steps.**
- **Symbol-set drift.** If the full lib ever loses an optional symbol, the
  bindings silently self-skip again. *Mitigation:* the `package-libs-verify`
  symbol assertion (Phase 1) and the smoke-test `*_available()` assertions
  (Phase 3) both fail loudly on that.

---

## Touch list (at a glance)

| Area | File(s) | Change |
|---|---|---|
| Capstone source build | `scripts/build-capstone.sh` *(new)* | pinned CMake build, idempotent — mirror of `build-keystone.sh` |
| Deps bootstrap | `scripts/install-deps.sh` | `--asm`/`--emu`/`--all` → source-build Capstone, not distro pkg |
| Docker | `Dockerfile.bindings-asm-base` | swap `libcapstone-dev` apt for `./scripts/build-capstone.sh` |
| Packaging | `Makefile` (`package-libs`, `emu_lib_slots`, `package-libs-verify`, new `vendor_native_deps`) | stage full lib in emu slot; vendor + rpath-patch three deps; verify symbols + vendored deps |
| Release/CI | `.github/workflows/release.yml`, `ci.yml` | native jobs use `DEPS_ARGS=--asm`; smokes drop pre-install + assert `asm/disas_available()` |
| Docs | `docs/packaging.md`, `docs/bindings.md`, `README.md`, `DESIGN.md` | packages are fully featured + self-contained; cite `build-capstone.sh` |

---

## Open questions

1. **Pin which Capstone release?** `5.0.1` is the safe default; `6.x` adds guests
   but is newer. Pick at Phase 0 and pin it the way Keystone pins `0.9.2`.
2. **Keep a distro-package fast path in `install-deps.sh`?** Keystone has none, so
   strict parity means "source only." But Capstone *does* package widely, so an
   opt-in `--capstone-from-pkg` could save dev-loop build time. Default to source
   (the ask); decide whether to keep the override.
3. **Lean vs. full as the default package** — if size pushback lands, do we split
   `asmtest` (full) and `asmtest-lite` (lean), or flip the default? Decide before
   the first real publish, not after.
