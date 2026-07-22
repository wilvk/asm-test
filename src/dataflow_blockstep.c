/*
 * dataflow_blockstep.c — the BLOCK-STEP + EMULATOR-REPLAY value tier (F1, increment 1):
 * a lower-perturbation scoped L0 value producer that fills the SAME asmtest_valtrace_t
 * the single-step (dataflow_ptrace.c) and emulator (dataflow_emu.c) producers fill, so
 * the shared L1 def-use + L2 slicer (dataflow.c) work UNCHANGED on its captures.
 * See docs/internal/plans/live-attach-dataflow-followup-plan.md (F1) and the increment-0
 * spike findings docs/internal/analysis/2026-07-15-blockstep-value-spike.md.
 *
 * THE PERTURBATION WIN. Direct PTRACE_SINGLESTEP traps on EVERY instruction — exactly the
 * stop density that widens the cross-thread deadlock window on a live runtime. This tier
 * decouples VALUES from STOPS: it drives the region with PTRACE_SINGLEBLOCK (one #DB per
 * TAKEN branch — an order of magnitude fewer stops), takes a full PTRACE_GETREGS snapshot
 * at each boundary, and REPLAYS each straight-line block between boundaries through a
 * Unicorn engine seeded with that real register state (its guest mapped AT THE REAL
 * addresses, memory faulted from the tracee via process_vm_readv) to reconstruct the
 * per-instruction values in the pure interior. The endpoints are always real observations;
 * replay only ever fills a bounded pure interior. The spike proved this reconstruction is
 * BYTE-IDENTICAL (down to a raw memcmp, modulo the stack-absolute delta two forks carry) to
 * true single-step on pure integer/flag code, while cutting the in-region stop count ~6x.
 *
 * PURITY GATE. Block-step advances the REAL process, so a syscall inside a block has
 * already retired by the boundary — emulating through it would be wrong. So the region's
 * (time-correct) bytes are static-scanned ONCE up front for OS-interacting / nondeterministic
 * instructions (syscall / sysenter / int 0x80 / rdtsc / rdtscp / rdrand / rdseed / cpuid):
 * a PURE region gets block-step + replay; an IMPURE one falls back to direct single-step
 * (the same shared capture core, driven one instruction at a time). F2 (below) lifts that
 * fallback for the SYSCALL half via record-and-inject.
 *
 * ============================ F2 — RECORD-AND-INJECT ============================
 *
 * F2 extends the tier from PURE methods to OS-INTERACTING ones. The design is much smaller
 * than the plan anticipated, and the reason is a MEASURED hardware fact:
 *
 *   PTRACE_SINGLEBLOCK (DEBUGCTL.BTF) ALREADY TRAPS `syscall` AND `int 0x80`, because they are
 *   control transfers. It does NOT trap rdtsc / rdtscp / rdrand / rdseed / cpuid, which are not.
 *
 * Measured on this Zen 5 (`mov eax,39; syscall; ...` — one SINGLEBLOCK lands at syscall+2 with
 * the kernel's return already in rax). That single fact decides everything:
 *
 *  1. THE SYSCALL IS AN INDUCED BOUNDARY, FOR FREE. The forward pass already stops immediately
 *     after the syscall retires, with the real result in the real registers and the kernel's
 *     memory delta already written to the real process. F2 costs ZERO extra perturbation: it
 *     adds no stop that block-stepping did not already take.
 *  2. THERE IS NOTHING TO FABRICATE, AND SO NO SYSCALL TABLE. The plan expected a decoder that
 *     knows which argument is an output buffer and how many bytes the kernel filled ("a syscall
 *     that fills a buffer is the hard case"). None is needed, and asmspy's decoder would not
 *     have helped: it maps NUMBER->name and formats path arguments FOR DISPLAY, and has no model
 *     of output buffers at all. Instead the effect is simply CARRIED ACROSS from the real
 *     tracee: rax/rcx/r11 are read from the real boundary snapshot, and the kernel's MEMORY
 *     delta is delivered by the re-snapshot F1 already performs at every boundary. Every
 *     injected byte is a real observation. There is no per-syscall knowledge in this file.
 *  3. NO MEMORY IS INJECTED, AND NO MEMORY *CAN* GO STALE — STRUCTURALLY. Because the syscall
 *     TERMINATES the block, no replayed instruction executes after it within that block, so
 *     there is no window in which Unicorn's (pre-syscall) memory could be read. The next block
 *     is seeded from the post-syscall ground-truth snapshot. This matters because the coherence
 *     canary compares registers only — it has NO memory comparison — so an injected result
 *     landing in memory would be unwitnessed. F2's answer is not to add a witness but to leave
 *     nothing to witness: the exposure is zero by construction rather than by checking.
 *
 * WHAT IS INJECTED, AND WHAT THE CANARY CAN STILL SEE. Only rax/rcx/r11 (rax alone for int 0x80
 * — measured: an interrupt gate leaves rcx/r11 untouched) and rip. Those four are necessarily
 * TAUTOLOGICAL at the syscall boundary: they are compared against the snapshot they were copied
 * from. Every OTHER register, rsp, and the arithmetic flags are still genuinely checked, so the
 * canary continues to validate the block's whole pure prefix. Flags survive a syscall (SYSCALL
 * saves rflags to r11, SYSRET restores them), which is why that check stays real. r11 is
 * injected rather than computed for a concrete reason: measured, it returns as the pre-syscall
 * rflags OR'd with TF (0x100) — the ptrace stepping bit — so "computing" it would mean modelling
 * the debug mechanism's own perturbation.
 *
 * WHY rdtsc / cpuid / rdrand / rdseed ARE STILL GATED. Not an oversight and not a decode
 * problem: BTF gives no boundary at which their retired value could be recorded, so
 * record-and-inject has nothing to record. Reaching them needs a different primitive (a hardware
 * exec breakpoint at the site, or single-stepping to it) — a separate increment, not a wider
 * table. They keep the single-step fallback, which is CORRECT, just unoptimized. Note this means
 * F2 displaces only ONE of the two callers of that fallback; the OTHER (`!replayable`, a
 * VEX/EVEX encoding no released Unicorn can execute) is untouched and unrelated — record-and-
 * inject cannot give Unicorn a decoder it does not have.
 *
 * THE REPLAY MUST NEVER EXECUTE AN IMPURE INSTRUCTION, AND "UC_ERR_OK" DOES NOT SAY IT DIDN'T.
 * Measured against the bundled Unicorn: `syscall` returns UC_ERR_OK and just advances rip (rax
 * keeps the syscall NUMBER); `rdtsc` returns UC_ERR_OK with a FABRICATED counter; `cpuid`
 * returns UC_ERR_OK with zeros. (`rdrand` does fault, so it alone fails closed unaided.) None of
 * them fails loudly — the identical failure mode F1 documented for VEX-128. So step_block
 * decodes at its OWN pc and refuses. That decode is deliberately INDEPENDENT of region_scan,
 * which is F1's central lesson applied: region_scan is a linear sweep that DESYNCS on a
 * constant-pool island (its `island` fixture proves it), so an impurity hidden behind one is
 * invisible to it — the gate would pass the instruction through AND the scan-derived witness
 * would be gone. The per-step decode follows real control flow and cannot desync.
 *
 * FAIL-CLOSED ON THE PREMISE ITSELF. The design rests on "BTF always traps the syscall", so the
 * replay REQUIRES it at runtime rather than trusting it: the real next boundary must be exactly
 * syscall+len, or there is no recorded effect to inject and the capture truncates. That covers a
 * host where BTF behaves differently, a signal at the syscall, and a syscall that does not
 * return where expected.
 *
 * WHAT F2 PUTS IN THE TRACE. The shared operand enumerator reports `syscall`/`int 0x80` as
 * touching NO registers (measured: 0 reads, 0 writes) — Capstone models the instruction, not the
 * ABI — so the kernel's result would otherwise never appear in the value trace at all, and the
 * def-use edge from a `read()` to its consumer could not exist. open_step therefore supplies the
 * syscall's architectural write set producer-locally, on the ONE path both value sources share,
 * so the oracle and the replay grow identical records.
 *
 * THE DOCUMENTED RESIDUAL. A sibling thread writing shared memory the region loads, with no
 * syscall to anchor it, is still not reconstructible — the endpoints detect it (the canary
 * truncates) but the replay cannot reproduce it. Unchanged by F2, and inherent to the model.
 * ===============================================================================
 *
 * COHERENCE CANARY. Replay is only correct if the emulator's inputs match reality; a sibling
 * that rewrites a loaded byte between the boundary snapshot and the real block's execution
 * would make the replay silently wrong. So at every boundary the emulator's COMPUTED
 * end-of-block state is compared to the real next boundary (GP regs + rip + rsp + arithmetic
 * flags); a mismatch sets vt->truncated and returns DF_BLOCKSTEP_FAULT — never silently wrong.
 *
 * ORACLE. asmtest_dataflow_blockstep_run(..., force_singlestep) captures the same region by
 * true single-step (registers from GETREGS, memory from process_vm_readv), the ground-truth
 * the block-step trace is cross-validated against. The two are one process each, so they
 * differ only by an absolute stack-address delta (ASLR + frame depth); info.entry_rsp reports
 * the region-entry rsp so a caller can normalize stack-absolute values (rsp-relative) before
 * an equality check, exactly as the shipped slice-oracle sidesteps it by keying on locations.
 *
 * Reuses the shared capture core READ-ONLY: the L0 sink (dataflow.c) and the Capstone
 * operand read/write-set enumerator (dataflow_operands.c). The per-step open/finalize glue
 * over a struct user_regs_struct is producer-local (as it is in dataflow_ptrace.c and the
 * spike) — a value-trace PRODUCER is a tier, not part of the shared asmtest_valtrace.h sink
 * API, so this file ships no header; its test re-declares the entry points and codes below.
 *
 * Requires Linux x86-64 + Capstone (operand enumerator + purity scan) + Unicorn (replay);
 * off-platform / without those it compiles to a DF_BLOCKSTEP_ENOSYS stub and callers
 * self-skip. At runtime it self-skips (DF_BLOCKSTEP_ETRACE / a 0 from the probe) where
 * ptrace is blocked (seccomp/yama) or PTRACE_SINGLEBLOCK is non-functional (a hypervisor
 * masking DEBUGCTL.BTF degrades block-step to per-instruction stepping).
 *
 * VECTOR BREADTH (increment 2, the F1 carryover). A region whose input arrives in a VECTOR
 * register replayed with WRONG (unseeded) vector state — Unicorn starts zeroed while the real
 * CPU holds the caller's value — and the GP-only canary could not see it whenever the vector
 * value reached MEMORY rather than a GP register. That hole is closed on three fronts:
 *
 *   1. BOUNDARY CAPTURE. Every boundary/step snapshot now also reads the tracee's vector state
 *      via PTRACE_GETREGSET(NT_X86_XSTATE), reassembling XMM (the FXSAVE legacy area) +
 *      YMM_Hi128 (component 2) + ZMM_Hi256 (component 6) + Hi16_ZMM (component 7, zmm16-31)
 *      into flat register images. Component offsets come from CPUID.0xD — they are NOT fixed
 *      constants (this Zen 5's layout is 576/832/896/1408, not the commonly-quoted
 *      576/1088/1152/1664, because its XCR0 omits MPX). A component whose XSTATE_BV bit is
 *      clear is in its INIT state (all zeros), which the zeroed snapshot already represents.
 *   2. VALUES IN THE TRACE. Vector-register and >8-byte memory operands land in the sink's
 *      wide[] side buffer (asmtest_valtrace_stash_wide) at their true architectural width —
 *      16 / 32 / 64 bytes — which asmtest_valtrace.h documents as exactly this path. So a
 *      YMM/ZMM value trace carries REAL 256/512-bit values read off real silicon.
 *   3. SEEDING + CANARY. The replay seeds Unicorn's XMM0-15 **and MXCSR** from the real
 *      boundary snapshot (both VERIFIED by read-back, see below), and the coherence canary
 *      compares the replay's end-of-block XMM + MXCSR control bits against the real next
 *      boundary, so a vector divergence TRUNCATES instead of lying. MXCSR is not decoration:
 *      its rounding-control / FTZ / DAZ bits are INPUTS to every FP result, and a tracee
 *      running with non-default rounding (`-ffast-math`'s crtfastmath.o, and JIT/managed
 *      runtimes — this tier's target) otherwise replays every FP op with the wrong rounding.
 *      Measured before the fix, on the legacy-SSE path: `divsd` under RC=toward-zero gave
 *      oracle 0x3fc9999999999999 vs replay 0x3fc999999999999a at rc=OK, truncated=0.
 *
 * THE HONEST BOUNDARY — WHAT "YMM/ZMM SUPPORT" DOES AND DOES NOT MEAN HERE. Measured against
 * the bundled Unicorn (2.0.1) AND against 2.1.3 built from source (2026-07-17, Zen 5):
 *
 *   - SEEDED + REPLAYED: XMM / 128-bit ONLY (plus MXCSR). Legacy SSE executes correctly in
 *     Unicorn (paddd / paddq / movups / divsd verified against silicon, the last under two
 *     rounding modes), so an SSE region with live-in vector state now replays byte-identically
 *     to the single-step oracle. That is the whole perturbation win for vector code.
 *   - CAPTURED but NOT replayed: YMM 256-bit and ZMM 512-bit, including zmm16-31, at full
 *     width from hardware, on the single-step path.
 *   - NOT REPLAYED, BY CONSTRUCTION: any VEX/EVEX-encoded instruction. NO RELEASED UNICORN
 *     EXECUTES AVX — 2.1.3 (the latest, 2025-03-07) still vendors QEMU 5.0.1, and QEMU only
 *     gained AVX TCG in 7.2 (decode-new.c.inc / emit.c.inc, absent from Unicorn's tree).
 *     `vaddps ymm` and `vaddps zmm` both return UC_ERR_INSN_INVALID. This is an UPSTREAM
 *     capability gap, not an installable dependency.
 *
 * TWO SILENT-WRONGNESS TRAPS THIS TIER NOW DEFENDS AGAINST (both measured; "UC_ERR_OK" is NOT
 * evidence that Unicorn did what was asked):
 *
 *   a. VEX-128 IS SILENTLY MIS-EXECUTED AS LEGACY SSE, in every released Unicorn. QEMU 5.0
 *      decodes the VEX prefix, IGNORES VEX.vvvv, routes the SSE opcode to the legacy 2-operand
 *      handler, and skips the mandatory upper-128 zeroing. Differential against this silicon,
 *      same inputs:  real `vpaddd xmm0,xmm1,xmm2` -> 11 22 33 44 (xmm0 = xmm1+xmm2);
 *      Unicorn -> 110 220 330 440 (xmm0 = xmm0+xmm2), returning UC_ERR_OK. VEX-128 does not
 *      fail loudly — it LIES. The replayability gate is therefore an ENCODING-level rule (any
 *      VEX/EVEX prefix byte), which is also why it rejects VEX-GP (BMI) that Unicorn does
 *      execute correctly: over-gating costs only the perturbation win (single-step is still
 *      correct), under-gating costs correctness. Capstone's own AVX metadata is NOT a usable
 *      basis for this gate — measured: `vpbroadcastq zmm0,xmm0` is reported by cs_regs_access
 *      as touching NO registers and belongs to NO X86_GRP_AVX/AVX2/AVX512 group.
 *   b. ZMM REGISTERS ARE UNPLUMBED IN UNICORN 2.0.1. uc_reg_write(UC_X86_REG_ZMM0) returns
 *      UC_ERR_OK and stores NOTHING (it reads back all zeros, and does not even alias
 *      XMM0/YMM0 storage). Fixed in 2.1.x. Hence uc_seed_vec VERIFIES every seed by read-back
 *      rather than trusting a return code, and info.uc_vec_width reports what this Unicorn
 *      actually holds.
 *
 * WHY THE UNICORN PIN IS NOT BUMPED. 2.1.3 fixes only (b) — the ZMM register file. That is
 * unreachable for this tier: with (a) forcing an encoding-level VEX/EVEX gate, no replayed
 * instruction can read or write YMM-upper or ZMM at all, so seeding them would be
 * unobservable, hence untestable, hence vacuous. This tier therefore deliberately seeds XMM
 * ONLY — the exact width a replayable (legacy-SSE) instruction can reach — and takes YMM/ZMM
 * values from hardware instead. A newer Unicorn becomes worth pinning the day QEMU's AVX TCG
 * lands in it; until then the pin is irrelevant to what this tier can do.
 *
 * ALSO FIXED HERE — a pre-existing increment-1 bug this lane only surfaced once it ran on bare
 * metal (see mr_tracee_window): the replay's stack snapshot used ONE process_vm_readv over the
 * whole window, which is atomic, so a window overrunning the top of the tracee's [stack] VMA
 * failed entirely and was "recovered" by zeroing — the replayed `ret` then popped 0 and the
 * capture died UC_ERR_FETCH_UNMAPPED on ~27% of container runs. Reads are now per-page.
 * Relatedly, a Unicorn fault mid-replay now returns DF_BLOCKSTEP_FAULT + truncated; it used to
 * fall through to the DF_BLOCKSTEP_ETRACE initializer and so masqueraded as "no ptrace here",
 * the self-skip code — a divergence reported as a missing substrate.
 *
 * THE DEFENCES ARE DELIBERATELY INDEPENDENT — that is the design lesson of this file, learned
 * the hard way. region_scan is a SINGLE POINT OF FAILURE feeding BOTH the replayability gate
 * AND, through touches_vec, the vector seed and the vector canary. So a wrong verdict does not
 * merely lose one check: it lets the instruction through AND removes the witness that would
 * have caught the lie. Every one of this file's gates therefore fails CLOSED, and the canary
 * covers state (XMM + MXCSR) that the gate is supposed to have already excluded — belt and
 * braces, because the belt is the same strap as the braces otherwise. A reviewer's mutant that
 * restores the old impurity early-break is now caught by the desync fail-closed rule instead,
 * which is exactly the redundancy working as intended.
 *
 * Scope (this increment): a deterministic, single-threaded leaf routine of up to six integer
 * arguments, executed from an inherited executable mapping; GP registers + rflags + memory
 * operands <= 64 bytes + vector registers at their architectural width. opts.region_off lets
 * the region start PAST the blob base, so the bytes before it are entry glue the tracee really
 * executes but the capture does not trace — the way live-in vector state is established.
 * F2 record-inject for impure methods remains a bounded follow-on.
 */
