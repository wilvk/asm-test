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
#include "asmtest_valtrace.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    int no_vec_canary;
    int force_replay;
    uint64_t stack_hi_pad;
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
} asmtest_blockstep_info_t;

int asmtest_dataflow_blockstep_probe(void);
int asmtest_dataflow_blockstep_is_pure(const uint8_t *code, size_t code_len,
                                       const char **reason);
int asmtest_dataflow_blockstep_is_replayable(const uint8_t *code,
                                             size_t code_len,
                                             const char **reason);
int asmtest_dataflow_blockstep_vec_width(int *nregs);
int asmtest_dataflow_blockstep_uc_vec_width(void);
int asmtest_dataflow_blockstep_run(const uint8_t *code, size_t code_len,
                                   const long *args, int nargs,
                                   const asmtest_blockstep_opts_t *opts,
                                   long *result, asmtest_valtrace_t *vt,
                                   asmtest_blockstep_info_t *info);

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

/* Case 6 — VEX-128 is a silent liar, and the encoding gate is what stops it. */
static void run_vex128_case(void) {
    const char *reason = NULL;
    CHECK(asmtest_dataflow_blockstep_is_replayable(
              vex128_liar + VEX128_LIAR_ROFF,
              sizeof vex128_liar - VEX128_LIAR_ROFF, &reason) == 0 &&
              reason != NULL && strcmp(reason, "avx") == 0,
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
              info.reason != NULL && strcmp(info.reason, "avx") == 0,
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
              reason != NULL && strcmp(reason, "avx") == 0,
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
              info.reason != NULL && strcmp(info.reason, "avx") == 0,
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

/* The purity static-scan classification, run whenever the real (non-stub) producer is built —
 * it needs only Capstone, not ptrace/SINGLEBLOCK, so it reports even on a locked-down host. */
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

    /* Purity classification (Capstone only) — always runs in the real build. */
    run_purity_check("loop_poly", loop_poly, sizeof loop_poly, 1, NULL);
    run_purity_check("mem_chain", mem_chain, sizeof mem_chain, 1, NULL);
    run_purity_check("imp_cpuid", imp_cpuid, sizeof imp_cpuid, 0, "cpuid");
    run_purity_check("imp_syscall", imp_syscall, sizeof imp_syscall, 0,
                     "syscall");
    run_purity_check("imp_rdtsc", imp_rdtsc, sizeof imp_rdtsc, 0, "rdtsc");
    run_purity_check("imp_rdrand", imp_rdrand, sizeof imp_rdrand, 0, "rdrand");
    run_purity_check("imp_int80", imp_int80, sizeof imp_int80, 0, "int 0x80");

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
                  why != NULL && strcmp(why, "avx") == 0,
              "replayable: VEX-256 region is NOT replayable (reason=%s)",
              why ? why : "?");
        CHECK(asmtest_dataflow_blockstep_is_replayable(
                  avx512_zmm + AVX512_ZMM_ROFF,
                  sizeof avx512_zmm - AVX512_ZMM_ROFF, &why) == 0 &&
                  why != NULL && strcmp(why, "avx") == 0,
              "replayable: EVEX region is NOT replayable (reason=%s)",
              why ? why : "?");
    }

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

    /* Vector breadth (increment 2). */
    run_vec_seed_case();
    run_stack_window_case();
    run_vex128_case();
    run_avx_case("avx_ymm(7)", avx_ymm, sizeof avx_ymm, AVX_YMM_ROFF, 32, 32);
    run_avx_case("avx512_zmm(7)", avx512_zmm, sizeof avx512_zmm,
                 AVX512_ZMM_ROFF, 64, 64);

report:
    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    else
        printf("# all %d checks passed\n", checks);
    return failures ? 1 : 0;
}
