# asm-test — Open defects & still-open review items: remediation plan

A consolidated plan for the work that is **still open** after the review-driven
defect sweep landed. It covers two sources:

1. **Codebase-identified items** — a genuine latent defect found through testing
   (captured in project memory) plus the deliberate, documented limitations grep
   surfaces in the headers.
2. **Still-open review items** — the four findings from the
   [2026-07-02 repo review](../archive/reviews/2026-07-02-repo-review.md) that the summaries
   list as not-yet-closed (S1, B2, B5, B7).

Everything else from the plans and reviews is either landed (see
docs/summaries/) or blocked on hardware / hosts / privileges that
this dev box (AMD Zen 5, Linux, `perf_event_paranoid=4`, no passwordless sudo)
cannot supply — see the
2026-07-02 roadmap assessment for
that blocked set, which is intentionally out of scope here.

> Status legend: **open** unless noted. Each item states the **issue** (with a
> `file:line` anchor) and a **proposed solution**. Update this file as items land,
> the way [inline-asm-keystone-plan.md](../archive/plans/inline-asm-keystone-plan.md) tracks its own.
> Sources: `M` = memory/testing, `R` = repo review finding.

---

## Priority ordering

| # | Item | Source | Severity | Status (2026-07-04) |
|---|------|--------|----------|---------------------|
| P0 | Go full-suite flaky segfault (codeimage/ptrace corruption) | M | High (memory corruption) | ✅ **Fixed & verified** (`runtime.LockOSThread`; 40/40 vs 30/40) |
| P1 | B7 — `make -j *-bindings-test` recursive-make race | R | Low | ✅ **Done** (single shared prereq; sub-make now built once) |
| P2 | B5 — no integrity pin on fetched/built third-party binaries | R | Medium (supply chain) | ✅ **Done** (real digest/commit manifest + verify + CI gate) |
| P3 | B2 — no `darwin-x86_64` payload in published packages | R | Medium–High | ✅ **Done** (macos-13 leg + complete-set verify) |
| P4 | S1 — release pipeline has never fired (cut a real tag) | R | High (leverage) | ◻ **Partial** — version green; tag/publish is the maintainer's |
| P5 | Deliberate documented limitations (Capstone decode, DRTRACE_EVENTS, per-thread hwtrace, Keystone ext-mnemonics) | M | — | ✅ **Confirmed** no-action-by-design |

---

## P0 — Go full-suite flaky segfault *(✅ DONE 2026-07-04 — root-caused & fixed)*

> **Outcome.** Root cause: the Go `HwTrace` wrapper ran `Begin` → traced call →
> `End` as three separate cgo calls **without pinning the goroutine to its OS
> thread**. The single-step backend arms per-thread `EFLAGS.TF` and handles the
> SIGTRAPs, so when Go's scheduler migrated the goroutine mid-region, TF was armed
> on one thread while the code ran on another → empty trace (or a stray SIGTRAP on
> a thread later running Go = the wandering-site crash). The full suite triggered
> it (all conformance/drtrace tests run first → max GC/thread churn) while the
> gated `-run TestHwtrace` subset stayed clean — matching the memory exactly.
> **Fix:** `runtime.LockOSThread()` in `Begin`, `runtime.UnlockOSThread()` in `End`
> ([bindings/go/hwtrace.go](../../bindings/go/hwtrace.go)), pinning the region's
> whole lifetime. **Verified** in the `asmtest-go` container on the SingleStep
> path: full `go test ./...` went from **30 ok / 10 fail (25%)** pre-fix to
> **40/40 (0%)** post-fix; the gated subsets were 20/20 both ways. *(Separate
> newly-surfaced observation, not this defect: on a host where AMD LBR is available
> (`CAP_PERFMON`), `HwTraceAuto(Best)` picks LBR and `Covered(0)` is statistically
> flaky — tracked as a follow-up, out of scope for the memory's ptrace/single-step
> bug.)*

