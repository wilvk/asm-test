/*
 * conformance.c — the cross-language conformance corpus, C reference runner.
 *
 * Track 0.4 (multi-language bindings substrate). A fixed set of canonical
 * routines with known captures: the SINGLE SOURCE OF TRUTH for "did a binding
 * wire the ABI up correctly". This C program is the reference — it drives the
 * routines through the binding-ABI entry points (asm_call_capture* and emu_call)
 * and checks each result against the expected literal, the same way a Python or
 * Rust binding will. Every language binding runs this same corpus and must
 * reproduce the same results.
 *
 *   ./conformance          run the corpus, print TAP, exit nonzero on mismatch
 *   ./conformance --emit   write the corpus as JSON (the portable expected-
 *                          results table other bindings consume) to stdout
 *
 * It deliberately does NOT use the ASSERT_* macros (those longjmp into the
 * framework's runner); it validates host-side with plain comparisons and the
 * non-jumping verdict shims (asmtest_check_abi / asmtest_check_flag), exercising
 * exactly the surface a foreign binding uses. Built with -DASMTEST_NO_MAIN so
 * the runtime's own main() does not collide with this driver.
 *
 * run_corpus() (the checks, with teeth) and emit_corpus() (the JSON table) list
 * the same cases with the same expected literals; keep them in sync.
 */
#include <stdio.h>
#include <string.h>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "asmtest.h"
/* The ptrace_descent tier drives the out-of-process single-step stepper + call-descent
 * handle (asmtest_ptrace.h). Self-skips off a ptrace-single-step host (qemu-user / yama)
 * or without Capstone; only runs where the corpus arch matches the host. */
#include "asmtest_ptrace.h"
/* Always available: the cross-arch and raw-bytes emulator cases below run on any
 * host (Unicorn emulates every guest regardless of host arch). Only the
 * host-native x86 emu cases stay gated to an x86-64 host. */
#include "asmtest_emu.h"
/* The in-line assembler tier is optional (needs Keystone). Compiled in only for
 * the `conformance-asm` build (-DASMTEST_ENABLE_ASM, links the assembler lib);
 * the assembler cases are emitted into corpus.json unconditionally so corpus-
 * driven bindings know to replay (or skip) them. */
#ifdef ASMTEST_ENABLE_ASM
#include "asmtest_assemble.h"
#endif

/* Canonical routines under test (examples/{add,flags,fp,simd}.s). Portable
 * across x86-64 and AArch64 via the GAS sources. */
extern long add_signed(long, long);
extern long set_carry(void);
extern long clear_carry(void);
extern long sum_via_rbx(long, long);  /* ABI-compliant: saves/restores rbx */
extern long clobbers_rbx(long, long); /* ABI violation: trashes rbx        */
extern double fp_add(double, double);
/* vec_add4f: lane-wise add of four float32s; vec128 in, vec128 out (xmm0/v0). */
extern long read_fault(const long *); /* loads *p; faults if p is unmapped */
extern double int_to_double(long); /* (double)n into xmm0 from an integer arg */

/* Unmapped guest address the fault case dereferences (clear of the emulator's
 * code/stack maps), so the fault lands at a recognizable fault_addr. */
#define CORPUS_FAULT_ADDR 0x00DEAD00UL

#if defined(__x86_64__)
#define CORPUS_ARCH "x86_64"
#elif defined(__aarch64__)
#define CORPUS_ARCH "aarch64"
#else
#define CORPUS_ARCH "unknown"
#endif

/* ------------------------------------------------------------------ */
/* TAP-ish runner                                                      */
/* ------------------------------------------------------------------ */
static int g_n;    /* cases run    */
static int g_fail; /* cases failed */

static void check(const char *name, int passed, const char *detail) {
    g_n++;
    if (passed) {
        printf("ok %d - %s\n", g_n, name);
    } else {
        g_fail++;
        printf("not ok %d - %s%s%s\n", g_n, name, detail ? " # " : "",
               detail ? detail : "");
    }
}

/* TAP skip (counts as run, never as failed) — the ptrace tier self-skips off a
 * single-step-capable host, like test_ptrace_callout. */
static void skip(const char *name, const char *reason) {
    g_n++;
    printf("ok %d - %s # SKIP %s\n", g_n, name, reason);
}

/* ---- ptrace_descent tier fixture: R calls an in-blob leaf sibling S. --------------
 * Host-native machine code (executed via fork + PTRACE_SINGLESTEP), so the bytes are
 * emitted per host arch and only replay where the corpus arch matches. A rel32/bl to an
 * in-blob sibling keeps the call target reachable from an mmap'd page (libc is not). */
