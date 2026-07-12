# asm-test — Fully featured packages: implementation plan

A phased roadmap for making the **published binding packages fully featured** —
shipping the in-line assembler (Keystone) and the disassembler (Capstone) tiers
working out of the box instead of degrading to capture + emulator only. There is
**one package per binding** (no lean/core/lite split): the existing `asmtest`
package, upgraded to bundle the superset lib, **vendor its native dependencies**,
and **bundle each dependency's license at the exact version vendored**. As part
of this, **Capstone is built from a pinned source release exactly the way Keystone
already is**, so neither optional tier depends on a distro/brew package.

This plan is the *how + in what order*. The headline it establishes: **the binding
code already supports both tiers and already loads a fixed lib name — and the
superset lib is a strict symbol superset of today's payload — so "fully featured"
is a packaging-payload change (ship the superset lib, vendor its native deps, ship
their licenses), not a binding rewrite. Zero binding churn.**

One consequence to state up front: the upgraded package **bundles Unicorn and
Keystone (GPL-2.0)**, so as a binary it is **effectively GPL-2.0**. There is no
MIT-clean *prebuilt* package; MIT-clean consumption remains the *link* bindings
built from source with the optional tiers compiled out (see non-goals).

> Status legend: **planned** unless noted. Update this file as phases land, the way
> [expansion-plan.md](expansion-plan.md) and
> [multi-language-bindings-plan.md](multi-language-bindings-plan.md) track theirs.
>
> **Status: implemented (2026-06-26).** All phases landed. Capstone source build
> (`scripts/build-capstone.sh`) + license capture in both build scripts; top-level
> MIT `LICENSE`; `package-libs` stages the superset into the emu slot and
> `scripts/package-native.sh` vendors the three deps (rpath → `$ORIGIN`/`@loader_path`)
> + assembles `THIRD-PARTY-LICENSES` via `scripts/collect-licenses.sh`;
> `package-libs-verify` checks the full set; the dlopen packers were decoupled from
> `package-libs` (they consume the staged/downloaded tree); manifests declare the
> compound SPDX; CI (`ci.yml` + `release.yml`) builds Keystone+Capstone from source,
> uses `--asm` + patchelf, and the release smokes assert the tiers with no system
> deps. Validated locally end-to-end on macOS (superset link, vendoring, gem build).
> The only remaining work is the **pre-publish GPL compliance checklist** below,
> which gates publishing, not the build.

---

## Goals & non-goals

**Goal.** A consumer who `pip install asmtest` / `gem install asmtest` / `npm
install asmtest` (etc.) on a supported platform gets, with **no `ASMTEST_LIB`, no
system libunicorn/libkeystone/libcapstone, and no source build**:

1. capture + emulator (already shipped today), **plus**
2. the **in-line assembler** — `CallAsm` / `asm_bytes` resolve and run
   (`asm_available()` → true), and
3. the **disassembler** — `disas(bytes, off)` decodes (`disas_available()` →
   true),
4. a **`THIRD-PARTY-LICENSES/` notice** carrying asm-test's own MIT text plus the
   verbatim license of **each vendored native lib at the version actually
   shipped** (Unicorn, Keystone, Capstone),

