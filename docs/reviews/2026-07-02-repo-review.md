# asm-test — repository review (2026-07-02)

**Scope:** follow-up whole-repo review. Tracks the status of the
[2026-07-01 review](2026-07-01-repo-review.md)'s findings against current
`main`, then adds new issues from a fresh **build/CI/packaging** deep-dive and a
**structural/process** pass (release cadence, doc/code ratio, repo hygiene).

**Method:** the prior review's 15 findings were each re-checked against the
source to classify fixed / partial / open. A dedicated build/CI/packaging pass
produced items B1–B7. A structural pass produced S1–S3. Items marked
**[verified]** were confirmed against the code by hand during this review.

**Caveat — incomplete code re-scan:** three planned second-opinion passes (core
runtime, language bindings, trace/emu tiers) were repeatedly aborted by a
sustained upstream API outage and produced no output. For those areas this
review relies on the 2026-07-01 pass — which already investigated and *cleared*
much of that surface (struct-mirror offsets, guard-page math, the fork/pipe
protocol) — with its still-open items re-verified below. A future pass should
still look for *new* code-level bugs there.

Paths are repo-relative; `file:line` points at the exact site.

---

## Overall

The 2026-07-01 review has largely landed: the two High correctness bugs (ptrace
child leak, RNG SIGFPE), the ULP UB, the version/header sync, the
`DRAPP_KEYSTONE` object-identity gap, and the doc-accuracy items are all fixed.
The highest-value *new* observation is not a crash but a process gap: **an
elaborate release-and-packaging pipeline that has never actually run via its
real trigger** — and, relatedly, two "green CI, wrong artifact" traps that a
real release would expose (a sanitizer build that instruments nothing; a
published package set that omits Intel macOS).

### Severity summary (new items)

| # | Finding | Severity | Area |
|---|---------|----------|------|
| S1 | Release pipeline has never fired (0 tags; ~970-line `[Unreleased]`) | High (leverage) | process |
| B1 | `SAN`/`COV`/`ASM_SYNTAX` not in object identity → false-green sanitizer | Medium–High | build |
| B2 | No `darwin-x86_64` payload in published packages | Medium–High | packaging |
| B3 | GPL §3 corresponding-source version can drift from shipped binary | Medium | packaging/legal |
| B6 | Publish tokens injected at job scope, live during build/smoke steps | Medium | CI/security |
| B5 | Fetched/built third-party binaries have no checksum/signature pin | Medium | supply chain |
| B4 | `DR_VERSION` hardcoded in six places | Medium–Low | build |
| S2 | Docs (20.2k lines) now outweigh code (16.3k); 40% is design scratch | Low | docs |
| S3 | Fragmented git identity; no `.mailmap` | Low | hygiene |
| B7 | `make -j *-bindings-test` recursive-make races | Low | build |

---

## Status of the 2026-07-01 findings

| # | Finding | Status |
|---|---------|--------|
| 1 | ptrace child leak on fault path | **Fixed** (`d2c657c`) |
| 2 | RNG division-by-zero on wide ranges | **Fixed** (`f9e0ca4`) |
| 4 | Signed-overflow UB in ULP helpers | **Fixed** (`f9e0ca4`) |
| 6 | Version bump didn't touch the C header | **Fixed** (`a568ece`) |
| 7 | `DRAPP_KEYSTONE` not in object prerequisites | **Fixed** (`5a9c29e`) |
| 12 | README oversells | **Fixed** — `README.md:9` now "implemented and exercised in CI" |
| 13 | AVX-512 doc stale | **Fixed** — `docs/floating-point-simd.md:124` now "wired" |
| 14 | DESIGN.md stale | **Fixed** (`f252f8c`) — non-compiling symbols gone |
| 5 | Native-trace robustness | **Partial** — `src/hwtrace.c:589,750` now set `truncated`; the `begin`-failure paths (`:617-625,713-730`) still worth confirming |
| 15 | Over-documentation | **Partial** — AMD plans consolidated (`b250569`); see S2 |
| 3 | In-process crash containment gaps | **Open [verified]** — no `sigaltstack`/`SA_ONSTACK` in `src/asmtest.c` (grep-empty); SIGALRM handler still async-signal-unsafe |
| 8 | Clean-room shell robustness | **Open [verified]** — `scripts/assert-clean-path.sh` still has no `set -u` and an unguarded `$TMPDIR` (`:70`); `scripts/clean-room-test.sh:204` still `printf "$summary"` |
| 9 | Lua drops integer args | **Open [verified]** — `bindings/lua/asmtest.lua:252,256` still hardcode `nil, 0` |
| 10 | Emulator no state reset between calls | **Open [verified]** — `src/emu.c` zeroes only the output struct; guest GP/vector/RW memory persist across `call()`s on a reused handle |
| 11 | Node 64-bit precision loss | **Open [verified]** — args now use `BigInt` (`bindings/node/asmtest.js:159`), but read-back still funnels through `Number()` (`:214,235,359,368,454`) |

