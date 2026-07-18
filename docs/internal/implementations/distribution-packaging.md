# Distribution: language-registry go-live and system package-manager coverage — implementation

> **Sources.** Actioned from [post-v1-expansion-plan.md](../plans/post-v1-expansion-plan.md)
> (Track A — publish the bindings), the archived
> [2026-07-04 repo review](../archive/reviews/2026-07-04-repo-review.md) (finding
> P1 — system package managers, still `⬜ Open`), the maintainer-actions section of
> [2026-07-04-plans-remaining-items.md](../analysis/2026-07-04-plans-remaining-items.md),
> and the release guides [docs/reference/packaging.md](../../reference/packaging.md)
> and [docs/reference/releasing.md](../../reference/releasing.md). Written
> 2026-07-17. If this doc and a source disagree, this doc wins (sources may be
> stale); if the CODE and this doc disagree, re-verify before implementing.

## Why this work exists

The framework is built, ten language bindings are packaged, and the release
pipeline installs and smoke-tests every one of them in CI — but **nobody can
`pip install` / `npm i` / `cargo add` it**, because no package name is
registered on any registry and no publish token exists. Separately, the C core
itself — the documented primary artifact — is installable only via
`git clone && make install`: no Homebrew formula, `.deb`, AUR `PKGBUILD`, vcpkg
port, or conan recipe exists anywhere in the repo (verified:
`grep -ril 'homebrew\|vcpkg\|conan\|PKGBUILD'` hits only prose — Homebrew
deps/env notes in docs and Makefile comments — never a packaging spec). This
doc takes both gaps to done: a credentialed registry go-live for the bindings
(T1–T6), and authored, CI-verified system packaging for the C core (T7–T13).

## What already exists (verified 2026-07-17)

The publish side is **built and repaired**; only the credentialed final step has
never run:

- [`.github/workflows/release.yml`](../../../.github/workflows/release.yml)
  (508 lines) — builds the four-platform native payload (`native` matrix, line
  35: `ubuntu-latest, ubuntu-24.04-arm, macos-latest, macos-15-intel`), merges
  and verifies it (`native-all`, lines 67–94, asserting the COMPLETE platform
  set), assembles GPL §3 corresponding source (lines 102–139), then per binding
  packages → installs fresh → clean-room smokes → dry-run publishes. Live
  publish steps are `if:`-gated to tag builds and in-step token-guarded:
  PyPI at lines 286–293 (`[ -n "$TWINE_PASSWORD" ] || skip`), npm/gem/nuget at
  lines 390–410 (once, from the Linux leg; **lua and java fall through by
  design** — line 409), crates.io at lines 429–436.
- The five 2026-07-17 pipeline fixes are landed (verified in `git log`):
  `dab1dfb` (libipt-dev is x86-only — arm64 leg), `0b2dd51` (`macos-13` →
  `macos-15-intel`), `a0ea9d0` (arm64 clean-room assert), `go vet` on
  every push (`100d259`; [ci.yml](../../../.github/workflows/ci.yml) `go-vet`
  job, lines 956–972), and the `ldd -r` drapp loadability gate
  (`91bb4f4`/`206be56`; [mk/bindings.mk](../../../mk/bindings.mk) lines
  344–353, inside `package-libs-drtrace`).
- `VERSION` is `1.1.0` but `git tag --list 'v*'` prints only `v1.0.0`
  (2026-06-24) — **the tag trigger has never fired**; every prior run was a
  `workflow_dispatch` dry-run. Matching this, [CHANGELOG.md](../../../CHANGELOG.md)
  already carries a **populated `## [1.1.0] — 2026-07-06` section** (line 1286)
  sitting below a populated `## [Unreleased]` section (line 7): 1.1.0 is
  written up but **neither released nor tagged**, so T3 folds the Unreleased
  items into that existing section rather than opening a second `## [1.1.0]`
  heading.
- Registered package names in the manifests: PyPI/npm/RubyGems **`asmtest`**
  ([bindings/python/pyproject.toml](../../../bindings/python/pyproject.toml)
  line 6, [bindings/node/package.json](../../../bindings/node/package.json)
  line 2, [bindings/ruby/asmtest.gemspec](../../../bindings/ruby/asmtest.gemspec)
  line 8), crates.io **`asm-test`**
  ([bindings/rust/Cargo.toml](../../../bindings/rust/Cargo.toml) line 2), NuGet
  **`AsmTest`** ([bindings/dotnet/asmtest-lib.csproj](../../../bindings/dotnet/asmtest-lib.csproj)
  line 24). The Go module is `github.com/wilvk/asm-test/bindings/go`
  ([bindings/go/go.mod](../../../bindings/go/go.mod) line 1) — a subdirectory
  module.
- The C core installs with `make install` (headers + `libasmtest.a` +
  `asmtest.pc`, honoring `PREFIX`/`DESTDIR` —
  [Makefile](../../../Makefile) lines 524–537); **the `v1.0.0` tag already has
  this** (`git show v1.0.0:Makefile` shows `install:` with
  `PREFIX`/`DESTDIR`/`pcdir`), so system packages can build the released tag.
- Docker lane pattern to mirror: a `Dockerfile.<lane>` plus a
  `docker-<lane>:` rule in [mk/docker.mk](../../../mk/docker.mk) that builds
  with `--build-arg BASE=$(DOCKER_BASE)` and runs `--rm` (e.g.
  `docker-drtrace`, lines 226–230; `docker-dataflow-attach`, lines 56–61).
  Version pins for fetched third parties live in the fetch scripts +
  [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt).