with **Capstone built from a pinned source release** (`scripts/build-capstone.sh`,
the counterpart to [`scripts/build-keystone.sh`](https://github.com/wilvk/asm-test/blob/main/scripts/build-keystone.sh))
so the disassembler tier no longer relies on `libcapstone-dev` / `capstone-devel`
/ brew `capstone` existing on the build host.

**Non-goals.**

- **A lean / `core` / `lite` published variant.** One fully-featured package per
  binding. We considered a capture-only MIT package and dropped it: the non-Python
  dlopen bindings eager-bind the emulator symbols and only resolve `libasmtest_emu`
  ([asmtest.rb:73](https://github.com/wilvk/asm-test/blob/main/bindings/ruby/asmtest.rb),
  [asmtest.js:51](https://github.com/wilvk/asm-test/blob/main/bindings/node/asmtest.js),
  [Asmtest.java:133](https://github.com/wilvk/asm-test/blob/main/bindings/java/Asmtest.java),
  [Asmtest.cs:53](https://github.com/wilvk/asm-test/blob/main/bindings/dotnet/Asmtest.cs)),
  so a capture-only payload would need code changes in five bindings for little
  gain. **MIT-clean consumption stays the *link* bindings** (Rust/Zig/C++/Go) built
  from source with the tiers off (`-DASMTEST_ENABLE_ASM`/`_DISAS` unset,
  `-Dasm=false`) — inherent, not a packaged artifact.
- **Changing the *link* bindings' source-dist model.** They ship *source*; the
  consumer builds/links `libasmtest`. Out of scope except doc + license-notice
  updates. (Fully-featured packaging is a *dlopen-package* concept; link bindings
  have no prebuilt payload.)
- **A combinatorial lib matrix.** Ship the single superset
  [`libasmtest_emu_full`](https://github.com/wilvk/asm-test/blob/main/Makefile),
  not an emu/emu+asm/emu+disas spread.
- **RISC-V in-line assembly.** Released Keystone has no RISC-V backend; that guest
  stays "assemble unsupported." Capstone *does* decode RISC-V, so `disas` may cover
  guests `CallAsm` cannot — no regression either way.
- **The native Win64 tier.** Win64 packaging is tracked in
  [win64-native-tier-plan.md](win64-native-tier-plan.md); this plan is the POSIX
  (`.so`/`.dylib`) dlopen story.

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

2. **The binding loaders take a fixed name and a strict superset drops straight
   in.** Each dlopen binding loads `libasmtest_emu.{so,dylib}` by name from its
   bundled `native/<plat>/` dir, binds the emulator entry points, and binds the
   optional asm/disas symbols **defensively** — `func_opt` (Ruby), a `try/catch`
   (Node), `pcall` (Lua), a null-when-absent helper (Java/.NET), `if has_emu`
   (Python). So replacing the lean `libasmtest_emu` with the superset under the
   **same name** changes nothing at the binding layer: the required symbols are
   still present, the optional probes simply start returning true.
   **No binding code changes.**

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
machine can load it), and **license compliance** (ship the right notices). Today
only the Python wheel is even self-contained — its CI step repairs the wheel with
`auditwheel`/`delocate`, vendoring `libunicorn`; the other dlopen packages bundle
only `libasmtest_emu` and declare a **system** `libunicorn` runtime dependency
([packaging.md](../../../reference/packaging.md)).

---

## The design decisions

### D1 — How the superset lib reaches the existing fixed-name loaders

**Chosen: stage the superset lib into the payload under the `libasmtest_emu`
slot name.** `package-libs` builds `shared-emu-full` and copies the real
`libasmtest_emu_full` file into `build/dist/native/<plat>/` **renamed** to
`libasmtest_emu.{so,dylib}`. Because the bindings dlopen by **absolute path**, the
lib's embedded install-name/soname is irrelevant ([packaging.md](../../../reference/packaging.md)),
and the superset symbol set satisfies both the required capture/emu symbols and the
optional asm/disas ones. **Zero binding-code churn.**

*Rejected:* teaching all ten loaders to prefer a new `libasmtest_emu_full` name —
ten files of churn for no functional gain. Reconsider only if the "emu slot is
secretly the superset" naming proves confusing.

### D2 — How the three native deps travel with the payload

The superset lib has load commands for `libunicorn`, `libkeystone`, `libcapstone`.

- **Python** — keep the repair tools. Once the wheel bundles the superset lib,
  `auditwheel repair` / `delocate-wheel` follow its dependencies and vendor **all
  three** transitively, rewriting rpaths. Needs Keystone + Capstone present at
  build time so the deps are traceable.
- **Ruby / Node / Lua / Java / .NET** — add a **vendor-and-patch** step: copy the
  three resolved `.so`/`.dylib` files into the same `native/<plat>/` dir and patch
  the lib's runtime search path to its own directory (`$ORIGIN` via `patchelf
  --set-rpath` on Linux; `@loader_path` via `install_name_tool` on macOS) so the
  loader finds the siblings with no system install. This also closes today's
  "needs a system libunicorn" gap for the non-Python bindings.

### D3 — Licenses, captured at the version vendored

**The package ships a `THIRD-PARTY-LICENSES/` directory** (named per ecosystem
convention where one exists) containing:

- asm-test's own `LICENSE` (MIT) — *and we add the missing top-level `LICENSE`
  file to the repo as step one*, since the manifests already claim MIT,
- `unicorn-<ver>/COPYING` (GPL-2.0), `keystone-<ver>/COPYING` (GPL-2.0),
  `capstone-<ver>/LICENSE.TXT` (BSD-3-Clause),
- a `NOTICE` manifest listing each component, its **exact version**, and SPDX id.

"Same version as used" is enforced at the source: the
`build-keystone.sh` / `build-capstone.sh` scripts copy `COPYING` / `LICENSE.TXT`
out of the **pinned checkout** they just cloned into the install prefix
(`share/licenses/<dep>-<ver>/`); for Unicorn (installed via `deps`, not
source-built) the packaging records `pkg-config --modversion unicorn` and copies
that package's license. `package-libs` then collects `share/licenses/*` into the
payload slot, so the notice can never drift from the binary actually shipped.
`package-libs-verify` asserts the notice's recorded version matches
`pkg-config --modversion` for each vendored dep.

The package's registry metadata must declare the **effective binary terms** (a
compound SPDX expression), because it conveys the GPL `.so` — see the licensing
determination below — not bare `MIT`.

---

## Licensing determination & compliance checklist

**Determination (recorded, not open):** `libasmtest_emu` and
`libasmtest_emu_full` are **works based on** Unicorn — and, for the superset lib,
Keystone — under GPL-2.0. This is **direct dynamic linking, not arm's-length
use**:
[`emu.c`](https://github.com/wilvk/asm-test/blob/main/src/emu.c) includes
`<unicorn/unicorn.h>` and calls `uc_open` / `uc_hook_add` / `uc_reg_read` …
throughout, linked `-lunicorn`;
[`assemble.c`](https://github.com/wilvk/asm-test/blob/main/src/assemble.c)
includes `<keystone/keystone.h>` and calls `ks_*`, linked `-lkeystone`. There is
**no mere-aggregation argument** for these `.so`s. (Capstone —
[`disasm.c`](https://github.com/wilvk/asm-test/blob/main/src/disasm.c), `cs_*` —
is BSD-3-Clause, no copyleft.) Consequences:

- As a **binary**, `libasmtest_emu_full.{so,dylib}` is **effectively GPL-2.0**
  (MIT is GPL-compatible, so asm-test's own object code may be conveyed under
  GPL — no conflict, just a relabel). **Every published package conveys it** and
  must ship under GPL-2.0-compatible terms with full compliance.
- The capture-only `libasmtest` (`asmtest.o` + `capture.o` + `ffi.o`, no Unicorn —
  [`ffi.c`](https://github.com/wilvk/asm-test/blob/main/src/ffi.c): "Nothing here
  depends on Unicorn") carries no GPL code, but **it is not packaged** (core track
  removed). The language-binding *source* has a defensible "separate work via a
  stable ABI" argument, but that is moot for the package, which conveys the GPL
  `.so` regardless.

So this is no longer a "does GPL apply?" question — it does — only a mechanical
checklist — **all mechanical items now done** (see
[docs/releasing.md](../../../reference/releasing.md)); only the human sign-off remains:

- [x] **SPDX** reflects the *binary*: `MIT AND GPL-2.0-only AND BSD-3-Clause` in
  every manifest. GPL-2.0-**only** confirmed.
- [x] **Verbatim license text** for every conveyed component, in
  `THIRD-PARTY-LICENSES/` — the pinned texts live in [licenses/](../../../../licenses/)
  and `scripts/collect-licenses.sh` bundles them version-matched.
- [x] **Corresponding source** — `scripts/fetch-corresponding-source.sh` assembles
  the upstream source at the versions shipped and the release job attaches it; a
  GPL §3(b) **written offer** is in every `NOTICE`.
- [x] **Unmodified build confirmed** — `build-*.sh` apply no patch (they clone a
  tag and build).
- [x] **No further restrictions** — the compound SPDX + bundled GPL terms impose
  none beyond GPL.
- [x] **Registry metadata updated** from bare `MIT` to the compound SPDX.
- [x] **Human sign-off** — confirmed 2026-06-26: distributing the effectively-GPL
  packages under the project's name is **approved**.

**No prebuilt MIT fallback exists** (the `core` variant was removed). If shipping
an effectively-GPL package is unacceptable, the publish must be **blocked** — the
only MIT-clean consumption is a *link* binding built from source with the optional
tiers compiled out.

---

## Phases

### Phase 0 — Capstone from source + license capture *(the explicit ask)*

- **`scripts/build-capstone.sh`** — a near-mirror of
  [`build-keystone.sh`](https://github.com/wilvk/asm-test/blob/main/scripts/build-keystone.sh):
  `set -eu`; pinned `VERSION` (**`5.0.1`**); idempotent early-exit when
  `pkg-config --exists capstone`; `git clone --depth 1 --branch "$VERSION"`; CMake
  build with shared libs and the arch set we decode (X86, ARM, AArch64, RISC-V),
  using Capstone's own flag style (`CAPSTONE_*_SUPPORT`/`CAPSTONE_ARCHITECTURE_DEFAULT`
  — it builds all arches by default, so a plain Release build is fine); `make -j`;
  `sudo make install`; refresh `ldconfig`. Capstone installs its own `capstone.pc`,
  so the existing `pkg-config --exists capstone` gate keeps working unchanged.
- **License capture (both scripts)** — `build-keystone.sh` and the new
  `build-capstone.sh` copy the pinned checkout's license file into
  `${PREFIX}/share/licenses/<dep>-<VERSION>/` before cleanup, so "same version as
  used" is guaranteed by construction.
- **`install-deps.sh`** — **drop the per-manager `capstone_pkg` entries entirely**
  (source-only, no distro fast-path — open question #3) so Capstone exactly
  mirrors Keystone's empty package slot: `--asm` / `--emu` / `--all` direct the
  user to `scripts/build-capstone.sh` when pkg-config can't find Capstone. Update
  the usage/`--help` and header comment.
- **`Dockerfile.bindings-asm-base`** — replace the `libcapstone-dev` apt install
  with `./scripts/build-capstone.sh` next to the existing `build-keystone.sh` line
  (it already has `cmake`, `git`, `python3`, `g++`, `make`).
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
- **`package-libs`** — build `shared shared-emu-full`; stage `libasmtest` plus the
  superset lib in the `libasmtest_emu.{so,dylib}` slot (D1).
- **`package-libs-verify`** — per-platform: the staged `libasmtest_emu` must export
  the asm + disas symbols (`nm`/`objdump`/`otool` grep for `asmtest_emu_call_asm6`
  + the disas symbol), so a release can never silently ship the lean lib.
- **Release/CI native jobs** — the
  [`native` matrix](https://github.com/wilvk/asm-test/blob/main/.github/workflows/release.yml)
  and the `ci.yml` `payloads` matrix run `make deps DEPS_ARGS=--emu`; change to
  `--asm` (pulls keystone + capstone + unicorn via the Phase 0 source builds) so
  the superset lib links on every payload runner.

*Exit:* with `ASMTEST_LIB` unset, every binding's bundled lib makes
`asm_available()` / `disas_available()` return **true**. Not yet self-contained
for the non-Python bindings.

### Phase 2 — Self-containment + license bundling

- **Python** — ensure the `python` release job installs Keystone + Capstone before
  `make python-package` so `auditwheel`/`delocate` vendor all three; add the
  `THIRD-PARTY-LICENSES/` payload to the wheel `package_data` and the
  `License-File` metadata.
- **Ruby / Node / Lua / Java / .NET** — add `vendor_native_deps` (invoked by
  `emu_lib_slots` and the .NET RID loop): copy the lib's resolved deps into the
  `<plat>` dir, patch search paths to `$ORIGIN` / `@loader_path` (D2), and copy
  `share/licenses/*` into the package's `THIRD-PARTY-LICENSES/`.
- **`package-libs-verify`** — extend: each slot carries three vendored deps + their
  version-matched licenses and an `$ORIGIN`/`@loader_path`-relative rpath; version
  in each notice must equal `pkg-config --modversion`.

*Exit:* installing the package on a runner with **no** system
unicorn/keystone/capstone present still loads and runs both tiers, and ships the
matching notices.

### Phase 3 — Smoke tests prove the tiers, and docs

- **Release smoke tests** — drop the pre-install `make deps DEPS_ARGS=--emu` from
  the per-binding jobs (so self-containment is actually under test) and assert
  `disas_available()` **and** `asm_available()` are true plus one real
  `disas`/`CallAsm` round-trip. The Java/.NET smokes already print `disasAvailable`
  — flip them from "print a bool" to "assert".
- **Docs** — update [packaging.md](../../../reference/packaging.md) (the package now ships the
  superset lib + vendored deps + notices; no system runtime dep),
  [bindings.md](../../../bindings/index.md) (tiers available out of the box),
  [README.md](../../../../README.md), [DESIGN.md](../../../../DESIGN.md), and a new
  **Licensing** section noting asm-test's source is MIT while the published
  package redistributes GPL-2.0 engines (effectively GPL). Cite
  `scripts/build-capstone.sh` everywhere `build-keystone.sh` is.

*Exit:* a tagged release publishes fully-featured packages from a clean machine,
and the smoke matrix fails if a future change reverts to the lean lib, breaks
vendoring, or drops a notice.

---

## Risks & mitigations

- **GPL-2.0 redistribution (the big one).** The superset `.so` is a work based on
  Unicorn (and Keystone) via direct linking — **settled, not contested** (see the
  [licensing determination](#licensing-determination--compliance-checklist)). So
  every published package is effectively GPL-2.0. *Mitigation:* the determination
  converts this from an open legal question into a mechanical checklist (verbatim
  texts, archived corresponding source + written offer, SPDX, no further
  restrictions) that **must clear before any `if: secrets.* != ''` real-publish
  step** — mandatory, not advisory. **There is no prebuilt MIT fallback** (core
  removed): if effectively-GPL packages are unacceptable, publish is blocked and
  MIT-clean consumption stays the link-binding source build with tiers off.
- **Payload size.** Superset lib + three vendored deps (Unicorn's tables are large)
  materially grows the package. *Mitigation:* accepted cost of one fully-featured
  package; we deliberately ship no lean variant. Revisit a lean package only if
  size demonstrably bites — explicitly out of scope here.
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
- **Symbol-set drift.** If the superset lib loses an optional symbol, bindings
  silently self-skip. *Mitigation:* the symbol assertion (Phase 1) and the
  `*_available()` smoke assertions (Phase 3) fail loudly.

---

## Touch list (at a glance)

| Area | File(s) | Change |
|---|---|---|
| Capstone source build | `scripts/build-capstone.sh` *(new)* | pinned `5.0.1` CMake build, idempotent — mirror of `build-keystone.sh`; emits versioned license |
| License capture | `scripts/build-keystone.sh`, `scripts/build-capstone.sh` | copy pinned checkout's `COPYING`/`LICENSE.TXT` into `share/licenses/<dep>-<ver>/` |
| Repo license | `LICENSE` *(new)* | add the top-level MIT text the manifests already claim |
| Deps bootstrap | `scripts/install-deps.sh` | `--asm`/`--emu`/`--all` → source-build Capstone, not distro pkg (drop `capstone_pkg`) |
| Docker | `Dockerfile.bindings-asm-base` | swap `libcapstone-dev` apt for `./scripts/build-capstone.sh` |
| Packaging | `Makefile` (`package-libs`, `emu_lib_slots`, new `vendor_native_deps`, `package-libs-verify`) | stage the superset lib in the emu slot; vendor + rpath-patch deps; bundle `THIRD-PARTY-LICENSES/`; verify symbols, deps, license versions |
| Release/CI | `.github/workflows/release.yml`, `ci.yml` | native jobs use `DEPS_ARGS=--asm`; smokes drop pre-install + assert `asm/disas_available()` |
| Docs | `docs/packaging.md`, `docs/bindings.md`, `README.md`, `DESIGN.md` | fully-featured single package; self-contained; new Licensing section (effectively GPL); cite `build-capstone.sh` |

---

## Open questions

All design questions are resolved; what remains is the pre-publish legal checklist
above (GPL-2.0-only-vs-or-later, written offer + archived source), which gates
*publishing*, not *implementing*.

1. **Capstone release — RESOLVED: `5.0.1`,** pinned the way Keystone pins `0.9.2`.
2. **Variants — RESOLVED: one fully-featured package per binding,** no
   core/lite/lean split.
3. **Capstone install — RESOLVED: source-only, no distro fast-path** (parity with
   Keystone; zero dev/release version skew; idempotency + prefix caching absorb the
   one-time compile).
