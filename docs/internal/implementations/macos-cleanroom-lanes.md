# macOS clean-room lane shakedowns (tart arm64, Docker-OSX x86) and sshpass containerization — implementation

> **Sources.** Actioned from
> [macos-clean-test-plan.md](../plans/macos-clean-test-plan.md) (Track C, Track
> D and its 2026-07-17 re-assessment, Verification steps 3–4) and
> [docs/clean-room-testing.md](../../clean-room-testing.md) (the operator's
> guide this work updates). Written 2026-07-17. If this doc and a source
> disagree, this doc wins (sources may be stale); if the CODE and this doc
> disagree, re-verify before implementing.

## Why this work exists

The repo's clean-room story proves that a freshly installed binding loads its
**bundled** native library — never a leaked dev `build/`, a Homebrew dylib, or
a `DYLD_*` override. The hosted-CI lanes (Tracks A/B/E) are done and gating,
but the two highest-fidelity lanes — a **vanilla arm64 macOS VM** via tart
(Track C) and a **vanilla x86-64 macOS userland** via Docker-OSX on Linux/KVM
(Track D) — were written to spec and have **never executed anywhere**. This doc
gets both to their first green run (a shakedown, by the plan's own words), and
removes Track D's host-`sudo` prerequisite by containerizing `sshpass` per
CLAUDE.md's "add it where the work runs" rule. It also repoints Track D at
upstream reality: the `sickcodes/docker-osx:ventura` image the lane defaults to
**no longer exists on Docker Hub** (verified 2026-07-17).

## What already exists (verified 2026-07-17)

The whole Track A harness is landed and green, and both VM lanes exist as code:

- [scripts/clean-env.sh](../../../scripts/clean-env.sh) — sourceable POSIX-sh
  scrub: unsets every `ASMTEST_*`/`DYLD_*`/`LD_*` override, pins
  `DYLD_FALLBACK_LIBRARY_PATH=/usr/lib`, strips Homebrew//usr/local from
  `PATH`, `cd`s outside the checkout.
- [scripts/assert-clean-path.sh](../../../scripts/assert-clean-path.sh) — the
  guard: rejects a resolved library path inside the checkout, `/opt/homebrew`,
  `$HOMEBREW_PREFIX`, or `/usr/local`; with a prefix argument also requires the
  path under the fresh install (or a temp extraction dir).
- [scripts/clean-room-test.sh](../../../scripts/clean-room-test.sh) — the
  driver over ruby/python/node/lua/java/dotnet. Two modes matter here:
  `ASMTEST_CLEANROOM_ONLY` (single-binding, used by `docker-clean-<lang>`) and
  `ASMTEST_CLEANROOM_PREBUILT` (lines 35–40: skip the `make <lang>-package`
  steps and consume artifacts already staged under `build/dist/` — the mode
  both VM lanes use, because the guests are toolchain-free on purpose).
- [scripts/osx-vm.sh](../../../scripts/osx-vm.sh) — Track C: clone a vanilla
  tart VM (arch guard lines 47–50, tart/sshpass preflights 51–54, base-VM check
  55–56, staged-payload check 59–62), boot headless (line 73), wait for
  DHCP + sshd, `tar | ssh` the tree in (lines 100–101), run the Track-A test
  in-guest with `ASMTEST_CLEANROOM_PREBUILT=1` (line 105), delete the VM.
  Banner lines 11–14 say "WRITTEN PER THE PLAN, NOT YET VALIDATED".
- [scripts/docker-osx-bindings.sh](../../../scripts/docker-osx-bindings.sh) —
  Track D: same shape against a Docker-OSX guest. Defaults at lines 42–44
  (`sickcodes/docker-osx:ventura`, `user`/`alpine`), KVM guard 49–50, host
  sshpass preflight 52–53 (the thing T4 removes), `docker run` flags 64–69
  (`--device /dev/kvm -p 50922:10022 -e NOPICKER=true -e GENERATE_UNIQUE=true`),
  sshd wait loop 81–89 (default 180 tries × 10 s), in-guest run line 98.
- Make targets: `osx-vm-test` in
  [mk/bindings.mk](../../../mk/bindings.mk) lines 618–620 (comment 611–617);
  `docker-osx-bindings` in [mk/docker.mk](../../../mk/docker.mk) lines 678–684
  with `DOCKER_OSX_IMAGE ?= sickcodes/docker-osx:ventura` at line 677;
  `make help` lists both under "Clean-room (macOS plan…)" at
  [Makefile](../../../Makefile) lines 171–174, each flagged "written per plan,
  UNVALIDATED".
- Patterns to mirror: [Dockerfile.hwtrace](../../../Dockerfile.hwtrace) lines
  20–22 (`apt-get install -y --no-install-recommends … && rm -rf
  /var/lib/apt/lists/*` on `ARG BASE_IMAGE`),
  [Dockerfile.drtrace](../../../Dockerfile.drtrace) lines 15 and 34–41
  (`ARG BASE=ubuntu:24.04`; pinned-version `ARG` + `curl` block), and the
  build-then-run target shapes in [mk/docker.mk](../../../mk/docker.mk)
  (e.g. `docker-bindings-base` at lines 182–184, the generated
  `docker-build-<lang>`/`docker-clean-<lang>` rules at 193–203).
- The release pipeline already produces the payload Track D needs:
  [release.yml](../../../.github/workflows/release.yml) builds
  `native-payload-<os>` on `[ubuntu-latest, ubuntu-24.04-arm, macos-latest,
  macos-15-intel]` (line 35) and merges them into a single `native-all`
  artifact (job at line 67) gated by
  `EXPECT_PLATFORMS="linux-x86_64 linux-aarch64 darwin-x86_64 darwin-arm64"`
  (line 89). Dry-run **29514910178** (2026-07-16) produced a valid
  `native-all` with all four platform dirs at the artifact root; verified today
  by actually downloading it and running `file` on the darwin-x86_64 dylibs
  (all "Mach-O 64-bit … x86_64"). It expires **2026-10-14**.

**Prove the baseline before touching anything** (all runnable on any dev host):

```sh
make help | grep -A3 'Clean-room'   # shows all four lane entries, C/D flagged UNVALIDATED
make clean-room-test                # Track A on the host: ends "clean-room-test: OK — every
                                    # present binding resolved its native lib from its fresh install."
make osx-vm-test                    # on a non-Apple-Silicon host: exits 1 with
                                    # "osx-vm.sh: Apple-Silicon macOS only (host is …)"
make docker-osx-bindings            # on a host without /dev/kvm: exits 1 with
                                    # "docker-osx-bindings: /dev/kvm absent — needs a bare-metal Linux host with KVM …"
```

Host facts for the machine this doc was written on (why the shakedowns cannot
run *here*): `uname -s`/`-m` = `Darwin`/`x86_64` (Intel Mac), no `/dev/kvm`, no
`tart`, no `sshpass`.

## Tasks

### T1 — Preflight an Apple-Silicon host for the tart lane  (S, depends on: none; **gated: Apple Silicon**)

**Goal.** An Apple-Silicon Mac has tart, sshpass, a pinned vanilla base VM, and
host-staged packages, so `make osx-vm-test` can start.

**Steps.**

1. On an Apple-Silicon Mac (macOS 13.0+; `uname -m` must print `arm64`), clone
   the repo and install the two host tools:
   ```sh
   brew install cirruslabs/cli/tart    # Cirrus Labs' own tap; not in homebrew-core
   brew install sshpass                # homebrew-core (1.10); if your brew does not carry it,
                                       # use the esolitos/ipa tap fallback (same 1.10 tarball, see Research notes):
                                       #   brew install esolitos/ipa/sshpass
   tart --version && sshpass -V | head -1
   ```
   Record the tart version in your shakedown notes (latest release today is
   2.33.0, published 2026-07-17 — see Research notes). These are **host**
   installs by necessity, not a CLAUDE.md violation: tart drives Apple's
   Virtualization.framework and cannot run inside a container — the same class
   of exception as hardware.
2. Pull the pinned vanilla (no Xcode, no Homebrew) base image under the name
   [scripts/osx-vm.sh](../../../scripts/osx-vm.sh) line 41 defaults to:
   ```sh
   tart clone ghcr.io/cirruslabs/macos-sequoia-vanilla:15.7.7 macos-vanilla
   tart list    # shows macos-vanilla
   ```
   `15.7.7` is what `:latest` resolved to when verified (2026-07-17); pinning
   the tag matches the repo's digest discipline
   ([scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt))
   without adding a digests line — the VM image is test infrastructure, never
   bundled into a user-facing package, which is what that file covers.
3. Update the two `:latest` hint strings in
   [scripts/osx-vm.sh](../../../scripts/osx-vm.sh) (comment line 31 and the
   no-base-VM error at line 56) from
   `ghcr.io/cirruslabs/macos-sequoia-vanilla:latest` to `…:15.7.7` so the
   script's own guidance is pinned too.
4. Stage everything the toolchain-free guest will consume, on the host:
   ```sh
   make packages package-libs
   ls build/dist/native/darwin-arm64/libasmtest_emu.dylib   # the script's preflight (osx-vm.sh:59)
   ls build/dist/ruby/asmtest-*.gem                          # what the in-guest ruby lane installs
   ```
   `packages` ([mk/bindings.mk](../../../mk/bindings.mk) line 472) drives every
   per-language packer including the link bindings; if a link-binding toolchain
   (rust/zig/go) is absent on the host, run the dlopen set individually instead
   — the guest only consumes those six:
   `make package-libs ruby-package python-package node-package lua-package
   java-package dotnet-package` (skip any whose toolchain the host lacks; the
   guest lane for it will self-skip or fail visibly, see T2).

**Code.** Only step 3's two-string edit in `scripts/osx-vm.sh`; everything else
is host setup.

**Tests.** No repo test surface — manual verification: the four commands in
steps 1–2–4 print a tart version, `sshpass 1.10`, `macos-vanilla` in
`tart list`, and both staged artifacts. `sh -n scripts/osx-vm.sh` after step 3.

**Docs.** Internal-only, no user-facing docs — this is operator setup already
described by [docs/clean-room-testing.md](../../clean-room-testing.md); T3 owns
the doc updates once the lane is green.

**Done when.**

- `tart list` shows `macos-vanilla`; `sshpass -V` prints 1.10 — from
  homebrew-core, or via the `esolitos/ipa` tap fallback if your brew does not
  carry it (`brew install esolitos/ipa/sshpass`, same 1.10 tarball — see
  Research notes). Either source satisfies this check.
- `build/dist/native/darwin-arm64/libasmtest_emu.dylib` and at least the ruby
  gem exist under `build/dist/`.
- `scripts/osx-vm.sh` mentions only the pinned image tag.

### T2 — Run the Track C shakedown: `make osx-vm-test` green on Apple Silicon  (M, depends on: T1; **gated: Apple Silicon**)

**Goal.** `make osx-vm-test` completes end-to-end — clone, headless boot, SSH
copy, in-guest clean-room test, VM delete — exiting 0 with a truthful summary.

**Steps.**

1. Run `make osx-vm-test` from the repo root. Expected happy-path output, in
   order: `cloning macos-vanilla -> asmtest-cleanroom`, `booting … headless`,
   `guest up at <ip>`, `copying the working tree …`, `running the Track-A
   clean-room install test in the vanilla guest`, the
   `clean-room-test summary (darwin-arm64):` table, `done (rc=0); deleting the
   VM`. The `trap cleanup` (osx-vm.sh:66–70) must delete the VM on **every**
   exit path — verify with `tart list` afterwards.
2. Know the expected in-guest shape before calling anything a bug. The vanilla
   guest has **no** Node, LuaJIT, .NET, JDK, or delocate, so those lanes
   self-skip ("that is the expected shape of a green run, not a failure" —
   osx-vm.sh:21–22). macOS still ships a system `/usr/bin/ruby` + `gem`, so the
   **ruby lane is the one likely live PASS**: with
   `ASMTEST_CLEANROOM_PREBUILT=1` it installs the host-staged gem into a
   throwaway `GEM_HOME` and asserts the resolved dylib path
   ([scripts/clean-room-test.sh](../../../scripts/clean-room-test.sh) lines
   62–77). Java is expected to SKIP via the macOS JRE-less-stub check (line
   147). If **every** lane skips, the run still exits 0 — record that outcome
   honestly in the plan rather than inventing a pass (and consider whether the
   guest image ships ruby at all; if not, that is a finding for the plan, not a
   reason to install toolchains in the guest).
3. Triage the likely first-run breakages, in the order the script hits them:
   - *VM never reported an IP* (osx-vm.sh:83): `tart run --no-graphics` was
     backgrounded with output discarded (line 73); re-run it foreground in
     another terminal to see the real Virtualization.framework error.
   - *sshd never came up* (line 94): the vanilla images use auto-login +
     password auth `admin`/`admin` (script defaults, lines 43–44); confirm
     Remote Login is enabled in the image by running the VM with graphics once.
   - *tar copy hangs/fails* (lines 100–101): host bsdtar piping into the guest
     over sshpass — test the pipe alone with
     `echo hi | sshpass -p admin ssh … 'cat'`.
   - *in-guest failure*: re-run the in-guest command by hand
     (`ssh … 'cd asmtest && ASMTEST_CLEANROOM_PREBUILT=1 ASMTEST_REPO_ROOT="$PWD"
     sh scripts/clean-room-test.sh'`) to see the per-binding
     `PASS`/`SKIP`/`FAIL` lines that `make` swallowed.
4. Fix whatever broke **in the script/harness in-tree** (not by hand-patching
   the guest — the VM is deleted every run by design), commit, and re-run until
   green.
5. Optional deeper validation (plan Verification step 3): confirm the lane
   catches what it exists to catch by feeding
   `scripts/assert-clean-path.sh /opt/homebrew/lib/libasmtest_emu.dylib` on the
   host and seeing `LEAK: resolved through Homebrew …` exit 1 — the same guard
   the guest runs.

**Code.** Whatever the shakedown demands, confined to
[scripts/osx-vm.sh](../../../scripts/osx-vm.sh) /
[scripts/clean-room-test.sh](../../../scripts/clean-room-test.sh) (guest-side
env quirks) and, if timing is the issue, the wait-loop bounds (osx-vm.sh lines
78, 92–96). Do not weaken the assert script to get to green.

**Tests.** The run is the test; there is no unit surface for a VM boot. A pass
looks like step 1's output with rc=0 and at least the expected SKIP lines; a
failure looks like a nonzero rc plus one of the step-3 messages. Also verify
teardown-on-failure: interrupt a run (Ctrl-C mid-boot) and confirm
`tart list` shows no `asmtest-cleanroom` leftover.

**Docs.** Internal-only for this task; T3 owns every status flip. Record the
shakedown outcome (tart version, image tag, per-binding verdicts, anything
fixed) in the commit message.

**Done when.**

- `make osx-vm-test` exits 0 on an Apple-Silicon host with the summary table
  printed and the VM deleted afterwards (`tart list` clean).
- Any in-tree fixes are committed with the run evidence.
- On a non-Apple-Silicon host the target still fails fast with the existing
  arch-guard message (unchanged behavior — the lane self-skips cleanly there
  in the sense the guard defines: a loud, explained hard stop, per the plan).

### T3 — Flip Track C status everywhere it is recorded  (S, depends on: T2)

**Goal.** No file still calls Track C unvalidated once it has run green.

**Steps.** Update, in one commit:

1. [docs/internal/plans/macos-clean-test-plan.md](../plans/macos-clean-test-plan.md):
   the Track C heading (line 189, `**written per spec (2026-07-07), UNVALIDATED —
   needs Apple Silicon**` → `**done (<green-run date>)**`), its status block
   (lines 190–203), the top-of-file "3 of 5 tracks done — A, B, E" note (line
   12) → "4 of 5 … A, B, C, E", and the Track C bullet in that note (line 29).
2. [scripts/osx-vm.sh](../../../scripts/osx-vm.sh) banner lines 11–14: replace
   the `*** WRITTEN PER THE PLAN, NOT YET VALIDATED` block with one line
   recording the first green run (host chip, macOS + image tag, date).
3. [mk/bindings.mk](../../../mk/bindings.mk) lines 616–617: drop the
   "WRITTEN PER THE PLAN, NOT YET VALIDATED" comment sentence.
4. [Makefile](../../../Makefile) line 173: help text `(Apple Silicon; written
   per plan, UNVALIDATED)` → `(Apple Silicon)`.
5. [docs/clean-room-testing.md](../../clean-room-testing.md) section "The macOS
   VM lanes (Tracks C/D — written per plan, unvalidated)" (lines 94–126):
   retitle and reword so Track C is validated (state the guest image + date)
   while Track D keeps its unvalidated flag until T6.
6. [CHANGELOG.md](../../../CHANGELOG.md) under `## [Unreleased]`: one line
   under the existing `### Added` (or `### Changed` if that fits the entry set
   at commit time), e.g. "macOS clean-room Track C (tart vanilla arm64 VM,
   `make osx-vm-test`) validated on Apple Silicon — first green shakedown run."

