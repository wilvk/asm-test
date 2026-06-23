/*
 * test_failure_demo.c — intentionally failing test to show failure reporting.
 * Built and run via `make demo-fail`; not part of the green `make test` build.
 */
#include "asmtest.h"

extern long add_signed(long a, long b);

TEST(demo, this_one_passes) {
    ASSERT_EQ(add_signed(10, 20), 30);
}

TEST(demo, this_one_fails) {
    /* add_signed(2, 2) is 4, not 5 — demonstrates the failure output. */
    ASSERT_EQ(add_signed(2, 2), 5);
}
