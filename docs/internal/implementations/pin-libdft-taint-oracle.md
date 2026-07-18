# libdft64 differential oracle for the DynamoRIO taint tier — implementation

> **Sources.** Actioned from
> [intel-pin-capabilities-plan.md](../plans/intel-pin-capabilities-plan.md) (track
> **PIN-4**), [data-flow-capture.md](../analysis/data-flow-capture.md) (the
> libdft coverage-gap ground truth), and the archived
> [dynamorio-taint-tier-plan.md](../archive/plans/dynamorio-taint-tier-plan.md)
> (the tier being oracled). Written 2026-07-17. If this doc and a source
> disagree, this doc wins (sources may be stale); if the CODE and this doc
> disagree, re-verify before implementing.

## Why this work exists

The DynamoRIO in-band taint tier is fully landed: it seeds a byte-granular tag
shadow, propagates `dst_tag = ∪ src_tags` inline, and reports sink hits
([src/dataflow_dr_client_inlined.c](../../../src/dataflow_dr_client_inlined.c)
built `-DASMTEST_TAINT`). Today its only independent check is an **offline**
emulator/Capstone forward slice. This work adds a **second, live, independently
implemented byte-level taint engine** — [libdft64][libdft64] on Intel Pin — runs
the *same* seed/sink fixtures through it, and asserts the two engines agree
byte-for-byte on the general-purpose / integer-memory subset both cover. The
user-visible outcome is a `make docker-taint-oracle` lane that cross-validates
the shipped DR taint client against the canonical Pin taint engine and
classifies every divergence (a real DR-client bug, a known libdft coverage gap,
or a modelling difference) instead of trusting one home-grown implementation
against one home-grown oracle.

This is an **oracle**, not a capability DR lacks. The DR taint tier already holds
the ground — in-band whole-process taint including external attach
(`Dockerfile.taint-attach`, `docker-dataflow-attach`, landed 2026-07-14). Do not
scope this doc as "Pin does something DR can't"; scope it as "a second engine
that must independently reproduce the DR client's sink set."

## What already exists (verified 2026-07-17)

The substrate this doc builds on is entirely landed and green. Before touching
anything, prove the baseline:

- **The seed/sink ABI.** [include/asmtest_taint.h](../../../include/asmtest_taint.h)
  defines `at_taint_seed_t` (`base`/`len`/`color`, line 48), `at_taint_hit_t`
  (`off`/`ea`/`seed_off`/`tag`/`kind`/`depth`, line 60), and `at_taint_report_t`
  (append/truncate discipline, line 76). This is the **projection target**: the
  libdft Pintool must fill `at_taint_hit_t` records so the two reports diff
  directly. The header is `<stdint.h>`/`<stddef.h>`-only by design.
