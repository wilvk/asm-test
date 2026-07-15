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
} asmtest_blockstep_opts_t;

typedef struct {
    int pure;
    const char *reason;
    uint64_t stops;
    uint64_t steps;
    uint64_t entry_rsp;
} asmtest_blockstep_info_t;

int asmtest_dataflow_blockstep_probe(void);
int asmtest_dataflow_blockstep_is_pure(const uint8_t *code, size_t code_len,
                                       const char **reason);
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
    asmtest_blockstep_opts_t oracle = {0, 1, 0,
                                       -1}; /* force_singlestep = the oracle */
    asmtest_blockstep_opts_t block = {0, 0, 0,
                                      -1}; /* purity-gated: pure -> replay  */

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
    asmtest_blockstep_opts_t opts = {0, 0, 0, -1}; /* purity-gated */
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
    asmtest_blockstep_opts_t opts = {0, 0, 1,
                                     1}; /* inject at interior block 1 */
    int rc = asmtest_dataflow_blockstep_run(loop_poly, sizeof loop_poly, args,
                                            1, &opts, &r, v, &info);
    CHECK(rc == DF_BLOCKSTEP_FAULT && v->truncated,
          "canary: injected replay-input divergence DETECTED -> truncated "
          "(rc=%d truncated=%d)",
          rc, (int)v->truncated);
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

report:
    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    else
        printf("# all %d checks passed\n", checks);
    return failures ? 1 : 0;
}
