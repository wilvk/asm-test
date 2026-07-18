# Pin probe-mode argument/return capture — implementation

> **Sources.** Actioned from
> [intel-pin-capabilities-plan.md](../plans/intel-pin-capabilities-plan.md)
> (track **PIN-3**) and
> [capture-args-returns.md](../analysis/capture-args-returns.md) (the "middle
> tier" design note). Written 2026-07-17. If this doc and a source disagree,
> this doc wins (sources may be stale); if the CODE and this doc disagree,
> re-verify before implementing. Repo pointers (paths, line numbers, make
> targets) were re-checked against the working tree on 2026-07-17.

## Why this work exists

We want to capture the **values** a function is called with and the value it
returns — the SysV integer/FP argument registers at entry, the return
register(s) plus flags at exit, and the buffers those arguments point at — for a
routine running at **native speed**. Intel Pin's *probe mode* does exactly this:
it splices a jump (a "probe") at the function's first bytes into a small
analysis trampoline, and the application's own code runs natively between
probes with **no software code cache**. That is stronger than an `LD_PRELOAD`
wrapper (which only sees dynamic-symbol calls) and far cheaper than full dynamic
binary instrumentation, and DynamoRIO has no supported equivalent (its old
hot-patch/probe API is unmaintained). The capture lands in the project's
existing value-trace record shape so it is diffable against the other value
producers; we prove it by diffing against the out-of-process ptrace stepper on
the same fixture, and we surface every probe-mode refusal (a function too short
or non-relocatable to probe) as an honest per-target skip with a reason.

Scope honesty: probe-style boundary trampolining is *approximable* without Pin,
so this is a robustness/ergonomics win, ranked below the SDE and XED-trace
tracks. And note DynamoRIO's external-attach path **is** wired in this repo for
the taint/dataflow tier (landed 2026-07-14,
[Dockerfile.taint-attach](../../../Dockerfile.taint-attach) +
`docker-dataflow-attach`); Pin's value here is the *native-code probe* model, not
"DR can't attach."

## What already exists (verified 2026-07-17)

The record shape and the independent producer to diff against are already in the
tree; **no Pin artifact exists yet** (`ls Dockerfile.*` shows no
`Dockerfile.pintool`; there is no `scripts/fetch-pin.sh`, no `PIN_VERSION`
anywhere outside docs, and no probe-mode Pintool source — the only Pin-related
commit is the docs commit `ca5ba2f`).

- [include/asmtest_valtrace.h](../../../include/asmtest_valtrace.h) — the shared
  value-trace substrate. `at_val_rec_t` (:61-86) is a plain-old-data operand
  record: `kind` (`AT_LOC_REG` / `AT_LOC_MEM_ABS` / `AT_LOC_MEM_OFF`), `reg`
  (Capstone reg id), `addr`, `size`, `is_write`, `value_valid`, `wide` /
  `wide_off`, an inline `value` for widths ≤ 8, and a `step`. `asmtest_valtrace_t`
  (:93-116) holds three caller-owned arrays (`insn_off`, `recs`, `wide`) with the
  honest append/`truncated` discipline. This is the record we fill.
- [src/ptrace_backend.c](../../../src/ptrace_backend.c) — the out-of-process
  single-step stepper. `read_pc_ret()` (:431) reads the tracee's PC, the integer
  **return register** (RAX on x86-64), and SP via `PTRACE_GETREGS`. This is the
  **independent producer** the plan names for the cross-check (T6): it is a
  wholly separate implementation, so agreement is real corroboration.
- [include/asmtest_taint_shm.h](../../../include/asmtest_taint_shm.h) — the
  cross-address-space hand-off **precedent** to copy. Its `at_shm_channel_t`
  (:50-68) embeds a `report` plus a **fixed** `hits[]` array and reads
  everything by **offset**, never by the stored pointer, because the segment maps
  at different virtual addresses in producer and consumer. Our shm channel
  mirrors this exactly (T2).
- [src/dataflow_helpers.c](../../../src/dataflow_helpers.c):55-61 — the precedent
  for **keeping Capstone reg ids as literals** in a translation unit that must
  not include the Capstone header (`HLP_X86_RAX = 35`, `HLP_X86_RDI = 39`,
  `HLP_X86_RSI = 43`). A Pintool is built against PinCRT and does not link
  Capstone, so it uses the same literal-id trick to fill `at_val_rec_t.reg`.
- [examples/attach_victim.c](../../../examples/attach_victim.c) — the pattern for
  a "program asm-test did not start": a `noinline` hot function
  (`hotfn(long n, long k)`) in a loop, with `prctl(PR_SET_PTRACER_ANY)` so a
  same-uid tracer attaches under a plain `docker run`. Our fixture (T2) mirrors
  this but adds FP args and a pointed-to buffer.
- [examples/test_dataflow_ptrace.c](../../../examples/test_dataflow_ptrace.c):62
  — the convention of `#define`-ing the Capstone reg ids a suite needs
  (`REG_XMM0 122`, `REG_YMM0 154`) rather than pulling in Capstone.
- Dependency-pinning precedent — copy this verbatim:
  [scripts/fetch-dynamorio.sh](../../../scripts/fetch-dynamorio.sh) (download →
  `tp_digest`/`tp_sha256` verify → capture license → echo home dir),
  [scripts/lib-thirdparty.sh](../../../scripts/lib-thirdparty.sh) (`tp_digest`,
  `tp_sha256`), the digest manifest
  [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt),
  the `ARG DR_VERSION` + `curl` + `ENV` block in
  [Dockerfile.drtrace](../../../Dockerfile.drtrace):34-41, and the license table
  in [licenses/README.md](../../../licenses/README.md).

**Prove the baseline is green before touching anything** (this repo prefers
Docker lanes — [CLAUDE.md](../../../CLAUDE.md); `make help` lists everything):

```
make check                 # framework self-tests all PASS
make docker-drtrace        # the closest existing fetched-and-pinned lane; ends 0
```

`make check` passing tells you the pure value-trace spine compiles and the tree
is clean; `docker-drtrace` proves the fetch-verify-license pattern you are about
to copy still works end to end.

## Tasks

### T1 — Reuse the shared Pin substrate from the XED trace tier  (S, depends on: none; the substrate is authored by `pin-xed-trace-tier.md#T1`/`#T2`)

**Goal.** The pinned Pin kit and everything around it — `scripts/fetch-pin.sh`,
the `pin` digest line in
[scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt), the
vendored license + `licenses/README.md` row, `Dockerfile.pintool`, and the
`docker-pintool` lane in [mk/docker.mk](../../../mk/docker.mk) — are **created
once by the XED trace tier** ([pin-xed-trace-tier.md](pin-xed-trace-tier.md#T1)
for the fetch script, digest, and license; its `#T2` for the Dockerfile and
lane), pinned at `PIN_VERSION=4.2-99776-g21d818fa2`. This task **authors none of
them**: it confirms that substrate is present at 4.2 and reuses it. This doc's
downstream tasks (T2–T7) build on those artifacts.

**Converge on 4.2 — no second Pin version.** Probe mode needs only a working Pin
kit and its bundled XED; Pin 4.2 bundles XED v2026.02.17, which decodes APX and
everything older, so it covers **every probe-mode need** and there is no reason
to pin an older 3.31 kit alongside it. The two lanes share **one** pinned
version. (Pin 3.31 is recorded as a fallback in Research notes only; do not add a
second digest line, fetch script, or Dockerfile.)

**Steps.** None — the substrate is owned by `pin-xed-trace-tier.md#T1`/`#T2`,
which the cross-doc etiquette makes the single author of these files. The only
action here is to verify, before this doc's later tasks run, that
`scripts/fetch-pin.sh`, `Dockerfile.pintool`, the `docker-pintool` rule, and the
`pin` digest line exist at `PIN_VERSION=4.2-99776-g21d818fa2`. If the XED tier
has not landed yet, that doc is the sole authority for building them — coordinate
so the substrate is built once, not duplicated. (This doc's T6 later wires its
own `pin-probe-test` invocation and `--cap-add=SYS_PTRACE` into the shared
`docker-pintool` lane; that is a consumer of the lane, not a second author of it.)

**Code.** None. The `.so` this doc's later tasks produce is **never** linked into
`libasmtest`/`libasmtest_emu` or any binding package (Pin is proprietary
freeware — test/oracle-only), exactly as DynamoRIO is handled.

**Tests.** No new artifact. Sanity against the shared substrate:
`sh scripts/fetch-pin.sh` echoes the kit root and vendors `licenses/Pin-4.2-*.txt`;
`make docker-pintool` builds the image with its in-image digest gate firing.
These are the XED tier's own Done-when checks — this doc relies on them.

**Docs.** The CHANGELOG `Added` bullet and the `licenses/README.md` Pin row are
authored by the XED tier (`pin-xed-trace-tier.md#T1`/`#T9`); this doc adds no
duplicate.

**Done when.**
- `scripts/fetch-pin.sh`, `Dockerfile.pintool`, and the `docker-pintool` rule
  exist at `PIN_VERSION=4.2-99776-g21d818fa2` (built by
  `pin-xed-trace-tier.md#T1`/`#T2`).
- Exactly **one** Pin version is pinned across both lanes; no duplicated fetch
  script, Dockerfile, lane, or digest line.

### T2 — Fixed-layout value-trace shm channel + a capture fixture  (S, depends on: T1)

**Goal.** A `include/asmtest_valtrace_shm.h` the Pintool (producer) and the
out-of-process validator (consumer) both agree on, read strictly by offset; plus
a native fixture function with integer args, an FP arg, an integer+FP return,
and a pointed-to buffer.

**Steps.**
1. Create `include/asmtest_valtrace_shm.h` mirroring
   [asmtest_taint_shm.h](../../../include/asmtest_taint_shm.h) shape and its
   CROSS-PROCESS RULE banner (map at different addresses → **read by offset,
   never by stored pointer**). Embed a fixed layout:

   ```c
   #define AV_SHM_NAME       "/asmtest_valtrace_pin"
   #define AV_SHM_RECS_CAP   64      /* entry args + exit results, ample     */
   #define AV_SHM_WIDE_CAP   8192    /* two 4 KiB pointer buffers (T4)        */
   typedef struct av_shm_channel {
       volatile uint32_t done;       /* 0 -> 1 (release) once exit captured  */
       uint32_t pad0;
       int64_t  result;             /* the fixture's return (liveness)       */
       uint32_t recs_len;           /* records actually written (<= CAP)     */
       uint32_t wide_len;           /* bytes used in wide[]                   */
       uint32_t truncated;          /* a cap overflowed                      */
       uint32_t skip;               /* 0 = captured; else an AV_SKIP_* reason (T5) */
       at_val_rec_t recs[AV_SHM_RECS_CAP];
       uint8_t      wide[AV_SHM_WIDE_CAP];
   } av_shm_channel_t;
   ```

   `at_val_rec_t` is plain scalars, so it is shm-safe verbatim. The validator
   rebuilds an `asmtest_valtrace_t` from `recs[]`/`wide[]` by offset for the diff.
2. Add `examples/pin_probe_victim.c` mirroring
   [examples/attach_victim.c](../../../examples/attach_victim.c): `prctl(PR_SET_PTRACER_ANY)`,
   print `pid` + the function address to stderr, then loop calling a `noinline`
   target. The target signature exercises the capture surface:
   `long capref(long a, long b, double d, const char *buf)` — SysV places `a`→RDI,
   `b`→RSI, `d`→XMM0, `buf`→RDX; it returns `a + b + (long)d + buf[0]` in RAX (and
   set XMM0 to a known FP value so the FP-return path is exercised too). `buf`
   points at a small fixed ASCII buffer so T4 has something to dereference.

**Code.** One header, one fixture `.c`. The reg ids the Pintool writes into
`at_val_rec_t.reg` are Capstone ids as literals (T3), matching the ptrace
producer's id space so the two traces compare field-for-field.

**Tests.** No standalone test; the header is validated by T6's diff and the
fixture is its input. A compile check belongs in T6's target.

**Docs.** Internal-only (a test channel header + a fixture); no user-facing page.

**Done when.**
- `include/asmtest_valtrace_shm.h` compiles under `-std=c11 -Wall -Werror` when
  included by a plain C TU that also includes `asmtest_valtrace.h`.
- `examples/pin_probe_victim.c` builds and, run standalone, prints its pid +
  `capref` address and loops.

### T3 — Probe-mode entry/exit capture Pintool  (M, depends on: T2)

**Goal.** A Pintool that, in probe mode, records the SysV integer/FP argument
registers of a named function at entry and its return register(s)+flags at exit,
into the T2 shm channel as `at_val_rec_t` records.

**Steps.**
1. Create `tools/pintool/` (new dir) with `probe_capture.cpp` and a
   `makefile.rules`, seeded from the kit's `source/tools/MyPinTool/` template
   (SPDX-MIT scaffolding — safe to derive from; see Research notes). Build with
   `make PIN_ROOT=/opt/pin obj-intel64/probe_capture.so` (the kit's external-tool
   contract).
2. In `main()`: `PIN_Init`, register an `IMG_AddInstrumentFunction` callback, then
   `PIN_StartProgramProbed()` (probe mode requires it; it never returns). Take the
   target function name from a knob (`-func capref`) and the shm name from a knob
   (`-shm /asmtest_valtrace_pin`); `shm_open`+`mmap` the channel once at startup.
3. In the IMG callback, `RTN_FindByName(img, funcname)`; if found, open the RTN,
   and **before** requesting probes run the safety pre-checks (T5). If safe,
   request two probed insertions:
   - `RTN_InsertCallProbed(rtn, IPOINT_BEFORE, (AFUNPTR)OnEntry, IARG_REG_VALUE,
     REG_RDI, …, REG_RSI, REG_RDX, REG_RCX, REG_R8, REG_R9, IARG_CONTEXT,
     IARG_END)` — the six SysV integer arg regs by value, plus `IARG_CONTEXT` to
     reach XMM0-XMM7 (probe mode allows `IARG_CONTEXT`; the segment-base regs it
     exposes are meaningless, which we do not read).
   - `RTN_InsertCallProbed(rtn, IPOINT_AFTER, (AFUNPTR)OnExit, IARG_PROTOTYPE,
     proto, IARG_REG_VALUE, REG_GAX, IARG_REG_VALUE, REG_GDX, IARG_CONTEXT,
     IARG_END)` — RAX/RDX return regs + `IARG_CONTEXT` for XMM0/XMM1 and RFLAGS.
     `IPOINT_AFTER` requires an `IARG_PROTOTYPE` (build it with `PROTO_Allocate`).
     Note `IARG_RETURN_REGS`, `IARG_REG_REFERENCE`, `IARG_CONST_CONTEXT`,
     `IARG_PARTIAL_CONTEXT`, and `IARG_THREAD_ID` are **unsupported** in probe
     mode — do not use them.
4. `OnEntry` writes one `at_val_rec_t` per arg register into `chan->recs[]`:
   `kind = AT_LOC_REG`, `reg` = the **Capstone** `X86_REG_*` id (map each Pin
   `REG_*` to its Capstone id via a small static Pin→Capstone table — the
   literal-id trick from
   [src/dataflow_helpers.c](../../../src/dataflow_helpers.c):57-61, which pins
   `X86_REG_RAX = 35`, `X86_REG_RDI = 39`, `X86_REG_RSI = 43` and **only** those
   three; [examples/test_dataflow_ptrace.c](../../../examples/test_dataflow_ptrace.c):62-64
   pins `X86_REG_XMM0 = 122`, `X86_REG_YMM0 = 154`, `X86_REG_R10 = 108`). The
   other ids this tool needs are **not** in either of those files, so use the
   values below — verified against the Capstone 5.0.1 `x86_reg` enum and
   cross-checked against the in-tree anchors just cited (`RAX=35`/`RDI=39`/`RSI=43`
   from `dataflow_helpers.c`; `R10=108` pins the contiguous `R8..R15` run, and
   `XMM0=122` pins `XMM1`):

   | reg | Capstone id | reg    | Capstone id |
   |-----|-------------|--------|-------------|
   | RCX | 38          | R9     | 107         |
   | RDX | 40          | EFLAGS | 25          |
   | R8  | 106         | XMM1   | 123         |

   Record these as verified literals in the Pin→Capstone table — **do not guess
   the numeric values**; the whole point of matching ids is that this trace
   compares field-for-field with the ptrace producer's id space. (These are
   ABI-stable enum values, so the literal table is authoritative and needs no
   header at Pintool build time — which is deliberate, since the `Dockerfile.pintool`
   image from T1 carries no Capstone. If you want to re-derive them rather than
   trust the table, `capstone/x86.h` is **not** in the tree: Capstone is built
   from pinned source, commit `097c04d`,
   [scripts/build-capstone.sh](../../../scripts/build-capstone.sh), which installs
   the header to `/usr/local/include/capstone/x86.h`; or read the enum from the
   pinned upstream 5.0.1 tag.) Set `is_write = false`, `value_valid = true`,
   the ≤8-byte value inline. Read XMM values via
   `PIN_GetContextRegval(ctxt, REG_XMM0, ...)` into the `wide[]` side buffer
   (size 16, `wide = true`, `wide_off` set) since they exceed 8 bytes.
5. `OnExit` writes RAX (and RDX for a 128-bit return), XMM0/XMM1, and an RFLAGS
   record (`reg` = Capstone `EFLAGS`), all `is_write = true`; sets
   `chan->recs_len`, `chan->result` (RAX), then publishes `chan->done = 1` with a
   release store (`__atomic_store_n(&chan->done, 1, __ATOMIC_RELEASE)`), the same
   handshake [asmtest_taint_shm.h](../../../include/asmtest_taint_shm.h):47-49
   documents.

**Code.** C++ Pintool (PinCRT: `-nostdlib`, no libc — use Pin's own facilities
and raw `syscall`/`mmap` via the kit's headers for shm; the kit's OS-APIs cover
`OS_OpenFD`/`OS_MMap`). Filling `at_val_rec_t` needs no Capstone — the reg ids are
literals. Log every decision via Pin `LOG()` (goes to `pintool.log`).

**Tests.** Exercised by T6 (there is no way to unit-test a Pintool in isolation
without a running Pin). The intermediate proof is a manual run:
`/opt/pin/pin -probe -t obj-intel64/probe_capture.so -func capref -- ./pin_probe_victim`
fills the shm channel; a tiny reader prints `recs_len` ≥ 7 and `result` equal to
`capref`'s computed return. A **pass** shows the RDI/RSI/RDX/XMM0 entry values
matching the fixture's known args; a **failure** shows `recs_len == 0` or a wrong
`result`.

**Docs.** Internal-only (a Pintool); user-facing summary lands in T7.

**Done when.**
- The Pintool builds: `make PIN_ROOT=/opt/pin obj-intel64/probe_capture.so`.
- Run under `pin -probe` against `pin_probe_victim`, the shm `done` flips to 1 and
  `recs[]` holds the entry args + exit return/flags with correct values.

### T4 — Pointed-to buffer capture: 4 KiB cap + mapped-range validation  (S, depends on: T3)

**Goal.** When an argument is a pointer, capture up to a configurable cap
(default **4 KiB**, per the design note) of the buffer it points at, and **never
fault**: validate the pointer against the target's mapped ranges first and refuse
an invalid pointer instead of dereferencing it.

**Steps.**
1. Add a knob `-ptrcap 4096` (bytes) and, in `OnEntry`, for the argument
   designated a pointer (the fixture's `buf` in RDX), read the buffer into
   `chan->wide[]` as an `AT_LOC_MEM_ABS` record (`addr` = the pointer value,
   `size` = min(cap, bytes-until-page-end), `is_write = false`,
   `value_valid = true`, `wide`/`wide_off` set).
2. **Validate before reading.** The design note is explicit: *never trust pointer
   validity; validate against the target's mapped ranges and cap the read*
   ([capture-args-returns.md](../analysis/capture-args-returns.md):75-77,105-108).
   In probe mode the application runs in-process with the tool, so validate with
   `PIN_SafeCopy` / `PIN_SafeCopyEx` (which returns the number of bytes actually
   copied and never faults) rather than a raw `memcpy` — a short/zero return means
   the pointer is bad, so record a **refused** buffer (a zero-length MEM record
   with a `skip`-style note), not a fault.
3. Cap the copy at `-ptrcap` and clamp to the end of the containing page so a
   valid-but-short mapping cannot over-read.

**Code.** Extends `OnEntry` in `probe_capture.cpp`. The cap and the page-clamp
are the two invariants; `PIN_SafeCopy`'s byte-count is the validity oracle.

**Tests.** In T6's fixture, pass one **valid** `buf` (captured, first bytes match)
and one **deliberately invalid** pointer (e.g. `(const char*)0x1`) in a second
call; assert the valid buffer is captured within the cap and the invalid one is
**refused, not faulted** (the process does not crash; the record is zero-length).
A pass: both calls return, valid buffer bytes present; a failure: a SIGSEGV in
the victim or a non-empty record for the bad pointer.

**Docs.** The 4 KiB cap and "never trust a pointer" posture are restated in T7's
user-facing note.

**Done when.**
- A valid pointer argument is captured up to 4 KiB and its bytes match the
  fixture.
- An invalid pointer is refused via `PIN_SafeCopy` returning short — no fault, a
  zero-length record — proven by the victim exiting cleanly.

### T5 — Detect probe-mode refusals; surface them as per-target skips with a reason  (M, depends on: T3)

**Goal.** A function Pin cannot probe (too short to hold a 14-byte probe, or
non-relocatable) is reported as an explicit per-target **skip with a reason**,
never a silent miss — the repo's honesty rule.

**Steps.**
1. **Pre-check** before every probe request (Pin's insertion APIs return `VOID`
   with **no** error path, so pre-checks are the only gate on the insertion side):
   `RTN_IsSafeForProbedInsertion(rtn)` / `...Ex(rtn, mode)` before
   `RTN_InsertCallProbed`. Their documented caveat — the check *"does not
   guarantee it is safe … it merely indicates that certain conditions are not
   present"* — means the pre-check is necessary but not sufficient; keep the
   post-check too.
2. If you switch from insertion to `RTN_ReplaceSignatureProbed` for a signature
   you want to wrap, **post-check** its return: it returns the relocated original
   entry, or **NULL on failure**. `RTN_ReplaceProbed` likewise returns NULL on
   failure. A NULL return is a definitive refusal.
3. Distinguish *in-place* from *relocation* refusals with the `PROBE_MODE` enum:
   request `...Ex(rtn, PROBE_MODE_DEFAULT, …)` first (in-place only,
   `PROBE_MODE_DEFAULT = 0` forbids relocation). If that is refused, retry with
   `PROBE_MODE_ALLOW_RELOCATION (1<<0)` — Pin keeps the probe in place if the
   first basic block is long enough, else relocates the whole routine (only when
   the size is known, no jumps leave the function, and there are no indirect
   jumps; relocation is **not** supported on Windows and *"may destabilize the
   application"*). If **both** are refused, that is the terminal per-target skip.
4. **Synthesize the reason** (Pin exposes no refusal reason-code API — see
   Research notes): compose a human string from what you *can* observe —
   `RTN_Size(rtn)` (too short: below the 14-byte probe / 5-7-byte insertion
   floor), which pre-check failed (in-place vs relocation), and a NULL Replace*
   return. Write it via Pin `LOG()` **and** set `chan->skip` to an enum
   (`AV_SKIP_TOO_SHORT`, `AV_SKIP_NOT_RELOCATABLE`, `AV_SKIP_NOT_FOUND`) so the
   out-of-process validator (T6) can print `# SKIP: <func>: <reason>` in TAP.
5. Add a second fixture function `examples/pin_probe_victim.c` that is
   *deliberately un-probeable* — a tiny leaf `long tiny(void){return 1;}`
   compiled so its body is a couple of bytes with an immediate jump target at its
   start — and target it in one T6 sub-run to prove the skip path fires.

**Code.** A `probe_or_skip(RTN)` helper in `probe_capture.cpp` returning an
`AV_SKIP_*` code; the fixture gains the `tiny` leaf.

**Tests.** T6 runs the tool against both `capref` (captured) and `tiny`
(skipped). A **pass**: `capref` yields a full record set and `chan->skip == 0`;
`tiny` yields `chan->skip == AV_SKIP_TOO_SHORT` and the validator prints
`# SKIP` — **not** an empty pass and **not** a crash. A **failure**: `tiny`
silently produces zero records with `skip == 0` (a silent miss — the exact thing
this task forbids).

**Docs.** T7 documents that refusals are reported, and that Pin publishes no
machine-readable refusal reason, so the tool synthesizes one.

**Done when.**
- `capref` captures; `tiny` reports a concrete `AV_SKIP_*` reason.
- The validator distinguishes "captured", "skipped with reason", and "crashed",
  and only the first counts as a pass.

### T6 — Cross-check Pin capture against the ptrace `read_pc_ret` path; wire the lane  (M, depends on: T3, T4, T5)

**Goal.** Pin's captured entry-args and exit-return/flags for `capref`
**agree** with the out-of-process ptrace stepper reading the same function on the
same inputs — two independent producers must match — packaged as a
`make pin-probe-test` target that runs in `docker-pintool` and self-skips off
x86.

**Steps.**
1. Add `examples/pin_probe_validator.c`: `shm_open`+`mmap` the channel, spin on
   `chan->done` (acquire), then rebuild an `asmtest_valtrace_t` from `recs[]` /
   `wide[]` by **offset** (never a stored pointer — the
   [asmtest_taint_shm.h](../../../include/asmtest_taint_shm.h) rule).
2. Independently capture the reference: attach to `pin_probe_victim` with the
   same flow as [examples/attach_trace.c](../../../examples/attach_trace.c)
   (`asmtest_ptrace_available` guard → `PTRACE_ATTACH` → `asmtest_ptrace_run_to(pid,
   base)`). `base` is the `capref` **runtime address in the victim's address
   space**, which the validator gets by parsing the address the victim already
   prints to stderr at startup (T2 step 2) — *not* `&capref`: `capref` is defined
   only in `pin_probe_victim.c`, so `&capref` neither exists in the separate
   `pin_probe_validator.c` TU nor names the victim's runtime address. This mirrors
   `attach_trace.c`, which passes a resolved `base`
   ([examples/attach_trace.c](../../../examples/attach_trace.c):96), never `&fn`.
   Read the **entry** integer args with `PTRACE_GETREGS` at the
   `run_to` stop, plant a breakpoint at the return address and read the **exit**
   RAX via the same `read_pc_ret()` path
   ([src/ptrace_backend.c](../../../src/ptrace_backend.c):431). RAX and the six
   integer arg registers are what both producers can see; assert they are equal.
   (XMM/flags agreement is a bonus assert where both paths capture it; the
   *required* diff is the integer arg + return set `read_pc_ret` covers.)
3. Add `pin-probe-test` to [mk/native-trace.mk](../../../mk/native-trace.mk),
   modeled on `drtrace-test` (:136-161): gate on `PIN_ROOT` being set and the host
   being x86-64; when absent, print `== pin-probe-test ==` then
   `1..0 # skipped` with a clear reason (the `drtrace-test` self-skip shape,
   :138-142). When present, build the victim/validator, build the Pintool
   (`make PIN_ROOT=$(PIN_ROOT) -C tools/pintool`), run
   `$(PIN_ROOT)/pin -probe -t …/probe_capture.so -func capref -- pin_probe_victim`
   with the validator draining the shm, and emit TAP (`ok`/`not ok`/`# SKIP`).
4. Wire `docker-pintool` (T1) to run `pin-probe-test`. It needs
   `--cap-add=SYS_PTRACE` for the ptrace reference producer, exactly as the taint
   attach lanes do ([mk/docker.mk](../../../mk/docker.mk):289 uses
   `--cap-add=SYS_PTRACE`).
5. Add `pin-probe-test` to `make help`'s optional-tiers block
   ([Makefile](../../../Makefile):115-121) with a one-line description and its
   x86-only / needs-`PIN_ROOT` gate.

**Code.** `pin_probe_validator.c` (ordinary C, links the ptrace stepper objects
the way [examples/attach_trace.c](../../../examples/attach_trace.c) is linked in
[mk/native-trace.mk](../../../mk/native-trace.mk):2177) + the make target.

**Tests.** This *is* the test lane. A **pass**: every integer arg + the RAX
return captured by Pin equals the ptrace-observed value → `ok N`. A **failure**:
any mismatch → `not ok N` with both values printed. A clean **skip**: no
`PIN_ROOT` or a non-x86 host → `1..0 # skipped` with a reason, exit 0.

**Docs.** T7.

**Done when.**
- `make pin-probe-test PIN_ROOT=/opt/pin` on x86-64 prints `ok` lines for the
  arg/return agreement and exits 0.
- `make docker-pintool` runs it green in the container.
- On aarch64 / macOS-arm64, or with `PIN_ROOT` unset, the lane prints
  `1..0 # skipped` with a reason and exits 0 (never a hard failure).

### T7 — Safety posture, changelog, and user-facing note  (S, depends on: T6)

**Goal.** The pointer-safety and privacy posture the design note demands is
**documented, not just coded**, and the lane is discoverable.

**Steps.**
1. Add a short section to
   [docs/guides/tracing/data-flow.md](../../../docs/guides/tracing/data-flow.md)
   (or a new `docs/guides/tracing/` page linked from
   [index.md](../../../docs/guides/tracing/index.md)) describing the Pin
   probe-mode capture lane: what it captures (SysV int/FP args, return+flags,
   pointed-to buffers), the **4 KiB default cap**, that pointers are validated
   against mapped ranges and **never trusted** (`PIN_SafeCopy`), that captured
   buffers may contain secrets and are sensitive artifacts
   ([capture-args-returns.md](../analysis/capture-args-returns.md):104-108), that
   the lane is **x86-only** and **test/oracle-only** (Pin is proprietary freeware,
   never shipped), and that probe-mode refusals are reported with a reason.
   Keep it warning-clean (`docs` builds with `-W`).
2. Append a `## [Unreleased]` `Added` bullet to
   [CHANGELOG.md](../../../CHANGELOG.md) for the probe-mode capture lane (or fold
   into T1's shared bullet).

**Code.** Docs only.

**Tests.** `make docs` (or `make docker-docs`) builds warning-free.

**Docs.** This task *is* the docs.

**Done when.**
- `make docker-docs` builds with no Sphinx warnings and the new page renders.
- `CHANGELOG.md` has the `Added` bullet under `## [Unreleased]`.

## Task order & parallelism

T1 (shared Pin substrate) is the root and is **shared with**
`pin-xed-trace-tier.md#T1` — coordinate so it is built once. T2 depends on T1.
T3 depends on T2. T4 and T5 both extend T3 and are **independent of each other**
(one person can take buffer capture, another the refusal path). T6 needs T3+T4+T5.
T7 is docs after T6.

```
T1 ── T2 ── T3 ──┬── T4 ─┐
                 └── T5 ─┴── T6 ── T7
```

Critical path: T1 → T2 → T3 → (T4 ∥ T5) → T6 → T7.

## Constraints & gates

- **License / never-ship.** Pin is proprietary freeware under the Intel
  Simplified Software License (binary-only; no modification/reverse-engineering
  of the Pin binary). The Pintool `.so` is **test/oracle-only** and never linked
  into `libasmtest`, `libasmtest_emu`, or any binding package — the same handling
  DynamoRIO gets. No public tier symbol, so the bindings-parity gate is untouched.
  The kit's tool scaffolding (`MyPinTool` makefile, `source/tools/Config/*`)
  carries SPDX-MIT headers, so deriving our tool makefile from it is fine; the
  bundled XED is Apache-2.0.
- **Pinning.** Follow the DynamoRIO pattern verbatim: `ARG PIN_VERSION` + `curl`
  + a SHA-256 line in
  [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt) +
  vendored license text. The fetch script **fails loudly** on a missing/mismatched
  digest (per [CLAUDE.md](../../../CLAUDE.md): a missing installable dependency is
  *added and pinned*, never self-skipped).
- **Real gates (self-skip is legitimate).** Pin is **x86-only**, so aarch64 /
  macOS-arm64 hosts self-skip with a reason. When `PIN_ROOT` is unset the lane
  self-skips (before fetching). These are the only clean self-skips.
- **Per-target refusals are NOT hardware gates.** A function too short or
  non-relocatable is a **per-target skip with a reason** (T5), recorded in TAP —
  distinct from the whole-lane x86 gate.
- **What to record when blocked.** If Pin cannot even be fetched in an
  environment (no network to `software.intel.com`), record it and let the lane
  self-skip on absent `PIN_ROOT` — do not narrow the feature.

## Research notes (verified 2026-07-18)

Externally verified; cite these rather than re-researching.

- **Pin kit + digest.** This lane **pins the same kit as the XED trace tier —
  Pin 4.2 (build 99776-g21d818fa2)** (2026-03-15),
  `.../pin-external-4.2-99776-g21d818fa2-gcc-linux.tar.gz`, self-computed SHA-256
  `194a2cec51678203452ece0d9e8cbb1819eb6e1221f0341091c49248f384d869`. The older
  last-of-3.3x kit, **Pin 3.31 (build 98869-gfa6f126a8)** —
  `https://software.intel.com/sites/landingpage/pintool/downloads/pin-external-3.31-98869-gfa6f126a8-gcc-linux.tar.gz`
  (HTTP 200, 32,994,324 bytes, Last-Modified 2024-08-26), self-computed SHA-256
  `82216144e3df768f0203b671ff48605314f13266903eb42dac01b91310eba956` — is a
  **recorded fallback only**; do not pin a second Pin version for probe mode.
  Intel publishes **no** official SHA-256 for these tarballs anywhere found; both
  hashes were computed from the official URLs and are otherwise uncorroborated —
  re-compute at pin time. Either kit works for probe mode (the API surface is
  stable across 3.x→4.x), so the tier converges on 4.2. Sources:
  `https://en.wikipedia.org/wiki/Pin_(computer_program)`,
  `https://aur.archlinux.org/packages/pin`, and the tarball URLs above.
- **License.** Inside both kits: `licensing/intel-simplified-software-license.txt`
  = "Intel Simplified Software License (Version October 2022)" — binary-only,
  redistribution with notice allowed, **no** modification/reverse-engineering of
  the Pin binary. `licensing/third-party-programs.txt`: bundled XED is Apache-2.0.
  Tool scaffolding carries SPDX-MIT, so shipping a tool makefile derived from it
  is fine.
- **Build system.** External-kit contract (from the kit's
  `source/tools/MyPinTool/makefile`): specify `PIN_ROOT` on the make invocation
  pointing at the kit root; the makefile then includes
  `$(PIN_ROOT)/source/tools/Config/makefile.config`, your `makefile.rules`, and
  `$(TOOLS_ROOT)/Config/makefile.default.rules`. So: copy `MyPinTool`, edit
  `makefile.rules`, run `make PIN_ROOT=/opt/pin obj-intel64/<tool>.so`. PinCRT
  flags baked into `makefile.unix.config`: `-nostartfiles -nodefaultlibs
  -nostdlib`, `-DPIN_CRT=1`, `-fno-exceptions -fno-rtti`, tool link `-shared
  -Wl,--hash-style=sysv`.
- **Probe mechanics.** A probe is a jump at the routine's start; the first
  instructions are relocated so a replacement can call the original.
  "A probe may be up to **14 bytes** long"; the tool must guarantee no jump
  target lands where the probe is placed and no thread is in the probed bytes
  (insert at image load; Pin removes probes on image unload). Probe mode needs
  `PIN_StartProgramProbed()`. Source: Pin 4.0.0 User Guide "Replacing a Routine in
  Probe Mode",
  `https://software.intel.com/sites/landingpage/pintool/docs/99633/Pin/doc/html/index.html`
  (via `https://web.archive.org/web/20260706231407/`).
- **Control / RTN / IARG APIs** (Pin 3.26 doc 98690, snapshot 2023-03-02, API
  surface identical in 4.0's probe example):
  `PIN_StartProgramProbed()` (never returns), `PIN_IsProbeMode()`,
  `PIN_IsSafeForProbedInsertion(ADDRINT)` (instruction ≥ 5/7 bytes, not
  control-flow, no memory operand). `RTN_InsertCallProbed(Ex)` returns **VOID —
  no failure indication**, so call `RTN_IsSafeForProbedInsertion(Ex)` first;
  `IPOINT_AFTER` needs an `IARG_PROTOTYPE`. `RTN_ReplaceSignatureProbed(Ex)`
  returns the relocated original entry or **NULL on failure**;
  `RTN_ReplaceProbed(Ex)` same-signature, also NULL on failure.
  `RTN_ReplaceSignatureProbed` and all `IsSafeForProbed*` checks are **Linux/
  Windows only**. `PROBE_MODE_DEFAULT = 0` forbids relocation;
  `PROBE_MODE_ALLOW_RELOCATION = (1<<0)` keeps in-place if the first basic block
  is long enough, else relocates the whole routine (needs known size, no jumps
  out, no indirect jumps; not supported on Windows; "may destabilize the
  application"). Probe-mode **unsupported IARGs**: `IARG_RETURN_REGS`,
  `IARG_REG_REFERENCE`, `IARG_CONST_CONTEXT`, `IARG_PARTIAL_CONTEXT`,
  `IARG_THREAD_ID`; with `IARG_CONTEXT` the seg-base / `REG_INST_G*` values are
  meaningless. Sources:
  `.../docs/98690/Pin/doc/html/group__RTN.html`,
  `.../group__PIN__CONTROL.html`, `.../group__INST__ARGS.html` (all via
  `https://web.archive.org/web/20230302144232/`).
- **Refusal detection has no reason-code API.** Pin emits **no** documented
  diagnostic or reason code when a probe request is refused
  (`RTN_InsertCallProbed` is `VOID`). So the plan requirement "refusals report a
  reason" is met by **synthesizing** the reason from: the `BOOL`
  `RTN_IsSafeForProbed*` pre-checks (in-place vs `ALLOW_RELOCATION` variants), a
  NULL `Replace*Probed` return, `RTN_Size`, and `PIN_IsSafeForProbedInsertion`
  (T5) — there is no Pin API that returns a refusal string.
- **APX / decoder currency (context, mostly PIN-2's concern).** The pinned Pin
  4.2 bundles XED v2026.02.17, which defines `XED_EXTENSION_APXEVEX` /
  `XED_EXTENSION_APXLEGACY` and **decodes APX**; the older 3.31 fallback bundles
  XED v2024.05.20, whose headers already define the same enums — so even the last
  3.3x kit decodes APX, and 4.2 certainly does. DynamoRIO
  APX support (`DR #6226`) is still **open** as of 2026-07-18; VNNI (`DR #5440`)
  is **closed/fixed** (2022-04-25, merged PR #5444) and works in the repo's pinned
  DR 11.91.20630 — do not repeat the stale "VNNI still breaks" claim. Sources:
  `https://github.com/intelxed/xed/releases`,
  `https://github.com/DynamoRIO/dynamorio/issues/6226`,
  `https://github.com/DynamoRIO/dynamorio/pull/5444`.

## Out of scope

- The shared Pin substrate — `scripts/fetch-pin.sh`, `Dockerfile.pintool`, the
  `docker-pintool` lane, the `pin` digest line, and the `licenses/README.md` Pin
  row, all pinned at `PIN_VERSION=4.2-99776-g21d818fa2` — is **created and owned
  by the XED trace tier**: [pin-xed-trace-tier.md](pin-xed-trace-tier.md#T1) `#T1`
  for the fetch script + digest + license, its `#T2` for the Dockerfile + lane. This
  doc's T1 is a **pure reuse** of that substrate and authors none of those files.
- The **XED-decoded control-flow trace tier** (block/insn offsets into
  `asmtest_trace_t`, APX decode where DR aborts) is
  [pin-xed-trace-tier.md](pin-xed-trace-tier.md).
- The **SDE future-ISA lane** (running suites under `sde64 -future`) is
  [pin-sde-future-isa-lane.md](pin-sde-future-isa-lane.md).
- The **libdft64 taint differential oracle** on Pin is
  [pin-libdft-taint-oracle.md](pin-libdft-taint-oracle.md).
- Correctness of the ptrace stepper this diffs against (int3/rep/re-entrancy) is
  [ptrace-blockstep-tracer-correctness.md](ptrace-blockstep-tracer-correctness.md);
  this doc only *consumes* `read_pc_ret`.
- The L1 def-use / L2 slice analysis over the captured trace, and any managed-code
  GC-move canonicalization of the addresses, are the dataflow docs
  ([dataflow-producer-correctness.md](dataflow-producer-correctness.md),
  [dataflow-f4-object-identity.md](dataflow-f4-object-identity.md)); the capture
  here only fills L0 records.
- AArch64 argument/return capture (SVE and AAPCS64) is not a Pin concern (Pin is
  x86-only); see [aarch64-sve-capture.md](aarch64-sve-capture.md).