#define _GNU_SOURCE

#include "asmtest_grow.h" /* asmtest_grow / _pow2 — overflow-checked pool growth (S6) */
#include <stddef.h> /* offsetof — the re-declared-struct layout guard */
#include <string.h> /* memset — the ENOSYS stubs below need it on EVERY platform */
#include <sys/types.h> /* pid_t — part of the entry-point signatures on every platform */

#include "asmtest_valtrace.h"

/* Return codes from the block-step producer (kept in step with the test's copy). Mirrors
 * dataflow_ptrace.c's DF_PTRACE_* vocabulary so a caller can share the self-skip logic. */
#define DF_BLOCKSTEP_OK 0 /* clean scoped capture                           */
#define DF_BLOCKSTEP_FAULT                                                     \
    1 /* fault / divergence: a partial trace, truncated */
#define DF_BLOCKSTEP_EINVAL                                                    \
    (-1) /* bad arguments                                  */
#define DF_BLOCKSTEP_ENOSYS                                                    \
    (-3) /* off Linux x86-64 / no Capstone / no Unicorn    */
#define DF_BLOCKSTEP_ETRACE                                                    \
    (-4) /* ptrace / SINGLEBLOCK unavailable: self-skip    */

/* T7: one real instruction extent within the blob, in the SAME (blob-absolute) coordinate
 * space as opts.region_off — a JIT method map's own unit. region_scan sweeps each extent
 * independently rather than one linear byte run, so a constant-pool island BETWEEN two extents
 * never desyncs the decoder: those bytes are simply never fetched. */
typedef struct {
    uint64_t off, len;
} asmtest_blockstep_extent_t;

/* Capture options. A zero-initialized struct is the production tier: gated
 * block-step+replay over the whole blob, unbounded, no test injection. */
typedef struct {
    uint64_t
        max_insns; /* 0 = unbounded (still bounded by the hard step backstop) */
    int force_singlestep; /* skip the gates; single-step (the ground-truth oracle) */
    int inject_divergence; /* test hook: corrupt a replay seed to fire the coherence canary */
    int inject_block; /* which 0-based interior block's replay seed to corrupt */
    uint64_t
        region_off; /* first IN-REGION byte offset into the blob; [0, region_off) is
                          * entry glue the tracee executes but the capture does not trace
                          * (how live-in vector state is established). 0 = whole blob. */
    int no_vec_seed; /* test hook: do NOT seed the replay's vector state (reproduces the
                        * pre-increment-2 bug) */
    int no_mxcsr_seed; /* test hook: seed the XMM file but NOT MXCSR. Isolates the FP
                        * rounding-mode bug from the register-content one — with no_vec_seed
                        * the xmm regs are zero, so an FP negative control would "pass" by
                        * merely re-proving the XMM bug (0.0/0.0 = NaN), not the MXCSR one. */
    int no_vec_canary; /* test hook: drop the VECTOR half of the coherence canary */
    int force_replay; /* test hook: bypass the purity + replayability gates */
    uint64_t
        stack_hi_pad; /* test hook: grow the replay's stack window this many bytes ABOVE
                            * rsp, forcing it past the top of the tracee's [stack] VMA so the
                            * partially-mapped-window case is reproducible on demand */
    int no_syscall_inject; /* test hook (F2): reach a syscall in the replay but do NOT inject
                            * the recorded effect — reproduces the pre-F2 behaviour, where the
                            * replay carried the syscall NUMBER in rax instead of the kernel's
                            * result. Proves the injection is load-bearing and that the canary
                            * catches its absence. */
    int no_undef_mask; /* test hook (T4): disable BOTH the per-record EFLAGS undefined-bit
                        * mask and the canary's undefined-bit tolerance — the negative control
                        * proving the mask, not luck, is what makes an undefined bit's
                        * divergence non-fatal. */
    uint64_t
        inject_flag_bit; /* test hook (T4): XOR this EFLAGS bitmask into the replay's
                               * computed end-of-block state right before the coherence canary
                               * compares it — makes the mask's discrimination testable even
                               * where Unicorn and silicon already happen to agree on every
                               * bit. */
    int no_hw_record; /* test hook (T6): do NOT arm the T5 DR exec breakpoints, so a scanned
                       * rdtsc/rdtscp/rdrand/rdseed/cpuid site gets no real boundary — the
                       * replay then reaches it with `next` describing a LATER boundary,
                       * step_block's `pc + len != pc_next` check fails, and the capture
                       * truncates. Reproduces the pre-T6 (T5) forward-pass-only behaviour and
                       * proves the DR-breakpoint boundary, not luck, is what makes injection
                       * possible — the `blind_rdtsc` discipline applied to this path. */
    const asmtest_blockstep_extent_t
        *extents; /* T7: the region's real instruction extents, blob-absolute,
                   * sorted and non-overlapping — NULL/0 (the default) means "whole
                   * region", today's behaviour. A caller vouches these ARE real
                   * instruction boundaries; region_scan then never fetches the bytes
                   * between them, so an embedded constant-pool island costs nothing. */
    size_t nextents;
} asmtest_blockstep_opts_t;

/* T4 — the tier's FIRST opts layout guard. Until now every appended opts field was a test
 * hook nobody outside this file and its suite touched, so the struct's silent-skew hazard
 * (the same one the info guard below exists for — `size` alone does NOT catch a field
 * landing in tail padding) went unguarded. Defined unconditionally, like the info guard,
 * so both the real producer and its ENOSYS stub build report the same true layout. */
void asmtest_dataflow_blockstep_opts_layout(size_t *size, size_t *last_off) {
    if (size != NULL)
        *size = sizeof(asmtest_blockstep_opts_t);
    if (last_off != NULL)
        *last_off = offsetof(asmtest_blockstep_opts_t, nextents);
}

/* Capture telemetry, filled on every non-EINVAL return. */
typedef struct {
    int pure; /* 1 = block-step+replay was used; 0 = single-stepped (fallback or forced) */
    const char
        *reason; /* why replay was declined: the offending mnemonic (impure) or
                         * "avx" (a VEX/EVEX encoding the emulator cannot run); else NULL */
    uint64_t
        stops; /* in-region ptrace stops taken (the perturbation measure) */
    uint64_t steps; /* in-region instructions captured */
    uint64_t
        entry_rsp; /* rsp at the region entry — the rsp-relative normalization anchor */
    /* --- vector breadth (increment 2) --- */
    int vec_width; /* widest vector width the HARDWARE + OS expose via XSTATE:
                       * 0 none / 16 XMM / 32 YMM / 64 ZMM */
    int vec_nregs; /* vector registers the hardware exposes: 0, 16, or 32 (AVX-512) */
    int uc_vec_width; /* widest vector width THIS Unicorn actually round-trips, proven by
                       * read-back (2.0.1 accepts a ZMM write and stores nothing) */
    int vec_seeded; /* XMM registers seeded into the replay AND verified by read-back */
    int mxcsr_seeded; /* 1 = MXCSR (FP rounding / FTZ / DAZ) seeded AND verified too */
    /* --- F2 (record-and-inject) --- */
    int injectable; /* the scan's verdict: every impurity in the region is a syscall /
                     * int 0x80, so record-and-inject can carry it (1 also when PURE) */
    uint64_t
        injected; /* syscalls whose recorded effect was injected into the replay.
                   * >0 is the only positive evidence that F2's path really ran:
                   * `pure` cannot say it, since it means "the replay was used". */
    /* --- T5 (F2 increment 2, forward pass) --- */
    uint64_t
        hw_hits; /* rdtsc/rdtscp/rdrand/rdseed/cpuid DR exec-breakpoint site boundaries
                  * taken this capture. Always 0 when force_singlestep skipped the replay
                  * entirely (nothing arms a DR slot on that path) — an explicit Done-when
                  * check, not an incidental zero. */
} asmtest_blockstep_info_t;

/* The tier ships no header (deliberately, see the file comment above), so
 * examples/test_dataflow_blockstep.c re-declares this struct rather than including it —
 * exactly the shape that skewed silently once already (F6's asmtest_dfwin_info_t, caught
 * only by adding this same guard: `size` alone is NOT sufficient, since a field landing
 * in tail padding leaves sizeof unchanged — `last_off`, the FINAL field's offset, moves
 * whenever any earlier field is added/removed/resized, padding or no padding). Defined
 * unconditionally (the struct itself is not behind the platform gate below) so both the
 * real producer and its ENOSYS stub build report the same true layout. */
void asmtest_dataflow_blockstep_info_layout(size_t *size, size_t *last_off) {
    if (size != NULL)
        *size = sizeof(asmtest_blockstep_info_t);
    if (last_off != NULL)
        *last_off = offsetof(asmtest_blockstep_info_t, hw_hits);
}

#if defined(__linux__) && defined(__x86_64__) &&                               \
    defined(ASMTEST_HAVE_CAPSTONE) && defined(ASMTEST_HAVE_UNICORN)

#include <capstone/capstone.h>
#include <cpuid.h>
#include <elf.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unicorn/unicorn.h>
#include <unistd.h>

#ifndef PTRACE_SINGLEBLOCK
#define PTRACE_SINGLEBLOCK 33 /* <sys/ptrace.h> omits it; the kernel wires it */
#endif

/* Hard backstop on TOTAL stops (prologue/glue to region entry, plus the region): bounds
 * wall time if the tracee never enters or never leaves [base, base+len). */
#define DFB_STOP_BACKSTOP (1u << 20)

/* TF (single-step) and RF (resume) are DEBUG-MECHANISM bits, not program semantics: a
 * single-stepped tracee can surface TF in its GETREGS eflags where a SINGLEBLOCK boundary
 * need not, and a #DB can set RF. The value trace records PROGRAM values, so both paths mask
 * these out of every captured eflags via the one shared gp_value below. */
#define EFLAGS_STEP_BITS                                                       \
    0x00010100ULL /* TF (bit 8) | RF (bit 16)                */
#define EFLAGS_ARITH_MASK                                                      \
    0x00000CD5ULL /* CF PF AF ZF SF DF OF — the canary's mask */

/* ------------------------------------------------------------------ */
/* Vector boundary state (XSTATE)                                      */
/* ------------------------------------------------------------------ */

/* Fixed offsets in the STANDARD (non-compacted) XSAVE layout the kernel hands back through
 * PTRACE_GETREGSET(NT_X86_XSTATE) — it builds a uabi buffer via XSTATE_COPY_XSAVE, so the
 * legacy FXSAVE area and the header are always at these architectural positions. Everything
 * BEYOND the header moves per-CPU and must come from CPUID.0xD (see dfb_xlayout). */
#define XSAVE_MXCSR_OFF 24   /* legacy FXSAVE area: MXCSR (4 bytes)        */
#define XSAVE_XMM_OFF   160  /* legacy FXSAVE area: xmm0-15, 16B each      */
#define XSAVE_XSTATE_BV 512  /* header: which components are non-INIT      */
#define DFB_XSTATE_MAX  4096 /* this Zen 5 needs 2440; AVX-512 tops ~2696 */

/* MXCSR's default — round-to-nearest, no FTZ/DAZ, all exceptions masked. This is what a fresh
 * Unicorn engine holds, and therefore what an UNSEEDED replay silently computes with. */
#define MXCSR_DEFAULT 0x1F80u

/* XSTATE_BV / XCR0 component bits this tier reads. A component whose bit is CLEAR is in its
 * architectural INIT state (all zeros) — a zeroed snapshot already represents it correctly. */
#define XFEAT_SSE  1 /* xmm0-15 low 128 (legacy area)      */
#define XFEAT_YMM  2 /* YMM_Hi128: ymm0-15 bits 128..255   */
#define XFEAT_ZMMH 6 /* ZMM_Hi256: zmm0-15 bits 256..511   */
#define XFEAT_HI16 7 /* Hi16_ZMM: zmm16-31, full 64B each  */

/* A boundary VECTOR snapshot: flat little-endian images of the vector register file. z[i]
 * holds register i's full 64 bytes; only the low `width` bytes are architecturally meaningful
 * and only the first `nregs` registers exist. Zeroed == the INIT state, which is correct. */
typedef struct {
    uint8_t z[32][64];
    /* MXCSR is part of the vector state and is LOAD-BEARING for values, not just status: bits
     * 13-14 select the FP rounding mode and bits 6/15 are DAZ/FTZ. A tracee running with
     * non-default rounding — which `-ffast-math`'s crtfastmath.o and JIT/managed runtimes both
     * install, i.e. exactly this tier's target — makes an unseeded replay compute every FP
     * result with the wrong rounding, on the legacy-SSE path the perturbation win rests on. */
    uint32_t mxcsr;
    int width; /* 0 none / 16 XMM / 32 YMM / 64 ZMM */
    int nregs; /* 0 / 16 / 32                       */
    int valid;
} dfb_vecstate_t;

/* One capture-point snapshot: the GP file plus (when the region needs it) the vector file. */
typedef struct {
    struct user_regs_struct gp;
    dfb_vecstate_t vec;
} dfb_snap_t;

/* Where each XSAVE component lives on THIS cpu, discovered once from CPUID.0xD. The offsets
 * are emphatically NOT constants: they depend on which features XCR0 enables, so this Zen 5
 * reports 576/896/1408 where a box whose XCR0 includes MPX reports 576/1152/1664. */
typedef struct {
    int probed;
    int ok;
    uint32_t ymm_off;  /* component 2 */
    uint32_t zmmh_off; /* component 6 */
    uint32_t hi16_off; /* component 7 */
    size_t bufsz;
    int width;
    int nregs;
} dfb_xlayout_t;

static const dfb_xlayout_t *dfb_xlayout(void) {
    static dfb_xlayout_t L;
    if (L.probed)
        return &L;
    L.probed = 1;
    uint32_t a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid_max(0, NULL) < 0xD)
        return &L;
    if (!__get_cpuid_count(0xD, 0, &a, &b, &c, &d))
        return &L;
    uint64_t xcr0 = ((uint64_t)d << 32) | a;
    if (!(xcr0 & (1ULL << XFEAT_SSE)))
        return &L; /* no SSE state: nothing this tier models */
    L.bufsz = b ? (size_t)b : 1024;
    if (L.bufsz > DFB_XSTATE_MAX)
        L.bufsz = DFB_XSTATE_MAX;
    L.width = 16;
    L.nregs = 16;
    if (xcr0 & (1ULL << XFEAT_YMM)) {
        if (!__get_cpuid_count(0xD, XFEAT_YMM, &a, &b, &c, &d))
            return &L;
        L.ymm_off = b;
        L.width = 32;
    }
    /* AVX-512 needs BOTH halves of the wide file: ZMM_Hi256 widens zmm0-15 to 512 bits, and
     * Hi16_ZMM adds zmm16-31. Take the wide path only when both are enabled. */
    if ((xcr0 & (1ULL << XFEAT_ZMMH)) && (xcr0 & (1ULL << XFEAT_HI16))) {
        if (!__get_cpuid_count(0xD, XFEAT_ZMMH, &a, &b, &c, &d))
            return &L;
        L.zmmh_off = b;
        if (!__get_cpuid_count(0xD, XFEAT_HI16, &a, &b, &c, &d))
            return &L;
        L.hi16_off = b;
        L.width = 64;
        L.nregs = 32;
    }
    L.ok = 1;
    return &L;
}

/* Is [off, off+n) inside a buffer of `sz` bytes? The CPUID offsets are trusted but bounds are
 * cheap, and a mis-sized regset must never read past the buffer. */
static int fits(size_t off, size_t n, size_t sz) {
    return off != 0 && off + n <= sz;
}

/* Read the tracee's vector register file at its current stop and reassemble it into flat
 * register images. Returns 1 on success. A component whose XSTATE_BV bit is clear is INIT
 * (zeros) and is simply left as the zeroed snapshot. */
static int xstate_read(pid_t pid, dfb_vecstate_t *vs) {
    const dfb_xlayout_t *L = dfb_xlayout();
    memset(vs, 0, sizeof *vs);
    if (!L->ok)
        return 0;
    uint8_t buf[DFB_XSTATE_MAX];
    memset(buf, 0, sizeof buf);
    struct iovec iov = {buf, L->bufsz};
    if (ptrace(PTRACE_GETREGSET, pid, (void *)(uintptr_t)NT_X86_XSTATE, &iov) !=
        0)
        return 0;
    size_t got = iov.iov_len;
    if (got < XSAVE_XSTATE_BV + 8)
        return 0;
    uint64_t bv;
    memcpy(&bv, buf + XSAVE_XSTATE_BV, 8);

    /* MXCSR lives in the legacy FXSAVE header, which the kernel's uabi buffer always fills —
     * it is NOT gated on an XSTATE_BV component bit (XSAVE writes it whenever SSE or AVX is in
     * the requested feature bitmap, init state or not). */
    vs->mxcsr = MXCSR_DEFAULT;
    if (fits(XSAVE_MXCSR_OFF, 4, got))
        memcpy(&vs->mxcsr, buf + XSAVE_MXCSR_OFF, 4);

    if ((bv & (1ULL << XFEAT_SSE)) && fits(XSAVE_XMM_OFF, 16 * 16, got))
        for (int i = 0; i < 16; i++)
            memcpy(vs->z[i], buf + XSAVE_XMM_OFF + i * 16, 16);
    if (L->width >= 32 && (bv & (1ULL << XFEAT_YMM)) &&
        fits(L->ymm_off, 16 * 16, got))
        for (int i = 0; i < 16; i++)
            memcpy(vs->z[i] + 16, buf + L->ymm_off + i * 16, 16);
    if (L->width >= 64) {
        if ((bv & (1ULL << XFEAT_ZMMH)) && fits(L->zmmh_off, 16 * 32, got))
            for (int i = 0; i < 16; i++)
                memcpy(vs->z[i] + 32, buf + L->zmmh_off + i * 32, 32);
        if ((bv & (1ULL << XFEAT_HI16)) && fits(L->hi16_off, 16 * 64, got))
            for (int i = 16; i < 32; i++)
                memcpy(vs->z[i], buf + L->hi16_off + (i - 16) * 64, 64);
    }
    vs->width = L->width;
    vs->nregs = L->nregs;
    vs->valid = 1;
    return 1;
}