Prove the baseline before touching anything:

```sh
cat VERSION                      # -> 1.1.0
git tag --list 'v*'              # -> v1.0.0 (only)
make test && make check          # local core green
gh run list --workflow=release.yml -L 3
                                 # newest dispatch (2026-07-17) green on every job
```

## Tasks

### T1 — Register the five registry accounts, package names, and tokens  (S, depends on: none)

**Goal.** Every registry name the manifests declare is owned by the maintainer,
with 2FA enabled and a publish token minted.
**Steps.**
1. Check availability first (HTTP 404 ⇒ free): `https://pypi.org/pypi/asmtest/json`,
   `npm view asmtest` (want `E404`), `https://crates.io/api/v1/crates/asm-test`,
   `https://rubygems.org/api/v1/gems/asmtest.json`,
   `https://api.nuget.org/v3-flatcontainer/asmtest/index.json`.
2. If a name is taken, decide the fallback (e.g. `asm-test-framework`) **before**
   creating anything, and update only the manifest `name`/`PackageId` field, the
   matching install lines in `release.yml`'s smoke steps, and
   [packaging.md](../../reference/packaging.md) — never the import/module names.
3. Create accounts and enable 2FA per registry (see Research notes for the exact
   requirements): PyPI (TOTP/WebAuthn — mandatory), npm, crates.io (GitHub login
   `wilvk`, verified email), RubyGems, NuGet (Microsoft account, 2FA enforced).
4. Mint tokens with the narrowest scope each registry offers: PyPI
   account-scoped API token (swap to project-scoped after the first upload, or
   prefer T4's trusted publishing); npm **granular** access token with publish
   permission for `asmtest` (classic tokens are being deprecated; publish-token
   lifetime is being capped at 7 days — plan to mint fresh per release until T4
   lands); crates.io token scoped `publish-new` + `publish-update`, crate
   `asm-test` (90-day default expiry); RubyGems API key with push scope; NuGet
   API key scoped *Push new + update* with glob `AsmTest`.
**Code.** None, unless a name collision forces the manifest edit in step 2.
**Tests.** No testable surface (registry-side state). Manual verification: each
registry dashboard shows the account with 2FA on; the crates.io token list shows
the scoped token.
**Docs.** None yet (T3 updates the release docs).
**Done when.**
- All five names are confirmed free-or-owned and the decision (kept vs fallback)
  is written into the T3 changelog entry.
- Five tokens exist, scoped as above, stored in the maintainer's password
  manager (never in the repo).

### T2 — Add the token secrets and confirm a fresh dry-run green  (S, depends on: T1)

**Goal.** The exact pipeline that will carry the release is green on a run newer
than the last merge, with all five secrets in place.
**Steps.**
1. `gh secret set PYPI_TOKEN` (paste token), then likewise `NPM_TOKEN`,
   `CARGO_REGISTRY_TOKEN`, `RUBYGEMS_API_KEY`, `NUGET_API_KEY`. These are the
   exact names `release.yml` reads (lines 290, 395–397, 433) and
   [releasing.md](../../reference/releasing.md) documents.
2. Dispatch: `gh workflow run "release (dry-run)" --ref main`, then
   `gh run watch`. A dispatch **cannot publish** even with secrets present —
   every publish step is also gated `startsWith(github.ref, 'refs/tags/')`.
3. If any job fails, fix it and re-dispatch until the run is green. Do not
   proceed to T3 on a stale green run — the plan's core lesson is that a
   dispatch-only workflow decays silently (it hid four defects for three weeks).
**Code.** None expected; only fixes surfaced by the fresh run.
**Tests.** The dispatch run IS the test: all jobs green — `native` ×4,
`native (collect + verify)`, `corresponding source (GPL §3)`, `python` ×4,
`{node,ruby,lua,java,dotnet} × {ubuntu-latest, macos-latest, macos-15-intel}`,
`rust`, `go`, `cpp`, `zig`.
**Docs.** Internal-only, no user-facing docs — no behavior change.
**Done when.**
- `gh run list --workflow=release.yml -L 1` shows a green run dated **after**
  the newest commit on `main`, with all five secrets set
  (`gh secret list` shows the five names).

### T3 — Push `v1.1.0`, verify the live publish end to end  (S, depends on: T2)

**Goal.** Version 1.1.0 of every registry-wired binding is live and installable
from its registry with no local build of the C core (dlopen bindings) or with a
resolvable source package (link bindings).
**Steps.**
1. Preflight on `main`: `make check-version` (every manifest matches `VERSION`);
   fold the `## [Unreleased]` items of [CHANGELOG.md](../../../CHANGELOG.md)
   **into the already-present `## [1.1.0]` section** (line 1286) — it is
   populated but dated `2026-07-06` and was never released, so merge the
   Unreleased bullets in and refresh its date to the release date. Do **not**
   create a new `## [1.1.0]` heading: a second one would be a malformed
   Keep-a-Changelog file and wrong release notes. Add an `Added` bullet for the
   first registry publish. Commit to `main` and push.
2. Tag and push: `git tag v1.1.0 && git push origin v1.1.0`. Also push the Go
   sub-module tag the proxy needs: `git tag bindings/go/v1.1.0 &&
   git push origin bindings/go/v1.1.0` — the module lives in a subdirectory, so
   its semver tags must carry the `bindings/go/` prefix (see Research notes);
   nothing in `release.yml` does this for you.
3. Watch the **tag** run. On it, the publish steps must show real uploads —
   `twine upload`, `npm publish`, `gem push`, `dotnet nuget push`,
   `cargo publish` — not `no <TOKEN>; skip`. The `corresponding-source` job also
   creates the GitHub release and attaches the GPL §3 archive (lines 128–139).
4. Live-install verification from a clean directory, per ecosystem:
   - `python3 -m venv /tmp/v && /tmp/v/bin/pip install asmtest==1.1.0 &&
     /tmp/v/bin/python -c "import asmtest; assert asmtest.asm_available()"`
   - `cd "$(mktemp -d)" && npm init -y && npm i asmtest@1.1.0 &&
     node -e "const a=require('asmtest'); if(!a.disasAvailable()) throw 1"`
   - `gem install asmtest -v 1.1.0` + the one-liner from release.yml line 335.
   - `cd "$(mktemp -d)" && dotnet new console && dotnet add package AsmTest
     --version 1.1.0` (resolves from nuget.org).
   - `cargo add asm-test@1.1.0` in a throwaway crate resolves (building it
     needs `make install-shared-emu` — link bindings ship source by design).
   - `GOPROXY=https://proxy.golang.org go list -m
     github.com/wilvk/asm-test/bindings/go@v1.1.0` prints the version.
5. Note crates.io publishes are **permanent** (yank, never delete) and most
   registries reject re-publishing a version — a botched artifact means a
   `v1.1.1`, not a retry.
**Code.** None (tag + registry state).
**Tests.** Step 4 is the test; a failure looks like `pip`/`npm`/`gem` "not
found" or a load error from the installed package, a pass prints each smoke
line.
**Docs.** Update [packaging.md](../../reference/packaging.md)'s intro (the
"what still stays out of this repo: the credentialed release workflow" sentence
is now stale — the workflow is in-repo and has published); confirm
[releasing.md](../../reference/releasing.md)'s steps match what you actually
ran and fix any drift. CHANGELOG per step 1.
**Done when.**
- The five registry pages show version 1.1.0; all step-4 installs pass.
- The `v1.1.0` GitHub release exists with the corresponding-source assets.

