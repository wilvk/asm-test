/*
 * test_dataflow_blockstep.c — F1 increment 1: the BLOCK-STEP + EMULATOR-REPLAY value tier
 * (src/dataflow_blockstep.c) end-to-end. Promotes the increment-0 spike
 * (examples/blockstep_value_spike.c, findings 2026-07-15-blockstep-value-spike.md) into a
 * gated suite that proves the F1 exit criteria against the shipped single-step ground truth:
 *
 *   1. BYTE-IDENTICAL. On a PURE method the block-step+replay value trace equals the true
 *      single-step trace, record-for-record. The tier drives one process per capture, so the
 *      two differ only by an absolute stack-address delta (ASLR + frame depth); we normalize
 *      both traces rsp-relative (info.entry_rsp anchor) — the "production is one process, the
 *      single-step trace is only an oracle" reconciliation the plan calls for — and then assert
 *      a literal record equality. (Register data values, memory-load/store values, arithmetic
 *      flags, per-step offsets and record counts are all absolute-identical; only stack pointers
 *      and stack effective-addresses carry the delta the anchor removes.)
 *   2. STOP-COUNT. The in-region ptrace stops drop proportionally to mean block length
 *      (block-step boundaries vs single-step per-instruction stops).
 *   3. IMPURE FALLBACK. A method carrying an OS-interacting / nondeterministic instruction
 *      (cpuid) is classified impure by the static purity gate and single-stepped, not replayed
 *      (info.pure == 0, info.reason names the mnemonic), still yielding the correct trace.
 *   4. CANARY. An injected replay-input divergence (a simulated concurrent memory rewrite) is
 *      DETECTED at the next real boundary and the capture drops to truncated — never silently
 *      wrong.
 *
 * VECTOR BREADTH (increment 2) adds the YMM/ZMM half, and every one of its claims is proved by
 * REPRODUCING THE BUG FIRST — each case has a hook that restores the pre-fix behaviour and an
 * assertion that the trace then really is wrong:
 *
 *   5. XMM LIVE-IN SEEDING. A region whose input arrives in xmm0/xmm1 (established by entry
 *      glue before opts.region_off, on the REAL cpu) replays byte-identically to the
 *      single-step oracle. NEGATIVE CONTROL (`no_vec_seed` + `no_vec_canary` = exactly the
 *      pre-increment-2 tier): the capture returns OK and NOT truncated while its trace
 *      DIFFERS from the oracle — the silent wrongness this increment exists to kill. A third
 *      run (`no_vec_seed` alone) shows the vector canary CATCHES it (truncated).
 *   6. VEX-128 IS A SILENT LIAR. `vpaddq xmm0,xmm1,xmm2` is gated off the replay. NEGATIVE
 *      CONTROL (`force_replay`): Unicorn returns UC_ERR_OK and a WRONG value — it drops
 *      VEX.vvvv and runs the legacy 2-operand form — so the forced trace diverges from the
 *      oracle. That is what the gate prevents, and it is why the gate is an ENCODING rule.
 *   7. AVX / AVX-512 REGIONS. VEX-256 and EVEX regions are gated to single-step and produce a
 *      CORRECT trace carrying REAL 256-/512-bit vector values (read off silicon via
 *      NT_X86_XSTATE, asserted against the exact expected bytes). NEGATIVE CONTROL
 *      (`force_replay`): the capture FAULTS + truncates (UC_ERR_INSN_INVALID) rather than
 *      self-skipping or lying. Hardware-gated: skipped where the cpu lacks AVX2 / AVX-512.
 *
 * Self-skips cleanly where ptrace is blocked (seccomp/yama) or PTRACE_SINGLEBLOCK is
 * non-functional (a hypervisor masking DEBUGCTL.BTF), and — via the makefile gate — is built
 * only where libunicorn is present. Off Linux x86-64 / without Capstone+Unicorn the producer
 * is an ENOSYS stub and this suite prints a skip. Linux x86-64 only.
 */
#define _GNU_SOURCE /* memfd_create */

#include "asmtest_valtrace.h"

#include <errno.h>
#include <stddef.h> /* offsetof — the re-declared-struct layout guard */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* The block-step producer ships NO header — a value-trace PRODUCER is a tier, not part of the
 * shared asmtest_valtrace.h sink API — so this suite re-declares its entry points, option/info
 * structs, and return codes here (exactly as test_dataflow_ptrace.c re-declares its producer).
 * The struct layouts must match src/dataflow_blockstep.c field-for-field. */
#define DF_BLOCKSTEP_OK     0
#define DF_BLOCKSTEP_FAULT  1
#define DF_BLOCKSTEP_EINVAL (-1)
#define DF_BLOCKSTEP_ENOSYS (-3)
#define DF_BLOCKSTEP_ETRACE (-4)

typedef struct {
    uint64_t max_insns;
    int force_singlestep;
    int inject_divergence;
    int inject_block;
    uint64_t region_off;
    int no_vec_seed;
    int no_mxcsr_seed;
    int no_vec_canary;
    int force_replay;
    uint64_t stack_hi_pad;
    int no_syscall_inject;
    int no_undef_mask;
    uint64_t inject_flag_bit;
} asmtest_blockstep_opts_t;

typedef struct {
    int pure;
    const char *reason;
    uint64_t stops;
    uint64_t steps;
    uint64_t entry_rsp;
    int vec_width;
    int vec_nregs;
    int uc_vec_width;
    int vec_seeded;
    int mxcsr_seeded;
    int injectable;
    uint64_t injected;
    uint64_t hw_hits;
} asmtest_blockstep_info_t;

int asmtest_dataflow_blockstep_probe(void);
int asmtest_dataflow_blockstep_is_pure(const uint8_t *code, size_t code_len,
                                       const char **reason);
int asmtest_dataflow_blockstep_is_replayable(const uint8_t *code,
                                             size_t code_len,
                                             const char **reason);
int asmtest_dataflow_blockstep_is_injectable(const uint8_t *code,
                                             size_t code_len,
                                             const char **reason);
int asmtest_dataflow_blockstep_scan_hwrec(const uint8_t *code, size_t code_len,
                                          uint64_t *off, size_t off_cap,
                                          int *overflow);
int asmtest_dataflow_blockstep_vec_width(int *nregs);
int asmtest_dataflow_blockstep_uc_vec_width(void);
int asmtest_dataflow_blockstep_run(const uint8_t *code, size_t code_len,
                                   const long *args, int nargs,
                                   const asmtest_blockstep_opts_t *opts,
                                   long *result, asmtest_valtrace_t *vt,
                                   asmtest_blockstep_info_t *info);
void asmtest_dataflow_blockstep_info_layout(size_t *size, size_t *last_off);
void asmtest_dataflow_blockstep_opts_layout(size_t *size, size_t *last_off);

/* Capstone register id the T4 assertions name, and the standard x86 EFLAGS bit positions
 * (architectural, not Capstone-versioned — safe to hardcode). Duplicated rather than
 * #include'd, exactly like test_dataflow_ptrace.c's REG_* constants. */
#define REG_EFLAGS 25
#define EFLAG_CF   0x0001u
#define EFLAG_PF   0x0004u
#define EFLAG_AF   0x0010u
#define EFLAG_ZF   0x0040u
#define EFLAG_SF   0x0080u
#define EFLAG_OF   0x0800u

static int checks, failures;
#define CHECK(c, ...)                                                          \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - " : "not ok %d - ", checks);                     \
        printf(__VA_ARGS__);                                                   \
        printf("\n");                                                          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

/* ------------------------------------------------------------------ */
/* Fixtures (x86-64 SysV; rdi = arg0, rsi = arg1)                       */
/* ------------------------------------------------------------------ */

/* loop_poly(n): the PRIMARY pure oracle fixture — a register-only accumulator loop whose every
 * taken back-edge is ONE block-step stop but SIX single-step stops, so it makes both the
 * byte-identical claim (arithmetic + DEFINED flags across many steps) and the stop-count
 * reduction measurable on one region. Uses only fully-flag-DEFINED add/cmp + flag-neutral
 * mov/lea — never xor-to-zero (whose AF is architecturally UNDEFINED). Returns
 * sum_{i=0}^{n-1}(3i+1) = 3n(n-1)/2 + n; for n=50 => 3725. */
static const uint8_t loop_poly[] = {
    0xb8, 0x00, 0x00, 0x00, 0x00, /* 0x00 mov eax, 0            */
    0xb9, 0x00, 0x00, 0x00, 0x00, /* 0x05 mov ecx, 0            */
    0x48, 0x8d, 0x14, 0x49,       /* 0x0a lea rdx, [rcx+rcx*2]  */
    0x48, 0x01, 0xd0,             /* 0x0e add rax, rdx          */
    0x48, 0x83, 0xc0, 0x01,       /* 0x11 add rax, 1            */
    0x48, 0x83, 0xc1, 0x01,       /* 0x15 add rcx, 1            */
    0x48, 0x39, 0xf9,             /* 0x19 cmp rcx, rdi          */
    0x7c, 0xec,                   /* 0x1c jl  0x0a              */
    0xc3,                         /* 0x1e ret                  */
};

/* mem_chain(a,b): the memory fixture — a straight-line load-after-store + register move chain
 * with NO flag-affecting instruction, isolating memory-value fidelity across the replay (a
 * store to [rsp-8], a dependent load, a lea, and the ret stack pop). One block; returns a+b. */
static const uint8_t mem_chain[] = {
    0x48, 0x89, 0xf8,             /* 0x00 mov rax, rdi       */
    0x48, 0x89, 0x44, 0x24, 0xf8, /* 0x03 mov [rsp-8], rax   */
    0x48, 0x8b, 0x4c, 0x24, 0xf8, /* 0x08 mov rcx, [rsp-8]   */
    0x48, 0x8d, 0x14, 0x31,       /* 0x0d lea rdx, [rcx+rsi] */
    0x48, 0x89, 0xd0,             /* 0x11 mov rax, rdx       */
    0xc3,                         /* 0x14 ret                */
};

/* imp_cpuid(a): the IMPURE-but-runnable fixture. cpuid is OS-visible / model-dependent (the
 * purity gate names it), yet single-steps as one ordinary instruction, so the fallback path
 * produces a correct, deterministic trace: rax is overwritten with arg0 after the cpuid, so the
 * routine returns arg0 regardless of the cpuid result. */
static const uint8_t imp_cpuid[] = {
    0xb8, 0x01, 0x00, 0x00, 0x00, /* 0x00 mov eax, 1  (feature leaf) */
    0x0f, 0xa2,                   /* 0x05 cpuid       IMPURE          */
    0x48, 0x89, 0xf8,             /* 0x07 mov rax, rdi (rax = arg0)   */
    0xc3,                         /* 0x0a ret                         */
};

/* Purity-classifier-only impure fixtures (SCANNED, never executed). */
static const uint8_t imp_syscall[] = {0xb8, 0x27, 0x00, 0x00,
                                      0x00, 0x0f, 0x05, 0xc3};
static const uint8_t imp_rdtsc[] = {0x0f, 0x31, 0xc3};
static const uint8_t imp_rdrand[] = {0x0f, 0xc7, 0xf0, 0xc3};
static const uint8_t imp_int80[] = {0xb8, 0x14, 0x00, 0x00,
                                    0x00, 0xcd, 0x80, 0xc3};

/* ------------------------------------------------------------------ */
/* T5 (F2 increment 2, forward pass) fixtures                          */
/* ------------------------------------------------------------------ */

/* hwrec_multi(): SCAN-ONLY (never executed) — 3 DISTINCT hwrec sites (rdtsc, cpuid, rdrand) at
 * known offsets {0, 2, 4}, for the scan-level unit test of region_scan's nhwrec/hwrec_off
 * bookkeeping (exposed test-side via asmtest_dataflow_blockstep_scan_hwrec). */
static const uint8_t hwrec_multi[] = {
    0x0f, 0x31,       /* 0x00 rdtsc      */
    0x0f, 0xa2,       /* 0x02 cpuid      */
    0x0f, 0xc7, 0xf0, /* 0x04 rdrand eax */
    0xc3,             /* 0x07 ret        */
};

/* hwrec_5site(): SCAN-ONLY — 5 DISTINCT hwrec sites, one past the 4-slot architectural DR cap,
 * for the hwrec_overflow assertion (first 4 offsets {0, 2, 5, 7} must still be reported). */
static const uint8_t hwrec_5site[] = {
    0x0f, 0x31,       /* 0x00 rdtsc      */
    0x0f, 0x01, 0xf9, /* 0x02 rdtscp     */
    0x0f, 0xa2,       /* 0x05 cpuid      */
    0x0f, 0xc7, 0xf0, /* 0x07 rdrand eax */
    0x0f, 0xc7, 0xf8, /* 0x0a rdseed eax */
    0xc3,             /* 0x0d ret        */
};

/* hwrec_rdtsc(): EXECUTED (unlike imp_rdtsc above) — a tiny impure region whose only impurity
 * is one rdtsc site, for the live forward-pass checks: the DR exec-breakpoint boundary firing
 * (info.hw_hits) while the externally observable verdict (gated -> single-step OK; forced ->
 * per-step decode truncates) stays exactly what it was before T5 — T6, not T5, lifts the
 * truncation. */
static const uint8_t hwrec_rdtsc[] = {
    0x0f, 0x31, /* 0x00 rdtsc */
    0xc3,       /* 0x02 ret   */
};

/* ------------------------------------------------------------------ */
/* Undefined-EFLAGS fixtures (T4, BSVS-1)                               */
/*                                                                      */
/* One tiny PURE, REPLAYABLE region per representative table row: set up operands with an   */
/* immediate mov (no external inputs needed — nargs=0), execute the instruction under test,  */
/* `ret`. Each is run BOTH masked (default opts) and unmasked (opts.no_undef_mask=1) and the  */
/* EFLAGS write record compared, so the check is self-referential — it never needs to know    */
/* what the real undefined bit's value happened to be on this cpu, only that masking cleared  */
/* EXACTLY the row's undefined set and nothing else. loop_poly (above) deliberately avoids    */
/* xor-to-zero because its AF is undefined; these fixtures are where that gap is closed.      */
/* ------------------------------------------------------------------ */
static const uint8_t undef_and[] = {0xb8, 0xff, 0x00, 0x00, 0x00, 0xbb, 0x0f,
                                    0x00, 0x00, 0x00, 0x21, 0xd8, 0xc3};
static const uint8_t undef_or[] = {0xb8, 0x0f, 0x00, 0x00, 0x00, 0xbb, 0xf0,
                                   0x00, 0x00, 0x00, 0x09, 0xd8, 0xc3};
static const uint8_t undef_xor[] = {0xb8, 0x0f, 0x00, 0x00, 0x00, 0xbb, 0xf0,
                                    0x00, 0x00, 0x00, 0x31, 0xd8, 0xc3};
/* xor eax,eax — the AF-undefined case loop_poly's own comment names. Used both as a table row
 * and (identically) as the dedicated byte-identical + AF-masked identity case below. */
static const uint8_t xor_to_zero[] = {0x31, 0xc0, 0xc3};
static const uint8_t undef_test[] = {0xb8, 0x0f, 0x00, 0x00, 0x00, 0xbb, 0xf0,
                                     0x00, 0x00, 0x00, 0x85, 0xd8, 0xc3};
static const uint8_t undef_mul[] = {0xb8, 0x03, 0x00, 0x00, 0x00, 0xbb, 0x05,
                                    0x00, 0x00, 0x00, 0xf7, 0xe3, 0xc3};
static const uint8_t undef_imul[] = {0xb8, 0x03, 0x00, 0x00, 0x00, 0xbb, 0x05,
                                     0x00, 0x00, 0x00, 0x0f, 0xaf, 0xc3, 0xc3};
static const uint8_t undef_div[] = {0xb8, 0x0a, 0x00, 0x00, 0x00, 0xba,
                                    0x00, 0x00, 0x00, 0x00, 0xbb, 0x03,
                                    0x00, 0x00, 0x00, 0xf7, 0xf3, 0xc3};
static const uint8_t undef_idiv[] = {0xb8, 0x0a, 0x00, 0x00, 0x00, 0x99, 0xbb,
                                     0x03, 0x00, 0x00, 0x00, 0xf7, 0xfb, 0xc3};
static const uint8_t undef_bsf[] = {0xb8, 0x00, 0x00, 0x00, 0x00, 0xbb, 0x10,
                                    0x00, 0x00, 0x00, 0x0f, 0xbc, 0xc3, 0xc3};
static const uint8_t undef_bsr[] = {0xb8, 0x00, 0x00, 0x00, 0x00, 0xbb, 0x10,
                                    0x00, 0x00, 0x00, 0x0f, 0xbd, 0xc3, 0xc3};
static const uint8_t undef_shl1[] = {0xb8, 0x01, 0x00, 0x00,
                                     0x00, 0xd1, 0xe0, 0xc3};
static const uint8_t undef_shl5[] = {0xb8, 0x01, 0x00, 0x00, 0x00,
                                     0xc1, 0xe0, 0x05, 0xc3};
static const uint8_t undef_shlcl[] = {0xb8, 0x01, 0x00, 0x00, 0x00, 0xb9, 0x05,
                                      0x00, 0x00, 0x00, 0xd3, 0xe0, 0xc3};
static const uint8_t undef_rol1[] = {0xb8, 0x01, 0x00, 0x00,
                                     0x00, 0xd1, 0xc0, 0xc3};
static const uint8_t undef_rol3[] = {0xb8, 0x01, 0x00, 0x00, 0x00,
                                     0xc1, 0xc0, 0x03, 0xc3};
static const uint8_t undef_bt[] = {0xb8, 0x08, 0x00, 0x00, 0x00, 0xbb, 0x03,
                                   0x00, 0x00, 0x00, 0x0f, 0xa3, 0xd8, 0xc3};