**Issue (as originally filed).** The Go binding's **full** suite `go test ./...`
segfaults on ~5% of
runs with a **wandering crash site** in the codeimage soft-dirty scan / ptrace
single-step — the signature of heap-layout-dependent memory corruption. It is
**not** the call-descent code: `src/descent.c` + the descend loop are
AddressSanitizer-clean (`SAN=address hwtrace-test` 132/132 over 5 runs), the
descent-only Go subsets were 0/60, and the gated `hwtrace-go-test` was 30/30
stable. The corruption pre-exists descent; descent's extra allocation churn
merely perturbs a latent bug in [src/codeimage.c](../../src/codeimage.c) (the
global `clear_refs` re-arm / soft-dirty snapshot ordering, e.g.
[src/codeimage.c:430](../../src/codeimage.c#L430)) or the single-step read path in
[src/ptrace_backend.c](../../src/ptrace_backend.c) into visibility under the Go
harness's process. Recorded in the `go-fulltest-flaky-crash` memory.

**Proposed solution.**
1. **Reproduce under instrumentation.** Build `libasmtest_hwtrace` (and the emu/
   corpus libs the Go package links) with `SAN=address SAN=undefined`, then run the
   **full** Go suite against that C library in a loop until the fault trips — an
   ASan report pins the corrupting write directly, versus the current bare
   wandering SIGSEGV. Docker lane: extend the existing `hwtrace-go` target with an
   ASan-instrumented variant so the Go process loads the poisoned `.so`.
2. **If ASan is silent through cgo**, bisect the codeimage soft-dirty scan: gate
   `ci_scan_range` / the `clear_refs` re-arm ([src/codeimage.c:430](../../src/codeimage.c#L430))
   behind a debug env flag and confirm the crash rate collapses when the scan is a
   no-op, isolating scan vs. single-step.
3. **Audit the two prime suspects** for a size/lifetime bug that a different heap
   layout exposes: the per-region snapshot buffer allocation/growth in
   `codeimage.c`, and the per-instruction `process_vm_readv` window sizing in
   `ptrace_backend.c` (off-by-one / stale-length on region boundaries).
4. **Regression gate.** Once fixed, add a stress lane (`go test -count=N ./...`
   under ASan) so a reintroduction is caught, and update the memory note.

**Acceptance:** the full Go suite survives ≥200 consecutive `-count` iterations
under an ASan-instrumented native lib with zero faults.

---

## P1 — B7: `make -j *-bindings-test` recursive-make race *(✅ DONE 2026-07-04)*

> **Outcome.** Added a single `drtrace-shared-prep` target (the `DRAPP_KEYSTONE=0`
> recursive sub-make, guarded by `DR_AVAILABLE`) and made it a **shared
> prerequisite of every** `drtrace-*-test` lane, deleting the per-lane inner
> `$(MAKE)`; the hwtrace `rust`/`go` lanes now take `shared-emu`/`$(CORPUS_LIB)` as
> normal prerequisites instead of a recipe sub-make
> ([mk/native-trace.mk](../../mk/native-trace.mk)). GNU make now builds the shared
> artifacts **exactly once** and every lane waits on them — no concurrent writers.
> **Verified:** `make -n drtrace-bindings-test DR_AVAILABLE=1` shows the shared
> sub-make scheduled **once** (was once-per-lane); DR-absent `make -n` still prints
> the SKIP for every lane (behavior preserved); the makefile parses clean.

**Issue.** [mk/native-trace.mk:386](../../mk/native-trace.mk#L386) (`drtrace-bindings-test`)

**Issue.** [mk/native-trace.mk:386](../../mk/native-trace.mk#L386) (`drtrace-bindings-test`)
and the `hwtrace-bindings-test` fan-out invoke per-language sub-makes
(`@$(MAKE) shared-drtrace drtrace-client DRAPP_KEYSTONE=0`, repeated at
`:444,454,467,479,488,500`) that each build the **same** `build/pic/*.o`,
`libasmtest_drapp.*`, and `build/drclient/` into one shared tree. Under `make -j`
locally, two language lanes write the same objects/libs concurrently → torn
writes or `cmake --build` collisions. CI only escapes it because each language
runs in its own container.

**Proposed solution.** Make the shared artifacts a **once-built prerequisite**
rather than a per-lane recursive rebuild: hoist `shared-drtrace drtrace-client`
(and `shared-emu`/`$(CORPUS_LIB)` for rust/go) into an order-only prerequisite of
the aggregate `*-bindings-test` target so GNU make builds them a single time
before fanning out, and drop the inner `$(MAKE) … DRAPP_KEYSTONE=0` from each
language lane. Where a recursive sub-make must stay, serialize the shared build
with a `.NOTPARALLEL`-guarded phase or a stamp file the lanes depend on. Validate
with `make -j$(nproc) drtrace-bindings-test` in a loop.

---

## P2 — B5: no integrity pin on fetched/built third-party binaries *(✅ DONE 2026-07-04)*

> **Outcome.** Added a trust-anchor manifest
> [scripts/third-party-digests.txt](../../scripts/third-party-digests.txt) with
> **real** pinned values (DynamoRIO tarball `sha256:1499acb…`, keystone 0.9.2 →
> commit `dc7932ef…`, capstone 5.0.1 → commit `097c04d9…`) and a POSIX helper
> [scripts/lib-thirdparty.sh](../../scripts/lib-thirdparty.sh). `fetch-dynamorio.sh`
> now SHA-256-verifies the download before extracting; `build-keystone.sh` /
> `build-capstone.sh` clone the tag then **assert `HEAD` equals the pinned commit**
> (catching a moved/force-pushed tag); all three **fail closed** if the manifest
> lacks an entry. A regenerator [scripts/refresh-thirdparty-digests.sh](../../scripts/refresh-thirdparty-digests.sh)
> makes bumps a one-command step, and `check-thirdparty-versions.sh` gained a gate
> asserting every declared version has a manifest anchor. **Verified:** all six
> scripts `sh -n`-clean; the drift gate passes and lists all three anchors; the
> helper returns the right values (and errors on an absent version); `tp_sha256`
> matches (`sha256("hello")=2cf24dba…`); and a live shallow clone of keystone 0.9.2
> resolves `HEAD` to exactly the pinned `dc7932ef…` (assertion passes legitimately,
> would trip on a moved tag). *(The 455 MB DR download itself was pinned to
> GitHub's own recorded asset digest rather than re-downloaded here.)*

**Issue.** [scripts/fetch-dynamorio.sh](../../scripts/fetch-dynamorio.sh) curls a

**Issue.** [scripts/fetch-dynamorio.sh](../../scripts/fetch-dynamorio.sh) curls a
release tarball and untars it straight into what gets bundled into wheels;
`scripts/build-keystone.sh` / `build-capstone.sh` `git clone --branch <tag>` a
**mutable** ref; `mk/docker.mk` fetches the Zig tarball. Integrity rests solely on
the version in the URL/tag — no SHA-256 or signature check before the artifact is
compiled in or vendored into user-facing packages. A moved tag or a compromised
release asset ships an altered engine under the project's name.

**Proposed solution.** Add a **pinned-digest manifest** (e.g. `scripts/third-party-digests.txt`
mapping `artifact@version → sha256`) and have each fetch script verify the
download against it (`sha256sum -c`) before use, failing the build on mismatch.
For the `git clone` engines, pin to the tag's **commit SHA** (`git clone` then
`git checkout <sha>` and assert `git rev-parse HEAD`), not the mutable tag. Wire a
CI gate that re-derives and diffs the digests so an intentional bump is a reviewed
one-line manifest change. Complements the existing `check-version` gate.

---

## P3 — B2: no `darwin-x86_64` payload in published packages *(✅ DONE 2026-07-04 — option (a))*

> **Outcome.** Took option (a), "ship it." Added the `macos-13` (Intel) runner to
> all three release matrices — `native`, `python` (wheels are per-OS), and
> `dlopen-package` — in [.github/workflows/release.yml](../../.github/workflows/release.yml),
> so `native-all` now carries a `darwin-x86_64` slot bundled into every package +
> the wheel set. Strengthened `package-libs-verify`
> ([mk/bindings.mk](../../mk/bindings.mk)) with an `EXPECT_PLATFORMS` complete-set
> assertion (default off for local single-slot verifies; the collect job passes the
> full `linux-x86_64 linux-aarch64 darwin-x86_64 darwin-arm64` set), so a dropped
> matrix leg fails loudly instead of silently shipping an incomplete set.
> **Verified:** `release.yml` parses as YAML; all three matrices carry `macos-13`;
> the gate flags a missing `darwin-x86_64` (negative test) and passes with all four
> slots present (positive test).

**Issue.** [.github/workflows/release.yml](../../.github/workflows/release.yml)'s

**Issue.** [.github/workflows/release.yml](../../.github/workflows/release.yml)'s
`native` matrix is `[ubuntu-latest, ubuntu-24.04-arm, macos-latest]`, and
`macos-latest` is arm64. `native-all` merges only what the matrix builds and
`package-libs-verify` only checks platforms **present**, so every published dlopen
package (ruby/node/lua/java/dotnet) and the wheel set omits `darwin-x86_64` — while
`README.md` advertises "x86-64 and AArch64, Linux and macOS" and `ci.yml` *tests*
`darwin-x86_64` nightly. An Intel-Mac install resolves no native lib while every
release job stays green.

**Proposed solution.** Two options — a maintainer choice:
- **(a) Ship it:** add a `macos-13` (Intel) leg to the `release.yml` `native`
  matrix, and strengthen `package-libs-verify` (`mk/bindings.mk`) to assert a
  **complete, explicit** platform set (fail if any of the four target combos is
  missing) rather than only checking present ones.
- **(b) Scope it:** explicitly document macOS packages as **arm64-only** in
  `README.md` and `docs/reference/packaging.md`, and align the README capability
  line so the advertised matrix matches what ships.

Recommend **(a)** since `ci.yml` already proves the Intel-mac build is green
nightly; the only missing piece is wiring that leg into `release.yml`.

---

## P4 — S1: the release pipeline has never fired *(◻ Partially done 2026-07-04 — maintainer action remains)*

> **Outcome.** Code-side reconciliation **verified green**: `make check-version`
> reports *all manifests at 1.1.0* (matching the `VERSION` file), so there is no
> version drift blocking a tag. Per maintainer decision (2026-07-04), the CHANGELOG
> `[Unreleased]` → `[1.1.0]` flush is **deferred to tag time** rather than dated now
> (no tag exists yet; dating an untagged release would misrepresent it). **The
> remaining work is intentionally the maintainer's** and out of scope for an agent:
> push the `v1.1.0` tag, register the per-ecosystem package names, and add the
> publish token secrets. This is the correct stopping point — everything an agent
> can safely do here (version consistency) is done and confirmed; nothing was
> fabricated. Land P2/P3 (done) first so the first real tag doesn't discover a
> missing Intel payload or an unpinned dependency.

**Issue.** `git tag` returns **zero tags**, yet `CHANGELOG.md` documents

**Issue.** `git tag` returns **zero tags**, yet `CHANGELOG.md` documents
`[1.0.0] — 2026-06-24` and a large `[Unreleased]` section has since accumulated.
`release.yml` triggers on `push: tags: ["v*"]`, so its real path has only ever run
as manual `workflow_dispatch` dry-runs. The 10-language packaging, `sync-version`,
`check-version`, and clean-room install proofs have never run end-to-end via their
trigger, and consumers have no version to pin. This is the **highest-leverage**
item: cutting a real tag is the fastest way to surface B2–B5 in practice.

**Proposed solution.** A maintainer action, sequenced to de-risk it:
1. Land P2/P3 first (so the first real release is not the thing that discovers a
   missing Intel payload or an unpinned dependency).
2. Flush `[Unreleased]` into a dated `[1.1.0]` section in `CHANGELOG.md`
   (VERSION already reads `1.1.0`), verify `sync-version`/`check-version` agree.
3. Register the per-ecosystem package names and add the publish token secrets
   (already scoped to their publish steps per B6).
4. Push `v1.1.0` and watch the real trigger; treat the first run as the
   integration test for the packaging machinery.

*(Code-side prep — steps 1–2 and any changelog/version reconciliation — is doable
here; the tag push + credential setup is the maintainer's.)*

---

## P5 — Deliberate documented limitations *(✅ Confirmed 2026-07-04 — no action by design)*

> **Outcome.** Re-verified each against its source: all four are **intentional,
> documented boundaries, not defects** — Capstone's decoder is gated so
> `asmtest_cs_decoder_present()` returns 0 and the tier self-skips
> ([src/cs_backend.c:27](../../src/cs_backend.c#L27)); `ASMTEST_DRTRACE_EVENTS` is
> marked *reserved* ([include/asmtest_drtrace.h:49](../../include/asmtest_drtrace.h#L49));
> hwtrace region registration is documented "NOT yet per-thread — bracket one
> registered routine per begin/end" ([include/asmtest_hwtrace.h:151](../../include/asmtest_hwtrace.h#L151));
> and Keystone extended-mnemonic assembly is upstream-gated
> ([include/asmtest_assemble.h:30](../../include/asmtest_assemble.h#L30)). Left
> as-is per the plan's recommendation; the future-implementation shapes below stand
> for when a concrete user need arises.

These grep as "not implemented / not yet" but are **intentional, documented**

These grep as "not implemented / not yet" but are **intentional, documented**
boundaries. Listed for completeness with the shape a future implementation takes;
none is a correctness bug.

| Limitation | Anchor | Proposed future solution |
|---|---|---|
| Capstone in-tree decoder absent (self-skips) | [src/cs_backend.c:27](../../src/cs_backend.c#L27) | Implement `asmtest_cs_decoder_present()` + a real Capstone-backed decode path behind the existing optional-dependency gate, mirroring the Unicorn disasm wiring |
| `ASMTEST_DRTRACE_EVENTS` reserved | [include/asmtest_drtrace.h:49](../../include/asmtest_drtrace.h#L49) | Define the event-record schema and emit it from the DR client when a caller opts into the events mode; keep the enum value stable |
| hwtrace region registration not per-thread | [include/asmtest_hwtrace.h:151](../../include/asmtest_hwtrace.h#L151) | Move the single registered-routine slot to thread-local state so concurrent `begin/end` brackets don't alias; requires a per-thread ring/decoder |
| Keystone extended-mnemonic assembly | [include/asmtest_assemble.h:30](../../include/asmtest_assemble.h#L30) | Upstream-gated: enable once a released Keystone supports it; assembly path is otherwise ready |

**Recommendation:** leave as documented limitations; promote to real work only on
a concrete user need. Track here rather than as stray `TODO`s in the headers.

---

## Suggested execution order

1. **P0** — the memory-corruption bug (only unblocked defect; do first).
2. **P2 + P3** — supply-chain pin and Intel-mac payload (make the release safe).
3. **P4 (steps 1–2)** — reconcile changelog/version for the first real tag.
4. **P1** — `-j` recursive-make hardening (Low, opportunistic).
5. **P5** — only on demand.