### T4 — Replace long-lived tokens: PyPI Trusted Publishing, npm provenance, crates.io Trusted Publishing  (M, depends on: T3)

**Goal.** The three registries that support OIDC publish from GitHub Actions
without stored long-lived secrets, and npm packages carry provenance.
**Steps.**
1. PyPI: add a trusted publisher on the `asmtest` project — owner `wilvk`, repo
   `asm-test`, workflow `release.yml`, plus a dedicated `pypi` GitHub
   environment. In `release.yml`'s `python` job, replace the twine-token step
   with `pypa/gh-action-pypi-publish` (it performs the OIDC exchange), add
   `permissions: id-token: write` **on that job only** (the workflow default is
   `contents: read`, line 14–15), and keep the tag `if:` gate.
2. npm: switch line 402 to `npm publish --provenance` (requires the public
   `repository` URL in `package.json` to match, cloud-hosted runner — both
   hold) with `id-token: write` on the `dlopen-package` job; long-term, adopt
   npm trusted publishing when granular-token publishing is disallowed by
   default (GitHub roadmap, see Research notes).
3. crates.io: add a trusted publisher for `asm-test` (GitHub Actions only) and
   swap the `rust` job's token step for `rust-lang/crates-io-auth-action`
   (30-minute tokens) + `id-token: write`.
4. After one green tagged release on the new path (this can only be proven by
   the next real tag, e.g. `v1.1.1`), delete `PYPI_TOKEN`,
   `CARGO_REGISTRY_TOKEN`, and rotate `NPM_TOKEN` down to whatever npm still
   requires.
**Code.** `release.yml` only — keep every job on the runner labels already
there (`macos-15-intel`, never `macos-13`).
**Tests.** `workflow_dispatch` still green (the publish path is tag-gated, so
the dry-run proves everything but the OIDC exchange); the exchange itself is
verified on the next tag. `npm audit signatures` in the throwaway T3-style
install verifies the provenance attestation.
**Docs.** Update the secrets table in [releasing.md](../../reference/releasing.md)
to mark PyPI/crates.io "OIDC — no secret" and note the npm provenance flag.
Changelog `Changed` entry.
**Done when.**
- Next tagged release publishes to PyPI and crates.io with those two secrets
  deleted; `npm audit signatures` reports a verified attestation for `asmtest`.

### T5 — Widen Linux wheel compatibility (manylinux floor)  (M, depends on: T3)

**Goal.** The published Linux wheels install on distros older than the
`ubuntu-latest` build host, with the chosen manylinux floor recorded.
**Steps.**
1. Measure the status quo: download the `wheel-ubuntu-latest` artifact from the
   T2 run and read the tag auditwheel assigned (`ls *.whl`). Building on
   `ubuntu-latest` links a new glibc, so expect a high `manylinux_2_XX` tag —
   that is the compatibility ceiling for users.
2. Decide and record the floor: `manylinux_2_28` (AlmaLinux 8, GCC 14) is the
   most compatible currently-supported x86_64/aarch64 image
   (`manylinux2014` is older but its toolchain cannot build the tiers;
   `manylinux_2_34` is alpha — see Research notes).
3. Implement: either run the two Linux legs of the `python` job inside
   `quay.io/pypa/manylinux_2_28_{x86_64,aarch64}` containers (mirror the
   existing build steps; `make deps` paths change to `dnf`), or adopt
   `cibuildwheel` v4.1.0 with `CIBW_MANYLINUX_*_IMAGE=manylinux_2_28`. Prefer
   whichever keeps the existing `auditwheel --exclude` list for the
   self-contained tier libs (release.yml lines 195–198) intact — those
   exclusions are load-bearing (renaming `libdynamorio` breaks the `dladdr`
   sibling lookup).
