# XED-decoded Pin trace tier + shared pintool substrate — implementation

> **Sources.** Actioned from
> [intel-pin-capabilities-plan.md](../plans/intel-pin-capabilities-plan.md)
> (track PIN-2 + the shared constraints) and
> [2026-07-17-intel-pin-vs-dynamorio.md](../analysis/2026-07-17-intel-pin-vs-dynamorio.md)
> (genuine-delta #2, "DBI decode of the newest extensions"). Written
> 2026-07-17. If this doc and a source disagree, this doc wins (sources may be
> stale); if the CODE and this doc disagree, re-verify before implementing.

## Why this work exists

DynamoRIO — the repo's shipped DBI tracer — maintains its own x86 decoder, and
that decoder still rejects Intel APX (EGPRs r16–r31, REX2, PUSH2/POP2, CCMP —
[DR issue #6226](https://github.com/DynamoRIO/dynamorio/issues/6226), open with
no activity since 2023). Intel Pin decodes with XED, Intel's canonical decoder,
so Pin can DBI-trace a routine using APX **today** on capable silicon, where the
drtrace tier would abort. This doc builds that Pin trace lane: a pinned,
digest-gated Pin kit (`Dockerfile.pintool` + `scripts/fetch-pin.sh` — the
substrate the [pin-probe-mode-capture.md](pin-probe-mode-capture.md) and
[pin-libdft-taint-oracle.md](pin-libdft-taint-oracle.md) tracks reuse), a
Pintool that fills the repo's shared `asmtest_trace_t` offset model over shared
memory, offset-parity assertions against the DynamoRIO and single-step
backends, and an APX fixture where Pin traces and DR decoder-errors.

Two corrections to the sources, binding here: (1) DR external attach **is**
wired in this repo for the taint/dataflow tier (`Dockerfile.taint-attach`,
`docker-dataflow-attach`, landed 2026-07-14) — Pin's value is decoder
*currency*, never "DR cannot attach". (2) AVX-512 VNNI is **fixed** in
DynamoRIO ([#5440](https://github.com/DynamoRIO/dynamorio/issues/5440) closed
2022-04-25 via [PR #5444](https://github.com/DynamoRIO/dynamorio/pull/5444));
the pinned DR 11.91.20630 decodes VNNI, so the Pin tier's case rests on APX
alone (T9 fixes the stale claims in the source docs).

## What already exists (verified 2026-07-17)

- [include/asmtest_trace.h](../../../include/asmtest_trace.h) — the
  engine-neutral trace sink every backend fills: `asmtest_trace_t` records
  instruction offsets in order plus de-duplicated distinct block-start offsets,
  with `insns_total`/`blocks_total` counters and a `truncated` bit.
  `trace_append_insn` / `trace_append_block` (lines 69–70) define the
  append/dedup/truncate semantics the Pintool must mirror. The header's block
  rule (comment above `asmtest_disas_is_branch`, ~line 193): *a block ends
  after every branch-class instruction, so the fall-through of a NOT-taken
  conditional branch (and a call-return re-entry) starts a new block, matching
  the PT / DynamoRIO / Unicorn partition.*
- [include/asmtest_drtrace.h](../../../include/asmtest_drtrace.h) — line 113
  declares `asmtest_trace_begin(const char *name)` / `asmtest_trace_end`, the
  real exported marker functions;
  [src/drtrace_app.c](../../../src/drtrace_app.c) line 462 defines them (with
  a `volatile`-sink body so they never fold away), and
  [src/drtrace_client.c](../../../src/drtrace_client.c) line 346 shows the
  client resolving them by name (`dr_get_proc_address(h,
  "asmtest_trace_begin")`). The Pintool resolves the same symbols with Pin's
  `RTN_FindByName`.
- [include/asmtest_taint_shm.h](../../../include/asmtest_taint_shm.h) — the
  launch-under-DBI shared-memory precedent: a fixed POSIX-shm layout
  (`at_shm_channel_t`) with a `done` flag, embedded fixed-capacity arrays, and
  the cross-process rule *consumers read only by offset, never stored
  pointers*. [examples/taint_workload.c](../../../examples/taint_workload.c)
  (the launched fixture that creates the segment, exports marker symbols,
  materializes hand-assembled bytes in W^X memory, and runs them) and
  [examples/taint_validator.c](../../../examples/taint_validator.c) (the
  out-of-process reader that runs after the DBI launcher exits) are the shapes
  T4/T6 mirror.
- [examples/test_drtrace.c](../../../examples/test_drtrace.c) — line 47's
  `ROUTINE[]`: an 18-byte hand-assembled two-block x86-64 routine
  (`mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret`) already
  traced by the DR tier. This exact byte array is the parity fixture.
- [Dockerfile.drtrace](../../../Dockerfile.drtrace) — the pinning pattern:
  `ARG DR_VERSION=11.91.20630` + `curl` the release tarball + extract to
  `/opt/...` + `ENV DYNAMORIO_HOME`.
  [scripts/fetch-dynamorio.sh](../../../scripts/fetch-dynamorio.sh) — the
  fetch-script pattern: cache under `build/`, verify the tarball SHA-256 via
  `tp_digest`/`tp_sha256` from
  [scripts/lib-thirdparty.sh](../../../scripts/lib-thirdparty.sh) against
  [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt)
  (format: `tarball-sha256  <name>  <version>  sha256:<hex>`), capture the
  upstream license into [licenses/](../../../licenses/) on first fetch, print
  the home dir on stdout.
  [scripts/refresh-thirdparty-digests.sh](../../../scripts/refresh-thirdparty-digests.sh)
  regenerates the manifest after a version bump.
- [mk/docker.mk](../../../mk/docker.mk) — `docker-drtrace` (~line 226): build
  `Dockerfile.drtrace` with `--build-arg DR_VERSION`, run the image. The
  `DOCKER`/`DOCKER_BASE`/`_docker_plat` knobs at the top are shared by every
  lane. [mk/native-trace.mk](../../../mk/native-trace.mk) —
  `dr-taint-attach-coop-test` (~line 541) is the run-launcher-then-validator
  target shape (an `ATTACH_SHM ?=` name knob, `rm -f /dev/shm$(NAME)`, run the
  workload, run the validator). `HWTRACE_OBJS` (line 2047) lists the objects
  that give any harness the in-process single-step backend
  (`ASMTEST_HWTRACE_SINGLESTEP`,
  [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h) line 70 —
  works on any x86-64 Linux, containerizable); the `$(BUILD)/test_hwtrace`
  link line (2058–2059) shows the libs to append.
- [mk/cli.mk](../../../mk/cli.mk) — the architecture-gate pattern to copy:
  `CLI_ARCH := $(shell uname -m)` (line 21) and an
  `ifneq ($(CLI_ARCH),x86_64)` branch (line 72) that prints
  `# SKIP ...` + `1..0 # skipped` with the reason spelled out.
- Confirmed absent (so everything below is new):
  `grep -rn "PIN_VERSION\|pintool\|fetch-pin" Makefile mk/ scripts/ Dockerfile.*`
  matches nothing; there is no `Dockerfile.pintool`, no `scripts/fetch-pin.sh`,
  no Pin line in `scripts/third-party-digests.txt`.

Baseline proof before touching anything (on any host):

```
make check          # framework self-tests (tests/expect.sh) — all pass
make help | head    # confirms the target inventory the docs below extend
```

On an x86-64 Linux host with Docker, additionally `make docker-drtrace`
(expected: image builds, `drtrace-test` + `drtrace-python-test` run green
inside it) — that proves the DR side of the parity assertions works before Pin
enters the picture.

## Tasks

### T1 — `scripts/fetch-pin.sh` + digest pin + license vendoring  (S, depends on: none)

**Goal.** A pinned Intel Pin 4.2 kit can be fetched, SHA-256-verified against
`scripts/third-party-digests.txt`, and its license vendored, by one idempotent
script that prints the kit root.

**Steps.**
1. Create `scripts/fetch-pin.sh` by mirroring
   [scripts/fetch-dynamorio.sh](../../../scripts/fetch-dynamorio.sh)
   structure line-for-line (env-overridable version/URL, `lib-thirdparty.sh`
   digest gate, cache reuse, license capture, home echoed on stdout).
2. Add the digest line to
   [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt).
   **Compute the digest yourself** (`curl -fsSL <url> | shasum -a 256`) and
   compare it against the research-notes value below; Intel publishes no
   official hash, so two independent computations agreeing is the bar. If they
   disagree, stop and investigate — do not pin either value.
3. Extend
   [scripts/refresh-thirdparty-digests.sh](../../../scripts/refresh-thirdparty-digests.sh):
   read `PIN_VERSION` out of `fetch-pin.sh` with the existing `read_ver`
   helper, add a `pin_digest()` that downloads the tarball to a temp file and
   `tp_sha256`s it (no GitHub API — Intel hosts the tarball directly), and
   emit the `pin` line in the rewritten manifest.
4. Add a `Pin` row to [licenses/README.md](../../../licenses/README.md)'s
   table, noting **test-lane only, never bundled** (see Constraints). Do NOT
   add an `emit` line to
   [scripts/collect-licenses.sh](../../../scripts/collect-licenses.sh) — that
   script enumerates only components shipped in packages, and Pin never ships.

**Code.** In `fetch-pin.sh`:

```sh
PIN_VERSION="${PIN_VERSION:-4.2-99776-g21d818fa2}"
PIN_URL="${PIN_URL:-https://software.intel.com/sites/landingpage/pintool/downloads/pin-external-${PIN_VERSION}-gcc-linux.tar.gz}"
PIN_CACHE="${PIN_CACHE:-$root/build/pin}"
home="$PIN_CACHE/pin-external-$PIN_VERSION-gcc-linux"
```

Completeness marker: `[ -x "$home/pin" ]` (the kit-root launcher), with the
same normalize-top-dir fallback fetch-dynamorio.sh uses in case the tarball's
top directory name differs. Digest check: `tp_digest tarball-sha256 pin
"$PIN_VERSION"`, refuse-if-unpinned, exactly like the DR script. License
capture on first fetch: copy
`$home/licensing/intel-simplified-software-license.txt` to
`licenses/Pin-$PIN_VERSION.txt` and
`$home/licensing/third-party-programs.txt` to
`licenses/Pin-$PIN_VERSION-third-party.txt` (the second file records that the
bundled XED is Apache-2.0). Manifest line:

```
tarball-sha256  pin  4.2-99776-g21d818fa2  sha256:194a2cec51678203452ece0d9e8cbb1819eb6e1221f0341091c49248f384d869
```

(re-verify per step 2 before committing).

**Tests.** No unit-test surface (a fetch script); manual verification is the
test: run `sh scripts/fetch-pin.sh` twice on any host with network. First run
prints `fetch-pin: fetching Intel Pin 4.2-...`, `fetch-pin: verified ...
(sha256:194a...)`, captures the license, and echoes the home path; second run
prints `fetch-pin: reusing cached ...`. Corrupt one byte of the cached tarball
mid-download simulation (or temporarily edit the manifest digest) and confirm
the script FAILS with the expected/got digest pair — the gate must be provably
live.

**Docs.** `licenses/README.md` row (step 4). Changelog handled in T9.

**Done when.**
- `sh scripts/fetch-pin.sh` succeeds, echoes a directory whose `pin` binary
  exists, and `licenses/Pin-4.2-99776-g21d818fa2.txt` contains the "Intel
  Simplified Software License (Version October 2022)" text.
- A wrong digest in the manifest makes the script exit non-zero with both
  hashes printed.
- `scripts/refresh-thirdparty-digests.sh` regenerates the manifest including
  the `pin` line (run it and diff — only formatting-stable changes).

### T2 — `Dockerfile.pintool` + `docker-pintool` lane  (S, depends on: T1)

**Goal.** `make docker-pintool` builds an image containing the digest-verified
Pin kit (and DynamoRIO, for the T7 parity arm) and runs the lane's test target.

**Steps.**
1. Create `Dockerfile.pintool` mirroring
   [Dockerfile.drtrace](../../../Dockerfile.drtrace)'s header-comment style
   and structure. Differences: copy
   `scripts/fetch-pin.sh scripts/lib-thirdparty.sh scripts/third-party-digests.txt`
   into the image *before* the full `COPY . .` and install Pin via
   `RUN PIN_CACHE=/opt/pin sh scripts/fetch-pin.sh` so the **digest gate runs
   inside the image build** (Dockerfile.drtrace predates the manifest and
   curls raw; the Pin lane starts gated). Then
   `ENV PIN_HOME=/opt/pin/pin-external-4.2-99776-g21d818fa2-gcc-linux`.
2. Keep the DynamoRIO block from Dockerfile.drtrace verbatim
   (`ARG DR_VERSION=11.91.20630` … `ENV DYNAMORIO_HOME=/opt/dynamorio`) — the
   T7 DR-parity arm needs `drrun`-less in-process DR plus the CMake-built
   client, so also keep `build-essential cmake curl ca-certificates` in the
   apt line. Capstone/Unicorn are NOT needed (no value producer here); leave
   them out to keep the image light.
3. Add the `docker-pintool` rule to [mk/docker.mk](../../../mk/docker.mk)
   next to `docker-drtrace` (~line 226), same shape:
   `PIN_VERSION ?= 4.2-99776-g21d818fa2`, build with
   `--build-arg BASE=$(DOCKER_BASE) --build-arg PIN_VERSION=$(PIN_VERSION)
   --build-arg DR_VERSION=$(DR_VERSION)`, tag `asmtest-pintool`, then
   `$(DOCKER) run --rm $(_docker_plat) asmtest-pintool`. Add the lane to the
   comment inventory at the top of mk/docker.mk.
4. Image `CMD`: `make pintool-test PIN_HOME=$PIN_HOME
   DYNAMORIO_HOME=$DYNAMORIO_HOME` (target lands in T3/T6; until then point
   CMD at `$PIN_HOME/pin -- /bin/true` as the bring-up smoke and flip it in
   T6).

**Code.** Per steps; `ARG PIN_VERSION=4.2-99776-g21d818fa2` must flow into the
`fetch-pin.sh` RUN via `ENV`-less `PIN_VERSION=$PIN_VERSION` on the RUN line so
`docker build --build-arg PIN_VERSION=...` can bump it.

**Tests.** `make docker-pintool` on an x86-64 Linux Docker host. Build output
must show `fetch-pin: verified ...` (the in-image digest gate firing). The run
step's observable output is the bring-up smoke (T2) and later the full TAP
stream (T6).

**Docs.** Lane listed in mk/docker.mk's header comment. Changelog in T9.

**Done when.**
- `make docker-pintool` builds and the container exits 0.
- Build log contains the `fetch-pin: verified` line (digest enforced
  in-image).
- `docker run --rm asmtest-pintool sh -c '$PIN_HOME/pin -- /bin/true; echo rc=$?'`
  prints `rc=0` (Pin launches a process end-to-end).

### T3 — `mk/pintool.mk` + out-of-kit tool build scaffolding  (M, depends on: T1)

**Goal.** A `pintool/` directory builds `asmtest_pintool.so` against the
pinned kit via Pin's out-of-kit `PIN_ROOT` make protocol, wired behind an
arch/OS gate in a new `mk/pintool.mk`.

**Steps.**
1. Create `mk/pintool.mk`; add `include mk/pintool.mk` to
   [Makefile](../../../Makefile) directly after the
   `include mk/native-trace.mk` line (~line 859) — it must come after so
   `HWTRACE_OBJS`, `DRAPP_KS_OBJ` and friends are already defined for T6's
   validator link. Follow the file-header comment style of
   [mk/cli.mk](../../../mk/cli.mk) (included-by-parent, knobs live in
   Makefile).
2. Gates, copied from mk/cli.mk's `CLI_ARCH` pattern:
   `PINTOOL_ARCH := $(shell uname -m)`; every runnable target in this file is
   wrapped in `ifneq ($(PINTOOL_ARCH),x86_64)` → print
   `# SKIP pintool: Intel Pin is x86-only and this host is $(PINTOOL_ARCH).`
   + `1..0 # skipped`; and `ifneq ($(UNAME_S),Linux)` → skip with
   `# SKIP pintool: the pinned Pin kit is gcc-linux; this host is $(UNAME_S).`
   These are REAL gates (architecture), per CLAUDE.md.
3. On the live branch, resolve the kit:
   `home=$${PIN_HOME:-$$(sh scripts/fetch-pin.sh)}` inside the recipe — a
   missing Pin is an installable dependency, so the target fetches it rather
   than self-skipping (CLAUDE.md rule; `PIN_HOME` overrides, and the docker
   lane always sets it).
4. Create `pintool/makefile` — copy the ~9-line out-of-kit stub from the
   kit's `source/tools/MyPinTool/makefile` (it is MIT/SPDX-licensed
   scaffolding, safe to derive from): `ifdef PIN_ROOT` →
   `CONFIG_ROOT := $(PIN_ROOT)/source/tools/Config`, include
   `$(CONFIG_ROOT)/makefile.config`, `makefile.rules`,
   `$(TOOLS_ROOT)/Config/makefile.default.rules`.
5. Create `pintool/makefile.rules` declaring the tool:
   `TEST_TOOL_ROOTS := asmtest_pintool` (the kit's rules then build
   `pintool/obj-intel64/asmtest_pintool.so` from `asmtest_pintool.cpp`).
6. Create a v0 `pintool/asmtest_pintool.cpp`: `#include "pin.H"`, a `main`
   that calls `PIN_Init(argc, argv)`, prints
   `asmtest_pintool: loaded` to stderr, and calls `PIN_StartProgram()`.
7. Add to mk/pintool.mk: `PINTOOL_SO := pintool/obj-intel64/asmtest_pintool.so`
   and a `pintool-tool` phony target whose recipe is
   `cd pintool && $(MAKE) PIN_ROOT=$$home obj-intel64/asmtest_pintool.so`.

**Code.** Note in mk/pintool.mk's header that the tool compiles under
**PinCRT** (`-DPIN_CRT=1 -nostdlib -fno-exceptions -fno-rtti`, injected by the
kit's `makefile.unix.config`), so the tool source must stick to the PinCRT
subset — no libstdc++ iostream niceties, no exceptions, plain POSIX
`open`/`mmap` (see T5).

**Tests.** On x86-64 Linux (host or `docker run --rm -it asmtest-pintool
bash`): `make pintool-tool` produces `pintool/obj-intel64/asmtest_pintool.so`;
`$PIN_HOME/pin -t pintool/obj-intel64/asmtest_pintool.so -- /bin/true` prints
`asmtest_pintool: loaded` and exits 0. On this repo's macOS-arm64 dev host:
`make pintool-tool` prints the SKIP reason and `1..0 # skipped` — verify both
branches.

**Docs.** Internal-only (build scaffolding), no user-facing docs; the make
target inventory comment in mk/pintool.mk is the documentation.

**Done when.**
- `make pintool-tool` builds the `.so` on x86-64 Linux and self-skips with a
  printed architecture reason on arm64/macOS.
- The null tool runs `/bin/true` under `pin` with the loaded line visible.
- `git grep -n "include mk/pintool.mk" Makefile` hits exactly once.

### T4 — shm channel layout + launch-under-pin fixture  (M, depends on: T3)

**Goal.** A fixed shared-memory layout (`pintool/pintool_shm.h`) and a
launched workload (`examples/pin_trace_workload.c`) that exports the
`asmtest_trace_begin`/`_end` markers, materializes the parity ROUTINE in W^X
memory, and runs it twice between markers.

**Steps.**
1. Create `pintool/pintool_shm.h` (deliberately **not** under `include/` —
   PIN-2 ships no public header; the fixture compiles with `-Ipintool`).
   Mirror [include/asmtest_taint_shm.h](../../../include/asmtest_taint_shm.h)'s
   cross-process doc comment (offsets only, never pointers). Layout — plain
   C99, C++-safe, `stdint.h` only:

```c
#define PIN_SHM_NAME       "/asmtest_pin_trace"
#define PIN_SHM_INSNS_CAP  256
#define PIN_SHM_BLOCKS_CAP 64
typedef struct asmtest_pin_channel {
    uint32_t magic;              /* 0x50494E54 "PINT": tool checks the layout */
    volatile uint32_t done;      /* fixture sets 1 after markers (release)    */
    uint64_t region_base;        /* fixture-space VA of the routine           */
    uint64_t region_len;
    char     region_name[64];
    int64_t  result;             /* r1*1000 + r2 liveness check               */
    uint64_t insns[PIN_SHM_INSNS_CAP];   /* offsets from region_base, in order */
    uint64_t insns_len, insns_total;
    uint64_t blocks[PIN_SHM_BLOCKS_CAP]; /* DISTINCT block-start offsets       */
    uint64_t blocks_len, blocks_total;
    uint32_t truncated;
} asmtest_pin_channel_t;
```

2. Create `examples/pin_trace_workload.c` mirroring
   [examples/taint_workload.c](../../../examples/taint_workload.c): shm name
   from `argv[1]` (default `PIN_SHM_NAME`), `shm_open(O_CREAT|O_RDWR)` +
   `ftruncate` + `mmap`, zero the channel, set `magic`.
3. Define the marker symbols **in the workload** (self-contained, like
   taint_workload.c does): `asmtest_trace_begin`/`asmtest_trace_end` as
   `__attribute__((noinline, visibility("default")))` functions with distinct
   `volatile`-sink bodies — copy the body shape from
   [src/drtrace_app.c](../../../src/drtrace_app.c) line 462. Do NOT link
   `drtrace_app.o` into the workload (its markers would collide; the workload
   runs under `pin`, not under the DR lifecycle).
4. Copy `ROUTINE[]` from
   [examples/test_drtrace.c](../../../examples/test_drtrace.c) line 47 with a
   `/* KEEP IN SYNC with examples/test_drtrace.c ROUTINE */` comment (the
   repo's established idiom — taint_workload.c does the same for its
   fixture). `mmap` RW, `memcpy`, `mprotect` R+X.
5. Flow: write `region_base`/`region_len`/`region_name="add2"` into the
   channel **before** the first marker call; then
   `asmtest_trace_begin("add2"); r1 = fn(3, 4); asmtest_trace_end("add2");`
   and again with `fn(60, 50)`; store `result = r1 * 1000 + r2` (expected
   `7 * 1000 + 109 = 7109`); set `done = 1`.
6. Build rule in mk/pintool.mk: compile with `-Ipintool`, link `-rdynamic`
   (marker symbols must reach the dynamic symbol table for `RTN_FindByName`,
   exactly as `$(BUILD)/test_drtrace`'s rule comments in
   [mk/native-trace.mk](../../../mk/native-trace.mk) explain) and `-lrt`.

**Code.** Ordering matters and must be commented in the source: the region is
first *executed* only after `asmtest_trace_begin` returns, so by the time Pin
JITs region code the tool has already read `region_base`/`region_len` (see
T5's assumption).

**Tests.** Native (no Pin): `./build/pin_trace_workload /asmtest_pin_t4 &&
echo rc=$?` prints `rc=0`; then a scratch check that
`/dev/shm/asmtest_pin_t4` exists with `done==1`, `result==7109`, and all
trace counters zero (nothing instrumented natively) — a 10-line throwaway
check or `xxd` inspection is fine here; T6's validator is the real consumer.

**Docs.** Internal-only; the shm header's doc comment is the contract.

**Done when.**
- The workload runs natively on x86-64 Linux, exits 0, and leaves
  `done=1, result=7109, insns_total=0` in the segment.
- `nm -D build/pin_trace_workload | grep asmtest_trace_begin` shows the
  exported marker.
- Zero diff under `include/` (`git status include/` clean).

### T5 — the Pintool: marker resolution + region instrumentation + shm trace  (M, depends on: T4)

**Goal.** `asmtest_pintool.so` resolves the exported markers by symbol, gates
recording on them, instruments every instruction in
`[region_base, region_base+region_len)`, and appends offsets to the shm
channel with the repo's append/dedup/truncated discipline.

**Steps.**
1. Replace the T3 null body of `pintool/asmtest_pintool.cpp`:
   - `KNOB<std::string> KnobShm(KNOB_MODE_WRITEONCE, "pintool", "shm",
     PIN_SHM_NAME, "POSIX shm channel name")` (include `pintool_shm.h`).
   - `main`: `PIN_InitSymbols(); PIN_Init(argc, argv);`
     `IMG_AddInstrumentFunction`, `TRACE_AddInstrumentFunction`,
     `PIN_StartProgram()`.
2. Marker resolution (`on_img`): only for `IMG_IsMainExecutable(img)`;
   `RTN_FindByName(img, "asmtest_trace_begin")`; if `RTN_Valid`,
   `RTN_Open` → `RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)on_begin,
   IARG_END)` → `RTN_Close`; same for `"asmtest_trace_end"` → `on_end`. This
   is the Pin analog of drtrace_client.c:346's `dr_get_proc_address`.
3. `on_begin` (analysis): lazily map the channel — build the path
   `"/dev/shm" + KnobShm.Value()` and use plain `open(path, O_RDWR)` +
   `mmap` (PinCRT provides these POSIX calls; avoid `shm_open`, which drags
   librt into a `-nostdlib` PinCRT link — if `open`/`mmap` are unavailable in
   the kit's CRT, fall back to the kit's `OS_OpenFD`/`OS_MapFileToMemory`
   os-apis). Check `magic`, cache `region_base`/`region_len`, set
   `g_active = 1`, reset the sequential-predecessor state. `on_end`: set
   `g_active = 0`.
4. `on_trace` (instrumentation callback): if the channel is not mapped yet,
   return (region code cannot be JITted before `on_begin` ran — the fixture
   only executes the region between markers; note this assumption in a
   comment, with `PIN_RemoveInstrumentation()` named as the fallback if it is
   ever observed violated). For each `BBL`, each `INS` whose `INS_Address` is
   in `[region_base, region_base+region_len)`:
   `INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)on_insn,
   IARG_ADDRINT off, IARG_BOOL INS_IsControlFlow(ins),
   IARG_UINT32 INS_Size(ins), IARG_END)`.
5. `on_insn(off, is_cf, size)` (analysis, hot path): if `!g_active`, return.
   Block-head rule — **derive heads at runtime; do not trust Pin's BBL
   partition** (Pin splits traces at boundaries that need not match the
   repo's block model): the instruction is a head iff (a) it is the first
   in-region instruction since `on_begin`, or (b) the previous in-region
   instruction was control-flow (`is_cf` was true), or (c)
   `off != prev_off + prev_size` (non-sequential entry: branch target,
   call-return re-entry). This reproduces the partition documented in
   [include/asmtest_trace.h](../../../include/asmtest_trace.h) (block ends
   after every branch-class instruction; NOT-taken fall-through starts a new
   block). Append: insns — record `off` if `insns_len < CAP` else
   `truncated = 1`; always `insns_total++`. Heads — linear-scan dedup against
   `blocks[]` (mirroring `trace_append_block`), record if new and
   `blocks_len < CAP` else `truncated = 1`; always `blocks_total++`.
6. Single-threaded assumption (the fixture is single-threaded): note it; no
   locks in v1.

**Code.** Everything lives in `pintool/asmtest_pintool.cpp`; PinCRT rules from
T3 apply (no exceptions/RTTI/iostream).

**Tests.** Manual end-to-end (the automated harness lands in T6):

```
rm -f /dev/shm/asmtest_pin_t5
$PIN_HOME/pin -t pintool/obj-intel64/asmtest_pintool.so -shm /asmtest_pin_t5 \
    -- ./build/pin_trace_workload /asmtest_pin_t5
```

exits 0; the segment now holds `insns_total=11`, `blocks_len=3`,
`blocks_total=4`, `truncated=0` (expected offsets in T6). A failure looks
like zero counters (markers not resolved — check `nm -D` on the workload and
`PIN_InitSymbols`) or wildly large counters (gating broken).

**Docs.** Internal-only.

**Done when.**
- The manual run above leaves the T6-table values in the segment.
- Running the workload **without** `-t` tool (plain `pin --`) still exits 0
  with zero counters — the tool is additive, never load-bearing for the app.

### T6 — validator + `pintool-test` target + single-step offset parity  (M, depends on: T5)

**Goal.** `make pintool-test` runs the workload under `pin`, then an
out-of-process validator that asserts the exact expected offsets AND
byte-for-byte offset parity with the in-process single-step backend.

**Steps.**
1. Create `examples/pin_trace_validator.c` mirroring
   [examples/taint_validator.c](../../../examples/taint_validator.c)'s
   read-after-launcher-exits shape: `shm_open(O_RDWR)` + `mmap` (librt is
   fine here — this is a normal host binary), TAP output
   (`ok N - ...` / `not ok N - ...`, matching test_drtrace.c's CHECK macro).
2. Expected-value assertions for the shared ROUTINE (copy `ROUTINE[]` again
   with the KEEP-IN-SYNC comment), inputs `(3,4)` then `(60,50)`:
   - `done==1`, `result==7109`, `truncated==0`;
   - insns, in order: `0x0,0x3,0x6,0xc,0x11` (jle taken) then
     `0x0,0x3,0x6,0xc,0xe,0x11` (fall-through) — `insns_len==insns_total==11`;
   - blocks, distinct, first-entry order: `0x0, 0x11, 0xe` —
     `blocks_len==3`, `blocks_total==4` (0x11 is a head only when entered as
     a branch target; in the second call `0x11` continues the `0xe` block).
3. Single-step parity (the always-on cross-backend check): the validator
   itself maps the same ROUTINE bytes W^X, initializes the hwtrace tier with
   `ASMTEST_HWTRACE_SINGLESTEP`
   ([include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h) line 70;
   works on any x86-64 Linux, in-container), registers the region with a
   fresh `asmtest_trace_new(256, 64)`, brackets the same two calls with
   `asmtest_hwtrace_begin`/`_end`, then diffs `insns[]` and `blocks[]`
   element-for-element against the shm arrays. Byte-exact or `not ok`.
4. Build rule in mk/pintool.mk:
   `$(BUILD)/pin_trace_validator: $(HWTRACE_OBJS) $(BUILD)/drtrace_app.o
   $(DRAPP_KS_OBJ) $(BUILD)/pin_trace_validator.o` linked
   `-rdynamic ... $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS)
   $(LINK_LIBBPF) $(DRAPP_KS_LIBS) -ldl -lpthread -lrt` — the union of the
   `$(BUILD)/test_hwtrace` (mk/native-trace.mk line 2058) and
   `$(BUILD)/test_drtrace` link lines. `drtrace_app.o` compiles everywhere
   (its DR calls are dlopen-based) and is needed for T7.
5. `pintool-test` in mk/pintool.mk, mirroring `dr-taint-attach-coop-test`
   (mk/native-trace.mk ~line 541): `PIN_SHM ?= /asmtest_pin_trace_ci`; under
   the T3 gates: build tool + workload + validator;
   `rm -f /dev/shm$(PIN_SHM)`; run
   `$$home/pin -t $(PINTOOL_SO) -shm $(PIN_SHM) -- $(BUILD)/pin_trace_workload $(PIN_SHM)`;
   run `$(BUILD)/pin_trace_validator $(PIN_SHM)`.
6. Flip `Dockerfile.pintool`'s CMD to
   `make pintool-test PIN_HOME=$PIN_HOME DYNAMORIO_HOME=$DYNAMORIO_HOME`.

**Code.** Per steps; keep the expected-offset tables as `static const`
arrays with a comment deriving each offset from the ROUTINE disassembly.

**Tests.** This task IS the test. Pass: `make docker-pintool` ends with a
TAP block, every line `ok`, including
`ok N - pin/single-step insn offsets byte-identical`. Fail: any `not ok` and
a non-zero container exit. On arm64/macOS hosts `make pintool-test` prints
the SKIP reason + `1..0 # skipped`.

**Docs.** Internal-only; changelog in T9.

**Done when.**
- `make docker-pintool` (x86-64 Linux Docker host) exits 0 with all-`ok` TAP
  including both expected-value and single-step-parity lines.
- `make pintool-test` on the macOS-arm64 dev host self-skips cleanly with the
  architecture reason printed.

### T7 — DynamoRIO offset-parity arm  (S, depends on: T6)

**Goal.** In the docker lane, the same ROUTINE traced by the DR tier yields
offsets byte-identical to Pin's — the three-way parity PIN-2's exit criteria
demand (Pin ≡ DR ≡ single-step).

**Steps.**
1. Extend `examples/pin_trace_validator.c`: when
   `getenv("ASMTEST_DRCLIENT")` is set, replay the exact
   [examples/test_drtrace.c](../../../examples/test_drtrace.c) flow —
   `asmtest_dr_init` (options naming the client .so), `asmtest_exec_alloc`
   the ROUTINE, `asmtest_dr_register_region("add2", ...)` with a fresh
   trace, `asmtest_trace_begin/end` around `fn(3,4)` and `fn(60,50)`,
   `asmtest_dr_shutdown` — then diff `insns[]`/`blocks[]` against the shm
   arrays. When the env var is absent, print
   `# SKIP dr-parity: DynamoRIO not configured (set ASMTEST_DRCLIENT)` — a
   real dependency-absence report on hosts, while the docker lane always
   exercises it.
2. In `pintool-test`, before the validator runs:
   `$(MAKE) drtrace-client` when `DR_AVAILABLE` is set (the variable
   mk/native-trace.mk already computes), and pass
   `ASMTEST_DRCLIENT=$(abspath $(BUILD)/libasmtest_drclient.so)
   ASMTEST_DR_LIB=$(abspath $(DR_DLLIB))` on the validator invocation — the
   same env pair `drtrace-test` uses (mk/native-trace.mk line 146).
3. `Dockerfile.pintool` already installs DR + cmake (T2), so the lane runs
   this arm unconditionally in-container.

**Code.** One process runs DR in-process *after* Pin has exited (the
validator is a separate process from the workload), so there is no
DBI-on-DBI stacking anywhere.

**Tests.** `make docker-pintool` now additionally prints
`ok N - pin/DynamoRIO insn offsets byte-identical` and
`ok N - pin/DynamoRIO block offsets byte-identical`. Host run without DR
prints the `# SKIP dr-parity` line and still passes.

**Docs.** Internal-only.

**Done when.**
- The docker lane's TAP shows all three producers byte-identical on the
  ordinary routine.
- The validator without `ASMTEST_DRCLIENT` skips the DR arm with a printed
  reason and exits 0.

### T8 — APX fixture: Pin traces, DR decoder-errors  (M, depends on: T5, T6)

**Goal.** An APX (REX2/EGPR) routine is decoded by the kit's XED on any
x86-64 host, traced end-to-end by Pin on APX silicon, and shown to
decoder-error under the pinned DynamoRIO — the negative control that proves
the tier earns its keep.

**Steps.**
1. Produce the APX byte fixture: a short routine using EGPRs (e.g.
   REX2-prefixed `add`/`mov` touching r16/r17, then `ret`). Try the image's
   assembler first (`as` from binutils; if it rejects `%r16`, hand-encode
   using the REX2 prefix per the current *Intel APX Architecture
   Specification* — do not trust memory for encodings). Store the bytes as
   `APX_ROUTINE[]` in a new `examples/pin_apx_workload.c` (same shm scaffold
   as T4's workload; region name `"apx"`).
2. Ungated decode assertion — `examples/pin_apx_decode.c`: a plain host
   program compiled with `-I$PIN_HOME/extras/xed-intel64/include/xed` and
   linked against `$PIN_HOME/extras/xed-intel64/lib/libxed.so`, which
   `xed_decode`s each instruction of `APX_ROUTINE[]` and asserts the reported
   extension is `XED_EXTENSION_APXEVEX` or `XED_EXTENSION_APXLEGACY` (both
   enums exist in the kit's `xed-extension-enum.h`; they sit at lines 141–142
   in the Pin 3.31 fallback kit's XED — verify the line numbers against the
   fetched 4.2 kit, see Research notes). This runs on ANY x86-64 host: it pins
   the bytes as
   really-APX without silicon.
3. Silicon gate: detect APX via CPUID leaf 7 subleaf 1 (the `APX_F` flag —
   verify the exact register/bit against the current Intel APX spec when
   coding; do not copy a bit number from this doc). Without `APX_F`:
   `# SKIP pintool-apx: host CPU lacks APX (hardware gate)` +
   `1..0 # skipped` — a REAL hardware gate per CLAUDE.md. Pin-on-SDE
   stacking (which would lift this gate) is a recorded follow-up, out of
   scope here (see Out of scope).
4. Positive half (APX silicon only): run `pin -t asmtest_pintool.so` over
   `pin_apx_workload`; validator asserts a complete trace
   (`insns_total` equals the fixture's instruction count, `truncated==0`).
5. Negative control (APX silicon only): the validator replays the APX bytes
   through the DR tier (T7 plumbing) and asserts DR does **not** produce a
   clean, complete trace — accept either an error return from the DR path or
   a trace that fails the completeness check, and print which was observed.
   Guard: this control is valid while the pinned DR (11.91.20630) predates
   APX ([#6226](https://github.com/DynamoRIO/dynamorio/issues/6226) open);
   when `DR_VERSION` is ever bumped past an APX-capable release, this
   assertion must be revisited — leave a comment naming the issue.
6. Wire a `pintool-apx-test` target into mk/pintool.mk and append it to
   `pintool-test`'s live branch; the decode assertion (step 2) runs
   unconditionally in the lane, the execution halves behind the CPUID gate.

**Code.** Honesty note to encode in the target's comment: XED *decode* of APX
is verified; whether Pin 4.2 can *instrument* APX code is empirically
unconfirmed (the 4.x manual never mentions APX — Research notes). This
fixture IS the probe: if Pin fails to instrument on APX silicon, record the
finding in a `docs/internal/analysis/` note (the
`dr-attach-probe-findings.md` precedent — a no-go is a valid result), keep
the decode assertion, and gate the execution half off with the recorded
reason.

**Tests.** On any x86-64 host/lane: `pin_apx_decode` prints
`ok - APX_ROUTINE decodes as APX (XED)`. On APX silicon: full TAP with the
Pin-complete-trace and DR-negative-control lines. Elsewhere the execution
half prints the hardware-gate SKIP.

**Docs.** Internal-only; changelog in T9.

**Done when.**
- The decode assertion passes inside `make docker-pintool` on a non-APX CI
  host (ungated).
- On APX silicon (when available): Pin traces the APX routine completely AND
  the DR replay of the same bytes fails the completeness check — both in one
  lane run.
- On non-APX hosts the execution half self-skips with the printed hardware
  reason.

### T9 — changelog + stale-claim corrections in source docs  (S, depends on: T6 for the changelog wording)

**Goal.** User-visible changelog entry, and the two stale VNNI claims in the
source docs corrected so no future reader re-derives the Pin case from a
refuted premise.

**Steps.**
1. Append under `## [Unreleased]` / `Added` in
   [CHANGELOG.md](../../../CHANGELOG.md) (line 7): one entry for the Pin
   trace lane (`docker-pintool` / `pintool-test`, pinned digest-gated Pin
   4.2, offset parity with the DynamoRIO and single-step backends, APX
   fixture).
2. In
   [2026-07-17-intel-pin-vs-dynamorio.md](../analysis/2026-07-17-intel-pin-vs-dynamorio.md)
   line 95, replace "VNNI programs still break ([DR #5440][dr5440])" with a
   corrected statement: #5440 was closed as completed 2022-04-25 (PR #5444);
   the pinned DR 11.91.20630 decodes VNNI; APX (#6226) is the live gap.
3. Same correction in
   [intel-pin-capabilities-plan.md](../plans/intel-pin-capabilities-plan.md)
   line 125 ("[DR #5440][dr5440] VNNI still breaks") — reword to cite #5440
   as fixed and rest PIN-2's case on #6226 alone.
4. Run `make docs` (or `make docker-docs`) — `docs/internal/**` is excluded
   from the published build, but the changelog edit must not introduce a
   Sphinx warning (`-W` fail-on-warning).

**Code.** None.

**Tests.** `make docs` exits 0. `git grep -n "VNNI still break"` returns
nothing.

**Docs.** This task is the docs task. No published user-facing page: the tier
ships no public header or binding (shared constraint), so `docs/` proper gains
nothing beyond the changelog line.

**Done when.**
- Changelog entry present under `## [Unreleased]`.
- Both stale VNNI claims corrected; `make docs` green.

## Task order & parallelism

```
T1 ──> T2 ──> (CMD flip in T6)
 └───> T3 ──> T4 ──> T5 ──> T6 ──> T7
                             └───> T8
T9 (steps 2–3 anytime; step 1 after T6 names the targets)
```

- **Critical path:** T1 → T3 → T4 → T5 → T6 (the parity lane).
- T2 (docker image) and T3 (tool scaffolding) are independent after T1 — two
  people can take them concurrently.
- T7 and T8 are independent of each other after T6.
- T9's doc corrections (steps 2–3) touch nothing else and can land first.

## Constraints & gates

- **License.** Pin is proprietary freeware under the Intel Simplified
  Software License (Oct 2022): redistribution of the unmodified binary with
  notice is allowed; modification/reverse-engineering is not. Therefore the
  kit is **test-lane only**: fetched and digest-verified at build/test time,
  never linked into `libasmtest`/`libasmtest_emu`, never entered into
  `scripts/collect-licenses.sh`'s shipped-package NOTICE, no public header
  (zero diff under `include/`), no bindings. The bundled XED is Apache-2.0;
  the kit's `source/tools/Config` scaffolding carries MIT SPDX headers, so
  deriving `pintool/makefile` from it is fine.
- **Pinning.** `PIN_VERSION=4.2-99776-g21d818fa2` with a SHA-256 line in
  `scripts/third-party-digests.txt`; any bump goes through
  `refresh-thirdparty-digests.sh` and a reviewed manifest diff. Intel
  publishes no official hash (see Research notes) — the digest is
  self-computed, so T1 requires an independent recomputation before pinning.
- **Real gates (self-skip with a printed reason).**
  - Non-x86-64 hosts and non-Linux hosts: Pin's pinned kit is x86-64
    gcc-linux; `pintool-*` targets skip exactly like `mk/cli.mk`'s
    `CLI_ARCH` gate. In particular, everything self-skips on this repo's
    macOS-arm64 dev host — end-to-end validation happens via
    `make docker-pintool` on an x86-64 Linux Docker host or CI runner (do
    not claim a Rosetta/qemu `--platform linux/amd64` run as validation;
    Pin under CPU emulation is not a supported configuration).
  - APX silicon for T8's execution halves (CPUID `APX_F`). The ungated
    decode assertion keeps the fixture partially exercised everywhere.
- **What to record when a gate blocks validation:** the SKIP line must name
  the gate (architecture / APX silicon) and, for T8, the run must still
  execute the XED decode assertion so every lane run proves the fixture's
  bytes are genuinely APX.
- **Missing installable deps are added, not skipped** (CLAUDE.md): Pin
  itself is installable, so `pintool-test` on a gate-passing host fetches
  the kit via `scripts/fetch-pin.sh` rather than skipping.

## Research notes (verified 2026-07-17)

- **Pin kit.** Current release is Pin 4.2 (build 99776-g21d818fa2), Linux
  tarball
  <https://software.intel.com/sites/landingpage/pintool/downloads/pin-external-4.2-99776-g21d818fa2-gcc-linux.tar.gz>
  (HTTP 200, 37,678,990 bytes, Last-Modified 2026-03-16). "4.2 is newest"
  rests on the [Wikipedia infobox](https://en.wikipedia.org/wiki/Pin_(computer_program))
  (latest stable 4.2, 2026-03-15), the [AUR `pin` package](https://aur.archlinux.org/packages/pin)
  (4.2-1, same URL), and the absence of any 4.3 doc URL — Intel's own
  [downloads page](https://www.intel.com/content/www/us/en/developer/articles/tool/pin-a-binary-instrumentation-tool-downloads.html)
  403s to automated fetch. Self-computed SHA-256 (2026-07-18, otherwise
  uncorroborated — recompute independently in T1):
  `194a2cec51678203452ece0d9e8cbb1819eb6e1221f0341091c49248f384d869`. Intel
  publishes no official hashes (also noted in
  [nixpkgs #384750](https://github.com/NixOS/nixpkgs/issues/384750)).
  Fallback if 4.2 ever regresses: the last 3.3x kit, Pin 3.31
  (98869-gfa6f126a8), tarball live at the same host, SHA-256
  `82216144e3df768f0203b671ff48605314f13266903eb42dac01b91310eba956`
  (self-computed, same caveat) — its bundled XED (v2024.05.20) already
  defines the APX extension enums.
- **License (read inside both extracted kits):**
  `licensing/intel-simplified-software-license.txt` — Intel Simplified
  Software License (Version October 2022), binary-only, redistribution with
  notice, no modification. `licensing/third-party-programs.txt` — bundled
  XED is Apache-2.0. The `source/tools/Config/*` and MyPinTool makefiles
  carry SPDX MIT headers. Pin 4.2 additionally ships `licensing/LGPL.txt`
  and a `bindings/` (Python) dir — unused here.
- **Out-of-kit tool build (verified in both kits):** kit ships
  `source/tools/Config/{makefile.config, makefile.unix.config,
  makefile.default.rules, ...}` plus the `source/tools/MyPinTool/makefile`
  template whose lines 12–20 document the protocol: set `PIN_ROOT`, include
  `makefile.config`, your `makefile.rules`, then `makefile.default.rules` —
  i.e. `make PIN_ROOT=/opt/pin obj-intel64/mytool.so`.
  `makefile.unix.config` (4.2) sets
  `XED_ROOT := $(PIN_ROOT)/extras/xed-$(TARGET)` and builds tools under
  PinCRT: `-DPIN_CRT=1`, `-nostartfiles -nodefaultlibs -nostdlib`,
  `-fno-exceptions -fno-rtti -fno-stack-protector`,
  `-Wl,--hash-style=sysv`. User guides: [3.31](https://software.intel.com/sites/landingpage/pintool/docs/98861/Pin/doc/html/index.html),
  [4.0](https://software.intel.com/sites/landingpage/pintool/docs/99633/Pin/doc/html/index.html),
  [4.1](https://software.intel.com/sites/landingpage/pintool/docs/99687/Pin/doc/html/index.html).
- **XED / APX.** Pin 4.2 bundles XED v2026.02.17; Pin 3.31 bundles XED
  v2024.05.20, whose `xed-extension-enum.h` lines 141–142 already define
  `XED_EXTENSION_APXEVEX` / `XED_EXTENSION_APXLEGACY`. Upstream
  [XED releases](https://github.com/intelxed/xed/releases): first APX
  support v2023.08.21; complete APX encoder v2023.12.19; APX Rev 8.0
  alignment v2026.03.18; latest v2026.07.15. **Caveat:** only XED-level
  *decode* of APX is confirmed for Pin; whether Pin 4.2 *instruments* APX
  code is unverified (the bundled manual contains no "APX" mention) — T8 is
  the empirical probe.
- **DynamoRIO issues.**
  [#5440](https://github.com/DynamoRIO/dynamorio/issues/5440) (AVX-512
  VNNI): CLOSED as completed 2022-04-25 via
  [PR #5444](https://github.com/DynamoRIO/dynamorio/pull/5444) — the repo's
  pinned DR 11.91.20630 post-dates the fix.
  [#6226](https://github.com/DynamoRIO/dynamorio/issues/6226) (Intel APX):
  OPEN as of 2026-07-18 (filed 2023-07-24, no activity), enumerating the
  missing pieces (16 EGPRs, REX2, PUSH2/POP2, CCMP/CTEST, NF forms); no
  other open DR issue matches APX/AVX10. So the pinned DR decodes VNNI but
  rejects all APX encodings — the exact boundary T7/T8 assert.

## Out of scope

- **SDE future-ISA lane (PIN-1)** — running whole `TEST()` suites under
  `sde64 -future`, and the Pin-on-SDE stacking that would lift T8's APX
  silicon gate: [pin-sde-future-isa-lane.md](pin-sde-future-isa-lane.md).
- **Pin probe-mode arg/return capture (PIN-3)** — builds on this doc's
  `Dockerfile.pintool` + `scripts/fetch-pin.sh` substrate:
  [pin-probe-mode-capture.md](pin-probe-mode-capture.md).
- **libdft64 differential taint oracle (PIN-4)** — same substrate:
  [pin-libdft-taint-oracle.md](pin-libdft-taint-oracle.md).
- **The DR trace/taint tiers themselves** (this doc only reads them for
  parity): drtrace correctness work lives with its own lanes; DR external
  attach for taint is already landed (`docker-dataflow-attach`) and is not
  re-scoped here.
- **Hardware-trace backends** (Intel PT / CoreSight / AMD LBR):
  [intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md),
  [coresight-live-decode.md](coresight-live-decode.md),
  [amd-branchsnap-lbr-docs.md](amd-branchsnap-lbr-docs.md).