**Code.** Comment/doc strings only; no behavior change.

**Tests.** `make docker-docs` (Sphinx `-W`, containerized — the repo's
preferred lane) builds green;
`grep -rn "UNVALIDATED" Makefile mk/ scripts/osx-vm.sh docs/clean-room-testing.md`
returns only Track D hits.

**Docs.** This task *is* the docs update (step 5–6).

**Done when.**

- The grep above shows no Track C "unvalidated" claim anywhere.
- `make help` shows Track C without the UNVALIDATED flag, Track D still with it.
- Docs build green; CHANGELOG has the entry.

### T4 — Containerize sshpass for the Docker-OSX lane  (S, depends on: none — **ungated**)

**Goal.** `make docker-osx-bindings` needs no host `sshpass` and no `sudo`:
every ssh/scp-equivalent call runs through a small pinned image, per CLAUDE.md's
"add it where the work runs" rule.

**Steps.**

1. Create `Dockerfile.sshpass` at the repo root, mirroring
   [Dockerfile.hwtrace](../../../Dockerfile.hwtrace)'s apt block and
   [Dockerfile.drtrace](../../../Dockerfile.drtrace)'s `ARG BASE=ubuntu:24.04`
   header:
   ```dockerfile
   # Dockerfile.sshpass — tiny ssh client image for the Docker-OSX clean-room
   # lane (Track D of docs/internal/plans/macos-clean-test-plan.md). The lane's
   # guest uses password auth, and per CLAUDE.md a missing installable
   # dependency is containerized, not demanded of the host — so the lane's
   # ssh calls run through this image instead of a host sshpass (which would
   # need sudo to install). Version pinned to noble's package; re-check the
   # exact string on a distro point-rebuild (e.g. 1.09-1build1).
   ARG BASE=ubuntu:24.04
   FROM ${BASE}
   ENV DEBIAN_FRONTEND=noninteractive
   RUN apt-get update \
    && apt-get install -y --no-install-recommends sshpass=1.09-1 openssh-client \
    && rm -rf /var/lib/apt/lists/*
   ```
   `sshpass=1.09-1` is noble's current universe version (see Research notes).
   No `scripts/third-party-digests.txt` line: that file covers engines bundled
   into user-facing packages; an apt package from the pinned distro is the
   "apt, where a pinned distro package suffices" case in CLAUDE.md.