#if defined(__x86_64__)
/* R@0: mov rax,rdi; call S(+4); add rax,rsi; ret.  S@0xc: inc rax; ret. */
static const unsigned char CL_CODE[] = {0x48, 0x89, 0xf8, 0xe8, 0x04, 0x00,
                                        0x00, 0x00, 0x48, 0x01, 0xf0, 0xc3,
                                        0x48, 0xff, 0xc0, 0xc3};
#define CL_HAVE      1
#define CL_REGION    0xc
#define CL_CALL_SITE 0x3
#define CL_LEAF_OFF  0xc
#define CL_LEAF_LEN  4
#define CL_RESULT    43
static const uint64_t CL_FRAME0[] = {0x0, 0x3, 0x8, 0xb};
static const uint64_t CL_LEAF[] = {0x0, 0x3};
#elif defined(__aarch64__)
/* R@0: bl S; add x0,x0,x1; ret.  S@0xc: add x0,x0,#1; ret. */
static const unsigned char CL_CODE[] = {
    0x03, 0x00, 0x00, 0x94, 0x00, 0x00, 0x01, 0x8b, 0xc0, 0x03,
    0x5f, 0xd6, 0x00, 0x04, 0x00, 0x91, 0xc0, 0x03, 0x5f, 0xd6};
#define CL_HAVE      1
#define CL_REGION    0xc
#define CL_CALL_SITE 0x0
#define CL_LEAF_OFF  0xc
#define CL_LEAF_LEN  8
#define CL_RESULT    43
static const uint64_t CL_FRAME0[] = {0x0, 0x4, 0x8};
static const uint64_t CL_LEAF[] = {0x0, 0x4};
#else
#define CL_HAVE 0
#endif

#if CL_HAVE
static int frame_eq(asmtest_descent_t *d, size_t f, const uint64_t *exp,
                    size_t n) {
    if (asmtest_descent_frame_insn_count(d, f) != n)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (asmtest_descent_frame_insn_at(d, f, i) != exp[i])
            return 0;
    return 1;
}

/* Run the two ptrace_descent cases (L1 edges, L2 descend-known). Self-skips cleanly. */
static void run_ptrace_descent(void) {
    const char *n1 = "ptrace_descent.calls_leaf.edges";
    const char *n2 = "ptrace_descent.calls_leaf.descend";
    if (!asmtest_ptrace_available()) {
        char why[160];
        asmtest_ptrace_skip_reason(why, sizeof why);
        skip(n1, why);
        skip(n2, why);
        return;
    }
    if (!asmtest_disas_available()) {
        skip(n1, "no Capstone (call/ret detection)");
        skip(n2, "no Capstone (call/ret detection)");
        return;
    }
    void *p = mmap(NULL, sizeof CL_CODE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        skip(n1, "mmap failed");
        skip(n2, "mmap failed");
        return;
    }
    memcpy(p, CL_CODE, sizeof CL_CODE);
    mprotect(p, sizeof CL_CODE, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof CL_CODE);
    uint64_t base = (uint64_t)(uintptr_t)p;
    long args[2] = {20, 22};
    const size_t NF0 = sizeof CL_FRAME0 / sizeof CL_FRAME0[0];
    const size_t NLF = sizeof CL_LEAF / sizeof CL_LEAF[0];

    /* L1 RECORD_EDGES: R's own body + a single (call -> S) edge. */
    {
        asmtest_descent_t *d =
            asmtest_descent_new(ASMTEST_DESCENT_RECORD_EDGES);
        asmtest_trace_t *t = asmtest_trace_new(64, 64);
        long res = 0;
        int rc =
            asmtest_ptrace_trace_call_ex(p, CL_REGION, args, 2, &res, t, d);
        int ok = rc == ASMTEST_PTRACE_OK && res == CL_RESULT &&
                 asmtest_descent_frames_len(d) == 1 &&
                 frame_eq(d, 0, CL_FRAME0, NF0) &&
                 asmtest_descent_edges_len(d) == 1 &&
                 asmtest_descent_edge_site(d, 0) == CL_CALL_SITE &&
                 asmtest_descent_edge_target(d, 0) == base + CL_LEAF_OFF;
        check(n1, ok, "L1 edge/body mismatch");
        asmtest_trace_free(t);
        asmtest_descent_free(d);
    }
    /* L2 DESCEND_KNOWN: descend the allow-set leaf S as a nested frame. */
    {
        asmtest_descent_t *d =
            asmtest_descent_new(ASMTEST_DESCENT_DESCEND_KNOWN);
        asmtest_descent_allow_region(d, (void *)(uintptr_t)(base + CL_LEAF_OFF),
                                     CL_LEAF_LEN);
        asmtest_trace_t *t = asmtest_trace_new(64, 64);
        long res = 0;
        int rc =
            asmtest_ptrace_trace_call_ex(p, CL_REGION, args, 2, &res, t, d);
        int ok = rc == ASMTEST_PTRACE_OK && res == CL_RESULT &&
                 asmtest_descent_frames_len(d) == 2 &&
                 frame_eq(d, 0, CL_FRAME0, NF0) &&
                 asmtest_descent_frame_base(d, 1) == base + CL_LEAF_OFF &&
                 asmtest_descent_frame_depth(d, 1) == 1 &&
                 frame_eq(d, 1, CL_LEAF, NLF) &&
                 asmtest_descent_edges_len(d) == 0;
        check(n2, ok, "L2 descend/frame mismatch");
        asmtest_trace_free(t);
        asmtest_descent_free(d);
    }
    munmap(p, sizeof CL_CODE);
}
#endif /* CL_HAVE */

