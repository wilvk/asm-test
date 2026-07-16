# asm-test — macOS Clean-Room & Portability Test Plan

Makes the macOS dlopen-binding install-tests **clean-room honest** (the bundled
native lib is the *only* thing that can satisfy the load — never a leaked dev
`build/`, a Homebrew dylib, or a `DYLD_*` override) and **portable across both
Darwin arches** (arm64 + x86-64), without depending on the scarce/slow hosted
Intel runner for the day-to-day signal.

> Status legend: **done** / **written per spec, UNVALIDATED** (code exists, lane has
> never run — first run is a shakedown) / **planned**. Update as tracks land.

> **Status (2026-07-16): 3 of 5 tracks done — A, B, E.** The hosted-CI story is
> complete and gating: Track A (scrubbed-env smoke + resolved-path assert) over all
> six dlopen bindings, Track B (static Mach-O checks on the Linux collector), Track E
> (CI wiring + docs). The two remaining tracks are *additive clean-room backstops*,
> both written-but-never-run, and they are blocked on **different** things — the
> distinction that matters most when picking one up:
>
> - **Track D (x86 Docker-OSX) needs no Apple hardware** and `/dev/kvm` is present on
>   the current dev host — its only real gate is fetching a prebuilt `darwin-x86_64`
>   payload. **Try this one first.**
> - **Track C (arm64 tart) is genuinely Apple-Silicon-gated** and cannot run here.

---

## Context: what the repo already does (and where it leaks)

macOS coverage today is already non-trivial — this plan **supplements** it, it does
not rebuild it:

- [`ci.yml`](../../../.github/workflows/ci.yml) `test` runs **arm64 macOS** natively on
  `macos-latest` every push; `test-macos-x86-rosetta` cross-builds `-arch x86_64`
  and runs it under **Rosetta 2** every push; `test-macos-x86` runs the **native
  Intel `macos-13`** corner nightly (it's flagged scarce/slow).
- [`release.yml`](../../../.github/workflows/release.yml) packages each binding from the
  cross-platform native payload, installs it **fresh with `ASMTEST_LIB` unset**, and
  smoke-loads it (a `cpu_has_avx2` probe — exercises the load path alone) on
  `macos-latest` (arm64).

So the **architecture matrix is already covered**. What is *not* guaranteed is that
those install-tests are genuinely **clean-room** — that the bundled lib is the only
thing that could have satisfied the load. Two concrete leaks in the current code:

1. **Loader fall-through to the dev tree.** The Python loader's candidate chain in
   [`_native.py`](../../../bindings/python/asmtest/_native.py#L30-L48) is: `ASMTEST_LIB`
   → bundled `_libs/` → **`_REPO_ROOT/build/`** → **bare system search**
   (`CDLL("libasmtest_emu.dylib")`). With `ASMTEST_LIB` unset, a smoke run launched
   from inside the checkout can resolve the developer's `build/` lib, or a
   system/Homebrew copy, and still print "ok". The release smoke only dodges this by
   `cd /tmp` before importing — an implicit, easily-broken guard, and the other
   bindings (Go/Rust/Ruby/Node/…) each re-implement their own search with the same
   exposure.
2. **Hosted runners are fresh-per-job but not pristine.** `macos-latest` ships Xcode +
   Command Line Tools + Homebrew. A binding that accidentally links
   `/opt/homebrew/lib/...` (or finds a brew `libunicorn`) **passes on the runner and
   fails on a bare user Mac**. The delocate-vendored Python wheel is the only lane
   that defends against this today; the multi-platform-payload bindings do not.

**Design decisions (validated against the code):**

- The smoke call is a feature probe with **no corpus fixture**, so a clean-room run
  needs only the package + a temp dir — no checkout, no `make` artifacts. The whole
  clean-room story is therefore *environmental*, not about new test cases.
- Most portability failures are **statically detectable** from the Mach-O itself
  (`lipo`/`otool`) and need no booted macOS — those checks belong in the existing
  Linux collector job, for free.
- The arch the *host* can't run natively is the expensive part. An Apple-Silicon
  host gives clean arm64 cheaply (tart); a Linux box gives clean x86 via Docker-OSX.
  Neither emulates the other arch — that stays the hosted-runner / real-hardware job.

### Where each lane fits

| Lane | Arch | Clean-room fidelity | Infra cost | Catches |
|---|---|---|---|---|
| **A** scrubbed-env smoke (this plan) | both | high (asserts the resolved path) | none — hosted | leaked `build/`, brew dylib, `DYLD_*` overrides |
| **B** static Mach-O assertions | both | n/a (build-time) | none — Linux collector | missing arch slice, absolute/`@rpath` install-name bugs |
| **C** tart vanilla VM | arm64 | highest (no Xcode/brew) | low — Apple Silicon host | deps that only exist because the runner is "dirty" |
| **D** Docker-OSX | x86-64 | high (vanilla guest) | high — self-hosted Linux + KVM | x86 clean-room, on-demand (no scarce nightly wait) |

---

## Track A — Scrubbed-env clean-room smoke — **done (harness + all dlopen bindings + Docker + CI + release.yml smokes)**

> **Status: implemented, 2026-07-01.** `make clean-room-test` (any host;
> `make macos-clean-test` is the darwin alias),
> [scripts/clean-room-test.sh](../../../scripts/clean-room-test.sh), packages each binding,
> installs it fresh into a throwaway prefix, and — under
> [`scripts/clean-env.sh`](../../../scripts/clean-env.sh) (env scrubbed, cwd outside the
> checkout) — asserts via
> [`scripts/assert-clean-path.sh`](../../../scripts/assert-clean-path.sh) that the resolved
> native lib lives under the fresh install, never a leaked `build/` tree, a Homebrew
> dylib, or `/usr/local`. A binding whose toolchain is absent self-skips.
>
> **All six dlopen bindings are covered:** Python (`-m asmtest --where`), Ruby/Lua
> (`library_path`), Node/Java (`libraryPath()`), and **.NET** (`Emu.LibraryPath`, read
> from `Process.Modules` so it reports the real loaded path however P/Invoke resolved the
> bundled name). The **link** bindings (C++/Rust/Go/Zig) ship source and link
> `libasmtest` themselves — no bundled payload to leak-check — so they are intentionally
> out of scope.
>
> **Verified:** ruby live on an **Intel macOS** host (loads from the installed gem under
> the throwaway prefix; the negative guard correctly rejects the dev `build/` path);
> **Ruby/Node/Java/.NET/Lua in Docker** via `make docker-clean-<lang>` /
> `make docker-clean-room`, each run with `CLEANROOM_ONLY=<lang>` so a self-skip **fails**
> the lane rather than passing vacuously (a missing toolchain can't be mistaken for a
> pass). A **`clean-room` CI job** over those five now gates every push, alongside the
> conformance `bindings` job; python's clean-room stays in the release.yml python job.
>
> **Done (Track E, 2026-07-07):** the release.yml `cd /tmp && <smoke>` /
> `env -u …` blocks (the 4th bullet below) now `source scripts/clean-env.sh` —
> interpreters resolved to absolute paths before the scrub, installs/builds with
> the full env, only the load under the scrub. The dlopen smokes keep their
> availability assertions; the per-binding resolved-**path** assertion continues
> to live in the ci.yml `clean-room` job (all five docker lanes) and the
> release.yml python job (asserts on the repaired wheel).

Make "install fresh, no `ASMTEST_LIB`" mean what it says, on **every** binding and
**both** hosted arches, at zero extra infra.

- **`scripts/clean-env.sh`** (new) — a sourceable shim that hardens the current
  process before the smoke import:
  - `unset ASMTEST_LIB ASMTEST_MANIFEST` (plus the tier + corpus overrides
    `ASMTEST_CORPUS_LIB`/`ASMTEST_HWTRACE_LIB`/`ASMTEST_DRAPP_LIB`/… — no override may
    pre-satisfy the load);
  - `unset DYLD_LIBRARY_PATH DYLD_INSERT_LIBRARIES` and **pin**
    `DYLD_FALLBACK_LIBRARY_PATH=/usr/lib` — *not* unset it: an unset
    `DYLD_FALLBACK_LIBRARY_PATH` reverts to dyld's built-in default
    (`$HOME/lib:/usr/local/lib:/lib:/usr/lib`), which *includes* `/usr/local/lib`, so a
    Homebrew copy there could still satisfy a bare-leaf-name load. Pinning to the system
    dir is the correct scrub (implemented this way in `clean-env.sh`);
  - strip `/opt/homebrew/bin` and `/usr/local/bin` from `PATH`;
  - `cd "$(mktemp -d)"` so the working dir is **outside any checkout** (kills the
    `_REPO_ROOT/build/` fall-through).
- **Loader self-report (small, high-value FFI/CLI add).** Give each binding a way to
  print the **absolute path it actually dlopen'd** so the test can *assert* it, not
  just trust the exit code. Python already has it (`_native.find_library()` returns
  `(CDLL, path)`); expose it as `python -m asmtest --where` and add the equivalent
  one-liner accessor to the Go/Rust/Ruby/Node/Java loaders. The clean-room assertion
  becomes: resolved path **is under** the installed package dir, and **is not under**
  the repo `build/`, `/opt/homebrew`, or `/usr/local`.
- **`make macos-clean-test`** (new host target, `darwin`-guarded) — packages the
  bindings, installs each into a throwaway prefix, then runs the smoke under
  `clean-env.sh` and the path assertion. Mirrors the matrix of the release smokes but
  fails loudly on a leaked resolution instead of silently passing.
- **Wire into CI** — replace the bare `cd /tmp && <smoke>` blocks in
  [`release.yml`](../../../.github/workflows/release.yml) with `source scripts/clean-env.sh
  && <smoke> && <assert-path>`, so the published-artifact lane proves clean resolution.

## Track B — Static universal-binary / Mach-O assertions — **done**

Catch the most common portability regression (wrong/missing arch slice, absolute
install-name) at **build time**, on the existing Linux collector — no macOS needed.

> **Status: implemented.** `make package-libs-verify-macho`
> ([scripts/verify-macho.sh](../../../scripts/verify-macho.sh)) runs over every
> `build/dist/native/darwin-*/` slot with `llvm-otool` / `llvm-lipo`, and is folded into
> [`package-libs-verify`](../../../mk/bindings.mk) (which the `package-libs-collect` CI job
> runs after installing `llvm`). It asserts each `.dylib` carries the slot's arch, uses
> `@rpath`/`@loader_path` install-names with no leaked `/Users`, `/opt/homebrew`, or
> `/usr/local` path, and has a min-OS load command (and `<= MACOS_MIN_FLOOR` when that var
> is set). It self-skips cleanly where the llvm tools are absent (a dev host), so
> `package-libs-verify` stays green everywhere. Proven against real Linux-built universal
> Mach-O fixtures (`clang --target=…-apple-macos -fuse-ld=lld` + `llvm-lipo`): it passes a
> correct universal/thin payload and fails a wrong-arch slice, an absolute install-name, and
> an over-floor min-OS.

- **`make package-libs-verify-macho`** (new; folded into or called beside
  [`package-libs-verify`](../../../Makefile)) — for every `build/dist/native/<darwin-*>`
  payload, assert:
  - `lipo -archs` (or `llvm-lipo`) reports the **expected arch** for the slot
    (`x86_64` for `darwin-x86_64`, `arm64` for `darwin-arm64`) — no thin-wrong-arch or
    silently-missing slice;
  - `otool -D` / `otool -L` (or `llvm-otool`) install name + deps use
    `@rpath` / `@loader_path`, with **no absolute `/Users/...`, `/opt/homebrew`, or
    `/usr/local`** baked in;
  - the `LC_BUILD_VERSION` / min-OS load command is present and ≤ the support floor.
- **Run on the collector.** The
  [`package-libs-collect`](../../../.github/workflows/ci.yml) job already merges every
  per-OS payload on `ubuntu-latest`; it carries `llvm` tools, so these assertions run
  there for free and gate the merged tree before it's published.

## Track C — tart ephemeral arm64 clean-room — **written per spec (2026-07-07), UNVALIDATED — needs Apple Silicon**

> **Status: written, never executed. Blocked on Apple hardware** (the only track
> that genuinely is — contrast Track D, which is runnable on a Linux box today).
> tart is built on Apple's `Virtualization.framework` and is **Apple-Silicon-only**;
> there is no substitute host, so this lane cannot run here at all.
>
> [`scripts/osx-vm.sh`](../../../scripts/osx-vm.sh) + `make osx-vm-test` implement
> the spec below (clone → headless boot → copy the tree with host-staged packages
> in → run the Track-A test over SSH with `ASMTEST_CLEANROOM_PREBUILT=1` — the
> vanilla guest is toolchain-free, so packaging stays on the host — → delete).
> **No Apple-Silicon host with tart was available where this was authored, so the
> lane has never run**; treat the first run as a shakedown and flip this status to
> *done* when it goes green.

The highest-fidelity arm64 clean room: a **vanilla macOS image with no Xcode/Homebrew**,
reverted between runs — something a hosted runner structurally cannot be. Local on any
Apple-Silicon box; optionally a self-hosted CI lane.

- **`scripts/osx-vm.sh`** (new) — thin wrapper over
  [tart](https://github.com/cirruslabs/tart) (Apple's `Virtualization.framework`,
  native arm64, near-bare-metal): `tart clone <vanilla-base> asmtest-cleanroom`, boot
  headless, `scp` the built packages in, run the Track-A scrubbed-env install-test over
  SSH, then `tart delete`. Snapshot/clone gives a byte-identical room every run.
- **`make osx-vm-test`** (new, Apple-Silicon-guarded) — `package` → spin VM → install
  each binding fresh → smoke + path-assert → teardown. Uses a **brew-free** base image
  on purpose, so a brew-leak surfaces here even though it hides on `macos-latest`.
- **EULA note in the doc** — macOS's license permits up to **2 macOS VMs on Apple
  hardware**, so tart-on-Mac is above board (unlike Track D on non-Apple hosts).

## Track D — Docker-OSX x86 clean-room — **written per spec (2026-07-07), UNVALIDATED — NOT Mac-gated**

> **Status: written, never executed. This lane needs NO Apple hardware** — it runs
> a macOS guest on Linux under KVM. Unlike Track C, nothing about it is
> Apple-Silicon-gated; it is the *cheaper* of the two unvalidated lanes to bring
> up, and it is the one to try first.
>
> **Two prerequisites, both satisfiable here (verified 2026-07-16):**
> 1. **Bare-metal Linux + `/dev/kvm`** — **present on the current dev host**, so
>    the `HAVE_KVM` guard passes and the lane is launchable today. (Confirmed by
>    the device node's existence; nested/virtualised KVM may still underperform or
>    fail at boot — part of what the shakedown finds out.)
> 2. **A prebuilt `darwin-x86_64` payload** — the one thing a Linux host *cannot*
>    produce. Fetch a release `native-all` artifact (or build it on an Intel mac)
>    and run with `ASMTEST_CLEANROOM_PREBUILT=1`. This is the only real gating
>    step, and it is a download, not hardware.
>
> [`scripts/docker-osx-bindings.sh`](../../../scripts/docker-osx-bindings.sh) +
> `make docker-osx-bindings` ([mk/docker.mk](../../../mk/docker.mk)) implement the
> spec below, including the `HAVE_KVM` guard (the target hard-errors with a clear
> message when `/dev/kvm` is absent) and the `make help` + docs tradeoff notes.
> Like Track C it copies host-staged packages in and runs the Track-A test with
> `ASMTEST_CLEANROOM_PREBUILT=1`. **No bare-metal KVM host was available where this
> was authored, so the lane has never run**; treat the first run as a shakedown
> (Docker-OSX is a brittle virtualised-Hackintosh path — see the tradeoffs below)
> and flip this status when green.

On-demand **x86 macOS** clean room that doesn't wait on the scarce nightly `macos-13`
runner. **Self-hosted bare-metal Linux only** (needs `/dev/kvm`; GitHub hosted runners
don't expose nested KVM). Fits the repo's "prefer Docker via `docker-*` targets"
convention.

- **`make docker-osx-bindings`** (new) — driven by
  [Docker-OSX](https://github.com/sickcodes/Docker-OSX) (`sickcodes/docker-osx:ventura`
  / `:auto`), which packages QEMU + OpenCore + OVMF + a macOS recovery image:
  - `--device /dev/kvm`, headless, SSH on `localhost:50922` (`NOPICKER=true`);
  - `scp` the packages in, run the **same** Track-A scrubbed-env install-test + path
    assert over SSH, capture the result, tear the container down.
- **`HAVE_KVM` guard** — the target hard-errors with a clear message if
  `/dev/kvm` is absent, so it's a no-op on laptops/hosted CI and only runs where it can.
- **Honest tradeoffs (documented in `make help` + docs):** x86-only (OpenCore path —
  no arm64 guest); brittle "virtualized Hackintosh" that can break on macOS
  point-updates; EULA gray on non-Apple hosts; tens-of-GB image; software-rendered
  graphics (irrelevant for a headless CLI smoke). **Not a duplicate** of
  `test-macos-x86-rosetta` — that proves the x86 *ABI* under Rosetta on Apple Silicon;
  this proves a *clean-room x86 dlopen* on a vanilla Intel macOS userland.

## Track E — CI wiring + docs — **done (2026-07-07), with two honest exceptions**

> **Status: implemented.** The hosted lanes are all wired: the release.yml
> per-binding smokes now run under `source scripts/clean-env.sh` (see Track A's
> status note for the shape), the ci.yml `clean-room` job gates every push, and
> Track B runs on the collector. Docs shipped:
> [`docs/clean-room-testing.md`](../../clean-room-testing.md) (linked from the
> docs-site Guides list) with cross-links from
> [portability.md](../../reference/portability.md), [packaging.md](../../reference/packaging.md),
> and [ci.md](../../reference/ci.md), plus a `make help` "Clean-room (macOS)" section.
> **Exception:** the optional self-hosted CI lanes for Tracks C/D are *not*
> wired — those tracks are written but unvalidated and this repo registers no
> self-hosted runners, so a `workflow_dispatch` job would only queue forever; add
> them once a runner exists and the lanes have run green.
>
> *(Update 2026-07-16: this block previously listed a second exception — "the
> CHANGELOG entry is deferred to the next release cut". That is **done**: the
> `macOS clean-room plan — Track E finished, Tracks C/D written` entry is in
> `CHANGELOG.md` under `[Unreleased]`.)*

- **Free lanes on hosted CI:** Track A (scrubbed smoke + path assert) into
  [`release.yml`](../../../.github/workflows/release.yml) and the
  [`ci.yml`](../../../.github/workflows/ci.yml) `test` job; Track B into
  `package-libs-collect`. These cost nothing and ship immediately.
- **Optional self-hosted lanes:** Tracks C/D behind `workflow_dispatch` (and/or a
  `clean-room` PR label), `runs-on: [self-hosted, macOS, arm64]` for tart and
  `[self-hosted, linux, kvm]` for Docker-OSX — never on the hosted pool.
- **Docs:** new `docs/clean-room-testing.md` (the matrix above + how to run each lane
  locally); cross-links from [`docs/portability.md`](../../reference/portability.md),
  [`docs/packaging.md`](../../reference/packaging.md), and [`docs/ci.md`](../../reference/ci.md); a `make help`
  "Clean-room (macOS)" section; one `CHANGELOG.md` entry.

---

## Scope / non-goals

- **Not** emulating arm64 macOS on Linux, nor x86 macOS on Apple Silicon — both are
  impractical under QEMU/OpenCore. Cross-arch stays hosted-runner / real-hardware.
- **Not** replacing the existing hosted macOS jobs — Tracks A/B harden them in place;
  C/D are additive clean-room backstops.
- **Not** adding new corpus cases — the smoke is a feature probe; this plan is about
  the *environment* the load happens in.

## Verification

1. **Track A (no VM):** `make macos-clean-test` on an Apple-Silicon host — every
   binding installs fresh, smokes, and the asserted dlopen path lands under the
   installed package (not `build/`, not Homebrew). Sanity-check the guard by exporting
   `ASMTEST_LIB=$PWD/build/libasmtest_emu.dylib` and confirming the assertion **fails**.
2. **Track B (no macOS):** `make package-libs-verify-macho` over a collected
   `build/dist/native/` tree — passes for universal/correct-arch payloads, fails a
   hand-broken thin slice and an absolute-install-name dylib.
3. **Track C:** `make osx-vm-test` on Apple Silicon — green on a brew-free vanilla
   image; a deliberately brew-linked build fails here while passing on `macos-latest`.
4. **Track D:** `make docker-osx-bindings` on a KVM-capable Linux box — the x86
   bindings install fresh and smoke clean over SSH; the target no-ops with a clear
   message where `/dev/kvm` is absent.