2. Add the build target in [mk/docker.mk](../../../mk/docker.mk), next to the
   Track D block (before line 677), following the `docker-bindings-base` shape
   (lines 182–184):
   ```make
   # sshpass client image for the Docker-OSX lane — see Dockerfile.sshpass.
   .PHONY: docker-sshpass
   docker-sshpass:
   	$(DOCKER) build $(_docker_plat) -f Dockerfile.sshpass \
   	  --build-arg BASE=$(DOCKER_BASE) -t asmtest-sshpass .
   	$(DOCKER) run --rm asmtest-sshpass sshpass -V | head -1
   ```
   and make the lane depend on it: `docker-osx-bindings: docker-sshpass` (line
   679).
3. Rework [scripts/docker-osx-bindings.sh](../../../scripts/docker-osx-bindings.sh):
   - **Delete** the host preflight at lines 52–53
     (`command -v sshpass … apt-get install sshpass`).
   - Add near the other defaults (after line 45):
     ```sh
     SSHPASS_IMG=${ASMTEST_SSHPASS_IMAGE:-asmtest-sshpass}
     docker image inspect "$SSHPASS_IMG" >/dev/null 2>&1 || {
         echo "$prog: sshpass image '$SSHPASS_IMG' not built — run 'make docker-sshpass'" >&2; exit 1; }
     ```
   - Replace the `ssh_g` body (lines 72–76) with the containerized call —
     `-i` keeps stdin piping working for the `tar | ssh_g` copy at lines 93–94,
     `--network host` makes the guest's published `localhost:50922` reachable
     (this lane is Linux-only, where host networking is real), and `sshpass -e`
     reads the password from `SSHPASS` instead of putting it in `ps` output:
     ```sh
     ssh_g() {
         docker run --rm -i --network host -e SSHPASS="$GPASS" "$SSHPASS_IMG" \
             sshpass -e ssh -p 50922 \
             -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
             -o LogLevel=ERROR "$GUSER@localhost" "$@"
     }
     ```
   - Update the header comment (lines 29–31) to say docker-only, no host
     sshpass.