/* Map a Capstone vector reg id to its register index + architectural width. Returns 1 for a
 * vector register this tier models. The three banks are contiguous in Capstone's enum
 * (asserted by the suite), so the arithmetic is exact. */
static int vec_reg_info(uint32_t reg, int *idx, int *width) {
    if (reg >= X86_REG_XMM0 && reg <= X86_REG_XMM31) {
        *idx = (int)(reg - X86_REG_XMM0);
        *width = 16;
        return 1;
    }
    if (reg >= X86_REG_YMM0 && reg <= X86_REG_YMM31) {
        *idx = (int)(reg - X86_REG_YMM0);
        *width = 32;
        return 1;
    }
    if (reg >= X86_REG_ZMM0 && reg <= X86_REG_ZMM31) {
        *idx = (int)(reg - X86_REG_ZMM0);
        *width = 64;
        return 1;
    }
    return 0;
}

/* How an impure instruction relates to the F2 record-and-inject path. THE SPLIT IS A MEASURED
 * HARDWARE FACT, not a preference — see the file header's F2 section:
 *
 *   DFB_IMP_SYSCALL / DFB_IMP_INT80 — PTRACE_SINGLEBLOCK (DEBUGCTL.BTF) traps these, because
 *     SYSCALL and INT are control transfers. The forward pass therefore gets a real boundary
 *     IMMEDIATELY AFTER the instruction retires, for free, with the kernel's result already in
 *     the registers and its memory delta already in the tracee. That boundary is the entire
 *     mechanism F2 needs.
 *   DFB_IMP_HWREC (T5+T6, F2 increment 2) — rdtsc / rdtscp / rdrand / rdseed / cpuid. MEASURED
 *     on this Zen 5: BTF does NOT trap them (they are not control transfers), so a block runs
 *     straight through them and there is no BTF boundary at which their retired value could be
 *     recorded. capture_blockstep arms a hardware DR exec breakpoint at each scanned site
 *     (region_scan's hwrec_off[]) and single-steps once past a hit (T5), so the boundary
 *     exists; step_block then injects the recorded post-state from that boundary into the
 *     replay and terminates the block there (T6) — the same record-and-inject shape as
 *     SYSCALL/INT80 above, minus the producer-local write-set synthesis (Capstone already
 *     reports the complete write set for all five mnemonics).
 *   DFB_IMP_OTHER — sysenter, and any undecodable byte. No sane return-to-next-instruction
 *     contract (sysenter) or nothing to vouch for (undecodable), so this stays on the
 *     single-step fallback with no DR-breakpoint primitive planned for it.
 */
typedef enum {
    DFB_IMP_NONE = 0,
    DFB_IMP_SYSCALL, /* `syscall`  — clobbers rax (result), rcx (next rip), r11 (rflags) */
    DFB_IMP_INT80, /* `int 0x80` — clobbers rax ONLY (measured: rcx/r11 survive)       */
    DFB_IMP_HWREC, /* rdtsc/rdtscp/rdrand/rdseed/cpuid — DR-breakpoint boundary (T5),
                    * record-and-inject (T6) */
    DFB_IMP_OTHER /* sysenter / undecodable: no BTF boundary, no DR-breakpoint plan either */
} dfb_imp_t;

static dfb_imp_t dfb_impurity_kind(const cs_insn *insn) {
    switch (insn->id) {
    case X86_INS_SYSCALL:
        return DFB_IMP_SYSCALL;
    case X86_INS_INT:
        if (insn->detail->x86.op_count == 1 &&
            insn->detail->x86.operands[0].type == X86_OP_IMM &&
            insn->detail->x86.operands[0].imm == 0x80)
            return DFB_IMP_INT80;
        return DFB_IMP_NONE;
    case X86_INS_RDTSC:
    case X86_INS_RDTSCP:
    case X86_INS_RDRAND:
    case X86_INS_RDSEED:
    case X86_INS_CPUID:
        return DFB_IMP_HWREC;
    case X86_INS_SYSENTER:
        return DFB_IMP_OTHER;
    default:
        return DFB_IMP_NONE;
    }
}

/* ------------------------------------------------------------------ */
/* Shared capture core: one record stream, two value sources           */
/* ------------------------------------------------------------------ */

/* A pluggable memory reader — the single-step path reads the tracee (process_vm_readv), the
 * replay path the Unicorn guest. Returns 1 iff all n bytes were read. */
typedef int (*mem_reader_fn)(void *ctx, uint64_t addr, void *buf, size_t n);

typedef struct {
    at_val_rec_t *v;
    size_t n, cap;
} recbuf;

static void recbuf_push(recbuf *rb, const at_val_rec_t *r) {
    if (rb->n == rb->cap &&
        !asmtest_grow((void **)&rb->v, &rb->cap, rb->n + 1, sizeof *rb->v))
        return;
    rb->v[rb->n++] = *r;
}

typedef struct {
    asmtest_valtrace_t *vt;
    const uint8_t *code;
    size_t code_len;
    uint64_t base;
    int have_cur;
    uint64_t cur_off;
    recbuf cur;
    mem_reader_fn mr;
    void *mr_ctx;
    int want_vec; /* the region references vector state: capture + seed it */
    /* --- F2: per-step impurity classification ---------------------------------
     * A SECOND, INDEPENDENT decoder, deliberately NOT sharing region_scan's answer. region_scan
     * is a LINEAR sweep and can DESYNC (F1's `island` fixture proves it: a constant-pool island
     * makes a `movabs` swallow the next instruction's prefix), so a syscall or an rdtsc hidden
     * behind an island is INVISIBLE to it. This handle decodes at the replay's ACTUAL pc, which
     * follows real control flow — it cannot desync, and it is what stops an unclassified impure
     * instruction reaching Unicorn. F1's own design lesson, applied: the gate and the witness
     * must not be fed by the same scan. */
    csh cs;
    int cs_ok;
    dfb_imp_t
        cur_imp; /* the impurity class of the step open_step just opened */
    uint64_t injected; /* syscalls whose recorded effect the replay injected */
    uint64_t
        hw_hits; /* T5: DR exec-breakpoint site boundaries taken (armed slot faulted,
                        * absorbed by one single-step) — 0 whenever capture_blockstep never runs
                        * (force_singlestep), by construction: nothing arms a DR slot there. */
    /* --- T4: undefined-EFLAGS classification --------------------------------------
     * `cur_undef` / `cur_defined` are the open step's (undefined, defined_written) pair from
     * dfb_undef_flags, set at open time and consumed by the NEXT finalize_step call (the step
     * whose write values that finalize fills is the one open_step just classified — capture_at
     * always finalizes-then-opens, so the ordering is exact). `undef_acc` is the running OR of
     * every replayed instruction's undefined bits since the current block's seed (reset in
     * capture_blockstep, accumulated in step_block), masking the coherence canary so it never
     * fires on a bit the SDM/APM never promised either side would agree on. `no_undef_mask`
     * (opts test hook) forces both to stay 0, the pre-T4 behaviour. */
    uint64_t cur_undef;
    uint64_t cur_defined;
    uint64_t undef_acc;
    int no_undef_mask;
} cap_ctx;

/* ------------------------------------------------------------------ */
/* T4 — undefined-EFLAGS classification (BSVS-1)                       */
/* ------------------------------------------------------------------ */

/* EFLAGS bit positions the table below reasons about — the same six the coherence canary
 * already masks via EFLAGS_ARITH_MASK, named individually so a row can pick its subset. */
#define DFB_EFLAG_CF 0x0001ULL
#define DFB_EFLAG_PF 0x0004ULL
#define DFB_EFLAG_AF 0x0010ULL
#define DFB_EFLAG_ZF 0x0040ULL
#define DFB_EFLAG_SF 0x0080ULL
#define DFB_EFLAG_OF 0x0800ULL
#define DFB_UNDEF_ARITH_ALL                                                    \
    (DFB_EFLAG_CF | DFB_EFLAG_PF | DFB_EFLAG_AF | DFB_EFLAG_ZF |               \
     DFB_EFLAG_SF | DFB_EFLAG_OF)

/* Resolve a shift/rotate instruction's applied count and its destination's bit width.
 * Capstone always normalizes these to TWO operands (dest, count) — even the legacy
 * `shl r/m,1` encoding surfaces an explicit immediate 1 — so the count operand is always
 * the LAST one. An immediate count is used as-is (already masked by the encoding's imm8);
 * a CL-sourced count is read from `gp` (the live pre-instruction register file — the same
 * count the tracee's own execution will apply) and masked exactly as the hardware masks it
 * (5 bits for <=32-bit destinations, 6 bits for 64-bit). Returns 0 (count UNKNOWN) only for
 * the CL form with `gp == NULL`, so the caller can fall back to the conservative union. */
static int dfb_shift_rotate_count(const cs_insn *in,
                                  const struct user_regs_struct *gp,
                                  unsigned *count, unsigned *width) {
    const cs_x86 *x = &in->detail->x86;
    if (x->op_count < 2)
        return 0;
    const cs_x86_op *dst = &x->operands[0];
    const cs_x86_op *cnt = &x->operands[x->op_count - 1];
    *width = dst->size ? (unsigned)dst->size * 8 : 32;
    if (cnt->type == X86_OP_IMM) {
        *count = (unsigned)(cnt->imm & 0xFF);
        return 1;
    }
    if (cnt->type == X86_OP_REG && gp != NULL) {
        unsigned raw = (unsigned)(gp->rcx & 0xFF);
        *count = raw & (*width == 64 ? 0x3F : 0x1F);
        return 1;
    }
    return 0;
}

/* Which EFLAGS bits does `in` leave architecturally UNDEFINED, and (via *defined_written)
 * which bits does it write with a defined value? Neither set implies the other: a bit
 * outside BOTH is simply not touched by this instruction (its prior undefined-ness, if
 * any, survives). The table is the authority (see
 * docs/internal/implementations/dataflow-producer-correctness.md T4 Research notes for the
 * Intel SDM Vol 1 App A / AMD APM Tab F-1 sources); Capstone's own eflags metadata is used
 * ONLY as a breadth signal for the "does this touch flags at all" default below — never to
 * decide which bit is undefined, which is exactly where that metadata is known to lie (VEX
 * forms report mask 0, but this file's replay never reaches a VEX/EVEX instruction — see
 * insn_is_vex_evex — so that specific lie is unreachable here). `gp` may be NULL (e.g. a
 * scan context with no live registers); count-dependent rows then take the conservative
 * union over every possible count. */
static uint64_t dfb_undef_flags(const cs_insn *in,
                                const struct user_regs_struct *gp,
                                uint64_t *defined_written) {
    uint64_t undef = 0, def = 0;
    switch (in->id) {
    case X86_INS_AND:
    case X86_INS_OR:
    case X86_INS_XOR:
    case X86_INS_TEST:
        /* CF/OF cleared (a defined value of 0); SF/ZF/PF defined; AF undefined. */
        undef = DFB_EFLAG_AF;
        def = DFB_EFLAG_CF | DFB_EFLAG_OF | DFB_EFLAG_SF | DFB_EFLAG_ZF |
              DFB_EFLAG_PF;
        break;
    case X86_INS_MUL:
    case X86_INS_IMUL:
        /* Both the 1-operand and 2/3-operand forms share the same SDM/APM undefined set. */
        undef = DFB_EFLAG_SF | DFB_EFLAG_ZF | DFB_EFLAG_AF | DFB_EFLAG_PF;
        def = DFB_EFLAG_CF | DFB_EFLAG_OF;
        break;
    case X86_INS_DIV:
    case X86_INS_IDIV:
        undef = DFB_UNDEF_ARITH_ALL;
        def = 0;
        break;
    case X86_INS_BSF:
    case X86_INS_BSR:
        undef = DFB_EFLAG_CF | DFB_EFLAG_OF | DFB_EFLAG_SF | DFB_EFLAG_AF |
                DFB_EFLAG_PF;
        def = DFB_EFLAG_ZF;
        break;
    case X86_INS_SHL:
    case X86_INS_SAL:
    case X86_INS_SHR:
    case X86_INS_SAR: {
        unsigned count = 0, width = 0;
        if (!dfb_shift_rotate_count(in, gp, &count, &width)) {
            if (in->id == X86_INS_SAR) {
                undef = DFB_EFLAG_AF | DFB_EFLAG_OF;
                def = DFB_EFLAG_CF | DFB_EFLAG_SF | DFB_EFLAG_ZF | DFB_EFLAG_PF;
            } else {
                undef = DFB_EFLAG_AF | DFB_EFLAG_OF | DFB_EFLAG_CF;
                def = DFB_EFLAG_SF | DFB_EFLAG_ZF | DFB_EFLAG_PF;
            }
            break;
        }
        if (count == 0) {
            undef = 0;
            def = 0; /* count 0: the instruction writes nothing */
        } else if (count == 1) {
            undef = DFB_EFLAG_AF;
            def = DFB_EFLAG_CF | DFB_EFLAG_OF | DFB_EFLAG_SF | DFB_EFLAG_ZF |
                  DFB_EFLAG_PF;
        } else {
            undef = DFB_EFLAG_AF | DFB_EFLAG_OF;
            def = DFB_EFLAG_SF | DFB_EFLAG_ZF | DFB_EFLAG_PF | DFB_EFLAG_CF;
            /* SHL/SHR/SAL (not SAR) additionally undefine CF once the count reaches the
             * destination's width — the shifted-out bit no longer exists. */
            if (in->id != X86_INS_SAR && count >= width) {
                undef |= DFB_EFLAG_CF;
                def &= ~DFB_EFLAG_CF;
            }
        }
        break;
    }
    case X86_INS_ROL:
    case X86_INS_ROR:
    case X86_INS_RCL:
    case X86_INS_RCR: {
        unsigned count = 0, width = 0;
        if (!dfb_shift_rotate_count(in, gp, &count, &width)) {
            undef = DFB_EFLAG_OF;
            def = DFB_EFLAG_CF;
            break;
        }
        (void)
            width; /* rotate's undefined-ness does not depend on operand width */
        if (count == 0) {
            undef = 0;
            def = 0;
        } else if (count == 1) {
            undef = 0;
            def = DFB_EFLAG_CF | DFB_EFLAG_OF;
        } else {
            undef = DFB_EFLAG_OF;
            def = DFB_EFLAG_CF; /* CF is defined at every non-zero count */
        }
        break;
    }
    case X86_INS_BT:
    case X86_INS_BTS:
    case X86_INS_BTR:
    case X86_INS_BTC:
        undef = DFB_EFLAG_OF | DFB_EFLAG_SF | DFB_EFLAG_AF | DFB_EFLAG_PF;
        def = DFB_EFLAG_CF;
        break;
    case X86_INS_LZCNT:
    case X86_INS_TZCNT:
    case X86_INS_POPCNT:
        undef = 0;
        def = DFB_UNDEF_ARITH_ALL;
        break;
    default:
        /* Not in the explicit table. An instruction Capstone reports as touching NO flags at
         * all leaves undef_acc exactly as it was (correct: an untouched flag's prior status,
         * defined or not, survives). One that DOES touch flags but isn't one of the special
         * cases above is, by construction, fully flag-defining on real x86 (ADD/SUB/CMP/
         * ADC/SBB/NEG/INC/DEC and friends never leave a bit undefined per the SDM) — so it
         * resets undef_acc for the whole arithmetic mask. Capstone's eflags field is used
         * here ONLY as a touches-flags-at-all signal (a breadth question its own auto-sync
         * gets right even where the PER-BIT detail is unreliable — see the file comment). */
        if (in->detail != NULL && in->detail->x86.eflags != 0) {
            undef = 0;
            def = DFB_UNDEF_ARITH_ALL;
        }
        break;
    }
    if (defined_written != NULL)
        *defined_written = def;
    return undef;
}

/* Classify the instruction at blob offset `off` — impurity class, its byte length via
 * *len_out, and (T4) its undefined/defined-written EFLAGS split via undef_out/defined_out.
 * Decodes ONE instruction at the real pc. Fails CLOSED on impurity: an undecodable byte is
 * reported DFB_IMP_OTHER, i.e. "not something the replay may execute", because an instruction
 * we cannot decode is precisely one we cannot vouch for. `gp` is the pre-instruction register
 * file (for CL-sourced shift/rotate counts); may be NULL. */
static dfb_imp_t dfb_classify_at(cap_ctx *c, uint64_t off,
                                 const struct user_regs_struct *gp,
                                 size_t *len_out, uint64_t *undef_out,
                                 uint64_t *defined_out) {
    if (len_out != NULL)
        *len_out = 0;
    if (undef_out != NULL)
        *undef_out = 0;
    if (defined_out != NULL)
        *defined_out = 0;
    if (!c->cs_ok || off >= c->code_len)
        return DFB_IMP_OTHER;
    cs_insn *insn = cs_malloc(c->cs);
    if (insn == NULL)
        return DFB_IMP_OTHER;
    const uint8_t *p = c->code + off;
    size_t remaining = c->code_len - (size_t)off;
    uint64_t addr = off;
    dfb_imp_t k = DFB_IMP_OTHER;
    if (cs_disasm_iter(c->cs, &p, &remaining, &addr, insn)) {
        k = dfb_impurity_kind(insn);
        if (len_out != NULL)
            *len_out = insn->size;
        if (!c->no_undef_mask && (undef_out != NULL || defined_out != NULL)) {
            uint64_t def = 0;
            uint64_t undef = dfb_undef_flags(insn, gp, &def);
            if (undef_out != NULL)
                *undef_out = undef;
            if (defined_out != NULL)
                *defined_out = def;
        }
    }
    cs_free(insn, 1);
    return k;
}

/* Map a Capstone x86 register id to its 64-bit container value in a GP register file,
 * folding sub-registers to the container. EFLAGS is masked of the debug-stepping bits so
 * both value sources agree. Returns 0 for regs not in this file (vector / segment
 * selectors), whose value is then left uncaptured — none of the pure fixtures hit it. */