4. Dispatch and confirm the wheel artifacts now carry `manylinux_2_28` tags and
   the clean-room tier asserts (lines 215–281) still pass.
**Code.** `release.yml` `python` job; no repo-source change expected.
**Tests.** The dispatch run; plus a container check:
`docker run --rm -v $PWD:/w almalinux:8 sh -c 'python3.12 -m pip install /w/wheelhouse/*.whl && python3.12 -c "import asmtest"'`
fails before (tag rejected) and passes after.
**Docs.** Record the floor in [packaging.md](../../reference/packaging.md)'s
Python bullet; changelog `Changed`.
**Done when.**
- Both Linux wheel artifacts are `manylinux_2_28`-tagged and install on an
  AlmaLinux 8 container; macOS legs unchanged.

### T6 — Close the fall-through registries: Maven Central leg + LuaRocks procedure  (M, depends on: T3)

**Goal.** The Java binding is publishable to Maven Central and the Lua rock has
a documented, working upload path — the two ecosystems release.yml deliberately
skips (line 409).
**Steps.**
1. Maven Central: register the namespace `io.github.wilvk` on the Central
   Portal (auto-granted for the GitHub username, verified via a temporary
   repo); generate a release-signing PGP key and publish it to
   `keyserver.ubuntu.com`. The jar's
   [`bindings/java/pom.xml`](../../../bindings/java/pom.xml) **already exists**
   and already declares `groupId` `io.github.wilvk` (line 13),
   `<description>`, `<url>`, and `<licenses>`=MIT — so **extend** it, don't
   recreate it: add the `<scm>` URLs, a license `<url>`, and the
   `maven-source`/`maven-javadoc`/`maven-gpg` plugins Central requires (the
   file's own comment at line 38 already flags these as missing). Note that
   `make java-package` builds the jar with raw `javac` + `jar cf`
   ([mk/bindings.mk](../../../mk/bindings.mk) lines 414–420) and does **not**
   consume the pom at all — wiring a real Maven build/publish that does is the
   actual work here.
2. Add a gated `maven` step or job to `release.yml` mirroring the crates.io
   shape: tag-gated, secret-guarded (`MAVEN_CENTRAL_TOKEN`, `MAVEN_GPG_KEY`),
   signing jar+pom+sources with detached ASCII-armored signatures and
   uploading via the Central Portal publisher API.
3. LuaRocks: keep it manual (a prebuilt-native binary rock is not automatable
   here — [releasing.md](../../reference/releasing.md) already says so) but
   *execute* it once: `luarocks upload bindings/lua/asmtest-1.1.0-1.rockspec
   --api-key=…`, and record the exact command + account in releasing.md's
   "Manual registries" section.
**Code.** `release.yml`; the **existing**
[`bindings/java/pom.xml`](../../../bindings/java/pom.xml) extended with the
`<scm>` URLs, license `<url>`, and Central plugins (its `groupId` is already
`io.github.wilvk`); `mk/bindings.mk`'s `java-package` (lines 414–420, today
`javac` + `jar cf`) rewired to run a real Maven build/publish that consumes the
pom (mirror how the gemspec carries metadata).
**Tests.** Dry-run: the Central Portal validates a staged bundle before
release — a dispatch uploads to staging only when the secret is present, so the
staging validation report is the test. For LuaRocks, `luarocks install asmtest`
afterward is the check.
**Docs.** releasing.md secrets table + manual-registries section; changelog.
**Done when.**
- `io.github.wilvk:asmtest:1.1.x` resolves from Maven Central in a throwaway
  Gradle/Maven project; `luarocks install asmtest` works.

### T7 — Stable source artifacts on the GitHub release (`make package-source`)  (S, depends on: none)

**Goal.** Every tagged release carries a checksummed source tarball asset, so
system packages (T8–T12) and Debian's orig-tarball flow have a stable,
digest-pinned source.
**Steps.**
1. Add `package-source` to the "Packaging & installation" section of
   [Makefile](../../../Makefile) (near `install`, line 524):
   `git archive --format=tar.gz --prefix=asm-test-$(ASMTEST_VERSION)/
   -o build/dist/asm-test-$(ASMTEST_VERSION).tar.gz HEAD` followed by a
   checksum line written to `build/dist/SHA256SUMS` (use a
   `shasum -a 256 || sha256sum` fallback — macOS ships the former, Linux the
   latter).
2. In `release.yml`'s `corresponding-source` job (it already has
   `contents: write` and runs `gh release upload`, lines 109–139), add
   `make package-source` and include the tarball + `SHA256SUMS` in the upload
   list.
3. Add a `help` line under "Packaging & install" (Makefile line 124 block).
**Code.** Makefile + release.yml as above.
**Tests.** Locally: `make package-source && tar tzf
build/dist/asm-test-1.1.0.tar.gz | head` lists `asm-test-1.1.0/Makefile`;
`(cd build/dist && shasum -a 256 -c SHA256SUMS)` passes. CI: the next dispatch
uploads the artifact; the next tag attaches both assets to the release.
**Docs.** One sentence in releasing.md's "Cutting a release" step 4; changelog.
**Done when.**
- `make package-source` emits tarball + SHA256SUMS locally; the assets appear
  on the next tagged GitHub release.

### T8 — Homebrew formula + tap, verified in a Docker lane  (M, depends on: none)

