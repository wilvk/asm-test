# CoreSight live OpenCSD decode tree (AArch64 board-gated) — implementation

> **Sources.** Actioned from
> [hardware-trace-plan.md](../plans/hardware-trace-plan.md) (Phase 1, the ARM
> CoreSight backend). Written 2026-07-17. If this doc and a source disagree,
> this doc wins (sources may be stale); if the CODE and this doc disagree,
> re-verify before implementing.

## Why this work exists

The hardware-trace tier already captures and decodes Intel PT, reconstructs AMD
branch records, and single-steps — but its fourth backend, ARM CoreSight
(`ASMTEST_HWTRACE_CORESIGHT`), self-skips on **every** host because the live
OpenCSD decode tree was never written. The decoder-independent half is done and
host-validated; only the board-gated glue — OpenCSD decode tree, ETM/ETE
configuration, sideband (trace-ID) handling — separates AArch64 users from real
hardware-assisted tracing. This doc lands that glue: after it, a bare-metal
AArch64 CoreSight board records the same `asmtest_trace_t` instruction/block
offsets as the Unicorn, DynamoRIO, PT, AMD, and single-step tiers, and every
other host self-skips with a *truthful* reason.

## What already exists (verified 2026-07-17)

- [src/cs_backend.c](../../../src/cs_backend.c) — the split CoreSight backend.
  `asmtest_cs_reconstruct()` (line 64) is the **landed, host-validated core**:
  it replays ordered `asmtest_cs_range_t` instruction ranges (struct at lines
  49–53: `start_off`, `end_off` exclusive, `ends_in_branch`) through the
  Capstone length-decoder into `insns[]`/`blocks[]`, arch-parameterized via
  `asmtest_arch_t`. The **live half is a stub**: `asmtest_cs_decoder_present()`
  returns 0 in *both* arms of the `ASMTEST_HAVE_OPENCSD` conditional (lines 109
  and 123), and `asmtest_cs_decode()` returns `ASMTEST_HW_ENOSYS`. The comment
  at lines 103–108 records why: no untested hardware code.
- [src/hwtrace.c](../../../src/hwtrace.c) — the capture + gating facade.
  `asmtest_hwtrace_available()` (line 320) walks decoder-present →
  `cpu_matches` (CORESIGHT ⇒ `__aarch64__`, lines 205–210) → `pmu_type()`
  (reads `/sys/bus/event_source/devices/cs_etm/type`, lines 145–157) →
  `perf_permitted_e()` (a real disabled-event open probe, lines 288–314).
  `hw_classify()` (line 350) produces the reason strings: "built without
  OpenCSD", "not an AArch64 host", "no cs_etm PMU (needs a CoreSight-capable
  AArch64 board)". The **capture path is already backend-generic**: begin opens
  the PMU by type, mmaps the base ring + AUX ring (snapshot ⇒ `PROT_READ`
  circular; else linear), and `asmtest_hwtrace_end()` disables the event, scans
  the data ring for `PERF_RECORD_AUX`/`PERF_AUX_FLAG_TRUNCATED`
  (`aux_data_ring_truncated()`, lines 1971–2002), then dispatches
  `asmtest_cs_decode(aux, head, r->base, r->len, r->trace)` at lines
  2079–2080, setting `truncated` on overflow or a non-OK decode (lines
  2081–2082). **The CoreSight work in this doc is almost entirely inside
  `cs_backend.c` plus one sideband extension in `hwtrace.c`.**
- [src/pt_backend.c](../../../src/pt_backend.c) — the pattern to mirror:
  `asmtest_pt_decode()` (line 141) walks the decoder's per-instruction stream,
  records only non-speculative in-region IPs as offsets, opens a block after
  every branch (`is_branch()`, line 108) and after every out-of-region
  excursion, and returns `ASMTEST_HW_EDECODE` when zero in-region instructions
  decode so `end()` flags `truncated` instead of reporting an empty
  "complete" trace (lines 205–214).
- [examples/test_hwtrace.c](../../../examples/test_hwtrace.c) —
  `test_cs_reconstruction()` (line 631) validates the core against the shared
  x86-64 fixture `ROUTINE` (line 120): ranges `{0,0xe,1},{0x11,0x12,1}` ⇒
  insns `{0,3,6,0xc,0x11}`, blocks `{0,0x11}`, not truncated. The AArch64
  fixture `ROUTINE_A64` already exists (lines 130–132) — `add x0,x0,x1; cmp
  x0,#100; b.le +8; sub x0,x0,#1; ret`, so `add2(20,22)=42` executes offsets
  `{0,4,8,0x10}` in two blocks `{0,0x10}` — **but it and its `LOOP_A64` sibling
  (lines 135–137) are compiled only inside `#if defined(__aarch64__)` (the
  whole A64-fixture block is lines 125–138)**. Any code that names those
  fixtures must therefore itself sit under `#if defined(__aarch64__)`, or the
  x86-64 build fails to compile (`'ROUTINE_A64' undeclared`); mirror how
  `test_ptrace_oop` (line 3845) arch-selects the A64 fixture. The live-capture
  block in `main` (lines 8327–8388) is **PT-only** and early-returns when PT is
  unavailable, so a CoreSight live test must be its own function called before
  it.