/* ------------------------------------------------------------------ */
/* Vector fixtures (increment 2)                                        */
/*                                                                      */
/* Each is one blob with TWO parts. The bytes BEFORE region_off are ENTRY GLUE: the tracee   */
/* really executes them on the real cpu, but the capture neither traces nor replays them —   */
/* they exist to establish LIVE-IN VECTOR STATE, which is the only way a vector value can    */
/* enter the region from outside and so the only thing boundary seeding can be tested on.    */
/* (Vector state computed INSIDE the region would replay self-consistently in a persistent   */
/* Unicorn even with no seeding at all, and would prove nothing.) The glue always ends in a  */
/* `jmp` — a TAKEN branch — so PTRACE_SINGLEBLOCK stops exactly AT region_off rather than    */
/* running past it to the region's own first branch.                                          */
/* ------------------------------------------------------------------ */

/* sse_live_in(a,b): the XMM boundary-seeding case. The region consumes xmm0/xmm1 as LIVE-IN
 * state and commits the result to MEMORY, never to a GP register — which is precisely the
 * shape the GP-only canary is blind to, so an unseeded replay is silently wrong rather than
 * loudly wrong. Returns arg0 (the GP path is deliberately unaffected); the observable is the
 * 16-byte store of xmm0 = a+b. Legacy SSE only, so Unicorn CAN replay it: this is the case
 * the whole increment's perturbation win rests on. Baseline x86-64 — no AVX needed. */
#define SSE_LIVE_IN_ROFF 0x0c
static const uint8_t sse_live_in[] = {
    0x66, 0x48, 0x0f, 0x6e, 0xc7, /* 0x00 movq xmm0, rdi        (glue) */
    0x66, 0x48, 0x0f, 0x6e, 0xce, /* 0x05 movq xmm1, rsi        (glue) */
    0xeb, 0x00,                   /* 0x0a jmp 0x0c              (glue) */
    0x66, 0x0f, 0xd4, 0xc1,       /* 0x0c paddq xmm0, xmm1  <- LIVE-IN */
    0x0f, 0x11, 0x44, 0x24, 0xf0, /* 0x10 movups [rsp-16], xmm0        */
    0x48, 0x89, 0xf8,             /* 0x15 mov rax, rdi                 */
    0xc3,                         /* 0x18 ret                          */
};

/* vex128_liar(a,b): the VEX-128 SILENT-MISDECODE case. `vpaddq xmm0,xmm1,xmm2` means
 * xmm0 = xmm1+xmm2 = b+a on real silicon. Every released Unicorn drops VEX.vvvv and executes
 * the legacy 2-operand `paddq xmm0,xmm2` = a+a instead — returning UC_ERR_OK. With a=7,b=5:
 * real 12, Unicorn 14. The result again reaches only MEMORY, so nothing but the vector canary
 * or the encoding gate can catch it. */
#define VEX128_LIAR_ROFF 0x11
static const uint8_t vex128_liar[] = {
    0x66, 0x48, 0x0f, 0x6e, 0xc7, /* 0x00 movq xmm0, rdi        (glue) */
    0x66, 0x48, 0x0f, 0x6e, 0xce, /* 0x05 movq xmm1, rsi        (glue) */
    0x66, 0x48, 0x0f, 0x6e, 0xd7, /* 0x0a movq xmm2, rdi        (glue) */
    0xeb, 0x00,                   /* 0x0f jmp 0x11              (glue) */
    0xc5, 0xf1, 0xd4, 0xc2,       /* 0x11 vpaddq xmm0,xmm1,xmm2  VEX128 */
    0x0f, 0x11, 0x44, 0x24, 0xf0, /* 0x15 movups [rsp-16], xmm0        */
    0x48, 0x89, 0xf8,             /* 0x1a mov rax, rdi                 */
    0xc3,                         /* 0x1d ret                          */
};

/* avx_ymm(a): a VEX-256 region. ymm0 is broadcast from arg0 by the glue, so the region has
 * 256-bit LIVE-IN state; it doubles it and returns the low lane. Gated to single-step (no
 * released Unicorn executes VEX-256), where its full 32-byte ymm0 value IS captured from
 * hardware. Returns 2*a. Needs AVX2 (vpbroadcastq ymm). */
#define AVX_YMM_ROFF 0x0c
static const uint8_t avx_ymm[] = {
    0x66, 0x48, 0x0f, 0x6e, 0xc7, /* 0x00 movq xmm0, rdi          (glue) */
    0xc4, 0xe2, 0x7d, 0x59, 0xc0, /* 0x05 vpbroadcastq ymm0, xmm0 (glue) */
    0xeb, 0x00,                   /* 0x0a jmp 0x0c                (glue) */
    0xc5, 0xfd, 0xd4, 0xc0,       /* 0x0c vpaddq ymm0, ymm0, ymm0        */
    0xc4, 0xe1, 0xf9, 0x7e, 0xc0, /* 0x10 vmovq rax, xmm0                */
    0xc3,                         /* 0x15 ret                            */
};

/* avx512_zmm(a): the EVEX analogue — 512-bit live-in state, doubled. Gated to single-step;
 * its full 64-byte zmm0 value is captured from hardware (ZMM_Hi256 + the legacy/YMM halves
 * reassembled). Returns 2*a. Needs AVX-512F. */
#define AVX512_ZMM_ROFF 0x0d
static const uint8_t avx512_zmm[] = {
    0x66, 0x48, 0x0f, 0x6e, 0xc7, /* 0x00 movq xmm0, rdi           (glue) */
    0x62, 0xf2, 0xfd, 0x48, 0x59,
    0xc0,       /* 0x05 vpbroadcastq zmm0, xmm0  (glue) */
    0xeb, 0x00, /* 0x0b jmp 0x0d                 (glue) */
    0x62, 0xf1, 0xfd, 0x48, 0xd4,
    0xc0,                         /* 0x0d vpaddq zmm0, zmm0, zmm0         */
    0xc4, 0xe1, 0xf9, 0x7e, 0xc0, /* 0x13 vmovq rax, xmm0                 */
    0xc3,                         /* 0x18 ret                             */
};

/* hi16_zmm(a): the Hi16_ZMM (zmm16-31) case. zmm16 lives ONLY in XSAVE component 7, reassembled
 * by a code path nothing else reaches — untested reassembly of a register file we cannot replay
 * would be exactly the silent-zeroing this increment exists to kill, so it is covered rather
 * than deleted. Returns 2*a via zmm16. Needs AVX-512F. */
#define HI16_ZMM_ROFF 0x0d
static const uint8_t hi16_zmm[] = {
    0x66, 0x48, 0x0f, 0x6e, 0xc7, /* 0x00 movq xmm0, rdi            (glue) */
    0x62, 0xe2, 0xfd, 0x48, 0x59,
    0xc0,       /* 0x05 vpbroadcastq zmm16, xmm0  (glue) */
    0xeb, 0x00, /* 0x0b jmp 0x0d                  (glue) */
    0x62, 0xa1, 0xfd, 0x40, 0xd4,
    0xc0, /* 0x0d vpaddq zmm16, zmm16, zmm16       */
    0x62, 0xe1, 0xfd, 0x08, 0x7e,
    0xc0, /* 0x13 vmovq rax, xmm16                 */
    0xc3, /* 0x19 ret                              */
};

/* evex_invis(a): the fixture that makes the ENCODING-vs-METADATA decision LOAD-BEARING. EVERY
 * instruction in its region is invisible to Capstone's AVX taxonomy — `vpbroadcastq zmm0,xmm0`
 * decodes with correct mnemonic and operands yet reports NO registers and belongs to NO
 * X86_GRP_AVX/AVX2/AVX512 group, and mov/ret are not AVX either. So a gate built on that
 * metadata calls this region REPLAYABLE and hands an EVEX instruction to an emulator with no
 * decoder for it; only the encoding rule (the 0x62 prefix byte) catches it. Returns arg0.
 * Needs AVX-512F. */
#define EVEX_INVIS_ROFF 0x07
static const uint8_t evex_invis[] = {
    0x66, 0x48, 0x0f, 0x6e, 0xc7, /* 0x00 movq xmm0, rdi            (glue) */
    0xeb, 0x00,                   /* 0x05 jmp 0x07                  (glue) */
    0x62, 0xf2, 0xfd, 0x48, 0x59,
    0xc0,             /* 0x07 vpbroadcastq zmm0,xmm0 INVISIBLE */
    0x48, 0x89, 0xf8, /* 0x0d mov rax, rdi                     */
    0xc3,             /* 0x10 ret                              */
};

/* vex_bmi(a): the same decision with NO hardware gate. `andn` is VEX-encoded but GP-only:
 * Unicorn executes it faithfully (measured against silicon), yet the encoding rule gates it —
 * the deliberate over-gate, whose only cost is the perturbation win. Capstone puts it in no AVX
 * group, so a metadata gate would call this region replayable. Asserting the over-gate pins the
 * encoding rule on EVERY x86-64 box, not just AVX-512 ones. Returns arg0. */
#define VEX_BMI_ROFF 0x02
static const uint8_t vex_bmi[] = {
    0xeb, 0x00,                   /* 0x00 jmp 0x02              (glue) */
    0xc4, 0xe2, 0x60, 0xf2, 0xc7, /* 0x02 andn eax, ebx, edi (VEX-GP)  */
    0x48, 0x89, 0xf8,             /* 0x07 mov rax, rdi                 */
    0xc3,                         /* 0x0a ret                          */
};

/* island(a,b): a CONSTANT-POOL ISLAND inside the region — the shape a JIT method-map (this
 * tier's stated target) routinely contains. The `jmp` hops two data bytes at 0x13 that a LINEAR
 * scan instead decodes as the start of a `movabs rax, imm64`, swallowing the VEX prefix of the
 * `vpaddq` at 0x15 as immediate data and then desyncing outright (remaining=5 of 17). The scan
 * NEVER SEES the VEX-128, so before the fail-closed fix BOTH verdicts stayed optimistic: the
 * VEX-128 reached Unicorn ungated AND touches_vec=0 removed both the seed and the vector canary
 * that would have caught the lie. Executes correctly (the jmp hops the island): the real vpaddq
 * stores xmm1+xmm2 = b+a to memory. Returns arg0. */
#define ISLAND_ROFF 0x11
static const uint8_t island[] = {
    0x66, 0x48, 0x0f, 0x6e, 0xc7, /* 0x00 movq xmm0, rdi        (glue) */
    0x66, 0x48, 0x0f, 0x6e, 0xce, /* 0x05 movq xmm1, rsi        (glue) */
    0x66, 0x48, 0x0f, 0x6e, 0xd7, /* 0x0a movq xmm2, rdi        (glue) */
    0xeb, 0x00,                   /* 0x0f jmp 0x11              (glue) */
    0xeb, 0x02,                   /* 0x11 jmp 0x15  <- REGION: hop it  */
    0x48, 0xb8,                   /* 0x13 ISLAND (data, not code)      */
    0xc5, 0xf1, 0xd4, 0xc2,       /* 0x15 vpaddq xmm0, xmm1, xmm2      */
    0x0f, 0x11, 0x44, 0x24, 0xf0, /* 0x19 movups [rsp-16], xmm0        */
    0x48, 0x89, 0xf8,             /* 0x1e mov rax, rdi                 */
    0xc3,                         /* 0x21 ret                          */
};

/* imp_vec(a,b): an IMPURE region that ALSO touches vector state — ordinary in real JIT'd code
 * (a cpuid/rdtsc beside SSE). The impurity gate settles `pure` at the cpuid, but the sweep must
 * keep classifying: before the fix it BROKE there, so the paddq after it was never seen,
 * touches_vec came back 0, xstate_read was never called on the single-step fallback, and every
 * vector record was emitted value_valid=0 at rc=OK — the headline "values in the trace"
 * deliverable failing silently on the exact path all AVX code is routed to. Returns arg0; the
 * observable is the 16-byte store of xmm0 = a+b. */
#define IMP_VEC_ROFF 0x0c
static const uint8_t imp_vec[] = {
    0x66, 0x48, 0x0f, 0x6e, 0xc7, /* 0x00 movq xmm0, rdi        (glue) */
    0x66, 0x48, 0x0f, 0x6e, 0xce, /* 0x05 movq xmm1, rsi        (glue) */
    0xeb, 0x00,                   /* 0x0a jmp 0x0c              (glue) */
    0x31, 0xc0,                   /* 0x0c xor eax, eax  <- REGION      */
    0x0f, 0xa2,                   /* 0x0e cpuid         (impure)       */
    0x66, 0x0f, 0xd4, 0xc1,       /* 0x10 paddq xmm0, xmm1  <- AFTER   */
    0x0f, 0x11, 0x44, 0x24, 0xf0, /* 0x14 movups [rsp-16], xmm0        */
    0x48, 0x89, 0xf8,             /* 0x19 mov rax, rdi                 */
    0xc3,                         /* 0x1c ret                          */
};

/* imp_vex: scan-only. `cpuid; vpaddq xmm0,xmm1,xmm2; ret` — the region that made the PUBLIC
 * is_replayable() lie: the old sweep broke at the cpuid, so it answered "yes, Unicorn can
 * faithfully replay this" (reason=NULL) about a region containing the instruction this tier
 * calls a silent liar. run() masked it (impurity single-steps the region anyway), so no check
 * caught it — but the entry point is public and the answer was wrong. */
static const uint8_t imp_vex[] = {
    0x0f, 0xa2,             /* cpuid                   */
    0xc5, 0xf1, 0xd4, 0xc2, /* vpaddq xmm0, xmm1, xmm2 */
    0xc3,                   /* ret                     */
};

/* fp_round(bits_a, bits_b): the MXCSR case. The glue installs RC=toward-zero (MXCSR 0x7F80) and
 * loads two doubles; the region divides them, commits the result to MEMORY, then CLEARS xmm0 —
 * so the register canary is blind and the stored value is the only witness. An unseeded MXCSR
 * replays at Unicorn's default 0x1F80 (round-to-nearest): 1.0/5.0 gives oracle
 * 0x3fc9999999999999 (RZ) but replay 0x3fc999999999999a (RN) at rc=OK, truncated=0, pure=1 — a
 * silent lie on the legacy-SSE path the whole perturbation win rests on. Non-default rounding is
 * not exotic: -ffast-math's crtfastmath.o and JIT/managed runtimes both install it. Returns
 * arg0. Baseline x86-64 (SSE2) — no AVX needed. */
#define FP_ROUND_ROFF 0x19
static const uint8_t fp_round[] = {
    0xc7, 0x44, 0x24, 0xf8, 0x80, 0x7f,
    0x00, 0x00,                         /* 0x00 mov [rsp-8],0x7F80 */
    0x0f, 0xae, 0x54, 0x24, 0xf8,       /* 0x08 ldmxcsr [rsp-8]    (RC=RZ) */
    0x66, 0x48, 0x0f, 0x6e, 0xc7,       /* 0x0d movq xmm0, rdi        (1.0) */
    0x66, 0x48, 0x0f, 0x6e, 0xce,       /* 0x12 movq xmm1, rsi        (5.0) */
    0xeb, 0x00,                         /* 0x17 jmp 0x19                    */
    0xf2, 0x0f, 0x5e, 0xc1,             /* 0x19 divsd xmm0,xmm1  <- REGION  */
    0xf2, 0x0f, 0x11, 0x44, 0x24, 0xf0, /* 0x1d movsd [rsp-16], xmm0        */
    0x0f, 0x57, 0xc0,                   /* 0x23 xorps xmm0,xmm0 (blind it)  */
    0x48, 0x89, 0xf8,                   /* 0x26 mov rax, rdi                */
    0xc3,                               /* 0x29 ret                         */
};

/* ------------------------------------------------------------------ */
/* F2 fixtures — record-and-inject over OS-interacting regions           */
/*                                                                      */
/* THE ORACLE CONSTRAINT THAT SHAPES ALL OF THESE. The tier's oracle captures the SAME region  */
/* twice, in TWO SEPARATE forked tracees, and demands byte-identical traces. So a fixture can  */
/* only use a syscall whose result is the SAME in both children. That rules out getpid and     */
/* clock_gettime — the plan's own example! — and it is why these use:                          */
/*   - getppid: returns the TEST's pid, so both children agree AND the value is externally     */
/*     checkable against the suite's own getpid(). A syscall result nothing hardcodes.         */
/*   - pread with an EXPLICIT offset into a seeded memfd: repeatable, and immune to the shared */
/*     file offset the two children inherit (a plain read() would give the 2nd child different */
/*     bytes and silently break the oracle rather than the code).                              */
/* clock_gettime is still covered, by a DIFFERENT oracle that does not need determinism —      */
/* see run_syscall_mem_case: the replay's recorded LOAD value is compared against the REAL     */
/* tracee's own returned rax from the same capture, which are independent sources.             */
/* ------------------------------------------------------------------ */

/* sc_getppid(): the syscall whose RESULT IS CONSUMED. getppid() == the test's own pid — equal
 * across both forked captures (unlike getpid) and checkable from outside. `add rax, rax` reads
 * the kernel's return back out of the register file, so the injected value reaches the TRACE as
 * a read record: without injection the replay would carry rax = 110, the syscall NUMBER.
 * Returns 2*testpid. */
static const uint8_t sc_getppid[] = {
    0xb8, 0x6e, 0x00, 0x00, 0x00, /* 0x00 mov eax, 110   (SYS_getppid) */
    0x0f, 0x05,                   /* 0x05 syscall        IMPURE        */
    0x48, 0x01, 0xc0,             /* 0x07 add rax, rax   consumes rax  */
    0xc3,                         /* 0x0a ret                          */
};

/* sc_int80(): the LEGACY gate. i386 __NR_getppid = 64. Distinct from `syscall` in a way that
 * matters to the write set: an interrupt gate clobbers rax ONLY, leaving rcx/r11 intact
 * (measured), so claiming rcx/r11 defs here would forge two defs that never happened.
 * Returns 2*testpid. */
