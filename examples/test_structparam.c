/*
 * test_structparam.c — struct-by-value parameters (Phase 6).
 * Small structs pass as their eightbytes through the existing trampolines;
 * large (>16-byte) structs use asm_call_capture_bigstruct.
 */
#include "asmtest.h"

struct pair {
    long a, b;
}; /* 16 bytes -> two INTEGER eightbytes */
struct mixed {
    long a;
    double b;
}; /* 16 bytes -> INTEGER + SSE eightbyte */
struct big {
    long a, b, c;
}; /* 24 bytes -> memory class */

extern long pst2(void);
extern long pst_mixed(void);
extern long bigsum(void);

TEST(structparam, all_int_struct_in_int_regs) {
    /* struct{long,long} by value == two integer args. */
    regs_t r;
    ASM_CALL2(&r, pst2, 30, 12);
    ASSERT_EQ(r.ret, 42);
}

TEST(structparam, mixed_struct_int_plus_sse) {
    /* struct{long; double} by value == one integer arg + one double arg. */
    regs_t r;
    asm_call_capture_fp(&r, (void *)pst_mixed, (long[6]){40},
                        (double[8]){2.0});
    ASSERT_EQ(r.ret, 42);
}

TEST(structparam, large_struct_by_value) {
    /* 24-byte struct: x86-64 copies it onto the stack; AArch64 passes a
     * pointer to it. */
    regs_t r;
    struct big s = {10, 20, 30};
    asm_call_capture_bigstruct(&r, (void *)bigsum, NULL, 0, &s, sizeof s);
    ASSERT_EQ(r.ret, 60);
}

TEST(structparam, large_struct_preserves_callee_saved) {
    regs_t r;
    struct big s = {1, 2, 3};
    asm_call_capture_bigstruct(&r, (void *)bigsum, NULL, 0, &s, sizeof s);
    ASSERT_EQ(r.ret, 6);
    ASSERT_ABI_PRESERVED(&r);
}