static int gp_value(const struct user_regs_struct *r, uint32_t reg,
                    uint64_t *out) {
    switch (reg) {
    case X86_REG_RAX:
    case X86_REG_EAX:
    case X86_REG_AX:
    case X86_REG_AL:
    case X86_REG_AH:
        *out = r->rax;
        return 1;
    case X86_REG_RBX:
    case X86_REG_EBX:
    case X86_REG_BX:
    case X86_REG_BL:
    case X86_REG_BH:
        *out = r->rbx;
        return 1;
    case X86_REG_RCX:
    case X86_REG_ECX:
    case X86_REG_CX:
    case X86_REG_CL:
    case X86_REG_CH:
        *out = r->rcx;
        return 1;
    case X86_REG_RDX:
    case X86_REG_EDX:
    case X86_REG_DX:
    case X86_REG_DL:
    case X86_REG_DH:
        *out = r->rdx;
        return 1;
    case X86_REG_RSI:
    case X86_REG_ESI:
    case X86_REG_SI:
    case X86_REG_SIL:
        *out = r->rsi;
        return 1;
    case X86_REG_RDI:
    case X86_REG_EDI:
    case X86_REG_DI:
    case X86_REG_DIL:
        *out = r->rdi;
        return 1;
    case X86_REG_RBP:
    case X86_REG_EBP:
    case X86_REG_BP:
    case X86_REG_BPL:
        *out = r->rbp;
        return 1;
    case X86_REG_RSP:
    case X86_REG_ESP:
    case X86_REG_SP:
    case X86_REG_SPL:
        *out = r->rsp;
        return 1;
    case X86_REG_R8:
    case X86_REG_R8D:
    case X86_REG_R8W:
    case X86_REG_R8B:
        *out = r->r8;
        return 1;
    case X86_REG_R9:
    case X86_REG_R9D:
    case X86_REG_R9W:
    case X86_REG_R9B:
        *out = r->r9;
        return 1;
    case X86_REG_R10:
    case X86_REG_R10D:
    case X86_REG_R10W:
    case X86_REG_R10B:
        *out = r->r10;
        return 1;
    case X86_REG_R11:
    case X86_REG_R11D:
    case X86_REG_R11W:
    case X86_REG_R11B:
        *out = r->r11;
        return 1;
    case X86_REG_R12:
    case X86_REG_R12D:
    case X86_REG_R12W:
    case X86_REG_R12B:
        *out = r->r12;
        return 1;
    case X86_REG_R13:
    case X86_REG_R13D:
    case X86_REG_R13W:
    case X86_REG_R13B:
        *out = r->r13;
        return 1;
    case X86_REG_R14:
    case X86_REG_R14D:
    case X86_REG_R14W:
    case X86_REG_R14B:
        *out = r->r14;
        return 1;
    case X86_REG_R15:
    case X86_REG_R15D:
    case X86_REG_R15W:
    case X86_REG_R15B:
        *out = r->r15;
        return 1;
    case X86_REG_RIP:
        *out = r->rip;
        return 1;
    case X86_REG_EFLAGS:
        *out = (uint64_t)r->eflags & ~EFLAGS_STEP_BITS;
        return 1;
    default:
        return 0;
    }
}

/* T6: map a Capstone x86 GP/EFLAGS register id (any width) to its Unicorn 64-bit CONTAINER
 * register constant — the write-side mirror of gp_value's read-side grouping, so the two agree
 * on which container a sub-register record belongs to. Used only for the HWREC injection path:
 * rdtsc/rdtscp/cpuid/rdrand/rdseed write exclusively to GP registers + EFLAGS (never memory,
 * never vector), so this need not handle anything else. Returns 0 for a register outside that
 * set (the caller then leaves the record uninjected — none of the five mnemonics hit it). */
static int uc_gp_container(uint32_t reg, int *uc_reg_out) {
    switch (reg) {
    case X86_REG_RAX:
    case X86_REG_EAX:
    case X86_REG_AX:
    case X86_REG_AL:
    case X86_REG_AH:
        *uc_reg_out = UC_X86_REG_RAX;
        return 1;
    case X86_REG_RBX:
    case X86_REG_EBX:
    case X86_REG_BX:
    case X86_REG_BL:
    case X86_REG_BH:
        *uc_reg_out = UC_X86_REG_RBX;
        return 1;
    case X86_REG_RCX:
    case X86_REG_ECX:
    case X86_REG_CX:
    case X86_REG_CL:
    case X86_REG_CH:
        *uc_reg_out = UC_X86_REG_RCX;
        return 1;
    case X86_REG_RDX:
    case X86_REG_EDX:
    case X86_REG_DX:
    case X86_REG_DL:
    case X86_REG_DH:
        *uc_reg_out = UC_X86_REG_RDX;
        return 1;
    case X86_REG_RSI:
    case X86_REG_ESI:
    case X86_REG_SI:
    case X86_REG_SIL:
        *uc_reg_out = UC_X86_REG_RSI;
        return 1;
    case X86_REG_RDI:
    case X86_REG_EDI:
    case X86_REG_DI:
    case X86_REG_DIL:
        *uc_reg_out = UC_X86_REG_RDI;
        return 1;
    case X86_REG_RBP:
    case X86_REG_EBP:
    case X86_REG_BP:
    case X86_REG_BPL:
        *uc_reg_out = UC_X86_REG_RBP;
        return 1;
    case X86_REG_RSP:
    case X86_REG_ESP:
    case X86_REG_SP:
    case X86_REG_SPL:
        *uc_reg_out = UC_X86_REG_RSP;
        return 1;
    case X86_REG_R8:
    case X86_REG_R8D:
    case X86_REG_R8W:
    case X86_REG_R8B:
        *uc_reg_out = UC_X86_REG_R8;
        return 1;
    case X86_REG_R9:
    case X86_REG_R9D:
    case X86_REG_R9W:
    case X86_REG_R9B:
        *uc_reg_out = UC_X86_REG_R9;
        return 1;
    case X86_REG_R10:
    case X86_REG_R10D:
    case X86_REG_R10W:
    case X86_REG_R10B:
        *uc_reg_out = UC_X86_REG_R10;
        return 1;
    case X86_REG_R11:
    case X86_REG_R11D:
    case X86_REG_R11W:
    case X86_REG_R11B:
        *uc_reg_out = UC_X86_REG_R11;
        return 1;
    case X86_REG_R12:
    case X86_REG_R12D:
    case X86_REG_R12W:
    case X86_REG_R12B:
        *uc_reg_out = UC_X86_REG_R12;
        return 1;
    case X86_REG_R13:
    case X86_REG_R13D:
    case X86_REG_R13W:
    case X86_REG_R13B:
        *uc_reg_out = UC_X86_REG_R13;
        return 1;
    case X86_REG_R14:
    case X86_REG_R14D:
    case X86_REG_R14W:
    case X86_REG_R14B:
        *uc_reg_out = UC_X86_REG_R14;
        return 1;
    case X86_REG_R15:
    case X86_REG_R15D:
    case X86_REG_R15W:
    case X86_REG_R15B:
        *uc_reg_out = UC_X86_REG_R15;
        return 1;
    case X86_REG_EFLAGS:
        *uc_reg_out = UC_X86_REG_EFLAGS;
        return 1;
    default:
        return 0;
    }
}

/* Resolve a memory operand's effective address from a register file: seg_base + base +
 * index*scale + disp, with fs_base/gs_base segment resolution and the RIP-relative
 * next-instruction fixup. Mirrors dataflow_ptrace.c resolve_ea so the core is drop-in. */
static uint64_t resolve_ea(const struct user_regs_struct *regs,
                           const at_val_rec_t *r, size_t insn_len) {
    uint64_t ea = (uint64_t)r->disp;
    if (r->reg == X86_REG_GS)
        ea += regs->gs_base;
    else if (r->reg == X86_REG_FS)
        ea += regs->fs_base;
    if (r->base != 0) {
        uint64_t bv;
        if (gp_value(regs, r->base, &bv)) {
            ea += bv;
            if (r->base == X86_REG_RIP)
                ea += insn_len;
        }
    }
    if (r->index != 0 && r->scale != 0) {
        uint64_t iv;
        if (gp_value(regs, r->index, &iv))
            ea += iv * (uint64_t)r->scale;
    }
    return ea;
}

/* Spill a >8-byte value into the sink's wide[] side buffer and point the record at it — the
 * path asmtest_valtrace.h documents for XMM/YMM/ZMM-width values. Returns 1 on success; a
 * full wide[] leaves the record value-less (and the sink flags `truncated` itself). */
static int stash_wide_value(cap_ctx *c, at_val_rec_t *r, const uint8_t *bytes,
                            size_t n) {
    size_t off = asmtest_valtrace_stash_wide(c->vt, bytes, n);
    if (off == (size_t)-1)
        return 0;
    r->wide = true;
    r->wide_off = (uint32_t)off;
    r->value = 0;
    r->value_valid = true;
    return 1;
}

/* Memory operand values: <= 8 bytes inline, wider (a vector load/store: 16 / 32 / 64) spilled
 * to wide[]. Reading through the pluggable reader keeps the tracee and the Unicorn guest on
 * one code path, so the oracle and the replay build byte-comparable records. */
static void fill_mem_value(cap_ctx *c, at_val_rec_t *r) {
    uint16_t sz = r->size;
    if (sz == 0 || sz > 64)
        return; /* this tier captures memory operands <= 64 bytes (ZMM width) */
    uint8_t buf[64] = {0};
    if (!c->mr(c->mr_ctx, r->addr, buf, sz))
        return;
    if (sz <= 8) {
        r->value = 0;
        memcpy(&r->value, buf, sz);
        r->value_valid = true;
        return;
    }
    stash_wide_value(c, r, buf, sz);
}

/* A vector REGISTER record's value, taken from a boundary snapshot at the register's
 * architectural width. Returns 1 iff the record was filled. A snapshot narrower than the
 * register (e.g. the replay, which models XMM only) declines rather than reporting a
 * zero-extended lie — an unfilled record stays value_valid = false, which is honest. */
static int fill_vec_value(cap_ctx *c, at_val_rec_t *r, const dfb_snap_t *s) {
    int idx, w;
    if (!vec_reg_info(r->reg, &idx, &w))
        return 0;
    r->size =
        (uint16_t)w; /* the operand enumerator leaves register widths at 0 */
    if (!s->vec.valid || idx >= s->vec.nregs || w > s->vec.width)
        return 0;
    if (w <=
        8) { /* unreachable today; keeps the inline/wide split in one place */
        r->value = 0;
        memcpy(&r->value, s->vec.z[idx], (size_t)w);
        r->value_valid = true;
        return 1;
    }
    return stash_wide_value(c, r, s->vec.z[idx], (size_t)w);
}

/* Finalize the current step's deferred WRITE values from the POST-instruction snapshot and
 * append the step. Mirrors dataflow_ptrace.c finalize_step. */
static void finalize_step(cap_ctx *c, const dfb_snap_t *s) {
    for (size_t i = 0; i < c->cur.n; i++) {
        at_val_rec_t *r = &c->cur.v[i];
        if (!r->is_write || r->value_valid)
            continue;
        if (r->kind == AT_LOC_REG) {
            uint64_t v;
            if (gp_value(&s->gp, r->reg, &v)) {
                /* T4: an EFLAGS write record must not present an architecturally undefined
                 * bit as if silicon (or the replay) defined it. `cur_undef` still describes
                 * THIS step — capture_at finalizes the pending step before open_step
                 * reclassifies cur_undef/cur_defined for the next one. */
                if (r->reg == X86_REG_EFLAGS)
                    v &= ~c->cur_undef;
                r->value = v;
                r->value_valid = true;
            } else {
                fill_vec_value(c, r, s);
            }
        } else {
            fill_mem_value(c, r); /* addr resolved when the step opened */
        }
    }
    asmtest_valtrace_append(c->vt, c->cur_off, c->cur.v, c->cur.n);
    c->have_cur = 0;
    c->cur.n = 0;
}

/* Open the step at `regs->rip`: enumerate its read/write set, capture READ values from this
 * PRE-state (registers via gp_value, memory via the reader at the resolved EA), resolve store
 * addresses, defer WRITE values. Returns the instruction byte length. Mirrors dataflow_ptrace.c
 * open_step. */
static size_t open_step(cap_ctx *c, const dfb_snap_t *s) {
    uint64_t off = s->gp.rip - c->base;
    c->cur.n = 0;
    c->cur_off = off;
    c->have_cur = 1;
    /* Classify THIS step's instruction from its real pc. Both value sources call through here,
     * so the single-step oracle and the replay agree on the syscall write set below; step_block
     * additionally reads c->cur_imp to decide whether the replay may execute it at all. */
    c->cur_imp =
        dfb_classify_at(c, off, &s->gp, NULL, &c->cur_undef, &c->cur_defined);

    at_val_rec_t rd[64], wr[64];
    size_t nr = 64, nw = 64;
    size_t insn_len = asmtest_operands(ASMTEST_ARCH_X86_64, c->code,
                                       c->code_len, off, rd, &nr, wr, &nw);

    /* F2 — THE SYSCALL'S OWN WRITE SET, supplied producer-locally.
     *
     * MEASURED: the shared operand enumerator reports `syscall` and `int 0x80` as touching NO
     * registers at all (0 reads, 0 writes) — Capstone models the instruction, not the ABI. So
     * without this, the syscall step would carry ZERO records and the kernel's result would
     * never appear in the value trace: the L1 def-use builder would see the downstream `add
     * rax, rax` READ a register that nothing in the trace ever DEFINED, and the edge back to
     * the syscall — the one edge a reader of an impure method actually wants — would not exist.
     * "Record the syscall's effect" is precisely this record.
     *
     * The set is architectural, not a per-syscall table: SYSCALL clobbers rax (the kernel's
     * return), rcx (= the next rip) and r11 (= the pre-syscall rflags). INT 0x80 goes through
     * an interrupt gate and clobbers rax ONLY — measured on this host, and the distinction is
     * real: claiming rcx/r11 defs for int 0x80 would forge two defs that never happened.
     *
     * Both value sources go through this ONE function, so the single-step oracle and the
     * block-step replay grow the same records automatically and stay comparable. The values are
     * deferred like any other write and land from the NEXT snapshot — which on both paths is a
     * REAL post-syscall boundary, never a fabrication. */
    if (c->cur_imp == DFB_IMP_SYSCALL || c->cur_imp == DFB_IMP_INT80) {
        static const uint32_t sc_regs[] = {X86_REG_RAX, X86_REG_RCX,
                                           X86_REG_R11};
        size_t n_sc = (c->cur_imp == DFB_IMP_SYSCALL) ? 3 : 1;
        for (size_t i = 0; i < n_sc && nw < 64; i++) {
            at_val_rec_t r;
            memset(&r, 0, sizeof r);
            r.kind = AT_LOC_REG;
            r.reg = sc_regs[i];
            r.size = 8;
            r.is_write = true;
            wr[nw++] = r;
        }
    }

    /* T5 (F2 increment 2) supplement check, per the same "supply what Capstone under-reports"
     * pattern as the syscall block above — but PROBED rather than assumed: MEASURED against the
     * pinned Capstone 5.0.1, cs_regs_access ALREADY reports the complete architectural write set
     * for all five DFB_IMP_HWREC mnemonics with no supplement needed: rdtsc -> rax,rdx; rdtscp ->
     * rax,rcx,rdx; cpuid -> reads eax,ecx / writes eax,ebx,ecx,edx; rdrand/rdseed -> the dest reg
     * PLUS eflags (not merely CF — the whole register, which the shared enumerator already turns
     * into an EFLAGS write record via put_reg, same as any other flag-writing instruction). So
     * unlike `syscall`/`int 0x80` (which Capstone models with ZERO register accesses, the ABI
     * effect being invisible to it), nothing is added here — the read/write sets `rd`/`wr` above
     * already carry it. Left as a comment, not dead code, so a future Capstone re-sync that
     * regresses this (the file header documents this pipeline's own metadata lying for AVX
     * elsewhere) has a place pointing back at the probe that proved it, and a diff that adds a
     * per-mnemonic supplement here is the fix if it ever does. */

    for (size_t i = 0; i < nr; i++) {
        at_val_rec_t r = rd[i];
        if (r.kind == AT_LOC_REG) {
            uint64_t v;
            if (gp_value(&s->gp, r.reg, &v)) {
                r.value = v;
                r.value_valid = true;
            } else {
                fill_vec_value(c, &r, s);
            }
        } else {
            r.addr = resolve_ea(&s->gp, &r, insn_len);
            fill_mem_value(c, &r); /* load value is in memory pre-instruction */
        }
        recbuf_push(&c->cur, &r);
    }
    for (size_t i = 0; i < nw; i++) {
        at_val_rec_t r = wr[i];
        r.value_valid = false;
        if (r.kind != AT_LOC_REG)
            r.addr = resolve_ea(&s->gp, &r, insn_len);
        recbuf_push(&c->cur, &r);
    }
    return insn_len;
}

/* One captured step: finalize the previous step with this stop's snapshot (its post-state) and
 * open the current one (its pre-state) — the single point of contact for both paths. Returns
 * the current instruction's byte length. */
static size_t capture_at(cap_ctx *c, const dfb_snap_t *s) {
    if (c->have_cur)
        finalize_step(c, s);
    return open_step(c, s);
}

/* ------------------------------------------------------------------ */
/* Memory readers                                                       */
/* ------------------------------------------------------------------ */
static int mr_tracee(void *ctx, uint64_t addr, void *buf, size_t n) {
    pid_t pid = *(pid_t *)ctx;
    struct iovec l = {buf, n};
    struct iovec r = {(void *)(uintptr_t)addr, n};
    return process_vm_readv(pid, &l, 1, &r, 1, 0) == (ssize_t)n;
}
static int mr_uc(void *ctx, uint64_t addr, void *buf, size_t n) {
    uc_engine *uc = (uc_engine *)ctx;
    return uc_mem_read(uc, addr, buf, n) == UC_ERR_OK;
}