static const uint8_t sc_int80[] = {
    0xb8, 0x40, 0x00, 0x00, 0x00, /* 0x00 mov eax, 64  (i386 getppid) */
    0xcd, 0x80,                   /* 0x05 int 0x80     IMPURE         */
    0x48, 0x01, 0xc0,             /* 0x07 add rax, rax                */
    0xc3,                         /* 0x0a ret                         */
};

/* sc_pread(fd): THE EXIT CRITERION — the kernel FILLS A BUFFER and the region LOADS it. This is
 * the case the plan calls "the hard case", and the one where a stale replay snapshot would be
 * silently wrong rather than loudly wrong.
 *
 * The SENTINEL is what makes it unambiguous. The region first stores 0x1111111111111111 into
 * the very buffer the kernel is about to overwrite, so the two hypotheses give DIFFERENT,
 * PREDICTABLE answers: a replay reading the post-syscall ground-truth snapshot returns the
 * file's bytes; one reading its own stale pre-syscall memory returns the sentinel. Without it a
 * stale read would return whatever the red zone happened to hold — unpredictable, and a fixture
 * that cannot distinguish its own failure mode proves nothing.
 * Returns SC_PREAD_MAGIC. */
#define SC_PREAD_SENTINEL 0x1111111111111111ULL
#define SC_PREAD_MAGIC    0x0BADF00DDEADBEEFULL
static const uint8_t sc_pread[] = {
    0x48, 0xb8, 0x11, 0x11, 0x11, 0x11,
    0x11, 0x11, 0x11, 0x11,             /* 0x00 movabs rax, SENTINEL     */
    0x48, 0x89, 0x44, 0x24, 0xc0,       /* 0x0a mov [rsp-64], rax        */
    0xb8, 0x11, 0x00, 0x00, 0x00,       /* 0x0f mov eax, 17 (pread64)    */
    0x48, 0x8d, 0x74, 0x24, 0xc0,       /* 0x14 lea rsi, [rsp-64]  buf   */
    0xba, 0x08, 0x00, 0x00, 0x00,       /* 0x19 mov edx, 8         count */
    0x41, 0xba, 0x00, 0x00, 0x00, 0x00, /* 0x1e mov r10d, 0        offset*/
    0x0f, 0x05,                         /* 0x24 syscall            IMPURE*/
    0x48, 0x8b, 0x44, 0x24, 0xc0,       /* 0x26 mov rax, [rsp-64]  LOAD  */
    0xc3,                               /* 0x2b ret                      */
};

/* sc_clock(): the plan's OWN named example, clock_gettime — and the one the byte-identical
 * oracle structurally CANNOT judge, because two captures happen at two different times. It is
 * covered anyway, by comparing the replay's recorded load value against the REAL tracee's
 * returned rax from the SAME capture (independent sources — see run_syscall_mem_case). Same
 * sentinel trick: a stale snapshot yields 0x1111111111111111, never a plausible tv_sec.
 * Returns tv_sec (CLOCK_MONOTONIC). */
static const uint8_t sc_clock[] = {
    0x48, 0xb8, 0x11, 0x11, 0x11,
    0x11, 0x11, 0x11, 0x11, 0x11, /* 0x00 movabs rax, SENTINEL      */
    0x48, 0x89, 0x44, 0x24, 0xc0, /* 0x0a mov [rsp-64], rax         */
    0xbf, 0x01, 0x00, 0x00, 0x00, /* 0x0f mov edi, 1 CLOCK_MONOTONIC*/
    0x48, 0x8d, 0x74, 0x24, 0xc0, /* 0x14 lea rsi, [rsp-64]         */
    0xb8, 0xe4, 0x00, 0x00, 0x00, /* 0x19 mov eax, 228              */
    0x0f, 0x05,                   /* 0x1e syscall            IMPURE */
    0x48, 0x8b, 0x44, 0x24, 0xc0, /* 0x20 mov rax, [rsp-64]  tv_sec */
    0xc3,                         /* 0x25 ret                       */
};

/* blind_rdtsc(a): the fixture that makes the PER-STEP DECODE load-bearing, by reproducing the
 * silent wrongness it prevents.
 *
 * MEASURED: Unicorn EXECUTES rdtsc and returns UC_ERR_OK with a fabricated counter. The
 * coherence canary would normally catch that at the next boundary — so a naive fixture proves
 * nothing about the decode, because the canary alone would have saved it. This region therefore
 * OVERWRITES BOTH of rdtsc's outputs (rax AND rdx) with rdi before the boundary, leaving the
 * canary completely blind: every register it compares matches. The only thing standing between
 * this region and a confidently-reported fabricated TSC in the trace is step_block's refusal to
 * execute an impure instruction. Returns arg0. */
static const uint8_t blind_rdtsc[] = {
    0x0f, 0x31,       /* 0x00 rdtsc        writes eax + edx    */
    0x48, 0x89, 0xf8, /* 0x02 mov rax, rdi kill rax            */
    0x48, 0x89, 0xfa, /* 0x05 mov rdx, rdi kill rdx -> BLIND   */
    0xc3,             /* 0x08 ret                              */
};

/* sc_loop(n): a syscall in a LOOP — the ONLY fixture here that injects more than once.
 * Every other F2 case injects exactly one syscall, so a defect that only appears on the SECOND
 * injection (state carried between blocks, a re-seed that happens once, an injection consumed
 * rather than repeated) would be invisible to all of them. r8/r9 carry the accumulator across
 * the syscalls precisely because they are caller-saved AND untouched by `syscall`, which
 * clobbers only rax/rcx/r11 — so the loop also proves the injection does not damage live state
 * around it. Returns n * testpid; injected == n. */
static const uint8_t sc_loop[] = {
    0x41, 0xb8, 0x00, 0x00, 0x00, 0x00, /* 0x00 mov r8d, 0    sum      */
    0x41, 0xb9, 0x00, 0x00, 0x00, 0x00, /* 0x06 mov r9d, 0    i        */
    0xb8, 0x6e, 0x00, 0x00, 0x00,       /* 0x0c mov eax, 110           */
    0x0f, 0x05,                         /* 0x11 syscall       IMPURE   */
    0x49, 0x01, 0xc0,                   /* 0x13 add r8, rax   sum+=pid */
    0x49, 0x83, 0xc1, 0x01,             /* 0x16 add r9, 1              */
    0x49, 0x39, 0xf9,                   /* 0x1a cmp r9, rdi            */
    0x7c, 0xed,                         /* 0x1d jl 0x0c                */
    0x4c, 0x89, 0xc0,                   /* 0x1f mov rax, r8            */
    0xc3,                               /* 0x22 ret                    */
};

/* sc_pread_heap(fd, buf): the kernel fills a buffer OUTSIDE the replay's stack window, and the
 * region then LOADS it. This pins the exact SCOPE of F2's memory story, which is inherited from
 * F1 rather than introduced here: the Unicorn guest maps only the code pages and a window
 * around rsp, so a kernel write to the heap is memory the replay never had. The load therefore
 * hits unmapped guest memory and the capture TRUNCATES — the honest outcome — while the
 * single-step fallback still reports the region correctly. Returns SC_PREAD_MAGIC when
 * single-stepped. */
static const uint8_t sc_pread_heap[] = {
    0xb8, 0x11, 0x00, 0x00, 0x00, /* 0x00 mov eax, 17  (rdi=fd, rsi=buf) */
    0xba, 0x08, 0x00, 0x00, 0x00, /* 0x05 mov edx, 8                     */
    0x41, 0xba, 0x00, 0x00, 0x00,
    0x00,             /* 0x0a mov r10d, 0                    */
    0x0f, 0x05,       /* 0x10 syscall               IMPURE   */
    0x48, 0x8b, 0x06, /* 0x12 mov rax, [rsi]  HEAP load      */
    0xc3,             /* 0x15 ret                            */
};

/* sc_execve(path): the syscall that DOES NOT RETURN — see run_fail_closed_case. On success the
 * kernel replaces the address space and control resurfaces at the new program's entry, so the
 * boundary after the syscall is NOT syscall+2 and there is no result to carry across. The
 * static scan cannot know this (it is a property of the run, not of the bytes), so it is the
 * runtime boundary check that must refuse. */
static const uint8_t sc_execve[] = {
    0xbe, 0x00, 0x00, 0x00, 0x00, /* 0x00 mov esi, 0     argv = NULL   */
    0xba, 0x00, 0x00, 0x00, 0x00, /* 0x05 mov edx, 0     envp = NULL   */
    0xb8, 0x3b, 0x00, 0x00, 0x00, /* 0x0a mov eax, 59    SYS_execve    */
    0x0f, 0x05,                   /* 0x0f syscall        NEVER RETURNS */
    0xc3,                         /* 0x11 ret            (on failure)  */
};

/* sc_then_cpuid(): scan-only. The ALL-quantifier case: an injectable impurity FOLLOWED by a
 * non-injectable one. If `injectable` were settled at the FIRST impurity — the exact shape of
 * the HIGH-3 early-break bug F1's review found — this region would be called injectable and its
 * cpuid would reach the replay. The honest reason is "cpuid", not "syscall". */
static const uint8_t sc_then_cpuid[] = {
    0xb8, 0x6e, 0x00, 0x00, 0x00, /* mov eax, 110 */
    0x0f, 0x05,                   /* syscall      */
    0x0f, 0xa2,                   /* cpuid        */
    0xc3,                         /* ret          */
};

/* ------------------------------------------------------------------ */
/* rsp-relative normalization + byte-identical comparison               */
/* ------------------------------------------------------------------ */

/* Is `q` a stack quantity — within the few words the fixtures touch around the region-entry
 * rsp? Generous window (1 MiB below, 64 KiB above) so every stack address / stack-pointer value
 * lands inside it while the fixtures' tiny data values (<= 3725) and any code address stay
 * outside. */
static int near_stack(uint64_t q, uint64_t anchor) {
    if (anchor == 0)
        return 0;
    uint64_t lo = anchor - 0x100000ULL;
    uint64_t hi = anchor + 0x10000ULL;
    return q >= lo && q <= hi;
}

/* Normalize every stack-absolute quantity in `v` to an offset from `anchor` (the capture's
 * region-entry rsp): a memory record's stack effective-address, and a register record's
 * stack-pointer value (rsp/rbp). Two one-process captures of the same deterministic routine
 * then differ in NOTHING — the ASLR/frame-depth stack delta is the only absolute variance, and
 * this removes it, so a literal record equality is meaningful. Data values, per-step offsets,
 * flags and record counts are untouched (already absolute-identical). */
static void normalize_rsp_relative(asmtest_valtrace_t *v, uint64_t anchor) {
    for (size_t i = 0; i < v->recs_len; i++) {
        at_val_rec_t *r = &v->recs[i];
        if (r->kind != AT_LOC_REG) {
            if (near_stack(r->addr, anchor))
                r->addr -= anchor;
        } else if (r->value_valid && !r->wide && near_stack(r->value, anchor)) {
            r->value -= anchor;
        }
    }
}

static int rec_eq(const at_val_rec_t *x, const at_val_rec_t *y) {
    return x->kind == y->kind && x->reg == y->reg && x->base == y->base &&
           x->index == y->index && x->scale == y->scale && x->disp == y->disp &&
           x->addr == y->addr && x->size == y->size &&
           x->is_write == y->is_write && x->value_valid == y->value_valid &&
           x->wide == y->wide && x->wide_off == y->wide_off &&
           x->value == y->value && x->step == y->step;
}

/* 1 iff the two (already-normalized) traces are identical; on a mismatch prints the first
 * differing element. *rawmemcmp receives whether a literal memcmp of the record arrays also
 * matched (a stricter check that also catches struct padding). */
static int traces_identical(const asmtest_valtrace_t *A,
                            const asmtest_valtrace_t *B, int *rawmemcmp) {
    if (rawmemcmp)
        *rawmemcmp = 0;
    if (A->steps_len != B->steps_len) {
        printf("#   steps_len differ: A=%zu B=%zu\n", A->steps_len,
               B->steps_len);
        return 0;
    }
    if (A->recs_len != B->recs_len) {
        printf("#   recs_len differ: A=%zu B=%zu\n", A->recs_len, B->recs_len);
        return 0;
    }
    for (size_t i = 0; i < A->steps_len; i++)
        if (A->insn_off[i] != B->insn_off[i]) {
            printf("#   insn_off[%zu] differ: A=0x%llx B=0x%llx\n", i,
                   (unsigned long long)A->insn_off[i],
                   (unsigned long long)B->insn_off[i]);
            return 0;
        }
    for (size_t i = 0; i < A->recs_len; i++)
        if (!rec_eq(&A->recs[i], &B->recs[i])) {
            const at_val_rec_t *x = &A->recs[i], *y = &B->recs[i];
            printf(
                "#   rec[%zu] differ (step %u kind %d reg %u write %d): "
                "A.addr=0x%llx A.value=0x%llx  B.addr=0x%llx B.value=0x%llx\n",
                i, x->step, (int)x->kind, x->reg, (int)x->is_write,
                (unsigned long long)x->addr, (unsigned long long)x->value,
                (unsigned long long)y->addr, (unsigned long long)y->value);
            return 0;
        }
    if (A->wide_len != B->wide_len ||
        (A->wide_len && memcmp(A->wide, B->wide, A->wide_len) != 0)) {
        printf("#   wide[] differ\n");
        return 0;
    }
    if (rawmemcmp)
        *rawmemcmp =
            (A->recs_len == B->recs_len) &&
            memcmp(A->recs, B->recs, A->recs_len * sizeof(at_val_rec_t)) == 0;
    return 1;
}

/* Find the LAST EFLAGS write record in `v`. Each fixture below has exactly one instruction
 * that writes flags, so "last" and "only" coincide; returns NULL if none is present. */
static const at_val_rec_t *find_eflags_write(const asmtest_valtrace_t *v) {
    const at_val_rec_t *found = NULL;
    for (size_t i = 0; i < v->recs_len; i++) {
        const at_val_rec_t *r = &v->recs[i];
        if (r->kind == AT_LOC_REG && r->reg == REG_EFLAGS && r->is_write)
            found = r;
    }
    return found;
}

/* ------------------------------------------------------------------ */
/* T4 — undefined-EFLAGS classification: table unit checks              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const uint8_t *code;
    size_t len;
    uint64_t undef_mask; /* this row's expected UNDEFINED EFLAGS bit set */
} undef_row_t;

static const undef_row_t undef_rows[] = {
    {"and", undef_and, sizeof undef_and, EFLAG_AF},
    {"or", undef_or, sizeof undef_or, EFLAG_AF},
    {"xor", undef_xor, sizeof undef_xor, EFLAG_AF},
    {"xor-to-zero", xor_to_zero, sizeof xor_to_zero, EFLAG_AF},
    {"test", undef_test, sizeof undef_test, EFLAG_AF},
    {"mul (1-op)", undef_mul, sizeof undef_mul,
     EFLAG_SF | EFLAG_ZF | EFLAG_AF | EFLAG_PF},
    {"imul (2-op)", undef_imul, sizeof undef_imul,
     EFLAG_SF | EFLAG_ZF | EFLAG_AF | EFLAG_PF},
    {"div", undef_div, sizeof undef_div,
     EFLAG_CF | EFLAG_PF | EFLAG_AF | EFLAG_ZF | EFLAG_SF | EFLAG_OF},
    {"idiv", undef_idiv, sizeof undef_idiv,
     EFLAG_CF | EFLAG_PF | EFLAG_AF | EFLAG_ZF | EFLAG_SF | EFLAG_OF},
    {"bsf", undef_bsf, sizeof undef_bsf,
     EFLAG_CF | EFLAG_OF | EFLAG_SF | EFLAG_AF | EFLAG_PF},
    {"bsr", undef_bsr, sizeof undef_bsr,
     EFLAG_CF | EFLAG_OF | EFLAG_SF | EFLAG_AF | EFLAG_PF},
    {"shl reg,1 (count=1)", undef_shl1, sizeof undef_shl1, EFLAG_AF},
    {"shl reg,5 (count>1)", undef_shl5, sizeof undef_shl5, EFLAG_AF | EFLAG_OF},
    {"shl reg,cl (cl=5)", undef_shlcl, sizeof undef_shlcl, EFLAG_AF | EFLAG_OF},
    {"rol reg,1 (count=1)", undef_rol1, sizeof undef_rol1, 0},
    {"rol reg,3 (count>1)", undef_rol3, sizeof undef_rol3, EFLAG_OF},
    {"bt", undef_bt, sizeof undef_bt,
     EFLAG_OF | EFLAG_SF | EFLAG_AF | EFLAG_PF},
};

/* Run one table row masked (default opts) and unmasked (opts.no_undef_mask=1), then assert
 * the masked EFLAGS write record equals the unmasked one with EXACTLY the row's undefined
 * bits cleared — a self-referential check that never has to know what value an undefined bit
 * happened to take on this cpu, only that masking changed nothing else. `force_singlestep` is
 * used (not the default block-step+replay opts) because it needs only PTRACE_SINGLESTEP, not
 * PTRACE_SINGLEBLOCK/BTF, so this table runs on a BTF-masked VM same as bare metal — the
 * doc's "run everywhere Capstone exists, VM included". Returns 0 (and prints a `# SKIP`,
 * charging no failure) iff `allow_skip` and ptrace itself is unavailable — checked ONLY on
 * the first row, so a genuinely ptrace-denied host self-skips cleanly instead of failing
 * every row; returns 1 otherwise. */
