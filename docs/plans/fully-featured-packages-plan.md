# asm-test — Fully featured packages: implementation plan

A phased roadmap for making the **published binding packages ship with both
optional native tiers working out of the box** — the in-line assembler
(Keystone) and the disassembler (Capstone) — instead of degrading to capture +
emulator only. Each binding is published as **two variants — a `full` package
(the default, every tier) and a `core` package (MIT-only, capture-only)** — the
`full` package **bundles the license text for each native dependency at the exact
version vendored**, and **Capstone is built from a pinned source release exactly
the way Keystone already is**, so neither optional tier depends on a distro/brew
package being present.

This plan is the *how + in what order*. The headline it establishes: **the
binding code already supports both tiers and already loads a fixed lib name —
so "fully featured" is a packaging-payload change (ship the superset lib, vendor
its three native deps, and ship their licenses), not a binding rewrite.** The two
variants split exactly on the GPL line: `full` carries the GPL engines, `core`
carries none.

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
4. a **`THIRD-PARTY-LICENSES/` notice** carrying asm-test's own MIT text plus the
   verbatim license of **each vendored native lib at the version actually
   shipped** (Unicorn, Keystone, Capstone),

with **Capstone built from a pinned source release** (`scripts/build-capstone.sh`,
the counterpart to [`scripts/build-keystone.sh`](https://github.com/wilvk/asm-test/blob/main/scripts/build-keystone.sh))
so the disassembler tier no longer relies on `libcapstone-dev` / `capstone-devel`
/ brew `capstone` existing on the build host.

And a consumer who installs the **`core` package** (e.g. `asmtest-core`) gets a
**dependency-free, MIT-only** build: native **same-arch** capture + the full
assertion surface, with the emulator/asm/disas tiers self-skipping. No Unicorn,
no GPL, nothing to vendor.

**Non-goals.**

- **Changing the *link* bindings' source-dist model** (Rust, Zig, C++, Go). Those
  ship *source*; the consumer builds/links `libasmtest` and gates the tiers at
  build time (`-DASMTEST_ENABLE_ASM` / `-DASMTEST_ENABLE_DISAS`, `-Dasm=true`).
  Out of scope except doc + license-notice updates. (The `full`/`core` split is a
  *dlopen-package* concept; link bindings have no prebuilt payload to split.)
- **A combinatorial lib matrix.** Exactly two variants — `full`
  ([`libasmtest_emu_full`](https://github.com/wilvk/asm-test/blob/main/Makefile))
  and `core` (`libasmtest`). No `lite`/emu-only middle package, no
  emu+asm/emu+disas spread.
- **RISC-V in-line assembly.** Released Keystone has no RISC-V backend; that guest
  stays "assemble unsupported," exactly as today. Capstone *does* decode RISC-V,
  so `disas` may cover guests `CallAsm` cannot — no regression either way.
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

2. **The binding loaders use a fixed name and degrade gracefully.**
   Each dlopen binding loads `libasmtest_emu.{so,dylib}` by name from its bundled
   `native/<plat>/` dir, **falling back to the capture-only `libasmtest`** when the
   emu lib is absent
   ([`_native.py`](https://github.com/wilvk/asm-test/blob/main/bindings/python/asmtest/_native.py),
   [`asmtest.rb`](https://github.com/wilvk/asm-test/blob/main/bindings/ruby/asmtest.rb),
   [`asmtest.js`](https://github.com/wilvk/asm-test/blob/main/bindings/node/asmtest.js)).
   They bind the optional emu/asm/disas symbols defensively and **self-skip** when
   a symbol is missing (`declare()` gates emu behind `if has_emu(lib)`; Node's
   `asm_call_asm6` try/catch). So a superset lib drops straight in under the emu
   name (probes start returning true), **and** a capture-only `libasmtest` is an
   already-supported state (emu/asm/disas gate off). That is exactly the `full`
   vs `core` split — `full` ships the superset under the emu slot; `core` ships
   only `libasmtest`. The binding code is identical either way.

3. **Licensing is under-specified.** Every binding manifest declares asm-test as
   **MIT** (`pyproject.toml`, `package.json`, `asmtest.gemspec`, the rockspec,
   `Cargo.toml`, the `.csproj`), but **the repo has no top-level `LICENSE` file**,
   and no package bundles the licenses of the native libs it dlopen's. Shipping
   the deps inside the `full` payload turns that from a latent gap into a hard
   requirement — you cannot redistribute Unicorn/Keystone/Capstone binaries
   without their license text, and GPL-2.0 (Unicorn, Keystone) carries a
   corresponding-source obligation.

"Fully featured" therefore has three parts for `full`: **feature completeness**
(ship the superset lib), **self-containment** (vendor its three native deps so a
clean machine can load it), and **license compliance** (ship the right notices).
`core` sidesteps all three — it has no native deps. Today only the Python wheel is
even self-contained — its CI step repairs the wheel with `auditwheel`/`delocate`,
vendoring `libunicorn`; the other dlopen packages bundle only `libasmtest_emu` and
declare a **system** `libunicorn` runtime dependency
([packaging.md](../packaging.md)).

---

## The design decisions

### D1 — How the full lib reaches the existing fixed-name loaders

**Chosen: stage the superset lib into the payload under the `libasmtest_emu`
slot name.** For the `full` variant, `package-libs` builds `shared-emu-full` and
copies the real `libasmtest_emu_full` file into `build/dist/native/<plat>/`
**renamed** to `libasmtest_emu.{so,dylib}`. For `core` it stages only the
capture-only `libasmtest.{so,dylib}` (no emu slot) — the loaders' existing
fallback finds it. Because the bindings dlopen by **absolute path**, the lib's
embedded install-name/soname is irrelevant ([packaging.md](../packaging.md)), and
the superset symbol set satisfies both the required capture symbols and the
optional emu/asm/disas ones. **Zero binding-code churn.**

*Rejected:* teaching all ten loaders to prefer a new `libasmtest_emu_full` name —
ten files of churn for no functional gain over D1. Reconsider only if the "emu
slot is secretly full" naming proves confusing.

### D2 — How the three native deps travel with the `full` payload

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

**`core` vendors nothing** — `libasmtest` has no external dependency
([`ffi.c`](https://github.com/wilvk/asm-test/blob/main/src/ffi.c): "Nothing here
depends on Unicorn"), so its payload is the single MIT `.so` and the loader needs
no rpath patching.

### D3 — Licenses, captured at the version vendored (`full` only)

**The `full` package ships a `THIRD-PARTY-LICENSES/` directory** (named per
ecosystem convention where one exists) containing:

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
`full` payload slot, so the notice can never drift from the binary actually
shipped. `package-libs-verify` asserts the notice's recorded version matches
`pkg-config --modversion` for each vendored dep.

The `full` package's registry metadata must declare the **effective binary
terms** (a compound SPDX expression), because it conveys the GPL `.so` — see the
licensing determination below. **`core` carries no third-party code, so it ships
asm-test's MIT `LICENSE` only and declares a plain `MIT`** — honestly, no
notices.

### D4 — Two variants, `full` as the default

| Variant | Package name | Native payload | Tiers | License |
|---|---|---|---|---|
| **`full`** *(default)* | `asmtest` | `libasmtest_emu_full` + unicorn + keystone + capstone | capture, emu, **asm**, **disas** | effectively `MIT AND GPL-2.0 AND BSD-3-Clause` |
| **`core`** | `asmtest-core` | `libasmtest` (no native deps) | capture only — **same-arch**; emu/asm/disas self-skip | `MIT` |

A `PKG_VARIANT={full,core}` Make variable selects the staged lib, the bundled
licenses, and the package-name suffix; each `<lang>-package` target runs once per
variant. The default `make packages` / the release matrix build **both** and
publish `asmtest` (full) as the headline package. The `core` name is per-registry
(npm/PyPI/gem/LuaRocks/Maven/NuGet) — pick one suffix (`-core`) and apply it
uniformly. Bindings need no code change: `full` populates the emu slot, `core`
ships only `libasmtest`; the loaders already handle both.

Note `core` is **narrower, not merely lighter**: it has no emulator, so no
cross-arch (AArch64/RV64/ARM32/Win64-on-SysV), no faults-as-data, no
traces/coverage/fuzz, no in-line asm, no disas. It runs a routine's real bytes on
the **host** architecture and captures state. Anyone who needs emulation takes
`full`.

---

## Licensing determination & compliance checklist

**Determination (recorded, not open):** `libasmtest_emu` and
`libasmtest_emu_full` are **works based on** Unicorn — and, for the full lib,
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
  GPL — no conflict, just a relabel). The **`full`** package conveys it and must
  ship under GPL-2.0-compatible terms with full compliance.
- The capture-only **`core`** lib (`libasmtest`: `asmtest.o` + `capture.o` +
  `ffi.o`, no Unicorn) carries **no GPL code at all** — it is the **MIT-only
  distributable** the `core` package ships. The language-binding *source* has a
  defensible "separate work via a stable ABI" argument, but that is **moot for
  `full`'s compliance**, because that package conveys the GPL `.so` regardless.

So for `full` this is no longer a "does GPL apply?" question — it does — only a
mechanical checklist to clear before any `if: secrets.* != ''` real-publish step:

- [ ] **SPDX per variant** reflects the *binary*, not just the source:
  `full` = `MIT AND GPL-2.0[-only|-or-later] AND BSD-3-Clause`; `core` = `MIT`.
  Confirm GPL-2.0 **-only** vs **-or-later** for Unicorn and Keystone and use the
  exact id.
- [ ] **Verbatim license text** for every component `full` conveys, at its
  vendored version, in `THIRD-PARTY-LICENSES/` (D3 captures it from the pinned
  checkout).
- [ ] **Corresponding source** for the GPL components *and* for asm-test's
  derivative `libasmtest_emu_full`: a written offer in `NOTICE` naming the exact
  upstream tag + commit, plus our own `build-*.sh` + CMake flags (the "scripts
  used to control compilation") — and **archive that source** rather than rely on
  upstream tags persisting for the GPL's 3-year window.
- [ ] **Unmodified build confirmed** — `build-*.sh` apply no patch (if they ever
  do, ship the modified source).
- [ ] **No further restrictions** — `full`'s registry terms can't restrict the
  GPL components beyond GPL.
- [ ] **Registry metadata updated** — `full` from bare `MIT` to the compound SPDX
  above; `core` stays `MIT`.

`core` is the answer for consumers who cannot take any GPL — shipped, not
hypothetical.

---

## Phases

### Phase 0 — Capstone from source + license capture *(the explicit ask)*

- **`scripts/build-capstone.sh`** — a near-mirror of
  [`build-keystone.sh`](https://github.com/wilvk/asm-test/blob/main/scripts/build-keystone.sh):
  `set -eu`; pinned `VERSION` (**`5.0.1`**); idempotent early-exit when
  `pkg-config --exists capstone`; `git clone --depth 1 --branch "$VERSION"`; CMake
  `-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DCAPSTONE_BUILD_TESTS=OFF`
  with the arch set we decode (X86, ARM, AArch64, RISC-V); `make -j`; `sudo make
  install`; refresh `ldconfig`. Capstone installs its own `capstone.pc`, so the
  existing `pkg-config --exists capstone` gate keeps working unchanged.
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
  with `./scripts/build-capstone.sh` next to the existing `build-keystone.sh`
  line (it already has `cmake`, `git`, `python3`, `g++`, `make`).
- **CI** — switch the bindings-asm Capstone install in
  [`ci.yml`](https://github.com/wilvk/asm-test/blob/main/.github/workflows/ci.yml)
  to the script so dev, Docker, and CI build the same pinned Capstone.

*Exit:* `make shared-emu-full` builds against a source-built Capstone on a host
with **no** `libcapstone-dev`, and both pinned engines drop a versioned license
into `share/licenses/`. The disassembler tier no longer has a distro dependency,
matching Keystone.

### Phase 1 — Package the variants (feature completeness)

- **Repo `LICENSE`** — add the top-level MIT `LICENSE` file the manifests already
  reference (prerequisite for D3, and the whole of `core`'s licensing).
- **`PKG_VARIANT` + `package-libs`** — for `full`, build `shared shared-emu-full`
  and stage the superset lib in the `libasmtest_emu.{so,dylib}` slot (D1); for
  `core`, build `shared` and stage only `libasmtest.{so,dylib}`.
- **`package-libs-verify`** — per-platform check by variant: the `full` slot's
  `libasmtest_emu` must export the emu + asm + disas symbols
  (`nm`/`objdump`/`otool` grep for `emu_open`, `asmtest_emu_call_asm6`, the disas
  symbol); the `core` slot must contain `libasmtest` and **no** emu lib, and that
  `libasmtest` must **lack** the emu symbols (so the variants can't be swapped).
- **Release/CI native jobs** — the
  [`native` matrix](https://github.com/wilvk/asm-test/blob/main/.github/workflows/release.yml)
  and the `ci.yml` `payloads` matrix run `make deps DEPS_ARGS=--emu`; the
  `full` builds need `--asm` (pulls keystone + capstone + unicorn via the Phase 0
  source builds); `core` needs no deps at all.

*Exit:* with `ASMTEST_LIB` unset, the `full` package's lib makes
`asm_available()` / `disas_available()` return **true**; `core` loads `libasmtest`
and self-skips emu/asm/disas. `full` not yet self-contained for the non-Python
bindings.

### Phase 2 — Self-containment + license bundling (`full`)

- **Python** — ensure the `python` release job installs Keystone + Capstone before
  the `full` `make python-package` so `auditwheel`/`delocate` vendor all three;
  add the `THIRD-PARTY-LICENSES/` payload to the wheel `package_data` and the
  `License-File` metadata.
- **Ruby / Node / Lua / Java / .NET** — add `vendor_native_deps` (invoked by
  `emu_lib_slots` and the .NET RID loop, **`full` only**): copy the lib's resolved
  deps into the `<plat>` dir, patch search paths to `$ORIGIN` / `@loader_path`
  (D2), and copy `share/licenses/*` into the package's `THIRD-PARTY-LICENSES/`.
- **`package-libs-verify`** — extend: each `full` slot carries three vendored deps
  + their version-matched licenses and an `$ORIGIN`/`@loader_path`-relative rpath;
  version in each notice must equal `pkg-config --modversion`. `core` slots carry
  exactly one `.so` and no third-party notices.

*Exit:* installing the `full` package on a runner with **no** system
unicorn/keystone/capstone present still loads and runs all tiers and ships the
matching notices; `core` was already self-contained (nothing to vendor).

### Phase 3 — Build both variants in the release matrix

- **`packages` / release matrix** — fan each binding job over
  `PKG_VARIANT ∈ {full, core}`, producing `asmtest` and `asmtest-core` artifacts
  per ecosystem; publish both (`full` is the headline). Per-registry `core` naming
  applied uniformly.
- **Python** — the per-platform wheel job builds both a full and a core wheel
  (different distribution names); only the full wheel is repaired/vendored.

*Exit:* a tagged release emits both variants for every dlopen binding.

### Phase 4 — Smoke tests prove the variants, and docs

- **Release smoke tests** — drop the pre-install `make deps DEPS_ARGS=--emu` from
  the per-binding jobs (so `full`'s self-containment is actually under test) and
  assert: for `full`, `disas_available()` **and** `asm_available()` are true plus
  one real `disas`/`CallAsm` round-trip; for `core`, `has_emu` is **false** and a
  same-arch `capture` round-trip succeeds. The Java/.NET smokes already print
  `disasAvailable` — flip them from "print a bool" to "assert".
- **Docs** — update [packaging.md](../packaging.md) (full/core variants; full
  ships the superset lib + vendored deps + notices and is effectively GPL; core is
  MIT, dependency-free, capture-only), [bindings.md](../bindings.md) (tiers
  available out of the box from the default package; `core` self-skips),
  [README.md](../../README.md), [DESIGN.md](../../DESIGN.md), and a new
  **Licensing** section noting asm-test's source is MIT while the `full` package
  redistributes GPL-2.0 engines. Cite `scripts/build-capstone.sh` everywhere
  `build-keystone.sh` is.

*Exit:* a tagged release publishes both variants from a clean machine, and the
smoke matrix fails if `full` reverts to the lean lib, breaks vendoring, or drops a
notice — or if `core` ever ships an emu/GPL symbol.

---

## Risks & mitigations

- **GPL-2.0 redistribution (the big one).** The full `.so` is a work based on
  Unicorn (and Keystone) via direct linking — **settled, not contested** (see the
  [licensing determination](#licensing-determination--compliance-checklist)). So
  `full` is effectively GPL-2.0 as distributed. *Mitigation:* the determination
  converts this from an open legal question into a mechanical checklist (verbatim
  texts, archived corresponding source + written offer, SPDX, no further
  restrictions) that **must clear before any `if: secrets.* != ''` real-publish
  step** — mandatory, not advisory. The only genuinely-open sub-question (is the
  binding *source* aggregated or combined?) is moot for compliance. **`core` is
  the shipped MIT-clean alternative** for consumers who cannot take GPL.
- **`core` is narrower, not just smaller.** Dropping `lite` means there is no
  emulator-without-the-tiers middle package: anyone wanting the emulator takes the
  full GPL payload. *Mitigation:* that was always the GPL boundary — `lite` was
  GPL too (Unicorn). `core` (no emulator, MIT) and `full` (everything, GPL) are
  the two honest sides of that line; a middle package only added size nuance, not
  a licensing option. Document the `core` capability envelope plainly so the
  trade-off is clear at install time.
- **Payload size of `full`.** Full lib + three vendored deps (Unicorn's tables are
  large) materially grows the default package. *Mitigation:* `core` is the small
  option for size-sensitive consumers — at the cost of the emulator, stated up
  front.
- **License/version drift.** A notice that names the wrong version is a compliance
  bug. *Mitigation:* capture at the pinned checkout (D3) and assert
  notice-version == `pkg-config --modversion` in `package-libs-verify`.
- **macOS rpath fiddliness.** `install_name_tool` rewrites are the classic
  cross-platform footgun. *Mitigation:* lean on `delocate` for Python; keep the
  hand-rolled macOS patching to the necessary `@loader_path` rewrites, verified by
  `package-libs-verify` and the no-system-deps smoke. (`core` needs none.)
- **Build time in CI.** Two source builds per `full` native runner. *Mitigation:*
  both scripts are idempotent via pkg-config; cache the install prefixes (lib +
  `.pc` + `share/licenses`) keyed on the pinned versions. `core` builds nothing
  extra.
- **Variant matrix doubling.** Every dlopen binding now builds/publishes twice.
  *Mitigation:* one `PKG_VARIANT` switch drives it; `core` reuses today's `shared`
  output with no vendoring, so it's nearly free.
- **Symbol-set drift.** If the full lib loses an optional symbol, bindings
  silently self-skip. *Mitigation:* the variant-aware symbol assertions (Phase 1)
  and the `*_available()` smoke assertions (Phase 4) fail loudly.

---

## Touch list (at a glance)

| Area | File(s) | Change |
|---|---|---|
| Capstone source build | `scripts/build-capstone.sh` *(new)* | pinned `5.0.1` CMake build, idempotent — mirror of `build-keystone.sh`; emits versioned license |
| License capture | `scripts/build-keystone.sh`, `scripts/build-capstone.sh` | copy pinned checkout's `COPYING`/`LICENSE.TXT` into `share/licenses/<dep>-<ver>/` |
| Repo license | `LICENSE` *(new)* | add the top-level MIT text the manifests already claim (also `core`'s whole license) |
| Deps bootstrap | `scripts/install-deps.sh` | `--asm`/`--emu`/`--all` → source-build Capstone, not distro pkg |
| Docker | `Dockerfile.bindings-asm-base` | swap `libcapstone-dev` apt for `./scripts/build-capstone.sh` |
| Packaging | `Makefile` (`PKG_VARIANT`, `package-libs`, `emu_lib_slots`, new `vendor_native_deps`, `package-libs-verify`, every `<lang>-package`) | stage full (emu slot) / core (`libasmtest`) per variant; vendor + rpath-patch deps for full; bundle `THIRD-PARTY-LICENSES/` for full; verify symbols, deps, license versions; build both variants |
| Release/CI | `.github/workflows/release.yml`, `ci.yml` | full jobs use `DEPS_ARGS=--asm`; fan over `PKG_VARIANT`; smokes drop pre-install + assert tiers per variant |
| Docs | `docs/packaging.md`, `docs/bindings.md`, `README.md`, `DESIGN.md` | full (default, GPL) + core (MIT) variants; self-contained; new Licensing section; cite `build-capstone.sh` |

---

## Open questions

1. **Capstone release — RESOLVED: `5.0.1`,** pinned the way Keystone pins
   `0.9.2`. (`6.x` adds guests but is newer; revisit only if a guest needs it.)
2. **Variants — RESOLVED: `full` (default) + `core`, no `lite`.** The two split on
   the GPL boundary; the emu-only middle package is dropped.
3. **Capstone install path — RESOLVED: source-only, no distro fast-path.** Parity
   with Keystone (which has no distro package at all), zero dev/release version
   skew (everyone tests the pinned `5.0.1` that ships), and the repeated cost is
   already absorbed by the script's `pkg-config` early-exit + prefix caching. A
   `--capstone-from-pkg` override would save only a comparatively cheap one-time
   Capstone compile — and only in environments that need Capstone but *not*
   Keystone, whose unavoidable LLVM-backed build dominates whenever the asm tier
   is in play. Not worth a second code path.
4. **`core` package name** — `asmtest-core` everywhere, or per-registry idioms
   (e.g. npm `@asmtest/core`)? Pick one and apply uniformly.