---

## New findings — build / CI / packaging

### B1. `SAN=1` / `COV=1` / `ASM_SYNTAX` are not part of core-object identity — Medium–High **[verified]**
`Makefile:41-46` (flags fold into `CFLAGS`), object rules from `Makefile:202`.

`SAN`/`COV` mutate `CFLAGS`, and `ASM_SYNTAX` selects a different source, but no
core object (`asmtest.o`, `capture.o`, the example/test/`emu`/`ffi` objects)
depends on a flag string — only on its source + header. GNU make compares
prerequisite mtimes, not recipe text, so a build with a different knob silently
reuses stale objects. `make test && make SAN=1 test` **rebuilds nothing** and
re-runs the already-built, non-instrumented binaries — an ASan/UBSan pass that
exercised zero instrumentation. Same trap for `make ASM_SYNTAX=nasm test` after
a GAS build.

The convenience targets dodge it by cleaning first (`make sanitize` /
`coverage`, `Makefile:515,526`), and `mk/docker.mk:64-65` explicitly cleans
between phases "so the GAS/NASM backends and the sanitizer build don't share
stale objects" — but the *documented bare knobs* (`Makefile:38-39`, help text)
have no guard. This is the exact bug class the `.drapp-flags` sentinel fixed for
finding #7, applied to only 2 of ~40 objects.

**Fix:** fold the active knobs into object identity via a `.build-flags`
sentinel prerequisite across all core objects, as done for `drtrace_app.o`.

### B2. No `darwin-x86_64` payload in published packages — Medium–High **[verified]**
`.github/workflows/release.yml:28` (`native` matrix), `:127`, `:217`.

The release matrices are `[ubuntu-latest, ubuntu-24.04-arm, macos-latest]`;
`macos-latest` is arm64. `native-all` (`:51`) merges only the payloads that
matrix builds, and `package-libs-verify` (`mk/bindings.mk:454-495`) only checks
platforms *present*. So every published dlopen package (ruby/node/lua/java/
dotnet) and the wheel set omits `darwin-x86_64` — while `README.md:65`
advertises "x86-64 and AArch64, Linux and macOS" and `ci.yml:63` *tests*
`darwin-x86_64` nightly. `docs/packaging.md:121` says the Intel payload "builds
nightly + on dispatch," but that job lives in `ci.yml` and is never wired into
`release.yml`. An Intel-Mac user's install resolves no native lib while every
release job stays green.

**Fix:** either add a `macos-13` leg to `release.yml` (and have
`package-libs-verify` assert a *complete* platform set), or explicitly document
macOS packages as arm64-only.

### B3. GPL §3 corresponding-source can drift from the shipped binary — Medium
`scripts/build-keystone.sh:12` / `build-capstone.sh:14` define the versions
compiled and vendored; `scripts/fetch-corresponding-source.sh:18-19`
independently defaults to the same numbers; `release.yml:95` invokes it passing
only `UNICORN_VERSIONS`. The upstream source archive attached to a release for
the GPL-2.0 Keystone is thus pinned by a *different* constant than the binary
actually built. Bump `build-keystone.sh` without editing the fetch script and
the release attaches the wrong source for a GPL binary, with no gate to catch
it. The Unicorn version is separately scraped from NOTICE text with a hardcoded
`2.1.4` fallback (`release.yml:90-93`) — a detection miss silently ships the
wrong source too.

**Fix:** one source of truth per engine version, asserted in CI.