static int run_undef_row(const undef_row_t *row, int allow_skip) {
    asmtest_valtrace_t *masked = asmtest_valtrace_new(64, 512, 512);
    asmtest_valtrace_t *raw = asmtest_valtrace_new(64, 512, 512);
    int cont = 1;
    if (masked == NULL || raw == NULL) {
        CHECK(0, "undef table: %s: valtrace_new", row->name);
        goto done;
    }
    {
        long rm = 0, rr = 0;
        asmtest_blockstep_info_t im = {0}, ir = {0};
        asmtest_blockstep_opts_t om, orw;
        memset(&om, 0, sizeof om);
        om.inject_block = -1;
        om.force_singlestep = 1;
        memset(&orw, 0, sizeof orw);
        orw.inject_block = -1;
        orw.force_singlestep = 1;
        orw.no_undef_mask = 1;

        int rcm = asmtest_dataflow_blockstep_run(row->code, row->len, NULL, 0,
                                                 &om, &rm, masked, &im);
        if (allow_skip && rcm == DF_BLOCKSTEP_ETRACE) {
            printf("# SKIP undefined-EFLAGS table: ptrace unavailable "
                   "(seccomp/yama)\n");
            cont = 0;
            goto done;
        }
        int rcr = asmtest_dataflow_blockstep_run(row->code, row->len, NULL, 0,
                                                 &orw, &rr, raw, &ir);
        if (rcm != DF_BLOCKSTEP_OK || rcr != DF_BLOCKSTEP_OK) {
            CHECK(0, "undef table: %s: capture failed (masked rc=%d raw rc=%d)",
                  row->name, rcm, rcr);
            goto done;
        }
        const at_val_rec_t *mrec = find_eflags_write(masked);
        const at_val_rec_t *rrec = find_eflags_write(raw);
        CHECK(mrec != NULL && rrec != NULL,
              "undef table: %s: an EFLAGS write record exists on both runs",
              row->name);
        if (mrec != NULL && rrec != NULL) {
            CHECK((mrec->value & row->undef_mask) == 0,
                  "undef table: %s: masked run clears every undefined bit "
                  "(undef_mask=0x%llx masked_value=0x%llx)",
                  row->name, (unsigned long long)row->undef_mask,
                  (unsigned long long)mrec->value);
            CHECK(mrec->value == (rrec->value & ~row->undef_mask),
                  "undef table: %s: masked value == raw value with ONLY the "
                  "undefined bits cleared (raw=0x%llx masked=0x%llx)",
                  row->name, (unsigned long long)rrec->value,
                  (unsigned long long)mrec->value);
        }
    }
done:
    asmtest_valtrace_free(masked);
    asmtest_valtrace_free(raw);
    return cont;
}

static void run_undef_flags_table(void) {
    size_t n = sizeof undef_rows / sizeof undef_rows[0];
    for (size_t i = 0; i < n; i++)
        if (!run_undef_row(&undef_rows[i], i == 0))
            return;
}

/* The xor-to-zero fixture item 1's own review names (loop_poly's comment dodges it
 * deliberately): block-step+replay is used (info.pure==1), the trace is byte-identical to
 * the single-step oracle, and AF — architecturally undefined for XOR — reads 0 in EVERY
 * post-xor EFLAGS record on BOTH paths, regardless of what either engine's arithmetic unit
 * actually computed for that bit. */
static void run_xor_zero_identity_case(void) {
    asmtest_valtrace_t *A = asmtest_valtrace_new(64, 512, 512);
    asmtest_valtrace_t *B = asmtest_valtrace_new(64, 512, 512);
    if (A == NULL || B == NULL) {
        CHECK(0, "xor-to-zero: valtrace_new");
        goto done;
    }
    {
        long ra = 0, rb = 0;
        asmtest_blockstep_info_t ia = {0}, ib = {0};
        asmtest_blockstep_opts_t oracle, block;
        memset(&oracle, 0, sizeof oracle);
        oracle.inject_block = -1;
        oracle.force_singlestep = 1;
        memset(&block, 0, sizeof block);
        block.inject_block = -1;

        int rca = asmtest_dataflow_blockstep_run(
            xor_to_zero, sizeof xor_to_zero, NULL, 0, &oracle, &ra, A, &ia);
        int rcb = asmtest_dataflow_blockstep_run(
            xor_to_zero, sizeof xor_to_zero, NULL, 0, &block, &rb, B, &ib);
        CHECK(rca == DF_BLOCKSTEP_OK && rcb == DF_BLOCKSTEP_OK,
              "xor-to-zero: capture succeeded on both paths (oracle rc=%d "
              "replay rc=%d)",
              rca, rcb);
        CHECK(ib.pure == 1,
              "xor-to-zero: block-step+replay path used (info.pure=%d)",
              ib.pure);
        CHECK(!A->truncated && !B->truncated,
              "xor-to-zero: neither trace truncated");

        normalize_rsp_relative(A, ia.entry_rsp);
        normalize_rsp_relative(B, ib.entry_rsp);
        int raw = 0;
        CHECK(traces_identical(A, B, &raw),
              "xor-to-zero: block-step+replay is BYTE-IDENTICAL to the "
              "single-step oracle");

        const at_val_rec_t *ra_rec = find_eflags_write(A);
        const at_val_rec_t *rb_rec = find_eflags_write(B);
        CHECK(ra_rec != NULL && rb_rec != NULL,
              "xor-to-zero: an EFLAGS write record exists on both paths");
        if (ra_rec != NULL && rb_rec != NULL)
            CHECK((ra_rec->value & EFLAG_AF) == 0 &&
                      (rb_rec->value & EFLAG_AF) == 0,
                  "xor-to-zero: AF masked to 0 in the post-xor EFLAGS record "
                  "on BOTH paths (oracle=0x%llx replay=0x%llx)",
                  (unsigned long long)ra_rec->value,
                  (unsigned long long)rb_rec->value);
    }
done:
    asmtest_valtrace_free(A);
    asmtest_valtrace_free(B);
}

/* Canary discrimination (bare metal — needs the block-step+replay path, gated behind the
 * caller's `probe == 1` check same as run_canary_case). Proves the MASK, not luck, is what
 * tolerates an undefined bit's divergence: AF (undefined for xor) is tolerated; ZF (defined)
 * is still caught; and with the mask forced off, the SAME AF flip is caught too. */
static void run_undef_canary_case(void) {
    long r = 0;
    asmtest_blockstep_opts_t o;
    asmtest_blockstep_info_t info;

    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    o.inject_flag_bit = EFLAG_AF;
    memset(&info, 0, sizeof info);
    int rc = asmtest_dataflow_blockstep_run(xor_to_zero, sizeof xor_to_zero,
                                            NULL, 0, &o, &r, v, &info);
    CHECK(rc == DF_BLOCKSTEP_OK && v != NULL && !v->truncated && info.pure == 1,
          "undef canary: AF flip after xor TOLERATED (undefined) -> not "
          "truncated (rc=%d truncated=%d pure=%d)",
          rc, v ? (int)v->truncated : -1, info.pure);
    asmtest_valtrace_free(v);

    v = asmtest_valtrace_new(64, 512, 512);
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    o.inject_flag_bit = EFLAG_ZF;
    memset(&info, 0, sizeof info);
    rc = asmtest_dataflow_blockstep_run(xor_to_zero, sizeof xor_to_zero, NULL,
                                        0, &o, &r, v, &info);
    CHECK(rc == DF_BLOCKSTEP_FAULT && v != NULL && v->truncated,
          "undef canary: ZF flip after xor DETECTED (defined) -> truncated "
          "(rc=%d truncated=%d)",
          rc, v ? (int)v->truncated : -1);
    asmtest_valtrace_free(v);

    v = asmtest_valtrace_new(64, 512, 512);
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    o.inject_flag_bit = EFLAG_AF;
    o.no_undef_mask = 1;
    memset(&info, 0, sizeof info);
    rc = asmtest_dataflow_blockstep_run(xor_to_zero, sizeof xor_to_zero, NULL,
                                        0, &o, &r, v, &info);
    CHECK(rc == DF_BLOCKSTEP_FAULT && v != NULL && v->truncated,
          "undef canary: no_undef_mask + AF flip DETECTED -> truncated "
          "(rc=%d truncated=%d) — the mask, not luck, tolerated the first "
          "case",
          rc, v ? (int)v->truncated : -1);
    asmtest_valtrace_free(v);
}

/* ------------------------------------------------------------------ */
/* Drivers                                                              */
/* ------------------------------------------------------------------ */

/* Exit criteria 1 + 2: one PURE fixture captured both ways. The block-step+replay trace,
 * normalized rsp-relative, is byte-identical to the single-step oracle; and its in-region stop
 * count is proportionally smaller. */
static void run_identity_case(const char *name, const uint8_t *code, size_t len,
                              const long *args, int nargs, long want_result) {
    asmtest_valtrace_t *A = asmtest_valtrace_new(4096, 65536, 4096);
    asmtest_valtrace_t *B = asmtest_valtrace_new(4096, 65536, 4096);
    if (A == NULL || B == NULL) {
        CHECK(0, "%s: valtrace_new", name);
        goto done;
    }

    long ra = 0, rb = 0;
    asmtest_blockstep_info_t ia = {0}, ib = {0};
    asmtest_blockstep_opts_t oracle, block;
    memset(&oracle, 0, sizeof oracle);
    oracle.inject_block = -1;
    oracle.force_singlestep = 1; /* the ground-truth oracle */
    memset(&block, 0, sizeof block);
    block.inject_block = -1; /* gated: pure + replayable -> replay */

    int rca = asmtest_dataflow_blockstep_run(code, len, args, nargs, &oracle,
                                             &ra, A, &ia);
    int rcb = asmtest_dataflow_blockstep_run(code, len, args, nargs, &block,
                                             &rb, B, &ib);
    if (rca != DF_BLOCKSTEP_OK || rcb != DF_BLOCKSTEP_OK) {
        CHECK(0, "%s: capture failed (single-step rc=%d, block-replay rc=%d)",
              name, rca, rcb);
        goto done;
    }
    CHECK(ra == want_result && rb == want_result,
          "%s: both paths returned %ld (oracle=%ld block=%ld)", name,
          want_result, ra, rb);
    CHECK(ib.pure == 1 && ia.pure == 0,
          "%s: pure method took the block-step+replay path (oracle "
          "single-stepped)",
          name);
    CHECK(!A->truncated && !B->truncated, "%s: neither trace truncated", name);

    normalize_rsp_relative(A, ia.entry_rsp);
    normalize_rsp_relative(B, ib.entry_rsp);

    int raw = 0;
    int same = traces_identical(A, B, &raw);
    CHECK(same,
          "%s: block-step+replay value trace is BYTE-IDENTICAL to single-step "
          "oracle (rsp-normalized; %zu steps, %zu records)",
          name, A->steps_len, A->recs_len);
    if (same)
        printf("#   raw memcmp of record arrays also identical: %s\n",
               raw ? "yes"
                   : "no (semantic fields identical; struct padding differs)");

    double ratio = ib.stops ? (double)ia.stops / (double)ib.stops : 0.0;
    CHECK(ib.stops < ia.stops,
          "%s: block-step CUT the in-region stop count %llu -> %llu (%.2fx "
          "fewer, ~mean block length)",
          name, (unsigned long long)ia.stops, (unsigned long long)ib.stops,
          ratio);
    printf("#   single-step stops = %llu; block-step stops = %llu; captured "
           "steps = %zu\n",
           (unsigned long long)ia.stops, (unsigned long long)ib.stops,
           A->steps_len);
done:
    asmtest_valtrace_free(A);
    asmtest_valtrace_free(B);
}

/* Exit criterion 3: an IMPURE method is detected by the static purity gate and single-stepped
 * (never replayed through the OS-interacting instruction), still yielding a correct trace. */
static void run_impure_case(void) {
    long args[1] = {0x1234};
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    if (v == NULL) {
        CHECK(0, "impure: valtrace_new");
        return;
    }
    long r = 0;
    asmtest_blockstep_info_t info = {0};
    asmtest_blockstep_opts_t opts;
    memset(&opts, 0, sizeof opts);
    opts.inject_block = -1; /* purity-gated */
    int rc = asmtest_dataflow_blockstep_run(imp_cpuid, sizeof imp_cpuid, args,
                                            1, &opts, &r, v, &info);
    CHECK(rc == DF_BLOCKSTEP_OK, "impure: capture completed (rc=%d)", rc);
    CHECK(info.pure == 0 && info.reason != NULL &&
              strcmp(info.reason, "cpuid") == 0,
          "impure: cpuid detected -> single-step fallback (reason=%s)",
          info.reason ? info.reason : "?");
    CHECK(r == 0x1234,
          "impure: single-stepped routine returned arg0 = 0x1234 (got 0x%lx)",
          r);
    CHECK(!v->truncated && v->steps_len == 4,
          "impure: complete 4-step trace over the fallback single-step path "
          "(steps=%zu)",
          v->steps_len);
    asmtest_valtrace_free(v);
}

/* Exit criterion 4: an injected replay-input divergence (simulating a sibling rewriting an
 * input between snapshot and execution) is DETECTED at the next real boundary and the capture
 * drops to truncated, never silently wrong. */
static void run_canary_case(void) {
    long args[1] = {50};
    asmtest_valtrace_t *v = asmtest_valtrace_new(4096, 65536, 4096);
    if (v == NULL) {
        CHECK(0, "canary: valtrace_new");
        return;
    }
    long r = 0;
    asmtest_blockstep_info_t info = {0};
    asmtest_blockstep_opts_t opts;
    memset(&opts, 0, sizeof opts);
    opts.inject_divergence = 1;
    opts.inject_block = 1; /* inject at interior block 1 */
    int rc = asmtest_dataflow_blockstep_run(loop_poly, sizeof loop_poly, args,
                                            1, &opts, &r, v, &info);
    CHECK(rc == DF_BLOCKSTEP_FAULT && v->truncated,
          "canary: injected replay-input divergence DETECTED -> truncated "
          "(rc=%d truncated=%d)",
          rc, (int)v->truncated);
    asmtest_valtrace_free(v);
}

/* ------------------------------------------------------------------ */
/* Vector cases (increment 2)                                           */
/* ------------------------------------------------------------------ */

/* Capture `code` both ways and report whether the traces agree. Returns 1 = identical,
 * 0 = DIFFER, -1 = a capture failed. Both captures are rsp-normalized first, exactly as
 * run_identity_case does, so a difference means a real VALUE divergence. */
static int vec_compare(const uint8_t *code, size_t len, uint64_t roff,
                       const long *args, int nargs,
                       const asmtest_blockstep_opts_t *block_opts, long *r_out,
                       int *truncated_out, int *rc_out,
                       asmtest_blockstep_info_t *info_out) {
    asmtest_valtrace_t *A = asmtest_valtrace_new(4096, 65536, 65536);
    asmtest_valtrace_t *B = asmtest_valtrace_new(4096, 65536, 65536);
    int verdict = -1;
    if (A == NULL || B == NULL)
        goto done;
    {
        asmtest_blockstep_opts_t oracle;
        memset(&oracle, 0, sizeof oracle);
        oracle.inject_block = -1;
        oracle.force_singlestep = 1;
        oracle.region_off = roff;

        asmtest_blockstep_opts_t blk = *block_opts;
        blk.region_off = roff;

        long ra = 0, rb = 0;
        asmtest_blockstep_info_t ia, ib;
        memset(&ia, 0, sizeof ia);
        memset(&ib, 0, sizeof ib);
        int rca = asmtest_dataflow_blockstep_run(code, len, args, nargs,
                                                 &oracle, &ra, A, &ia);
        int rcb = asmtest_dataflow_blockstep_run(code, len, args, nargs, &blk,
                                                 &rb, B, &ib);
        if (rc_out)
            *rc_out = rcb;
        if (r_out)
            *r_out = rb;
        if (truncated_out)
            *truncated_out = (int)B->truncated;
        if (info_out)
            *info_out = ib;
        if (rca != DF_BLOCKSTEP_OK) {
            printf("#   oracle capture failed rc=%d\n", rca);
            goto done;
        }
        (void)rcb;
        normalize_rsp_relative(A, ia.entry_rsp);
        normalize_rsp_relative(B, ib.entry_rsp);
        int raw = 0;
        verdict = traces_identical(A, B, &raw) ? 1 : 0;
    }
done:
    asmtest_valtrace_free(A);
    asmtest_valtrace_free(B);
    return verdict;
}

/* Case 5 — XMM live-in seeding, proved by reproducing the bug first.
 *
 *   (a) FIXED:            seeded  -> byte-identical to the single-step oracle.
 *   (b) NEGATIVE CONTROL: no seed + no vector canary (== the pre-increment-2 tier)
 *                         -> rc=OK, NOT truncated, and the trace DIFFERS. Silently wrong.
 *   (c) CANARY:           no seed, canary ON -> the divergence is DETECTED (truncated).
 *
 * (b) is the load-bearing one: it proves this suite can FAIL, that the bug was real, and that
 * it was invisible to the GP-only canary. Without it, (a) passing would prove nothing. */
