/*
 * test_struct.c — struct return (Phase 6).
 * Large structs come back via the hidden result pointer (asm_call_capture_sret /
 * ASM_SRET); small structs come back in registers we already capture.
 */
#include "asmtest.h"

#include <stdint.h>

struct big {
    long a, b, c;
}; /* 24 bytes > 16 -> memory class */
struct pair {
    long a, b;
}; /* 16 bytes -> register class */

extern void make_big(void);  /* struct big make_big(long, long, long)  */
extern void make_pair(void); /* struct pair make_pair(long, long)      */

TEST(structret, large_struct_via_sret) {
    regs_t r;
    struct big result = {0, 0, 0};
    long args[] = {10, 20, 30};
    asm_call_capture_sret(&r, (void *)make_big, &result, args, 3);
    ASSERT_EQ(result.a, 10);
    ASSERT_EQ(result.b, 20);
    ASSERT_EQ(result.c, 30);
    /* make_big also returns the result pointer in rax/x0. */
    ASSERT_UEQ(r.ret, (unsigned long)(uintptr_t)&result);
}

TEST(structret, sret_via_macro) {
    regs_t r;
    struct big result = {0, 0, 0};
    ASM_SRET(&r, make_big, &result, 7, 8, 9);
    ASSERT_EQ(result.a, 7);
    ASSERT_EQ(result.b, 8);
    ASSERT_EQ(result.c, 9);
}

TEST(structret, small_struct_in_registers) {
    /* A <=16-byte all-integer struct returns in rax:rdx (x0:x1) — already in
     * regs_t, no special call needed. */
    regs_t r;
    ASM_CALL2(&r, make_pair, 7, 9);
    ASSERT_EQ(r.ret, 7); /* first eightbyte (rax / x0) */
#if defined(__x86_64__)
    ASSERT_EQ(r.rdx,
              9); /* second eightbyte (rdx); x1 not captured on AArch64 */
#endif
}