**Goal.** `brew install wilvk/asmtest/asmtest` builds the released tag and
passes `brew test`, proven in CI on Linux (Homebrew runs on Linux).
**Steps.**
1. Create `packaging/homebrew/asmtest.rb`: `class Asmtest < Formula` with
   `desc`, `homepage "https://github.com/wilvk/asm-test"`,
   `url "https://github.com/wilvk/asm-test/archive/refs/tags/v1.0.0.tar.gz"`,
   `sha256 "<computed>"` (compute: `curl -L <url> | shasum -a 256`),
   `license "MIT"` (the core conveys no GPL engine — only the dlopen *binding*
   payloads do). `def install: system "make", "install",
   "PREFIX=#{prefix}"`. `test do`: write a minimal consumer C file, compile
   with `pkg-config --cflags --libs asmtest`, run it.
2. Create `Dockerfile.syspkg-brew`: `ARG BREW_VERSION` pinned to the current
   `homebrew/brew` release tag, `FROM homebrew/brew:${BREW_VERSION}`; copy the
   formula into a local tap
   (`$(brew --repository)/Library/Taps/wilvk/homebrew-asmtest/Formula/`), then
   `brew install --build-from-source wilvk/asmtest/asmtest`,
   `brew test wilvk/asmtest/asmtest`, `brew audit --strict wilvk/asmtest/asmtest`,
   `brew style wilvk/asmtest`.
3. Add `docker-syspkg-brew` to [mk/docker.mk](../../../mk/docker.mk), mirroring
   `docker-drtrace` (lines 226–230): build with the pinned `BREW_VERSION`
   build-arg, `docker run --rm`.
4. Publish (credential/maintainer-gated): create the GitHub repo
   `wilvk/homebrew-asmtest` containing `Formula/asmtest.rb`. Homebrew **core**
   is out of reach for now — self-submitted formulae need ≥90 forks / ≥90
   watchers / ≥225 stars (see Research notes); record that threshold and
   revisit.
**Code.** As above; keep the formula building the **static core** only (the
documented primary path) — tiers stay `make`-time opt-ins.
**Tests.** `make docker-syspkg-brew` — failure: non-zero from install/test/
audit with brew's error; pass: audit clean and the `test do` binary's output.
**Docs.** T13 aggregates docs; here only a comment header in the formula.
**Done when.**
- `make docker-syspkg-brew` green on this host.
- After tap publication: `brew tap wilvk/asmtest && brew install asmtest` works
  on a Mac (manual, credential-gated).

### T9 — Debian packaging (`libasmtest-dev`), verified in a Docker lane  (M, depends on: none)

**Goal.** `dpkg-buildpackage` produces an installable, lintian-clean
`libasmtest-dev` .deb from this tree.
**Steps.**
1. Create `packaging/debian/` holding the `debian/` dir: `control` (source
   `asm-test`; binary `libasmtest-dev`, `Section: libdevel`,
   `Build-Depends: debhelper-compat (= 13)`), `rules` (dh sequencer;
   `override_dh_auto_install: make install PREFIX=/usr DESTDIR=$$(pwd)/debian/libasmtest-dev`),
   `changelog` (`asm-test (1.0.0-1)`), `copyright` (DEP-5, MIT),
   `source/format` (`3.0 (native)` for the CI lane; note in `rules` that an
   archive upload uses `3.0 (quilt)` + the T7 orig tarball), `watch`
   (GitHub tags).
2. Create `Dockerfile.syspkg-deb`: `FROM debian:bookworm-slim` (pin the tag),
   apt-install `build-essential debhelper devscripts lintian pkg-config`
   pinned via the distro (a pinned distro package suffices — CLAUDE.md), copy
   the repo, symlink `packaging/debian` to `./debian`, run
   `dpkg-buildpackage -us -uc -b`, `lintian ../*.deb`, `apt install ./../*.deb`,
   then compile+run the same pkg-config consumer as T8.
3. Add `docker-syspkg-deb` to mk/docker.mk (same shape as T8 step 3).
4. Record the archive path (maintainer-gated, no SLA): WNPP/ITP bug → GPG-signed
   source upload to mentors.debian.net via `dput` → RFS bug → DD sponsor →
   ftpmaster NEW review.
**Code.** As above. Package split stays single (`libasmtest-dev`): the shipped
artifact is a static lib + headers + .pc; no runtime `.so` package until
`install-shared` is part of the story.
**Tests.** `make docker-syspkg-deb` — failure: dpkg-buildpackage/lintian
errors; pass: lintian silent (or only overridden pedantic tags) and the
consumer binary runs.
**Docs.** T13; plus the submission path recorded in releasing.md by T13.
**Done when.**
- `make docker-syspkg-deb` green: builds, lintian-clean, installs, consumer
  compiles against `/usr/include/asmtest` via pkg-config.

### T10 — AUR `PKGBUILD` + `.SRCINFO`, verified in a Docker lane  (S, depends on: none)

**Goal.** `makepkg` builds and installs an `asm-test` package from the released
tag on Arch, namcap-clean.
**Steps.**
1. Create `packaging/aur/PKGBUILD`: `pkgname=asm-test`, `pkgver=1.0.0`,
   `pkgrel=1`, `arch=(x86_64 aarch64)`, `license=(MIT)`,
   `source=("https://github.com/wilvk/asm-test/archive/refs/tags/v$pkgver.tar.gz")`,
   `sha256sums=(<computed — same digest as T8>)`; `build() { make lib; }`,
   `check() { make check; }`, `package() { make install PREFIX=/usr
   DESTDIR="$pkgdir"; install -Dm644 LICENSE
   "$pkgdir/usr/share/licenses/$pkgname/LICENSE"; }`.