static void run_vec_seed_case(void) {
    long args[2] = {7, 5};
    asmtest_blockstep_opts_t o;
    long r = 0;
    int trunc = 0, rc = 0;
    asmtest_blockstep_info_t info;

    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    int same = vec_compare(sse_live_in, sizeof sse_live_in, SSE_LIVE_IN_ROFF,
                           args, 2, &o, &r, &trunc, &rc, &info);
    CHECK(same == 1 && rc == DF_BLOCKSTEP_OK && !trunc,
          "vec-seed: SSE region with LIVE-IN xmm0/xmm1 replays "
          "BYTE-IDENTICALLY to the "
          "single-step oracle (same=%d rc=%d truncated=%d)",
          same, rc, trunc);
    CHECK(info.pure == 1 && info.vec_seeded == 16,
          "vec-seed: took the replay path with all 16 XMM regs seeded AND "
          "verified by "
          "read-back (pure=%d vec_seeded=%d)",
          info.pure, info.vec_seeded);
    CHECK(r == 7, "vec-seed: replayed region returned arg0 = 7 (got %ld)", r);

    /* (b) NEGATIVE CONTROL — restore the pre-fix behaviour and prove it is SILENTLY wrong. */
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    o.no_vec_seed = 1;
    o.no_vec_canary = 1;
    same = vec_compare(sse_live_in, sizeof sse_live_in, SSE_LIVE_IN_ROFF, args,
                       2, &o, &r, &trunc, &rc, &info);
    CHECK(same == 0 && rc == DF_BLOCKSTEP_OK && !trunc,
          "vec-seed NEGATIVE CONTROL: WITHOUT vector seeding the replay is "
          "SILENTLY WRONG "
          "— trace differs from the oracle yet rc=OK and truncated=0 "
          "(differs=%d rc=%d "
          "truncated=%d)",
          same == 0, rc, trunc);

    /* (c) The vector canary catches exactly what (b) showed slipping through. */
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    o.no_vec_seed = 1;
    (void)vec_compare(sse_live_in, sizeof sse_live_in, SSE_LIVE_IN_ROFF, args,
                      2, &o, &r, &trunc, &rc, &info);
    CHECK(rc == DF_BLOCKSTEP_FAULT && trunc,
          "vec-seed CANARY: the same unseeded divergence is DETECTED by the "
          "vector canary "
          "-> truncated (rc=%d truncated=%d)",
          rc, trunc);
}

/* Case 5b — the PARTIALLY-MAPPED STACK WINDOW, made deterministic.
 *
 * A pre-existing increment-1 bug, found because this lane finally RUNS on bare metal (it
 * self-skips on GitHub's BTF-masked runners, so nothing ever exercised it): the replay's stack
 * window can extend past the top of the tracee's [stack] VMA, process_vm_readv is atomic per
 * iovec and so fails for the WHOLE window, and the old code "recovered" by zeroing it — after
 * which the replayed `ret` popped 0 and the capture died UC_ERR_FETCH_UNMAPPED. It fired on
 * ~27% of runs in the dataflow-attach container and 0% on the host, i.e. it was a coin-flip on
 * stack layout, not something a passing run could be trusted to cover.
 *
 * stack_hi_pad forces the window 2 MiB above rsp, which is unconditionally past the stack top,
 * so the case is reproduced ON DEMAND rather than by luck. With the per-page reader the mapped
 * pages — including the one holding the return address — are still recovered and the capture is
 * byte-identical to the oracle. Reverting mr_tracee_window to a single all-or-nothing
 * process_vm_readv makes this case fail every time. */
static void run_stack_window_case(void) {
    long args[1] = {50};
    asmtest_blockstep_opts_t o;
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    o.stack_hi_pad =
        0x200000; /* 2 MiB above rsp: certainly past the [stack] VMA top */
    long r = 0;
    int trunc = 0, rc = 0;
    asmtest_blockstep_info_t info;
    int same = vec_compare(loop_poly, sizeof loop_poly, 0, args, 1, &o, &r,
                           &trunc, &rc, &info);
    CHECK(same == 1 && rc == DF_BLOCKSTEP_OK && !trunc && r == 3725,
          "stack-window: a replay window running 2MiB PAST the top of the "
          "tracee's [stack] "
          "VMA still replays byte-identically — the per-page reader recovers "
          "the mapped "
          "pages instead of zeroing the whole window (same=%d rc=%d "
          "truncated=%d r=%ld)",
          same, rc, trunc, r);
}

/* Case 5c — MXCSR seeding (HIGH 2). The XMM file is only HALF the state an SSE instruction
 * reads: MXCSR's rounding-control bits are inputs to every FP result. The region commits its
 * quotient to memory and then clears xmm0, so the register canary is blind — exactly the shape
 * that makes a wrong value SILENT rather than caught.
 *   (a) FIXED:            MXCSR seeded -> byte-identical to the oracle.
 *   (b) NEGATIVE CONTROL: no_vec_seed + no_vec_canary -> rc=OK, NOT truncated, trace DIFFERS
 *                         (oracle 0x3fc9999999999999 RZ vs replay 0x3fc999999999999a RN).
 * Seeding MXCSR is only legitimate because Unicorn HONOURS it — verified against silicon under
 * both RN and RZ. Had it merely stored the value, the honest move would have been to gate FP
 * regions off the replay rather than lie about them. */
static void run_mxcsr_case(void) {
    /* 1.0 and 5.0 as raw doubles; 1.0/5.0 is inexact, so the rounding mode is observable. */
    long args[2] = {(long)0x3FF0000000000000LL, (long)0x4014000000000000LL};
    asmtest_blockstep_opts_t o;
    long r = 0;
    int trunc = 0, rc = 0;
    asmtest_blockstep_info_t info;

    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    int same = vec_compare(fp_round, sizeof fp_round, FP_ROUND_ROFF, args, 2,
                           &o, &r, &trunc, &rc, &info);
    CHECK(same == 1 && rc == DF_BLOCKSTEP_OK && !trunc && info.pure == 1,
          "mxcsr: an FP region running under RC=toward-zero replays "
          "BYTE-IDENTICALLY to the "
          "oracle (same=%d rc=%d truncated=%d pure=%d)",
          same, rc, trunc, info.pure);
    CHECK(info.mxcsr_seeded == 1,
          "mxcsr: MXCSR seeded into the replay AND verified by read-back "
          "(mxcsr_seeded=%d)",
          info.mxcsr_seeded);

    /* NEGATIVE CONTROL — isolate MXCSR. It must be `no_mxcsr_seed`, NOT `no_vec_seed`: with the
     * whole vector seed off, xmm0/xmm1 are ZERO and the region computes 0.0/0.0 = NaN, so the
     * trace would differ for the XMM reason and the control would pass while proving nothing
     * about rounding. Here the XMM file IS seeded correctly and ONLY the rounding mode is
     * wrong, so the divergence can have exactly one cause. */
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    o.no_mxcsr_seed = 1;
    o.no_vec_canary = 1;
    same = vec_compare(fp_round, sizeof fp_round, FP_ROUND_ROFF, args, 2, &o,
                       &r, &trunc, &rc, &info);
    CHECK(same == 0 && rc == DF_BLOCKSTEP_OK && !trunc &&
              info.vec_seeded == 16 && info.mxcsr_seeded == 0,
          "mxcsr NEGATIVE CONTROL: with the XMM file correctly seeded but "
          "MXCSR left at "
          "Unicorn's default, the replay rounds to NEAREST while the cpu "
          "rounds toward ZERO — "
          "silently wrong at rc=OK truncated=0 (differs=%d xmm_seeded=%d "
          "mxcsr_seeded=%d)",
          same == 0, info.vec_seeded, info.mxcsr_seeded);

    /* ...and the MXCSR half of the canary catches exactly what the control showed slipping. */
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    o.no_mxcsr_seed = 1;
    (void)vec_compare(fp_round, sizeof fp_round, FP_ROUND_ROFF, args, 2, &o, &r,
                      &trunc, &rc, &info);
    CHECK(rc == DF_BLOCKSTEP_FAULT && trunc,
          "mxcsr CANARY: the same rounding-mode divergence is DETECTED (MXCSR "
          "control bits) -> "
          "truncated (rc=%d truncated=%d)",
          rc, trunc);
}

/* Case 5d — region_scan FAILS CLOSED on decoder desync (HIGH 1). A constant-pool island makes
 * the linear sweep swallow a VEX prefix as immediate data and desync; the VEX-128 that follows
 * is never classified. The scan must therefore refuse to vouch for bytes it never decoded:
 * replayable=0 ("decode") AND touches_vec=1.
 *   (a) FIXED:            gated to single-step, trace correct.
 *   (b) NEGATIVE CONTROL: force_replay past the gate -> the unseen VEX-128 is mis-executed and
 *                         the trace DIFFERS. That is what failing closed buys. */
static void run_desync_case(void) {
    const char *why = NULL;
    CHECK(asmtest_dataflow_blockstep_is_replayable(
              island + ISLAND_ROFF, sizeof island - ISLAND_ROFF, &why) == 0 &&
              why != NULL && strcmp(why, "decode") == 0,
          "desync: a region the sweep cannot fully decode is NOT vouched for — "
          "replayable=0 "
          "(reason=%s), not the optimistic initial verdict",
          why ? why : "(null)");

    long args[2] = {7, 5};
    asmtest_blockstep_opts_t o;
    long r = 0;
    int trunc = 0, rc = 0;
    asmtest_blockstep_info_t info;

    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    int same = vec_compare(island, sizeof island, ISLAND_ROFF, args, 2, &o, &r,
                           &trunc, &rc, &info);
    CHECK(same == 1 && rc == DF_BLOCKSTEP_OK && !trunc && info.pure == 0 &&
              info.reason != NULL && strcmp(info.reason, "decode") == 0,
          "desync: gated to single-step -> CORRECT trace over a region "
          "carrying an "
          "undecodable island (same=%d rc=%d pure=%d reason=%s)",
          same, rc, info.pure, info.reason ? info.reason : "(null)");

    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    o.force_replay = 1;
    o.no_vec_canary = 1;
    same = vec_compare(island, sizeof island, ISLAND_ROFF, args, 2, &o, &r,
                       &trunc, &rc, &info);
    CHECK(same == 0,
          "desync NEGATIVE CONTROL: forcing the replay over the un-decoded "
          "bytes IS wrong — "
          "the VEX-128 the sweep never saw gets mis-executed (differs=%d rc=%d "
          "truncated=%d)",
          same == 0, rc, trunc);
}

/* Case 5e — the impurity verdict must NOT truncate the sweep (HIGH 3). An impure region that
 * also touches vector state is ordinary JIT'd code. Purity is settled at the cpuid; touches_vec
 * is not, and getting it wrong silently strips the XSTATE read from the single-step fallback —
 * the path this change routes ALL AVX/AVX-512 code to. Asserts the vector VALUES really land. */
static void run_impure_vector_case(void) {
    long args[2] = {7, 5};
    asmtest_valtrace_t *v = asmtest_valtrace_new(4096, 65536, 65536);
    if (v == NULL) {
        CHECK(0, "impure-vec: valtrace_new");
        return;
    }
    asmtest_blockstep_opts_t o;
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    o.region_off = IMP_VEC_ROFF;
    long r = 0;
    asmtest_blockstep_info_t info;
    memset(&info, 0, sizeof info);
    int rc = asmtest_dataflow_blockstep_run(imp_vec, sizeof imp_vec, args, 2,
                                            &o, &r, v, &info);
    CHECK(rc == DF_BLOCKSTEP_OK && !v->truncated && info.pure == 0 &&
              info.reason != NULL && strcmp(info.reason, "cpuid") == 0 &&
              r == 7,
          "impure-vec: impure+vector region single-stepped, complete trace "
          "(rc=%d pure=%d "
          "reason=%s r=%ld)",
          rc, info.pure, info.reason ? info.reason : "?", r);

    /* The deliverable: the vector instructions AFTER the cpuid still get real values. Count XMM
     * register records and how many carry a value; the two must agree. */
    int xmm_recs = 0, xmm_valued = 0, store_ok = 0;
    for (size_t i = 0; i < v->recs_len; i++) {
        const at_val_rec_t *rec = &v->recs[i];
        if (rec->kind == AT_LOC_REG && rec->size == 16) {
            xmm_recs++;
            if (rec->value_valid && rec->wide)
                xmm_valued++;
        }
        if (rec->kind != AT_LOC_REG && rec->is_write && rec->size == 16 &&
            rec->value_valid && rec->wide &&
            (size_t)rec->wide_off + 16 <= v->wide_len) {
            uint64_t lo;
            memcpy(&lo, v->wide + rec->wide_off, 8);
            if (lo == 12) /* xmm0 = a + b */
                store_ok = 1;
        }
    }
    CHECK(xmm_recs > 0 && xmm_valued == xmm_recs && store_ok,
          "impure-vec: every XMM record AFTER the cpuid carries a REAL value "
          "and the store is "
          "a+b=12 — the sweep kept classifying past the impurity verdict "
          "(xmm_recs=%d "
          "valued=%d store_ok=%d)",
          xmm_recs, xmm_valued, store_ok);
    asmtest_valtrace_free(v);
}

/* Case 6 — VEX-128 is a silent liar, and the encoding gate is what stops it. */
static void run_vex128_case(void) {
    const char *reason = NULL;
    CHECK(asmtest_dataflow_blockstep_is_replayable(
              vex128_liar + VEX128_LIAR_ROFF,
              sizeof vex128_liar - VEX128_LIAR_ROFF, &reason) == 0 &&
              reason != NULL && strcmp(reason, "vex/evex") == 0,
          "vex128: region carrying a VEX-128 encoding is classified "
          "NON-REPLAYABLE "
          "(reason=%s)",
          reason ? reason : "?");

    long args[2] = {7, 5};
    asmtest_blockstep_opts_t o;
    long r = 0;
    int trunc = 0, rc = 0;
    asmtest_blockstep_info_t info;

    /* Gated (the production path): single-stepped, correct. */
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    int same = vec_compare(vex128_liar, sizeof vex128_liar, VEX128_LIAR_ROFF,
                           args, 2, &o, &r, &trunc, &rc, &info);
    CHECK(same == 1 && rc == DF_BLOCKSTEP_OK && !trunc && info.pure == 0 &&
              info.reason != NULL && strcmp(info.reason, "vex/evex") == 0,
          "vex128: gated off the replay -> single-stepped to a CORRECT trace "
          "(same=%d "
          "rc=%d pure=%d reason=%s)",
          same, rc, info.pure, info.reason ? info.reason : "?");

    /* NEGATIVE CONTROL — force the replay the gate refuses and watch Unicorn lie: it returns
     * UC_ERR_OK having executed the legacy 2-operand form, so the trace diverges with no
     * fault at all. This is what the gate buys, and why it keys on the ENCODING. */
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    o.force_replay = 1;
    o.no_vec_canary = 1;
    same = vec_compare(vex128_liar, sizeof vex128_liar, VEX128_LIAR_ROFF, args,
                       2, &o, &r, &trunc, &rc, &info);
    CHECK(same == 0 && rc == DF_BLOCKSTEP_OK && !trunc,
          "vex128 NEGATIVE CONTROL: forcing the replay past the gate is "
          "SILENTLY WRONG — "
          "Unicorn drops VEX.vvvv, returns UC_ERR_OK, and the trace differs "
          "(differs=%d "
          "rc=%d truncated=%d)",
          same == 0, rc, trunc);
}

/* Case 7 — an AVX (VEX-256) / AVX-512 (EVEX) region: gated to single-step, CORRECT, and
 * carrying REAL full-width vector values read off silicon. Hardware-gated by `need_width`. */
static void run_avx_case(const char *name, const uint8_t *code, size_t len,
                         uint64_t roff, int need_width, int val_width) {
    int hw = asmtest_dataflow_blockstep_vec_width(NULL);
    if (hw < need_width) {
        printf("# SKIP %s: cpu exposes %d-byte vector state, need %d (hardware "
               "gate)\n",
               name, hw, need_width);
        return;
    }
    const char *reason = NULL;
    CHECK(asmtest_dataflow_blockstep_is_replayable(code + roff, len - roff,
                                                   &reason) == 0 &&
              reason != NULL && strcmp(reason, "vex/evex") == 0,
          "%s: region is classified NON-REPLAYABLE (reason=%s)", name,
          reason ? reason : "?");

    long args[1] = {7};
    asmtest_valtrace_t *v = asmtest_valtrace_new(4096, 65536, 65536);
    if (v == NULL) {
        CHECK(0, "%s: valtrace_new", name);
        return;
    }
    asmtest_blockstep_opts_t o;
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    o.region_off = roff;
    long r = 0;
    asmtest_blockstep_info_t info;
    memset(&info, 0, sizeof info);
    int rc =
        asmtest_dataflow_blockstep_run(code, len, args, 1, &o, &r, v, &info);
    CHECK(rc == DF_BLOCKSTEP_OK && !v->truncated && info.pure == 0 &&
              info.reason != NULL && strcmp(info.reason, "vex/evex") == 0,
          "%s: gated to the single-step fallback, complete trace (rc=%d "
          "truncated=%d "
          "pure=%d reason=%s)",
          name, rc, (int)v->truncated, info.pure,
          info.reason ? info.reason : "?");
    CHECK(r == 14, "%s: single-stepped region returned 2*arg0 = 14 (got %ld)",
          name, r);

    /* The deliverable: REAL vector values, at full architectural width, in the trace. The
     * region doubles a broadcast of arg0, so after `vpaddq` every 64-bit lane of the vector
     * register holds 14. The region's only val_width-wide REGISTER write is that vpaddq
     * destination, so keying on the width identifies it without dragging Capstone's reg-id
     * enum into this suite. `found` and `matched` are reported separately so a missing record
     * cannot be confused with a wrong one. */
    int found = 0, matched = 0;
    for (size_t i = 0; i < v->recs_len; i++) {
        const at_val_rec_t *rec = &v->recs[i];
        if (rec->kind != AT_LOC_REG || !rec->is_write ||
            rec->size != (uint16_t)val_width)
            continue;
        found = 1;
        if (!rec->wide || !rec->value_valid)
            continue;
        if ((size_t)rec->wide_off + (size_t)val_width > v->wide_len)
            continue;
        const uint8_t *b = v->wide + rec->wide_off;
        int all = 1;
        for (int lane = 0; lane < val_width / 8; lane++) {
            uint64_t q;
            memcpy(&q, b + lane * 8, 8);
            if (q != 14)
                all = 0;
        }
        if (all)
            matched = 1;
    }
    CHECK(found && matched,
          "%s: the trace carries the REAL %d-bit vector value from silicon — "
          "every lane of "
          "the %d-byte wide[] record is 2*arg0 = 14 (found=%d matched=%d)",
          name, val_width * 8, val_width, found, matched);

    /* NEGATIVE CONTROL — force the replay past the gate. No released Unicorn executes
     * VEX-256/EVEX, so this must FAULT + truncate. It must NOT return the ETRACE self-skip
     * code (which would read as "no ptrace here" and quietly pass the lane), and it must not
     * return a clean trace. */
    asmtest_valtrace_t *w = asmtest_valtrace_new(4096, 65536, 65536);
    if (w != NULL) {
        asmtest_blockstep_opts_t f;
        memset(&f, 0, sizeof f);
        f.inject_block = -1;
        f.region_off = roff;
        f.force_replay = 1;
        long r2 = 0;
        asmtest_blockstep_info_t i2;
        memset(&i2, 0, sizeof i2);
        int rc2 =
            asmtest_dataflow_blockstep_run(code, len, args, 1, &f, &r2, w, &i2);
        CHECK(rc2 == DF_BLOCKSTEP_FAULT && w->truncated,
              "%s NEGATIVE CONTROL: forcing the replay FAULTS + truncates "
              "(UC_ERR_INSN_INVALID) — never a silent trace, never the ETRACE "
              "self-skip "
              "(rc=%d truncated=%d)",
              name, rc2, (int)w->truncated);
        asmtest_valtrace_free(w);
    }
    asmtest_valtrace_free(v);
}

