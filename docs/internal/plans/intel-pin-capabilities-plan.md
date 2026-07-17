# asm-test — Intel Pin capabilities: one umbrella, four separable plans

> **Context (2026-07-17).** This plan actions the surviving items of
> [2026-07-17-intel-pin-vs-dynamorio.md](../analysis/2026-07-17-intel-pin-vs-dynamorio.md)
> — the investigation into what Intel Pin makes possible that the shipped
> DynamoRIO tier cannot. Four items survived; each is a **self-contained plan
> track** below (`PIN-1..PIN-4`), ordered by how squarely it is a DynamoRIO
> *impossibility* and how well it fits the framework's mission. They share no
> code and can land in any order or independently — the "separate plans" the
> request asked for, gathered under one index so the shared constraints
> (license, pinning, never-ship) are stated once.
>
> Siblings: the DBI substrate this compares against
> ([asmtest_drtrace.h](../../../include/asmtest_drtrace.h),
> [asmtest_taint.h](../../../include/asmtest_taint.h),
> [dynamorio-taint-tier-plan.md](../archive/plans/dynamorio-taint-tier-plan.md));
> the Unicorn future-ISA ceiling this routes around
> ([live-attach-dataflow-followup-plan.md](live-attach-dataflow-followup-plan.md),
> F1 increment 2); the arg/return-capture note PIN-3 serves
> ([capture-args-returns.md](../analysis/capture-args-returns.md)).

> **Status legend: planned** unless a track is marked *(landed)*. No track is
> started.

## Shared constraints (stated once, apply to every track)

Read before starting any track — they are the reason all four are shaped as
fetched-and-pinned test lanes rather than shipped tiers.