2. Generate `.SRCINFO`: `makepkg --printsrcinfo > .SRCINFO` (regenerate on
   every PKGBUILD change — AUR pushes require it).
3. Create `Dockerfile.syspkg-aur`: `FROM archlinux:base-devel-<dated tag>`
   (pin the dated tag), create a non-root `builder` user (makepkg refuses
   root), copy `packaging/aur/`, run `makepkg --noconfirm`, `namcap PKGBUILD
   *.pkg.tar.zst`, `pacman -U --noconfirm *.pkg.tar.zst`, then the shared
   pkg-config consumer smoke.
4. Add `docker-syspkg-aur` to mk/docker.mk. Publication (credential-gated):
   AUR account with an SSH public key, push PKGBUILD + regenerated `.SRCINFO`
   to `ssh://aur@aur.archlinux.org/asm-test.git` `master` — no pre-publication
   review.
**Code.** As above. The Docker lane verifies x86_64 only — the official
`archlinux` image publishes no aarch64 variant — so note in the lane's comment
that the `arch=(… aarch64)` entry is verified by running `makepkg` on an Arch
Linux ARM host, best-effort (a documented gap, not a self-skip: the x86_64 lane
still fully exercises the PKGBUILD).
**Tests.** `make docker-syspkg-aur` — namcap warnings beyond the accepted list
fail the lane (grep its output); pass ends with the consumer's output line.
**Docs.** T13.
**Done when.**
- `make docker-syspkg-aur` green; `.SRCINFO` regenerates without diff (`git
  diff --exit-code` after `makepkg --printsrcinfo`).

### T11 — vcpkg overlay port, verified in a Docker lane  (M, depends on: none)

**Goal.** `vcpkg install asmtest --overlay-ports=…` builds the released tag and
a CMake consumer links `libasmtest.a` from the vcpkg tree.
**Steps.**
1. Create `packaging/vcpkg/ports/asmtest/vcpkg.json` (`name: asmtest`,
   `version: 1.0.0`, description, homepage, `license: MIT`) and
   `portfile.cmake`: `vcpkg_from_github(OUT_SOURCE_PATH SOURCE_PATH REPO
   wilvk/asm-test REF v1.0.0 SHA512 <computed — `vcpkg hash` the tarball>)`;
   build with `vcpkg_execute_required_process(COMMAND make lib …)` in
   `${SOURCE_PATH}`; `file(INSTALL …)` the headers to
   `${CURRENT_PACKAGES_DIR}/include/asmtest` and `build/libasmtest.a` to
   `lib/`; `vcpkg_install_copyright(FILE_LIST ${SOURCE_PATH}/LICENSE)` (a
   mandatory maintainer-guide requirement); a `usage` file pointing at
   pkg-config/include.
2. Create `Dockerfile.syspkg-vcpkg`: pinned Ubuntu base
   (`--build-arg BASE=$(DOCKER_BASE)` like the core image), apt `git curl zip
   unzip tar build-essential cmake ninja-build pkg-config`, `ARG VCPKG_REF`
   pinned to a microsoft/vcpkg release tag, clone + `./bootstrap-vcpkg.sh`,
   then `vcpkg install asmtest --overlay-ports=/src/packaging/vcpkg/ports`,
   then compile a consumer against
   `~/vcpkg/installed/x64-linux/{include,lib}`.
3. Add `docker-syspkg-vcpkg` to mk/docker.mk.
4. Record the upstream path (maintainer-gated): one-port PR to
   microsoft/vcpkg with `vcpkg x-add-version`; **the maturity criterion
   (≥6 months of release/development) is not yet met** — v1.0.0 is dated
   2026-06-24 — so upstreaming waits until ~2026-12; the overlay port is the
   supported consumption story meanwhile.
**Code.** As above.
**Tests.** `make docker-syspkg-vcpkg` — failure: vcpkg build error log path
printed by vcpkg; pass: `asmtest:x64-linux` listed by `vcpkg list` and the
consumer runs.
**Docs.** T13 (installation.md documents the overlay-ports invocation).
**Done when.**
- `make docker-syspkg-vcpkg` green end to end on this host.

### T12 — Conan recipe + `test_package`, verified in a Docker lane  (M, depends on: none)

**Goal.** `conan create` builds the released tag into a Conan 2 package whose
`test_package` consumer links and runs.
**Steps.**
1. Create `packaging/conan/recipes/asmtest/all/conanfile.py`: `name =
   "asmtest"`, `license = "MIT"`, `description`/`homepage`/`url` matching
   upstream (a conan-center requirement); `conandata.yml` mapping `1.0.0` to
   the tag tarball URL + sha256 (same digest as T8/T10); `source()` via
   `get(self, **self.conan_data["sources"][self.version], strip_root=True)`;
   `build()` runs `make lib`; `package()` copies `include/*.h` →
   `include/asmtest`, `build/libasmtest.a` → `lib`, `LICENSE` → `licenses`;
   `package_info(): self.cpp_info.libs = ["asmtest"]`. Add
   `test_package/conanfile.py` + a minimal C consumer built with the CMake
   helpers.
2. Create `Dockerfile.syspkg-conan`: `FROM python:3.12-slim` (pin), apt
   `build-essential cmake pkg-config`, `pip install conan==2.<pinned exact>`,
   `conan profile detect`, then `conan create
   /src/packaging/conan/recipes/asmtest/all --version=1.0.0`.
3. Add `docker-syspkg-conan` to mk/docker.mk.
4. Record the upstream path (maintainer-gated): fork conan-io/
   conan-center-index, PR the recipe (latest version only in `config.yml`),
   first-time CLA, ~30-configuration CI.
