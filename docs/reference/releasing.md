# Releasing

The [`release.yml`](../../.github/workflows/release.yml) workflow assembles every
binding's package from the cross-platform native payload, installs each one fresh
(no `ASMTEST_LIB`, no system deps) and asserts the optional tiers resolve, then —
**only on a tag and only when the matching secret is present** — publishes to each
registry. On `workflow_dispatch` it runs end to end as a **dry-run** (no secrets
required), so you can validate the whole pipeline on a fork.

## Before the first real publish: the GPL gate

The packages bundle Unicorn + Keystone (GPL-2.0) binaries, so each published
package is **effectively GPL-2.0** (asm-test's own source stays MIT; Capstone is
BSD-3-Clause). The mechanical compliance is **done**:

- ✅ Verbatim license texts bundled per package (`THIRD-PARTY-LICENSES/`), at the
  version shipped — see
  [licenses/](https://github.com/wilvk/asm-test/tree/main/licenses) and
  [scripts/collect-licenses.sh](https://github.com/wilvk/asm-test/blob/main/scripts/collect-licenses.sh).
- ✅ Compound SPDX (`MIT AND GPL-2.0-only AND BSD-3-Clause`) in every manifest.
- ✅ **GPL-2.0-only** confirmed (not -or-later).
- ✅ A GPL §3(b) **written offer** in every package's `NOTICE`.
- ✅ **Corresponding source** auto-assembled and attached to the GitHub release by
  the `corresponding-source` job
  ([scripts/fetch-corresponding-source.sh](https://github.com/wilvk/asm-test/blob/main/scripts/fetch-corresponding-source.sh)).

The human decision — whether it is acceptable to distribute effectively-GPL
packages under the project's name — has been **confirmed (2026-06-26): approved**.
(The MIT-clean alternative, had it been declined, would have been the capture-only
link-binding path built with the tiers off — there is no prebuilt MIT package.)

So the GPL gate is **cleared**. What remains to publish is purely operational: add
the registry secrets below and push a version tag.

## Required secrets (per registry)

Add these as repository secrets. A registry with no secret is simply skipped — the
others still publish.

| Registry | Secret | Binding |
|---|---|---|
| PyPI | **OIDC — no secret** (Trusted Publishing) | python |
| npm | `NPM_TOKEN` (publish with `--provenance`) | node |
| RubyGems | `RUBYGEMS_API_KEY` | ruby |
| crates.io | **OIDC — no secret** (Trusted Publishing) | rust |
| NuGet | `NUGET_API_KEY` | dotnet |
| Maven Central | `MAVEN_CENTRAL_TOKEN` + `MAVEN_GPG_KEY` (+ optional `MAVEN_GPG_PASSPHRASE`) | java |

PyPI and crates.io use **OIDC Trusted Publishing** (no stored token): the workflow
mints a short-lived token per run via `id-token: write`. Each requires a one-time
**trusted-publisher registration** on the registry (owner `wilvk`, repo `asm-test`,
workflow `release.yml`; PyPI also wants a `pypi` environment) — without it the OIDC
upload is refused. npm publishes with `--provenance` (verify with
`npm audit signatures`); RubyGems/NuGet still use stored API keys.

**Maven Central** (the `maven` job) publishes the java binding via the Central Portal
publisher plugin declared in `bindings/java/pom.xml`. One-time setup: register the
namespace `io.github.wilvk` on [central.sonatype.com](https://central.sonatype.com)
(auto-granted for the GitHub username, verified via a temporary repo), then generate a
Portal **user token** and a release-signing PGP key (publish the public key to
`keyserver.ubuntu.com`). Store the token as `MAVEN_CENTRAL_TOKEN` in `<user>:<pass>`
form and the ASCII-armored private key as `MAVEN_GPG_KEY` (its passphrase, if any, as
`MAVEN_GPG_PASSPHRASE`). Without both, the deploy step no-ops; with them, a tag build
signs + uploads sources/javadoc/jar to **staging** (`autoPublish=false`) — the Portal
validation report is the acceptance test before you release the staged deployment.

## Cutting a release

1. Bump the version in [VERSION](https://github.com/wilvk/asm-test/blob/main/VERSION), then run `make sync-version` to
   propagate it to `include/asmtest.h` and every package manifest (they must
   match — CI's `check-version` gate fails on any drift; the ABI manifest is also
   checked at load). Do not hand-edit the manifests.
2. Update [CHANGELOG.md](https://github.com/wilvk/asm-test/blob/main/CHANGELOG.md).
3. Tag and push:
   ```sh
   git tag v1.1.0 && git push origin v1.1.0
   ```
4. The workflow builds the cross-platform payload, runs the fresh-install smokes,
   attaches the corresponding-source archive **and the reproducible source
   tarball** (`asm-test-<version>.tar.gz` + `SHA256SUMS`, from `make
   package-source`) to the release, and publishes to each registry whose secret
   is set. Each publish is gated on `startsWith(github.ref, 'refs/tags/')` so a
   `workflow_dispatch` never publishes. The source tarball is the digest-pinned
   asset the system-package specs (Homebrew/Debian/AUR/vcpkg/conan) consume.

Re-publishing the same version fails on most registries (NuGet uses
`--skip-duplicate`) — bump the version to re-release.

## Manual registries

- **LuaRocks** — `make lua-package` stages the rock payload; a binary rock with a
  prebuilt native payload isn't automated in `release.yml`, so publish it by hand
  once per release from an account holding an upload API key:
  ```sh
  luarocks upload bindings/lua/asmtest-1.1.0-1.rockspec --api-key="$LUAROCKS_API_KEY"
  ```
  Record the publishing account here when first used; verify afterward with
  `luarocks install asmtest`.
- **Maven Central** — **now automated** by the tag-gated `maven` job (a real
  `mvn deploy` from `bindings/java/pom.xml`, sources + javadoc + PGP signatures); it
  needs the `MAVEN_CENTRAL_TOKEN` + `MAVEN_GPG_KEY` secrets and the one-time namespace
  registration described under *Required secrets* above. `make java-package` (a real
  `mvn package`) builds the same jar locally.
- **Go** — no publish step: `proxy.golang.org` serves the module from the tagged
  repo automatically.

## System packages

The C core (the MIT static lib + headers + `asmtest.pc`) has authored,
CI-verified packaging specs for five managers under
[`packaging/`](https://github.com/wilvk/asm-test/blob/main/packaging/). Each is
built, linted, installed and consumed in a container — `make docker-syspkg-<mgr>`
locally, the `syspkg` CI job on every push. All five are **MIT only**: the static
core links nothing third-party (the GPL engines are conveyed solely by the dlopen
binding packages, not here).

**Version-bump rule.** These specs pin a *released tag's* source-tarball asset by
digest — deliberately **not** `VERSION`. `scripts/sync-version.sh` (which rewrites
the working-tree binding manifests) does **not** touch them, so bumping a package
is a **post-tag** step: once `v<x.y.z>` is tagged and `make package-source` has
attached `asm-test-<x.y.z>.tar.gz` to the release, update the pinned version, URL
and digest in each spec, regenerate the AUR `.SRCINFO`, and re-run
`make docker-syspkg` before submitting. The sha256 is the one in the release's
`SHA256SUMS`; vcpkg needs the sha512 (`sha512sum` / `vcpkg hash`).

**Per-manager submission runbook** — each is a maintainer/credential-gated step
the CI lanes deliberately do not perform:

- **Homebrew tap** — create the GitHub repo `wilvk/homebrew-asmtest` holding
  `Formula/asmtest.rb` (from
  [`packaging/homebrew/asmtest.rb`](https://github.com/wilvk/asm-test/blob/main/packaging/homebrew/asmtest.rb)).
  Users then `brew tap wilvk/asmtest && brew install asmtest`. Homebrew *core* is
  out of reach until the acceptable-formulae bar (≥90 forks OR ≥90 watchers OR
  ≥225 stars, self-submitted) is met.
- **Debian** — file a WNPP/ITP bug; build a `3.0 (quilt)` source package over the
  T7 orig tarball (`asm-test_<ver>.orig.tar.gz`); GPG-sign and `dput` to
  mentors.debian.net; file an RFS; a DD sponsors the upload; ftpmaster NEW review.
  (The CI lane uses `3.0 (native)` + `-b`; only the archive upload uses quilt.)
- **AUR** — add an SSH key to the AUR account; push the `PKGBUILD` + regenerated
  `.SRCINFO` to `ssh://aur@aur.archlinux.org/asm-test.git` `master`. No review.
- **vcpkg** — a one-port PR to microsoft/vcpkg with `vcpkg x-add-version`. The
  ≥6-month project-maturity criterion is not yet met (`v1.0.0` is 2026-06-24), so
  this waits until ~2026-12; the overlay port
  (`--overlay-ports=packaging/vcpkg/ports`) is the supported path meanwhile.
- **conan-center** — fork conan-io/conan-center-index and PR the recipe
  ([`packaging/conan/recipes/asmtest/`](https://github.com/wilvk/asm-test/blob/main/packaging/conan/recipes/asmtest/)),
  latest version only in `config.yml`; sign the first-time CLA; the PR's
  ~30-configuration CI must pass.