/* Case 8 — the ENCODING gate is LOAD-BEARING, not merely argued (MEDIUM 5).
 *
 * This tier's central design claim is that the replayability gate must key on the ENCODING and
 * NOT on Capstone's AVX metadata. Until these checks existed the claim was unfalsifiable by this
 * suite: every scanned region happened to contain an instruction Capstone DOES put in an AVX
 * group, so swapping insn_is_vex_evex() for a X86_GRP_AVX/AVX2/AVX512 check passed all 47
 * checks — the suite could not tell the design apart from the one it rejects. (The instruction
 * that proves the gap, `vpbroadcastq zmm0,xmm0`, sat in the entry glue, which region_scan never
 * scans.) These two regions are gated ONLY by the encoding rule, so the metadata gate FAILS
 * them:
 *   - vex_bmi:    `andn` is VEX-encoded, GP-only, in no AVX group. No hardware gate, so this
 *                 pins the rule on every x86-64 box. Also pins the deliberate OVER-gate.
 *   - evex_invis: every region instruction is metadata-invisible, so a metadata gate would hand
 *                 a real EVEX instruction to an emulator with no decoder for it. AVX-512-gated. */
static void run_encoding_gate_case(void) {
    const char *why = NULL;
    CHECK(asmtest_dataflow_blockstep_is_replayable(
              vex_bmi + VEX_BMI_ROFF, sizeof vex_bmi - VEX_BMI_ROFF, &why) ==
                  0 &&
              why != NULL && strcmp(why, "vex/evex") == 0,
          "encoding-gate: VEX-GP (`andn`, in NO Capstone AVX group) is gated "
          "by the ENCODING "
          "rule — a metadata gate would call it replayable (reason=%s)",
          why ? why : "(null)");

    long args[1] = {7};
    asmtest_blockstep_opts_t o;
    long r = 0;
    int trunc = 0, rc = 0;
    asmtest_blockstep_info_t info;
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    int same = vec_compare(vex_bmi, sizeof vex_bmi, VEX_BMI_ROFF, args, 1, &o,
                           &r, &trunc, &rc, &info);
    CHECK(same == 1 && rc == DF_BLOCKSTEP_OK && info.pure == 0 &&
              info.reason != NULL && strcmp(info.reason, "vex/evex") == 0,
          "encoding-gate: the over-gated VEX-GP region single-steps to a "
          "correct trace — "
          "over-gating costs only the perturbation win (same=%d rc=%d pure=%d "
          "reason=%s)",
          same, rc, info.pure, info.reason ? info.reason : "?");

    int hw = asmtest_dataflow_blockstep_vec_width(NULL);
    if (hw < 64) {
        printf("# SKIP encoding-gate/evex_invis: cpu exposes %d-byte vector "
               "state, need 64 "
               "(hardware gate)\n",
               hw);
        return;
    }
    why = NULL;
    CHECK(asmtest_dataflow_blockstep_is_replayable(
              evex_invis + EVEX_INVIS_ROFF, sizeof evex_invis - EVEX_INVIS_ROFF,
              &why) == 0 &&
              why != NULL && strcmp(why, "vex/evex") == 0,
          "encoding-gate: a region whose EVERY instruction is invisible to "
          "Capstone's AVX "
          "metadata is still gated by the ENCODING rule (reason=%s)",
          why ? why : "(null)");

    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    same = vec_compare(evex_invis, sizeof evex_invis, EVEX_INVIS_ROFF, args, 1,
                       &o, &r, &trunc, &rc, &info);
    CHECK(same == 1 && rc == DF_BLOCKSTEP_OK && info.pure == 0 && r == 7,
          "encoding-gate: the metadata-invisible EVEX region single-steps to a "
          "correct trace "
          "(same=%d rc=%d pure=%d r=%ld)",
          same, rc, info.pure, r);

    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    o.force_replay = 1;
    asmtest_valtrace_t *w = asmtest_valtrace_new(4096, 65536, 65536);
    if (w != NULL) {
        long r2 = 0;
        asmtest_blockstep_info_t i2;
        memset(&i2, 0, sizeof i2);
        o.region_off = EVEX_INVIS_ROFF;
        int rc2 = asmtest_dataflow_blockstep_run(evex_invis, sizeof evex_invis,
                                                 args, 1, &o, &r2, w, &i2);
        CHECK(rc2 == DF_BLOCKSTEP_FAULT && w->truncated,
              "encoding-gate NEGATIVE CONTROL: what a metadata gate would have "
              "allowed — "
              "forcing the EVEX replay FAULTS + truncates (rc=%d truncated=%d)",
              rc2, (int)w->truncated);
        asmtest_valtrace_free(w);
    }
}

/* Case 9 — the PUBLIC is_replayable() must not let purity mask its answer (MEDIUM 4). */
static void run_is_replayable_independence_case(void) {
    const char *why = NULL;
    int rep =
        asmtest_dataflow_blockstep_is_replayable(imp_vex, sizeof imp_vex, &why);
    CHECK(rep == 0 && why != NULL && strcmp(why, "vex/evex") == 0,
          "is_replayable: `cpuid; vpaddq xmm0,xmm1,xmm2; ret` is NOT "
          "replayable (reason=%s) — "
          "the impurity verdict must not mask a VEX-128 from a PUBLIC caller",
          why ? why : "(null)");
    const char *pw = NULL;
    CHECK(asmtest_dataflow_blockstep_is_pure(imp_vex, sizeof imp_vex, &pw) ==
                  0 &&
              pw != NULL && strcmp(pw, "cpuid") == 0,
          "is_replayable: ...while is_pure() on the same region still names "
          "the impurity "
          "(reason=%s) — the two verdicts are independent",
          pw ? pw : "(null)");
}

/* T5 (F2 increment 2, forward pass) — THE LOAD-BEARING NEW CHECK: region_scan's static sweep
 * finds the right rdtsc/rdtscp/rdrand/rdseed/cpuid sites (offsets, count) and honors the
 * 4-slot architectural DR cap, exposed test-side via asmtest_dataflow_blockstep_scan_hwrec.
 * Capstone-only (no ptrace), so it runs on every VM lane, same as the is_injectable checks
 * above. */
static void run_hwrec_scan_case(void) {
    uint64_t off[8];
    int overflow = -1;

    memset(off, 0xAA, sizeof off);
    int n = asmtest_dataflow_blockstep_scan_hwrec(
        hwrec_multi, sizeof hwrec_multi, off, 8, &overflow);
    CHECK(n == 3 && overflow == 0 && off[0] == 0 && off[1] == 2 && off[2] == 4,
          "scan_hwrec: hwrec_multi (rdtsc@0, cpuid@2, rdrand@4) -> n=%d "
          "overflow=%d offs={%llu,%llu,%llu}",
          n, overflow, (unsigned long long)off[0], (unsigned long long)off[1],
          (unsigned long long)off[2]);

    memset(off, 0xAA, sizeof off);
    overflow = -1;
    int n5 = asmtest_dataflow_blockstep_scan_hwrec(
        hwrec_5site, sizeof hwrec_5site, off, 8, &overflow);
    CHECK(n5 == 4 && overflow == 1 && off[0] == 0 && off[1] == 2 &&
              off[2] == 5 && off[3] == 7,
          "scan_hwrec: hwrec_5site (5 DISTINCT sites) caps at n=%d (want 4) "
          "with overflow=%d (want 1) — the architectural DR0-3 slot count, "
          "honestly reported rather than silently dropping the 5th",
          n5, overflow);
}

/* Case 10 — Hi16_ZMM / zmm16-31 reassembly (LOW 7). Component 7 is the only source for these
 * registers; nothing else in the suite reaches that path, and silently zeroing a register file
 * is the failure mode this whole increment exists to kill. */
static void run_hi16_zmm_case(void) {
    int hw = asmtest_dataflow_blockstep_vec_width(NULL);
    if (hw < 64) {
        printf("# SKIP hi16_zmm: cpu exposes %d-byte vector state, need 64 "
               "(hardware gate)\n",
               hw);
        return;
    }
    long args[1] = {7};
    asmtest_valtrace_t *v = asmtest_valtrace_new(4096, 65536, 65536);
    if (v == NULL) {
        CHECK(0, "hi16_zmm: valtrace_new");
        return;
    }
    asmtest_blockstep_opts_t o;
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    o.region_off = HI16_ZMM_ROFF;
    long r = 0;
    asmtest_blockstep_info_t info;
    memset(&info, 0, sizeof info);
    int rc = asmtest_dataflow_blockstep_run(hi16_zmm, sizeof hi16_zmm, args, 1,
                                            &o, &r, v, &info);
    CHECK(rc == DF_BLOCKSTEP_OK && !v->truncated && r == 14,
          "hi16_zmm: region computing through zmm16 single-steps to a complete "
          "trace, "
          "returned 2*arg0 = 14 (rc=%d truncated=%d r=%ld)",
          rc, (int)v->truncated, r);

    int found = 0, matched = 0;
    for (size_t i = 0; i < v->recs_len; i++) {
        const at_val_rec_t *rec = &v->recs[i];
        if (rec->kind != AT_LOC_REG || !rec->is_write || rec->size != 64)
            continue;
        found = 1;
        if (!rec->wide || !rec->value_valid ||
            (size_t)rec->wide_off + 64 > v->wide_len)
            continue;
        const uint8_t *b = v->wide + rec->wide_off;
        int all = 1;
        for (int lane = 0; lane < 8; lane++) {
            uint64_t q;
            memcpy(&q, b + lane * 8, 8);
            if (q != 14)
                all = 0;
        }
        if (all)
            matched = 1;
    }
    CHECK(found && matched,
          "hi16_zmm: zmm16's REAL 512-bit value is reassembled from XSAVE "
          "component 7 — every "
          "lane is 2*arg0 = 14 (found=%d matched=%d)",
          found, matched);
    asmtest_valtrace_free(v);
}

/* The purity static-scan classification, run whenever the real (non-stub) producer is built —
 * it needs only Capstone, not ptrace/SINGLEBLOCK, so it reports even on a locked-down host. */
/* ------------------------------------------------------------------ */
/* F2 drivers — record-and-inject                                       */
/* ------------------------------------------------------------------ */

/* Capstone's register id for RAX, DERIVED rather than hardcoded: the suite links no Capstone
 * header, and pinning the literal 35 would silently rot the day the pinned Capstone renumbers
 * its enum. asmtest_operands is in the shared valtrace header, so asking it what `add rax, rax`
 * writes gets the answer from the very decoder that produced the records under test. */
static uint32_t reg_rax_id(void) {
    static const uint8_t add_rax_rax[] = {0x48, 0x01, 0xc0};
    at_val_rec_t rd[8], wr[8];
    size_t nr = 8, nw = 8;
    asmtest_operands(ASMTEST_ARCH_X86_64, add_rax_rax, sizeof add_rax_rax, 0,
                     rd, &nr, wr, &nw);
    /* The READ set, deliberately: `add rax, rax` reads rax and NOTHING else, whereas its WRITE
     * set is {EFLAGS, RAX} in that order — taking the first write returns EFLAGS. */
    if (nr == 1 && rd[0].kind == AT_LOC_REG)
        return rd[0].reg;
    return 0;
}

/* The step index whose instruction offset is `off`, or -1. */
static long step_at_off(const asmtest_valtrace_t *v, uint64_t off) {
    for (size_t i = 0; i < v->steps_len; i++)
        if (v->insn_off[i] == off)
            return (long)i;
    return -1;
}

/* The value a given step WROTE to register `reg`, via *out. Returns 1 if such a record exists
 * and carries a value. */
static int step_write_value(const asmtest_valtrace_t *v, long step,
                            uint32_t reg, uint64_t *out) {
    for (size_t i = 0; i < v->recs_len; i++) {
        const at_val_rec_t *r = &v->recs[i];
        if ((long)r->step == step && r->is_write && r->kind == AT_LOC_REG &&
            r->reg == reg && r->value_valid) {
            *out = r->value;
            return 1;
        }
    }
    return 0;
}

/* Register-write records a step carries. */
static size_t step_nwrites(const asmtest_valtrace_t *v, long step) {
    size_t n = 0;
    for (size_t i = 0; i < v->recs_len; i++)
        if ((long)v->recs[i].step == step && v->recs[i].is_write &&
            v->recs[i].kind == AT_LOC_REG)
            n++;
    return n;
}

/* The F2 EXIT CRITERION driver, for a syscall whose result is DETERMINISTIC across the two
 * forked captures. Captures the same OS-interacting region both ways and demands the same bar
 * F1 set for pure methods: byte-identical to the single-step oracle, or gated.
 *
 * `want_nwrites` pins the syscall's architectural write set: 3 for `syscall` (rax/rcx/r11), 1
 * for `int 0x80` (rax only — an interrupt gate leaves rcx/r11 alone). Asserting the COUNT is
 * what stops the int-0x80 path quietly forging two register defs that never happened; the
 * byte-identical check cannot see that, because both paths would forge them identically. */
static void run_syscall_identity_case(const char *name, const uint8_t *code,
                                      size_t len, const long *args, int nargs,
                                      long want_result, uint64_t sc_off,
                                      long want_sc_rax, size_t want_nwrites) {
    asmtest_valtrace_t *A = asmtest_valtrace_new(4096, 65536, 4096);
    asmtest_valtrace_t *B = asmtest_valtrace_new(4096, 65536, 4096);
    if (A == NULL || B == NULL) {
        CHECK(0, "%s: valtrace_new", name);
        goto done;
    }
    long ra = 0, rb = 0;
    asmtest_blockstep_info_t ia = {0}, ib = {0};
    asmtest_blockstep_opts_t oracle, block;
    memset(&oracle, 0, sizeof oracle);
    oracle.inject_block = -1;
    oracle.force_singlestep = 1;
    memset(&block, 0, sizeof block);
    block.inject_block = -1;

    int rca = asmtest_dataflow_blockstep_run(code, len, args, nargs, &oracle,
                                             &ra, A, &ia);
    int rcb = asmtest_dataflow_blockstep_run(code, len, args, nargs, &block,
                                             &rb, B, &ib);
    if (rca != DF_BLOCKSTEP_OK || rcb != DF_BLOCKSTEP_OK) {
        CHECK(0, "%s: capture failed (single-step rc=%d, block-replay rc=%d)",
              name, rca, rcb);
        goto done;
    }
    CHECK(ra == want_result && rb == want_result,
          "%s: both paths returned the real kernel-derived result %ld "
          "(oracle=%ld block=%ld)",
          name, want_result, ra, rb);
    /* THE F2 HEADLINE: an OS-interacting region that F1 sent to the single-step fallback now
     * takes the block-step + replay path. `injected` is the only positive evidence of it —
     * `pure` just means "the replay was used", which force_replay could also produce. */
    CHECK(ib.pure == 1 && ib.injected > 0,
          "%s: IMPURE region took the block-step+replay path via "
          "record-and-inject (pure=%d injected=%llu)",
          name, ib.pure, (unsigned long long)ib.injected);
    CHECK(ia.pure == 0 && ia.injected == 0,
          "%s: the oracle single-stepped it (no injection on that path)", name);
    CHECK(!A->truncated && !B->truncated, "%s: neither trace truncated", name);

    normalize_rsp_relative(A, ia.entry_rsp);
    normalize_rsp_relative(B, ib.entry_rsp);
    int raw = 0;
    int same = traces_identical(A, B, &raw);
    CHECK(same,
          "%s: block-step+replay value trace is BYTE-IDENTICAL to the "
          "single-step oracle across the syscall (%zu steps, %zu records)",
          name, A->steps_len, A->recs_len);
    if (same)
        printf("#   raw memcmp of record arrays also identical: %s\n",
               raw ? "yes" : "no (semantic fields identical; padding differs)");

    /* The RECORD half of record-and-inject: the syscall's own step must carry the kernel's
     * result as a def. The shared operand enumerator reports `syscall` as touching NO registers
     * at all, so without the producer-local write set this step would be empty and the def-use
     * edge from the syscall to its consumer could not exist. */
    long s = step_at_off(B, sc_off);
    uint64_t v = 0;
    int got = (s >= 0) && step_write_value(B, s, reg_rax_id(), &v);
    CHECK(got && (long)v == want_sc_rax,
          "%s: the syscall step DEFINES rax = the kernel's real return %ld "
          "(got %lld) — the def-use edge to its consumer exists",
          name, want_sc_rax, (long long)v);
    CHECK(s >= 0 && step_nwrites(B, s) == want_nwrites,
          "%s: the syscall step's write set is exactly %zu register def(s) "
          "(got %zu) — no forged defs",
          name, want_nwrites, s >= 0 ? step_nwrites(B, s) : (size_t)0);

    CHECK(ib.stops < ia.stops,
          "%s: record-and-inject KEPT the perturbation win — in-region stops "
          "%llu -> %llu (the syscall boundary is free: BTF already trapped it)",
          name, (unsigned long long)ia.stops, (unsigned long long)ib.stops);
done:
    asmtest_valtrace_free(A);
    asmtest_valtrace_free(B);
}

