# asm-test — AMD MSR-direct LBR snapshot (`asmtest_amd_msr_trace`)

Read the AMD LbrExtV2 branch-record MSRs **directly** around a region — enable the LBR,
run the routine, freeze, read the 16 `FROM`/`TO` entries — to get an exact Tier-A snapshot
with **zero PMU interrupts**, decoded by the shared `asmtest_amd_decode`. This is Phase 5's
"MSR-direct snapshot" item from [amd-tracing-plan.md](amd-tracing-plan.md), and the
"option 4" of the Zen 5 tracing-improvement determination.

> **Status: LANDED, empirically validated on Zen 5 (Ryzen 9 9950X).** The analysis below
> shows this path is **contamination-limited** — from userspace the only way to freeze the
> LBR is a `/dev/cpu/N/msr` write syscall, whose glue branches eat into the 16-entry window.
> Whether it captures anything *useful* was an empirical question, settled by the live
> measurement: with a user-only `LBR_SELECT` filter the syscall glue is thin enough that a
> **tiny routine's branches survive** — `make docker-hwtrace-msr` (a `--privileged` lane
> reading the real MSRs) reconstructs a 4-trip loop **complete, not truncated** (`insns=11`,
> loop-body block covered, `rc=0`), with zero PMU interrupts. So it ships as a niche
> zero-interrupt Tier-A tier, honestly `truncated` for routines too large for the surviving
> window (amd_decode's depth check). Self-skips everywhere without `amd_lbr_v2` +
> `/dev/cpu/N/msr` (`CAP_SYS_ADMIN` + the `msr` module), so ordinary CI is untouched.

---

## The hardware (confirmed against the kernel + msr-index.h)

AMD LbrExtV2 (Zen 4/5, `amd_lbr_v2`) exposes its 16-entry branch stack through MSRs
(`arch/x86/events/amd/lbr.c`, `arch/x86/include/asm/msr-index.h`):

| MSR | Address | Role |
|---|---|---|
| `MSR_AMD_DBG_EXTN_CFG` | `0xc000010f` | bit 6 (`LBRV2EN`) enables the extended LBR |
| `MSR_AMD64_LBR_SELECT` | `0xc000010e` | branch filter, **suppress mode**: bit 0 suppresses kernel-ending branches (user-only), bits 2–8 suppress branch *types* |
| `MSR_AMD_SAMP_BR_FROM` | `0xc0010300` | entry `i`: FROM = `0xc0010300 + i*2`, TO = `+ i*2 + 1` |
| `MSR_IA32_DEBUGCTLMSR` | `0x1d9` | legacy LBR/freeze bits (not the primary enable here) |

**Entry decode.** FROM holds `ip[57:0]` (+ `mispredict` at bit 60); TO holds `ip[57:0]` +
`valid` (bit 63) + `spec` (bit 62); an entry is valid iff `valid||spec`. Internal register
renaming pins `FROM[0]/TO[0]` to the TOS, so entries read `0..15` are **newest→oldest** —
exactly the order `asmtest_amd_decode` (and `perf_branch_entry[]`) already consume, so the
decoder is reused unchanged. Address IPs are sign-extended to canonical; for a low mmap'd
routine (bit 57 = 0) the `ip[57:0]` field *is* the address.

Depth is 16 on every shipping part — already read at runtime via `asmtest_amd_lbr_depth()`
(CPUID `0x80000022` EBX[9:4]).

---

## Why this is contamination-limited (the crux)

The value proposition is **zero PMU interrupts** (vs the `sample_period=1` PMI flood) and
**no BPF toolchain** (vs the shipped `#3` `bpf_get_branch_snapshot` boundary snapshot). But
userspace cannot touch an MSR without a syscall, and that is the whole problem:

- To **freeze** the LBR at region exit you must write `DBG_EXTN_CFG` via
  `pwrite(/dev/cpu/N/msr)`. The user-space instructions between the routine's `ret` and the
  `syscall` that performs the wrmsr are **taken branches the LBR records** before it stops.
- To **enable/reset** before the region, same story on the front end.

A **user-only `LBR_SELECT`** (suppress kernel) removes the syscall's *kernel* branches (the
bulk), leaving only the thin userspace wrapper glue. And `asmtest_amd_decode` already filters
to in-region branches, so the glue (out-of-region) drops out of the *reconstruction* — but it
still **occupies stack slots**, evicting the routine's oldest branches when
`in_region_branches + surviving_glue > 16`. So the capture is complete only for routines with
roughly `≤ 16 − glue` taken branches.

**The open number is `glue`** — how many user branches survive the user-only filter around a
minimal-glue enable/freeze. If a pre-staged raw `syscall` (args set before the call, so only
the `ret` + `syscall` insn intervene) keeps `glue` to ~1–3, then routines with ≤ ~13 branches
reconstruct — a genuine (if niche) zero-interrupt Tier-A. If the glibc wrapper / relocation
stubs push `glue` toward the window size, it captures essentially nothing and is a dead end.
**Only the live measurement settles this**, hence the empirical-first posture.

