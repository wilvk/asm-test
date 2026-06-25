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

#include "asmtest.h"
#if defined(__x86_64__)
#include "asmtest_emu.h"
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
extern double int_to_double(long);    /* (double)n into xmm0 from an integer arg */

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
        int ok = asmtest_check_flag(&r, ASMTEST_CF, 1, "CF", why, sizeof why) == 0;
        check("set_carry.cf_set", ok, ok ? NULL : why);
    }
    {
        regs_t r;
        memset(&r, 0, sizeof r);
        asm_call_capture(&r, (void *)clear_carry, (long[6]){0});
        int ok = asmtest_check_flag(&r, ASMTEST_CF, 0, "CF", why, sizeof why) == 0;
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
            check("emu.read_fault", fok, fok ? NULL : "fault not reported as data");

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
            check("emu.int_to_double", xok, xok ? NULL : "xmm/rip/rflags read mismatch");
            emu_close(e);
        }
    }
#endif

    return g_fail;
}

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
