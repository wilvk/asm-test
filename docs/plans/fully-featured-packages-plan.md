# asm-test — Fully featured packages: implementation plan

A phased roadmap for making the **published binding packages ship with both
optional native tiers working out of the box** — the in-line assembler
(Keystone) and the disassembler (Capstone) — instead of degrading to capture +
emulator only. Each binding is published as **two variants — a `full` package
(the default) and a `lite` package** — every package **bundles the license text
for each native dependency at the exact version vendored**, and **Capstone is
built from a pinned source release exactly the way Keystone already is**, so
neither optional tier depends on a distro/brew package being present.

This plan is the *how + in what order*. The headline it establishes: **the
binding code already supports both tiers and already loads a fixed lib name —
so "fully featured" is a packaging-payload change (ship the superset lib, vendor
its three native deps, and ship their licenses), not a binding rewrite.**

> Status legend: **planned** unless noted. Update this file as phases land, the way
> [expansion-plan.md](expansion-plan.md) and
> [multi-language-bindings-plan.md](multi-language-bindings-plan.md) track theirs.

---

## Goals & non-goals

**Goal.** A consumer who `pip install asmtest` / `gem install asmtest` / `npm
install asmtest` (etc.) on a supported platform gets, from the **default (`full`)
package**, with **no `ASMTEST_LIB`, no system libunicorn/libkeystone/libcapstone,
and no source build**:

1. capture + emulator (already shipped today), **plus**
2. the **in-line assembler** — `CallAsm` / `asm_bytes` resolve and run
   (`asm_available()` → true), and
3. the **disassembler** — `disas(bytes, off)` decodes (`disas_available()` →
   true),
4. a **`THIRD-PARTY-LICENSES/` notice** in the package carrying asm-test's own
   MIT text plus the verbatim license of **each vendored native lib at the
   version actually shipped** (Unicorn, Keystone, Capstone), and
5. a smaller **`lite` package** (e.g. `asmtest-lite`) for consumers who want only
   capture + emulator — the current lean payload, asm/disas self-skipping —

