/* test_capture_win64.c — driver for the native Win64 capture trampoline.
 *
 * Calls the asm_call_capture*_win64 entry points (src/capture_win64.asm) on a
 * handful of Win64-ABI routines and checks the captured return value,
 * stack-arg/shadow-space handling, FP/vector capture, and ABI preservation over
 * the Win64 callee-saved set (GP + xmm6..15).
 *
 * The layout comes from <asmtest.h>'s ASMTEST_ABI_WIN64 branch of regs_t (the
 * single source of truth, with _Static_assert offset pins + the manifest), so
 * this suite doubles as the native Win64 conformance check.
 *
 * Two build lanes, same source:
 *   - ms_abi native lane (Linux/macOS x86-64): the host is System V, so the
 *     trampoline is declared __attribute__((ms_abi)) to be called the Win64 way.
 *   - PE/Wine lane (mingw-w64): the target is already Win64, so no attribute.
 */
#include <cpuid.h>
#include <stdint.h>
#include <stdio.h>

#include "asmtest.h"

/* The Win64 capture test links neither asmtest.o nor libunicorn, so it carries
 * its own AVX2 probe (CPUID feature bits + XCR0 YMM-state enable) to gate the
 * 256-bit case — Wine runs PE instructions on the host CPU, so this reflects the
 * real silicon. Mirrors asmtest_cpu_has_avx2() in src/asmtest.c. */
static int has_avx2(void) {
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
    if (!(c & (1u << 27)) || !(c & (1u << 28))) return 0; /* OSXSAVE + AVX */
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return 0;
    if (!(b & (1u << 5))) return 0;                        /* AVX2 (leaf 7 EBX) */
    unsigned xcr0_lo, xcr0_hi;
    __asm__("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    return (xcr0_lo & 0x6) == 0x6;                         /* XMM + YMM state on */
}

/* AVX-512F probe, mirrors asmtest_cpu_has_avx512f() in src/asmtest.c (Wine runs PE
 * instructions on the host CPU, so this reflects real silicon). */
static int has_avx512f(void) {
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
    if (!(c & (1u << 27))) return 0;                      /* OSXSAVE */
    unsigned xcr0_lo, xcr0_hi;
    __asm__("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    if ((xcr0_lo & 0xe6) != 0xe6) return 0;               /* opmask+ZMM_Hi256+Hi16_ZMM */
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return 0;
    return (b & (1u << 16)) != 0;                         /* AVX-512F (leaf 7 EBX) */
}

#if defined(_WIN32)
#  define WIN64ABI /* mingw target is already Microsoft x64 */
#else
#  define WIN64ABI __attribute__((ms_abi))
#endif

extern WIN64ABI void asm_call_capture_win64(regs_t *out, void *fn,
                                            const long long *args);
extern WIN64ABI void asm_call_capture_args_win64(regs_t *out, void *fn,
                                                 const long long *args,
                                                 int nargs);
extern WIN64ABI void asm_call_capture_vec_win64(regs_t *out, void *fn,
                                                const long long *iargs,
                                                const vec128_t *vargs);
extern WIN64ABI void asm_call_capture_fp_win64(regs_t *out, void *fn,
                                               const long long *iargs,
                                               const double *fargs);
extern WIN64ABI void asm_call_capture_fp_n_win64(regs_t *out, void *fn,
                                                 const long long *iargs,
                                                 const double *fargs, int nfargs);
extern WIN64ABI void asm_call_capture_sret_win64(regs_t *out, void *fn,
                                                 void *result,
                                                 const long long *args,
                                                 int nargs);
extern WIN64ABI void asm_call_capture_vec_n_win64(regs_t *out, void *fn,
                                                  const long long *iargs,
                                                  const vec128_t *vargs,
                                                  int nvargs);
extern WIN64ABI void asm_call_capture_bigstruct_win64(regs_t *out, void *fn,
                                                      const long long *iargs,
                                                      int niargs,
                                                      const void *sptr,
                                                      unsigned long long ssize);
extern WIN64ABI void asm_call_capture_vec256_win64(vec256_t *vec, void *fn,
                                                   const long long *iargs,
                                                   const vec256_t *vargs);
extern WIN64ABI void asm_call_capture_vec512_win64(vec512_t *vec, void *fn,
                                                   const long long *iargs,
                                                   const vec512_t *vargs);

/* Routines under test — addresses only; never called through these C types. */
extern void win64_ret_arg0(void);
extern void win64_sum2(void);
extern void win64_preserve_rbx(void);
extern void win64_clobber_rbx(void);
extern void win64_ret_arg4(void);
extern void win64_sum6(void);
extern void win64_addsd(void);
extern void win64_vec_preserve_xmm6(void);
extern void win64_vec_clobber_xmm6(void);
extern void win64_addsd6(void);
extern void win64_sret_make(void);
extern void win64_vaddsd5(void);
extern void win64_struct_sum(void);
extern void win64_vaddpd_ymm(void);
extern void win64_vaddpd_zmm(void);

static int fails = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            printf("ok   - %s\n", (msg));                                      \
        } else {                                                               \
            printf("FAIL - %s\n", (msg));                                      \
            fails++;                                                           \
        }                                                                      \
    } while (0)