static int run_corpus(void) {
    char why[128];

    /* add_signed(40, 2) -> 42, ABI preserved. */
    {
        regs_t r;
        memset(&r, 0, sizeof r);
        long args[6] = {40, 2, 0, 0, 0, 0};
        asm_call_capture(&r, (void *)add_signed, args);
        int ok = (r.ret == 42) && (asmtest_check_abi(&r, why, sizeof why) == 0);
        check("add_signed.basic", ok, r.ret == 42 ? why : "wrong return");
    }

    /* sum_via_rbx(20, 22) -> 42, ABI preserved (saves/restores the reg). */
    {
        regs_t r;
        memset(&r, 0, sizeof r);
        long args[6] = {20, 22, 0, 0, 0, 0};
        asm_call_capture(&r, (void *)sum_via_rbx, args);
        int ok = (r.ret == 42) && (asmtest_check_abi(&r, why, sizeof why) == 0);
        check("sum_via_rbx.abi_preserved", ok, ok ? NULL : why);
    }

    /* clobbers_rbx: the verdict shim must REPORT a violation (returns nonzero). */
    {
        regs_t r;
        memset(&r, 0, sizeof r);
        long args[6] = {1, 2, 0, 0, 0, 0};
        asm_call_capture(&r, (void *)clobbers_rbx, args);
        int verdict = asmtest_check_abi(&r, why, sizeof why);
        check("clobbers_rbx.abi_violation_detected", verdict != 0,
              "expected a violation, shim reported clean");
    }

    /* set_carry sets CF; clear_carry clears it. */
    {
        regs_t r;
        memset(&r, 0, sizeof r);
        asm_call_capture(&r, (void *)set_carry, (long[6]){0});
        int ok =
            asmtest_check_flag(&r, ASMTEST_CF, 1, "CF", why, sizeof why) == 0;
        check("set_carry.cf_set", ok, ok ? NULL : why);
    }
    {
        regs_t r;
        memset(&r, 0, sizeof r);
        asm_call_capture(&r, (void *)clear_carry, (long[6]){0});
        int ok =
            asmtest_check_flag(&r, ASMTEST_CF, 0, "CF", why, sizeof why) == 0;
        check("clear_carry.cf_clear", ok, ok ? NULL : why);
    }

    /* fp_add(1.5, 2.25) -> 3.75 (exact). */
    {
        regs_t r;
        memset(&r, 0, sizeof r);
        long iargs[6] = {0};
        double fargs[8] = {1.5, 2.25, 0, 0, 0, 0, 0, 0};
        asm_call_capture_fp(&r, (void *)fp_add, iargs, fargs);
        check("fp_add.basic", r.fret == 3.75, "wrong FP return");
    }

    /* vec_add4f({1,2,3,4}, {10,20,30,40}) -> {11,22,33,44} (exact). */
    {
        regs_t r;
        memset(&r, 0, sizeof r);
        long iargs[6] = {0};
        vec128_t v[8];
        memset(v, 0, sizeof v);
        v[0].f32[0] = 1;
        v[0].f32[1] = 2;
        v[0].f32[2] = 3;
        v[0].f32[3] = 4;
        v[1].f32[0] = 10;
        v[1].f32[1] = 20;
        v[1].f32[2] = 30;
        v[1].f32[3] = 40;
        extern void vec_add4f(void);
        asm_call_capture_vec(&r, (void *)vec_add4f, iargs, v);
        int ok = r.vec[0].f32[0] == 11 && r.vec[0].f32[1] == 22 &&
                 r.vec[0].f32[2] == 33 && r.vec[0].f32[3] == 44;
        check("vec_add4f.basic", ok, "wrong vector lanes");
    }

#if defined(__x86_64__)
    /* Emulator tier (x86-64 guest drives host-compiled bytes): add_signed runs
     * to `ret`, no fault, rax == 42. Cross-arch emu uses raw-byte guests, out of
     * scope for this offset-corpus; gated to an x86-64 host like `make emu-test`. */
    {
        emu_t *e = emu_open();
        if (!e) {
            check("emu.add_signed", 0, "emu_open failed");
        } else {
            emu_result_t res;
            memset(&res, 0, sizeof res);
            long args[2] = {40, 2};
            bool ran = emu_call(e, (void *)add_signed, 64, args, 2, 0, &res);
            int ok = ran && !res.faulted && res.regs.rax == 42;
            check("emu.add_signed", ok, ok ? NULL : "emu result mismatch");

            /* read_fault dereferences an unmapped address: the run does not
             * complete cleanly, but the fault is data — where (fault_addr) and
             * why (fault_kind == read), not a crash. */
            emu_result_t fres;
            memset(&fres, 0, sizeof fres);
            long fargs[1] = {(long)CORPUS_FAULT_ADDR};
            emu_call(e, (void *)read_fault, 64, fargs, 1, 0, &fres);
            int fok = fres.faulted && fres.fault_addr == CORPUS_FAULT_ADDR &&
                      fres.fault_kind == EMU_FAULT_READ;
            check("emu.read_fault", fok,
                  fok ? NULL : "fault not reported as data");

            /* int_to_double(42) lands (double)42 in xmm0 — the guest XMM file,
             * beyond the GP registers — and a clean run keeps rip/rflags live
             * (x86 always holds rflags bit 1 set), so the widened register reads
             * resolve the FP/vector lanes and rip/rflags, not just rax..r15. */
            emu_result_t xres;
            memset(&xres, 0, sizeof xres);
            long xargs[1] = {42};
            emu_call(e, (void *)int_to_double, 64, xargs, 1, 0, &xres);
            int xok = !xres.faulted && xres.regs.xmm[0].f64[0] == 42.0 &&
                      xres.regs.rip != 0 && (xres.regs.rflags & 0x2u) != 0;
            check("emu.int_to_double", xok,
                  xok ? NULL : "xmm/rip/rflags read mismatch");
            emu_close(e);
        }
    }
#endif

    /* ------------------------------------------------------------------ */
    /* Cross-arch & raw-bytes emulator tier — driven through the opaque-   */
    /* handle FFI surface (asmtest_emu_*), exactly as a foreign binding    */
    /* does. These guests run raw machine-code bytes, so they emulate on   */
    /* ANY host (x86-64 or AArch64), unlike the host-native cases above.   */
    /* The bytes were assembled once and are checked in as literals, so    */
    /* the base build needs no Keystone (and RISC-V, absent from many      */
    /* Keystone builds, still runs under Unicorn).                         */
    /* ------------------------------------------------------------------ */

    /* AArch64 `add x0, x0, x1; ret`, args (40, 2) -> x0 == 42. */
    {
        static const unsigned char code[] = {0x00, 0x00, 0x01, 0x8B,
                                             0xC0, 0x03, 0x5F, 0xD6};
        emu_arm64_t *e = emu_arm64_open();
        emu_arm64_result_t *r = asmtest_emu_arm64_result_new();
        long args[2] = {40, 2};
        int ran = e && r && emu_arm64_call(e, code, sizeof code, args, 2, 0, r);
        check("emu_arm64.add",
              ran && !asmtest_emu_result_faulted((emu_result_t *)r) &&
                  asmtest_emu_arm64_reg(r, "x0") == 42,
              "arm64 add mismatch");
        asmtest_emu_arm64_result_free(r);
        if (e)
            emu_arm64_close(e);
    }

    /* RISC-V (RV64) `add a0, a0, a1; ret`, args (40, 2) -> a0 (x10) == 42. */
    {
        static const unsigned char code[] = {0x33, 0x05, 0xB5, 0x00,
                                             0x67, 0x80, 0x00, 0x00};
        emu_riscv_t *e = emu_riscv_open();
        emu_riscv_result_t *r = asmtest_emu_riscv_result_new();
        long args[2] = {40, 2};
        int ran = e && r && emu_riscv_call(e, code, sizeof code, args, 2, 0, r);
        check("emu_riscv.add",
              ran && !asmtest_emu_result_faulted((emu_result_t *)r) &&
                  asmtest_emu_riscv_reg(r, "a0") == 42,
              "riscv add mismatch");
        asmtest_emu_riscv_result_free(r);
        if (e)
            emu_riscv_close(e);
    }

    /* ARM32 (A32) `add r0, r0, r1; bx lr`, args (40, 2) -> r0 == 42. */
    {
        static const unsigned char code[] = {0x01, 0x00, 0x80, 0xE0,
                                             0x1E, 0xFF, 0x2F, 0xE1};
        emu_arm_t *e = emu_arm_open();
        emu_arm_result_t *r = asmtest_emu_arm_result_new();
        long args[2] = {40, 2};
        int ran = e && r && emu_arm_call(e, code, sizeof code, args, 2, 0, r);
        check("emu_arm.add",
              ran && !asmtest_emu_result_faulted((emu_result_t *)r) &&
                  asmtest_emu_arm_reg(r, "r0") == 42,
              "arm add mismatch");
        asmtest_emu_arm_result_free(r);
        if (e)
            emu_arm_close(e);
    }

    /* x86-64 raw bytes under the emulator (host-portable via Unicorn): the new
     * scalar-arg wrappers for wide integer args, FP args, vector args, and the
     * Win64 convention. Each code window is padded to 64 bytes (the wrappers
     * copy a fixed 64-byte window). */
    {
        emu_t *e = emu_open();
        emu_result_t *r = asmtest_emu_result_new();
        if (!e || !r) {
            check("emu.wide_int", 0, "emu_open failed");
        } else {
            /* `mov rax,rdi; add rax,rsi; add rax,rdx; ret`, three args via the
             * 6-arg wrapper -> rax == 42 (more than the 2 args of call2). */
            static const unsigned char wi[64] = {0x48, 0x89, 0xF8, 0x48, 0x01,
                                                 0xF0, 0x48, 0x01, 0xD0, 0xC3};
            asmtest_emu_call6(e, wi, 10, 20, 12, 0, 0, 0, 3, 0, r);
            check("emu.wide_int", asmtest_emu_x86_reg(r, "rax") == 42,
                  "wide-int mismatch");

            /* `addsd xmm0, xmm1; ret`, two doubles via the FP wrapper -> 3.75. */
            static const unsigned char fb[64] = {0xF2, 0x0F, 0x58, 0xC1, 0xC3};
            asmtest_emu_call_fp2(e, fb, 1.5, 2.25, r);
            check("emu.fp_add", asmtest_emu_x86_xmm_f64(r, 0, 0) == 3.75,
                  "emu fp mismatch");

            /* `addps xmm0, xmm1; ret`, two vectors via the vector wrapper. */
            static const unsigned char vb[64] = {0x0F, 0x58, 0xC1, 0xC3};
            float lanes[8] = {1, 2, 3, 4, 10, 20, 30, 40};
            asmtest_emu_call_vec_f32(e, vb, lanes, 2, r);
            check("emu.vec_add4f",
                  asmtest_emu_x86_xmm_f32(r, 0, 0) == 11 &&
                      asmtest_emu_x86_xmm_f32(r, 0, 1) == 22 &&
                      asmtest_emu_x86_xmm_f32(r, 0, 2) == 33 &&
                      asmtest_emu_x86_xmm_f32(r, 0, 3) == 44,
                  "emu vec mismatch");

            /* Win64 convention: `mov rax,rcx; add rax,rdx; ret`, args in rcx/rdx
             * -> rax == 42 (a Win64 routine tested on a System V host). */
            static const unsigned char wb[64] = {0x48, 0x89, 0xC8, 0x48,
                                                 0x01, 0xD0, 0xC3};
            asmtest_emu_call_win64_6(e, wb, 40, 2, 0, 0, 2, 0, r);
            check("emu.win64_add", asmtest_emu_x86_reg(r, "rax") == 42,
                  "win64 mismatch");
        }
        asmtest_emu_result_free(r);
        if (e)
            emu_close(e);
    }

    /* AArch64 trace / basic-block coverage: a two-block select routine
     * `x0 == 0 ? 42 : 99`. With x0 = 0 the entry block (offset 0) and the
     * .zero block (offset 12) are entered, while the x0 != 0 block (offset 4)
     * is not — coverage reported as data through the opaque trace handle. */
    {
        static const unsigned char code[] = {
            0x60, 0x00, 0x00, 0xB4, 0x60, 0x0C, 0x80, 0xD2, 0xC0, 0x03,
            0x5F, 0xD6, 0x40, 0x05, 0x80, 0xD2, 0xC0, 0x03, 0x5F, 0xD6};
        emu_arm64_t *e = emu_arm64_open();
        emu_arm64_result_t *r = asmtest_emu_arm64_result_new();
        emu_trace_t *tr = asmtest_emu_trace_new(64, 64);
        long args[1] = {0};
        int ran =
            e && r && tr &&
            emu_arm64_call_traced(e, code, sizeof code, args, 1, 0, r, tr);
        int ok = ran && !asmtest_emu_result_faulted((emu_result_t *)r) &&
                 asmtest_emu_arm64_reg(r, "x0") == 42 &&
                 asmtest_emu_trace_covered(tr, 0) &&
                 asmtest_emu_trace_covered(tr, 12) &&
                 !asmtest_emu_trace_covered(tr, 4);
        check("emu_arm64.trace_sel", ok, "arm64 trace/coverage mismatch");
        asmtest_emu_trace_free(tr);
        asmtest_emu_arm64_result_free(r);
        if (e)
            emu_arm64_close(e);
    }

#ifdef ASMTEST_ENABLE_ASM
    /* ------------------------------------------------------------------ */
    /* In-line assembler tier (Keystone) — assemble source text, then run  */
    /* it on the emulator or emit raw bytes. Compiled only for the         */
    /* conformance-asm build; the bindings test this same surface.         */
    /* ------------------------------------------------------------------ */
    {
        emu_t *e = emu_open();
        if (!e) {
            check("asm.add_signed", 0, "emu_open failed");
        } else {
            emu_result_t res;
            memset(&res, 0, sizeof res);
            int ok = asmtest_emu_call_asm6(e, "mov rax, rdi; add rax, rsi; ret",
                                           ASM_SYNTAX_INTEL, 40, 2, 0, 0, 0, 0,
                                           2, 0, &res);
            check("asm.add_signed", ok && !res.faulted && res.regs.rax == 42,
                  "asm add_signed mismatch");

            /* Widened shim: AT&T syntax + a third arg (rdi+rsi+rdx). */
            memset(&res, 0, sizeof res);
            ok = asmtest_emu_call_asm6(
                e, "mov %rdi, %rax; add %rsi, %rax; add %rdx, %rax; ret",
                ASM_SYNTAX_ATT, 10, 20, 12, 0, 0, 0, 3, 0, &res);
            check("asm.att_3arg", ok && res.regs.rax == 42,
                  "asm att_3arg mismatch");

            /* Failure path: a bad string fails (0) and leaves a diagnostic. */
            memset(&res, 0, sizeof res);
            int bad = asmtest_emu_call_asm6(e, "mov rax, nonsense_token",
                                            ASM_SYNTAX_INTEL, 0, 0, 0, 0, 0, 0,
                                            0, 0, &res);
            check("asm.bad_source",
                  bad == 0 && asmtest_asm_last_error()[0] != 0,
                  "asm bad source should fail with a diagnostic");
            emu_close(e);
        }

        /* Multi-arch assemble-to-bytes: AArch64 `ret` is C0 03 5F D6. */
        unsigned char buf[16];
        int n = asmtest_asm_bytes(ASM_ARM64, ASM_SYNTAX_INTEL, "ret",
                                  0x00100000, buf, sizeof buf);
        check("asm.arm64_bytes", n == 4 && buf[0] == 0xC0 && buf[3] == 0xD6,
              "asm arm64 bytes mismatch");
    }
#endif /* ASMTEST_ENABLE_ASM */

    /* ptrace_descent tier: the out-of-process stepper's call-descent (L1 edges, L2
     * descend-known). Host-native, so it runs only where the corpus arch matches; it
     * self-skips off a single-step-capable host. */
#if CL_HAVE
    run_ptrace_descent();
#else
    skip("ptrace_descent.calls_leaf.edges", "no ptrace stepper on this arch");
    skip("ptrace_descent.calls_leaf.descend", "no ptrace stepper on this arch");
#endif

    return g_fail;
}