**Code.** As above.
**Tests.** `make docker-syspkg-conan` — `conan create` runs the test_package
automatically; pass ends with the consumer's output, failure with conan's
build trace.
**Docs.** T13.
**Done when.**
- `make docker-syspkg-conan` green: package created and test_package runs.

### T13 — Aggregate lane, CI job, user docs, and the submission runbook  (S, depends on: T8, T9, T10, T11, T12)

**Goal.** One command proves all five system packagings; CI runs it on every
push; users can find the install commands; every credential-gated submission
step is written down.
**Steps.**
1. Add `docker-syspkg: docker-syspkg-brew docker-syspkg-deb docker-syspkg-aur
   docker-syspkg-vcpkg docker-syspkg-conan` to mk/docker.mk, and `.PHONY`
   entries for all six.
2. Add help lines to the Docker section of `make help`
   ([Makefile](../../../Makefile) lines 159–165):
   `docker-syspkg[-brew|-deb|-aur|-vcpkg|-conan]  system-package build+install lanes`.
3. Add a `syspkg` job to [ci.yml](../../../.github/workflows/ci.yml) mirroring
   the `taint` job shape (a job that runs `make docker-<lane>` targets, lines
   408–419): `runs-on: ubuntu-latest`, matrix over the five lane names,
   `run: make docker-syspkg-${{ matrix.mgr }}`, `timeout-minutes: 25`.
