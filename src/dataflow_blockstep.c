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
 * (the same shared capture core, driven one instruction at a time). F2 lifts the fallback
 * via record-and-inject.
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
} asmtest_blockstep_opts_t;

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
} asmtest_blockstep_info_t;

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
    if (rb->n == rb->cap) {
        size_t nc = rb->cap ? rb->cap * 2 : 16;
        at_val_rec_t *nv = (at_val_rec_t *)realloc(rb->v, nc * sizeof *nv);
        if (nv == NULL)
            return;
        rb->v = nv;
        rb->cap = nc;
    }
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
} cap_ctx;

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
        *out = r->r8;
        return 1;
    case X86_REG_R9:
        *out = r->r9;
        return 1;
    case X86_REG_R10:
        *out = r->r10;
        return 1;
    case X86_REG_R11:
        *out = r->r11;
        return 1;
    case X86_REG_R12:
        *out = r->r12;
        return 1;
    case X86_REG_R13:
        *out = r->r13;
        return 1;
    case X86_REG_R14:
        *out = r->r14;
        return 1;
    case X86_REG_R15:
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

    at_val_rec_t rd[64], wr[64];
    size_t nr = 64, nw = 64;
    size_t insn_len = asmtest_operands(ASMTEST_ARCH_X86_64, c->code,
                                       c->code_len, off, rd, &nr, wr, &nw);
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
    int replayable; /* no VEX/EVEX-encoded instruction (see insn_is_vex_evex)                */
    int touches_vec; /* references vector state, so the capture must snapshot + seed it      */
    /* The two verdicts have INDEPENDENT reasons, because a region can fail both gates and
     * `pure` must not mask a replayability verdict a public caller asked for. */
    const char *
        impure_reason; /* the offending mnemonic, when !pure                   */
    const char *
        replay_reason; /* "vex/evex" or "decode", when !replayable             */
} dfb_scan_t;

/* Why the replay path was declined, if it was. Impurity is the stronger verdict, so it names
 * the region when both gates fire. */
static const char *scan_reason(const dfb_scan_t *s) {
    if (!s->pure)
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

/* Linearly disassemble the region's bytes ONCE and decide all three questions. Classifying per
 * region UP FRONT is what sidesteps the ordering trap: block-step advances the REAL process, so
 * a syscall inside a block has already retired by the boundary — it must never be emulated
 * through.
 *
 * THIS SCAN FAILS CLOSED, DELIBERATELY, AND THAT IS THE WHOLE POINT. It is a single point of
 * failure feeding BOTH the replayability gate AND (via touches_vec) the vector seed and the
 * vector canary — so a wrong verdict does not merely lose a check, it lets an instruction
 * through AND removes the check that would have caught it. Three ways it used to fail OPEN,
 * each now closed:
 *
 *   1. DECODER DESYNC. `remaining != 0` after the loop means cs_disasm_iter stopped early —
 *      an embedded constant-pool island (routine in the JIT method-maps this tier targets) can
 *      make a `movabs` swallow a following VEX prefix as immediate data, so the sweep ends with
 *      the OPTIMISTIC initial verdicts still in place and a VEX-128 reaches the replay ungated
 *      AND unwitnessed. Measured: `jmp +2 / movabs-island / vpaddq xmm0,xmm1,xmm2 / movups` →
 *      replayable=1 touches_vec=0 with remaining=5 of 17.
 *   2. THE IMPURITY EARLY BREAK. Aborting the sweep at the first impure instruction left every
 *      vector instruction AFTER it unseen, so touches_vec=0 → no XSTATE read on the single-step
 *      fallback → every vector record emitted value_valid=0 at rc=OK. Only the PURITY answer is
 *      settled early; the sweep must still classify the whole region.
 *   3. cs_open FAILING. Now sets replayable=0 rather than relying on a downstream accident.
 *
 * A linear sweep is still only exact for a straight instruction stream; failing closed is what
 * makes that honest. A production classifier would follow the JIT method-map's real instruction
 * extents, at which point the desync verdict becomes rare rather than load-bearing. */
static void region_scan(const uint8_t *code, size_t len, dfb_scan_t *out) {
    memset(out, 0, sizeof *out);
    out->pure = 1;
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
    uint64_t addr = 0;
    const uint8_t *p = code;
    size_t remaining = len;
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
        /* ...but the sweep RUNS ON: purity is decided, replayability and touches_vec are not. */
    }
    cs_free(insn, 1);
    cs_close(&h);
    if (remaining != 0) {
        /* Desync: the bytes past this point were never classified, so no optimistic verdict
         * over them is earned. Decline the replay and force the vector machinery on. */
        out->replayable = 0;
        out->replay_reason = "decode";
        out->touches_vec = 1;
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
 * debug bits). A mismatch means the replay's inputs diverged from reality (e.g. a sibling
 * rewrote a loaded byte) and the block drops to `truncated`. */