#if CL_HAVE
/* Emit a byte array / u64 array as a JSON int list (ptrace_descent tier only). */
static void emit_bytes(const unsigned char *b, size_t n) {
    printf("[");
    for (size_t i = 0; i < n; i++)
        printf("%s%d", i ? ", " : "", (int)b[i]);
    printf("]");
}
static void emit_u64s(const uint64_t *a, size_t n) {
    printf("[");
    for (size_t i = 0; i < n; i++)
        printf("%s%llu", i ? ", " : "", (unsigned long long)a[i]);
    printf("]");
}
#endif

/* ------------------------------------------------------------------ */
/* JSON emission — the portable expected-results table                 */
/* ------------------------------------------------------------------ */
static void emit_corpus(void) {
    printf("{\n");
    printf("  \"corpus\": {\n");
    printf("    \"version\": \"%s\",\n", ASMTEST_VERSION);
    printf("    \"arch\": \"%s\",\n", CORPUS_ARCH);
    printf("    \"cases\": [\n");

    printf("      {\"name\": \"add_signed.basic\", \"tier\": \"capture\", "
           "\"call\": \"int\", \"routine\": \"add_signed\", "
           "\"args\": [40, 2], "
           "\"expect\": {\"ret\": 42, \"abi_preserved\": true}},\n");
    printf("      {\"name\": \"sum_via_rbx.abi_preserved\", "
           "\"tier\": \"capture\", \"call\": \"int\", "
           "\"routine\": \"sum_via_rbx\", \"args\": [20, 22], "
           "\"expect\": {\"ret\": 42, \"abi_preserved\": true}},\n");
    printf("      {\"name\": \"clobbers_rbx.abi_violation_detected\", "
           "\"tier\": \"capture\", \"call\": \"int\", "
           "\"routine\": \"clobbers_rbx\", \"args\": [1, 2], "
           "\"expect\": {\"abi_preserved\": false}},\n");
    printf("      {\"name\": \"set_carry.cf_set\", \"tier\": \"capture\", "
           "\"call\": \"int\", \"routine\": \"set_carry\", \"args\": [], "
           "\"expect\": {\"flags_set\": [\"CF\"]}},\n");
    printf("      {\"name\": \"clear_carry.cf_clear\", \"tier\": \"capture\", "
           "\"call\": \"int\", \"routine\": \"clear_carry\", \"args\": [], "
           "\"expect\": {\"flags_clear\": [\"CF\"]}},\n");
    printf("      {\"name\": \"fp_add.basic\", \"tier\": \"capture\", "
           "\"call\": \"fp\", \"routine\": \"fp_add\", \"fargs\": [1.5, 2.25], "
           "\"expect\": {\"fret\": 3.75}},\n");
    printf("      {\"name\": \"vec_add4f.basic\", \"tier\": \"capture\", "
           "\"call\": \"vec\", \"routine\": \"vec_add4f\", "
           "\"vargs\": [[1, 2, 3, 4], [10, 20, 30, 40]], "
           "\"expect\": {\"vret_f32\": [11, 22, 33, 44]}}");
#if defined(__x86_64__)
    printf(",\n      {\"name\": \"emu.add_signed\", \"tier\": \"emu\", "
           "\"guest\": \"x86_64\", \"routine\": \"add_signed\", "
           "\"args\": [40, 2], "
           "\"expect\": {\"reg\": {\"rax\": 42}, \"faulted\": false}},\n");
    printf("      {\"name\": \"emu.read_fault\", \"tier\": \"emu\", "
           "\"guest\": \"x86_64\", \"routine\": \"read_fault\", "
           "\"args\": [%lu], "
           "\"expect\": {\"faulted\": true, \"fault_addr\": %lu, "
           "\"fault_kind\": %d}},\n",
           CORPUS_FAULT_ADDR, CORPUS_FAULT_ADDR, EMU_FAULT_READ);
    printf("      {\"name\": \"emu.int_to_double\", \"tier\": \"emu\", "
           "\"guest\": \"x86_64\", \"routine\": \"int_to_double\", "
           "\"args\": [42], "
           "\"expect\": {\"faulted\": false, \"xmm_f64\": {\"0\": 42.0}}}");
#endif

    /* Cross-arch & raw-bytes emulator tier (host-portable: emitted on every
     * host). `code` is the routine's machine-code bytes; `guest` picks the
     * emulator engine. These anchor the binding parity for the cross-arch
     * guests, the wide/FP/vector/Win64 emu calls, and trace coverage. */
    printf(",\n      {\"name\": \"emu_arm64.add\", \"tier\": \"emu_bytes\", "
           "\"guest\": \"arm64\", \"call\": \"int\", "
           "\"code\": [0, 0, 1, 139, 192, 3, 95, 214], \"args\": [40, 2], "
           "\"expect\": {\"reg\": {\"x0\": 42}, \"faulted\": false}},\n");
    printf("      {\"name\": \"emu_riscv.add\", \"tier\": \"emu_bytes\", "
           "\"guest\": \"riscv\", \"call\": \"int\", "
           "\"code\": [51, 5, 181, 0, 103, 128, 0, 0], \"args\": [40, 2], "
           "\"expect\": {\"reg\": {\"a0\": 42}, \"faulted\": false}},\n");
    printf("      {\"name\": \"emu_arm.add\", \"tier\": \"emu_bytes\", "
           "\"guest\": \"arm\", \"call\": \"int\", "
           "\"code\": [1, 0, 128, 224, 30, 255, 47, 225], \"args\": [40, 2], "
           "\"expect\": {\"reg\": {\"r0\": 42}, \"faulted\": false}},\n");
    printf("      {\"name\": \"emu.wide_int\", \"tier\": \"emu_bytes\", "
           "\"guest\": \"x86_64\", \"call\": \"int\", "
           "\"code\": [72, 137, 248, 72, 1, 240, 72, 1, 208, 195], "
           "\"args\": [10, 20, 12], "
           "\"expect\": {\"reg\": {\"rax\": 42}, \"faulted\": false}},\n");
    printf("      {\"name\": \"emu.fp_add\", \"tier\": \"emu_bytes\", "
           "\"guest\": \"x86_64\", \"call\": \"fp\", "
           "\"code\": [242, 15, 88, 193, 195], \"fargs\": [1.5, 2.25], "
           "\"expect\": {\"xmm_f64\": {\"0\": 3.75}, \"faulted\": false}},\n");
    printf(
        "      {\"name\": \"emu.vec_add4f\", \"tier\": \"emu_bytes\", "
        "\"guest\": \"x86_64\", \"call\": \"vec\", "
        "\"code\": [15, 88, 193, 195], "
        "\"vargs\": [[1, 2, 3, 4], [10, 20, 30, 40]], "
        "\"expect\": {\"vret_f32\": [11, 22, 33, 44], \"faulted\": false}},\n");
    printf("      {\"name\": \"emu.win64_add\", \"tier\": \"emu_bytes\", "
           "\"guest\": \"x86_64_win64\", \"call\": \"int\", "
           "\"code\": [72, 137, 200, 72, 1, 208, 195], \"args\": [40, 2], "
           "\"expect\": {\"reg\": {\"rax\": 42}, \"faulted\": false}},\n");
    printf("      {\"name\": \"emu_arm64.trace_sel\", \"tier\": \"emu_trace\", "
           "\"guest\": \"arm64\", \"call\": \"int\", "
           "\"code\": [96, 0, 0, 180, 96, 12, 128, 210, 192, 3, 95, 214, "
           "64, 5, 128, 210, 192, 3, 95, 214], \"args\": [0], "
           "\"expect\": {\"reg\": {\"x0\": 42}, \"covered\": [0, 12], "
           "\"uncovered\": [4], \"faulted\": false}},\n");

    /* In-line assembler tier — emitted unconditionally so a binding can replay
     * (or self-skip when its loaded lib has no assembler). `call` selects the
     * shape: run assembled text on the emulator, expect an assemble error, or
     * assemble-to-bytes for any arch. */
    printf("      {\"name\": \"asm.add_signed\", \"tier\": \"asm\", "
           "\"call\": \"run\", \"syntax\": \"intel\", "
           "\"src\": \"mov rax, rdi; add rax, rsi; ret\", \"args\": [40, 2], "
           "\"expect\": {\"reg\": {\"rax\": 42}, \"faulted\": false}},\n");
    printf("      {\"name\": \"asm.att_3arg\", \"tier\": \"asm\", "
           "\"call\": \"run\", \"syntax\": \"att\", "
           "\"src\": \"mov %%rdi, %%rax; add %%rsi, %%rax; add %%rdx, %%rax; "
           "ret\", \"args\": [10, 20, 12], "
           "\"expect\": {\"reg\": {\"rax\": 42}, \"faulted\": false}},\n");
    printf("      {\"name\": \"asm.bad_source\", \"tier\": \"asm\", "
           "\"call\": \"error\", \"syntax\": \"intel\", "
           "\"src\": \"mov rax, nonsense_token\", "
           "\"expect\": {\"error\": true}},\n");
    printf("      {\"name\": \"asm.arm64_bytes\", \"tier\": \"asm\", "
           "\"call\": \"assemble\", \"arch\": \"arm64\", \"src\": \"ret\", "
           "\"expect\": {\"bytes\": [192, 3, 95, 214]}}");

    /* ptrace_descent tier — host-native call-descent (emitted only on an arch with a
     * ptrace stepper; the `arch` field lets a binding on a different host skip it). */
#if CL_HAVE
    printf(",\n      {\"name\": \"ptrace_descent.calls_leaf.edges\", "
           "\"tier\": \"ptrace_descent\", \"arch\": \"%s\", \"level\": 1, "
           "\"code\": ",
           CORPUS_ARCH);
    emit_bytes(CL_CODE, sizeof CL_CODE);
    printf(", \"region\": %d, \"args\": [20, 22], \"expect\": {\"result\": %d, "
           "\"frame0\": ",
           (int)CL_REGION, CL_RESULT);
    emit_u64s(CL_FRAME0, sizeof CL_FRAME0 / sizeof CL_FRAME0[0]);
    printf(", \"edges\": [{\"site\": %d, \"target_off\": %d}]}},\n",
           (int)CL_CALL_SITE, (int)CL_LEAF_OFF);
    printf("      {\"name\": \"ptrace_descent.calls_leaf.descend\", "
           "\"tier\": \"ptrace_descent\", \"arch\": \"%s\", \"level\": 2, "
           "\"code\": ",
           CORPUS_ARCH);
    emit_bytes(CL_CODE, sizeof CL_CODE);
    printf(", \"region\": %d, \"allow_off\": %d, \"allow_len\": %d, \"args\": "
           "[20, 22], "
           "\"expect\": {\"result\": %d, \"frame0\": ",
           (int)CL_REGION, (int)CL_LEAF_OFF, (int)CL_LEAF_LEN, CL_RESULT);
    emit_u64s(CL_FRAME0, sizeof CL_FRAME0 / sizeof CL_FRAME0[0]);
    printf(", \"frames\": [{\"base_off\": %d, \"depth\": 1, \"insns\": ",
           (int)CL_LEAF_OFF);
    emit_u64s(CL_LEAF, sizeof CL_LEAF / sizeof CL_LEAF[0]);
    printf("}], \"edges\": []}}");
#endif

    printf("\n    ]\n");
    printf("  }\n}\n");
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--emit") == 0) {
        emit_corpus();
        return 0;
    }
    int fail = run_corpus();
    printf("# %d passed, %d failed, %d total\n", g_n - fail, fail, g_n);
    return fail ? 1 : 0;
}