/* Read a page-aligned WINDOW out of the tracee PAGE BY PAGE, tolerating pages that are not
 * mapped there (those stay zero). Returns the bytes actually recovered.
 *
 * This is load-bearing, not defensive padding. process_vm_readv is ATOMIC per iovec: if ANY
 * byte of the range is unmapped it fails for the WHOLE range. The replay's stack window
 * [rsp-0x1000, rsp+0x2000) routinely runs off the TOP of the tracee's [stack] VMA — how close
 * rsp sits to the top depends on call depth and the kernel's stack randomization — so a
 * single-iovec read fails outright on a perfectly healthy tracee. The old all-or-nothing read
 * then "recovered" by ZEROING the entire window, so the replayed `ret` popped a return address
 * of 0 and the capture died UC_ERR_FETCH_UNMAPPED. Measured at ~27% of runs inside the
 * dataflow-attach container and 0% on the host (whose stack sits deeper under a larger
 * environment) — which is exactly why it survived increment 1: the block-step lane self-skips
 * on GitHub's BTF-masked runners, so nothing ever exercised it. Per-page reads recover the
 * pages that DO exist, which always include the frame the region actually touches. */
static size_t mr_tracee_window(pid_t pid, uint64_t addr, uint8_t *buf,
                               size_t n) {
    memset(buf, 0, n);
    size_t got = 0;
    for (size_t off = 0; off < n; off += 0x1000) {
        size_t chunk = (n - off) < 0x1000 ? (n - off) : 0x1000;
        if (mr_tracee(&pid, addr + off, buf + off, chunk))
            got += chunk;
    }
    return got;
}

/* ------------------------------------------------------------------ */
/* Region static scan (F1 region-granularity classifier)               */
/* ------------------------------------------------------------------ */

/* What one linear sweep of the region's bytes decides. Both gates route to the SAME
 * single-step fallback, but for different reasons, so they are reported separately. */
typedef struct {
    int pure; /* no syscall / sysenter / int 0x80 / rdtsc[p] / rdrand / rdseed / cpuid */
    int injectable; /* F2: impure, but EVERY impurity found is a syscall / int 0x80 —
                       * an impurity record-and-inject can carry (see dfb_impurity_kind) */
    int replayable; /* no VEX/EVEX-encoded instruction (see insn_is_vex_evex)                */
    int touches_vec; /* references vector state, so the capture must snapshot + seed it      */
    /* The two verdicts have INDEPENDENT reasons, because a region can fail both gates and
     * `pure` must not mask a replayability verdict a public caller asked for. */
    const char *
        impure_reason; /* the offending mnemonic, when !pure                   */
    const char *
        replay_reason; /* "vex/evex" or "decode", when !replayable             */
    /* T5 (F2 increment 2, forward pass): rdtsc/rdtscp/rdrand/rdseed/cpuid sites the sweep
     * found, by their offset in THIS scanned buffer — capture_blockstep arms one hardware DR
     * exec breakpoint per entry (absolute rbase + hwrec_off[i]). Capped at 4, the architectural
     * DR0-3 slot count: more than 4 DISTINCT sites sets hwrec_overflow and the region keeps the
     * whole-region single-step fallback rather than lie about a 5th site it cannot watch.
     * Consulted by `injectable` as of T6 (record-and-inject over the DR-breakpoint boundary),
     * subject to that same overflow cap. */
    uint64_t hwrec_off[4];
    size_t nhwrec;
    int hwrec_overflow;
} dfb_scan_t;

/* Does the region get the block-step + replay path? F2 widens F1's rule: a region is no longer
 * required to be PURE, only to have no impurity the replay cannot honestly carry. */
static int scan_replay_ok(const dfb_scan_t *s) {
    return (s->pure || s->injectable) && s->replayable;
}

/* Why the replay path was declined, if it was. An impurity we cannot inject is the stronger
 * verdict, so it names the region when both gates fire. */
static const char *scan_reason(const dfb_scan_t *s) {
    if (!s->pure && !s->injectable)
        return s->impure_reason;
    if (!s->replayable)
        return s->replay_reason;
    return NULL;
}

/* Does this instruction carry a VEX or EVEX encoding? A byte-level fact, exact on x86-64:
 * C4/C5 are always VEX and 62 is always EVEX there (BOUND / LDS / LES are invalid in 64-bit
 * mode), and only segment / address-size overrides may legally precede them (a 66/F2/F3/REX
 * before VEX is #UD).
 *
 * UPSTREAM GATE (T8): this whole encoding-level rule rests on no released Unicorn being able
 * to execute VEX/EVEX correctly — see the sentinel check (run_avx_tcg_sentinel_case) in
 * examples/test_dataflow_blockstep.c, which makes that dependency explicit and self-testing.
 * On FAILURE of that sentinel (i.e. once Unicorn ships QEMU >= 7.2 TCG), see
 * docs/internal/implementations/dataflow-producer-correctness.md T8 for the trigger condition
 * and the pin-bump playbook before relaxing anything here.
 *
 * This deliberately keys on the ENCODING rather than on Capstone's AVX metadata, which is
 * measurably incomplete: `vpbroadcastq zmm0,xmm0` decodes with correct mnemonic and operands
 * yet cs_regs_access reports it touching NO registers and it is in NO X86_GRP_AVX/AVX2/AVX512
 * group. A gate built on that metadata would silently pass EVEX through to a replay that
 * mis-executes it. The encoding rule cannot miss, at the cost of also gating VEX-GP (BMI),
 * which Unicorn does run correctly — over-gating only forfeits the perturbation win, while
 * under-gating forfeits correctness.
 *
 * MEASURED COST of that conservatism, over this repo's own sources compiled four ways (224
 * functions): -O2 and -O3 decline 0% (baseline x86-64 emits no VEX at all), -O3 -mavx2 and
 * -O3 -march=native decline 17.3% — and ALL of those contain genuine vector VEX/EVEX, which no
 * released Unicorn can run anyway. The BMI over-gate cost **0 functions**: there was no region
 * declined solely for VEX-GP. So the rule is not merely safe-by-argument, it is close to free
 * in practice, and a BMI allowlist would currently buy nothing. */
static int insn_is_vex_evex(const cs_insn *in) {
    for (uint16_t i = 0; i < in->size; i++) {
        uint8_t b = in->bytes[i];
        if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 ||
            b == 0x65 || b == 0x67)
            continue; /* a segment / address-size override may precede VEX */
        return b == 0xC4 || b == 0xC5 || b == 0x62;
    }
    return 0;
}

static int is_vec_or_mask_reg(uint32_t r) {
    int idx, w;
    if (vec_reg_info(r, &idx, &w))
        return 1;
    return r >= X86_REG_K0 && r <= X86_REG_K7;
}

/* Does the instruction reference vector state? Used only to decide whether a capture must pay
 * for an XSTATE read per stop, so it errs toward YES: any VEX/EVEX encoding counts even when
 * cs_regs_access reports nothing (the EVEX gap above), and an unavailable regs_access counts. */
static int insn_touches_vec(csh h, cs_insn *in) {
    if (insn_is_vex_evex(in))
        return 1;
    cs_regs rr, rw;
    uint8_t nr = 0, nw = 0;
    if (cs_regs_access(h, in, rr, &nr, rw, &nw) != CS_ERR_OK)
        return 1;
    for (uint8_t i = 0; i < nr; i++)
        if (is_vec_or_mask_reg(rr[i]))
            return 1;
    for (uint8_t i = 0; i < nw; i++)
        if (is_vec_or_mask_reg(rw[i]))
            return 1;
    for (uint8_t i = 0; i < in->detail->x86.op_count; i++)
        if (in->detail->x86.operands[i].type == X86_OP_REG &&
            is_vec_or_mask_reg(in->detail->x86.operands[i].reg))
            return 1;
    return 0;
}

/* Which OS-interacting / nondeterministic instruction is this, if any? NULL = pure. */
static const char *insn_impurity(const cs_insn *insn) {
    switch (insn->id) {
    case X86_INS_SYSCALL:
        return "syscall";
    case X86_INS_SYSENTER:
        return "sysenter";
    case X86_INS_RDTSC:
        return "rdtsc";
    case X86_INS_RDTSCP:
        return "rdtscp";
    case X86_INS_RDRAND:
        return "rdrand";
    case X86_INS_RDSEED:
        return "rdseed";
    case X86_INS_CPUID:
        return "cpuid";
    case X86_INS_INT:
        /* int 0x80 is the legacy syscall gate; the plan names it specifically. */
        if (insn->detail->x86.op_count == 1 &&
            insn->detail->x86.operands[0].type == X86_OP_IMM &&
            insn->detail->x86.operands[0].imm == 0x80)
            return "int 0x80";
        return NULL;
    default:
        return NULL;
    }
}

/* Linearly disassemble ONE extent [addr0, addr0+extlen) of `code` and fold its findings into
 * `out`. Shared by region_scan's whole-region call (one implicit extent, [0, len)) and its
 * per-extent calls (T7) — the per-instruction classification is identical either way; only the
 * byte RANGE fed to cs_disasm_iter differs. `insn` is caller-owned (cs_malloc'd once, reused
 * across extents) so a multi-extent region pays for exactly one allocation, not one per extent.
 *
 * THIS SWEEP FAILS CLOSED, DELIBERATELY, AND THAT IS THE WHOLE POINT — over EVERY extent it is
 * given. It is a single point of failure feeding BOTH the replayability gate AND (via
 * touches_vec) the vector seed and canary, so a wrong verdict does not merely lose a check, it
 * lets an instruction through AND removes the check that would have caught it. Three ways it
 * used to fail OPEN, each now closed:
 *
 *   1. DECODER DESYNC. `remaining != 0` after the loop means cs_disasm_iter stopped early WITHIN
 *      this extent — an embedded constant-pool island can make a `movabs` swallow a following
 *      VEX prefix as immediate data, so the sweep ends with the OPTIMISTIC initial verdicts
 *      still in place and a VEX-128 reaches the replay ungated AND unwitnessed. Measured:
 *      `jmp +2 / movabs-island / vpaddq xmm0,xmm1,xmm2 / movups` → replayable=1 touches_vec=0
 *      with remaining=5 of 17. T7's whole point is that a CALLER-VOUCHED extent boundary makes
 *      this rare rather than load-bearing: the island's bytes, sitting BETWEEN two extents,
 *      are simply never fetched, so there is nothing here to desync on.
 *   2. THE IMPURITY EARLY BREAK. Aborting the sweep at the first impure instruction left every
 *      vector instruction AFTER it unseen, so touches_vec=0 → no XSTATE read on the single-step
 *      fallback → every vector record emitted value_valid=0 at rc=OK. Only the PURITY answer is
 *      settled early; the sweep must still classify the whole extent.
 *   3. cs_open FAILING. Handled by region_scan before this is ever called.
 *
 * A linear sweep is still only exact for a straight instruction stream; failing closed per
 * extent is what makes that honest when a caller's extent list is itself wrong (e.g. the
 * "extent" still contains an undecodable byte, or a real instruction longer than what the
 * caller vouched for). */
static void region_scan_extent(csh h, cs_insn *insn, const uint8_t *code,
                               uint64_t addr0, size_t extlen, dfb_scan_t *out) {
    const uint8_t *p = code + addr0;
    uint64_t addr = addr0;
    size_t remaining = extlen;
    while (remaining > 0 && cs_disasm_iter(h, &p, &remaining, &addr, insn)) {
        if (insn_touches_vec(h, insn))
            out->touches_vec = 1;
        if (out->replayable && insn_is_vex_evex(insn)) {
            out->replayable = 0;
            /* No released Unicorn executes VEX/EVEX, and VEX-128 mis-executes SILENTLY; see
             * the file header. Named for the ENCODING the gate keys on, not for "AVX" — the
             * rule also (deliberately) catches VEX-GP such as BMI's andn. */
            out->replay_reason = "vex/evex";
        }
        const char *imp = insn_impurity(insn);
        if (imp != NULL && out->pure) {
            out->pure =
                0; /* the FIRST impure instruction names the region... */
            out->impure_reason = imp;
        }
        /* F2: `injectable` is NOT settled early — it is an ALL-quantifier ("every impurity in
         * this region is one the replay can carry"), so it must see the WHOLE region (every
         * extent, not just this one). A region whose first impurity is a syscall and whose
         * second is a sysenter is NOT injectable, and settling at the first would be exactly
         * the HIGH-3 early-break bug in a new costume. */
        dfb_imp_t k = imp != NULL ? dfb_impurity_kind(insn) : DFB_IMP_NONE;
        if (imp != NULL && k != DFB_IMP_SYSCALL && k != DFB_IMP_INT80 &&
            k != DFB_IMP_HWREC) {
            /* None of the three kinds the replay can carry (SYSCALL/INT80/HWREC, as of T6) —
             * this leaves DFB_IMP_OTHER (sysenter/undecodable), which has no BTF boundary and
             * no DR-breakpoint plan either. */
            out->injectable = 0;
            /* Name the impurity that actually DISQUALIFIES the region, not merely the first one
             * seen: with `syscall; sysenter` the honest reason is "sysenter" — a caller told
             * "syscall" would go looking for a gate that no longer exists. */
            out->impure_reason = imp;
        }
        if (k == DFB_IMP_HWREC) {
            /* Distinct by construction: a linear sweep over non-overlapping extents visits each
             * byte offset at most once, so consecutive hits can never repeat an address. Cap at
             * 4 (DR0-3); a 5th+ site (from this or any earlier extent) sets hwrec_overflow
             * rather than silently dropping it or lying about coverage. */
            if (out->nhwrec < 4)
                out->hwrec_off[out->nhwrec++] = insn->address;
            else
                out->hwrec_overflow = 1;
        }
        /* ...but the sweep RUNS ON: purity is decided, replayability and touches_vec are not. */
    }
    if (remaining != 0) {
        /* Desync WITHIN this extent: the bytes past this point were never classified, so no
         * optimistic verdict over them is earned. Decline the replay and force the vector
         * machinery on. `injectable` is deliberately NOT cleared here: `replayable = 0` already
         * gates the region off the replay (scan_replay_ok ANDs them), and clearing it would
         * make the reason "the impurity" rather than the truthful "decode". */
        out->replayable = 0;
        out->replay_reason = "decode";
        out->touches_vec = 1;
    }
}

/* Classify a region's static bytes over its (optional) real instruction EXTENTS — see
 * asmtest_blockstep_extent_t. `extents`/`nextents` NULL/0 sweeps [0, len) as ONE implicit
 * extent, exactly F1/F2's original whole-blob behaviour; otherwise each extent is swept
 * independently and every verdict (pure/injectable/replayable/touches_vec, and T5's hwrec site
 * list) aggregates across all of them — bytes outside every extent are never decoded, so a
 * caller-vouched island between two extents costs nothing (see region_scan_extent). Classifying
 * UP FRONT (rather than lazily) is what sidesteps the ordering trap: block-step advances the
 * REAL process, so a syscall inside a block has already retired by the boundary — it must never
 * be emulated through.
 *
 * PROVENANCE. This tier stays agnostic about where extents come from: the JIT method map (the
 * addr-channel that publishes managed method bodies, e.g. src/dataflow_method.c) is where a
 * managed integration would get them, but nothing here reads or validates that map — the
 * caller vouches the bytes it hands over via extents ARE real instruction boundaries, and a
 * caller that lies gets `region_scan_extent`'s per-extent desync fail-closed, never a silent
 * wrong answer. */
static void region_scan(const uint8_t *code, size_t len,
                        const asmtest_blockstep_extent_t *extents,
                        size_t nextents, dfb_scan_t *out) {
    memset(out, 0, sizeof *out);
    out->pure = 1;
    out->injectable =
        1; /* vacuously, until an impurity we cannot inject is seen */
    out->replayable = 1;
    csh h;
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &h) != CS_ERR_OK) {
        /* No decoder: assume the worst — and MEAN it. Leave the region `pure` (impurity is
         * unknowable without a decoder) but decline the replay, which routes to single-step:
         * correct, just unoptimized. */
        out->touches_vec = 1;
        out->replayable = 0;
        out->replay_reason = "decode";
        return;
    }
    cs_option(h, CS_OPT_DETAIL, CS_OPT_ON);
    cs_insn *insn = cs_malloc(h);
    if (extents == NULL || nextents == 0) {
        region_scan_extent(h, insn, code, 0, len, out);
    } else {
        for (size_t i = 0; i < nextents; i++)
            region_scan_extent(h, insn, code, extents[i].off, extents[i].len,
                               out);
    }
    cs_free(insn, 1);
    cs_close(&h);
    if (out->hwrec_overflow) {
        /* T6: `injectable` admits HWREC sites only up to the DR0-3 slot count region_scan
         * already caps hwrec_off at — a 5th+ site has no slot to watch, so the region keeps
         * the whole-region single-step fallback rather than silently under-cover it. Checked
         * once, after every extent's sweep (not inside region_scan_extent), so a region whose
         * first four HWREC sites are seen before the 5th disqualifies still gets the honest
         * reason. */
        out->injectable = 0;
        out->impure_reason = "hwrec-overflow";
    }
}

/* ------------------------------------------------------------------ */
/* Tracee spawn / teardown                                              */
/* ------------------------------------------------------------------ */
typedef long (*fn6_t)(long, long, long, long, long, long);

/* Map the routine's bytes into an inherited executable page (RW then R+X, so it works on a
 * W^X kernel). Returns the mapping or NULL. */
static void *map_exec(const uint8_t *code, size_t code_len) {
    void *ex = mmap(NULL, code_len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ex == MAP_FAILED)
        return NULL;
    memcpy(ex, code, code_len);
    if (mprotect(ex, code_len, PROT_READ | PROT_EXEC) != 0) {
        munmap(ex, code_len);
        return NULL;
    }
    return ex;
}

/* Fork a self-owned tracee that TRACEME's, SIGSTOPs for us, then calls the routine at `base`
 * on its natural (inherited) stack. Both the single-step and block-step captures spawn through
 * THIS one function, so the fixture's `call` return address is a single fixed code site across
 * captures — only the stack ABSOLUTE addresses differ (ASLR + this run's frame depth), which
 * info.entry_rsp lets a caller normalize away. Returns the pid stopped at the initial SIGSTOP
 * (EXITKILL applied), or -1. Only rdi..r9 are wired — the fixtures take at most six integer
 * args. */
