/*
 * test_arith.c — example test suite for the add_signed routine in add.s.
 */
#include "asmtest.h"

extern long add_signed(long a, long b);

TEST(arith, adds_two_positives) {
    ASSERT_EQ(add_signed(2, 3), 5);
}

TEST(arith, adds_negative) {
    ASSERT_EQ(add_signed(-4, 1), -3);
}

TEST(arith, identity_with_zero) {
    ASSERT_EQ(add_signed(42, 0), 42);
}

TEST(arith, result_differs_from_input) {
    ASSERT_NE(add_signed(7, 1), 7);
}
