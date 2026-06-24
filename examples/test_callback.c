/*
 * test_callback.c — example suite for the libc-callback routines in callback.s.
 *
 * These routines take a C function pointer and invoke it per array element, the
 * same shape as a qsort() comparator or a map/filter helper. The point is that
 * an assembly routine can call *back* into C correctly: the framework just calls
 * the routine with ordinary C function pointers and asserts on the result.
 */
#include "asmtest.h"

extern long sum_map(const long *arr, long n, long (*fn)(long));
extern long count_if(const long *arr, long n, long (*pred)(long));

/* Callbacks the assembly invokes per element. */
static long dbl(long x) { return x * 2; }
static long square(long x) { return x * x; }
static long is_even(long x) { return (x % 2) == 0; }
static long is_positive(long x) { return x > 0; }

TEST(callback, sum_map_doubles) {
    long a[] = {1, 2, 3, 4, 5};
    ASSERT_EQ(sum_map(a, 5, dbl), 30); /* 2*(1+2+3+4+5) */
}

TEST(callback, sum_map_squares) {
    long a[] = {1, 2, 3, 4};
    ASSERT_EQ(sum_map(a, 4, square), 1 + 4 + 9 + 16);
}

TEST(callback, sum_map_empty_is_zero) {
    long a[] = {7}; /* n=0: callback must never run */
    ASSERT_EQ(sum_map(a, 0, dbl), 0);
}

TEST(callback, sum_map_negative_n_is_zero) {
    long a[] = {7};
    ASSERT_EQ(sum_map(a, -3, dbl), 0); /* defensive: n<=0 */
}

TEST(callback, count_if_even) {
    long a[] = {1, 2, 3, 4, 5, 6};
    ASSERT_EQ(count_if(a, 6, is_even), 3);
}

TEST(callback, count_if_positive) {
    long a[] = {-2, -1, 0, 1, 2};
    ASSERT_EQ(count_if(a, 5, is_positive), 2);
}

TEST(callback, count_if_none) {
    long a[] = {1, 3, 5};
    ASSERT_EQ(count_if(a, 3, is_even), 0);
}

/* A callback that itself clobbers the caller-saved registers and recurses back
 * into another asm-callback routine: a stress for stack alignment and
 * callee-saved preservation across the call boundary. */
static const long g_vals[] = {2, 4, 6, 8};
static long count_even_inner(long x) {
    /* call back into the framework's other routine from inside a callback */
    return count_if(g_vals, 4, is_even) + (x & 1); /* 4 + (x&1) */
}

TEST(callback, nested_callback_preserves_abi) {
    long a[] = {1, 2}; /* sum of (4+1) + (4+0) = 9 */
    ASSERT_EQ(sum_map(a, 2, count_even_inner), 9);
}