with **Capstone built from a pinned source release** (`scripts/build-capstone.sh`,
the counterpart to [`scripts/build-keystone.sh`](https://github.com/wilvk/asm-test/blob/main/scripts/build-keystone.sh))
so the disassembler tier no longer relies on `libcapstone-dev` / `capstone-devel`
/ brew `capstone` existing on the build host.

**Non-goals.**

- **Changing the *link* bindings' source-dist model** (Rust, Zig, C++, Go). Those
  ship *source*; the consumer builds/links `libasmtest` and gates the tiers at
  build time (`-DASMTEST_ENABLE_ASM` / `-DASMTEST_ENABLE_DISAS`, `-Dasm=true`).
  Out of scope except doc + license-notice updates. (The `full`/`lite` split is a
  *dlopen-package* concept; link bindings have no prebuilt payload to split.)
- **A combinatorial lib matrix.** Two variants only — `full`
  ([`libasmtest_emu_full`](https://github.com/wilvk/asm-test/blob/main/Makefile))
  and `lite` (`libasmtest_emu`). Not an emu/emu+asm/emu+disas spread.
- **RISC-V in-line assembly.** Released Keystone has no RISC-V backend; that guest
  stays "assemble unsupported," exactly as today. Capstone *does* decode RISC-V,
  so `disas` may cover guests `CallAsm` cannot — no regression either way.
- **The native Win64 tier.** Win64 packaging is tracked in
  [win64-native-tier-plan.md](win64-native-tier-plan.md); this plan is the POSIX
  (`.so`/`.dylib`) dlopen story.
- **A GPL-free package.** Both variants bundle Unicorn (GPL-2.0); `full` adds
  Keystone (GPL-2.0). A capture-only, Unicorn-free package (`libasmtest` alone)
  would be the GPL-free option — noted as a possible third variant below, not
  built here.

---

## Where we are today

Three facts set the shape of this work:

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
   binding change — the probes simply start returning true. The same fixed name
   means a `full` and a `lite` package differ **only** in which lib (and which
   licenses) land in `native/<plat>/`; the binding code is identical.

3. **Licensing is under-specified.** Every binding manifest declares asm-test as
   **MIT** (`pyproject.toml`, `package.json`, `asmtest.gemspec`, the rockspec,
   `Cargo.toml`, the `.csproj`), but **the repo has no top-level `LICENSE` file**,
   and no package bundles the licenses of the native libs it dlopen's. Shipping
   the deps inside the payload turns that from a latent gap into a hard
   requirement — you cannot redistribute Unicorn/Keystone/Capstone binaries
   without their license text, and GPL-2.0 (Unicorn, Keystone) carries a
   corresponding-source obligation.

"Fully featured" therefore has three parts: **feature completeness** (ship the
superset lib), **self-containment** (vendor its three native deps so a clean
machine can load it), and **license compliance** (ship the right notices for what
was vendored). Today only the Python wheel is even self-contained — its CI step
repairs the wheel with `auditwheel`/`delocate`, vendoring `libunicorn`; the other
dlopen packages bundle only `libasmtest_emu` and declare a **system** `libunicorn`
runtime dependency ([packaging.md](../packaging.md)).

---

## The design decisions

### D1 — How the full lib reaches the existing fixed-name loaders

**Chosen: stage the superset lib into the payload under the `libasmtest_emu`
slot name.** For the `full` variant, `package-libs` builds `shared-emu-full` and
copies the real `libasmtest_emu_full` file into `build/dist/native/<plat>/`
**renamed** to `libasmtest_emu.{so,dylib}`; for `lite` it stages the lean
`libasmtest_emu` as today. Because the bindings dlopen by **absolute path**, the
lib's embedded install-name/soname is irrelevant
([packaging.md](../packaging.md)), and the superset symbol set satisfies both the
required capture/emu symbols and the optional asm/disas ones. **Zero
binding-code churn**, and one fixed name serves both variants.

*Rejected:* teaching all ten loaders to prefer a new `libasmtest_emu_full` name —
ten files of churn for no functional gain over D1. Reconsider only if the "emu
slot is secretly full" naming proves confusing.

### D2 — How the three native deps travel with the payload (`full` only)

The full lib has load commands for `libunicorn`, `libkeystone`, `libcapstone`.

- **Python** — keep the repair tools. Once the wheel bundles the full lib,
  `auditwheel repair` / `delocate-wheel` follow its dependencies and vendor **all
  three** transitively, rewriting rpaths. Needs Keystone + Capstone present at
  build time so the deps are traceable.
- **Ruby / Node / Lua / Java / .NET** — add a **vendor-and-patch** step: copy the
  three resolved `.so`/`.dylib` files into the same `native/<plat>/` dir and patch
  the full lib's runtime search path to its own directory (`$ORIGIN` via `patchelf
  --set-rpath` on Linux; `@loader_path` via `install_name_tool` on macOS) so the
  loader finds the siblings with no system install.

The `lite` variant vendors only `libunicorn` (its single dep) the same way —
closing the existing "needs a system libunicorn" gap for the non-Python bindings
as a side benefit.

### D3 — Licenses, captured at the version vendored

**Each package ships a `THIRD-PARTY-LICENSES/` directory** (named per ecosystem
convention where one exists) containing, for the variant:

- asm-test's own `LICENSE` (MIT) — *and we add the missing top-level `LICENSE`
  file to the repo as step one*, since the manifests already claim MIT,
- `unicorn-<ver>/COPYING` (GPL-2.0) — both variants,
- `keystone-<ver>/COPYING` + `capstone-<ver>/LICENSE.TXT` — `full` only,
- a `NOTICE` manifest listing each component, its **exact version**, and SPDX id.

"Same version as used" is enforced at the source: the
`build-keystone.sh` / `build-capstone.sh` scripts copy `COPYING` / `LICENSE.TXT`
out of the **pinned checkout** they just cloned into the install prefix
(`share/licenses/<dep>-<ver>/`); for Unicorn (installed via `deps`, not
source-built) the packaging records `pkg-config --modversion unicorn` and copies
that package's license. `package-libs` then collects `share/licenses/*` into each
payload slot, so the notice can never drift from the binary actually shipped.
`package-libs-verify` asserts the notice's recorded version matches
`pkg-config --modversion` for each vendored dep.

Each ecosystem's own license metadata stays **MIT** (asm-test's license); the
vendored libs are declared as third-party notices, not as the package's license —
except where a registry needs the *effective* distribution terms surfaced (see
the GPL risk below).

### D4 — Two variants, `full` as the default

| Variant | Package name | Native payload | Tiers | Licenses bundled |
|---|---|---|---|---|
| **`full`** *(default)* | `asmtest` | `libasmtest_emu_full` + unicorn + keystone + capstone | capture, emu, **asm**, **disas** | MIT + GPL-2.0×2 + BSD-3 |
| **`lite`** | `asmtest-lite` | `libasmtest_emu` + unicorn | capture, emu (asm/disas self-skip) | MIT + GPL-2.0 |

A `PKG_VARIANT={full,lite}` Make variable selects the staged lib, the bundled
licenses, and the package-name suffix; each `<lang>-package` target runs once per
variant. The default `make packages` / the release matrix build **both** and
publish `asmtest` (full) as the headline package. The `lite` name is per-registry
(npm/PyPI/gem/LuaRocks/Maven/NuGet) — pick one suffix (`-lite`) and apply it
uniformly. Bindings need no code change: both variants stage the same fixed lib
name; only contents differ.

---

## Phases

### Phase 0 — Capstone from source + license capture *(the explicit ask)*

- **`scripts/build-capstone.sh`** — a near-mirror of
  [`build-keystone.sh`](https://github.com/wilvk/asm-test/blob/main/scripts/build-keystone.sh):
  `set -eu`; pinned `VERSION` (default a known-good release, e.g. `5.0.1`);
  idempotent early-exit when `pkg-config --exists capstone`; `git clone --depth 1
  --branch "$VERSION"`; CMake `-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
  -DCAPSTONE_BUILD_TESTS=OFF` with the arch set we decode (X86, ARM, AArch64,
  RISC-V); `make -j`; `sudo make install`; refresh `ldconfig`. Capstone installs
  its own `capstone.pc`, so the existing `pkg-config --exists capstone` gate keeps
  working unchanged.
- **License capture (both scripts)** — `build-keystone.sh` and the new
  `build-capstone.sh` copy the pinned checkout's license file into
  `${PREFIX}/share/licenses/<dep>-<VERSION>/` before cleanup, so "same version as
  used" is guaranteed by construction.
- **`install-deps.sh`** — stop pointing `--asm` / `--emu` / `--all` at the distro
  `capstone_pkg`; mirror the Keystone branch (direct the user to
  `scripts/build-capstone.sh` when pkg-config can't find Capstone). Update the
  usage/`--help` and header comment.
- **`Dockerfile.bindings-asm-base`** — replace the `libcapstone-dev` apt install
  with `./scripts/build-capstone.sh` next to the existing `build-keystone.sh`
  line (it already has `cmake`, `git`, `python3`, `g++`, `make`).
- **CI** — switch the bindings-asm Capstone install in
  [`ci.yml`](https://github.com/wilvk/asm-test/blob/main/.github/workflows/ci.yml)
  to the script so dev, Docker, and CI build the same pinned Capstone.

*Exit:* `make shared-emu-full` builds against a source-built Capstone on a host
with **no** `libcapstone-dev`, and both pinned engines drop a versioned license
into `share/licenses/`. The disassembler tier no longer has a distro dependency,
matching Keystone.

### Phase 1 — Package the superset lib (feature completeness)

- **Repo `LICENSE`** — add the top-level MIT `LICENSE` file the manifests already
  reference (prerequisite for D3).
- **`PKG_VARIANT` + `package-libs`** — build `shared shared-emu-full` for `full`,
  `shared shared-emu` for `lite`; stage `libasmtest` plus the variant's lib in the
  `libasmtest_emu.{so,dylib}` slot (D1).
- **`package-libs-verify`** — per-platform check by variant: the `full` slot's
  `libasmtest_emu` must export the asm + disas symbols
  (`nm`/`objdump`/`otool` grep for `asmtest_emu_call_asm6` + the disas symbol);
  the `lite` slot must **not** (so the variants can't be swapped by mistake).
- **Release/CI native jobs** — the
  [`native` matrix](https://github.com/wilvk/asm-test/blob/main/.github/workflows/release.yml)
  and the `ci.yml` `payloads` matrix run `make deps DEPS_ARGS=--emu`; change the
  full-variant builds to `--asm` (pulls keystone + capstone + unicorn via the
  Phase 0 source builds) so the full lib links on every payload runner.

*Exit:* with `ASMTEST_LIB` unset, the `full` package's lib makes
`asm_available()` / `disas_available()` return **true**; `lite` still self-skips.
Not yet self-contained for the non-Python bindings.

### Phase 2 — Self-containment + license bundling

- **Python** — ensure the `python` release job installs Keystone + Capstone before
  `make python-package` so `auditwheel`/`delocate` vendor all three; add the
  `THIRD-PARTY-LICENSES/` payload to the wheel `package_data` and the
  `License-File` metadata.
- **Ruby / Node / Lua / Java / .NET** — add `vendor_native_deps` (invoked by
  `emu_lib_slots` and the .NET RID loop): copy the lib's resolved deps into the
  `<plat>` dir, patch search paths to `$ORIGIN` / `@loader_path` (D2), and copy
  `share/licenses/*` into the package's `THIRD-PARTY-LICENSES/`.
- **`package-libs-verify`** — extend: each `full` slot carries three vendored deps
  + their version-matched licenses and an `$ORIGIN`/`@loader_path`-relative
  rpath; each `lite` slot carries one. Version in each notice must equal
  `pkg-config --modversion`.

*Exit:* installing either package on a runner with **no** system
unicorn/keystone/capstone present still loads and runs its tiers, and ships the
matching notices.

### Phase 3 — Build both variants in the release matrix

- **`packages` / release matrix** — fan each binding job over
  `PKG_VARIANT ∈ {full, lite}`, producing `asmtest` and `asmtest-lite` artifacts
  per ecosystem; publish both (`full` is the headline). Per-registry `lite`
  naming applied uniformly.
- **Python** — the per-platform wheel job builds both a full and a lite wheel
  (different distribution names), each repaired independently.

*Exit:* a tagged release emits both variants for every dlopen binding.

### Phase 4 — Smoke tests prove the tiers, and docs

- **Release smoke tests** — drop the pre-install `make deps DEPS_ARGS=--emu` from
  the per-binding jobs (so self-containment is actually under test) and assert:
  for `full`, `disas_available()` **and** `asm_available()` are true plus one real
  `disas`/`CallAsm` round-trip; for `lite`, both are **false** and the calls
  self-skip. The Java/.NET smokes already print `disasAvailable` — flip them from
  "print a bool" to "assert".
- **Docs** — update [packaging.md](../packaging.md) (full/lite variants; full
  ships the superset lib + vendored deps + notices; no system runtime dep),
  [bindings.md](../bindings.md) (tiers available out of the box from the default
  package; `lite` self-skips), [README.md](../../README.md),
  [DESIGN.md](../../DESIGN.md), and a new **Licensing** section noting asm-test is
  MIT while the `full` package redistributes GPL-2.0 engines. Cite
  `scripts/build-capstone.sh` everywhere `build-keystone.sh` is.

*Exit:* a tagged release publishes both variants from a clean machine, and the
smoke matrix fails if a future change reverts to the lean lib, breaks vendoring,
or drops a notice.

---

## Risks & mitigations

- **GPL-2.0 redistribution (the big one).** The default `full` package bundles
  Unicorn **and** Keystone binaries (both GPL-2.0); even `lite` bundles Unicorn.
  Distributing GPL-2.0 binaries obliges us to offer corresponding source and
  include the license. *Mitigation:* the pinned `build-*.sh` scripts already name
  the exact upstream source (tag + repo) — record that in `NOTICE` as the
  written offer, ship the verbatim `COPYING`, and **confirm GPL-2.0 permits this
  mere-aggregation/dlopen arrangement with MIT code before flipping any
  `if: secrets.* != ''` real-publish step.** This gate is mandatory, not advisory.
  If it doesn't clear cleanly, the GPL-free capture-only variant (`libasmtest`,
  no Unicorn) becomes the fallback default.
- **Payload size.** Full lib + three vendored deps (Unicorn's tables are large)
  materially grows the default package. *Mitigation:* that is exactly why `lite`
  exists — size-sensitive consumers take `asmtest-lite`; the turnkey path stays
  turnkey.
- **License/version drift.** A notice that names the wrong version is a compliance
  bug. *Mitigation:* capture at the pinned checkout (D3) and assert
  notice-version == `pkg-config --modversion` in `package-libs-verify`.
- **macOS rpath fiddliness.** `install_name_tool` rewrites are the classic
  cross-platform footgun. *Mitigation:* lean on `delocate` for Python; keep the
  hand-rolled macOS patching to the necessary `@loader_path` rewrites, verified by
  `package-libs-verify` and the no-system-deps smoke.
- **Build time in CI.** Two source builds per native runner. *Mitigation:* both
  scripts are idempotent via pkg-config; cache the install prefixes (lib + `.pc` +
  `share/licenses`) keyed on the pinned versions.
- **Variant matrix doubling.** Every dlopen binding now builds/publishes twice.
  *Mitigation:* one `PKG_VARIANT` switch drives it; `lite` reuses today's exact
  payload, so it's mostly free.
- **Symbol-set drift.** If the full lib loses an optional symbol, bindings
  silently self-skip. *Mitigation:* the variant-aware symbol assertion (Phase 1)
  and the `*_available()` smoke assertions (Phase 4) fail loudly.

---

## Touch list (at a glance)

| Area | File(s) | Change |
|---|---|---|
| Capstone source build | `scripts/build-capstone.sh` *(new)* | pinned CMake build, idempotent — mirror of `build-keystone.sh`; emits versioned license |
| License capture | `scripts/build-keystone.sh`, `scripts/build-capstone.sh` | copy pinned checkout's `COPYING`/`LICENSE.TXT` into `share/licenses/<dep>-<ver>/` |
| Repo license | `LICENSE` *(new)* | add the top-level MIT text the manifests already claim |
| Deps bootstrap | `scripts/install-deps.sh` | `--asm`/`--emu`/`--all` → source-build Capstone, not distro pkg |
| Docker | `Dockerfile.bindings-asm-base` | swap `libcapstone-dev` apt for `./scripts/build-capstone.sh` |
| Packaging | `Makefile` (`PKG_VARIANT`, `package-libs`, `emu_lib_slots`, new `vendor_native_deps`, `package-libs-verify`, every `<lang>-package`) | stage full/lite lib in emu slot; vendor + rpath-patch deps; bundle `THIRD-PARTY-LICENSES/`; verify symbols, deps, license versions; build both variants |
| Release/CI | `.github/workflows/release.yml`, `ci.yml` | full jobs use `DEPS_ARGS=--asm`; fan over `PKG_VARIANT`; smokes drop pre-install + assert `asm/disas_available()` per variant |
| Docs | `docs/packaging.md`, `docs/bindings.md`, `README.md`, `DESIGN.md` | full (default) + lite variants; self-contained; new Licensing section; cite `build-capstone.sh` |

---

## Open questions

1. **Pin which Capstone release?** `5.0.1` is the safe default; `6.x` adds guests
   but is newer. Pick at Phase 0 and pin it the way Keystone pins `0.9.2`.
2. **Keep a distro-package fast path in `install-deps.sh`?** Keystone has none, so
   strict parity is "source only." Capstone packages widely, so an opt-in
   `--capstone-from-pkg` could save dev-loop build time. Default to source (the
   ask); decide whether to keep the override.
3. **`lite` package name** — `asmtest-lite` everywhere, or per-registry idioms
   (e.g. npm `@asmtest/lite`)? Pick one and apply uniformly.
4. **A third, GPL-free `core` variant?** Capture-only (`libasmtest`, no Unicorn)
   would be MIT-only — the answer for consumers who can't take any GPL. Worth
   shipping alongside, or only if the GPL risk gate forces it?