4. Deliberately do **not** touch
   [scripts/osx-vm.sh](../../../scripts/osx-vm.sh)'s host sshpass (lines
   53–54, 86): the tart lane runs on macOS where Docker is not a lane
   prerequisite, and `brew install sshpass` is now a plain homebrew-core
   install (no tap, no sudo) — the blocker CLAUDE.md targets does not exist
   there. Its error message at line 54 is already correct.

**Code.** As above: one new Dockerfile, one new make target + one dependency
edit, one script rework.

**Tests.** All runnable on any host with Docker, no KVM needed:

- `make docker-sshpass` — builds and prints exactly `sshpass 1.09` (a version
  drift in noble shows up here as an apt "version not found" build failure —
  the intended loud signal).
- Stdin piping through the shim (the riskiest part of the change):
  `echo hello | docker run --rm -i asmtest-sshpass sh -c 'cat | wc -c'` prints
  `6`.
- `sh -n scripts/docker-osx-bindings.sh` parses.
- On a non-KVM host `make docker-osx-bindings` now builds the sshpass image
  first, then still fails fast with the `/dev/kvm absent` guard message —
  ordering is visible in the output.

**Docs.** [docs/clean-room-testing.md](../../clean-room-testing.md) Track D
row (line 106): "Host needed" becomes "bare-metal Linux + `/dev/kvm` + docker"
(sshpass no longer a host requirement). CHANGELOG `## [Unreleased]` gets one
`### Changed`-style line: "Docker-OSX clean-room lane: sshpass containerized
(`Dockerfile.sshpass`, `make docker-sshpass`) — no host install/sudo needed."
(May share a combined entry with T5.)

