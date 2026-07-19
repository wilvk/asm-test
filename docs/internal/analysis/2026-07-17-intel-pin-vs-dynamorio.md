# Analysis: what could Intel Pin do for asm-test that DynamoRIO cannot?

*Status: analysis / findings. Records a design investigation prompted by "how
could Intel Pin be useful, and is there anything it makes possible that
DynamoRIO does not." It is a sibling of the DBI trade-off notes
[data-flow-capture.md](data-flow-capture.md) (the taint substrate),
[jit-runtime-tracing.md](jit-runtime-tracing.md) (why in-process DBI fights
managed runtimes), and [capture-args-returns.md](capture-args-returns.md) (arg/
return capture). The implementation roadmap it feeds is
[intel-pin-capabilities-plan.md](../plans/intel-pin-capabilities-plan.md), which
decomposes each surviving item into its own separable plan track.*

## The question, sharpened

Pin ([Intel's DBI framework][pin]) is DynamoRIO's direct peer, and the DBI slot
in this repo is **already occupied — deeply** — by the DynamoRIO tier: an
in-process control-flow tracer ([asmtest_drtrace.h](../../../include/asmtest_drtrace.h),
`libasmtest_drapp` + `libasmtest_drclient`) **and** a full in-band taint/data-flow
tier (byte-granular tag shadow, seed/sink markers, launch-under-`drrun`,
self-attach-mid-run, a POSIX-shm out-of-process validator, `MovedReferences2`
GC-move canonicalization, .NET managed taint —
[asmtest_taint.h](../../../include/asmtest_taint.h),
[src/dataflow_dr_client_inlined.c](../../../src/dataflow_dr_client_inlined.c),
[dynamorio-taint-tier-plan.md](../archive/plans/dynamorio-taint-tier-plan.md)).

So "add Pin" is only interesting if it clears a bar DynamoRIO **cannot** —
otherwise it duplicates a shipped tier and inherits the bindings-parity tax for
nothing. This note separates the genuine deltas from the maturity differences.

## First cut: most "Pin advantages" are maturity, not impossibility

Three commonly-cited Pin strengths are **not** exclusive to it here:

| Claimed Pin edge | Why it is not a DR *impossibility* |
|---|---|
| Attach to a running PID | DR has `drrun -attach` / `dr_inject` (Linux ptrace takeover). Unwired in this repo (the tier uses the cooperative `dr_app_*` model), but a DR capability, not a Pin-only one. See [dr-attach-probe-findings.md](dr-attach-probe-findings.md). |
| Windows in-band DBI | DR supports Windows. The repo's DR tier is Linux-x86-64 by the repo's own scoping, not DR's limit ([macos-drtrace-plan.md](../plans/macos-drtrace-plan.md) tracks the port surface). |
| Whole-process taint (libdft/Triton) | The taint **ground is already held** in-tree on DR. Pin's value here is as an *independent oracle* (below), not a new capability. |

The differential-oracle idea is worth keeping even though it is not an
"impossibility": the DR taint client is home-grown and its correctness oracle is
an **offline** emulator/Capstone L2 slice. **libdft64** is the canonical,
peer-reviewed, *independently implemented* byte-level taint engine on Pin — a
second in-band DBI taint producer diffed byte-for-byte against the DR client is
the strongest possible check, and it is exactly the `ASSERT_MATCHES_REF`
"independent reference model" idiom the repo already lives by. It carries into
the plan as a track, framed honestly as oracle-hardening rather than new reach.

## The genuine deltas — things DynamoRIO cannot do

### 1. Test routines whose instructions the host CPU does not have (Intel SDE)

**The marquee item, and it lands on this project's core promise.** Pin is the
substrate of Intel's **Software Development Emulator ([SDE][sde])**, which
*emulates* future and absent ISA extensions at instrumentation time — **APX**
(the r16–r31 GPRs, REX2), **AVX10.2**, **AMX**, and **AVX-512** on hosts that
lack them; current SDE releases track the latest Intel ISA extensions
programming reference (the newest sync covers Nova Lake / Diamond Rapids
extensions).

This is **categorically outside DynamoRIO's model.** DR *translates* code and
runs it on the real silicon: if the CPU does not have the instruction, DR cannot
execute it. (DR's `drcpusim` only *detects* too-new instructions; it does not
emulate them.) The gap is documented on both of the repo's other flanks:

- **The emulator tier cannot cover it.**
  [live-attach-dataflow-followup-plan.md](../plans/live-attach-dataflow-followup-plan.md)
  (F1 increment 2) measured that Unicorn — even 2.1.3, built from source — vendors
  QEMU 5.0.1, which predates AVX TCG (QEMU 7.2): `vaddps ymm` returns
  `UC_ERR_INSN_INVALID`, and worse, **VEX-128 is silently mis-executed as legacy
  SSE returning `UC_ERR_OK`**. The doc's words: "a capability that does not exist
  in any release." AMX/APX are further still.
- **The native tiers cannot cover it.** [CLAUDE.md](../../../CLAUDE.md) names "a
  specific CPU generation" as a legitimate **hardware self-skip gate** — a routine
  targeting an extension no dev box has is untestable by construction today.

An SDE lane **converts part of that hardware gate into an installable, pinnable
dependency** — precisely what CLAUDE.md's "a missing dependency is not a blocker,
add it" rule demands, since SDE (unlike Intel PT silicon) *can* be installed.
Concretely: run the existing `TEST()` suites under `sde64 -future --`, so
AVX-512 / AMX / APX assembly gets the full register / flag / memory / ABI
assertion battery **on any x86-64 host, including CI runners**. This is the one
item that extends "assert on registers, flags, and memory through the real ABI"
to code **no one's silicon can run yet** — the framework's mission, aimed at the
exact audience that writes bleeding-edge assembly.

### 2. DBI *decode* of the newest extensions, even on capable silicon (XED)

Distinct from #1: even when the CPU **does** support an extension, a DBI engine's
own decoder must model it to rebuild the block in its code cache. Pin decodes with
**XED**, Intel's canonical decoder, updated in lockstep with silicon. DynamoRIO
maintains its own decoder and lags structurally:

- AVX-512 was a multi-**month** effort and landed years late ([DR #1312][dr1312]);
- AVX-512 VNNI was itself once broken, but that gap is **closed** — [DR #5440][dr5440]
  was resolved 2022-04-25 (PR #5444), and the pinned DR 11.91.20630 post-dates the
  fix, so it decodes VNNI. The live decoder gap is APX alone, next;
- **APX is an open future-work item ([DR #6226][dr6226])** — a routine using
  APX's r16–r31 cannot pass through DR's code cache **today**, so the drtrace /
  taint tier would abort on it.

So "the DBI trace/taint tier works on a routine using the newest extensions" is
currently a **Pin-only** property on capable hardware. For a framework whose
users are the people writing that assembly, this is the difference between the
DBI tier tracing their routine and refusing it. (Honest scope: this is a decoder
*currency* advantage that erodes as DR catches each extension up; it is a real
gap at any given moment, not a permanent architectural one like #1.)

### 3. Probe mode — instrumentation with no code cache at all

Pin's **probe mode** inserts trampolines at function boundaries and otherwise
lets the application run its **original code natively** — no recompilation, no
software code cache, near-zero overhead between probes. DynamoRIO has **no
supported equivalent** (its old probe/hot-patch API is unmaintained; everything
DR does goes through the cache). This is the robust middle tier the
[capture-args-returns.md](capture-args-returns.md) arg/return-capture note is
reaching for — stronger than LD_PRELOAD (which only interposes dynamic symbols)
and far cheaper than full DBI.

**Weakest exclusivity claim of the three:** "not possible with DR" is true, but
probe-style boundary trampolining is *approximable* without Pin (hand-rolled
trampolines, Frida-style inline hooks), so this is a convenience/robustness win,
not a hard capability only Pin can deliver. Ranked last for that reason.

## The reverse gaps — what Pin *loses* against DynamoRIO here

Kept explicit so no future reader mistakes Pin for a strict superset:

- **x86 only.** Pin has **no AArch64**. DR does. Any Pin tier is Linux/Windows
  x86/x86-64 and adds nothing to the emulator's cross-ISA reach (AArch64 / ARM32
  / RISC-V), which is a first-class part of this project.
- **No in-process no-IPC model.** The drtrace tier's distinctive strength is that
  the app `dlopen`s `libdynamorio.so`, calls `dr_app_start` for **in-process**
  takeover, and the client writes offsets **straight into the test's own
  `asmtest_trace_t`** — no IPC. Pin's model is launch-under-`pin` (a separate
  injector), so a shipping Pin *tier* would have to re-solve a shared-memory
  hand-off DR gives for free (the taint tier's `drrun` shm validator is the
  in-tree precedent).
- **Proprietary.** Pin and SDE are proprietary freeware (redistributable
  binaries, not open source), where DR is BSD and the engines are BSD/GPL. This
  is workable **only** as a fetched-and-pinned, **never-shipped** test-lane
  dependency — exactly how DR is handled today
  ([Dockerfile.drtrace](../../../Dockerfile.drtrace): `ARG DR_VERSION` + tarball +
  SHA-256 gate, license text vendored under [licenses/](../../../licenses/), kept
  out of `libasmtest` and the `libasmtest_emu` superset). Never bundleable into a
  distributed library.

## Bottom line

Ranked by how squarely each is a DynamoRIO *impossibility* and how well it fits
the framework's mission:

1. **SDE future/absent-ISA test lane** — a true impossibility for DR *and* for
   the Unicorn tier; turns a documented hardware self-skip into a CI-runnable
   tier for APX/AVX10/AMX/AVX-512 assembly. **The one to build.**
2. **XED-decoded DBI over the newest extensions** — a real, currency-bounded gap
   where DR's decoder has not caught up (APX open today).
3. **Probe-mode arg/return capture** — a DR impossibility but approximable
   elsewhere; a robustness win, not a unique capability.
4. **libdft64 differential oracle** — not an impossibility, but the cleanest
   Pin-shaped hardening of the already-shipped DR taint tier, on-method for the
   repo's cross-validation idiom.

All four are **test/oracle-only, fetched-and-pinned, never-shipped** — and #1 is
the only one that extends the core promise to code no host can run.

## Sources

- Intel Pin — <https://www.intel.com/content/www/us/en/developer/articles/tool/pin-a-dynamic-binary-instrumentation-tool.html>
- Intel SDE — <https://www.intel.com/content/www/us/en/developer/articles/tool/software-development-emulator.html>
- Intel SDE release notes (APX / AVX10.2 emulation) — <https://www.intel.com/content/www/us/en/developer/articles/release-notes/intel-software-development-emulator-release-notes.html>
- Intel SDE benefits (emulator-enabled Pin tools: mix / debugtrace / footprint) — <https://www.intel.com/content/www/us/en/developer/articles/technical/benefits-of-using-intel-software-development-emulator.html>
- DynamoRIO APX support (open) — <https://github.com/DynamoRIO/dynamorio/issues/6226>
- DynamoRIO AVX-512 support — <https://github.com/dynamorio/dynamorio/issues/1312>
- DynamoRIO AVX-512 VNNI — <https://github.com/DynamoRIO/dynamorio/issues/5440>

[pin]: https://www.intel.com/content/www/us/en/developer/articles/tool/pin-a-dynamic-binary-instrumentation-tool.html
[sde]: https://www.intel.com/content/www/us/en/developer/articles/tool/software-development-emulator.html
[dr1312]: https://github.com/dynamorio/dynamorio/issues/1312
[dr5440]: https://github.com/DynamoRIO/dynamorio/issues/5440
[dr6226]: https://github.com/DynamoRIO/dynamorio/issues/6226