- [mk/native-trace.mk](../../../mk/native-trace.mk) — lines 1916–1919:
  `OPENCSD_CFLAGS`/`OPENCSD_LIBS` from `pkg-config libopencsd`, and
  `OPENCSD_DEF := -DASMTEST_HAVE_OPENCSD` when the module exists.
  `cs_backend.o` compiles with those flags (lines 1979–1980; PIC twin at
  2732–2733); every hwtrace link line already carries `$(OPENCSD_LIBS)`.
  `hwtrace-test` (recipe at line 2128) runs `$(BUILD)/test_hwtrace` plus the
  IBS lane.
- [Makefile](../../../Makefile) lines 697–700: `make tidy` runs clang-tidy on
  `src/cs_backend.c` with the OpenCSD flags when `pkg-config --exists
  libopencsd`.
- [Dockerfile.hwtrace](../../../Dockerfile.hwtrace) — FROM
  `asmtest-bindings-base` (ubuntu:24.04,
  [Dockerfile.bindings-base](../../../Dockerfile.bindings-base) line 15);
  installs `python3 python3-pytest libipt-dev` — **no `libopencsd-dev`**, so
  the `docker-hwtrace` lane builds `cs_backend.o` decoder-free while the CI
  `hwtrace` job ([.github/workflows/ci.yml](../../../.github/workflows/ci.yml)
  line 539) apt-installs `libopencsd-dev` on the runner. That asymmetry is the
  build-parity gap T1 closes.
- [mk/docker.mk](../../../mk/docker.mk) lines 442–445: `docker-hwtrace`
  builds `Dockerfile.hwtrace` and `docker run`s the resulting `asmtest-hwtrace`
  image; the run command list
  `make hwtrace-test && make codeimage-test && make hwtrace-python-test` is the
  image's CMD ([Dockerfile.hwtrace](../../../Dockerfile.hwtrace) line 24), not
  a docker.mk sub-target.

Prove the baseline before touching anything:

```sh
make hwtrace-test     # host: suite passes; look for
                      #   "ok … CoreSight reconstruct succeeds on synthetic ranges"
                      # and (any non-board host) a SKIP whose reason matches the host
make docker-hwtrace   # containerized twin; same green output, PT/CS/AMD self-skip
```

## Tasks

### T1 — Close the OpenCSD build-parity gap (Dockerfile + C-API link fix)  (S, depends on: none)

**Goal.** The `docker-hwtrace` lane builds `cs_backend.c` with
`-DASMTEST_HAVE_OPENCSD` exactly as the CI `hwtrace` job does, and the link
line carries the OpenCSD **C API** library the live tree will actually call.

**Steps.**

1. Edit [Dockerfile.hwtrace](../../../Dockerfile.hwtrace): add `libopencsd-dev`
   to the apt line (line 21), mirroring how `libipt-dev` sits there today:

   ```dockerfile
   RUN apt-get update \
    && apt-get install -y --no-install-recommends python3 python3-pytest libipt-dev libopencsd-dev \
    && rm -rf /var/lib/apt/lists/*
   ```

   Extend the comment block above it: libopencsd is a userspace build
   dependency (Ubuntu 24.04 noble ships 1.4.1-1build1), NOT CoreSight
   hardware; with it installed the decode tree compiles and links, and the
   tier still self-skips (no `cs_etm` PMU in a container).
2. Fix the link flags in [mk/native-trace.mk](../../../mk/native-trace.mk)
   lines 1914–1919. Debian/Ubuntu's `libopencsd.pc` says `Libs: -lopencsd`
   **only** — the C API entry points (`ocsd_create_dcd_tree` etc.) live in the
   separate `libopencsd_c_api.so` the same package installs (see Research
   notes). Today nothing calls them so the gap is invisible; the moment T3
   lands, linking would fail with `undefined reference to ocsd_create_dcd_tree`
   everywhere OpenCSD is present. Restructure so the C API lib is appended
   exactly when the module exists:

   ```make
   OPENCSD_CFLAGS ?= $(shell pkg-config --cflags libopencsd 2>/dev/null)
   ifeq ($(shell pkg-config --exists libopencsd 2>/dev/null && echo 1),1)
   OPENCSD_DEF  := -DASMTEST_HAVE_OPENCSD
   OPENCSD_LIBS ?= $(shell pkg-config --libs libopencsd 2>/dev/null) -lopencsd_c_api
   else
   OPENCSD_LIBS ?=
   endif
   ```

   Keep the existing "module is `libopencsd`, NOT `opencsd`" comment and add
   one line explaining the split-library fact with the packages.ubuntu.com
   citation. `-lopencsd_c_api` precedes `-lopencsd`'s C++ core via its own
   `DT_NEEDED`, so shared linking resolves transitively; no `-lstdc++` needed
   in C link lines.
3. Run `make docker-hwtrace`. In the build log confirm `cs_backend.o` compiles
   with `-DASMTEST_HAVE_OPENCSD` (grep the echoed compile line), and the suite
   output is unchanged (the tier still self-skips: `present()` is still 0).
4. Host sanity where libopencsd is absent: `make hwtrace-test` still builds
   with empty `OPENCSD_LIBS` (nothing to link) — no regression.

**Code.** Only the two files above. No C changes.