1. **Pin and SDE are proprietary freeware.** Redistributable binaries, not open
   source. They are therefore **test/oracle-only** and **never linked into
   `libasmtest`, `libasmtest_emu`, or any distributed binding package** — exactly
   how DynamoRIO is handled ([asmtest_drtrace.h](../../../include/asmtest_drtrace.h)
   banner: "kept entirely out of the core libasmtest and the libasmtest_emu
   superset"). No track adds a bindings-parity obligation, because no track ships
   a public tier symbol.
2. **Pin down the version, exactly like DynamoRIO.** Follow the
   [Dockerfile.drtrace](../../../Dockerfile.drtrace) pattern verbatim — `ARG
   PIN_VERSION` / `ARG SDE_VERSION` + `curl` the kit tarball + **assert its
   SHA-256** against a new line in
   [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt) +
   vendor the license text under [licenses/](../../../licenses/). A
   `scripts/fetch-pin.sh` / `scripts/fetch-sde.sh` mirrors
   [scripts/fetch-dynamorio.sh](../../../scripts/fetch-dynamorio.sh) (download →
   verify digest → capture license → echo the home dir).
3. **CLAUDE.md governs the gate.** SDE and Pin are **installable**, so per
   [CLAUDE.md](../../../CLAUDE.md) ("a missing dependency is not a blocker — add
   it") they are added to the relevant `Dockerfile.*` + `docker-*` rule, **not**
   self-skipped. The only real self-skip gates that remain are (a) non-x86 hosts
   (Pin/SDE are x86-only — AArch64 / macOS-arm64 lanes self-skip with a reason)
   and (b) for PIN-1's *native cross-check* half only, the actual silicon.
4. **Reuse the shared sink.** Any trace-producing track fills
   [asmtest_trace_t](../../../include/asmtest_trace.h) (offsets from the region
   base, append/dedup/`truncated` discipline) so its output is diffable against
   every existing backend — the whole point of a new producer here is
   cross-validation.

## Items

| # | Track | DR-impossibility? | Fits mission? | Effort | x86-only self-skip |
|---|---|---|---|---|---|
| **PIN-1** | **SDE future/absent-ISA test lane** — run existing `TEST()` suites under `sde64` so APX/AVX10/AMX/AVX-512 assembly gets full register/flag/memory/ABI assertions on any x86-64 host incl. CI | **yes** (also impossible for the Unicorn tier) | **core promise** | M | yes |
| **PIN-2** | **XED-decoded Pin trace tier** — a `libasmtest_pintool` control-flow producer that decodes the newest extensions (APX today) DR's code cache rejects | yes, currency-bounded | high | L | yes |
| **PIN-3** | **Pin probe-mode arg/return capture** — boundary trampolines, original code runs native; the `capture-args-returns.md` middle tier | yes, but approximable | med | M | yes |
| **PIN-4** | **libdft64 differential oracle** — an independent in-band taint engine diffed byte-for-byte against the shipped DR taint client | no (oracle-hardening) | high (cross-val idiom) | M–L | yes |

---

## PIN-1 — SDE future/absent-ISA test lane *(planned)*

**The one to build.** Turns [CLAUDE.md](../../../CLAUDE.md)'s "specific CPU
generation" hardware self-skip into a runnable tier: assemble a routine using an
extension no host has (APX r16–r31, AMX tiles, AVX10.2, AVX-512 on an
AVX2-only box), then run the **unmodified** suite binary under
`sde64 -future -- ./build/<suite>`, and the framework's existing capture
trampolines observe real register / flag / memory / ABI results. Neither the DR
tier (executes on real silicon) nor the Unicorn tier (QEMU 5.0.1, no AVX TCG —
[live-attach-dataflow-followup-plan.md](live-attach-dataflow-followup-plan.md)
F1 inc 2) can do this.

**Why it works with no framework code change first:** SDE emulates the *whole
process*, so `capture.s`'s trampoline `call` and the `regs_t` snapshot run inside
the emulated CPU. The routine-under-test's new instructions execute; the harness's
plain x86-64 does too. So increment 1 is a **build/CI lane, not a source change.**

**Increments**

1. **Lane bring-up (no source change).** `Dockerfile.sde` (`ARG SDE_VERSION` +
   digest gate per shared-constraint 2) + a `docker-sde` / `sde-test` Make target
   that builds the normal suites and runs each under `sde64 -future`. Prove it on
   an **existing** suite first (a plain arithmetic suite must pass identically
   under SDE — the null test that SDE is transparent to correct code).
2. **A future-ISA example suite.** `examples/apx_*.s` (+ `test_apx_*.c`) using
   r16–r31 / REX2, and an AVX-512 suite gated to run under SDE on an AVX2 host.
   These are the fixtures that **fail to even assemble/run** without SDE and pass
   with it — the lane's reason to exist. Assembler reach is the real subtask:
   confirm the GAS/clang and NASM backends encode the target extension (bump the
   pinned assembler in the SDE image if not — a dependency, add it).
3. **Emulator cross-check where the ISA overlaps.** For extensions Unicorn *does*
   model (baseline AVX2 and below), assert SDE and the Unicorn tier agree, so the
   lane is anchored to an existing backend and not a lone oracle.
4. **`mix` instruction-class report (optional).** SDE ships an emulator-aware Pin
   tool (`-mix`) that histograms dynamic instructions by ISA-extension group;
   fold its count into an `asmtest_trace_t`-shaped report so a future-ISA routine
   gets instruction/block counts nothing else can produce.

**Self-skip:** non-x86 hosts (aarch64 Linux/macOS) skip with a reason;
`sde-test` self-skips if the pinned SDE is absent (before the lane fetches it).
No silicon gate — that is the point.

**Exit criteria:** an APX (r16–r31) routine and an AVX-512-on-AVX2-host routine
each get `ASSERT_EQ` / `ASSERT_FLAG_*` / `ASSERT_ABI_PRESERVED` results under
`sde64 -future` on a plain x86-64 CI runner; a baseline suite passes byte-
identically with and without SDE (transparency null test); the lane self-skips
cleanly on aarch64; SDE is fetched, digest-verified, and its license vendored.

---

## PIN-2 — XED-decoded Pin trace tier *(planned)*

A `libasmtest_pintool` control-flow producer filling
[asmtest_trace_t](../../../include/asmtest_trace.h), whose reason to exist is
**decoder currency**: Pin decodes with XED, so it instruments routines using the
newest extensions (APX **today**, [DR #6226][dr6226] open) that DynamoRIO's code
cache rejects ([DR #5440][dr5440] VNNI still breaks). On capable silicon this is a
DBI trace where the drtrace tier aborts.

**The plug-in contract** (from the DynamoRIO tier map): a Pintool resolves the
app's exported marker PCs (`asmtest_trace_begin` / `_end`, the region markers) by
symbol and instruments basic blocks in `[base, base+len)` to append offsets. The
**one thing that does not map from DR** is the in-process no-IPC hand-off: Pin's
model is launch-under-`pin`, so the trace crosses a **shared-memory segment**
(reuse the taint tier's `drrun`-shm precedent,
[asmtest_taint_shm.h](../../../include/asmtest_taint_shm.h)) rather than being
written straight into the app struct.

**Increments**

1. **Launcher + marker-PC block-count tool.** `Dockerfile.pintool` (pinned Pin
   kit) + a minimal Pintool that counts blocks in a named region and writes an
   `asmtest_trace_t` over shm; drive an existing traced fixture and assert the
   block/insn offsets **match the DR and single-step backends** on an
   ordinary-ISA routine (parity is the acceptance test).
2. **Newest-extension fixture.** Trace an APX routine end-to-end under Pin and
   assert the same routine's DR trace **aborts/decoder-errors** — the negative
   control that proves the tier earns its keep (skip the DR half on a host whose
   DR build predates the extension).
3. **Bindings:** **none.** Per shared-constraint 1 this ships no public tier
   symbol; it is exercised by C fixtures + the docker lane only.

**Self-skip:** non-x86 hosts; also self-skip increment 2's APX fixture where the
host CPU lacks APX *and* SDE is not stacked under Pin (Pin-on-SDE is possible but
out of scope for v1 — record as a follow-up).

**Exit criteria:** on an ordinary routine, the Pin trace is offset-identical to
the DR and single-step traces (cross-backend parity); on an APX routine, Pin
produces a complete trace while DR errors — both asserted in one lane; the tool
is launch-under-`pin` with the trace delivered over shm; zero diff under
`include/` (no public header).

---

## PIN-3 — Pin probe-mode arg/return capture *(planned)*

The [capture-args-returns.md](../analysis/capture-args-returns.md) "middle tier":
Pin **probe mode** trampolines at function entry/exit while the app runs its
**original code natively** between probes — stronger than LD_PRELOAD (dynamic
symbols only) and far cheaper than full DBI, with no code cache. DR has no
supported equivalent. Honest scope: probe-style trampolining is *approximable*
without Pin, so this is a robustness/ergonomics win, ranked below PIN-1/2.

**Increments**

1. **Entry/exit capture Pintool.** In probe mode, instrument a named function:
   record the SysV integer/FP arg registers at entry and the return
   register(s) + flags at exit into the `asmtest_valtrace_t`-shaped shm record
   ([asmtest_valtrace.h](../../../include/asmtest_valtrace.h)); resolve pointed-to
   buffers with a configurable size cap (the note's 4 KiB default) and strict
   validation against mapped ranges.
2. **Cross-check against the ptrace arg/return path.** Diff Pin's captured
   arg/return values against the out-of-process ptrace stepper's `read_pc_ret`
   ([src/ptrace_backend.c](../../../src/ptrace_backend.c)) on the same fixture —
   two independent producers must agree.
3. **Safety posture** per the note: caps on pointer reads, never trust a pointer,
   treat captured buffers as sensitive; documented, not just coded.

**Self-skip:** non-x86 hosts. Probe mode also refuses non-relocatable /
too-short functions — surface that as a per-target skip with a reason, never a
silent miss (the repo's honesty rule).

**Exit criteria:** entry args and exit return/flags for a chosen native function
are captured under Pin probe mode and **agree with the ptrace path** on the same
inputs; a pointed-to buffer is captured within the size cap and an invalid
pointer is refused, not faulted on; probe-mode refusals report a reason.

---

## PIN-4 — libdft64 differential oracle for the DR taint tier *(planned)*

Not a DR impossibility — an **oracle**. The DR taint client
([src/dataflow_dr_client_inlined.c](../../../src/dataflow_dr_client_inlined.c) built
`-DASMTEST_TAINT`) is home-grown and today validated against an **offline**
emulator/Capstone L2 slice. **libdft64** is the canonical, independently
implemented byte-level taint engine on Pin. Running it as a second in-band DBI
taint producer over the **same seed/sink fixtures** and asserting byte-for-byte
agreement is the strongest check available — the `ASSERT_MATCHES_REF`
independent-reference idiom, applied to the one tier lacking a live peer.

**The interface work is a mapping, not a rebuild:** libdft's sink observations
(tainted bytes reaching a watched operand) must be projected onto
[at_taint_report_t](../../../include/asmtest_taint.h) `at_taint_hit_t` records
(region `off`, effective address `ea`, union `tag`, `kind`) so the two reports
diff directly. Seeds are shared verbatim (`at_taint_seed_t` base/len/color).

**Increments**

1. **libdft64 lane bring-up.** `Dockerfile.pintool` (shared with PIN-2) + a pinned
   libdft64 checkout (source-built against the pinned Pin, digest-gated); a small
   Pintool that seeds `[base,len)` and reports sink hits over shm in the
   `at_taint_hit_t` shape.
2. **Oracle diff.** Run the **existing** DR taint fixtures
   (`examples/dr_taint*.c`) under both the DR client and libdft64; assert the sink
   sets agree (same `off`/`ea`, compatible `tag` union). Where they differ,
   classify: a real DR-client bug, a libdft coverage gap (it famously skips
   XMM/SSE — [data-flow-capture.md](../analysis/data-flow-capture.md)), or a
   modelling difference — and record the boundary, do not paper over it.
3. **Wire as an optional gate,** not a blocker: `docker-taint-oracle` runs the
   diff; a divergence in the covered subset fails, a known libdft coverage gap
   (SIMD) is an allowed, *named* skip — never a blanket pass.

**Self-skip:** non-x86 hosts; the SIMD subset where libdft has no coverage is a
named skip, distinct from a clean pass.

**Exit criteria:** the DR taint tier's own fixtures produce a sink set that
matches libdft64's byte-for-byte on the GP/integer-memory subset both cover;
every divergence is classified (DR bug / libdft gap / modelling) and recorded;
the SIMD coverage gap is a named skip, not silent; libdft64 + Pin are pinned and
digest-verified.

---

## What this plan deliberately does **not** do

- **No shipped Pin tier and no bindings.** Every track is a fetched, pinned,
  test/oracle-only lane (shared-constraint 1). Nothing enters `libasmtest`, the
  `libasmtest_emu` superset, or a binding package, so the bindings-parity gate is
  untouched.
- **No AArch64.** Pin/SDE are x86-only; these lanes self-skip on ARM and add
  nothing to the emulator's cross-ISA reach. That reach stays Unicorn's job.
- **No replacement of the DR tier.** DR keeps the in-band, in-process, no-IPC DBI
  trace + whole-process taint role it already fills. Pin is additive at the
  edges DR cannot reach (PIN-1/2) or as an independent check (PIN-4).

[dr5440]: https://github.com/DynamoRIO/dynamorio/issues/5440
[dr6226]: https://github.com/DynamoRIO/dynamorio/issues/6226