Even in the best case this is strictly a **niche** relative to what already ships: the `#3`
BPF snapshot freezes at a **hardware** exit breakpoint (`#DB` in kernel context — zero
userspace glue) and is the clean boundary capture this MSR path only approximates. MSR-direct
earns its place solely where a self-hosted runner has `CAP_SYS_ADMIN` + the `msr` module but
**not** `CAP_BPF`/libbpf.

---

## Design

`int asmtest_amd_msr_trace(const void *base, size_t len, void (*run_fn)(void*), void *arg,
asmtest_trace_t *trace)` (callback-thunk model, like `asmtest_amd_snapshot_trace`):

1. **Pin** the calling thread to one CPU (`sched_setaffinity`) — MSRs are per-CPU and the
   region must run on the CPU whose `/dev/cpu/N/msr` we read.
2. **Open** `/dev/cpu/N/msr` `O_RDWR` (fails without `CAP_SYS_ADMIN` + the `msr` module →
   clean self-skip / `EUNAVAIL`).
3. **Disable + reset:** clear `DBG_EXTN_CFG` (bit 6 → 0); write `LBR_SELECT` = user-only,
   all-types; zero the 16 `FROM`/`TO` pairs while disabled (no racy recording).
4. **Enable:** set `DBG_EXTN_CFG` bit 6.
5. **`run_fn(arg)`**, then **freeze** with the *minimal-glue* wrmsr (pre-staged so only the
   thunk `ret` + the `syscall` intervene).
6. **Read** the 16 `FROM`/`TO` MSRs into a `perf_branch_entry[16]` (newest-first), decoding
   `ip[57:0]` + `valid`/`spec`.
7. **`asmtest_amd_decode`** the array into `trace` (in-region-filtered; window overflow /
   glue eviction → `truncated`, honest via the existing depth check). Restore MSR state.

**Availability:** `asmtest_amd_msr_available()` = `amd_lbr_v2` flag **and** `/dev/cpu/0/msr`
openable `O_RDWR`. Off AMD, without the `msr` module, or without `CAP_SYS_ADMIN`: 0 (self-skip).

**Reuse:** the decode is the shipped `asmtest_amd_decode` — no new decoder, no new parity
surface beyond the one capture entry (allow-listed C-level, like `asmtest_amd_snapshot_trace`).

---

## Phases

- **Phase 0 — MSR read/write plumbing + availability probe.** *(landed)* `pread`/`pwrite`
  on `/dev/cpu/N/msr` ([src/msr_lbr.c](../../../src/msr_lbr.c)); `asmtest_amd_msr_available()`
  = `amd_lbr_v2` + `/dev/cpu/N/msr` openable `O_RDWR`.
- **Phase 1 — enable/reset/freeze/read sequence.** *(landed)* `asmtest_amd_msr_trace`
  (callback-thunk): pin CPU, disable→user-only filter→reset→enable, run, freeze, read 16
  FROM/TO into a newest-first `perf_branch_entry[]`, decode via `asmtest_amd_decode`; save/
  restore MSR state.
- **Phase 2 — live empirical measurement.** *(landed)* `make docker-hwtrace-msr` (a
  `--privileged` lane; `--device /dev/cpu:/dev/cpu` cannot expose the per-CPU device
  directory) runs `test_amd_msr` on the Zen 5 box: a 4-trip loop MSR-captures **complete**
  (`insns=11`, loop-body block 0x7 covered, `truncated=0`) — the routine's ~3 taken branches
  survive the freeze-syscall glue under the user-only filter (the `from=0xa` back-edges are
  the `dec+jnz` macro-op-fused address AMD reports, which the shared decoder already handles).
- **Phase 3 — outcome: SHIPPED as a niche tier.** The capture is wired behind
  `asmtest_amd_msr_available()`; it self-skips everywhere without the substrate/privilege.
  Documented as the zero-interrupt, libbpf-free Tier-A for a `CAP_SYS_ADMIN` self-hosted
  runner — the deterministic BPF boundary snapshot stays the cleaner-boundary default where
  `CAP_BPF` is available.

---

## Risks & open points

- **`glue` is the whole ballgame** (above) — measured in Phase 2, not assumed.
- **MSR faults.** A wrong MSR address / bit `#GP`s. Addresses are pinned from the kernel
  source; the probe opens `/dev/cpu/N/msr` and any `pread`/`pwrite` error aborts cleanly to
  `EUNAVAIL` rather than trapping.
- **Privilege exposure.** `CAP_SYS_ADMIN` + the `msr` module is a *higher* bar than `#3`'s
  `CAP_BPF`/`CAP_PERFMON`; this is a self-hosted-runner-only path, never a portable default.
- **Per-CPU coherence.** The pin must hold across enable→run→freeze→read; migration mid-region
  would read another CPU's stack. `sched_setaffinity` to a single CPU covers it.
- **State restoration.** Leaving `DBG_EXTN_CFG`/`LBR_SELECT` modified could perturb a
  concurrent perf LBR user; the capture saves and restores prior MSR values.

## Validation

- **Decode:** host-independent (synthetic `FROM`/`TO` values → `asmtest_amd_decode`), reusing
  the existing `test_amd_reconstruction` fixtures — no hardware.
- **MSR read + capture:** live on the Zen 5 dev box only, in the privileged lane; self-skips
  everywhere else (non-AMD, no `msr`, no `CAP_SYS_ADMIN`), so ordinary CI is unaffected.