- **The seed/sink markers.** [src/dataflow_dr.c:84](../../../src/dataflow_dr.c#L84)
  exports `asmtest_dr_taint_seed_marker(base, len, color)` and
  [:94](../../../src/dataflow_dr.c#L94) `asmtest_dr_taint_sink_marker(report)`;
  their symbol names are `AT_TAINT_SEED_SYM` /`AT_TAINT_SINK_SYM`
  ([asmtest_taint.h:41](../../../include/asmtest_taint.h#L41)). Fixtures are linked
  `-rdynamic` ([mk/native-trace.mk:271](../../../mk/native-trace.mk#L271)) so the
  marker PCs are resolvable — the DR client resolves them by
  `dr_get_proc_address` ([dataflow_dr_client_inlined.c:2201](../../../src/dataflow_dr_client_inlined.c#L2201)).
  **The libdft Pintool reuses the exact same exported symbols** — it hooks them by
  name from the Pin side, so the fixtures need no change.
- **The DR-side fixtures and oracle harness.**
  [examples/dr_taint.c](../../../examples/dr_taint.c) holds the GP/integer-memory
  fixtures (byte arrays `taint_chain`, `taint_sink_chain`, `taint_heapstore`,
  `taint_callarg`, `taint_memlen` — verified at
  [:91](../../../examples/dr_taint.c#L91) onward) and the app-side def-use BFS
  (`defuse_depth`, `fill_seed_and_depth`) that fills each hit's `seed_off`/`depth`.
  [examples/dr_taint_simd.c](../../../examples/dr_taint_simd.c) is the XMM/YMM
  analog — **the named-skip subset for this doc**.
- **The DR taint driver.**
  [src/dataflow_dr.c:433](../../../src/dataflow_dr.c#L433)
  `asmtest_dataflow_dr_taint_run(...)` runs a fixture natively under DR with the
  taint client, seeds `[base,len)`, and fills a `at_taint_report_t`. This is the
  producer whose sink set libdft must match.
- **The cross-process shm precedent.**
  [include/asmtest_taint_shm.h](../../../include/asmtest_taint_shm.h) documents the
  **read-by-offset / ignore-stored-pointers** rule for a POSIX shm results channel
  under launch-under-DBI. Mirror that rule for the libdft channel (the Pintool and
  the app are the same process under Pin, but the diff validator is separate).
- **The pinning machinery.**
  [scripts/fetch-dynamorio.sh](../../../scripts/fetch-dynamorio.sh) is the
  download→verify-digest→capture-license pattern to mirror for Pin;
  [scripts/lib-thirdparty.sh](../../../scripts/lib-thirdparty.sh) provides
  `tp_digest`/`tp_sha256`; [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt)
  holds the pinned digests (format `<kind> <name> <version> <algo>:<value>`).
  [Dockerfile.taint-native](../../../Dockerfile.taint-native) is the taint-lane
  Docker pattern to mirror; [mk/docker.mk:250](../../../mk/docker.mk#L250) is the
  `docker-taint-native` rule to mirror.

**Prove the baseline is green (do this first):**

```
make docker-taint-native
```

Expect a run ending with the seeded oracle diff, negative controls, and the SIMD
lane all printing TAP `ok` lines (the container installs DynamoRIO + Capstone +
Unicorn and runs `dr-taint-native-test`, `dr-taint-simd-test`, and others — see
[Dockerfile.taint-native:57](../../../Dockerfile.taint-native#L57)). If that lane
is red, stop — nothing here can be validated on a broken taint tier.

**Confirmed absent (this doc adds the libdft side of it):** `grep -rn libdft`
finds only doc prose, never code; there is no `docker-taint-oracle` target and
no `pin`/`libdft64` line in `scripts/third-party-digests.txt`. `scripts/fetch-pin.sh`
is also absent today, but this doc does **not** author it — it is
[pin-xed-trace-tier.md#T1](pin-xed-trace-tier.md)'s to add, and this doc consumes
it (T1). This doc adds `scripts/fetch-libdft.sh`, the `Dockerfile.taint-oracle`
lane, and the distinct 3.20 `pin` + `libdft64` manifest rows.

## Tasks

### T1 — Pin the Pin 3.20 kit and the libdft64 checkout, digest-gated  (M, depends on: none)

**Goal.** The exact pinned Pin 3.20 kit and libdft64 commit are fetched, verified
against `scripts/third-party-digests.txt`, fail loudly on mismatch, and their
license texts vendored. The Pin kit is fetched by
[pin-xed-trace-tier.md#T1](pin-xed-trace-tier.md)'s `scripts/fetch-pin.sh` — this
doc does **not** author that script; it invokes it with a 3.20
`PIN_VERSION`/`PIN_URL` override. libdft64 is fetched by this doc's own
`scripts/fetch-libdft.sh`, mirroring
[fetch-dynamorio.sh](../../../scripts/fetch-dynamorio.sh) as it does for
DynamoRIO.

**Why Pin 3.20 specifically.** libdft64's README names its tested kit exactly —
`pin-3.20-98437-gf02b61307-gcc-linux` — and its `install_pin.sh` downloads that
tarball. There is no upstream evidence of libdft64 building against a newer kit
(issue [#23][libdft-issue23] reports only linker warnings against 3.27, no
maintainer fix). Treat 3.20 as the compatible pin; do **not** reuse the newer
APX kit that [pin-xed-trace-tier.md](pin-xed-trace-tier.md) pins for its own track
(see [Constraints & gates](#constraints--gates) for the two-kit resolution).

**Steps.**
1. **Consume** [pin-xed-trace-tier.md#T1](pin-xed-trace-tier.md)'s
   `scripts/fetch-pin.sh` — do **not** re-author it. That script is env-keyed on
   `PIN_VERSION`/`PIN_URL`, but its default URL template is
   `pin-external-${PIN_VERSION}-gcc-linux.tar.gz`, which cannot produce the 3.20
   URL from `PIN_VERSION` alone (the 3.20 tarball has **no** `external` segment).
   So invoke it with **both** overrides —
   `PIN_VERSION=3.20-98437-gf02b61307` **and**
   `PIN_URL=https://software.intel.com/sites/landingpage/pintool/downloads/pin-3.20-98437-gf02b61307-gcc-linux.tar.gz`.
   The script sources `lib-thirdparty.sh`, downloads to a cache,
   `tp_digest tarball-sha256 pin "$PIN_VERSION"` against the 3.20 row this doc
   adds (step 3), compares `tp_sha256`, extracts, captures the Pin license, and
   echoes `PIN_ROOT` — all keyed on the passed version, so the one script serves
   both kits. `pin-xed-trace-tier.md#T1` is a hard prerequisite: it is the sole
   author of `fetch-pin.sh`, and this doc no longer bootstraps the script itself.
2. Create `scripts/fetch-libdft.sh`: `git clone` [AngoraFuzzer/libdft64][libdft64]
   into a cache, `git checkout` the pinned commit, verify `git rev-parse HEAD`
   equals `tp_digest git-commit libdft64 <shortsha>` (mirror how Keystone/Capstone
   are git-commit-pinned in the manifest), echo `LIBDFT_ROOT`.
3. Add two lines to
   [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt):
   ```
   tarball-sha256  pin       3.20-98437-gf02b61307  sha256:ca2f542eee2013471961bb683d06ccb20ef5dd8ed0d02537cf4d47f09bd616bf
   git-commit      libdft64  20804d5                commit:20804d5bae5d8aed31a71761b1a1149e35a0da95
   ```
   The `pin` row is keyed on the 3.20 **version**, so it is a distinct manifest
   line that coexists with the 4.2 `pin` row
   [pin-xed-trace-tier.md#T1](pin-xed-trace-tier.md) adds — the shared
   `fetch-pin.sh` looks up whichever version it is invoked with. These two digest
   lines (the 3.20 `pin` row + the `libdft64` row) are this doc's only
   contribution to the manifest. Both digests are from the
   [Research notes](#research-notes-verified-2026-07-17) below (Pin 3.20 tarball
   self-hashed 2026-07-18; libdft64 master HEAD 2025-02-21).
4. Vendor licenses under [licenses/](../../../licenses/): the shared
   `fetch-pin.sh` captures the Pin kit's
   `licensing/intel-simplified-software-license.txt` on first fetch (to
   `licenses/Pin-3.20-98437-gf02b61307.txt`, keyed on the passed `PIN_VERSION` —
   the sibling's naming, not this doc's to choose; Intel Simplified Software
   License, binary-only); this doc's `fetch-libdft.sh` captures the libdft64
   `LICENSE`/`COPYING` → `licenses/libdft64.txt`. Update
   [licenses/README.md](../../../licenses/README.md)'s table with both rows and
   note both are **test/oracle-only, never bundled into a shipped package** (see
   the license posture in [Constraints & gates](#constraints--gates)).

**Code.** One POSIX `sh` script (`scripts/fetch-libdft.sh`; `fetch-pin.sh` is
pin-xed-trace-tier.md#T1's, consumed not re-authored); two manifest lines; the
libdft64 vendored license (the Pin license is captured by the shared
`fetch-pin.sh`); one README table edit. No C.

**Tests.** No unit test — these are fetch scripts. Verification is that invoking
the shared script for the 3.20 kit
(`PIN_VERSION=3.20-98437-gf02b61307 PIN_URL=https://software.intel.com/sites/landingpage/pintool/downloads/pin-3.20-98437-gf02b61307-gcc-linux.tar.gz sh scripts/fetch-pin.sh`)
prints a `PIN_ROOT` and, if you corrupt the 3.20 manifest digest, exits non-zero
with an `integrity check FAILED` message (mirror `fetch-dynamorio.sh`'s failure
path); and that `sh scripts/fetch-libdft.sh` checks out the pinned commit. The
real end-to-end check is T2 building on top of these.

**Docs.** `licenses/README.md` (above). No user-facing docs — internal test lane.

**Done when.**
- The shared `scripts/fetch-pin.sh` (pin-xed-trace-tier.md#T1), invoked with
  `PIN_VERSION=3.20-98437-gf02b61307` and the matching no-`external` `PIN_URL`,
  downloads, digest-verifies against the 3.20 `pin` row this doc added, and
  prints a `PIN_ROOT`; tampering with that pinned digest makes it exit non-zero.
- `sh scripts/fetch-libdft.sh` checks out the pinned commit and refuses a
  mismatched HEAD.
- `licenses/Pin-3.20-98437-gf02b61307.txt` and `licenses/libdft64.txt` exist;
  the README table lists both and marks them oracle-only.

### T2 — Dockerfile.taint-oracle: build libdft64 against the pinned Pin  (M, depends on: T1)

**Goal.** A `Dockerfile.taint-oracle` + `make docker-taint-oracle` lane that
fetches Pin 3.20, builds libdft64 against it, and (for now) prints a build-success
line — the foundation every later task rides on.

**Steps.**
1. Create `Dockerfile.taint-oracle` mirroring
   [Dockerfile.taint-native](../../../Dockerfile.taint-native): `FROM
   ubuntu:24.04`, apt `build-essential cmake pkg-config curl ca-certificates
   xz-utils git clang` plus DynamoRIO + Capstone + libunicorn (the diff needs the
   DR client *and* the emulator oracle in the same image). libdft64's tested
   platform is Ubuntu 20.04; if a build break traces to the toolchain, pin
   `FROM ubuntu:20.04` for this image only and record why — a toolchain gap is a
   dependency to add, not a reason to narrow the lane
   ([CLAUDE.md](../../../CLAUDE.md)).
2. `ARG PIN_VERSION=3.20-98437-gf02b61307` + `ARG PIN_URL=...` + curl/extract to
   `/opt/pin`, `ENV PIN_ROOT=/opt/pin` — the same `ARG`+curl block
   `Dockerfile.taint-native` uses for `DR_VERSION`. **Do not** unify this with the
   APX kit `pin-xed-trace-tier` pins; this lane needs 3.20 (see gates).
3. Clone + checkout libdft64 at the pinned commit into `/opt/libdft64`. Build it
   against the external kit: `cd /opt/libdft64 && PIN_ROOT=/opt/pin make` — the
   top-level Makefile builds `src/` (→ `src/obj-intel64/libdft.a`) then the tools
   with `TARGET=intel64` using Pin's own build infra (verified from upstream
   [Makefile][libdft-mk] / [INSTALL.md][libdft-install]).
4. `COPY . .` the repo, build Capstone from source
   (`sh scripts/build-capstone.sh && ldconfig`) as `Dockerfile.taint-native` does,
   and set the `CMD` to run `make dr-taint-oracle-test` (the target T5 adds; until
   then, a placeholder that echoes the libdft build path proves the image).
5. Add `docker-taint-oracle` to [mk/docker.mk](../../../mk/docker.mk) mirroring the
   `docker-taint-native` rule at [:250](../../../mk/docker.mk#L250). Pass
   `--build-arg DR_VERSION=$(DR_VERSION)` exactly as that rule does — this image
   also installs DynamoRIO (step 1) for the DR client, so DR must be pinned to the
   repo-wide `$(DR_VERSION)` rather than silently falling back to the Dockerfile's
   `ARG` default:
   ```make
   .PHONY: docker-taint-oracle
   docker-taint-oracle:
   	$(DOCKER) build $(_docker_plat) -f Dockerfile.taint-oracle \
   	  --build-arg BASE=$(DOCKER_BASE) --build-arg DR_VERSION=$(DR_VERSION) \
   	  -t asmtest-taint-oracle .
   	$(DOCKER) run --rm $(_docker_plat) asmtest-taint-oracle
   ```

**Code.** One Dockerfile; one `docker.mk` rule.

**Tests.** `make docker-taint-oracle` builds the image without error and the
container prints `libdft.a` present + the tool `.so` built. A failure looks like a
non-zero `make` inside the image (a link error against Pin's PinCRT, or a missing
`libdft.a`); a pass is the build-success line.

**Docs.** Internal-only. Add the target to the `make help` group (T7).

**Done when.**
- `make docker-taint-oracle` builds green and prints libdft64's `libdft.a` and the
  oracle tool `.so` were produced against the pinned Pin 3.20.
- On a non-x86 host the lane self-skips with a printed reason (Pin is x86-only —
  gate on `uname -m`, see gates).

### T3 — The libdft64 oracle Pintool: seed from markers, project sinks onto `at_taint_hit_t`  (L, depends on: T2)

**Goal.** A Pintool `pintools/libdft_oracle/oracle.cpp` that, running under Pin
with libdft64 linked, seeds `[base,len)` when the app calls the seed marker and
appends one `at_taint_hit_t` per watched sink, over a POSIX shm channel, in the
exact shape the DR client produces.

**Steps.**
1. Create `pintools/libdft_oracle/` from libdft64's `tools/` template (its
   `makefile.rules` sets `TOOL_ROOTS`, `LIBDFT_PATH=../src/obj-intel64/`, and
   `TOOL_LIBS += -L$(LIBDFT_PATH) -ldft`). Copy that makefile pattern and add one
   `TOOL_ROOTS := oracle`. Build with `make PIN_ROOT=/opt/pin
   obj-intel64/oracle.so`. The Pin tool-build scaffolding carries SPDX MIT headers,
   so a derived tool makefile is fine to ship (Research notes).
2. **Resolve the seed/sink markers by name.** In the tool's `main`, register an
   `IMG_AddInstrumentFunction` that, for the app image, finds `RTN` for
   `AT_TAINT_SEED_SYM` ("asmtest_dr_taint_seed_marker") and `AT_TAINT_SINK_SYM`
   ("asmtest_dr_taint_sink_marker") and inserts `RTN_InsertCall(... IPOINT_BEFORE)`
   reading the SysV arg registers (`IARG_FUNCARG_ENTRYPOINT_VALUE, 0/1/2`). This
   mirrors how the DR client resolves the same PCs — the app is unchanged.
3. **Seed callback.** `on_seed(base, len, color)`: paint `[base, base+len)` in
   libdft's tagmap. libdft64's tag API sets a byte tag via its `tagmap_setb` /
   `tag_t` interface (`src/`); set every byte in range to a tag carrying the
   seed's color bit. Keep the mapping simple: libdft's tag is a byte set; encode
   the `at_tag_t` bit0=tainted + up-to-7 colors 1:1.
4. **Sink callbacks.** `on_sink(report_ptr)`: record the app's report pointer;
   then arm the three sink kinds the DR client watches
   ([dataflow_dr_client_inlined.c:1926](../../../src/dataflow_dr_client_inlined.c#L1926)),
   using libdft's instruction hooks over the registered code region:
   - **kind 1 — branch condition.** At each conditional branch in-region, read the
     EFLAGS taint. libdft64 **ignores eflags** (README limitation), so flag-carried
     flow is a known gap — handle it by watching the *register that defined the
     flag* instead (the branch's source operand), and record the divergence class
     if the two engines disagree here (T5/T6). Emit a hit with `kind = 1`.
   - **kind 2 — call arg.** At each `call` in-region, test the taint of arg0
     (`rdi`); emit `kind = 2`.
   - **kind 0 — mem-len.** At each `rep movs`, test the count register (`rcx`);
     emit `kind = 0`.
   For each fired sink write one `at_taint_hit_t` into the shm channel: `off` =
   the sink instruction's offset from the region base (the tool learns the base
   from a third marker or from the seed context — see step 5), `ea` = the sink
   operand EA (0 for reg/branch), `tag` = the union observed, `kind` as above.
   Leave `seed_off`/`depth` = 0 — the app-side def-use BFS fills them (the ABI
   defers them: [asmtest_taint.h:57](../../../include/asmtest_taint.h#L57)).
5. **The shm channel.** Add `include/asmtest_taint_oracle_shm.h` mirroring
   [asmtest_taint_shm.h](../../../include/asmtest_taint_shm.h)'s **cross-process
   rule** (the tool writes via a producer-space pointer; the consumer reads
   `hits[]` + `hits_len`/`hits_total`/`truncated` by **offset**, never the stored
   `.hits` pointer). Keep it slim — libdft produces no value trace, so no embedded
   `at_drval_t`:
   ```c
   #define AT_ORACLE_SHM_NAME "/asmtest_taint_oracle"
   #define AT_ORACLE_HITS_CAP 16
   typedef struct at_oracle_shm {
       volatile uint32_t done;   /* 0 until the fixture returns; then 1 */
       uint32_t pad0;
       int64_t  result;          /* the fixture's return value (liveness) */
       uint64_t region_base;     /* so the tool can report region offsets */
       at_taint_report_t report; /* .hits -> hits[] in PRODUCER space */
       at_taint_hit_t hits[AT_ORACLE_HITS_CAP];
   } at_oracle_shm_t;
   ```
   The tool `shm_open`s `AT_ORACLE_SHM_NAME` (override via a `-shm` knob), maps it,
   and appends with the same append/truncate discipline as `at_taint_report_t`.

**Code.** One C++ Pintool (`oracle.cpp`), one tool makefile, one new header
`include/asmtest_taint_oracle_shm.h`. The header stays `<stdint.h>`-only so the C
app and the C++ tool agree on the layout.

**Tests.** Standalone: run `pin -t obj-intel64/oracle.so -- <a tiny hand-built
seed→sink binary>` and confirm the tool writes a non-empty `hits[]` with the
expected `kind`. Full validation is T5 (the differential diff). A failure at this
stage looks like an empty `hits[]` (markers not resolved — check the app is
`-rdynamic` and the symbol names match `AT_TAINT_*_SYM`) or a Pin decode error.

**Docs.** Internal-only — this is oracle machinery, not a shipped tier symbol.

**Done when.**
- `pin -t oracle.so -- <seed/sink probe>` writes at least one `at_taint_hit_t`
  with the correct `off`/`kind` into the shm channel.
- The tool resolves both markers by name and paints/reads libdft tags without a
  Pin fault.

### T4 — The oracle driver harness: run the shared fixtures under Pin  (M, depends on: T3)

**Goal.** `examples/pin_taint.c` runs the *same* GP/integer-memory fixtures as
[dr_taint.c](../../../examples/dr_taint.c), natively, under `pin -t oracle.so`,
and leaves a completed shm channel for the diff.

**Steps.**
1. **Share the fixtures.** Extract the fixture byte arrays + their metadata
   (`SINK_OFF`, `SINK_DEPTH`, `CALLARG_OFF`, `MEMLEN_OFF`, `MEMLEN_DEPTH`, the
   region layouts) from [dr_taint.c](../../../examples/dr_taint.c) into a new
   `examples/taint_fixtures.h`, and `#include` it from both `dr_taint.c` (small
   refactor — replace the inline `static const uint8_t taint_chain[] = {...}` at
   [:91](../../../examples/dr_taint.c#L91) with the header) and `pin_taint.c`. A
   differential oracle is only meaningful on **byte-identical inputs**, so a single
   source of truth for the fixtures is mandatory, not optional.
2. `pin_taint.c` (mode-selected by `argv[1]`, matching dr_taint's modes:
   `seeded`/`sink`/`heapstore`/`highbyte`/`callarg`/`memlen` + negatives): map the
   fixture bytes executable (`asmtest_exec_alloc`, as dr_taint does), `shm_open` +
   map the `at_oracle_shm_t` channel, write `region_base`, call
   `asmtest_dr_taint_seed_marker(buf, len, color)` (unseeded negative → skip the
   seed call), register the report via `asmtest_dr_taint_sink_marker(&shm->report)`,
   run the fixture natively, set `shm->result` + `shm->done = 1`, and exit. Under
   `pin` the tool observes all of this and fills `hits[]`.
3. Link `pin_taint` `-rdynamic` (so the tool resolves the marker PCs) with the
   same marker TU the DR harness uses — the markers live in
   [dataflow_dr.c](../../../src/dataflow_dr.c#L84) under `-DASMTEST_TAINT`. Add a
   `pin-taint` build rule to [mk/native-trace.mk](../../../mk/native-trace.mk)
   beside the `dr_taint` rules at [:264](../../../mk/native-trace.mk#L264).
4. Self-skip discipline: if `PIN_ROOT` is unset or `pin` is not on `PATH`, the
   run prints `# SKIP: Pin not found` and exits 0 — mirror `dr-taint-native-test`'s
   `ifndef DR_AVAILABLE` skip ([mk/native-trace.mk:286](../../../mk/native-trace.mk#L286)).

**Code.** `examples/pin_taint.c`; new `examples/taint_fixtures.h`; a `dr_taint.c`
include-refactor (byte arrays only — leave the harness logic untouched); a
`native-trace.mk` build rule.

**Tests.** After this task the run
`pin -t oracle.so -- ./build/pin_taint seeded` produces a completed shm channel;
assert `shm->done == 1` and `shm->result` equals the fixture's known return. A
regression here is a hang (fixture faulted under Pin) or `done` never set.

**Docs.** Internal-only.

**Done when.**
- `./build/pin_taint <mode>` under Pin completes for every DR-side mode and leaves
  a populated `at_oracle_shm_t`.
- The refactor leaves `make dr-taint-native-test` **byte-identically green** (the
  fixtures moved to a header must not change a single fixture byte — the DR lane is
  the proof).

### T5 — The differential diff: assert DR ≡ libdft on the GP/integer subset, classify divergences  (M, depends on: T4)

**Goal.** `make dr-taint-oracle-test` runs each shared fixture under **both** the
DR client and libdft64 and asserts the two sink sets agree byte-for-byte on the
covered subset; every disagreement is classified and recorded, never papered over.

**Steps.**
1. Add `examples/taint_oracle_diff.c` (or extend `pin_taint.c` with a `diff`
   mode): for a given fixture mode, obtain **both** reports — the DR one from
   `asmtest_dataflow_dr_taint_run(...)` ([dataflow_dr.c:433](../../../src/dataflow_dr.c#L433))
   and the libdft one drained from the `at_oracle_shm_t` channel after the
   `pin_taint` run — then compare. Both reports fill `at_taint_hit_t`, so the
   comparison is field-wise on the **client-filled** fields (`off`, `ea`, `tag`
   union compatibility, `kind`); `seed_off`/`depth` are app-side, so fill them the
   same way for both via the existing `defuse_depth`/`fill_seed_and_depth`
   ([dr_taint.c](../../../examples/dr_taint.c)) before comparing, or exclude them.
2. **Agreement assertion.** For each covered mode
   (`seeded`/`sink`/`heapstore`/`highbyte`/`callarg`/`memlen`): assert the sink
   sets are equal as multisets on `{off, ea, kind}` and the `tag` unions are
   compatible (same tainted bit / color set). A pass is a TAP `ok` per mode; a
   failure prints both hit lists side by side.
3. **Divergence classification (mandatory — the point of the oracle).** When a
   mode diverges, classify into exactly one bucket and record it:
   - **DR-client bug** — DR reports a hit libdft (a mature engine) does not, or
     vice-versa, on an operand class both cover (GP reg, integer memory). This
     fails the lane red.
   - **libdft coverage gap** — the divergence lands in a class libdft documents as
     unsupported (eflags, implicit flow, x87/FPU, AVX-512 ZMM, ternary). This is an
     *allowed, named* skip (T6), never a blanket pass.
   - **modelling difference** — e.g. the call-arg sink is calling-convention-based
     (a direct `call` does not machine-read `rdi`), so the two engines legitimately
     attribute it differently; record the boundary in a comment + the changelog.
4. Wire `dr-taint-oracle-test` into [mk/native-trace.mk](../../../mk/native-trace.mk)
   beside `dr-taint-native-test`, with the same `ifndef`/skip guard extended to
   also skip when `pin`/libdft is absent. Point `Dockerfile.taint-oracle`'s `CMD`
   at it (replacing T2's placeholder).

**Code.** `examples/taint_oracle_diff.c`; a `native-trace.mk` `dr-taint-oracle-test`
lane; the `Dockerfile.taint-oracle` `CMD` swap.

**Tests.** This *is* the test lane. Each covered fixture is one TAP assertion of
DR≡libdft agreement + a negative control (unseeded → both empty). A confirmed
DR-client bug would surface here as a red `not ok` with the classification printed;
a libdft gap surfaces as a `# SKIP (libdft-gap-...)` line (T6).

**Docs.** A `## [Unreleased]` `Added` entry in
[CHANGELOG.md](../../../CHANGELOG.md): the libdft64 differential oracle lane
(`make docker-taint-oracle`) cross-validating the DR taint client (T7 also touches
the changelog — one entry, extended).

**Done when.**
- `make dr-taint-oracle-test` (inside `make docker-taint-oracle`) asserts
  byte-for-byte DR≡libdft sink agreement on every GP/integer-memory fixture, with
  passing negative controls.
- Every divergence in the covered subset is either a red failure (DR bug) or a
  classified, named skip — no silent pass.

### T6 — Named coverage-gap skips (the honest boundary)  (S, depends on: T5)

**Goal.** libdft64's documented blind spots are enumerated as **named** skips the
lane prints explicitly, and the SIMD fixtures are routed to the named-skip path,
not a false pass.

**Steps.**
1. In `dr-taint-oracle-test`, for each fixture class libdft cannot cover, print a
   distinct skip token so a reader sees *which* gap fired, not a blanket "skipped":
   - `libdft-partial-sse-avx` — libdft64 claims "basic SSE/AVX" but its upstream
     soundness note says propagation rules may be wrong; the XMM/YMM fixtures
     ([dr_taint_simd.c](../../../examples/dr_taint_simd.c)) run under the oracle as
     a **named skip**, cross-checked *informationally* (report the delta, do not
     fail on it).
   - `libdft-gap-avx512-zmm` — ZMM registers have no tag slot in libdft64
     (`src/ins_helper.h` logs "found zxmm!" and breaks); AVX-512 taint is silently
     dropped. Any ZMM fixture is a hard named skip.
   - `libdft-gap-eflags` — libdft64 ignores EFLAGS; the branch-condition sink
     (kind 1) is flag-carried, so if the two engines disagree *only* on the flag
     path, classify as this gap (T5 already routes the flag through the defining
     register to keep kind-1 meaningful).
   - `libdft-gap-implicit-flow` — control-dependence taint is out of scope for both
     engines; note it so no one reads its absence as a bug.
   - `libdft-gap-x87-fpu` and `libdft-gap-ternary-insn` — x87/FPU and ternary
     instructions are on libdft64's TODO; no fixture exercises them today, but the
     tokens are reserved so a future fixture self-classifies.
2. Add a short `docs/internal/analysis/`-adjacent note **inside the diff lane's
   output header** (printed, not a new doc) listing these tokens, so the skip
   reasons live where the reader runs the lane.

**Code.** Skip-token strings + the informational SIMD path in `dr-taint-oracle-test`;
no new files.

**Tests.** Running the lane on a host with Pin+libdft prints each applicable
`libdft-gap-*` / `libdft-partial-*` token; the SIMD fixtures print
`libdft-partial-sse-avx` and do **not** turn the lane red.

**Docs.** The gap enumeration is the honest-boundary record required by PIN-4's
exit criteria; cite [data-flow-capture.md](../analysis/data-flow-capture.md) (which
documents that even libdft punts on SIMD) in the changelog entry.

**Done when.**
- The SIMD subset is a **named** skip (`libdft-partial-sse-avx`), distinct from a
  clean pass and distinct from a failure.
- Each libdft blind spot has a distinct printed token; no gap is silent.

### T7 — CI wiring, `make help`, and the changelog  (S, depends on: T5)

**Goal.** The lane is discoverable and CI-gated.

**Steps.**
1. Add a `taint-oracle` job to [.github/workflows/ci.yml](../../../.github/workflows/ci.yml)
   beside the existing `taint` job (around [:408](../../../.github/workflows/ci.yml#L408)):
   one step, `run: make docker-taint-oracle`. It is a pure Docker lane (no
   hardware, no credentials), so it runs on the standard x86-64 Linux runner. Do
   **not** add a macOS leg (Pin is x86-only and the lane self-skips on non-x86; if
   a macOS-intel leg is ever wanted, that host id is `macos-15-intel`, never the
   retired `macos-13`).
2. Add the target to the `make help` "Docker (Linux CI lanes)" group in
   [Makefile](../../../Makefile#L159) — the block that lists the other `docker-*`
   lanes (`docker-drtrace`, `docker-drtrace-bindings`, `docker-drext-probe` at
   [:163-165](../../../Makefile#L163)), **not** the "Native runtime trace tiers"
   group above it (which lists only the non-docker `*-test` targets). A one-liner
   `docker-taint-oracle  libdft64 differential taint oracle (Pin; x86-64 Linux)`.
   Note the sibling `docker-taint-native` lane has **no** `make help` line today,
   so this is a fresh grouping decision — place it beside the `docker-drtrace`
   family, and if you add a `docker-taint-native` line at the same time keep the
   two taint docker lanes adjacent.
3. Finalize the `## [Unreleased]` `Added` entry in
   [CHANGELOG.md](../../../CHANGELOG.md) begun in T5.

**Code.** One CI job; one `make help` line; one changelog entry.

**Tests.** CI green on the new job. Locally, `make help | grep taint-oracle`
shows the line.

**Docs.** Changelog (above). Internal test lane — no Sphinx page.

**Done when.**
- CI runs `taint-oracle` green (or self-skips cleanly if the image cannot build
  Pin — but on the x86-64 runner it builds).
- `make help` lists `docker-taint-oracle`; `CHANGELOG.md` has the `Added` entry.

## Task order & parallelism

```
T1 (fetch/pin) ─► T2 (docker image) ─► T3 (Pintool) ─► T4 (driver) ─► T5 (diff) ─┬─► T6 (named skips)
                                                                                  └─► T7 (CI + help)
```

The chain T1→T2→T3→T4→T5 is the **critical path** and is strictly ordered (each
task needs the previous artifact: no image without the pin, no tool without the
image, no diff without the tool and the driver). T6 and T7 are independent of each
other and can be done concurrently once T5 lands. T3 (the Pintool) is the largest
single lift; T1 (pinning) can be started immediately and in parallel with reading
up on the libdft tag API before T3.

## Constraints & gates

- **Pin is proprietary freeware — test/oracle-only, never shipped.** The Intel
  Simplified Software License (October 2022) permits redistribution of the Pin
  *binary* with notice but **forbids modification / reverse-engineering /
  disassembly** of it. Per shared-constraint 1 of the plan, Pin (and anything
  linked against its PinCRT, including this Pintool) is **never** linked into
  `libasmtest`, `libasmtest_emu`, or any distributed binding package — it lives
  only in the `docker-taint-oracle` test lane. libdft64 inherits the original
  libdft license (Columbia NSL, permissive) but is likewise oracle-only here.
- **Two Pin kits, one per lane — a known conflict, resolved by pinning both.**
  This lane pins **Pin 3.20** (libdft64's only tested kit); the sibling
  [pin-xed-trace-tier.md](pin-xed-trace-tier.md) pins **Pin 4.2**
  (`4.2-99776-g21d818fa2`) for APX decode currency. Those pulls conflict (3.20
  ships 2021 XED with no APX; the APX track needs ≥ v2023.08.21 XED). Do **not**
  unify them onto one kit: keep `ARG PIN_VERSION` per Dockerfile and pin two kits.
  [pin-xed-trace-tier.md#T1](pin-xed-trace-tier.md) is the **sole author** of
  `scripts/fetch-pin.sh` and a hard prerequisite for T1 here. That script is
  env-keyed on `PIN_VERSION`/`PIN_URL`, so reusing it for the 3.20 kit means
  overriding **both**: the sibling's URL template is
  `pin-external-${PIN_VERSION}-gcc-linux.tar.gz`, but the 3.20 tarball has **no**
  `external` segment (`pin-${PIN_VERSION}-gcc-linux.tar.gz`), so a script keyed on
  `PIN_VERSION` alone cannot produce the 3.20 URL — set both
  `PIN_VERSION=3.20-98437-gf02b61307` and the matching no-`external` `PIN_URL`
  (T1 step 1). This doc does **not** re-author or bootstrap `fetch-pin.sh`; its
  only fetch-side additions are `scripts/fetch-libdft.sh` and a distinct 3.20
  `pin` digest row keyed on version. libdft64 building against a *newer* kit is
  **untested upstream** (only warnings-only reports against 3.27); do not assume
  it.
- **Real self-skip gate: non-x86 hosts.** Pin is x86-only. On aarch64 / Apple
  silicon the lane self-skips with a printed reason (gate on `uname -m` /
  `_docker_plat`), exactly as the plan mandates. This is the *only* hardware-shaped
  gate; a missing Pin kit or libdft checkout is **not** a gate — it is fetched and
  pinned (T1), never self-skipped, per [CLAUDE.md](../../../CLAUDE.md).
- **The SIMD gap is a named skip, not a hardware gate.** libdft64's XMM/YMM
  coverage is partial and its ZMM coverage is absent; that is a *coverage* boundary
  recorded as `libdft-partial-sse-avx` / `libdft-gap-avx512-zmm` (T6), distinct
  from both a clean pass and a real skip gate.
- **Do not premise value on "DR attach is unwired."** DR external attach for the
  taint/dataflow tier is landed (`Dockerfile.taint-attach`, `docker-dataflow-attach`,
  2026-07-14). This oracle's value is *independent cross-validation of a
  home-grown engine*, not filling a DR capability gap.
- **When a gate blocks validation, record it** in the lane's printed output and in
  the changelog (e.g. "SIMD agreement is informational — libdft rules unverified
  upstream"), never a silent pass.

## Research notes (verified 2026-07-17)

- **libdft64 fork + commit.** The canonical maintained fork is
  [AngoraFuzzer/libdft64][libdft64] ("modified from VUzzer64, originally from
  libdft", developed by ByteDance AI Lab), master HEAD
  `20804d5bae5d8aed31a71761b1a1149e35a0da95` (2025-02-21). The VUSec original
  ([vuzzer64][vuzzer64]) is stale (last commit 2022-03-29); the feature fork
  [vusec/libdft64-ng][libdft-ng] carries heavier shadow-memory semantics than a
  byte-level differential oracle needs. **Use AngoraFuzzer @ 20804d5.**
- **Paired Pin kit.** libdft64's README/`install_pin.sh` name
  `pin-3.20-98437-gf02b61307-gcc-linux` exactly; tarball
  `https://software.intel.com/sites/landingpage/pintool/downloads/pin-3.20-98437-gf02b61307-gcc-linux.tar.gz`
  (verified live 2026-07-18: HTTP 200, 35,664,681 bytes, Last-Modified 2021-07-08).
  **SHA-256 `ca2f542eee2013471961bb683d06ccb20ef5dd8ed0d02537cf4d47f09bd616bf`**
  (self-computed 2026-07-18 from that URL — Intel publishes no official digest, so
  this is a single-download hash; record it as this repo's pin regardless, since a
  moved asset must fail loudly).
- **Building against an external kit.** `export PIN_ROOT=<kit>; make` at libdft64's
  top level builds `src/` → `src/obj-intel64/libdft.a`, then the tools with
  `TARGET=intel64` (clang/clang++). A tool's `makefile.rules` links
  `TOOL_LIBS += -L$(LIBDFT_PATH) -ldft` with `LIBDFT_PATH=../src/obj-intel64/`
  and `TOOL_ROOTS := <tool>`. Tested platform: Ubuntu 20.04. Newer kits: issue
  [#23][libdft-issue23] reports GNU_PROPERTY linker warnings against Pin 3.27
  (functionality reportedly unaffected, no maintainer fix) — treat 3.20 as the
  compatible pin.
- **Coverage gaps (libdft64 upstream, verbatim).** Byte-level only; "Ignore
  implicit flows"; "Ignore eflags registers"; TODO: ternary instructions, eflags
  rules, FPU instructions, per-instruction testing; soundness note: "taint
  propagation rules may be wrong", only "basic instructions" supported. **SSE
  nuance:** the "famously skips XMM/SSE" line in this repo's own docs
  ([data-flow-capture.md](../analysis/data-flow-capture.md),
  [intel-pin-capabilities-plan.md](../plans/intel-pin-capabilities-plan.md) PIN-4)
  is true of the *original Columbia* libdft, **not** this fork — AngoraFuzzer's
  README claims "basic SSE, AVX" and `src/ins_helper.h` maps
  `REG_XMM0..15`/`REG_YMM0..15` to tag slots; **ZMM has no slot** (logs "found
  zxmm!" and breaks — AVX-512 taint silently dropped). Hence `libdft-partial-sse-avx`
  (basic, rules unverified) is the correct token for XMM/YMM, and
  `libdft-gap-avx512-zmm` for ZMM.
- **Pin build system.** The kit ships `source/tools/Config/*` + a `MyPinTool`
  template. External-kit build: `make PIN_ROOT=/opt/pin obj-intel64/tool.so`; the
  template makefile carries an SPDX MIT header, so a derived tool makefile is fine
  to ship. PinCRT flags (`-DPIN_CRT=1`, `-nostdlib`, `-fno-rtti`, `-fno-exceptions`)
  are supplied by the kit's `makefile.unix.config` — a Pintool must not fight them.
- **Pin/XED currency (context, not used here).** Pin 3.20's XED predates APX; the
  APX-decode currency argument belongs to
  [pin-xed-trace-tier.md](pin-xed-trace-tier.md), which pins a newer kit. DynamoRIO
  VNNI is **supported** in the repo's pinned DR 11.91.20630 (issue
  [#5440][dr5440] closed 2022, fixed by PR #5444) — the "VNNI still breaks" claim in
  older analysis is stale; APX ([#6226][dr6226]) remains open. None of this changes
  the 3.20 pin this oracle needs.

## Out of scope

- **The XED-decoded Pin trace tier / APX decode currency** — owned by
  [pin-xed-trace-tier.md](pin-xed-trace-tier.md) (track PIN-2). It owns the
  *newer* Pin kit, `Dockerfile.pintool`, and — as **sole author** —
  [`scripts/fetch-pin.sh`](pin-xed-trace-tier.md); this doc **consumes** that
  script (invoking `pin-xed-trace-tier.md#T1`'s `fetch-pin.sh` with a 3.20
  `PIN_VERSION`/`PIN_URL` override, T1) rather than re-authoring it, and does not
  re-scope PIN-2's work. This doc's only fetch-side additions are
  `scripts/fetch-libdft.sh` and the distinct 3.20 `pin` digest row.
- **Pin probe-mode arg/return capture** — [pin-probe-mode-capture.md](pin-probe-mode-capture.md)
  (track PIN-3).
- **The SDE future-ISA test lane** — [pin-sde-future-isa-lane.md](pin-sde-future-isa-lane.md)
  (track PIN-1).
- **The DR taint tier's own increments** (in-band propagation, launch-under-DR,
  GC-move survival, SIMD taint) — all landed, documented in
  [dynamorio-taint-tier-plan.md](../archive/plans/dynamorio-taint-tier-plan.md);
  this doc only *validates* that tier's output, never extends it.
- **PT-path + emulator replay data-flow** — [dataflow-pt-replay-tier.md](dataflow-pt-replay-tier.md).

[libdft64]: https://github.com/AngoraFuzzer/libdft64
[libdft-mk]: https://raw.githubusercontent.com/AngoraFuzzer/libdft64/master/Makefile
[libdft-install]: https://github.com/AngoraFuzzer/libdft64/blob/master/INSTALL.md
[libdft-issue23]: https://github.com/AngoraFuzzer/libdft64/issues/23
[libdft-ng]: https://github.com/vusec/libdft64-ng
[vuzzer64]: https://github.com/vusec/vuzzer64
[dr5440]: https://github.com/DynamoRIO/dynamorio/issues/5440
[dr6226]: https://github.com/DynamoRIO/dynamorio/issues/6226