static int regs_coherent(const struct user_regs_struct *uc,
                         const struct user_regs_struct *real) {
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
    return ((uint64_t)uc->eflags & EFLAGS_ARITH_MASK) ==
           ((uint64_t)real->eflags & EFLAGS_ARITH_MASK);
}

/* Replay one straight-line block through Unicorn, capturing each interior instruction, until a
 * TAKEN transfer whose target is `pc_next`. Returns 0 on the clean terminator, 1 if Unicorn
 * branched somewhere OTHER than the real boundary (divergence), -1 on a Unicorn fault /
 * undecodable step. The terminating branch is left as the open step (finalized by the next
 * block's seed, or at region exit). */
static int step_block(cap_ctx *c, uc_engine *uc, uint64_t pc_next) {
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
                             int *vec_seeded, int *mxcsr_seeded) {
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

        int brc = step_block(c, uc, S_next.gp.rip);
        if (brc < 0) {
            /* A Unicorn fault / undecodable step — e.g. UC_ERR_INSN_INVALID on a VEX/EVEX
             * instruction that slipped past the replayability gate. This is a DIVERGENCE, not
             * a missing substrate: it must truncate, never masquerade as the ETRACE self-skip
             * that ret was initialized to. */
            c->vt->truncated = true;
            ret = DF_BLOCKSTEP_FAULT;
            goto out;
        }

        dfb_snap_t ucR;
        memset(&ucR, 0, sizeof ucR);
        uc_get_regs(uc, &ucR.gp);
        if (c->want_vec)
            uc_get_vec(uc, &ucR.vec);
        if (brc == 1 || !regs_coherent(&ucR.gp, &S_next.gp) ||
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

int asmtest_dataflow_blockstep_is_pure(const uint8_t *code, size_t code_len,
                                       const char **reason) {
    if (reason != NULL)
        *reason = NULL;
    if (code == NULL || code_len == 0)
        return DF_BLOCKSTEP_EINVAL;
    dfb_scan_t s;
    region_scan(code, code_len, &s);
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
    region_scan(code, code_len, &s);
    if (s.replayable)
        return 1;
    if (reason != NULL)
        *reason = s.replay_reason;
    return 0;
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
     * force_singlestep still runs the scan so info.reason stays informative. */
    dfb_scan_t scan;
    region_scan(code + o.region_off, code_len - (size_t)o.region_off, &scan);
    int gated_off = (!scan.pure || !scan.replayable) && !o.force_replay;
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

    uint64_t stops = 0, steps = 0, entry_rsp = 0;
    int vec_seeded = 0, mxcsr_seeded = 0;
    int rc;
    if (use_replay) {
        c.mr = mr_uc; /* mr_ctx set once the engine stands up */
        int inj = o.inject_divergence ? o.inject_block : -1;
        rc = capture_blockstep(&c, pid, base, code_len, rbase, rend, &o, result,
                               &stops, &steps, &entry_rsp, inj, &vec_seeded,
                               &mxcsr_seeded);
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
    }

    free(c.cur.v);
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