**Tests.** No new test file — this is build plumbing. Verification is the
`docker-hwtrace` lane itself plus the CI `hwtrace` job (ubuntu-latest,
`libopencsd-dev` already installed at ci.yml line 539) staying green with the
new `-lopencsd_c_api` on the link line. A failure looks like
`ld: cannot find -lopencsd_c_api` (package missing) or pkg-config silently not
firing (check `pkg-config --exists libopencsd && echo yes` inside the image).

**Docs.** Internal-only, no user-facing docs: dependency plumbing with no
behavior change. (CHANGELOG waits for T5 when the feature itself ships.)

**Done when.**

- `make docker-hwtrace` output shows `cs_backend.c` compiled with
  `-DASMTEST_HAVE_OPENCSD` and the full suite still passes/self-skips exactly
  as before.
- `docker run --rm asmtest-hwtrace pkg-config --modversion libopencsd` prints
  a version (noble ships 1.4.1).
- `make hwtrace-test` on a libopencsd-free host is unchanged.

### T2 — Board bring-up: verify capture substrate on a real CoreSight board  (M, depends on: T1)

**Goal.** A named bare-metal AArch64 CoreSight board (Juno, ZCU102/Kria,
Jetson, or a Pixel with `CONFIG_CORESIGHT` — per the plan's validation notes)
demonstrably captures `cs_etm` AUX data through *this repo's* generic perf
path, and its trace parameters are recorded for T3.

**Steps.**

1. On the board, confirm the substrate the gating chain probes:

   ```sh
   cat /sys/bus/event_source/devices/cs_etm/type          # PMU present
   ls /sys/bus/event_source/devices/cs_etm/sinks/          # at least one sink (tmc_etr0 / tmc_etb0 / trbe)
   cat /proc/sys/kernel/perf_event_paranoid                # lower to -1, or grant CAP_PERFMON
   ```

2. Prove kernel-side capture works independent of this repo, per the kernel's
   CoreSight doc: `perf record -e cs_etm//u --per-thread -- /bin/true` then
   `perf report -D | grep -c AUXTRACE`. Record which sink the kernel
   auto-selected (kernels ≥ 5.10 pick a default sink; if the open fails with
   no default, name one: `perf record -e cs_etm/@tmc_etr0/u …`).
3. Build the repo on the board (`libopencsd-dev`, `libcapstone-dev`,
   `pkg-config` via apt — same packages as ci.yml line 539) and run
   `make hwtrace-test`. Expected today: the CoreSight tier reports "built
   without OpenCSD" **even with OpenCSD installed** — because `present()` is
   hard-coded 0, `hw_classify` misattributes the stage. That misleading string
   disappears when T3 flips the stub; note it, don't fix it separately.
4. Record in the PR description (T3 will need all of these): board + SoC,
   kernel version, ETM architecture per CPU (`ls
   /sys/bus/event_source/devices/cs_etm/cpu0/` and read `trcidr/trcidr0..2,8`,
   plus `mgmt/trcdevarch` if present ⇒ ETE, else ETMv4), sink type, and
   whether AUX records carry `PERF_AUX_FLAG_CORESIGHT_FORMAT_RAW` (TRBE ⇒ raw
   unformatted; ETR/ETB ⇒ 16-byte frame-formatted). The exact sysfs metadata
   file names are the ones perf's own recorder reads — verify with `ls` on the
   board rather than trusting this doc.
5. Verify the repo's *generic* open path reaches the probe stage:
   run the suite and check `asmtest_hwtrace_status(CORESIGHT,…)` output (the
   `test_status_surface` lane prints stage/errno) — after privilege is
   lowered, the failing gate must be the decoder stage only.

**Code.** None on the happy path. Contingency (record it if hit): if
`perf_event_open` on `cs_etm` fails with `-EINVAL` because the kernel predates
default-sink selection, the sink must be passed via `attr.config2` — that would
be a small, board-validated extension to the begin path in
[src/hwtrace.c](../../../src/hwtrace.c) (lines 1883–1939), mirroring how perf
encodes `@sink`.

**Tests.** Manual, hardware-gated by nature: the observable outputs of steps
1–5 are the test. Capture the command transcripts; they are the evidence the
house "no untested hardware code" rule requires before T3 may land.

**Docs.** Internal-only: the recorded board facts land in the T3/T4 commit
messages and in this doc's implementation notes when executed.

**Done when.**

- `perf record -e cs_etm//u --per-thread` produces `AUXTRACE` data on the
  board.
- `make hwtrace-test` builds and runs on the board (CoreSight still skipping —
  decoder stub, expected).
- Board facts (ETMv4 vs ETE, sink, formatted vs raw, register values) are
  written down for T3.

### T3 — Write `asmtest_cs_decode`: the OpenCSD decode tree, sideband, and the `present()` flip  (L, depends on: T2)

**Goal.** `asmtest_cs_decode()` turns a captured cs_etm AUX buffer into
ordered `asmtest_cs_range_t` ranges via a real OpenCSD decode tree and feeds
`asmtest_cs_reconstruct(ASMTEST_ARCH_ARM64, …)`, and
`asmtest_cs_decoder_present()` returns 1 under `ASMTEST_HAVE_OPENCSD` — all
written and validated **on the board** (there is no synthetic AUX workaround:
OpenCSD has no packet encoder analogous to libipt's, so per the house rule the
tree stays unwritten until a board runs it; T2+T3+T4 land together as one
board-validated series).

**Steps.**

1. **Sideband first** — extend the data-ring scan in
   [src/hwtrace.c](../../../src/hwtrace.c). `aux_data_ring_truncated()`
   (static, lines 1971–2002) already walks every record and consumes the ring;
   widen it into `aux_data_ring_scan(cs_sideband_t *out)` that additionally
   captures, for CoreSight sessions:
   - the 7-bit trace ID from `PERF_RECORD_AUX_OUTPUT_HW_ID` (uapi record type;
     the kernel emits it for cs_etm when the event schedules in). Mirror the
     field layout from the kernel's `include/linux/coresight-pmu.h`
     (`CS_AUX_HW_ID_TRACE_ID_MASK`; v0.1 adds sink IDs) — it is not a uapi
     header, so copy the masks into `hwtrace.c` with a cited comment and
     verify the extracted ID against `perf report -D` on the board.
   - whether any `PERF_RECORD_AUX` carried `PERF_AUX_FLAG_CORESIGHT_FORMAT_RAW`
     (uapi `linux/perf_event.h`) — raw (TRBE) vs frame-formatted (ETR/ETB)
     selects the decode-tree source type below.

   Thread both to the decoder: change the internal decl of
   `asmtest_cs_decode` (hwtrace.c line 89 and cs_backend.c) to accept a small
   `asmtest_cs_sideband_t { int trace_id; int raw_format; }` parameter, or
   stash them in file-scope accessors `asmtest_cs_set_sideband()` called from
   `end()` before dispatch — pick the explicit-parameter form; both TUs are
   internal so the signature is free to change (the PT dispatch at line
   2076-2078 is untouched).
2. **Read the ETM/ETE config** in `cs_backend.c` under `ASMTEST_HAVE_OPENCSD`:
   a helper `cs_read_cpu_config()` that reads, for the current CPU's
   `/sys/bus/event_source/devices/cs_etm/cpu<n>/` node, the ID registers
   (`trcidr/trcidr0`, `trcidr1`, `trcidr2`, `trcidr8`) and — if
   `mgmt/trcdevarch` exists — DEVARCH (⇒ ETE). Fill `ocsd_etmv4_cfg`
   (`reg_idr0/1/2/8`, `reg_configr = 0` for this minimal capture — no
   timestamps, cycacc, or context tracing are requested by the begin path —
   `reg_traceidr = sideband trace ID`, `arch_ver`/`core_prof` per header) or
   `ocsd_ete_cfg` (adds `reg_devarch`). Struct definitions:
   `opencsd/etmv4/trc_pkt_types_etmv4.h`, `opencsd/ete/trc_pkt_types_ete.h`
   (installed by `libopencsd-dev`). Verify field names against the installed
   1.4.1 headers, not this doc.
3. **Build the tree** in `asmtest_cs_decode()` (include
   `<opencsd/c_api/opencsd_c_api.h>`):
   - `ocsd_create_dcd_tree(src, flags)` with
     `OCSD_TRC_SRC_FRAME_FORMATTED` +
     `OCSD_DFRMTR_FRAME_MEM_ALIGN | OCSD_DFRMTR_RESET_ON_4X_FSYNC` for
     ETR/ETB, or `OCSD_TRC_SRC_SINGLE` (flags 0) when the sideband says raw —
     the same split perf's cs-etm-decoder.c makes.
   - `ocsd_dt_create_decoder(handle, "ETE"‑or‑"ETMV4I",
     OCSD_CREATE_FLG_FULL_DECODER, &cfg, &csid)`.
   - `ocsd_dt_add_buffer_mem_acc(handle, (ocsd_vaddr_t)(uintptr_t)base,
     OCSD_MEM_SPACE_ANY, base, (uint32_t)len)` — the registered region is the
     only image the decoder may walk, exactly like `read_region` in
     pt_backend.c.
   - `ocsd_dt_set_gen_elem_outfn(handle, cs_elem_cb, &ctx)`.
   - Drive the datapath: loop `ocsd_dt_process_data(handle, OCSD_OP_DATA,
     index, chunk, p, &consumed)` over `aux[0, aux_len)`, then one
     `OCSD_OP_EOT`; `ocsd_destroy_dcd_tree` on every exit path.
4. **The element callback** `cs_elem_cb` collects ranges into a growable
   `asmtest_cs_range_t` array (malloc/realloc in the ctx; `ASMTEST_HW_EDECODE`
   on allocation failure):
   - `OCSD_GEN_TRC_ELEM_INSTR_RANGE` with `st_addr` inside
     `[base_ip, base_ip+len)`: append `{st_addr-base_ip, en_addr-base_ip,
     ends_in_branch}` where `ends_in_branch = (last_i_type is a branch)` —
     taken or not, matching `is_branch()` in pt_backend.c (`en_addr` is
     already exclusive, matching `end_off`). Clamp `en_addr` to the region
     end.
   - out-of-region `INSTR_RANGE`, `ADDR_NACC` (the decoder lost the image —
     expected whenever execution leaves the region, since the memory accessor
     serves only `[base, base+len)`), `TRACE_ON`, `NO_SYNC`, `EO_TRACE`,
     `EXCEPTION`/`EXCEPTION_RET`: do not append; instead set
     `ends_in_branch = 1` on the **last collected range** so the next
     in-region range opens a new block — the CS equivalent of pt_backend.c's
     `prev_was_branch = 1` on leaving the region. Spot-verify the
     `OCSD_GEN_TRC_ELEM_*` names against the installed
     `opencsd/trc_gen_elem_types.h` before use (see Research caveats).
   - return `OCSD_RESP_CONT`.
5. **Finish**: call `asmtest_cs_reconstruct(ASMTEST_ARCH_ARM64, ranges, n,
   base, len, trace)`; if zero in-region ranges were collected from a
   non-empty AUX buffer, return `ASMTEST_HW_EDECODE` (mirrors pt_backend.c
   lines 205–214 — `end()` then flags `truncated`). Flip the
   `ASMTEST_HAVE_OPENCSD` arm's `asmtest_cs_decoder_present()` to `return 1;`
   and rewrite the stale stub comment (lines 103–108). The `#else` arm keeps
   returning 0.
6. After each board iteration: `make hwtrace-test` on the board; on the
   laptop, `make docker-hwtrace` (arm64 container: decoder present, CPU
   matches, **PMU absent** ⇒ skip reason must now read "no cs_etm PMU (needs a
   CoreSight-capable AArch64 board)" — the truthful string for the first
   time) and `make tidy` (the opencsd gate at Makefile lines 697–700 now
   lints real code). Run `make fmt` before committing.