### B4. `DR_VERSION` hardcoded in six places — Medium–Low
`mk/bindings.mk:227` (comment: "Kept in one place"), `mk/docker.mk:174`,
`ci.yml:209`, `scripts/fetch-dynamorio.sh:15`, `Dockerfile.drtrace:29`,
`Dockerfile.drtrace-lang:26` — all `11.91.20630`. Bump the Make layer but miss
`ci.yml`/the Dockerfiles and CI tests one DynamoRIO while packaging bundles
another; `THIRD-PARTY-LICENSES` then records a version that doesn't match the
shipped `libdynamorio.so`.

### B5. Fetched/built third-party binaries have no integrity pin — Medium
`scripts/fetch-dynamorio.sh:28-33` (curl → tar; result bundled into wheels),
`build-keystone.sh:30` / `build-capstone.sh:32` (`git clone --branch <tag>` — a
mutable ref), `mk/docker.mk:118-121` (Zig tarball). Integrity rests solely on
the version in the URL/tag; no SHA-256/signature check before the artifact is
compiled in or vendored into user-facing packages. A moved tag or compromised
release asset yields an altered engine published under the project's name.

### B6. Publish tokens injected at job scope — Medium
`release.yml:128-129` (`PYPI_TOKEN` as job-level `env`), `:246-249`
(NPM/RubyGems/NuGet), `:301-302` (`CARGO_REGISTRY_TOKEN`). Tokens are in the
environment of *every* step, including `pip install build twine` and the smoke
steps that run the just-built package (`dotnet run`, `node -e`, …). A compromised
build/tool dependency (or the packaged code itself) on a tag build could read
and exfiltrate them. **Fix:** scope each token to its single publish step.

### B7. `make -j *-bindings-test` recursive-make races — Low
`mk/native-trace.mk:403` (`drtrace-bindings-test`) and `:517`
(`hwtrace-bindings-test`) fan out to per-language sub-makes that each build the
same `build/pic/*.o`, `libasmtest_drapp.*`, and `build/drclient/` into one
shared tree. `make -j` locally lets two lanes write the same objects/libs at
once → torn writes or `cmake --build` collisions. CI escapes it only because
each language runs in its own container.

---

## New findings — structural / process

### S1. The release pipeline has never actually fired — High (leverage) **[verified]**
`git tag` returns **zero tags**, yet `CHANGELOG.md` documents `[1.0.0] —
2026-06-24` and ~970 lines have since accumulated under `[Unreleased]`.
`release.yml` triggers on `push: tags: ["v*"]`, so its real path has only ever
run as manual `workflow_dispatch` dry-runs. The 10-language packaging,
`sync-version`, `check-version`, and clean-room install proofs have never run
end-to-end via their trigger, and consumers have no version to pin.

**Recommend:** cut and tag a real release. It flushes the changelog, gives a
pinnable artifact, and — most usefully — is the fastest way to surface B2–B6 in
practice rather than in review.

### S2. Documentation now outweighs code — Low **[verified]**
20,199 lines of Markdown vs 16,258 lines of C+asm+headers. **8,161 of the doc
lines (40%) are 15 plan + 4 analysis files** — unpublished design scratch
(`docs/conf.py:36` excludes `plans/*`/`analysis/*`/`reviews/*` from the built
site). Git history preserves landed plans; keeping design-era scratch in the
working tree inflates the review surface. Reinforces finding #15.

**Recommend:** archive plans once their work lands.

### S3. Fragmented git identity — Low **[verified]**
One author appears as three identities across 245 commits, one being
`Willem van Ketwich <willemvanketwich@Willems-MacBook-Pro.local>` (176 commits —
a machine hostname that won't attribute on GitHub). No `.mailmap`.

**Fix:** add a `.mailmap` and correct the local `git config user.email`.

---

## Suggested fix order

1. **S1** — tag a real release (highest leverage; stress-tests the packaging
   machinery and surfaces B2–B6 concretely).
2. **B1, B2** — false-green sanitizer + missing Intel-mac payload (both
   "green CI, wrong artifact").
3. **B3, B6, B5** — GPL source drift, token scope, checksum pinning
   (compliance + supply chain).
4. Remaining open 2026-07-01 items: **#9** (Lua args), **#3** (in-process
   containment), **#10 / #11** (emu reset / Node precision), **#8** (shell
   robustness).
5. **B4, B7, S2, S3** — drift-proofing, `-j` safety, curation, hygiene.