static pid_t spawn_tracee(uint64_t base, const long *a) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0)
            _exit(127);
        raise(SIGSTOP);
        volatile long r = ((fn6_t)base)(a[0], a[1], a[2], a[3], a[4], a[5]);
        (void)r;
        _exit(0);
    }
    int status = 0;
    for (;;) {
        pid_t w = waitpid(pid, &status, 0);
        if (w >= 0 || errno != EINTR)
            break;
    }
    if (!WIFSTOPPED(status)) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return -1;
    }
    ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)(uintptr_t)PTRACE_O_EXITKILL);
    return pid;
}

static void reap(pid_t pid) {
    int status;
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
}

/* ------------------------------------------------------------------ */
/* T5 (F2 increment 2, forward pass) — hardware DR exec-breakpoint plumbing */
/* ------------------------------------------------------------------ */

/* The x86 debug registers, reached through struct user's u_debugreg[] via PTRACE_POKEUSER —
 * the same door src/ptrace_backend.c's set_hw_bp/clear_hw_bp and
 * cli/asmspy_engine.c's rgn_hw_bp_arm/rgn_hw_bp_disarm open, mirrored here as NEW LOCAL
 * plumbing rather than by widening either: ptrace_backend.c's set_hw_bp is a static,
 * single-slot API by design (its one caller only ever needs DR0), and this file needs up to 4
 * LIVE slots at once (one per scanned hwrec site) for the lifetime of one capture — a shape
 * neither existing copy has. DR0..3 hold breakpoint addresses; DR7 enables them and selects
 * the condition/length per slot; DR6 reports which slot(s) faulted. */
#define DFB_DR_OFFSET(n)                                                       \
    (offsetof(struct user, u_debugreg) + (size_t)(n) * sizeof(long))

/* Arm hardware EXECUTION breakpoint `slot` (0-3) at `addr`, per-thread: DR<slot> = addr, and
 * DR7's L<slot> bit (bit 2*slot) set — R/W<slot> = 00 (execute) and LEN<slot> = 00 (required
 * for an execute breakpoint) are left 0, i.e. untouched, exactly as
 * ptrace_backend.c's set_hw_bp / asmspy_engine.c's rgn_hw_bp_arm encode DR0. `*dr7` is the
 * caller's running DR7 image (start it at 0): threading it through by pointer, rather than a
 * PEEKUSER read-modify-write here, sidesteps PTRACE_PEEKUSER's -1-is-ambiguous return
 * convention entirely — this file always owns DR7 from a freshly forked, never-yet-armed
 * tracee, so the caller's own accumulator is already the ground truth. Returns 0 on success,
 * -1 on a ptrace failure (e.g. POKEUSER refused). */
static int dfb_arm_hw_bp(pid_t pid, int slot, uint64_t addr,
                         unsigned long *dr7) {
    if (slot < 0 || slot > 3)
        return -1;
    if (ptrace(PTRACE_POKEUSER, pid, (void *)DFB_DR_OFFSET(slot),
               (void *)(uintptr_t)addr) != 0)
        return -1;
    *dr7 |= (1UL << (2 * slot));
    if (ptrace(PTRACE_POKEUSER, pid, (void *)DFB_DR_OFFSET(7), (void *)*dr7) !=
        0)
        return -1;
    return 0;
}

/* Disarm ALL four hardware breakpoint slots (DR7 = 0), clear DR0-3, and clear DR6's sticky
 * status bits. Best-effort (mirrors clear_hw_bp / rgn_hw_bp_disarm), and called on EVERY exit
 * path before the tracee is reaped: PTRACE_DETACH does not clear the debug registers (measured
 * in the F3 work, cli/asmspy_engine.c:2443) — this tracee is always SIGKILLed rather than
 * detached, so nothing outlives it, but disarming here keeps the pattern honest regardless of
 * how the tracee's lifetime ends. */
static void dfb_clear_hw_bps(pid_t pid) {
    ptrace(PTRACE_POKEUSER, pid, (void *)DFB_DR_OFFSET(7), (void *)0UL);
    for (int i = 0; i < 4; i++)
        ptrace(PTRACE_POKEUSER, pid, (void *)DFB_DR_OFFSET(i), (void *)0UL);
    ptrace(PTRACE_POKEUSER, pid, (void *)DFB_DR_OFFSET(6), (void *)0UL);
}

/* ------------------------------------------------------------------ */
/* Path A — true single-step (the ground-truth oracle + impure fallback) */
/* ------------------------------------------------------------------ */
/* Drive PTRACE_SINGLESTEP over [rbase, rend) of an already-stopped tracee, capturing each
 * in-region step. Returns DF_BLOCKSTEP_OK on a clean region exit (*result = rax), else
 * DF_BLOCKSTEP_ETRACE / _FAULT. *stops = in-region single-step stops; *steps = captured
 * steps; *entry_rsp = rsp at the first in-region stop.
 *
 * This is also where YMM/ZMM values come from at FULL hardware width: an AVX/AVX-512 region
 * is gated off the replay, lands here, and its per-step XSTATE read captures real 256/512-bit
 * vector values off real silicon. Correct, just without the perturbation win. */
static int capture_singlestep(cap_ctx *c, pid_t pid, uint64_t rbase,
                              uint64_t rend, uint64_t max_insns, long *result,
                              uint64_t *stops, uint64_t *steps,
                              uint64_t *entry_rsp) {
    int rc = DF_BLOCKSTEP_ETRACE, entered = 0;
    uint64_t nstop = 0, guard = 0;
    dfb_snap_t S;
    for (;;) {
        if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) != 0)
            break;
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (c->have_cur)
                c->vt->truncated = true;
            rc = entered ? DF_BLOCKSTEP_FAULT : DF_BLOCKSTEP_ETRACE;
            break;
        }
        if (!WIFSTOPPED(status))
            continue;
        if (WSTOPSIG(status) != SIGTRAP)
            break;
        if (++guard > DFB_STOP_BACKSTOP) {
            c->vt->truncated = true;
            break;
        }
        memset(&S, 0, sizeof S);
        if (ptrace(PTRACE_GETREGS, pid, NULL, &S.gp) != 0)
            break;
        uint64_t pc = S.gp.rip;
        int in_region = (pc >= rbase && pc < rend);
        /* The vector snapshot is only paid for where the region needs it, and is needed at the
         * FIRST out-of-region stop too — that stop carries the last in-region step's
         * post-state. */
        if (c->want_vec && (in_region || entered))
            xstate_read(pid, &S.vec);
        if (in_region) {
            if (!entered && entry_rsp != NULL)
                *entry_rsp = S.gp.rsp;
            capture_at(c, &S); /* finalize prev (post) + open current (pre) */
            entered = 1;
            nstop++;
            if (max_insns != 0 && nstop >= max_insns) {
                if (c->have_cur) {
                    asmtest_valtrace_append(c->vt, c->cur_off, c->cur.v,
                                            c->cur.n);
                    c->have_cur = 0;
                }
                c->vt->truncated = true;
                rc = DF_BLOCKSTEP_FAULT;
                break;
            }
        } else if (entered) {
            if (c->have_cur)
                finalize_step(c, &S); /* last step's post-state */
            if (result != NULL)
                *result = (long)S.gp.rax;
            rc = DF_BLOCKSTEP_OK;
            break;
        }
    }
    if (stops != NULL)
        *stops = nstop;
    if (steps != NULL)
        *steps = c->vt->steps_len;
    return rc;
}

/* ------------------------------------------------------------------ */
/* Path B — block-step + Unicorn replay                                */
/* ------------------------------------------------------------------ */

/* Copy Unicorn's GP file into a struct user_regs_struct (for capture + the canary). */
static void uc_get_regs(uc_engine *uc, struct user_regs_struct *r) {
    memset(r, 0, sizeof *r);
    uint64_t v;
#define RD(ID, F)                                                              \
    do {                                                                       \
        v = 0;                                                                 \
        uc_reg_read(uc, ID, &v);                                               \
        r->F = v;                                                              \
    } while (0)
    RD(UC_X86_REG_RAX, rax);
    RD(UC_X86_REG_RBX, rbx);
    RD(UC_X86_REG_RCX, rcx);
    RD(UC_X86_REG_RDX, rdx);
    RD(UC_X86_REG_RSI, rsi);
    RD(UC_X86_REG_RDI, rdi);
    RD(UC_X86_REG_RBP, rbp);
    RD(UC_X86_REG_RSP, rsp);
    RD(UC_X86_REG_R8, r8);
    RD(UC_X86_REG_R9, r9);
    RD(UC_X86_REG_R10, r10);
    RD(UC_X86_REG_R11, r11);
    RD(UC_X86_REG_R12, r12);
    RD(UC_X86_REG_R13, r13);
    RD(UC_X86_REG_R14, r14);
    RD(UC_X86_REG_R15, r15);
    RD(UC_X86_REG_RIP, rip);
    RD(UC_X86_REG_EFLAGS, eflags);
    RD(UC_X86_REG_FS_BASE, fs_base);
    RD(UC_X86_REG_GS_BASE, gs_base);
#undef RD
}

/* Seed Unicorn's GP file from a real boundary snapshot. */
static void uc_set_regs(uc_engine *uc, const struct user_regs_struct *r) {
    uint64_t v;
#define WR(ID, F)                                                              \
    do {                                                                       \
        v = r->F;                                                              \
        uc_reg_write(uc, ID, &v);                                              \
    } while (0)
    WR(UC_X86_REG_RAX, rax);
    WR(UC_X86_REG_RBX, rbx);
    WR(UC_X86_REG_RCX, rcx);
    WR(UC_X86_REG_RDX, rdx);
    WR(UC_X86_REG_RSI, rsi);
    WR(UC_X86_REG_RDI, rdi);
    WR(UC_X86_REG_RBP, rbp);
    WR(UC_X86_REG_RSP, rsp);
    WR(UC_X86_REG_R8, r8);
    WR(UC_X86_REG_R9, r9);
    WR(UC_X86_REG_R10, r10);
    WR(UC_X86_REG_R11, r11);
    WR(UC_X86_REG_R12, r12);
    WR(UC_X86_REG_R13, r13);
    WR(UC_X86_REG_R14, r14);
    WR(UC_X86_REG_R15, r15);
    WR(UC_X86_REG_RIP, rip);
    WR(UC_X86_REG_EFLAGS, eflags);
    WR(UC_X86_REG_FS_BASE, fs_base);
    WR(UC_X86_REG_GS_BASE, gs_base);
#undef WR
}

/* What vector width does THIS Unicorn actually hold? Probed by writing a pattern and reading
 * it back — never by trusting a return code. Unicorn 2.0.1 accepts uc_reg_write(ZMM0) with
 * UC_ERR_OK and stores NOTHING (reads back zeros, not even aliasing XMM0/YMM0 storage); 2.1.x
 * fixed that. Returns 0 / 16 / 32 / 64. Cached: the answer is a property of the linked
 * library, not of a capture. */
static int uc_vec_width_probe(void) {
    static int cached = -1;
    if (cached >= 0)
        return cached;
    cached = 0;
    uc_engine *uc = NULL;
    if (uc_open(UC_ARCH_X86, UC_MODE_64, &uc) != UC_ERR_OK)
        return cached;
    static const struct {
        int id;
        int width;
    } probe[] = {
        {UC_X86_REG_XMM0, 16}, {UC_X86_REG_YMM0, 32}, {UC_X86_REG_ZMM0, 64}};
    for (size_t i = 0; i < sizeof probe / sizeof probe[0]; i++) {
        uint8_t in[64], back[64];
        for (int k = 0; k < probe[i].width; k++)
            in[k] = (uint8_t)(k + 1);
        memset(back, 0, sizeof back);
        if (uc_reg_write(uc, probe[i].id, in) != UC_ERR_OK)
            break;
        if (uc_reg_read(uc, probe[i].id, back) != UC_ERR_OK)
            break;
        if (memcmp(in, back, (size_t)probe[i].width) != 0)
            break; /* accepted the write and dropped it: this width is a lie */
        cached = probe[i].width;
    }
    uc_close(uc);
    return cached;
}

/* Seed Unicorn's XMM0-15 from a real boundary snapshot, VERIFYING each write by read-back.
 * Returns the number of registers proven to hold their seed.
 *
 * XMM only, deliberately: with the encoding-level VEX/EVEX gate in force, legacy SSE is the
 * only vector code that can be replayed, and it can reach nothing above bit 127. Seeding
 * YMM-upper / ZMM would be state no replayed instruction could ever read — unobservable, so
 * untestable, so vacuous. The YMM/ZMM boundary VALUES are captured from hardware instead
 * (xstate_read), which is where real 256/512-bit state genuinely lands. */
static int uc_seed_vec(uc_engine *uc, const dfb_vecstate_t *vs, int seed_mxcsr,
                       int *mxcsr_ok) {
    if (mxcsr_ok != NULL)
        *mxcsr_ok = 0;
    if (!vs->valid)
        return 0;
    int n = 0;
    for (int i = 0; i < 16 && i < vs->nregs; i++) {
        uint8_t back[16];
        if (uc_reg_write(uc, UC_X86_REG_XMM0 + i, vs->z[i]) != UC_ERR_OK)
            continue;
        if (uc_reg_read(uc, UC_X86_REG_XMM0 + i, back) != UC_ERR_OK)
            continue;
        if (memcmp(back, vs->z[i], 16) == 0)
            n++;
    }
    /* MXCSR too — the XMM file is only half the state an SSE instruction reads. Verified the
     * same way, and verified to be MEANINGFUL rather than merely accepted: Unicorn honours the
     * rounding-control bits and agrees with this silicon on 1.0/5.0 under both RN (…999a) and
     * RZ (…9999). Had it merely stored the value without honouring it, the honest move would
     * have been to gate FP regions off the replay rather than lie about them. */
    uint32_t back = 0;
    if (seed_mxcsr &&
        uc_reg_write(uc, UC_X86_REG_MXCSR, &vs->mxcsr) == UC_ERR_OK &&
        uc_reg_read(uc, UC_X86_REG_MXCSR, &back) == UC_ERR_OK &&
        back == vs->mxcsr && mxcsr_ok != NULL)
        *mxcsr_ok = 1;
    return n;
}

/* Read Unicorn's XMM file into a snapshot. width = 16 states honestly what the replay models:
 * a YMM/ZMM record asked of this snapshot declines rather than reporting a zero-extended lie. */
static void uc_get_vec(uc_engine *uc, dfb_vecstate_t *vs) {
    memset(vs, 0, sizeof *vs);
    for (int i = 0; i < 16; i++)
        uc_reg_read(uc, UC_X86_REG_XMM0 + i, vs->z[i]);
    vs->mxcsr = MXCSR_DEFAULT;
    uc_reg_read(uc, UC_X86_REG_MXCSR, &vs->mxcsr);
    vs->width = 16;
    vs->nregs = 16;
    vs->valid = 1;
}

/* The VECTOR half of the coherence canary: does the replay's computed end-of-block XMM agree
 * with the real next boundary? This is what makes an unseeded / mis-executed vector operation
 * TRUNCATE rather than lie — the GP-only canary is blind to it whenever the vector value
 * reaches memory instead of a GP register. Compares the 128 bits SSE can actually write; the
 * upper halves are untouched by any replayable instruction, so comparing them would be
 * f(x) == x. */
static int vec_coherent(const dfb_vecstate_t *uc, const dfb_vecstate_t *real) {
    if (!uc->valid || !real->valid)
        return 1; /* nothing captured to compare */
    for (int i = 0; i < 16 && i < real->nregs; i++)
        if (memcmp(uc->z[i], real->z[i], 16) != 0)
            return 0;
    /* MXCSR's STATUS bits (0-5) are sticky exception flags the replay legitimately accumulates
     * differently; the CONTROL bits — rounding (13-14), FTZ (15), DAZ (6), masks (7-12) — must
     * match, since they are inputs to every FP result the block computes. */
    if ((uc->mxcsr & ~0x3Fu) != (real->mxcsr & ~0x3Fu))
        return 0;
    return 1;
}

/* The coherence CANARY: does Unicorn's computed end-of-block state agree with the real next
 * boundary? Compares the GP regs, rip, rsp and the arithmetic flags (ignoring IF / reserved /
 * debug bits, and — T4 — every EFLAGS bit `undef_mask` names as architecturally undefined by
 * the block's replayed instructions, so a canary that agrees on program semantics only never
 * fires on silicon's arbitrary undefined-bit choice). A mismatch means the replay's inputs
 * diverged from reality (e.g. a sibling rewrote a loaded byte) and the block drops to
 * `truncated`. */
static int regs_coherent(const struct user_regs_struct *uc,
                         const struct user_regs_struct *real,
                         uint64_t undef_mask) {
    const uint64_t U[] = {uc->rax, uc->rbx, uc->rcx, uc->rdx, uc->rsi, uc->rdi,
                          uc->rbp, uc->rsp, uc->r8,  uc->r9,  uc->r10, uc->r11,
                          uc->r12, uc->r13, uc->r14, uc->r15, uc->rip};
    const uint64_t R[] = {real->rax, real->rbx, real->rcx, real->rdx, real->rsi,
                          real->rdi, real->rbp, real->rsp, real->r8,  real->r9,
                          real->r10, real->r11, real->r12, real->r13, real->r14,
                          real->r15, real->rip};
    for (size_t i = 0; i < sizeof U / sizeof U[0]; i++)
        if (U[i] != R[i])
            return 0;
    const uint64_t mask = EFLAGS_ARITH_MASK & ~undef_mask;
    return ((uint64_t)uc->eflags & mask) == ((uint64_t)real->eflags & mask);
}

/* Replay one straight-line block through Unicorn, capturing each interior instruction, until a
 * TAKEN transfer whose target is `pc_next`. Returns 0 on the clean terminator, 1 if Unicorn
 * branched somewhere OTHER than the real boundary (divergence), -1 on a Unicorn fault /
 * undecodable step. The terminating branch is left as the open step (finalized by the next
 * block's seed, or at region exit). */