**Code.** `src/cs_backend.c` (~250 new lines under `ASMTEST_HAVE_OPENCSD`),
`src/hwtrace.c` (data-ring scan widening + sideband threading, ~60 lines).
Public headers unchanged — `asmtest_hwtrace.h` already declares the full
surface. Keep OpenCSD strictly inside `cs_backend.c` (the header is
C-API-only, so the TU stays C; the C++ core is linked, never included).

**Tests.** The host-runnable halves get automated coverage now; the live path
is T4's. Extend `examples/test_hwtrace.c`:

- a sideband-scan unit test: build a synthetic data-ring page containing a
  fabricated `PERF_RECORD_AUX_OUTPUT_HW_ID` record + a `PERF_RECORD_AUX` with
  the RAW flag, run the (newly non-static or test-declared) scan, assert the
  extracted trace ID and format bit. Runs on every host — same style as the
  F43 ring-parse seam test already in this file.
- assert the skip-reason truth-table change: with OpenCSD compiled in, on a
  non-AArch64 host the CoreSight reason is "not an AArch64 host"; on AArch64
  sans PMU it is the "no cs_etm PMU…" string (this executes in
  `docker-hwtrace` on an Apple-silicon dev machine).

A failure prints `not ok N - <assertion>`; a pass keeps the suite's
`1..N` all-ok.

