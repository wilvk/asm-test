# Releasing

The [`release.yml`](../.github/workflows/release.yml) workflow assembles every
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
  version shipped — see [licenses/](../licenses/) and
  [scripts/collect-licenses.sh](../scripts/collect-licenses.sh).
- ✅ Compound SPDX (`MIT AND GPL-2.0-only AND BSD-3-Clause`) in every manifest.
- ✅ **GPL-2.0-only** confirmed (not -or-later).
- ✅ A GPL §3(b) **written offer** in every package's `NOTICE`.
- ✅ **Corresponding source** auto-assembled and attached to the GitHub release by
  the `corresponding-source` job
  ([scripts/fetch-corresponding-source.sh](../scripts/fetch-corresponding-source.sh)).

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
| PyPI | `PYPI_TOKEN` | python |
| npm | `NPM_TOKEN` | node |
| RubyGems | `RUBYGEMS_API_KEY` | ruby |
| crates.io | `CARGO_REGISTRY_TOKEN` | rust |
| NuGet | `NUGET_API_KEY` | dotnet |

## Cutting a release

1. Bump `ASMTEST_VERSION` in the [Makefile](../Makefile) and the version field in
   every package manifest (they must match; the ABI manifest is checked at load).
2. Update [CHANGELOG.md](../CHANGELOG.md).
3. Tag and push:
   ```sh
   git tag v1.0.0 && git push origin v1.0.0
   ```
4. The workflow builds the cross-platform payload, runs the fresh-install smokes,
   attaches the corresponding-source archive to the release, and publishes to each
   registry whose secret is set. Each publish is gated on
   `startsWith(github.ref, 'refs/tags/')` so a `workflow_dispatch` never publishes.

Re-publishing the same version fails on most registries (NuGet uses
`--skip-duplicate`) — bump the version to re-release.

## Manual registries

- **LuaRocks** — `make lua-package` stages the rock payload; publish with
  `luarocks upload bindings/lua/asmtest-1.0.0-1.rockspec --api-key=...` (a binary
  rock with a prebuilt native payload isn't automated here).
- **Maven Central** — the jar is built by `make java-package`; Central requires
  GPG signing + a Sonatype account, done out of band.
- **Go** — no publish step: `proxy.golang.org` serves the module from the tagged
  repo automatically.