static int step_block(cap_ctx *c, uc_engine *uc, uint64_t pc_next,
                      const dfb_snap_t *next, int no_inject) {
    dfb_snap_t S;
    for (size_t guard = 0; guard <= c->code_len + 4; guard++) {
        memset(&S, 0, sizeof S);
        uc_get_regs(uc, &S.gp);
        if (c->want_vec)
            uc_get_vec(uc, &S.vec);
        uint64_t pc = S.gp.rip;
        size_t len = capture_at(c, &S); /* finalize prev + open current */
        if (len == 0)
            return -1;
        /* T4: fold this instruction's undefined/defined-written EFLAGS bits into the
         * block's running mask — capture_at's open_step half just classified it into
         * cur_undef/cur_defined. A later instruction's DEFINED write clears a bit an
         * earlier one left undefined (real re-definition), never the reverse. */
        c->undef_acc = (c->undef_acc & ~c->cur_defined) | c->cur_undef;

        /* ---- F2: the replay must never EXECUTE an impure instruction ----------------
         *
         * This check is the reason the tier is not silently wrong, and "UC_ERR_OK" is exactly
         * why it is needed. MEASURED against the bundled Unicorn: `syscall` returns UC_ERR_OK
         * and simply advances rip (rax keeps the syscall NUMBER); `rdtsc` returns UC_ERR_OK
         * with a FABRICATED counter (0x132f2_1638543f); `cpuid` returns UC_ERR_OK with all
         * zeros. None of them fails. They lie — the same failure mode F1 found in VEX-128.
         *
         * It is deliberately INDEPENDENT of region_scan rather than trusting its verdict, which
         * is F1's own hard-won lesson (a gate and the witness that would catch the gate's
         * failure must not share an input). region_scan is a linear sweep that DESYNCS on a
         * constant-pool island — the shape this tier's JIT targets routinely contain — so it
         * can miss an impurity entirely. This decode is at the replay's real pc. */
        if (c->cur_imp == DFB_IMP_SYSCALL || c->cur_imp == DFB_IMP_INT80) {
            /* FAIL CLOSED on the premise itself. The whole design rests on a measured fact —
             * DEBUGCTL.BTF traps SYSCALL/INT, so the forward pass ALWAYS has a real boundary
             * immediately after the instruction retires. Rather than assume that, require it:
             * the real next boundary must be exactly the instruction after this one. If it is
             * not (BTF masked or behaving differently, a signal, a syscall that did not return
             * where expected), we have no recorded effect to inject and MUST NOT guess. */
            if (next == NULL || pc + len != pc_next || no_inject)
                return -1;
            /* INJECT. Note what this is and is not: every value written here was READ FROM THE
             * REAL TRACEE at the real boundary. Nothing is fabricated, no syscall-specific
             * table is consulted, and no argument is decoded — the kernel's own retired result
             * is simply carried across. rcx/r11 are injected rather than computed because r11
             * provably CANNOT be computed honestly: measured, it comes back as the pre-syscall
             * rflags OR'd with TF (0x100) — the ptrace single-step bit — so a "computed" r11
             * would have to model the debug mechanism perturbing the value it reports. */
            uint64_t v = next->gp.rax;
            uc_reg_write(uc, UC_X86_REG_RAX, &v);
            if (c->cur_imp == DFB_IMP_SYSCALL) {
                /* int 0x80 goes through an interrupt gate and leaves rcx/r11 ALONE (measured);
                 * writing them there would corrupt two live registers. */
                v = next->gp.rcx;
                uc_reg_write(uc, UC_X86_REG_RCX, &v);
                v = next->gp.r11;
                uc_reg_write(uc, UC_X86_REG_R11, &v);
            }
            v = pc + len;
            uc_reg_write(uc, UC_X86_REG_RIP, &v);
            c->injected++;
            return 0; /* the syscall TERMINATES the block, exactly at the real boundary */
        }
        if (c->cur_imp == DFB_IMP_HWREC) {
            /* T6: capture_blockstep takes a REAL boundary at a scanned rdtsc/rdtscp/rdrand/
             * rdseed/cpuid site (T5's DR exec breakpoint + one absorbing single-step), so
             * `next` genuinely carries the retired value here — inject it exactly as the
             * syscall branch above does for rax/rcx/r11, then terminate the block at the
             * boundary so no replayed instruction ever runs on stale post-site state (the
             * next block reseeds from S_next). FAIL CLOSED on the premise, same as syscall:
             * if the real next boundary is not exactly pc+len (BTF/DR behaving differently, a
             * signal, no_hw_record having skipped arming so `next` describes a LATER
             * boundary), there is no recorded effect to inject. */
            if (next == NULL || pc + len != pc_next)
                return -1;
            /* UNLIKE syscall (a producer-local write set Capstone never reports), Capstone's
             * write set for all five HWREC mnemonics is already complete (measured — see
             * open_step's T5 comment): c->cur is THIS step's write records, opened by the
             * capture_at call above. Every value injected here was READ FROM THE REAL TRACEE
             * at the real boundary — nothing fabricated, no per-mnemonic table. */
            for (size_t i = 0; i < c->cur.n; i++) {
                at_val_rec_t *r = &c->cur.v[i];
                int ucreg;
                uint64_t v;
                if (!r->is_write || r->kind != AT_LOC_REG ||
                    !uc_gp_container(r->reg, &ucreg) ||
                    !gp_value(&next->gp, r->reg, &v))
                    continue;
                uc_reg_write(uc, ucreg, &v);
            }
            uint64_t v = pc + len;
            uc_reg_write(uc, UC_X86_REG_RIP, &v);
            c->injected++;
            return 0; /* the site TERMINATES the block, exactly at the real boundary */
        }
        if (c->cur_imp == DFB_IMP_OTHER)
            return -1; /* sysenter / undecodable: no boundary exists to record from — truncate */

        if (uc_emu_start(uc, pc, (uint64_t)-1, 0, 1) != UC_ERR_OK)
            return -1;
        uint64_t next = 0;
        uc_reg_read(uc, UC_X86_REG_RIP, &next);
        if (next != pc + len) { /* a taken control transfer */
            if (next == pc_next)
                return 0; /* the block terminator: reached the real boundary */
            return 1;     /* diverged to a different target than reality */
        }
    }
    return -1; /* no terminator within the region bound */
}

/* Block-step the real tracee and replay each block through Unicorn. Returns DF_BLOCKSTEP_OK on
 * a clean region exit (*result = rax), DF_BLOCKSTEP_FAULT when the coherence canary fires
 * (vt->truncated set), or DF_BLOCKSTEP_ETRACE on setup/ptrace failure. *stops = in-region
 * block boundaries; *steps = captured steps; *entry_rsp = rsp at the entry boundary.
 * inject_block >= 0 corrupts Unicorn's seed rax at that 0-based interior block to SIMULATE a
 * concurrent-divergence input, exercising the canary. */
static int capture_blockstep(cap_ctx *c, pid_t pid, uint64_t base, size_t len,
                             uint64_t rbase, uint64_t rend,
                             const asmtest_blockstep_opts_t *o, long *result,
                             uint64_t *stops, uint64_t *steps,
                             uint64_t *entry_rsp, int inject_block,
                             int *vec_seeded, int *mxcsr_seeded,
                             const uint64_t *hwrec_off, size_t nhwrec) {
    const int no_vec_seed = o->no_vec_seed;
    const int seed_mxcsr = !o->no_mxcsr_seed;
    const int no_vec_canary = o->no_vec_canary;
    int ret = DF_BLOCKSTEP_ETRACE;
    uc_engine *uc = NULL;
    uint8_t *stackbuf = NULL;
    uint64_t win_base = 0, win_size = 0;
    dfb_snap_t S_cur;
    int at_entry = 0;
    uint64_t nstop = 1; /* the entry boundary itself */
    int block_idx = 0;
    long real_result = 0;
    unsigned long dr7 =
        0; /* T5: the DR7 image dfb_arm_hw_bp builds up, slot by slot */

    memset(&S_cur, 0, sizeof S_cur);

    /* 1) Block-step through the entry glue until the first IN-REGION stop = boundary 0. With
     *    opts.region_off the glue includes the blob's own prologue, so this is also what
     *    establishes live-in vector state on the REAL cpu before the region is ever traced. */
    for (uint64_t g = 0; g < DFB_STOP_BACKSTOP; g++) {
        if (ptrace(PTRACE_SINGLEBLOCK, pid, NULL, NULL) != 0)
            goto out;
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR)
                continue;
            goto out;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status))
            goto out;
        if (!WIFSTOPPED(status))
            continue;
        if (WSTOPSIG(status) != SIGTRAP)
            goto out;
        if (ptrace(PTRACE_GETREGS, pid, NULL, &S_cur.gp) != 0)
            goto out;
        if (S_cur.gp.rip >= rbase && S_cur.gp.rip < rend) {
            at_entry = 1;
            break;
        }
    }
    if (!at_entry)
        goto out;
    if (c->want_vec)
        xstate_read(pid, &S_cur.vec); /* the region's LIVE-IN vector state */
    if (entry_rsp != NULL)
        *entry_rsp = S_cur.gp.rsp;

    /* T5 (F2 increment 2, forward pass): arm one hardware exec breakpoint per scanned
     * rdtsc/rdtscp/rdrand/rdseed/cpuid site, absolute address rbase + hwrec_off[i] — so the
     * forward pass gets a REAL boundary at each even though BTF alone never traps them (see
     * dfb_impurity_kind). region_scan already capped nhwrec at 4, the DR0-3 slot count, so no
     * further bound is needed here. Best-effort per slot: a POKEUSER failure just means that
     * one site's boundary is missed, and the per-step decode in step_block truncates when the
     * replay reaches it — same fail-closed outcome as an un-injected site has always had.
     * `no_hw_record` (T6 test hook) skips this loop entirely: no slot is armed, so the forward
     * pass never gets a boundary at the site and step_block's `pc + len != pc_next` check
     * truncates when the replay reaches it — the pre-T6 behaviour, on demand. */
    if (!o->no_hw_record)
        for (size_t i = 0; i < nhwrec && i < 4; i++)
            dfb_arm_hw_bp(pid, (int)i, rbase + hwrec_off[i], &dr7);

    /* 2) Stand up Unicorn: code mapped at the REAL base, a stack window at the REAL rsp, both
     *    copied from the (stopped) tracee, and the GP file seeded from the entry boundary. Real
     *    addresses => effective addresses + values compare directly against the oracle. */
    if (uc_open(UC_ARCH_X86, UC_MODE_64, &uc) != UC_ERR_OK)
        goto out;
    {
        uint64_t code_base = base & ~0xFFFULL;
        uint64_t code_size = ((base + len + 0xFFFULL) & ~0xFFFULL) - code_base;
        if (uc_mem_map(uc, code_base, code_size, UC_PROT_ALL) != UC_ERR_OK)
            goto out;
        uint8_t *cb = (uint8_t *)malloc(code_size);
        if (cb == NULL)
            goto out;
        if (!mr_tracee(&pid, code_base, cb, code_size)) {
            memset(cb, 0, code_size);
            memcpy(cb + (base - code_base), c->code, len);
        }
        uc_mem_write(uc, code_base, cb, code_size);
        free(cb);

        win_base = (S_cur.gp.rsp - 0x1000) & ~0xFFFULL;
        /* [rsp-0x1000, rsp+0x2000): rsp-8 and the ret slot. stack_hi_pad is a test hook that
         * pushes the top of the window deliberately past the tracee's [stack] VMA, making the
         * partially-unmapped-window case (see mr_tracee_window) reproducible on demand
         * instead of only ~27% of the time under one particular stack layout. */
        win_size = 0x3000 + ((o->stack_hi_pad + 0xFFFULL) & ~0xFFFULL);
        if (uc_mem_map(uc, win_base, win_size, UC_PROT_READ | UC_PROT_WRITE) !=
            UC_ERR_OK)
            goto out;
        stackbuf = (uint8_t *)malloc(win_size);
        if (stackbuf == NULL)
            goto out;
    }
    c->mr_ctx = uc;

    /* Snapshot the tracee's stack window as of the entry boundary and seed Unicorn. */
    mr_tracee_window(pid, win_base, stackbuf, win_size);
    uc_mem_write(uc, win_base, stackbuf, win_size);
    uc_set_regs(uc, &S_cur.gp);
    if (c->want_vec && !no_vec_seed && vec_seeded != NULL)
        *vec_seeded = uc_seed_vec(uc, &S_cur.vec, seed_mxcsr, mxcsr_seeded);

    for (;;) {
        /* Advance the REAL tracee one block; this is the perturbing stop we count. */
        if (ptrace(PTRACE_SINGLEBLOCK, pid, NULL, NULL) != 0)
            goto out;
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR)
                continue;
            goto out;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            c->vt->truncated = true; /* ended before a clean region return */
            ret = DF_BLOCKSTEP_FAULT;
            goto out;
        }
        if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP)
            goto out;
        dfb_snap_t S_next;
        memset(&S_next, 0, sizeof S_next);
        if (ptrace(PTRACE_GETREGS, pid, NULL, &S_next.gp) != 0)
            goto out;

        /* T5: a DR exec breakpoint on a scanned hwrec site faults BEFORE the instruction
         * retires (bits 0-3 of DR6 name the slot) — pre-empting the ordinary BTF boundary this
         * SINGLEBLOCK was chasing, so S_next currently sits AT the site, not past it. Absorb it
         * with one PTRACE_SINGLESTEP so S_next becomes the real POST-RETIREMENT boundary (the
         * instruction's actual outputs), then fall through to the SAME per-block flow an
         * ordinary boundary takes below. A DR6 with only BS (bit 14) set, or 0, is the
         * unmodified BTF path — this block is then a no-op. */
        if (nhwrec > 0) {
            unsigned long dr6 = (unsigned long)ptrace(
                PTRACE_PEEKUSER, pid, (void *)DFB_DR_OFFSET(6), NULL);
            if ((dr6 & 0xFUL) != 0) {
                ptrace(PTRACE_POKEUSER, pid, (void *)DFB_DR_OFFSET(6),
                       (void *)0UL); /* DR6 is sticky: clear before resuming */
                if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) != 0)
                    goto out;
                int st2 = 0;
                for (;;) {
                    pid_t w2 = waitpid(pid, &st2, 0);
                    if (w2 >= 0 || errno != EINTR)
                        break;
                }
                if (WIFEXITED(st2) || WIFSIGNALED(st2)) {
                    c->vt->truncated = true;
                    ret = DF_BLOCKSTEP_FAULT;
                    goto out;
                }
                if (!WIFSTOPPED(st2) || WSTOPSIG(st2) != SIGTRAP)
                    goto out;
                if (ptrace(PTRACE_GETREGS, pid, NULL, &S_next.gp) != 0)
                    goto out;
                c->hw_hits++;
            }
        }

        int in_region_next = (S_next.gp.rip >= rbase && S_next.gp.rip < rend);
        if (c->want_vec)
            xstate_read(pid, &S_next.vec); /* ground truth for the canary */

        /* The region-EXIT terminator (a ret / tail jump) transfers to a real address OUTSIDE
         * the mapped region — Unicorn would UC_ERR_FETCH_UNMAPPED trying to fetch there even
         * under count=1. Map a one-page landing pad at that boundary so the terminator's data
         * effects (pop rsp, [rsp] read) execute and Unicorn halts cleanly AT the boundary.
         * Best-effort: an already-mapped page is fine. */
        if (!in_region_next) {
            uint64_t pad = S_next.gp.rip & ~0xFFFULL;
            uc_mem_map(uc, pad, 0x1000, UC_PROT_READ | UC_PROT_EXEC);
        }

        /* Seed this block from the real starting boundary + its memory snapshot, then replay
         * it. (The seed also finalizes the previous block's branch with real ground truth on
         * the next capture_at.) */
        uc_set_regs(uc, &S_cur.gp);
        uc_mem_write(uc, win_base, stackbuf, win_size);
        /* Re-seed the vector file at EVERY boundary, not just the entry: the same
         * ground-truth-endpoints discipline the GP file gets. Without it a replay error in
         * one block would silently propagate into the next. */
        if (c->want_vec && !no_vec_seed) {
            int n = uc_seed_vec(uc, &S_cur.vec, seed_mxcsr, mxcsr_seeded);
            if (vec_seeded != NULL && n > *vec_seeded)
                *vec_seeded = n;
        }
        if (inject_block >= 0 && block_idx == inject_block) {
            uint64_t bad =
                S_cur.gp.rax + 1; /* simulate a diverging replay input */
            uc_reg_write(uc, UC_X86_REG_RAX, &bad);
        }

        c->undef_acc =
            0; /* T4: a fresh block — no instruction has run yet to undefine a bit */
        int brc =
            step_block(c, uc, S_next.gp.rip, &S_next, o->no_syscall_inject);
        if (brc < 0) {
            /* A Unicorn fault / undecodable step — e.g. UC_ERR_INSN_INVALID on a VEX/EVEX
             * instruction that slipped past the replayability gate, or (F2) an impure
             * instruction the replay may not execute and has no boundary to inject from. This
             * is a DIVERGENCE, not a missing substrate: it must truncate, never masquerade as
             * the ETRACE self-skip that ret was initialized to. */
            c->vt->truncated = true;
            ret = DF_BLOCKSTEP_FAULT;
            goto out;
        }

        dfb_snap_t ucR;
        memset(&ucR, 0, sizeof ucR);
        uc_get_regs(uc, &ucR.gp);
        if (c->want_vec)
            uc_get_vec(uc, &ucR.vec);
        if (o->inject_flag_bit !=
            0) /* T4 test hook: force a chosen bit to disagree */
            ucR.gp.eflags ^= o->inject_flag_bit;
        if (brc == 1 || !regs_coherent(&ucR.gp, &S_next.gp, c->undef_acc) ||
            (c->want_vec && !no_vec_canary &&
             !vec_coherent(&ucR.vec, &S_next.vec))) {
            c->vt->truncated =
                true; /* divergence detected: never silently wrong */
            ret = DF_BLOCKSTEP_FAULT;
            goto out;
        }

        if (!in_region_next) {
            /* Region return: finalize the terminating step with the real boundary's vector
             * state (the replay models only XMM, so the real snapshot is the better witness)
             * over Unicorn's GP post-state. */
            if (c->have_cur) {
                dfb_snap_t fin = ucR;
                if (c->want_vec && S_next.vec.valid)
                    fin.vec = S_next.vec;
                finalize_step(c, &fin);
            }
            real_result = (long)S_next.gp.rax;
            ret = DF_BLOCKSTEP_OK;
            break;
        }

        /* Advance to the next block: resnapshot the tracee's stack (stopped at S_next now) so
         * the next block's loads see ground-truth memory. */
        nstop++;
        block_idx++;
        mr_tracee_window(pid, win_base, stackbuf, win_size);
        S_cur = S_next;
    }

    if (result != NULL)
        *result = real_result;