**Docs.** None yet (T5 owns all user-visible docs, after T4 proves parity).

**Done when.**

- On the board: `make hwtrace-test` reaches the CoreSight decode with no
  crash and `asmtest_hwtrace_available(ASMTEST_HWTRACE_CORESIGHT) == 1`.
- Off-board (docker arm64): the lane is green and the CoreSight skip reason is
  the truthful PMU-stage string.
- x86-64 CI (`hwtrace` job): green, CoreSight reason "not an AArch64 host".
- `make tidy` opencsd gate passes; `make fmt-check` clean.

### T4 — Live parity acceptance on the board + truncation path  (M, depends on: T3)

**Goal.** One live CoreSight capture of the shared AArch64 fixture
reconstructs exactly the offsets every other backend produces, the overflow
path sets `truncated`, and the test self-skips cleanly everywhere else.

**Steps.**

1. Add `test_cs_live_capture()` to
   [examples/test_hwtrace.c](../../../examples/test_hwtrace.c), called from
   `main` **before** the PT-only early-return block (lines 8327–8355 —
   unreachable on a board where PT is unavailable). **Mind the arch guard:**
   `ROUTINE_A64`/`LOOP_A64` are compiled only under `#if defined(__aarch64__)`
   (test_hwtrace.c lines 125–138), so every line that names them must sit
   inside that guard — otherwise the x86-64 build fails with
   `'ROUTINE_A64' undeclared`, breaking the very `hwtrace` CI job and
   `make docker-hwtrace` this task's Done-when requires to stay green and print
   a SKIP. Split the function so the skip check compiles everywhere and only
   the capture body is arch-gated:
   - **Unguarded** (compiles and runs on x86-64, prints the SKIP): the
     availability check — if
     `!asmtest_hwtrace_available(ASMTEST_HWTRACE_CORESIGHT)`, print
     `# SKIP hwtrace CoreSight capture: <asmtest_hwtrace_skip_reason>` and
     return. On x86-64 this SKIP-and-return is the whole body the compiler
     sees; everything below is elided by the guard.
   - **Inside `#if defined(__aarch64__)`** (it references `ROUTINE_A64`): map it
     W^X (`mmap` RW → `memcpy` → `mprotect` RX → `__builtin___clear_cache`) as
     the arch-selected W^X block in `test_ptrace_oop` does (lines 3867–3875,
     operating on its guard-selected `RT`/`RTN`) — **not** the unguarded x86
     `ROUTINE` block at lines 8358–8366; then `INIT_OPTS` with
     `ASMTEST_HWTRACE_CORESIGHT`, init / `register_region("add2_a64")` /
     `begin` / call `fn(20,22)` / `end` / shutdown.
   - assert (also inside the guard): return 42; insns exactly `{0,4,8,0x10}`
     (the `sub` at 0xc is skipped by the taken `b.le`); blocks `{0,0x10}` and
     `asmtest_emu_trace_blocks_len == 2`; not truncated. This is the same
     ground-truth walk `test_cs_reconstruction` proves synthetically — the
     live capture must match it byte-for-byte.
