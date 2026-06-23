/*
 * test_mem.c — exercises memory/string/ordering assertions plus per-suite
 * setup/teardown and skip, against the fill_bytes routine in mem.s.
 */
#include "asmtest.h"

#include <stdlib.h>
#include <string.h>

extern void fill_bytes(void *buf, long val, long n);

#define BUFLEN 16
static unsigned char *buf;

SETUP(mem) {
    buf = malloc(BUFLEN);
    memset(buf, 0, BUFLEN);
}

TEARDOWN(mem) {
    free(buf);
    buf = NULL;
}

TEST(mem, fills_buffer_with_value) {
    fill_bytes(buf, 0xAB, BUFLEN);
    unsigned char expect[BUFLEN];
    memset(expect, 0xAB, BUFLEN);
    ASSERT_MEM_EQ(buf, expect, BUFLEN);
}

TEST(mem, zero_length_leaves_buffer_clear) {
    fill_bytes(buf, 0xFF, 0);
    unsigned char expect[BUFLEN];
    memset(expect, 0x00, BUFLEN);
    ASSERT_MEM_EQ(buf, expect, BUFLEN);
}

TEST(cmp, ordering_relations) {
    ASSERT_LT(3, 5);
    ASSERT_LE(5, 5);
    ASSERT_GT(9, 2);
    ASSERT_GE(7, 7);
}

TEST(cmp, string_equality) {
    ASSERT_STREQ("asm", "asm");
}

TEST(mem, partial_fill_not_yet_specified) {
    SKIP("partial-fill semantics not finalized");
}