out:
    /* T5: disarm every DR slot on EVERY exit path before the tracee is reaped — every `goto
     * out` above, and the clean region-return fallthrough, land here. Harmless when nothing
     * was ever armed (nhwrec == 0): POKEUSER on an already-0 register is a no-op. */
    if (nhwrec > 0)
        dfb_clear_hw_bps(pid);
    if (stops != NULL)
        *stops = nstop;
    if (steps != NULL)
        *steps = c->vt->steps_len;
    free(stackbuf);
    if (uc != NULL)
        uc_close(uc);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Functional self-skip probes (hang-proof)                            */
/* ------------------------------------------------------------------ */
static int wait_stop_sigtrap(pid_t pid) {
    int st;
    for (int i = 0; i < 200; i++) {
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid)
            return WIFSTOPPED(st) && WSTOPSIG(st) == SIGTRAP;
        if (w < 0)
            return 0;
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
    return 0;
}
static int probe_ptrace(void) {
    pid_t pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0)
            _exit(1);
        raise(SIGSTOP);
        _exit(0);
    }
    int status = 0, ok = 0;
    if (waitpid(pid, &status, 0) >= 0 && WIFSTOPPED(status))
        ok = 1;
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return ok;
}
static int probe_singleblock(void) {
    static const uint8_t blob[] = {0xCC, 0x90, 0x90, 0x90, 0x90, 0x90, 0xC3};
    void *p = mmap(NULL, sizeof blob, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return 0;
    memcpy(p, blob, sizeof blob);
    pid_t pid = fork();
    if (pid < 0) {
        munmap(p, sizeof blob);
        return 0;
    }
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        ((void (*)(void))p)();
        _exit(0);
    }
    int functional = 0;
    struct user_regs_struct regs;
    if (wait_stop_sigtrap(pid) &&
        ptrace(PTRACE_GETREGS, pid, NULL, &regs) == 0 &&
        regs.rip == (uint64_t)(uintptr_t)p + 1 &&
        ptrace(PTRACE_SINGLEBLOCK, pid, NULL, NULL) == 0 &&
        wait_stop_sigtrap(pid) && ptrace(PTRACE_GETREGS, pid, NULL, &regs) == 0)
        functional = regs.rip < (uint64_t)(uintptr_t)p ||
                     regs.rip >= (uint64_t)(uintptr_t)p + sizeof blob;
    int st;
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    munmap(p, sizeof blob);
    return functional;
}

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

int asmtest_dataflow_blockstep_probe(void) {
    if (!probe_ptrace())
        return 0;
    if (!probe_singleblock())
        return 0;
    return 1;
}

/* T7: this and the sibling classifiers below (is_replayable/is_injectable/scan_hwrec) always
 * sweep the WHOLE blob as one implicit extent — extents are a run() capability only, since only
 * run() carries the region_off a caller-supplied extent list is anchored to. A caller wanting a
 * classifier answer over its real instruction extents passes them to run() and reads
 * info.reason from there. */
int asmtest_dataflow_blockstep_is_pure(const uint8_t *code, size_t code_len,
                                       const char **reason) {
    if (reason != NULL)
        *reason = NULL;
    if (code == NULL || code_len == 0)
        return DF_BLOCKSTEP_EINVAL;
    dfb_scan_t s;
    region_scan(code, code_len, NULL, 0, &s);
    if (s.pure)
        return 1;
    if (reason != NULL)
        *reason = s.impure_reason;
    return 0;
}

/* Can the emulator faithfully REPLAY this region? Distinct from purity: an impure region has
 * already retired its side effects on the real cpu by the boundary, whereas a non-replayable
 * one is code Unicorn cannot execute correctly (any VEX/EVEX encoding — see the file header;
 * VEX-128 in particular does not fail, it returns a wrong answer with UC_ERR_OK). Both route
 * to the single-step fallback. Returns 1 replayable, 0 not (*reason = "vex/evex" | "decode").
 *
 * This verdict is INDEPENDENT of purity, and must be: it used to be computed by a sweep that
 * broke at the first impure instruction, so `cpuid; vpaddq xmm0,xmm1,xmm2; ret` came back
 * REPLAYABLE with reason=NULL — this function telling a caller "Unicorn can faithfully replay
 * this" about a region containing the very instruction the file header calls a silent liar.
 * run() happened to mask it (impurity routed the region to single-step anyway), but the answer
 * was wrong and the entry point is public. */
int asmtest_dataflow_blockstep_is_replayable(const uint8_t *code,
                                             size_t code_len,
                                             const char **reason) {
    if (reason != NULL)
        *reason = NULL;
    if (code == NULL || code_len == 0)
        return DF_BLOCKSTEP_EINVAL;
    dfb_scan_t s;
    region_scan(code, code_len, NULL, 0, &s);
    if (s.replayable)
        return 1;
    if (reason != NULL)
        *reason = s.replay_reason;
    return 0;
}

/* F2: can the replay carry this region's OS interaction by RECORD-AND-INJECT? 1 = yes (which
 * includes a PURE region, vacuously); 0 = no, and *reason names the impurity that disqualifies
 * it. Deliberately a THIRD verdict rather than a redefinition of is_pure(): "pure" is a property
 * of the code and ten other things ask it, whereas this is a property of what THIS tier can
 * carry. A caller wanting F1's question still gets F1's answer.
 *
 * The honest boundary, MEASURED rather than assumed (see dfb_impurity_kind): `syscall` and
 * `int 0x80` are trapped by DEBUGCTL.BTF because they are control transfers, so the forward pass
 * gets a real post-retirement boundary for free. `rdtsc` / `rdtscp` / `rdrand` / `rdseed` /
 * `cpuid` are NOT trapped — a block runs straight through them — so there is no boundary at
 * which their retired value could be recorded, and no amount of decoding creates one. */
int asmtest_dataflow_blockstep_is_injectable(const uint8_t *code,
                                             size_t code_len,
                                             const char **reason) {
    if (reason != NULL)
        *reason = NULL;
    if (code == NULL || code_len == 0)
        return DF_BLOCKSTEP_EINVAL;
    dfb_scan_t s;
    region_scan(code, code_len, NULL, 0, &s);
    if (s.injectable)
        return 1;
    if (reason != NULL)
        *reason = s.impure_reason;
    return 0;
}

/* T5 (F2 increment 2, forward pass): how many DISTINCT rdtsc/rdtscp/rdrand/rdseed/cpuid sites
 * does a static sweep of this region find, and did it exceed the 4-slot architectural DR cap?
 * Exposes region_scan's hwrec bookkeeping for testing without a live capture, exactly as
 * is_pure / is_replayable / is_injectable already expose its other three verdicts. Returns the
 * count found (0-4, since the scan itself caps `nhwrec` there), or DF_BLOCKSTEP_EINVAL on bad
 * args. `off` (may be NULL) receives up to `off_cap` of the found offsets; `overflow` (may be
 * NULL) is set 1 iff a 5th+ site exists (the region then keeps the whole-region single-step
 * fallback rather than watch only 4 of 5+). */
int asmtest_dataflow_blockstep_scan_hwrec(const uint8_t *code, size_t code_len,
                                          uint64_t *off, size_t off_cap,
                                          int *overflow) {
    if (overflow != NULL)
        *overflow = 0;
    if (code == NULL || code_len == 0)
        return DF_BLOCKSTEP_EINVAL;
    dfb_scan_t s;
    region_scan(code, code_len, NULL, 0, &s);
    if (overflow != NULL)
        *overflow = s.hwrec_overflow;
    for (size_t i = 0; off != NULL && i < s.nhwrec && i < off_cap; i++)
        off[i] = s.hwrec_off[i];
    return (int)s.nhwrec;
}

/* The widest vector register width the HARDWARE + OS expose through XSTATE (0/16/32/64), and
 * — when nregs is non-NULL — how many vector registers exist (16, or 32 with AVX-512). Lets a
 * suite hardware-gate its YMM/ZMM cases rather than fail on a box without the silicon. */
int asmtest_dataflow_blockstep_vec_width(int *nregs) {
    const dfb_xlayout_t *L = dfb_xlayout();
    if (nregs != NULL)
        *nregs = L->ok ? L->nregs : 0;
    return L->ok ? L->width : 0;
}

/* The widest vector width THIS Unicorn build actually round-trips, proven by read-back
 * (0/16/32/64). Not the same question as the hardware's width: 2.0.1 accepts a ZMM write with
 * UC_ERR_OK and stores nothing. */
int asmtest_dataflow_blockstep_uc_vec_width(void) {
    return uc_vec_width_probe();
}

int asmtest_dataflow_blockstep_run(const uint8_t *code, size_t code_len,
                                   const long *args, int nargs,
                                   const asmtest_blockstep_opts_t *opts,
                                   long *result, asmtest_valtrace_t *vt,
                                   asmtest_blockstep_info_t *info) {
    if (vt == NULL || code == NULL || code_len == 0 || nargs < 0 || nargs > 6 ||
        (nargs > 0 && args == NULL))
        return DF_BLOCKSTEP_EINVAL;

    asmtest_blockstep_opts_t o;
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    if (opts != NULL)
        o = *opts;
    if (o.region_off >= code_len)
        return DF_BLOCKSTEP_EINVAL; /* an empty region is a caller bug */
    /* T7: extents, when given, are blob-absolute (the same coordinate space as region_off) and
     * must be sorted, non-overlapping, and fully inside [region_off, code_len) — the caller
     * vouches these ARE real instruction boundaries. An empty entry (len==0) is a caller bug,
     * not a "skip" convention. `prev_end` starting at region_off is what also enforces the
     * first extent not starting before the region itself. */
    if (o.nextents > 0) {
        if (o.extents == NULL)
            return DF_BLOCKSTEP_EINVAL;
        uint64_t prev_end = o.region_off;
        for (size_t i = 0; i < o.nextents; i++) {
            uint64_t eoff = o.extents[i].off, elen = o.extents[i].len;
            if (elen == 0 || eoff < prev_end ||
                eoff + elen > (uint64_t)code_len)
                return DF_BLOCKSTEP_EINVAL;
            prev_end = eoff + elen;
        }
    }
    if (info != NULL)
        memset(info, 0, sizeof *info);
    vt->mem_space = AT_LOC_MEM_ABS;

    long a[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < nargs; i++)
        a[i] = args[i];

    /* The two gates, over the REGION's bytes only — [0, region_off) is entry glue the tracee
     * executes but the capture never traces or replays, so its instructions do not disqualify
     * anything. PURE: no OS-interacting / nondeterministic instruction (block-step advances the
     * real process, so a syscall in a block has already retired by the boundary). REPLAYABLE:
     * no VEX/EVEX (no released Unicorn executes AVX, and VEX-128 mis-executes SILENTLY).
     * Either gate routes to the single-step fallback, which is correct — just unoptimized.
     * force_singlestep still runs the scan so info.reason stays informative.
     *
     * T7: opts.extents, when given, is region-scoped over the SAME bytes region_scan always
     * swept — just as a set of real instruction extents instead of one linear run. Converted to
     * region-relative (subtracting region_off) here, in a scratch array whose lifetime is this
     * one call: region_scan itself stays extent-relative to whatever buffer it is handed,
     * exactly as its addr/hwrec_off bookkeeping already was pre-T7. */
    dfb_scan_t scan;
    if (o.nextents > 0) {
        asmtest_blockstep_extent_t *rel =
            (asmtest_blockstep_extent_t *)malloc(o.nextents * sizeof *rel);
        if (rel == NULL)
            return DF_BLOCKSTEP_ETRACE;
        for (size_t i = 0; i < o.nextents; i++) {
            rel[i].off = o.extents[i].off - o.region_off;
            rel[i].len = o.extents[i].len;
        }
        region_scan(code + o.region_off, code_len - (size_t)o.region_off, rel,
                    o.nextents, &scan);
        free(rel);
    } else {
        region_scan(code + o.region_off, code_len - (size_t)o.region_off, NULL,
                    0, &scan);
    }
    int gated_off = !scan_replay_ok(&scan) && !o.force_replay;
    int use_replay = !gated_off && !o.force_singlestep;

    void *ex = map_exec(code, code_len);
    if (ex == NULL)
        return DF_BLOCKSTEP_ETRACE;
    uint64_t base = (uint64_t)(uintptr_t)ex;
    uint64_t rbase = base + o.region_off;
    uint64_t rend = base + code_len;
    pid_t pid = spawn_tracee(base, a);
    if (pid < 0) {
        munmap(ex, code_len);
        return DF_BLOCKSTEP_ETRACE;
    }

    cap_ctx c;
    memset(&c, 0, sizeof c);
    c.vt = vt;
    c.code = code;
    c.code_len = code_len;
    c.base = base;
    c.want_vec = scan.touches_vec && dfb_xlayout()->ok;
    c.no_undef_mask = o.no_undef_mask;
    /* The per-step decoder (F2). Independent of region_scan's handle by design, and shared by
     * BOTH value sources so the syscall write set is identical on the oracle and the replay.
     * cs_ok = 0 makes dfb_classify_at answer DFB_IMP_OTHER for every step, which fails closed:
     * the replay refuses to execute anything, so a decoder-less build truncates rather than
     * emulating instructions it cannot vouch for. */
    c.cs_ok = (cs_open(CS_ARCH_X86, CS_MODE_64, &c.cs) == CS_ERR_OK);
    if (c.cs_ok)
        cs_option(c.cs, CS_OPT_DETAIL, CS_OPT_ON);

    uint64_t stops = 0, steps = 0, entry_rsp = 0;
    int vec_seeded = 0, mxcsr_seeded = 0;
    int rc;
    if (use_replay) {
        c.mr = mr_uc; /* mr_ctx set once the engine stands up */
        int inj = o.inject_divergence ? o.inject_block : -1;
        rc = capture_blockstep(&c, pid, base, code_len, rbase, rend, &o, result,
                               &stops, &steps, &entry_rsp, inj, &vec_seeded,
                               &mxcsr_seeded, scan.hwrec_off, scan.nhwrec);
    } else {
        c.mr = mr_tracee;
        c.mr_ctx = &pid;
        rc = capture_singlestep(&c, pid, rbase, rend, o.max_insns, result,
                                &stops, &steps, &entry_rsp);
    }

    if (info != NULL) {
        info->pure = use_replay;
        info->reason = NULL;
        if (gated_off && !o.force_singlestep)
            info->reason = scan_reason(&scan);
        info->stops = stops;
        info->steps = steps;
        info->entry_rsp = entry_rsp;
        info->vec_width =
            asmtest_dataflow_blockstep_vec_width(&info->vec_nregs);
        info->uc_vec_width = uc_vec_width_probe();
        info->vec_seeded = vec_seeded;
        info->mxcsr_seeded = mxcsr_seeded;
        info->injectable = scan.injectable;
        info->injected = c.injected;
        info->hw_hits =
            c.hw_hits; /* 0 whenever capture_blockstep never ran (force_singlestep):
                                    * nothing arms a DR slot on that path (see cap_ctx.hw_hits) */
    }

    free(c.cur.v);
    if (c.cs_ok)
        cs_close(&c.cs);
    reap(pid);
    munmap(ex, code_len);
    return rc;
}

#else /* not (Linux x86-64 + Capstone + Unicorn): ENOSYS stubs */

int asmtest_dataflow_blockstep_probe(void) { return DF_BLOCKSTEP_ENOSYS; }

int asmtest_dataflow_blockstep_is_pure(const uint8_t *code, size_t code_len,
                                       const char **reason) {
    (void)code;
    (void)code_len;
    if (reason != NULL)
        *reason = NULL;
    return DF_BLOCKSTEP_ENOSYS;
}

int asmtest_dataflow_blockstep_is_replayable(const uint8_t *code,
                                             size_t code_len,
                                             const char **reason) {
    (void)code;
    (void)code_len;
    if (reason != NULL)
        *reason = NULL;
    return DF_BLOCKSTEP_ENOSYS;
}

int asmtest_dataflow_blockstep_is_injectable(const uint8_t *code,
                                             size_t code_len,
                                             const char **reason) {
    (void)code;
    (void)code_len;
    if (reason != NULL)
        *reason = NULL;
    return DF_BLOCKSTEP_ENOSYS;
}

int asmtest_dataflow_blockstep_scan_hwrec(const uint8_t *code, size_t code_len,
                                          uint64_t *off, size_t off_cap,
                                          int *overflow) {
    (void)code;
    (void)code_len;
    (void)off;
    (void)off_cap;
    if (overflow != NULL)
        *overflow = 0;
    return DF_BLOCKSTEP_ENOSYS;
}

int asmtest_dataflow_blockstep_vec_width(int *nregs) {
    if (nregs != NULL)
        *nregs = 0;
    return 0;
}

int asmtest_dataflow_blockstep_uc_vec_width(void) { return 0; }

int asmtest_dataflow_blockstep_run(const uint8_t *code, size_t code_len,
                                   const long *args, int nargs,
                                   const asmtest_blockstep_opts_t *opts,
                                   long *result, asmtest_valtrace_t *vt,
                                   asmtest_blockstep_info_t *info) {
    (void)code;
    (void)code_len;
    (void)args;
    (void)nargs;
    (void)opts;
    (void)result;
    (void)vt;
    if (info != NULL)
        memset(info, 0, sizeof *info);
    return DF_BLOCKSTEP_ENOSYS;
}

#endif