2. Truncation: a second capture with `opts.aux_size = 4096` around a
   long-running loop (mirror the `LOOP_A64` fixture at lines 135–137, raised
   trip count) must end with `truncated == true` via either the
   `PERF_AUX_FLAG_TRUNCATED` scan or the head≥size backstop (hwtrace.c lines
   2067–2074). This capture names `LOOP_A64`, so — like step 1's body — it
   lives inside the same `#if defined(__aarch64__)` guard.
3. Not-taken discriminator (still inside the `#if defined(__aarch64__)` guard —
   it re-runs the `ROUTINE_A64` mapping): call `fn(200,22)` (sum 222 > 100 ⇒
   `b.le` falls through) and assert insns `{0,4,8,0xc,0x10}` with blocks
   `{0,0xc}` — the `sub` at 0xc is the executed discriminator proving the
   decode follows the trace rather than a baked-in answer (the same
   one-sided-fixture failure mode the PT fixture fixed; see the plan's
   2026-07-17 correction).
4. Run the full matrix: board `make hwtrace-test` (live assertions run);
   x86-64 host + `make docker-hwtrace` (new test prints its SKIP line, suite
   green); CI `hwtrace` job green.

**Code.** Test-only (`examples/test_hwtrace.c`, ~120 lines). If step 3
exposes decode-side gaps (e.g. `ends_in_branch` on not-taken atoms), fix them
in `cs_backend.c` within this same board series.

**Tests.** This task *is* the test. Pass: three new `ok` groups on the board,
one printed SKIP everywhere else. Failure: `not ok … CoreSight live insns
{0,4,8,0x10}` on the board, or a missing SKIP line off-board.

**Docs.** None (T5).

**Done when.**

- Board run shows the live parity, not-taken, and truncation assertions all
  `ok`, and the transcript is attached to the landing PR/commit.
- `make hwtrace-test` on x86-64 and `make docker-hwtrace` self-skip the new
  test with a printed reason and exit 0.
- The T2–T4 series lands on `main` only with that board transcript (house
  rule; see Constraints).

### T5 — Flip the docs: CoreSight is live-on-board, not a scaffold  (S, depends on: T4)

**Goal.** Every user-facing claim that CoreSight "always self-skips / is a
scaffold" is replaced by the shipped reality, and the change is logged.

**Steps.**

1. [docs/guides/tracing/native-tracing.md](../../../docs/guides/tracing/native-tracing.md)
   lines 337–339: replace "the CoreSight backend is a documented scaffold
   pending AArch64 board access (it always self-skips until completed)" with:
   implemented and board-validated (name the board from T2); self-skips
   everywhere without a `cs_etm` PMU; decode is OpenCSD (ETMv4 + ETE),
   validated on <board> with <sink>.