**Done when.**

- `grep -n sshpass scripts/docker-osx-bindings.sh` shows only the container
  invocation — no `command -v sshpass`, no host-install advice.
- The three Docker-based checks above pass on a KVM-less host.
- `make docker-osx-bindings` off-KVM: image builds, then the unchanged guard
  error — exit 1 with the friendly message.

### T5 — Repoint Track D at upstream Docker-OSX reality (dead tags, prebuilt-disk path)  (S, depends on: none — **ungated**)

**Goal.** The lane's defaults and docs reflect what `sickcodes/docker-osx`
actually publishes in 2026, so the T6 operator is not sent to pull an image
that 404s.

Upstream facts (all verified 2026-07-17; URLs in Research notes): Docker Hub
now has exactly three tags — `latest`, `master` (both pushed 2025-11-11), and
one commit-sha tag. **`:ventura`, `:auto`, `:sonoma`, `:sequoia`, `:naked` all
return 404** (the Hub repo was deleted ~2024-08-28 and only latest/master were
re-pushed); the GitHub README still advertising them is stale. `:latest` is
Arch-based, `SHORTNAME=sequoia`, and its CMD downloads the Sequoia
**recovery/installer** on first boot — a fresh install needing one interactive
session, so this script's headless wait-for-sshd would time out on a virgin
run. The `user`/`alpine` credentials belonged to the `:auto` lineage, whose
prebuilt disk URL (`images.sick.codes/mac_hdd_ng_auto.img`) now 404s — the
fully-prebuilt headless path is unbuildable as shipped. The scriptable shape
that remains: **one interactive install producing a reusable `mac_hdd_ng.img`,
then headless runs against that disk.**

**Steps.**

1. In [mk/docker.mk](../../../mk/docker.mk) line 677 change the default:
   `DOCKER_OSX_IMAGE ?= sickcodes/docker-osx:latest` (the only maintained tag
   that exists), and extend the comment block above the target (lines 668–676)
   with two sentences: version tags are dead upstream since the 2024 Hub
   deletion; first boot of `:latest` is an interactive installer, so a
   prebuilt disk (`DOCKER_OSX_DISK`) is required for headless runs.