4. Docs: add an "Install from a system package manager" section to
   [installation.md](../../getting-started/installation.md) (after "Get the
   source") giving the tap/AUR/overlay/conan commands, each labelled with its
   current status (tap live vs spec-in-repo pending submission) so the page
   never overclaims. Extend [releasing.md](../../reference/releasing.md) with
   a "System packages" section: the per-manager submission runbook (tap repo
   push; Debian ITP→mentors→RFS; AUR SSH push; vcpkg PR after the 2026-12
   maturity date; conan-center PR + CLA) and the version-bump rule — **the
   packaging specs pin released tags, not `VERSION`**, so bumping them is a
   post-tag step (they are deliberately NOT wired into
   `scripts/sync-version.sh`, which rewrites working-tree manifests).
5. Changelog: one `Added` entry covering packaging specs + lanes.
**Code.** mk/docker.mk, Makefile help, ci.yml, two docs pages.
**Tests.** `make docker-syspkg` runs all five lanes serially and exits 0;
`make help | grep syspkg` shows the lines; the ci.yml job is green on the next
push.
**Docs.** As step 4 (this task IS the docs task); changelog step 5.
**Done when.**
- `make docker-syspkg` green locally; CI `syspkg` matrix green on push.
- installation.md and releasing.md render in `make docker-docs` with no `-W`
  warnings.

## Task order & parallelism

Two independent streams share only the release-artifact story:

- **Registry go-live (serial):** T1 → T2 → T3, then T4 / T5 / T6 in any order
  (all three touch `release.yml` — coordinate merges, but they are logically
  independent).
- **System packages (parallel):** T7, T8, T9, T10, T11, T12 are mutually
  independent (five people could take one each); T13 last.
- **Critical path:** T1 → T2 → T3 (the adoption lever). The syspkg stream can
  start immediately against `v1.0.0` and later re-pin to `v1.1.0` (a
  URL+digest+version bump per spec, per T13's runbook rule).

## Constraints & gates

- **Credential gates (real, per CLAUDE.md):** registry accounts/tokens (T1–T6),
  the tap/AUR/Debian/vcpkg/conan submission accounts and PRs (T8–T12 step 4,
  T13 runbook). Record each in releasing.md; everything else — authoring and CI
  verification — is unblocked and must not self-skip.
- **Pinning:** every Dockerfile pins its base tag and tool versions
  (`BREW_VERSION`, `VCPKG_REF`, `conan==2.x.y`, dated `archlinux` tag);
  the packaging specs pin the release tarball by sha256/sha512 in-spec. The
  `scripts/third-party-digests.txt` pattern is for fetched third-party
  engines and is not extended here — the pinned artifact is our own tag.
- **License:** the C core packages are **MIT only** (no GPL engines are
  conveyed — `libasmtest.a` links nothing third-party). Do not copy the
  compound `MIT AND GPL-2.0-only AND BSD-3-Clause` expression from the dlopen
  binding packages ([packaging.md](../../reference/packaging.md)) into any
  syspkg spec.
- **CI runners:** any workflow edit keeps `macos-15-intel` for Intel-mac legs —
  `macos-13` was retired 2025-12-08 and must never be reintroduced.
- **Publish permanence:** crates.io cannot delete (only yank); most registries
  reject same-version re-uploads — a broken 1.1.0 artifact means v1.1.1.

## Research notes (verified 2026-07-17)

- **PyPI**: 2FA mandatory for all users since 2024-01-01 (TOTP/WebAuthn)
  (<https://blog.pypi.org/posts/2023-12-13-2fa-enforcement/>). Trusted
  Publishing mints 15-minute tokens via OIDC; GitHub config = owner + repo +
  workflow filename (+ recommended environment); GitHub Actions / GitLab.com /
  Google Cloud / ActiveState only (<https://docs.pypi.org/trusted-publishers/>,
  <https://docs.pypi.org/trusted-publishers/adding-a-publisher/>). "Pending
  publishers" can create a project on first publish but do NOT reserve the name
  (<https://docs.pypi.org/trusted-publishers/creating-a-project-through-oidc/>).
- **npm**: per-package 2FA/granular-token publish settings
  (<https://docs.npmjs.com/requiring-2fa-for-package-publishing-and-settings-modification/>);
  post-Shai-Hulud roadmap: classic tokens deprecated, granular publish tokens
  capped at 7-day lifetime, token publishing disallowed by default, publish
  narrowed to local-with-2FA / short-lived tokens / trusted publishing
  (<https://github.blog/security/supply-chain-security/our-plan-for-a-more-secure-npm-supply-chain/>).
  Provenance: `npm publish --provenance`, npm CLI ≥9.5.0, cloud-hosted GitHub/
  GitLab runners, matching public `repository` URL; verify with
  `npm audit signatures` (<https://docs.npmjs.com/generating-provenance-statements/>).
- **crates.io**: GitHub-only login, verified email; names first-come-first-
  served; publish permanent (yank only); license + description required
  (<https://doc.rust-lang.org/cargo/reference/publishing.html>). Token scopes
  `publish-new`/`publish-update`/`yank`/`change-owners` + per-crate scoping
  (<https://rust-lang.github.io/rfcs/2947-crates-io-token-scopes.html>);
  90-day default token expiry
  (<https://blog.rust-lang.org/2025/02/05/crates-io-development-update/>);
  Trusted Publishing GA July 2025, GitHub Actions only, 30-minute tokens via
  `rust-lang/crates-io-auth-action`
  (<https://blog.rust-lang.org/2025/07/11/crates-io-development-update-2025-07/>).
- **RubyGems**: MFA mandatory only above 180M downloads; OTP/WebAuthn CLI flows
  (<https://guides.rubygems.org/using-mfa-in-command-line/>,
  <https://blog.rubygems.org/2022/08/15/requiring-mfa-on-popular-gems.html>).
- **NuGet**: Microsoft/AAD account, 2FA enforced
  (<https://learn.microsoft.com/en-us/nuget/nuget-org/individual-accounts>);
  optional ID-prefix reservation via account@nuget.org
  (<https://learn.microsoft.com/en-us/nuget/nuget-org/id-prefix-reservation>).
- **Wheels**: supported images manylinux2014 (glibc 2.17), manylinux_2_28
  (AlmaLinux 8, GCC 14), manylinux_2_34 + manylinux_2_39 (alpha), musllinux_1_2
  (<https://github.com/pypa/manylinux>); cibuildwheel latest v4.1.0, released
  2026-06-12 (<https://github.com/pypa/cibuildwheel/releases>,
  <https://pypi.org/project/cibuildwheel/>).
- **Maven Central**: detached ASCII-armored PGP signatures from a key on
  keyserver.ubuntu.com / keys.openpgp.org / pgp.mit.edu
  (<https://central.sonatype.org/publish/requirements/gpg/>); namespace
  `io.github.<username>` auto-granted via the Central Portal
  (<https://central.sonatype.org/register/namespace/>).
- **Homebrew**: core needs DFSG-open, stable, notable software — ≥30 forks OR
  ≥30 watchers OR ≥75 stars, **3× higher if self-submitted** (≥90/90/225);
  otherwise a tap (<https://docs.brew.sh/Acceptable-Formulae>); taps are any
  GitHub repo named `homebrew-<name>`, no review
  (<https://docs.brew.sh/Taps>).
- **AUR**: SSH key on the account; push PKGBUILD + regenerated `.SRCINFO` to
  `ssh://aur@aur.archlinux.org`; no mandatory review
  (<https://wiki.archlinux.org/title/AUR_submission_guidelines>).
- **Debian**: WNPP/ITP bug → GPG-signed dput to mentors.debian.net → RFS → DD
  sponsor → ftpmaster NEW review; no fixed SLA
  (<https://mentors.debian.net/intro-maintainers/>).
- **vcpkg**: one port per PR; project maturity ≥6 months; CI on ≥1 official
  triplet; `share/<port>/copyright` via `vcpkg_install_copyright`; versions via
  `vcpkg x-add-version`; PRs stale >60 days may close
  (<https://learn.microsoft.com/en-us/vcpkg/contributing/maintainer-guide>).
- **conan-center**: fork conan-io/conan-center-index + recipe PR; first-time
  CLA; ~30+ CI configurations; license/description must match upstream; latest
  version only
  (<https://github.com/conan-io/conan-center-index/blob/master/docs/adding_packages/README.md>).
- **Go subdirectory modules**: version tags for a module in a repo
  subdirectory must be prefixed with that subdirectory — here
  `bindings/go/v1.1.0` — or the proxy cannot serve the version
  (<https://go.dev/ref/mod>, "Module paths": the subdirectory "also serves as
  a prefix for semantic version tags").

## Out of scope

- **The consumer-facing GitHub Action / GitLab template** (review P2) — landed
  2026-07-07 (`action.yml`, `ci/asmtest.gitlab-ci.yml`); its Marketplace
  listing is a separate maintainer action, not part of this doc.
- **A macOS drtrace payload slot** — the DynamoRIO tier is deliberately
  linux-x86_64-only in the payloads; porting it is
  [macos-dynamorio-port.md](macos-dynamorio-port.md).
- **What replaces `macos-15-intel` after 2027-08** for the darwin-x86_64
  payload leg — [self-hosted-ci-runners.md](self-hosted-ci-runners.md).
- **Publishing tiers with hardware backends more broadly** (Intel PT/IBS/
  CoreSight capture correctness) — owned by their tier docs, e.g.
  [intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md); the
  payloads here only bundle what `make package-libs` already stages.