/* THE MEMORY ORACLE — and the answer to "nothing in the canary compares memory".
 *
 * The coherence canary compares registers only, so if the kernel's buffer write reached the
 * replay WRONG, no canary would notice. This case supplies the missing witness WITHOUT needing
 * the syscall to be deterministic, by comparing two INDEPENDENT sources from the SAME capture:
 *
 *   - `result` is read from the REAL tracee's rax at the real region-exit boundary.
 *   - the trace's LOAD record value is read from UNICORN's memory during the replay.
 *
 * The region ends `mov rax, [buf]; ret`, so those two are the same datum via completely
 * different paths. They agree iff the replay's post-syscall memory matched reality. That works
 * for clock_gettime — whose value is different in every capture, so the byte-identical oracle
 * structurally cannot judge it — which is why the plan's own headline example is covered here
 * rather than above. `plausible` additionally pins the value to reality rather than merely to
 * self-consistency. */
static void run_syscall_mem_case(const char *name, const uint8_t *code,
                                 size_t len, const long *args, int nargs,
                                 uint64_t load_off, int (*plausible)(long),
                                 const char *plausible_desc) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(4096, 65536, 4096);
    if (v == NULL) {
        CHECK(0, "%s: valtrace_new", name);
        return;
    }
    long r = 0;
    asmtest_blockstep_info_t info = {0};
    asmtest_blockstep_opts_t o;
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    int rc = asmtest_dataflow_blockstep_run(code, len, args, nargs, &o, &r, v,
                                            &info);
    CHECK(rc == DF_BLOCKSTEP_OK && !v->truncated && info.pure == 1 &&
              info.injected == 1,
          "%s: replayed via record-and-inject (rc=%d truncated=%d pure=%d "
          "injected=%llu)",
          name, rc, (int)v->truncated, info.pure,
          (unsigned long long)info.injected);
    if (rc != DF_BLOCKSTEP_OK)
        goto done;

    CHECK(plausible(r),
          "%s: the region returned a REAL kernel value — %s (got %ld)", name,
          plausible_desc, r);

    /* The load AFTER the syscall: its recorded value came from Unicorn's memory, and must equal
     * what the real tracee actually returned. A replay reading its own stale pre-syscall
     * snapshot would report the sentinel the region stored there itself. */
    long s = step_at_off(v, load_off);
    uint64_t got = 0;
    int found = 0;
    for (size_t i = 0; s >= 0 && i < v->recs_len; i++) {
        const at_val_rec_t *rec = &v->recs[i];
        if ((long)rec->step == s && !rec->is_write && rec->kind != AT_LOC_REG &&
            rec->value_valid) {
            got = rec->value;
            found = 1;
            break;
        }
    }
    CHECK(found && (long)got == r,
          "%s: the replay's LOAD of the kernel-written buffer == the REAL "
          "tracee's returned value (%lld vs %ld) — the kernel's memory delta "
          "reached the replay",
          name, (long long)got, r);
    CHECK(found && got != SC_PREAD_SENTINEL,
          "%s: and it is NOT the sentinel the region stored pre-syscall "
          "(0x%llx) — the replay did not read its own stale snapshot",
          name, (unsigned long long)SC_PREAD_SENTINEL);
done:
    asmtest_valtrace_free(v);
}

/* NEGATIVE CONTROL for the INJECTION. no_syscall_inject reaches the syscall in the replay and
 * declines to carry the recorded effect — the pre-F2 behaviour, where Unicorn's rax still held
 * the syscall NUMBER. Must TRUNCATE, never produce a trace. */
static void run_no_inject_case(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(1024, 8192, 1024);
    if (v == NULL) {
        CHECK(0, "no-inject: valtrace_new");
        return;
    }
    long r = 0;
    asmtest_blockstep_info_t info = {0};
    asmtest_blockstep_opts_t o;
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    o.no_syscall_inject = 1;
    int rc = asmtest_dataflow_blockstep_run(sc_getppid, sizeof sc_getppid, NULL,
                                            0, &o, &r, v, &info);
    CHECK(rc == DF_BLOCKSTEP_FAULT && v->truncated && info.injected == 0,
          "no-inject NEGATIVE CONTROL: a replay that reaches a syscall and "
          "does NOT inject the recorded effect FAULTS + truncates (rc=%d "
          "truncated=%d injected=%llu) — the injection is load-bearing",
          rc, (int)v->truncated, (unsigned long long)info.injected);
    asmtest_valtrace_free(v);
}

/* NEGATIVE CONTROL for the PER-STEP DECODE — the check that stops the replay executing an
 * impure instruction, independently of region_scan.
 *
 * blind_rdtsc overwrites BOTH of rdtsc's outputs before the boundary, so the coherence canary
 * sees every register match and cannot help. force_replay pushes the region past the REGION
 * gate, exactly as a region_scan desync would (F1's `island` fixture proves a linear sweep can
 * miss an instruction entirely). What must stop it is step_block's own decode at the real pc.
 * Measured: Unicorn runs rdtsc and returns UC_ERR_OK with a fabricated counter, so without this
 * defence the capture would report rc=OK, truncated=0 and a made-up TSC in the trace. */
static void run_blind_rdtsc_case(void) {
    long args[1] = {0x1234};
    asmtest_valtrace_t *v = asmtest_valtrace_new(1024, 8192, 1024);
    if (v == NULL) {
        CHECK(0, "blind-rdtsc: valtrace_new");
        return;
    }
    /* First: it is correctly classified NOT injectable, and the reason names rdtsc. */
    const char *why = NULL;
    CHECK(asmtest_dataflow_blockstep_is_injectable(
              blind_rdtsc, sizeof blind_rdtsc, &why) == 0 &&
              why != NULL && strcmp(why, "rdtsc") == 0,
          "blind-rdtsc: rdtsc is NOT injectable — BTF does not trap it, so no "
          "boundary exists to record its retired value (reason=%s)",
          why ? why : "?");

    long r = 0;
    asmtest_blockstep_info_t info = {0};
    asmtest_blockstep_opts_t o;
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    o.force_replay = 1; /* simulate the region gate having missed it */
    int rc = asmtest_dataflow_blockstep_run(blind_rdtsc, sizeof blind_rdtsc,
                                            args, 1, &o, &r, v, &info);
    CHECK(rc == DF_BLOCKSTEP_FAULT && v->truncated,
          "blind-rdtsc NEGATIVE CONTROL: forced past the region gate — with "
          "the canary BLIND (both rdtsc outputs overwritten) — the per-step "
          "decode still refuses and TRUNCATES (rc=%d truncated=%d), rather "
          "than reporting Unicorn's fabricated TSC as a real value",
          rc, (int)v->truncated);
    asmtest_valtrace_free(v);

    /* And gated normally, it single-steps to a correct trace: over-gating costs only the win. */
    asmtest_valtrace_t *w = asmtest_valtrace_new(1024, 8192, 1024);
    if (w == NULL)
        return;
    long r2 = 0;
    asmtest_blockstep_info_t i2 = {0};
    asmtest_blockstep_opts_t o2;
    memset(&o2, 0, sizeof o2);
    o2.inject_block = -1;
    int rc2 = asmtest_dataflow_blockstep_run(blind_rdtsc, sizeof blind_rdtsc,
                                             args, 1, &o2, &r2, w, &i2);
    CHECK(rc2 == DF_BLOCKSTEP_OK && !w->truncated && i2.pure == 0 &&
              i2.reason != NULL && strcmp(i2.reason, "rdtsc") == 0 &&
              r2 == 0x1234,
          "blind-rdtsc: gated to the single-step fallback -> correct trace "
          "(rc=%d pure=%d reason=%s r=%ld)",
          rc2, i2.pure, i2.reason ? i2.reason : "?", r2);
    asmtest_valtrace_free(w);
}

/* T5 (F2 increment 2, forward pass) — THE LIVE EVIDENCE, on the Zen 5 box: the DR exec
 * breakpoint fires and info.hw_hits counts it, while the EXTERNALLY OBSERVABLE verdict stays
 * exactly what it was before this task lands (this task alone must not change verdicts — T6
 * is what lifts the truncation). Three sub-cases over hwrec_rdtsc (`rdtsc; ret`):
 *
 *   1. Gated normally (default opts): the region is still classified NOT injectable (T5 keeps
 *      DFB_IMP_HWREC out of `injectable`, exactly as DFB_IMP_OTHER always was), so it single-
 *      steps to a correct trace, UNCHANGED from before this task — and hw_hits stays 0, since
 *      capture_blockstep (the only place that arms a DR slot) never runs on this path.
 *   2. force_replay=1 (forced past the region gate, mirroring blind_rdtsc's own technique):
 *      capture_blockstep now arms DR0 at the rdtsc site, takes the boundary, and hw_hits
 *      becomes >= 1 — but step_block's DFB_IMP_HWREC case still refuses to execute rdtsc in
 *      the replay (T6 not landed), so the capture still FAULTS + truncates, exactly as it did
 *      when this was undifferentiated DFB_IMP_OTHER. Same verdict, new telemetry.
 *   3. force_singlestep=1 (the explicit Done-when check): hw_hits MUST stay 0 — no arming on
 *      the fallback path, because force_singlestep routes to capture_singlestep, which never
 *      calls capture_blockstep at all. */
static void run_hwrec_forward_case(void) {
    asmtest_valtrace_t *v1 = asmtest_valtrace_new(256, 4096, 256);
    if (v1 == NULL) {
        CHECK(0, "hwrec-forward: valtrace_new (gated)");
        return;
    }
    long r1 = 0;
    asmtest_blockstep_info_t i1 = {0};
    asmtest_blockstep_opts_t o1;
    memset(&o1, 0, sizeof o1);
    o1.inject_block = -1;
    int rc1 = asmtest_dataflow_blockstep_run(hwrec_rdtsc, sizeof hwrec_rdtsc,
                                             NULL, 0, &o1, &r1, v1, &i1);
    CHECK(rc1 == DF_BLOCKSTEP_OK && !v1->truncated && i1.pure == 0 &&
              i1.hw_hits == 0 && i1.reason != NULL &&
              strcmp(i1.reason, "rdtsc") == 0,
          "hwrec-forward: gated normally -> single-step fallback, UNCHANGED "
          "from before T5 (rc=%d truncated=%d pure=%d hw_hits=%llu "
          "reason=%s)",
          rc1, (int)v1->truncated, i1.pure, (unsigned long long)i1.hw_hits,
          i1.reason ? i1.reason : "?");
    asmtest_valtrace_free(v1);

    asmtest_valtrace_t *v2 = asmtest_valtrace_new(256, 4096, 256);
    if (v2 == NULL) {
        CHECK(0, "hwrec-forward: valtrace_new (forced)");
        return;
    }
    long r2 = 0;
    asmtest_blockstep_info_t i2 = {0};
    asmtest_blockstep_opts_t o2;
    memset(&o2, 0, sizeof o2);
    o2.inject_block = -1;
    o2.force_replay = 1; /* push it past the region gate, as blind_rdtsc does */
    int rc2 = asmtest_dataflow_blockstep_run(hwrec_rdtsc, sizeof hwrec_rdtsc,
                                             NULL, 0, &o2, &r2, v2, &i2);
    CHECK(rc2 == DF_BLOCKSTEP_FAULT && v2->truncated && i2.hw_hits >= 1,
          "hwrec-forward: forced past the gate -> the DR exec breakpoint "
          "fires and hw_hits counts it (hw_hits=%llu), but the per-step "
          "decode still refuses to let Unicorn execute rdtsc, so the "
          "capture still FAULTS + truncates (rc=%d truncated=%d) — SAME "
          "verdict as before T5, new evidence only",
          (unsigned long long)i2.hw_hits, rc2, (int)v2->truncated);
    asmtest_valtrace_free(v2);

    asmtest_valtrace_t *v3 = asmtest_valtrace_new(256, 4096, 256);
    if (v3 == NULL) {
        CHECK(0, "hwrec-forward: valtrace_new (force_singlestep)");
        return;
    }
    long r3 = 0;
    asmtest_blockstep_info_t i3 = {0};
    asmtest_blockstep_opts_t o3;
    memset(&o3, 0, sizeof o3);
    o3.inject_block = -1;
    o3.force_singlestep = 1;
    int rc3 = asmtest_dataflow_blockstep_run(hwrec_rdtsc, sizeof hwrec_rdtsc,
                                             NULL, 0, &o3, &r3, v3, &i3);
    CHECK(rc3 == DF_BLOCKSTEP_OK && !v3->truncated && i3.hw_hits == 0,
          "hwrec-forward: force_singlestep=1 -> hw_hits STAYS 0 (Done-when "
          "check: no arming on the fallback path; hw_hits=%llu rc=%d "
          "truncated=%d)",
          (unsigned long long)i3.hw_hits, rc3, (int)v3->truncated);
    asmtest_valtrace_free(v3);
}

/* THE FAIL-CLOSED RULE — a syscall that DOES NOT RETURN to the next instruction.
 *
 * F2 rests on a measured premise: BTF traps the syscall, so the forward pass always has a real
 * boundary immediately AFTER it retired, holding the kernel's result. The replay does not trust
 * that premise, it REQUIRES it (`pc + len == pc_next`). `execve` is the honest way to falsify
 * it: on success the address space is REPLACED and control resurfaces at the new program's
 * entry point, so the boundary after the syscall is nowhere near syscall+2. There is no
 * "result" to carry across — the process that would have received it no longer exists.
 *
 * Injecting the boundary's rax here would be pure fabrication. The tier must truncate, and this
 * asserts that it does. This is also the concrete member of the class the plan asks about —
 * "what is not faithfully replayable at all" — and it is detected at RUNTIME from the boundary
 * itself, with no execve-specific knowledge anywhere in the tier. */
static void run_fail_closed_case(void) {
    /* An absolute path in the tracee's address space. The tracee is a fork of this process, so
     * this pointer is valid there — no code-relative addressing needed. */
    static const char path[] = "/proc/self/exe";
    asmtest_valtrace_t *v = asmtest_valtrace_new(1024, 8192, 1024);
    if (v == NULL) {
        CHECK(0, "fail-closed: valtrace_new");
        return;
    }
    long a[1] = {(long)(uintptr_t)path}, r = 0;
    asmtest_blockstep_info_t info = {0};
    asmtest_blockstep_opts_t o;
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    int rc = asmtest_dataflow_blockstep_run(sc_execve, sizeof sc_execve, a, 1,
                                            &o, &r, v, &info);
    CHECK(rc == DF_BLOCKSTEP_FAULT && v->truncated && info.injected == 0,
          "fail-closed: a syscall that NEVER RETURNS to the next instruction "
          "(execve — the address space is replaced) is refused at the "
          "boundary check and TRUNCATES (rc=%d truncated=%d injected=%llu); "
          "the premise \"BTF put the boundary just past the syscall\" is "
          "REQUIRED at runtime, never assumed",
          rc, (int)v->truncated, (unsigned long long)info.injected);
    CHECK(info.injectable == 1,
          "fail-closed: and the STATIC scan could not have known — it calls "
          "the region injectable (injectable=%d), because whether a syscall "
          "returns is a property of the RUN, not of the bytes. The runtime "
          "check is the only thing standing here",
          info.injectable);
    asmtest_valtrace_free(v);
}

/* REPEATED injection. Every other F2 case injects exactly once, so this is the only one that
 * can catch a defect appearing on the second injection. Asserts the count is EXACTLY n — not
 * merely ">0" — so an injection that fires once and then silently stops still fails. */
static void run_syscall_loop_case(void) {
    enum { N = 5 };
    long a[1] = {N};
    asmtest_valtrace_t *A = asmtest_valtrace_new(4096, 65536, 4096);
    asmtest_valtrace_t *B = asmtest_valtrace_new(4096, 65536, 4096);
    if (A == NULL || B == NULL) {
        CHECK(0, "sc_loop: valtrace_new");
        goto done;
    }
    long ra = 0, rb = 0;
    asmtest_blockstep_info_t ia = {0}, ib = {0};
    asmtest_blockstep_opts_t oracle, block;
    memset(&oracle, 0, sizeof oracle);
    oracle.inject_block = -1;
    oracle.force_singlestep = 1;
    memset(&block, 0, sizeof block);
    block.inject_block = -1;
    int rca = asmtest_dataflow_blockstep_run(sc_loop, sizeof sc_loop, a, 1,
                                             &oracle, &ra, A, &ia);
    int rcb = asmtest_dataflow_blockstep_run(sc_loop, sizeof sc_loop, a, 1,
                                             &block, &rb, B, &ib);
    if (rca != DF_BLOCKSTEP_OK || rcb != DF_BLOCKSTEP_OK) {
        CHECK(0, "sc_loop: capture failed (oracle rc=%d block rc=%d)", rca,
              rcb);
        goto done;
    }
    long want = (long)N * (long)getpid();
    CHECK(ra == want && rb == want,
          "sc_loop: %d getppid calls summed to %d*testpid = %ld on BOTH paths "
          "(oracle=%ld block=%ld)",
          N, N, want, ra, rb);
    CHECK(ib.injected == (uint64_t)N,
          "sc_loop: the replay injected EXACTLY %d syscalls (got %llu) — "
          "repeated injection, not a one-shot",
          N, (unsigned long long)ib.injected);
    CHECK(!A->truncated && !B->truncated, "sc_loop: neither trace truncated");
    normalize_rsp_relative(A, ia.entry_rsp);
    normalize_rsp_relative(B, ib.entry_rsp);
    int raw = 0;
    CHECK(traces_identical(A, B, &raw),
          "sc_loop: byte-identical to the single-step oracle across %d "
          "injected syscalls (%zu steps, %zu records)",
          N, A->steps_len, A->recs_len);
    CHECK(ib.stops < ia.stops,
          "sc_loop: still cut the stops %llu -> %llu across a loop of syscalls",
          (unsigned long long)ia.stops, (unsigned long long)ib.stops);
done:
    asmtest_valtrace_free(A);
    asmtest_valtrace_free(B);
}

