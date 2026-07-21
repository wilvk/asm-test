# Intel SDE future/absent-ISA test lane — implementation

> **Sources.** Actioned from
> [intel-pin-capabilities-plan.md](../archive/plans/intel-pin-capabilities-plan.md)
> (track PIN-1) and
> [2026-07-17-intel-pin-vs-dynamorio.md](../analysis/2026-07-17-intel-pin-vs-dynamorio.md)
> (genuine delta #1). Written 2026-07-17. If this doc and a source disagree,
> this doc wins (sources may be stale); if the CODE and this doc disagree,
> re-verify before implementing.

## Why this work exists

Assembly that uses an ISA extension the host CPU lacks — APX's r16–r31,
AVX10.2, AMX, or AVX-512 on an AVX2-only box — is untestable by this framework
today: the DynamoRIO tier executes on real silicon, and the Unicorn tier
vendors QEMU 5.0.1, which predates AVX TCG (see
[live-attach-dataflow-followup-plan.md](../archive/plans/live-attach-dataflow-followup-plan.md),
the F1 "AVX-instruction replay is an UPSTREAM GATE" note). Intel's Software
Development Emulator (SDE) emulates those extensions for the *whole process*,
so running an **unmodified** suite binary under `sde64 -future` gives future-ISA
routines the full register/flag/memory/ABI assertion battery on any x86-64
host, including CI runners. This lane fetches a pinned SDE, proves it is
transparent to correct baseline code, and adds the APX fixtures that cannot
run any other way.

## What already exists (verified 2026-07-17)

There is **no SDE anywhere in the tree yet**: no `Dockerfile.sde`, no
`scripts/fetch-sde.sh`, no `sde` mention in the `Makefile` or `mk/*.mk`, no
`intel-sde` line in [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt),
and no `examples/apx_*` files. Everything below is the substrate you build on:

- [Dockerfile.drtrace](../../../Dockerfile.drtrace) — the fetched-and-pinned
  image pattern to mirror: `ARG DR_VERSION=11.91.20630` + `curl` the tarball +
  `ENV DYNAMORIO_HOME=/opt/dynamorio`, then `CMD make drtrace-test ...`.
- [scripts/fetch-dynamorio.sh](../../../scripts/fetch-dynamorio.sh) — the fetch
  script pattern: download → verify SHA-256 via `tp_digest`/`tp_sha256` from
  [scripts/lib-thirdparty.sh](../../../scripts/lib-thirdparty.sh) (refusing an
  unpinned download) → extract to a `build/` cache → capture the upstream
  license into [licenses/](../../../licenses/) → echo the home dir on stdout.
- [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt) —
  the trust manifest, format `<kind>  <name>  <version>  <algo>:<value>`;
  currently holds dynamorio, keystone, capstone, and two zig lines.
  [scripts/refresh-thirdparty-digests.sh](../../../scripts/refresh-thirdparty-digests.sh)
  rewrites this file but today emits **only** the dynamorio/keystone/capstone
  lines (the zig lines would be dropped by a refresh — a pre-existing gap; T1
  must at least add the new SDE line to the emitter so a refresh cannot lose it).
- [mk/docker.mk](../../../mk/docker.mk) — docker lane rules; `docker-drtrace`
  (lines 225–230) is the rule shape to copy (`$(DOCKER) build $(_docker_plat)
  -f Dockerfile.drtrace --build-arg BASE=$(DOCKER_BASE) --build-arg
  DR_VERSION=$(DR_VERSION) -t asmtest-drtrace .` then `run --rm`).
  `DOCKER_BASE ?= ubuntu:24.04`.
- [mk/native-trace.mk](../../../mk/native-trace.mk) — the self-skip shape for a
  lane target (lines 137–142): `drtrace-test` prints `== drtrace-test ==`,
  `# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=...`, `1..0 # skipped`.
- [mk/cli.mk](../../../mk/cli.mk) (lines 15–21) — the honest **architecture**
  gate: `CLI_ARCH := $(shell uname -m)` checked *before* any dependency check,
  because an architecture cannot be apt-installed.
- Suite auto-discovery ([Makefile](../../../Makefile)): every
  `examples/test_foo.c` + `examples/foo.s` pair links via the
  `$(BUILD)/test_%` pattern rule; `SUITE_EXCLUDES` (Makefile, ~line 62) lists
  suites kept out of `make test`. `make test` runs `$(SUITES)`; suites emit
  TAP (`TAP version 13`, `ok N - suite.name`, skips as
  `ok N - suite.name # SKIP reason` — [src/asmtest.c](../../../src/asmtest.c)
  line 1533), with **no timestamps**, so output is byte-diffable.
- CPUID capability gates: `asmtest_cpu_has_avx2()` / `asmtest_cpu_has_avx512f()`
  ([include/asmtest.h](../../../include/asmtest.h) lines 302–303, implemented
  with CPUID + XGETBV in [src/asmtest.c](../../../src/asmtest.c) lines
  465–494). The `ASM_VCALL256*`/`ASM_VCALL512*` macros SKIP when the gate is
  closed (asmtest.h lines 921–957). [examples/simd.s](../../../examples/simd.s)
  already contains `vec_add8d` (AVX-512 `vaddpd %zmm`), and
  [examples/test_simd.c](../../../examples/test_simd.c)'s
  `simd.avx512_adds_eight_doubles_512bit` self-skips on a host without
  AVX-512F — under SDE this skip must open (T5).
- The Unicorn emulator tier: `emu_call(E, (void *)routine, CODE_WINDOW, args,
  nargs, max_insns, &r)` runs the *host's compiled routine bytes* in an
  isolated guest CPU ([examples/test_emu.c](../../../examples/test_emu.c));
  link shape for an emu-using suite is the `$(BUILD)/test_emu` rule
  (Makefile ~line 818): `$(FRAMEWORK_OBJS)` + routine objects + `emu.o
  trace.o disasm.o fuzz.o` + the test object, linked with `$(UNICORN_LIBS)
  $(CAPSTONE_LIBS)`.
- The shared trace sink: `asmtest_trace_t`
  ([include/asmtest_trace.h](../../../include/asmtest_trace.h) lines 44–60:
  `insns/insns_total`, `blocks/blocks_total`, `truncated`) and
  `asmtest_trace_report()` ([src/trace.c](../../../src/trace.c) line 193).
- CI: [.github/workflows/ci.yml](../../../.github/workflows/ci.yml) has the
  `drtrace` job (lines 336–359) as the lane-job pattern: ubuntu-latest, fetch
  the pinned engine, run the make target.

**Prove the baseline is green** before touching anything (host or container):

```sh
make test          # every suite prints TAP and "N passed, 0 failed"
make check         # framework self-tests via tests/expect.sh
make docker-test   # same, inside the ubuntu:24.04 CI container
```

On this repo's macOS-arm64 dev host `make test` runs the AArch64 bodies; the
SDE lane itself can only run on x86-64 (see Constraints & gates).

## Tasks

### T1 — Pin Intel SDE 10.8.0: digest line, fetch script, vendored license  (S, depends on: none)

**Goal.** `scripts/fetch-sde.sh` downloads SDE 10.8.0, refuses it unless its
SHA-256 matches a new manifest line, vendors the license, and echoes `SDE_HOME`.

**Steps.**
1. Append to [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt)
   (mirror the existing `dynamorio` line's spacing):
   ```
   tarball-sha256  intel-sde  10.8.0  sha256:50b320cd226acef7a491f5b321fc1be3c3c7984f9e27a456e64894b5b0979dd3
   ```
   That digest was computed from the live download and cross-checks against the
   AUR `intel-sde` PKGBUILD (see Research notes).
2. Create `scripts/fetch-sde.sh` mirroring
   [scripts/fetch-dynamorio.sh](../../../scripts/fetch-dynamorio.sh)
   line-for-line in structure. Differences only:
   - `SDE_VERSION="${SDE_VERSION:-10.8.0}"`,
     `SDE_URL="${SDE_URL:-https://downloadmirror.intel.com/915934/sde-external-${SDE_VERSION}-2026-03-15-lin.tar.xz}"`
     (the `915934` mirror id and the date are release-specific — a version bump
     must override `SDE_URL` whole; say so in the header comment).
   - cache `SDE_CACHE="${SDE_CACHE:-$root/build/sde}"`,
     `home="$SDE_CACHE/sde-external-$SDE_VERSION-2026-03-15-lin"` (the
     tarball's own top-level dir name); extract with `tar -xJf` (xz).
   - digest check: `tp_digest tarball-sha256 intel-sde "$SDE_VERSION"` — keep
     the exact "refusing to use an unpinned download" failure text.
   - final sanity: `[ -x "$home/sde64" ]` (instead of the libdynamorio check);
     echo `$home`.
   - license capture: SDE's license is a PDF plus companions, not a single
     `.txt`, so vendor a **directory** `licenses/intel-sde-$SDE_VERSION/`
     containing `Licenses/LICENSE.pdf`, `Licenses/third-party-programs.txt`,
     and the `Licenses/pin licensing/` subdirectory, copied verbatim from the
     kit on first fetch (the Intel Simplified Software License permits
     redistribution **without modification** with the notice reproduced —
     verbatim copy is mandatory).
3. `chmod +x scripts/fetch-sde.sh`.
4. Extend [scripts/refresh-thirdparty-digests.sh](../../../scripts/refresh-thirdparty-digests.sh):
   read `SDE_VERSION` from `fetch-sde.sh` with the existing `read_ver` helper,
   re-download the tarball to a temp file, `tp_sha256` it, and emit the
   `intel-sde` line in the manifest-writing block — otherwise the next refresh
   silently drops the pin (that block currently emits only
   dynamorio/keystone/capstone; note in your commit message that the zig lines
   have the same latent problem, which this doc does not fix).
5. Add a row to [licenses/README.md](../../../licenses/README.md)'s table,
   whose columns are `| File | Component | Version | SPDX |`. Two columns need
   care because SDE does not look like the existing rows: (a) **File** — every
   existing row names a single backtick-quoted license *filename*, but SDE
   vendors a **directory** (T1 step 2), so put `intel-sde-10.8.0/` here; (b)
   **SPDX** — the Intel Simplified Software License (Feb 2020) is **not on the
   SPDX license list**, so it has no standard identifier; write
   `LicenseRef-IntelSimplified` (or `proprietary (no SPDX id)`), *not* a prose
   license name — a prose string in the SPDX column would break the pattern
   every other row follows. **Component** = "Intel SDE (test-lane oracle)";
   **Version** = 10.8.0. Add an explicit note that SDE is a **test-lane-only**
   dependency, never bundled into packages, so
   [scripts/collect-licenses.sh](../../../scripts/collect-licenses.sh) does
   **not** pick it up (that script assembles notices only for *shipped*
   payloads — see Constraints & gates for how this differs from DynamoRIO).

**Code.** Shell only, as above. No C changes.

**Tests.** On any Linux x86-64 box or in a container with `curl` + `xz-utils`:
`SDE_HOME=$(scripts/fetch-sde.sh)` prints progress on stderr, the home on
stdout, and `"$SDE_HOME/sde64" -help | head -1` prints
`Intel(R) Software Development Emulator.  Version: 10.8.0 ...`. Corrupt the
manifest digest (flip one hex char) and re-run with a cold cache: the script
must fail with the integrity-check error, exit nonzero, and leave no cached
kit. There is no make-target test surface yet (T3 wires it); this manual
check is the verification.

**Docs.** Internal-only at this step (licenses/README.md row is part of the
step itself). Changelog comes with T8.

**Done when.**
- `scripts/fetch-sde.sh` fetches, verifies, extracts, and echoes a dir
  containing an executable `sde64`.
- A wrong digest fails loudly; a missing manifest line fails loudly.
- `licenses/intel-sde-10.8.0/` exists after first fetch with the three items.
- `sh scripts/refresh-thirdparty-digests.sh` (network) rewrites the manifest
  and the `intel-sde` line survives.

### T2 — Dockerfile.sde: SDE + APX-capable pinned assemblers  (M, depends on: T1)

**Goal.** An x86-64 image containing digest-verified SDE 10.8.0, GAS from
pinned binutils 2.46.1, NASM 3.02, and libunicorn, that runs `make sde-test`.

**Steps.**
1. Create `Dockerfile.sde` mirroring
   [Dockerfile.drtrace](../../../Dockerfile.drtrace)'s shape (`ARG
   BASE=ubuntu:24.04`, apt block, pinned fetch, `WORKDIR /src`, `COPY . .`,
   `CMD`). Apt installs: `build-essential curl ca-certificates xz-utils
   pkg-config libunicorn-dev` (libunicorn for T6's cross-check; **no** nasm
   from apt — Ubuntu 24.04's nasm 2.16.01 cannot encode APX).
2. SDE block — verify the digest **inside** the image build (an improvement on
   Dockerfile.drtrace, and what the plan's "SHA-256 gate" requires). Copy the
   trust anchors first so the check reuses the tested helpers:
   ```dockerfile
   ARG SDE_VERSION=10.8.0
   ARG SDE_URL=https://downloadmirror.intel.com/915934/sde-external-10.8.0-2026-03-15-lin.tar.xz
   COPY scripts/lib-thirdparty.sh scripts/third-party-digests.txt /tmp/tp/
   RUN curl -fsSL "$SDE_URL" -o /tmp/sde.tar.xz \
    && sh -c 'TP_MANIFEST=/tmp/tp/third-party-digests.txt; . /tmp/tp/lib-thirdparty.sh; \
              want=$(tp_digest tarball-sha256 intel-sde '"$SDE_VERSION"') || exit 1; \
              got="sha256:$(tp_sha256 /tmp/sde.tar.xz)"; \
              [ "$got" = "$want" ] || { echo "SDE digest mismatch: $got != $want" >&2; exit 1; }' \
    && mkdir -p /opt/sde \
    && tar -xJf /tmp/sde.tar.xz -C /opt/sde --strip-components=1 \
    && rm /tmp/sde.tar.xz
   ENV SDE_HOME=/opt/sde
   ```
   (Note `lib-thirdparty.sh` resolves the manifest from `$0`'s dir, which is
   wrong under `sh -c` — the explicit `TP_MANIFEST` override above is
   required, and is an existing documented knob of that lib.)
3. Binutils block: fetch
   `https://ftp.gnu.org/gnu/binutils/binutils-2.46.1.tar.xz`, digest-gate it
   against a new `tarball-sha256  binutils  2.46.1  sha256:<computed>` manifest
   line (compute on first download, record in
   `scripts/third-party-digests.txt` and the refresh script, same motion as
   T1), then `./configure --prefix=/opt/binutils --disable-werror
   --disable-gdb && make && make install`. Export both lookup paths:
   ```dockerfile
   ENV PATH=/opt/binutils/bin:$PATH COMPILER_PATH=/opt/binutils/bin
   ```
   (`COMPILER_PATH` makes gcc's `-x assembler-with-cpp` driver find the new
   `as` ahead of `/usr/bin/as`; `PATH` covers direct invocation.) Then verify
   in-image: `as --version | grep -F 2.46.1` and assemble an APX probe:
   `printf 'movq %%r16, %%rax\n\taddq %%rsi, %%rdi, %%rax\n' | as -o /dev/null -`
   (an EGPR move = binutils ≥ 2.42, an NDD add = ≥ 2.43; both pass on 2.46.1).
4. NASM block: fetch
   `https://www.nasm.us/pub/nasm/releasebuilds/3.02/nasm-3.02.tar.xz`,
   digest-gate with a new `tarball-sha256  nasm  3.02  sha256:<computed>`
   line, `./configure --prefix=/opt/nasm && make && make install`, prepend
   `/opt/nasm/bin` to `PATH`, verify `nasm -v | grep -F 3.02`.
5. `CMD make sde-test SDE_HOME=$SDE_HOME` (target arrives in T3; until then
   build the image with `docker build -f Dockerfile.sde .` and smoke it with
   `docker run --rm <img> $SDE_HOME/sde64 -help`).
6. Add the `docker-sde` rule + `SDE_VERSION ?= 10.8.0` to
   [mk/docker.mk](../../../mk/docker.mk) directly below the `docker-drtrace`
   rule, copying its exact shape (build with `-f Dockerfile.sde`, tag
   `asmtest-sde`, then `run --rm`). Add `docker-sde` to the `Docker (Linux CI
   lanes)` section of `make help` in [Makefile](../../../Makefile) (~line 163,
   next to `docker-drtrace`).

**Code.** Dockerfile + two manifest lines + one make rule, as above.

**Tests.** `make docker-sde` on an x86-64 Linux host builds the image and (post
T3) runs the lane. Until T3 lands, the image-build itself is the test: the
digest gates and the two assembler probes are `RUN` steps, so a wrong digest
or an APX-incapable assembler **fails the build**. Failure looks like a
nonzero `docker build`; success ends with the `sde64 -help` banner.

**Docs.** Internal-only (the Dockerfile's header comment, mirroring
Dockerfile.drtrace's). User docs in T8.

**Done when.**
- `docker build -f Dockerfile.sde .` succeeds on x86-64 and fails loudly if
  any of the three digests is wrong.
- In-container: `as --version` reports 2.46.1, `nasm -v` reports 3.02,
  `$SDE_HOME/sde64 -help` prints the 10.8.0 banner.
- `scripts/third-party-digests.txt` gained `binutils` and `nasm` lines and the
  refresh script emits them.

### T3 — `sde-test` lane target + transparency null test  (M, depends on: T1, T2)

**Goal.** `make sde-test` runs existing suites under `sde64 -future` and proves
SDE is byte-for-byte transparent to correct baseline code; it self-skips
honestly off x86-64 and when SDE is absent.

**Steps.**
1. Create `mk/sde.mk` and add `include mk/sde.mk` in
   [Makefile](../../../Makefile) after the existing include block (line 864,
   `include mk/docs.mk`). Follow the mk-file header comment convention
   ("Included by ../Makefile ... edit targets here, knobs there").
2. Gate order per [mk/cli.mk](../../../mk/cli.mk): architecture first, then
   the installable dependency:
   ```make
   SDE_ARCH := $(shell uname -m)
   SDE_HOME ?=
   SDE64    := $(SDE_HOME)/sde64
   ifneq ($(wildcard $(SDE64)),)
   SDE_AVAILABLE := 1
   endif
   SDE_CHIP ?= -future
   ```
3. Write `sde-test` with the [mk/native-trace.mk](../../../mk/native-trace.mk)
   `drtrace-test` conditional shape (`ifeq`/`ifndef` at parse level; `== sde-test ==`,
   `# SKIP: ...`, `1..0 # skipped`). Skip texts: non-x86 → `# SKIP: Intel SDE
   is x86-only; host is $(SDE_ARCH)` (a REAL gate); SDE absent → `# SKIP:
   Intel SDE not found. Set SDE_HOME=$$(scripts/fetch-sde.sh)` (points at the
   fetch, per the CLAUDE.md missing-dependency rule the lane exists so this
   skip only ever fires on a host that chose not to fetch).
4. The live branch — the transparency **null test** on suites with no
   CPUID-dependent skips (`test_arith`, `test_capture`, `test_mem`; all in
   `$(SUITES)` today):
   ```make
   sde-null-test: $(BUILD)/test_arith $(BUILD)/test_capture $(BUILD)/test_mem
   	@set -e; for t in test_arith test_capture test_mem; do \
   	  echo "== sde-null: $$t =="; \
   	  $(BUILD)/$$t                        > $(BUILD)/sde-null-$$t.native.tap; \
   	  $(SDE64) $(SDE_CHIP) -- $(BUILD)/$$t > $(BUILD)/sde-null-$$t.sde.tap; \
   	  diff -u $(BUILD)/sde-null-$$t.native.tap $(BUILD)/sde-null-$$t.sde.tap; \
   	done; echo "sde-null-test: native and sde64 $(SDE_CHIP) TAP identical"
   ```
   (TAP output carries no timings — verified by running `build/test_arith` —
   so `diff` is exact. `test_simd` is deliberately NOT here: its capability
   skips *should* differ under SDE; that difference is T5's assertion.)
5. Chain sub-lanes from `sde-test` with `@$(MAKE) --no-print-directory`, the
   drtrace-test idiom, so T4–T7 each add one line here.
6. Add `sde-test` to `make help` under `Native runtime trace tiers` (~line
   115): `sde-test    run suites under Intel SDE -future (future/absent ISA; set SDE_HOME)`.

**Code.** `mk/sde.mk` as above; one `include` line and two help lines in
`Makefile`.

**Tests.** The target is the test. Pass: three `diff`s silent, final
"TAP identical" line, exit 0. Failure: a unified diff of the two TAP streams
(that diff IS the transparency violation report). Self-skip check on this
repo's arm64 dev host: `make sde-test` prints the x86-only SKIP and exits 0.
On x86-64 without fetching: prints the fetch-pointer SKIP, exits 0. Full
in-container run: `make docker-sde`.

**Docs.** T8.

**Done when.**
- `make docker-sde` (x86-64 host) ends with `sde-null-test: native and sde64
  -future TAP identical`, exit 0.
- `make sde-test` on macOS-arm64 → x86-only SKIP, exit 0.
- `make sde-test SDE_HOME=/nonexistent` on x86-64 → not-found SKIP naming
  `scripts/fetch-sde.sh`, exit 0.

### T4 — APX (r16–r31 / REX2) example suite  (M, depends on: T2, T3)

**Goal.** An `examples/apx_basic.s` routine set using APX EGPRs and NDD, with
`examples/test_apx_basic.c` assertions that SKIP off-APX and pass under
`sde64 -future`.

**Steps.**
1. Add an APX CPUID gate next to the existing pair: declare
   `int asmtest_cpu_has_apx(void);` in
   [include/asmtest.h](../../../include/asmtest.h) beside
   `asmtest_cpu_has_avx512f` (line 303) and implement it in
   [src/asmtest.c](../../../src/asmtest.c) beside the AVX-512 gate (~line
   483): CPUID leaf 7 subleaf 1, `EDX` bit 21 (APX_F), using the same
   `__get_cpuid_count` idiom; return 0 in the non-x86 `#else` arm.
   `include/asmtest.h` is **not** a bindings-parity tier header
   ([scripts/check-bindings-parity.sh](../../../scripts/check-bindings-parity.sh)
   `TIER_HEADERS` lists only hwtrace/drtrace/trace_auto/ptrace/codeimage), so
   no binding work follows.
2. Create `examples/apx_basic.s` mirroring
   [examples/add.s](../../../examples/add.s)'s shape (`#include "asm.h"`,
   `ASM_FUNC name` … `ASM_ENDFUNC name`), x86-64 body only:
   ```asm
   /* long apx_sum4(long a, long b, long c, long d);  EGPR/REX2 exercise */
   ASM_FUNC apx_sum4
   #if defined(__x86_64__)
       movq    %rdi, %r16          /* REX2-encoded: EGPR destinations   */
       movq    %rsi, %r17
       movq    %rdx, %r18
       movq    %rcx, %r19
       addq    %r17, %r16
       addq    %r19, %r18
       leaq    (%r16,%r18), %rax   /* EGPR base+index through REX2 lea  */
       ret
   #endif
   ASM_ENDFUNC apx_sum4

   /* long apx_ndd_add(long a, long b);  new-data-destination form */
   ASM_FUNC apx_ndd_add
   #if defined(__x86_64__)
       addq    %rsi, %rdi, %rax    /* APX NDD: rax = rdi + rsi, needs GAS >= 2.43 */
       ret
   #endif
   ASM_ENDFUNC apx_ndd_add
   ```
   (AT&T NDD syntax is destination-last as shown; if the pinned GAS 2.46.1
   rejects a line, check `gas/testsuite/gas/i386/x86-64-apx-*.s` in the
   binutils tree for the canonical spelling and adjust — iterate inside the
   container, never against the host assembler.)
3. Create `examples/test_apx_basic.c` following
   [examples/test_capture.c](../../../examples/test_capture.c)'s macro shapes,
   with a suite-local gate modeled on `test_emu.c`'s `REQUIRE_X86_HOST`:
   every TEST begins `if (!asmtest_cpu_has_apx()) SKIP("APX not available on
   this host (run under sde64 -future)");`. Cases: `ASSERT_EQ(apx_sum4(1,2,3,4), 10)`;
   `ASM_CALL2(&r, apx_ndd_add, 20, 22)` then `ASSERT_EQ(r.ret, 42)` and
   `ASSERT_ABI_PRESERVED(&r)` (EGPRs are extra scratch state; the classic
   callee-saved set must still be intact); a flags case asserting
   `ASSERT_FLAG_SET(&r, CF)` after an EGPR add that carries.
4. Add `test_apx_basic` to `SUITE_EXCLUDES` in [Makefile](../../../Makefile)
   (~line 62) — the host assembler may predate APX, so this suite must never
   enter `make test`. The generic `$(BUILD)/test_%` pattern rule already links
   the pair; no link rule needed.
5. Wire into `mk/sde.mk` with an assembler-capability probe so a host-side
   `sde-test` without the pinned GAS degrades honestly:
   ```make
   SDE_GAS_APX := $(shell printf 'movq %%r16, %%rax\naddq %%rsi, %%rdi, %%rax\n' | \
                    $(CC) -x assembler -c -o /dev/null - 2>/dev/null && echo 1)
   ```
   In the `sde-apx-test` sub-target: if the probe failed, print `# SKIP:
   assembler cannot encode APX (need binutils >= 2.43; make docker-sde runs
   the pinned 2.46.1)`; else `$(MAKE) $(BUILD)/test_apx_basic` and run
   **only** under SDE: `$(SDE64) $(SDE_CHIP) -- $(BUILD)/test_apx_basic`,
   then grep the TAP for `# SKIP` on the apx lines — under `-future` none may
   remain (CPUID inside SDE reports APX_F, opening the gate in step 1).
6. Optional stretch, same task: `examples/apx_basic.asm` (NASM syntax, `r16`…
   registers, NASM ≥ 3.00) so `make ASM_SYNTAX=nasm sde-test` also covers the
   Intel-syntax backend inside the image (NASM 3.02 is pinned there). Do not
   block the task on it; land `.s` first.

**Code.** As above; ~60 lines of asm + ~50 of C + ~15 of make + the 15-line
CPUID gate.

**Tests.** `make docker-sde` output must show the `sde-apx-test` section with
every `apx_basic.*` case `ok` and none `# SKIP`. Negative control (manual, in
the container): run `$(BUILD)/test_apx_basic` **without** SDE — every case
prints `ok ... # SKIP APX not available...` (CPUID gate closed on real
pre-APX silicon), proving the fixture cannot silently rot into a vacuous
pass. On the arm64 host the suite never builds (excluded + lane skips).

**Docs.** T8 documents the suite as the lane's reason-to-exist example.

**Done when.**
- In-container: `sde64 -future -- build/test_apx_basic` → all cases `ok`, no
  skips, exit 0.
- Same binary bare in-container → all cases `# SKIP`, exit 0.
- `make test` (host and container) neither builds nor runs `test_apx_basic`.

### T5 — Assert the AVX-512-on-AVX2 un-skip under SDE  (S, depends on: T3)

**Goal.** The lane proves the *existing* AVX-512 test in `test_simd` runs (not
skips) under `sde64 -future`, converting a documented capability self-skip
into a real execution.

**Steps.**
1. Add an `sde-avx512-test` sub-target to `mk/sde.mk`, chained from
   `sde-test`:
   ```make
   sde-avx512-test: $(BUILD)/test_simd
   	@echo "== sde-avx512-test =="
   	@$(SDE64) $(SDE_CHIP) -- $(BUILD)/test_simd > $(BUILD)/sde-simd.tap; \
   	grep -E '^ok [0-9]+ - simd\.avx512_adds_eight_doubles_512bit$$' \
   	    $(BUILD)/sde-simd.tap || { \
   	  echo "FAIL: avx512 case skipped or failed under SDE"; \
   	  grep 'avx512' $(BUILD)/sde-simd.tap; exit 1; }
   ```
   The anchored regex rejects the `... # SKIP AVX-512F not available` form —
   the line must be a bare `ok`. Do the same for
   `simd.avx2_adds_four_doubles_256bit` (guards the AVX2-on-SSE-host case).
2. No fixture changes: `vec_add8d` in [examples/simd.s](../../../examples/simd.s)
   and the `ASM_VCALL512_2` gate in [include/asmtest.h](../../../include/asmtest.h)
   already express the test; SDE's emulated CPUID + XGETBV satisfy
   `asmtest_cpu_has_avx512f()`'s XCR0 `0xe6` check under `-future`. If it ever
   does not (a chip-definition regression in a future SDE), fall back to
   `SDE_CHIP=-skx` for this sub-target and record why — do not delete the
   assertion.

**Code.** ~10 lines of make.

**Tests.** The target is the test. Pass: the two grep'd `ok` lines echo and
the target exits 0. Failure mode A (skipped): the FAIL message plus the
`# SKIP` line it grepped. Failure mode B (wrong result under emulation): the
`not ok` line — a genuine SDE-vs-hardware divergence worth a bug report.

**Docs.** T8.

**Done when.**
- `make docker-sde` shows `sde-avx512-test` passing on the CI/dev x86 host
  regardless of that host's real AVX support.
- Manual cross-check on any AVX2-only host: bare `build/test_simd` skips the
  avx512 case; under `sde64 -future` it passes — the pair demonstrates the
  lane's entire value in one command.

### T6 — Unicorn cross-check on overlapping ISA  (M, depends on: T3)

**Goal.** Anchor SDE against an existing in-tree oracle: for baseline ISA both
model (scalar + SSE), the same routine executed natively-under-SDE and under
the Unicorn tier must agree, in one binary.

**Steps.**
1. Create `examples/test_sde_crosscheck.c` modeled on
   [examples/test_emu.c](../../../examples/test_emu.c) (SETUP/TEARDOWN with
   `emu_open`/`emu_close`, `REQUIRE_X86_HOST()`-style gate, `CODE_WINDOW 64`):
   - `crosscheck.gp_add`: `long native = add_signed(20, 22);` then
     `emu_call(E, (void *)add_signed, 64, args, 2, 0, &r)`;
     `ASSERT_EQ(native, (long)r.regs.rax);`
   - `crosscheck.gp_flags`: `ASM_CALL2(&cap, sum_via_rbx, 1, 2)` vs the
     emulated run's `r.regs` — compare result and CF/ZF.
   - `crosscheck.sse_vec`: `ASM_VCALL2(&cap, vec_add4f, a, b)` vs
     `emu_call_vec` on `(void *)vec_add4f` (follow `test_emu.c`'s
     `emu_call_vec` argument shape) — compare all four f32 lanes.
   Keep to SSE and below: Unicorn's QEMU 5.0.1 has no AVX TCG (the ceiling
   this lane routes around), so AVX belongs to T5's one-sided assertion, not
   here.
2. Link rule in `mk/sde.mk`, copying the `$(BUILD)/test_emu` rule's object
   list from [Makefile](../../../Makefile) (~line 818) with `add.o flags.o
   simd.o` as the routine objects and `$(UNICORN_LIBS) $(CAPSTONE_LIBS)` at
   link. Add `test_sde_crosscheck` to `SUITE_EXCLUDES`.
3. Sub-target `sde-crosscheck-test`, chained from `sde-test`, gated on
   `pkg-config --exists unicorn` (mirror the optional-tier detection comment
   in Makefile ~line 777): absent → `# SKIP: libunicorn not found (the
   docker-sde image installs it)`; present → run the binary **twice**: bare
   (native vs Unicorn on real silicon — this is a valid check on any x86 box)
   and under `$(SDE64) $(SDE_CHIP) --` (SDE vs Unicorn-inside-SDE — the
   lane's anchor). Both runs must be all-`ok`.

**Code.** ~80 lines C, ~15 make.

**Tests.** The suite is the test. A pass prints two all-`ok` TAP runs. A
failure names the diverging register/lane via the normal `ASSERT_EQ`
diagnostics — triage: bare-run failure = emulator-tier bug (pre-existing);
SDE-only failure = SDE emulation divergence; file upstream and pin the case
with a comment, never delete it.

**Docs.** T8.

**Done when.**
- `make docker-sde` shows `sde-crosscheck-test` with both runs green.
- On a host without libunicorn the sub-target prints its named SKIP, exit 0,
  while the rest of `sde-test` still runs.

### T7 — Optional `-mix` instruction-mix report in `asmtest_trace_t` shape  (M, depends on: T3, T4)

**Goal.** `make sde-mix` histograms a future-ISA suite's dynamic instructions
via SDE's emulator-aware mix tool and folds the totals into the repo's
canonical trace-report shape.

**Steps.**
1. Sub-target in `mk/sde.mk` (NOT chained into `sde-test` — it is a report,
   not a gate):
   ```make
   sde-mix: $(BUILD)/test_apx_basic $(BUILD)/sde_mix_report
   	$(SDE64) $(SDE_CHIP) -mix -omix $(BUILD)/sde-mix.txt -- $(BUILD)/test_apx_basic
   	$(BUILD)/sde_mix_report $(BUILD)/sde-mix.txt
   ```
   Always pass `-omix` explicitly: the kit's doc and the driver's short help
   disagree on the default filename (`sde-mix-out.txt` vs `mix.out` — see
   Research notes), so never rely on it.
2. Create `tools/sde_mix_report.c` following the
   [mk/bench.mk](../../../mk/bench.mk) tools pattern (`$(BUILD)/emu-bench`
   compiles a `tools/*.c` against framework objects). Behavior: parse the mix
   file's **global** summary section, take the `*total` dynamic-instruction
   count, fill `asmtest_trace_t t = {0}; t.insns_total = total;` and print via
   `asmtest_trace_report(&t, stdout)`
   ([src/trace.c](../../../src/trace.c):193) — the shared-sink shape every
   backend reports in — then echo the `*isa-set-*` breakdown lines verbatim
   below it (the per-extension histogram is the part nothing else in the tree
   can produce). First implementation step is **empirical**: run the `sde64
   -mix` command in the container and read the actual file before writing the
   parser; the kit's `doc/mix.html` documents the format, and `-mix-format
   json` exists if the text layout proves awkward — parsing the JSON instead
   is an acceptable design change, note it in the tool header.
3. Link rule: `$(BUILD)/sde_mix_report: $(BUILD)/sde_mix_report.o
   $(BUILD)/trace.o` (trace.o is dependency-free — Makefile ~line 793).
4. Honesty limit, stated in the tool's header comment: `-mix` counts are
   process-wide dynamic totals, not region-scoped offsets — `insns`/`blocks`
   offset arrays stay empty and `blocks_total` stays 0. Scoping mix to a
   marker region is future work for the Pin tracing tier, not this lane.

**Code.** ~120 lines C, ~8 make.

**Tests.** In-container: `make sde-mix SDE_HOME=/opt/sde` prints an
`asmtest_trace_report` block whose `insns_total` is > 0 and an ISA-set section
that includes an `APX`-family line (the APX suite executed EGPR instructions
under emulation — visible proof). A malformed/missing mix file must exit
nonzero with a message naming the file. No self-skip surface beyond
`sde-test`'s own gates (reuse them by making `sde-mix` require
`SDE_AVAILABLE`).

**Docs.** T8 mentions the target in the guide's "reports" paragraph.

**Done when.**
- `make docker-sde` followed by an interactive `make sde-mix` in the container
  prints the trace-shaped report with a nonzero total and an APX ISA-set line.
- The tool builds warning-clean under `make WERROR=1` and is `make fmt` clean.

### T8 — CI job, user-facing docs, changelog  (S, depends on: T3, T4, T5, T6)

**Goal.** The lane runs on every push, and users can discover it.

**Steps.**
1. Add an `sde` job to
   [.github/workflows/ci.yml](../../../.github/workflows/ci.yml) after the
   `drtrace` job (line 336), modeled on it but container-first (the pinned
   assemblers live in the image, so the runner needs only Docker):
   `runs-on: ubuntu-latest`, `timeout-minutes: 30`, steps: checkout,
   `make docker-sde`. ubuntu-latest runners are x86-64, so the lane executes
   rather than skipping. (No macOS leg: SDE ships Linux/Windows kits only; if
   one is ever added it must use `macos-15-intel`, never `macos-13`.)
2. User-facing guide: create `docs/guides/tracing/sde-testing.md` ("Testing
   future-ISA assembly with Intel SDE") and add `sde-testing` to the hidden
   toctree in [docs/guides/tracing/index.md](../../../docs/guides/tracing/index.md)
   plus a row in its backend table (Page: SDE future-ISA lane; Backend: Intel
   SDE emulation under `sde64 -future`; Records: full register/flag/memory/ABI
   assertions on emulated ISA; Runs where: any x86-64 Linux). Content: what
   the lane is, `make docker-sde` / `make sde-test SDE_HOME=$(scripts/fetch-sde.sh)`,
   the transparency guarantee, the APX example, the `-mix` report, and the
   honest boundary — SDE is an *emulator*: timing, and the real ABI at silicon
   level, are asserted only up to emulation fidelity, which is why T6 anchors
   it to Unicorn and T3 to native TAP identity. Link this implementation doc
   via a GitHub blob URL only (published pages must not relative-link into
   `docs/internal/`). Build gate: `make docker-docs` (Sphinx `-W`, so a broken
   toctree fails).
3. Changelog: one `Added` bullet under `## [Unreleased]` in
   [CHANGELOG.md](../../../CHANGELOG.md) describing the lane (pinned SDE
   10.8.0, `docker-sde`/`sde-test`, transparency null test, APX suite,
   AVX-512-on-AVX2 un-skip, Unicorn cross-check, `sde-mix`).
4. `make help` entries were added in T2/T3; verify both appear.

**Code.** YAML + markdown only.

**Tests.** CI job green on a real push (per this repo's verify-before-done
memory rule: watch the actual run, not just local `act`-style approximations).
`make docker-docs` exits 0. `make check` still green (no framework change in
this task).

**Docs.** This task IS the docs task.

**Done when.**
- The `sde` job passes on GitHub Actions for the branch push.
- The new guide renders in `make docker-docs` output with no `-W` warnings.
- `CHANGELOG.md` has the entry; `make help` lists `sde-test` and `docker-sde`.

## Task order & parallelism

```
T1 ──> T2 ──> T3 ──> T5        (T5, T6 independent of each other)
              T3 ──> T6
       T2 ──────────> T4 ──> T7
T3..T6 ──> T8
```

- **Critical path:** T1 → T2 → T3 → T4 → T8 (the APX suite is the lane's
  reason to exist; T8 needs everything it documents).
- **Parallelizable:** after T3 lands, T4 (person A: fixtures + CPUID gate) and
  T5+T6 (person B: make-level assertions + cross-check suite) touch disjoint
  files. T7 is optional and can trail everything except T4.
- T1 and the Dockerfile skeleton of T2 can be written concurrently, but T2's
  digest-gate step needs T1's manifest line to build.

## Constraints & gates

- **License.** SDE is proprietary freeware under the Intel Simplified Software
  License (Feb 2020): redistribution allowed **without modification** with the
  notice reproduced; no reverse engineering; bundled Pin/XED under
  `Licenses/third-party-programs.txt` and `Licenses/pin licensing/`. Vendor
  all three verbatim (T1). SDE is **test/oracle-only**: never linked into
  `libasmtest`, `libasmtest_emu`, or any binding package, and never linked
  into any shipped artifact — so, **unlike DynamoRIO**, it gets **no entry at
  all** in [scripts/collect-licenses.sh](../../../scripts/collect-licenses.sh).
  (DynamoRIO is *not* "never-ship": it CAN be a native-trace-tier payload,
  which is why that script carries a guarded `present 'libdynamorio*' &&
  emit_lit DynamoRIO ...` line that emits its notice only when the lib is
  actually staged into the package slot. SDE never reaches a slot, so no such
  line is ever added — do not mirror the DynamoRIO line for SDE.) This is
  exactly the posture the plan's shared-constraint 1 sets.
- **Pinning.** Every fetched artifact (SDE, binutils, NASM) carries a
  `tarball-sha256` line in `scripts/third-party-digests.txt`, is verified at
  fetch/build time, and fails loudly on mismatch. A version bump that forgets
  the manifest cannot ship unpinned (the fetch script refuses).
- **Real gate: non-x86 hosts.** SDE is x86-only. `sde-test` self-skips with a
  printed reason on `uname -m != x86_64` (this covers macOS-arm64 and aarch64
  Linux, including an arm64 host's `docker-sde`, where `_docker_plat` follows
  the host and the in-container make sees aarch64). Running the x86 image via
  `DOCKER_PLATFORM=linux/amd64` qemu-user emulation is **unsupported** — SDE
  is itself a DBI engine and stacking it on qemu-user is neither tested nor
  claimed; record results-if-tried in the lane doc, do not gate on them.
- **No silicon gate.** That is the lane's point: it needs no APX/AVX-512
  hardware, ever. Do not add a CPU-feature self-skip to any `sde-*` target.
- **This dev host cannot validate the lane end-to-end** (macOS-arm64): the
  observable acceptance here is the two self-skip paths plus a green
  `make docker-sde` on an x86-64 machine or the T8 CI job — treat the CI run
  as the real end-to-end check and record its URL in the landing commit.

## Research notes (verified 2026-07-17)

- **SDE version/kit.** Current release 10.8.0 (2026-03-15), confirmed from the
  kit's own `doc/index.html` and the `sde64` binary version string. Download
  page (license-click-gated, 403s automated fetches):
  <https://www.intel.com/content/www/us/en/download/684897/intel-software-development-emulator.html>.
  Direct tarball (fetches 200 without cookies, verified live):
  <https://downloadmirror.intel.com/915934/sde-external-10.8.0-2026-03-15-lin.tar.xz>
  (33,413,448 bytes). SHA-256 computed from the download:
  `50b320cd226acef7a491f5b321fc1be3c3c7984f9e27a456e64894b5b0979dd3`, matching
  the AUR `intel-sde` PKGBUILD
  (<https://aur.archlinux.org/cgit/aur.git/plain/PKGBUILD?h=intel-sde>).
  Release notes page (403s to tooling; secondary evidence only):
  <https://www.intel.com/content/www/us/en/developer/articles/release-notes/intel-software-development-emulator-release-notes.html>.
- **License text.** Intel Simplified Software License (Version February 2020),
  from `Licenses/LICENSE.pdf` inside the kit; companions
  `Licenses/third-party-programs.txt` and `Licenses/pin licensing/`.
- **`-future` semantics.** A chip/CPUID selector: maps to XED chip `FUTURE`
  ("Future chip"), sets both the CPUID the guest sees and the chip-check that
  errors (default: die) on any instruction illegal for that chip (kit
  `doc/chip_check.html`). Kit `misc/cdata.txt`: `FUTURE` = 252 ISA sets, a
  strict superset of `DIAMOND_RAPIDS` (`-dmr`, 244); both include `APX_F*` and
  `AVX10_2*` sets. Other 10.8.0 chip flags: `-srf -gnr -arl -lnl -ptl -nvl
  -cwf -dmr`.
- **`-mix` semantics.** "Compute mix histogram analysis tool": per-thread +
  global dynamic-instruction histograms (top blocks, opcode/iform, function
  tables, ISA-set/category groups); `-omix` names the output and implies
  `-mix`; `-mix-format json` available. The kit doc says default output
  `sde-mix-out.txt` while the driver's short help says `mix.out` — always pass
  `-omix` (kit `doc/mix.html`).
- **GAS/binutils minimums** (from `gas/NEWS` in the official
  <https://ftp.gnu.org/gnu/binutils/binutils-2.46.1.tar.xz> tarball): 2.42
  (2024-01-29) initial APX — 32 GPRs, NDD, PUSH2/POP2, PUSHP/POPP, `{rex2}` —
  and AVX10.1; 2.43 (2024-08-04) completes APX_F (NF, zero-upper,
  CCMP/CTEST, CFCMOV); 2.44 (2025-02-02) adds AVX10.2; 2.45 (2025-07-27)
  drops AVX10.2 256-bit rounding (spec now 512-only) — prefer ≥ 2.45; latest
  is 2.46 (announced 2026-02-09,
  <https://lists.gnu.org/archive/html/info-gnu/2026-02/msg00006.html>) with
  the 2.46.1 tarball (2026-06-08) on <https://ftp.gnu.org/gnu/binutils/>.
  **Pin 2.46.1.** Ubuntu 24.04 ships 2.42 → EGPR-only; hence the source build
  in T2.
- **NASM minimums.** NASM 3.00 adds APX + AVX10
  (<https://www.nasm.us/doc/nasmdocc.html>); 3.01/3.02 fix numerous
  instruction encodings; latest stable 3.02 (2026-06-28,
  <https://www.nasm.us/pub/nasm/releasebuilds/>). Ubuntu 24.04 apt nasm is
  2.16.01 (<https://packages.ubuntu.com/noble/nasm>) — pre-APX. **Pin 3.02.**
- **Digests still to compute at implementation time:** binutils-2.46.1.tar.xz
  and nasm-3.02.tar.xz SHA-256s (compute on first verified download from the
  official hosts above, record in `scripts/third-party-digests.txt`; the SDE
  digest is already known, above).
- **Caveats.** intel.com pages 403 automated fetches, so the release-notes
  claim that 10.8.0 tracks ISE rev 319433-060 rests on a search snippet; the
  gate page's click-through wording was not readable (the vendored LICENSE.pdf
  from inside the kit is authoritative for our purposes); NASM release dates
  are directory mtimes.

## Out of scope

- **Tracing under Pin / XED decode of APX on real silicon** — the DBI trace
  producer and its shm hand-off: [pin-xed-trace-tier.md](pin-xed-trace-tier.md).
- **Pin probe-mode arg/return capture**:
  [pin-probe-mode-capture.md](pin-probe-mode-capture.md).
- **libdft64 taint oracle** (shares the Pin kit, not the SDE kit):
  [pin-libdft-taint-oracle.md](pin-libdft-taint-oracle.md).
- **Stacking Pin tools on SDE for APX-host-less DBI traces** — recorded as a
  follow-up inside the Pin plan's PIN-2 track; nothing here depends on it.
- **Raising the Unicorn tier's AVX ceiling** (upstream QEMU/Unicorn gate):
  tracked in
  [live-attach-dataflow-followup-plan.md](../archive/plans/live-attach-dataflow-followup-plan.md);
  this lane routes around it, it does not fix it.
- **Packaging/notice generation for shipped artifacts** (SDE never ships):
  [distribution-packaging.md](distribution-packaging.md).
- **The refresh script's pre-existing zig-line drop** (noted in T1): a repo
  hygiene fix any doc could own; this one only guarantees its own lines
  survive a refresh.