2. [docs/guides/tracing/hardware-tracing.md](../../../docs/guides/tracing/hardware-tracing.md):
   the backend table row (line 39 "specific **AArch64** boards (scaffold)"),
   the callout at line 54, and the limitation bullet at line 545 — same
   correction. Keep the honest CI caveat: live capture still cannot run on
   standard CI (board runner is
   [self-hosted-ci-runners.md](self-hosted-ci-runners.md)'s scope).
3. Update the status paragraph in
   [hardware-trace-plan.md](../plans/hardware-trace-plan.md) (lines 36–50):
   the live tree is no longer the open item; record the board evidence, per
   the plan's own "update this file as phases land" instruction.
4. `CHANGELOG.md` under `## [Unreleased]` / `### Added`: "ARM CoreSight live
   OpenCSD decode tree (`asmtest_cs_decode`): the hardware-trace tier's
   CoreSight backend now captures and decodes ETMv4/ETE trace on bare-metal
   AArch64 boards, reconstructing the same instruction/block offsets as the
   PT/AMD/single-step/DynamoRIO/Unicorn tiers; validated on <board>. Docker
   `hwtrace` lane gains `libopencsd-dev` build parity."
5. `make docs` (or `make docker-docs`) — Sphinx `-W`, so any broken link
   fails loudly.

**Code.** None.

**Tests.** `make docs` exits 0. `grep -rn "scaffold" docs/guides/tracing/`
returns no CoreSight rows.

**Docs.** This task is the docs.

**Done when.**

- Both guides and the plan describe the shipped state; changelog entry
  present; `make docs` green.

## Task order & parallelism

```
T1 (host, today) ──► T2 (board bring-up) ──► T3 (decode tree) ──► T4 (parity) ──► T5 (docs)
```

- T1 is independent and landable immediately on any host; it also makes the
  board build in T2 a one-command apt install.
- T2→T3→T4 are strictly ordered and **board-resident**; they land on `main`
  as one validated series (T3's code must never merge ahead of T4's
  transcript). One person owns the board series; a second can do T1 and
  pre-write T5's text in parallel.
- Critical path: board access → T3. Everything except T1 and T5 prose is
  behind the hardware gate.

## Constraints & gates

- **Hardware gate (real, per CLAUDE.md):** a bare-metal AArch64 board with a
  CoreSight ETM/ETE source and a sink (TMC-ETR/ETB or TRBE) and lowered
  `perf_event_paranoid` / `CAP_PERFMON`. Cloud ARM VMs and Apple silicon do
  not qualify (KVM guests get no self-hosted trace; macOS exposes no
  CoreSight). This gate is why the tier self-skips: record the skip reason,
  never fake the gate.
- **No synthetic workaround:** OpenCSD is decoder-only — unlike libipt there
  is no packet encoder to synthesize a cs_etm AUX stream for the fixture, so
  the "validate decode on any host" trick `asmtest_pt_encode_fixture` uses is
  unavailable. Hence the house "no untested hardware code" rule keeps
  `asmtest_cs_decode` unwritten until a board runs it, and T2–T4 ship
  together with the board transcript as evidence.
- **Installable deps are NOT gates:** `libopencsd-dev` is added to
  `Dockerfile.hwtrace` (T1), never self-skipped around. It is a pinned distro
  package (the `libipt-dev`/`libncurses-dev` apt pattern — no
  `third-party-digests.txt` line, that file covers fetched tarballs/tags
  only).
- **Licensing:** OpenCSD is BSD-3-Clause; C++ core stays isolated in the
  linked library — `cs_backend.c` remains a C TU using only the C API, and
  the decoder links only into the hwtrace binaries/`libasmtest_hwtrace`,
  never core.
- **CI:** GitHub-hosted runners never run the live path; the x86-64 `hwtrace`
  job validates build + gating + reconstruction only. A recurring board lane
  is [self-hosted-ci-runners.md](self-hosted-ci-runners.md)'s scope — do not
  add runner config here.
- Repo conventions: `make fmt` before committing (CI gates `fmt-check`);
  Makefile edits go in `mk/native-trace.mk` where the targets live; commit to
  `main` and push per project memory.

## Research notes (verified 2026-07-17)

- **Ubuntu packaging.** The repo's lanes build FROM `ubuntu:24.04`
  ([Dockerfile.bindings-base](../../../Dockerfile.bindings-base) line 15).
  Noble ships `libopencsd-dev` **1.4.1-1build1** (runtime `libopencsd1`) —
  <https://packages.ubuntu.com/noble/libopencsd-dev>. Jammy's 1.2.0-1
  (<https://packages.ubuntu.com/jammy/libopencsd-dev>) fails perf's own build
  gate (`#error OpenCSD >= 1.2.1 is required`,
  <https://raw.githubusercontent.com/torvalds/linux/master/tools/build/feature/test-libopencsd.c>)
  — stay on noble. Upstream latest is 1.8.3; ETE support landed in 1.0.0 —
  <https://github.com/Linaro/OpenCSD>.
- **Split libraries.** noble's `libopencsd-dev` installs `libopencsd.so`
  (C++ core), `libopencsd_c_api.so` (the C API this backend calls), headers
  under `/usr/include/opencsd/{c_api,ete,etmv3,etmv4,ptm,stm}`, and
  `libopencsd.pc` — <https://packages.ubuntu.com/noble/arm64/libopencsd-dev/filelist>.
  The Debian `.pc` template's Libs line is `-lopencsd` **only** (no
  `_c_api`) — verified in the source package's `debian/libopencsd.pc.in`
  (<https://sources.debian.org/src/libopencsd/> → `debian/libopencsd.pc.in`)
  — which is why T1 appends `-lopencsd_c_api` explicitly.
- **C API entry points** (all confirmed in
  <https://raw.githubusercontent.com/Linaro/OpenCSD/master/decoder/include/opencsd/c_api/opencsd_c_api.h>):
  `ocsd_create_dcd_tree(src_type, deformatterCfgFlags)` /
  `ocsd_destroy_dcd_tree`; `ocsd_dt_create_decoder(handle, name,
  create_flags, cfg, &csid)`; `ocsd_dt_add_buffer_mem_acc(handle, address,
  mem_space, buf, len)`; `ocsd_dt_set_gen_elem_outfn(handle, fn, ctx)`;
  `ocsd_dt_process_data(handle, op, index, size, data, &consumed)`. Callback
  type `FnTraceElemIn(ctx, index_sop, trc_chan_id, elem)` —
  <https://raw.githubusercontent.com/Linaro/OpenCSD/master/decoder/include/opencsd/c_api/ocsd_c_api_types.h>.
- **Constants**
  (<https://raw.githubusercontent.com/Linaro/OpenCSD/master/decoder/include/opencsd/ocsd_if_types.h>):
  `OCSD_TRC_SRC_FRAME_FORMATTED`/`OCSD_TRC_SRC_SINGLE`; deformatter flags
  `HAS_FSYNCS 0x01`, `HAS_HSYNCS 0x02`, `FRAME_MEM_ALIGN 0x04` (ETR/ETB:
  16-byte-aligned frames, no syncs), `PACKED_RAW_OUT 0x08`,
  `UNPACKED_RAW_OUT 0x10`, `RESET_ON_4X_FSYNC 0x20`; decoder names
  `"ETMV4I"`, `"ETE"`, `"ETMV3"`, `"PTM"`, `"STM"`; create flags
  `PACKET_PROC 0x01`, `FULL_DECODER 0x02`; `OCSD_MEM_SPACE_ANY 0xFF`.
- **Configs.** `ocsd_etmv4_cfg {reg_idr0/1/2, reg_idr8..13, reg_configr,
  reg_traceidr, arch_ver, core_prof}` —
  <https://raw.githubusercontent.com/Linaro/OpenCSD/master/decoder/include/opencsd/etmv4/trc_pkt_types_etmv4.h>;
  `ocsd_ete_cfg` adds `reg_devarch` —
  <https://raw.githubusercontent.com/Linaro/OpenCSD/master/decoder/include/opencsd/ete/trc_pkt_types_ete.h>.
- **Generic elements**
  (<https://raw.githubusercontent.com/Linaro/OpenCSD/master/decoder/include/opencsd/trc_gen_elem_types.h>):
  UNKNOWN, NO_SYNC, TRACE_ON, EO_TRACE, PE_CONTEXT, INSTR_RANGE,
  I_RANGE_NOPATH, ADDR_NACC, ADDR_UNKNOWN, EXCEPTION, EXCEPTION_RET,
  TIMESTAMP, CYCLE_COUNT, EVENT, SWTRACE, SYNC_MARKER, MEMTRANS,
  INSTRUMENTATION, ITMTRACE, CUSTOM; element fields `st_addr`, `en_addr`
  (**exclusive**), `isa`, `last_i_type`/`subtype`, `last_instr_exec`,
  `last_instr_sz`, `num_instr_range`.
- **perf reference implementation.** perf builds the tree
  FRAME_FORMATTED-vs-SINGLE per queue from
  `PERF_AUX_FLAG_CORESIGHT_FORMAT_RAW`, flags
  `FRAME_MEM_ALIGN | RESET_ON_4X_FSYNC`, creates ETMV4I/ETE decoders from
  captured registers, and handles EO_TRACE/NO_SYNC/TRACE_ON as
  discontinuities, INSTR_RANGE as the executed run —
  <https://raw.githubusercontent.com/torvalds/linux/master/tools/perf/util/cs-etm-decoder/cs-etm-decoder.c>.
  Trace IDs arrive via `PERF_RECORD_AUX_OUTPUT_HW_ID` (v0 global; v0.1 adds
  sink IDs; `CS_AUX_HW_ID_TRACE_ID_MASK`/`SINK_ID_MASK`) and mixed
  formatted/unformatted trace is unsupported —
  <https://raw.githubusercontent.com/torvalds/linux/master/tools/perf/util/cs-etm.c>;
  ETMv4 metadata block registers (TRCCONFIGR/TRCTRACEIDR/TRCIDR0/1/2/8/
  TRCAUTHSTATUS/TS_SOURCE; ETE adds TRCDEVARCH) —
  <https://raw.githubusercontent.com/torvalds/linux/master/tools/perf/util/cs-etm.h>.
  perf uses a *callback* memory accessor; this backend uses the simpler
  buffer accessor over the registered region — equivalent for a fixed
  `[base, base+len)`.
- **Frame layout** (16-byte frames, ≤15 data bytes, even-byte LSB selects
  ID-vs-data, byte 15 auxiliary) — verified against OpenCSD's deformatter
  implementation
  <https://raw.githubusercontent.com/Linaro/OpenCSD/master/decoder/source/trc_frame_deformatter.cpp>
  (background only; the deformatter does this for us).
- **Kernel usage doc** (cs_etm PMU, sinks sysfs, `perf record -e
  cs_etm/@tmc_etr0/u --per-thread`) —
  <https://raw.githubusercontent.com/torvalds/linux/master/Documentation/trace/coresight/coresight.rst>.
- **Caveats to re-verify on the board / installed headers:** the frame layout
  was verified against OpenCSD's implementation, not ARM IHI0029 itself; the
  exact hex of `CS_AUX_HW_ID_*_MASK` and noble's `OCSD_VER_NUM` encoding were
  not independently fetched; `OCSD_GEN_TRC_ELEM_*` ordering was summarized
  from headers — **spot-verify names/values against the installed 1.4.1
  headers before hard-coding anything** (T3 steps say where).

## Out of scope

- **Intel PT work of any kind** — the one PT arm (open/mmap/drain/decode,
  window begin/end, escalation ladder) is
  [intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md);
  foreign-pid PT attach is
  [intel-pt-attach-foreign-pid.md](intel-pt-attach-foreign-pid.md).
- **A self-hosted AArch64 board CI runner** (recurring lane, security
  posture) — [self-hosted-ci-runners.md](self-hosted-ci-runners.md). This doc
  only produces the board-run transcript for its own landing series.
- **AArch64 ptrace single-step validation** (the non-CoreSight AArch64 route)
  — [aarch64-ptrace-single-step-validation.md](aarch64-ptrace-single-step-validation.md).
- **asmspy/CLI AArch64 support** —
  [asmspy-aarch64-support.md](asmspy-aarch64-support.md).
- **SVE state capture on AArch64** —
  [aarch64-sve-capture.md](aarch64-sve-capture.md).
- CoreSight **snapshot-mode** circular-ring decode (`opts.snapshot`) and
  multi-region hardware address filters: deliberate follow-ups, matching the
  PT backend's documented linear-ring-first scope (hwtrace.c lines
  2065–2066).