/* THE MEMORY SCOPE BOUNDARY. A kernel write OUTSIDE the replay's stack window is memory the
 * Unicorn guest never had (it maps code + a window around rsp — F1's scope, inherited). The
 * load must therefore FAULT the replay rather than return a stale or zero value, while the
 * single-step fallback still reports the region correctly. Asserting BOTH halves is the point:
 * "it truncates" alone would also be satisfied by a tier that truncates everything. */
static void run_pread_heap_case(int fd, void *buf) {
    long a[2] = {(long)fd, (long)(uintptr_t)buf};

    asmtest_valtrace_t *v = asmtest_valtrace_new(1024, 8192, 1024);
    if (v == NULL) {
        CHECK(0, "sc_pread_heap: valtrace_new");
        return;
    }
    long r = 0;
    asmtest_blockstep_info_t info = {0};
    asmtest_blockstep_opts_t o;
    memset(&o, 0, sizeof o);
    o.inject_block = -1;
    int rc = asmtest_dataflow_blockstep_run(sc_pread_heap, sizeof sc_pread_heap,
                                            a, 2, &o, &r, v, &info);
    CHECK(rc == DF_BLOCKSTEP_FAULT && v->truncated,
          "sc_pread_heap: a kernel write OUTSIDE the replay's stack window "
          "(heap buffer) faults the replay's load and TRUNCATES (rc=%d "
          "truncated=%d) — F2's memory scope is F1's guest map, and it fails "
          "CLOSED rather than reporting stale bytes",
          rc, (int)v->truncated);
    asmtest_valtrace_free(v);

    asmtest_valtrace_t *w = asmtest_valtrace_new(1024, 8192, 1024);
    if (w == NULL)
        return;
    long r2 = 0;
    asmtest_blockstep_info_t i2 = {0};
    asmtest_blockstep_opts_t o2;
    memset(&o2, 0, sizeof o2);
    o2.inject_block = -1;
    o2.force_singlestep = 1;
    memset(buf, 0, 8);
    int rc2 = asmtest_dataflow_blockstep_run(
        sc_pread_heap, sizeof sc_pread_heap, a, 2, &o2, &r2, w, &i2);
    CHECK(rc2 == DF_BLOCKSTEP_OK && !w->truncated &&
              (uint64_t)r2 == SC_PREAD_MAGIC,
          "sc_pread_heap CONTROL: the SAME region single-steps to the correct "
          "heap value 0x%llx (rc=%d truncated=%d r=0x%llx) — the truncation "
          "above is the replay's memory scope, not a broken fixture",
          (unsigned long long)SC_PREAD_MAGIC, rc2, (int)w->truncated,
          (unsigned long long)r2);
    asmtest_valtrace_free(w);
}

static int plausible_pread(long r) { return (uint64_t)r == SC_PREAD_MAGIC; }
static int plausible_monotonic(long r) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    /* tv_sec of CLOCK_MONOTONIC, sampled moments apart: equal or a hair behind. */
    return r >= 0 && r <= ts.tv_sec && r > ts.tv_sec - 60;
}

static void run_purity_check(const char *name, const uint8_t *code, size_t len,
                             int want_pure, const char *want_reason) {
    const char *reason = NULL;
    int pure = asmtest_dataflow_blockstep_is_pure(code, len, &reason);
    if (want_pure)
        CHECK(pure == 1,
              "purity: %s classified PURE -> block-step+replay eligible", name);
    else
        CHECK(pure == 0 && reason != NULL && want_reason != NULL &&
                  strcmp(reason, want_reason) == 0,
              "purity: %s classified IMPURE (%s) -> single-step fallback", name,
              reason ? reason : "?");
}

int main(void) {
    printf("# F1 increment 1: block-step + emulator-replay value tier\n");

    int probe = asmtest_dataflow_blockstep_probe();
    if (probe == DF_BLOCKSTEP_ENOSYS) {
        printf("# SKIP dataflow-blockstep: built off Linux x86-64 / without "
               "Capstone+Unicorn\n"
               "1..0\n");
        return 0;
    }

    /* Before ANY telemetry below is trusted: this suite re-declares
     * asmtest_blockstep_info_t (the tier ships no header), and a silent layout skew
     * between the two copies corrupts every field read out of `info` from here on —
     * exactly the skew that cost F6's sibling telemetry struct 3 green checks before
     * this same guard caught it (2026-07-17-dataflow-tier-open-followups.md #3). Check
     * it, do not assume it. */
    {
        size_t isz = 0, ioff = 0;
        asmtest_dataflow_blockstep_info_layout(&isz, &ioff);
        CHECK(isz == sizeof(asmtest_blockstep_info_t) &&
                  ioff == offsetof(asmtest_blockstep_info_t, hw_hits),
              "info: the suite's re-declared telemetry struct matches the "
              "producer's SIZE and final-field OFFSET (a skew here silently "
              "corrupts every info.* read below; size alone misses a field "
              "that tail padding absorbs)");
    }

    /* T4: the tier's FIRST opts layout guard (no_undef_mask/inject_flag_bit are appended
     * fields on a struct this suite re-declares field-for-field) — same skew hazard, same
     * check, before any opts. field below is trusted. */
    {
        size_t osz = 0, ooff = 0;
        asmtest_dataflow_blockstep_opts_layout(&osz, &ooff);
        CHECK(osz == sizeof(asmtest_blockstep_opts_t) &&
                  ooff == offsetof(asmtest_blockstep_opts_t, inject_flag_bit),
              "opts: the suite's re-declared options struct matches the "
              "producer's SIZE and final-field OFFSET");
    }

    /* T4: undefined-EFLAGS table checks need only PTRACE_SINGLESTEP (no BTF), so they run
     * here — BEFORE the BTF-gated `probe != 1` skip below — on a GitHub Actions VM same as
     * bare metal. run_undef_row's own preflight self-skips cleanly if ptrace itself is
     * unavailable (seccomp/yama). */
    run_undef_flags_table();

    /* Purity classification (Capstone only) — always runs in the real build. */
    run_purity_check("loop_poly", loop_poly, sizeof loop_poly, 1, NULL);
    run_purity_check("mem_chain", mem_chain, sizeof mem_chain, 1, NULL);
    run_purity_check("imp_cpuid", imp_cpuid, sizeof imp_cpuid, 0, "cpuid");
    run_purity_check("imp_syscall", imp_syscall, sizeof imp_syscall, 0,
                     "syscall");
    run_purity_check("imp_rdtsc", imp_rdtsc, sizeof imp_rdtsc, 0, "rdtsc");
    run_purity_check("imp_rdrand", imp_rdrand, sizeof imp_rdrand, 0, "rdrand");
    run_purity_check("imp_int80", imp_int80, sizeof imp_int80, 0, "int 0x80");

    /* F2 classification (Capstone only): which impurities record-and-inject can CARRY. The
     * split is a measured hardware fact — BTF traps syscall/int 0x80 (control transfers) and
     * does not trap rdtsc/cpuid/rdrand — not a policy choice, so it is asserted as one. */
    {
        const char *why = NULL;
        CHECK(asmtest_dataflow_blockstep_is_injectable(
                  imp_syscall, sizeof imp_syscall, &why) == 1,
              "injectable: a `syscall` region IS injectable — BTF traps it, so "
              "the forward pass gets a real post-retirement boundary for free");
        CHECK(asmtest_dataflow_blockstep_is_injectable(
                  imp_int80, sizeof imp_int80, &why) == 1,
              "injectable: an `int 0x80` region IS injectable (the legacy gate "
              "is a control transfer too)");
        CHECK(asmtest_dataflow_blockstep_is_injectable(
                  loop_poly, sizeof loop_poly, &why) == 1,
              "injectable: a PURE region is injectable vacuously (nothing to "
              "carry)");
        CHECK(asmtest_dataflow_blockstep_is_injectable(
                  imp_rdtsc, sizeof imp_rdtsc, &why) == 0 &&
                  why != NULL && strcmp(why, "rdtsc") == 0,
              "injectable: `rdtsc` is NOT injectable (reason=%s) — BTF does "
              "not trap it, so there is no boundary to record from",
              why ? why : "?");
        CHECK(asmtest_dataflow_blockstep_is_injectable(
                  imp_cpuid, sizeof imp_cpuid, &why) == 0 &&
                  why != NULL && strcmp(why, "cpuid") == 0,
              "injectable: `cpuid` is NOT injectable (reason=%s)",
              why ? why : "?");
        CHECK(asmtest_dataflow_blockstep_is_injectable(
                  imp_rdrand, sizeof imp_rdrand, &why) == 0 &&
                  why != NULL && strcmp(why, "rdrand") == 0,
              "injectable: `rdrand` is NOT injectable (reason=%s)",
              why ? why : "?");
        /* The ALL-quantifier. `injectable` must see the WHOLE region: settling it at the FIRST
         * impurity would call this region injectable on the strength of its syscall and hand
         * its cpuid to the replay — the HIGH-3 early-break bug in a new costume. And the reason
         * must name the impurity that DISQUALIFIES it, not the first one seen. */
        CHECK(asmtest_dataflow_blockstep_is_injectable(
                  sc_then_cpuid, sizeof sc_then_cpuid, &why) == 0 &&
                  why != NULL && strcmp(why, "cpuid") == 0,
              "injectable: `syscall; cpuid` is NOT injectable and names the "
              "DISQUALIFYING impurity (reason=%s, not \"syscall\") — the "
              "verdict is an ALL-quantifier, not the first impurity seen",
              why ? why : "?");
        /* is_pure() must keep F1's meaning: ten other things ask it, and F2 widening what the
         * TIER accepts must not quietly redefine what PURE means. */
        CHECK(asmtest_dataflow_blockstep_is_pure(imp_syscall,
                                                 sizeof imp_syscall, &why) == 0,
              "injectable: a syscall region is still IMPURE — F2 widened the "
              "tier's gate, it did not redefine purity");
    }

    /* Replayability classification (Capstone only) — the encoding-level VEX/EVEX gate. SSE is
     * replayable; every VEX/EVEX form is not. The pure-GP fixtures must stay replayable, or
     * the gate would have silently cost increment 1 its entire perturbation win. */
    {
        const char *why = NULL;
        CHECK(asmtest_dataflow_blockstep_is_replayable(
                  loop_poly, sizeof loop_poly, &why) == 1,
              "replayable: loop_poly (GP only) stays REPLAYABLE — the gate "
              "does not "
              "over-reach");
        CHECK(asmtest_dataflow_blockstep_is_replayable(
                  mem_chain, sizeof mem_chain, &why) == 1,
              "replayable: mem_chain (GP only) stays REPLAYABLE");
        CHECK(
            asmtest_dataflow_blockstep_is_replayable(
                sse_live_in + SSE_LIVE_IN_ROFF,
                sizeof sse_live_in - SSE_LIVE_IN_ROFF, &why) == 1,
            "replayable: legacy-SSE region is REPLAYABLE (Unicorn executes SSE "
            "correctly)");
        CHECK(asmtest_dataflow_blockstep_is_replayable(
                  avx_ymm + AVX_YMM_ROFF, sizeof avx_ymm - AVX_YMM_ROFF,
                  &why) == 0 &&
                  why != NULL && strcmp(why, "vex/evex") == 0,
              "replayable: VEX-256 region is NOT replayable (reason=%s)",
              why ? why : "?");
        CHECK(asmtest_dataflow_blockstep_is_replayable(
                  avx512_zmm + AVX512_ZMM_ROFF,
                  sizeof avx512_zmm - AVX512_ZMM_ROFF, &why) == 0 &&
                  why != NULL && strcmp(why, "vex/evex") == 0,
              "replayable: EVEX region is NOT replayable (reason=%s)",
              why ? why : "?");
    }

    /* Scan-only regression checks (Capstone, no ptrace): the three ways region_scan used to
     * fail OPEN. It is a single point of failure feeding BOTH the gate and — via touches_vec —
     * the vector seed and canary, so a wrong verdict does not merely lose a check, it lets the
     * instruction through AND removes the witness. */
    run_is_replayable_independence_case();

    /* T5 (F2 increment 2, forward pass): the scan-level unit test — Capstone only, no ptrace,
     * so it runs on every VM lane same as the checks just above. */
    run_hwrec_scan_case();

    /* What this box and this Unicorn can actually do — reported, never assumed. */
    {
        int nregs = 0;
        int hw = asmtest_dataflow_blockstep_vec_width(&nregs);
        int ucw = asmtest_dataflow_blockstep_uc_vec_width();
        printf("#   hardware vector state: %d-byte x %d regs (%s)\n", hw, nregs,
               hw >= 64   ? "AVX-512"
               : hw >= 32 ? "AVX"
                          : "SSE");
        printf("#   unicorn round-trips:   %d-byte (2.0.1 accepts a ZMM write "
               "and stores "
               "NOTHING; 2.1.x fixed it)\n",
               ucw);
        printf("#   replayed vector width: 16-byte (legacy SSE only — no "
               "released unicorn "
               "executes VEX/EVEX)\n");
        CHECK(ucw >= 16,
              "unicorn holds at least XMM (128-bit) state, proven by read-back "
              "(width=%d)",
              ucw);
    }

    if (probe != 1) {
        printf(
            "# SKIP live block-step/replay: ptrace unavailable (seccomp/yama) "
            "or PTRACE_SINGLEBLOCK non-functional (BTF masked)\n");
        goto report;
    }

    {
        long a1[1] = {50};
        run_identity_case("loop_poly(n=50)", loop_poly, sizeof loop_poly, a1, 1,
                          3725);
        long a2[2] = {7, 5};
        run_identity_case("mem_chain(7,5)", mem_chain, sizeof mem_chain, a2, 2,
                          12);
    }
    run_impure_case();
    run_canary_case();

    /* T4: the xor-to-zero identity case and the undefined-EFLAGS canary discrimination need
     * the real block-step+replay path (BTF), so they live here — gated behind the same
     * `probe == 1` check as run_canary_case() above. */
    run_xor_zero_identity_case();
    run_undef_canary_case();

    /* ---- F2: record-and-inject over OS-interacting regions ---- */
    {
        /* getppid() == this test's pid: the same in BOTH forked captures (getpid would not
         * be), so the byte-identical oracle can judge it, and checkable from outside. */
        long want = 2L * (long)getpid();
        run_syscall_identity_case("sc_getppid", sc_getppid, sizeof sc_getppid,
                                  NULL, 0, want, /*sc_off*/ 0x05,
                                  /*sc_rax*/ (long)getpid(), /*nwrites*/ 3);
        /* int 0x80 clobbers rax ONLY — measured. Its write set must be 1, not 3. */
        run_syscall_identity_case("sc_int80", sc_int80, sizeof sc_int80, NULL,
                                  0, want, /*sc_off*/ 0x05,
                                  /*sc_rax*/ (long)getpid(), /*nwrites*/ 1);

        /* THE EXIT CRITERION: a syscall that FILLS A BUFFER the region then loads. */
        int fd = memfd_create("asmtest_f2", 0);
        if (fd < 0) {
            printf("# SKIP sc_pread: memfd_create: %s\n", strerror(errno));
        } else {
            uint64_t magic = SC_PREAD_MAGIC;
            if (write(fd, &magic, sizeof magic) != (ssize_t)sizeof magic) {
                printf("# SKIP sc_pread: seeding the memfd failed\n");
            } else {
                long a[1] = {(long)fd};
                run_syscall_identity_case(
                    "sc_pread(kernel fills a stack buffer)", sc_pread,
                    sizeof sc_pread, a, 1, (long)SC_PREAD_MAGIC,
                    /*sc_off*/ 0x24, /*sc_rax*/ 8, /*nwrites*/ 3);
                run_syscall_mem_case("sc_pread/mem", sc_pread, sizeof sc_pread,
                                     a, 1, /*load_off*/ 0x26, plausible_pread,
                                     "the exact 8 bytes seeded into the memfd");
                /* The same kernel write, but landing OUTSIDE the replay's guest map. */
                void *heap = malloc(64);
                if (heap != NULL) {
                    run_pread_heap_case(fd, heap);
                    free(heap);
                }
            }
            close(fd);
        }

        /* clock_gettime — the plan's own named example, and one the byte-identical oracle
         * CANNOT judge (two captures, two different times). Covered by the memory oracle. */
        run_syscall_mem_case("sc_clock_gettime", sc_clock, sizeof sc_clock,
                             NULL, 0, /*load_off*/ 0x20, plausible_monotonic,
                             "a plausible CLOCK_MONOTONIC tv_sec");
    }
    run_syscall_loop_case();
    run_no_inject_case();
    run_fail_closed_case();
    run_blind_rdtsc_case();
    run_hwrec_forward_case();

    /* Vector breadth (increment 2). */
    run_vec_seed_case();
    run_mxcsr_case();
    run_stack_window_case();
    run_desync_case();
    run_impure_vector_case();
    run_encoding_gate_case();
    run_vex128_case();
    run_avx_case("avx_ymm(7)", avx_ymm, sizeof avx_ymm, AVX_YMM_ROFF, 32, 32);
    run_avx_case("avx512_zmm(7)", avx512_zmm, sizeof avx512_zmm,
                 AVX512_ZMM_ROFF, 64, 64);
    run_hi16_zmm_case();

report:
    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    else
        printf("# all %d checks passed\n", checks);
    return failures ? 1 : 0;
}