static int abi_preserved(const regs_t *r) {
    return r->rbx == ASMTEST_SENTINEL_RBX && r->rbp == ASMTEST_SENTINEL_RBP &&
           r->rdi == ASMTEST_SENTINEL_RDI && r->rsi == ASMTEST_SENTINEL_RSI &&
           r->r12 == ASMTEST_SENTINEL_R12 && r->r13 == ASMTEST_SENTINEL_R13 &&
           r->r14 == ASMTEST_SENTINEL_R14 && r->r15 == ASMTEST_SENTINEL_R15;
}

/* xmm6..15 are callee-saved on Win64; the _vec trampolines seed each with both
 * lanes = its register index, so preservation is vec[i].u64 == {i, i}. */
static int xmm_callee_preserved(const regs_t *r) {
    for (int i = 6; i <= 15; i++) {
        if (r->vec[i].u64[0] != (uint64_t)i || r->vec[i].u64[1] != (uint64_t)i)
            return 0;
    }
    return 1;
}

static vec128_t lane_from_double(double d) {
    vec128_t v;
    v.u64[0] = 0;
    v.u64[1] = 0;
    v.f64[0] = d;
    return v;
}

int main(void) {
    regs_t r;
    const long long args[6] = {111, 222, 333, 444, 555, 666};

    asm_call_capture_win64(&r, (void *)win64_ret_arg0, args);
    CHECK(r.ret == 111, "win64_ret_arg0 returns the 1st int arg (rcx)");
    CHECK(abi_preserved(&r), "win64_ret_arg0 preserves the callee-saved set");

    asm_call_capture_win64(&r, (void *)win64_sum2, args);
    CHECK(r.ret == 333, "win64_sum2 returns rcx+rdx (111+222)");

    asm_call_capture_win64(&r, (void *)win64_ret_arg4, args);
    CHECK(r.ret == 555, "win64_ret_arg4 returns the 5th arg (stack + shadow)");

    asm_call_capture_win64(&r, (void *)win64_preserve_rbx, args);
    CHECK(r.ret == 111 && abi_preserved(&r),
          "win64_preserve_rbx: correct result and callee-saved intact");

    asm_call_capture_win64(&r, (void *)win64_clobber_rbx, args);
    CHECK(r.ret == 111, "win64_clobber_rbx still returns rcx");
    CHECK(!abi_preserved(&r) && r.rbx != ASMTEST_SENTINEL_RBX,
          "win64_clobber_rbx: ABI violation detected (rbx not preserved)");

    /* asm_call_capture_args_win64: arbitrary integer arity (register + stack). */
    asm_call_capture_args_win64(&r, (void *)win64_sum6, args, 6);
    CHECK(r.ret == 111 + 222 + 333 + 444 + 555 + 666,
          "win64_sum6 via _args: 4 register + 2 stack args summed");

    asm_call_capture_args_win64(&r, (void *)win64_sum2, args, 2);
    CHECK(r.ret == 333, "win64_sum2 via _args with nargs=2");

    /* asm_call_capture_vec_win64: FP return + full vector file + xmm6-15. */
    vec128_t vargs[4];
    vargs[0] = lane_from_double(1.5);
    vargs[1] = lane_from_double(2.25);
    vargs[2] = lane_from_double(0.0);
    vargs[3] = lane_from_double(0.0);

    asm_call_capture_vec_win64(&r, (void *)win64_addsd, args, vargs);
    CHECK(r.fret == 3.75, "win64_addsd via _vec: xmm0+xmm1 FP return captured");
    CHECK(xmm_callee_preserved(&r),
          "win64_addsd preserves the xmm6-15 callee-saved set");

    asm_call_capture_vec_win64(&r, (void *)win64_vec_preserve_xmm6, args, vargs);
    CHECK(r.fret == 1.5 && xmm_callee_preserved(&r),
          "win64_vec_preserve_xmm6: correct FP result and xmm6-15 intact");

    asm_call_capture_vec_win64(&r, (void *)win64_vec_clobber_xmm6, args, vargs);
    CHECK(!xmm_callee_preserved(&r) && r.vec[6].u64[0] == 0 && r.vec[6].u64[1] == 0,
          "win64_vec_clobber_xmm6: FP ABI violation detected (xmm6 not preserved)");
    CHECK(r.vec[7].u64[0] == 7 && r.vec[15].u64[0] == 15,
          "win64_vec_clobber_xmm6: the other xmm sentinels survive");

    /* asm_call_capture_fp_win64: 4 doubles + FP return, GP callee-saved checked. */
    const double fargs[4] = {1.5, 2.25, 4.0, 8.0};
    asm_call_capture_fp_win64(&r, (void *)win64_addsd, args, fargs);
    CHECK(r.fret == 3.75 && abi_preserved(&r),
          "win64_addsd via _fp: FP return captured, GP callee-saved intact");

    /* asm_call_capture_fp_n_win64: arbitrary FP arity (register + stack). */
    const double fargs6[6] = {1.0, 2.0, 4.0, 8.0, 16.0, 32.0};
    asm_call_capture_fp_n_win64(&r, (void *)win64_addsd6, args, fargs6, 6);
    CHECK(r.fret == 63.0,
          "win64_addsd6 via _fp_n: 4 xmm + 2 stack double args summed");

    /* asm_call_capture_sret_win64: struct return via the hidden rcx pointer. */
    long long result[2] = {0, 0};
    const long long sret_args[2] = {1234, 5678};
    asm_call_capture_sret_win64(&r, (void *)win64_sret_make, result, sret_args, 2);
    CHECK(result[0] == 1234 && result[1] == 5678,
          "win64_sret_make via _sret: struct written through hidden pointer");
    CHECK(r.ret == (uint64_t)(uintptr_t)result,
          "win64_sret_make via _sret: result pointer returned in rax");

    /* asm_call_capture_vec_n_win64: arbitrary vector arity (xmm + stack). */
    vec128_t vargs5[5];
    vargs5[0] = lane_from_double(1.0);
    vargs5[1] = lane_from_double(2.0);
    vargs5[2] = lane_from_double(4.0);
    vargs5[3] = lane_from_double(8.0);
    vargs5[4] = lane_from_double(16.0);
    asm_call_capture_vec_n_win64(&r, (void *)win64_vaddsd5, args, vargs5, 5);
    CHECK(r.fret == 31.0,
          "win64_vaddsd5 via _vec_n: 4 xmm + 1 stack vector arg summed");
    CHECK(xmm_callee_preserved(&r),
          "win64_vaddsd5 via _vec_n: xmm6-15 callee-saved intact");

    /* asm_call_capture_vec256_win64: AVX2 256-bit (ymm) capture under Win64,
     * the wide-vector analog of the SysV asm_call_capture_vec256. Self-skips on
     * a non-AVX2 host. Checks the full 256-bit return (all four doubles, so the
     * upper 128 lanes the 128-bit path can't see are exercised). */
    if (has_avx2()) {
        vec256_t v256[2] = {{{0}}};
        v256[0].f64[0] = 1; v256[0].f64[1] = 2; v256[0].f64[2] = 3; v256[0].f64[3] = 4;
        v256[1].f64[0] = 10; v256[1].f64[1] = 20; v256[1].f64[2] = 30; v256[1].f64[3] = 40;
        vec256_t out256[16] = {{{0}}};
        asm_call_capture_vec256_win64(out256, (void *)win64_vaddpd_ymm, args, v256);
        CHECK(out256[0].f64[0] == 11 && out256[0].f64[1] == 22 &&
              out256[0].f64[2] == 33 && out256[0].f64[3] == 44,
              "win64_vaddpd_ymm via _vec256: full 256-bit ymm0 sum (incl upper 128)");
    } else {
        printf("ok   - win64_vaddpd_ymm via _vec256 # SKIP no AVX2\n");
    }

    /* asm_call_capture_vec512_win64: AVX-512 512-bit (zmm) capture under Win64,
     * the wide-vector analog at twice the width again. Self-skips on a non-AVX-512
     * host. Checks the full 512-bit return (all eight doubles, so the upper lanes
     * neither the 128- nor 256-bit path can see are exercised). */
    if (has_avx512f()) {
        vec512_t v512[2] = {{{0}}};
        for (int i = 0; i < 8; i++) {
            v512[0].f64[i] = i + 1;        /* 1..8   */
            v512[1].f64[i] = (i + 1) * 10; /* 10..80 */
        }
        vec512_t out512[32] = {{{0}}};
        asm_call_capture_vec512_win64(out512, (void *)win64_vaddpd_zmm, args, v512);
        CHECK(out512[0].f64[0] == 11 && out512[0].f64[3] == 44 &&
              out512[0].f64[7] == 88,
              "win64_vaddpd_zmm via _vec512: full 512-bit zmm0 sum (incl upper lanes)");
    } else {
        printf("ok   - win64_vaddpd_zmm via _vec512 # SKIP no AVX-512\n");
    }

    /* asm_call_capture_bigstruct_win64: large struct passed by reference. */
    long long big[4] = {1, 2, 3, 4};
    const long long bs_iargs[1] = {100};
    asm_call_capture_bigstruct_win64(&r, (void *)win64_struct_sum, bs_iargs, 1,
                                     big, sizeof big);
    CHECK(r.ret == 100 + 1 + 2 + 3 + 4,
          "win64_struct_sum via bigstruct: by-reference 32-byte struct read");

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails,
           fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