2. In [scripts/docker-osx-bindings.sh](../../../scripts/docker-osx-bindings.sh):
   - line 42: `IMG=${DOCKER_OSX_IMAGE:-sickcodes/docker-osx:latest}`;
   - add `DISK=${DOCKER_OSX_DISK:-}` beside it, and build the `docker run`
     argument list POSIX-safely (replacing lines 64–69):
     ```sh
     set -- -d --name "$NAME" --device /dev/kvm -p 50922:10022 \
         -e NOPICKER=true -e GENERATE_UNIQUE=true
     if [ -n "$DISK" ]; then
         [ -f "$DISK" ] || { echo "$prog: DOCKER_OSX_DISK '$DISK' not found" >&2; exit 1; }
         set -- "$@" -v "$DISK:/image" -e IMAGE_PATH=/image
     else
         echo "$prog: WARNING no DOCKER_OSX_DISK — a virgin $IMG boots the macOS *installer*," >&2
         echo "$prog: which needs one interactive session first; headless sshd will likely time out." >&2
     fi
     docker rm -f "$NAME" >/dev/null 2>&1 || true
     docker run "$@" "$IMG" >/dev/null
     ```
   - rewrite the header's image/credential paragraphs (lines 4–6, 34–36):
     document the surviving tags, that `user`/`alpine` only exist if the
     operator created that account during the one-time install (or override
     `ASMTEST_OSX_USER`/`ASMTEST_OSX_PASS`), and the one-time-install recipe
     (T6 step 2 below, condensed).
   - keep `NOPICKER=true` (verified still honored: it deletes the InstallMedia
     boot entry and boots the nopicker OpenCore image) and the `50922:10022`
     forward (both confirmed unchanged in the current image's CMD).
3. Update [docs/clean-room-testing.md](../../clean-room-testing.md)'s Track D
   note block (lines 115–126) with the same reality: `:latest` only, one-time
   interactive install, `DOCKER_OSX_DISK` for headless reuse, and cite the
   plan's re-assessment that this lane is a **backstop** —
   [macos-clean-test-plan.md](../plans/macos-clean-test-plan.md) Track D
   re-assessment (lines 238–276): `macos-15-intel` CI legs now give real
   Intel-Apple clean-room coverage, so the lane's remaining unique value is a
   *vanilla* (non-CI-image) Intel-macOS userland.
4. Note the mount-point caveat honestly in both the script header and the doc:
   `-v <disk>:/image -e IMAGE_PATH=/image` is the long-standing Docker-OSX
   reuse convention (formerly the `:naked` flow), but the current `:latest`
   `Launch.sh`'s handling of an overridden `IMAGE_PATH` is **shakedown-verified
   in T6, not pre-verified here** — if it is not honored, the fallback is
   building the still-present `Dockerfile.naked` from the pinned upstream repo
   (record the exact commit you build from in the script header when you do).

**Code.** As in steps 1–2; no behavior change on hosts without KVM (the make
guard still fires first).

**Tests.** `sh -n scripts/docker-osx-bindings.sh`. On any Docker host:
`docker pull sickcodes/docker-osx:latest` succeeds (≈1.24 GB compressed —
this is the QEMU/Arch wrapper, not the macOS disk) while
`docker pull sickcodes/docker-osx:ventura` fails with "manifest … not found" —
demonstrating exactly why this task exists. Off-KVM, `make docker-osx-bindings`
still exits 1 at the guard. `make docker-docs` builds green after step 3.

**Docs.** Step 3, plus a shared CHANGELOG line with T4 or its own:
"Docker-OSX clean-room lane repointed at surviving upstream tags
(`:ventura` et al. deleted from Docker Hub); added `DOCKER_OSX_DISK` prebuilt
disk support."

**Done when.**

- `grep -n "docker-osx:" mk/docker.mk scripts/docker-osx-bindings.sh` shows
  only `:latest`.
- Running the script with `DOCKER_OSX_DISK=/nonexistent` on a KVM host (or
  reading the code path) errors before `docker run`; without the var it prints
  the two-line WARNING.
- Docs and script header no longer promise `user`/`alpine` as shipped-in
  credentials.

### T6 — Track D shakedown: first green `make docker-osx-bindings` on a KVM box, then flip its status  (L, depends on: T4, T5; **gated: bare-metal Linux + `/dev/kvm`**)

**Goal.** On a bare-metal Linux host with `/dev/kvm`, the lane runs end-to-end
against a self-made prebuilt disk and exits 0; every "UNVALIDATED" marker for
Track D is then flipped.

**Steps.**

1. **Stage the payload + packages** on the KVM box (a Linux host cannot build
   Mach-O, so fetch the release artifact — verified downloadable today, expires
   2026-10-14):
   ```sh
   gh auth status || gh auth login   # artifact/workflow calls need an authenticated gh, even for a public repo
   gh run download 29514910178 --repo wilvk/asm-test -n native-all -D build/dist/native
   file build/dist/native/darwin-x86_64/libasmtest_emu.dylib   # "Mach-O 64-bit … x86_64"
   make packages   # or the per-dlopen-binding <lang>-package targets, as in T1 step 4
   ```
   The artifact root contains the four platform dirs directly
   (`darwin-x86_64/`, `darwin-arm64/`, `linux-x86_64/`, `linux-aarch64/`), so
   `-D build/dist/native` lands `darwin-x86_64` exactly where the script's
   preflight (docker-osx-bindings.sh:56) looks. If the artifact has expired,
   dispatch the workflow again (`gh workflow run 'release (dry-run)' --repo
   wilvk/asm-test`) and download from the new run — the `native-all` job's
   `EXPECT_PLATFORMS` gate guarantees the darwin-x86_64 slot or a loud failure.
2. **One-time interactive install** (the step the 2024 upstream deletion made
   unavoidable; budget hours, tens of GB — disk-space check first):
   ```sh
   docker run -it --name asmtest-osx-install --device /dev/kvm -p 50922:10022 \
     -v /tmp/.X11-unix:/tmp/.X11-unix -e "DISPLAY=${DISPLAY:-:0.0}" \
     -e GENERATE_UNIQUE=true sickcodes/docker-osx:latest
   ```
   In the QEMU window: boot the recovery, Disk Utility → erase the largest
   virtio disk (APFS), install macOS (Sequoia), create the account
   **`user` / `alpine`** (matching the script defaults; anything else needs
   `ASMTEST_OSX_USER`/`ASMTEST_OSX_PASS`), then System Settings → General →
   Sharing → **Remote Login ON**. Shut the guest down cleanly, then extract the
   disk (default upstream path; if absent, `docker exec asmtest-osx-install
   find / -name 'mac_hdd_ng*.img' 2>/dev/null`):
   ```sh
   docker cp asmtest-osx-install:/home/arch/OSX-KVM/mac_hdd_ng.img ./mac_hdd_ng.img
   docker rm -f asmtest-osx-install
   ```
3. **Headless shakedown run**:
   ```sh
   DOCKER_OSX_DISK=$PWD/mac_hdd_ng.img make docker-osx-bindings
   ```
   Expected: sshpass image builds (T4), KVM guard passes, guest boots from the
   prebuilt disk, `guest up` within the 30-min wait window, tree copied, the
   in-guest summary table prints (same expected shape as T2 step 2 — ruby the
   likely live PASS on a vanilla guest, the rest SKIP), `done (rc=0); removing
   the container`. This step also **verifies T5's `IMAGE_PATH` hedge**: if the
   guest ignores the mounted disk and boots the installer instead, fall back to
   building `Dockerfile.naked` from the upstream repo at a recorded commit and
   set `DOCKER_OSX_IMAGE` to that local build — then fold that recipe back
   into the script header/docs.
4. Triage with the same discipline as T2 step 3: sshd timeout → `docker logs
   --tail 50 asmtest-docker-osx` (the script already prints this on timeout,
   lines 84–86); ssh reachable but auth fails → wrong in-guest account vs
   `ASMTEST_OSX_USER/PASS`; fix in-tree, re-run.
5. **Flip Track D status** (mirror of T3, one commit):
   [macos-clean-test-plan.md](../plans/macos-clean-test-plan.md) Track D
   heading (line 219) + status block + top note ("4 of 5" → "5 of 5");
   [scripts/docker-osx-bindings.sh](../../../scripts/docker-osx-bindings.sh)
   banner lines 12–15; [mk/docker.mk](../../../mk/docker.mk) comment lines
   675–676; [Makefile](../../../Makefile) line 174;
   [docs/clean-room-testing.md](../../clean-room-testing.md) Tracks C/D
   section; one CHANGELOG `[Unreleased]` line. Keep the plan's honest framing:
   this is a **backstop** lane (its re-assessment stands — `macos-15-intel` CI
   covers Intel-Apple; this uniquely covers a *vanilla* Intel userland).

**Code.** Only what step 3/4 triage demands, in the script/harness; plus the
step-5 status flips.

**Tests.** The run is the test. Pass: step 3's output, rc 0, container removed
(`docker ps -a` clean). Failure: nonzero rc with the guard/timeout/auth
messages above. Post-flip: `grep -rn "UNVALIDATED" Makefile mk/ scripts/
docs/clean-room-testing.md` returns nothing; `make docker-docs` green. On any
non-KVM host, `make docker-osx-bindings` must still fail fast at the guard —
the lane self-skips cleanly (loudly, with the documented reason) everywhere the
hardware gate is unmet.

**Docs.** Step 5 is the docs work.

**Done when.**

- `DOCKER_OSX_DISK=… make docker-osx-bindings` exits 0 on the KVM box with the
  summary table; re-running it reuses the disk without another install.
- No Track D "UNVALIDATED" marker remains; CHANGELOG updated.
- The mount-point mechanism actually used (`:latest` + `IMAGE_PATH`, or a
  locally-built naked image at a recorded commit) is the one the script header
  and docs describe.

## Task order & parallelism

Two independent streams, one per lane:

```
Track C (Apple-Silicon gated):  T1 → T2 → T3
Track D:                        T4 ─┐
                                T5 ─┴→ T6 (KVM gated)
```

- T4 and T5 are ungated and can land **today**, in parallel, by different
  people; they touch disjoint code (new Dockerfile/target vs. defaults/docs)
  except both edit `scripts/docker-osx-bindings.sh` — coordinate the merge or
  do them serially in either order.
- T1–T3 need an Apple-Silicon Mac; T6 needs a bare-metal Linux KVM box. The
  two streams never block each other.
- Critical path to "both lanes validated": T6 (the interactive-install step
  dominates). Land T4+T5 first regardless — they are the only parts with no
  hardware gate, and per the plan's re-assessment T6 is a deprioritized
  backstop: weigh its ~30 GB + hours-long install against its remaining unique
  value before scheduling it.

## Constraints & gates

- **Apple Silicon (T1–T3)** — real hardware gate. tart uses Apple's
  Virtualization.framework and is Apple-Silicon-only (macOS 13.0+); there is no
  container or emulation substitute. On any other host `make osx-vm-test` must
  keep failing fast with the existing arch-guard message. Record the gate, do
  not narrow the lane.
- **Bare-metal Linux + `/dev/kvm` (T6)** — real hardware gate (global
  consistency position: the plan's 2026-07-16 "launchable today" note referred
  to a different host; on any macOS host — including the Intel Mac this doc was
  verified on — there is no `/dev/kvm`, and GitHub hosted runners expose no
  nested KVM). The `HAVE_KVM` guard in
  [mk/docker.mk](../../../mk/docker.mk) lines 680–682 is the self-skip.
- **EULA** — macOS's license permits up to 2 macOS VMs **on Apple hardware**:
  Track C is above board; Track D on non-Apple hosts is EULA-gray and stays an
  opt-in, self-hosted-only, never-hosted-CI lane (already documented in
  [docs/clean-room-testing.md](../../clean-room-testing.md) lines 115–119 —
  keep that framing in every edit).
- **Pinning** — `sshpass=1.09-1` in `Dockerfile.sshpass` (noble); tart vanilla
  image by version tag (`15.7.7`); Docker Hub tags for docker-osx are outside
  our control — `:latest` is the only maintained name, so record the image's
  config digest / `org.opencontainers.image.version` label in the T6 shakedown
  notes, and record the upstream git commit if the `Dockerfile.naked` fallback
  is built. No `scripts/third-party-digests.txt` entries: nothing here is
  bundled into a user-facing package (that file's scope).
- **Artifact retention** — `native-all` from run 29514910178 expires
  **2026-10-14** (90-day default). After that, `gh workflow run
  'release (dry-run)'` regenerates it; the `EXPECT_PLATFORMS` gate makes a
  missing darwin-x86_64 slot loud. Never point CI legs at retired runners:
  macOS Intel legs are `macos-15-intel`, never `macos-13` (retired 2025-12-08).
- **When a gate blocks validation**: land the ungated tasks (T4, T5), leave the
  UNVALIDATED markers exactly as they are (T3/T6 flip them only on real green
  runs), and record in the plan which gate blocked and on what host — the
  pattern the plan already uses.

## Research notes (verified 2026-07-17)

- **tart install**: `brew install cirruslabs/cli/tart` (Cirrus Labs' own tap;
  not in homebrew-core) — https://tart.run/quick-start/. Requires Apple
  Silicon, macOS 13.0+. Latest release `2.33.0`, published 2026-07-17 (note:
  relicensed FSL-1.1-ALv2) — https://github.com/cirruslabs/tart/releases.
- **Vanilla (no-Xcode, no-brew) images**:
  `ghcr.io/cirruslabs/macos-{tahoe,sequoia,sonoma}-vanilla` — "a vanilla macOS
  installation with helpful tweaks such as auto-login, but no additional
  software preinstalled" (the `-base` variant is the one that adds Homebrew) —
  https://github.com/cirruslabs/macos-image-templates. Current tags:
  sequoia-vanilla `:latest` == `15.7.7`
  (https://github.com/cirruslabs/macos-image-templates/pkgs/container/macos-sequoia-vanilla),
  tahoe-vanilla `:latest` == `26.5`, digest
  `sha256:e12d678b248f3122e276fa64632970a8e1c6dc60ff6738d21fe9bfa5ea58f426`
  (https://github.com/cirruslabs/macos-image-templates/pkgs/container/macos-tahoe-vanilla).
  Password auth admin/admin as the script assumes (ecosystem docs + the
  script's own defaults; not quoted verbatim from the upstream README).
- **sshpass on macOS**: now a plain homebrew-core formula, version 1.10 —
  https://formulae.brew.sh/formula/sshpass (formula:
  https://github.com/Homebrew/homebrew-core/blob/master/Formula/s/sshpass.rb,
  SourceForge tarball sha256
  `ad1106c203cbb56185ca3bad8c6ccafca3b4064696194da879f81c8d7bdfeeda`). The
  historical tap still works as fallback: `brew install esolitos/ipa/sshpass`
  (https://github.com/esolitos/homebrew-ipa, same 1.10 tarball). The hint in
  `scripts/osx-vm.sh:54` (`brew install sshpass`) is already correct.
- **sshpass on Ubuntu 24.04 (noble)**: `1.09-1` (universe) —
  https://packages.ubuntu.com/noble/sshpass. Re-check the exact string at
  image-build time (a point rebuild like `1.09-1build1` would break the pin
  loudly).
- **sickcodes/docker-osx tags**: exactly `latest`, `master` (pushed
  2025-11-11) and one commit-sha tag —
  https://hub.docker.com/v2/repositories/sickcodes/docker-osx/tags; `:ventura`,
  `:auto`, `:sonoma`, `:sequoia`, `:naked` all 404 on the tag API. Cause: the
  Hub repo was deleted ~2024-08-28
  (https://x.com/sickcodes/status/1828696525834457186,
  https://github.com/sickcodes/Docker-OSX/issues/799); only latest/master were
  re-pushed. ghcr.io/sickcodes/docker-osx is not anonymously pullable (token
  DENIED). The README (https://github.com/sickcodes/Docker-OSX) still
  advertises the dead tags — stale vs. the registry. Repo last pushed
  2025-11-11 (https://api.github.com/repos/sickcodes/Docker-OSX).
- **What `:latest` is**: config blob created 2025-11-11, label
  `org.opencontainers.image.version=20251019.0.436919`; Arch-based,
  `SHORTNAME=sequoia`, CMD downloads the Sequoia BaseSystem **recovery** on
  first boot (fresh install, not a preinstalled system — the
  headless-timeout inference in T5/T6 comes from this CMD/ENV, not from an
  actual boot). Boot flags `--device /dev/kvm -p 50922:10022 -e NOPICKER=true
  -e GENERATE_UNIQUE=true` confirmed unchanged; `NOPICKER=true` deletes the
  InstallMedia entry and boots the nopicker OpenCore image. `user`/`alpine`
  belongs to the `:auto` lineage (Dockerfile.auto,
  https://raw.githubusercontent.com/sickcodes/Docker-OSX/master/Dockerfile.auto),
  whose prebuilt disk https://images.sick.codes/mac_hdd_ng_auto.img now 404s.
  Version-specific images are build-your-own
  (https://raw.githubusercontent.com/sickcodes/Docker-OSX/master/Dockerfile).
  Actively-maintained alternative if Docker-OSX rots further: `dockurr/macos`
  (https://github.com/dockur/macos — KVM required, web-viewer install, guest
  SSH undocumented; not a drop-in).
- **Artifact 29514910178**: "release (dry-run)" workflow_dispatch, created
  2026-07-16; `native-all` artifact expired=false, 35,640,717 B, expires
  2026-10-14
  (https://api.github.com/repos/wilvk/asm-test/actions/runs/29514910178/artifacts).
  End-to-end verified today: `gh run download 29514910178 --repo wilvk/asm-test
  -n native-all` succeeded and `file` confirms every
  `darwin-x86_64/*.dylib` is Mach-O 64-bit x86_64.

## Out of scope

- **Registering self-hosted runners / wiring Tracks C-D into CI** behind
  `workflow_dispatch` (`[self-hosted, macOS, arm64]` /
  `[self-hosted, linux, kvm]`) — the plan defers this until the lanes have run
  green *and* runners exist; owned by
  [self-hosted-ci-runners.md](self-hosted-ci-runners.md).
- **Porting the DynamoRIO tier to macOS** (the VM guests here only exercise the
  dlopen/install path, never the DBI tiers) —
  [macos-dynamorio-port.md](macos-dynamorio-port.md).
- **A macOS out-of-process stepper** (Mach APIs, unrelated to clean-room
  install testing) — [macos-oop-mach-stepper.md](macos-oop-mach-stepper.md).
- **Publishing/packaging changes** (what the bindings bundle, registry
  publishing) — [distribution-packaging.md](distribution-packaging.md); this
  doc only *consumes* staged packages.
- **The Track A/B/E harness itself** — done and landed; this doc treats
  `clean-env.sh` / `assert-clean-path.sh` / `clean-room-test.sh` as fixed
  substrate and never weakens them.
