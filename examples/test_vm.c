/*
 * test_vm.c — an UNUSUAL USE CASE: unit-testing a *stateful interpreter* written
 * in assembly, not just a one-shot arithmetic routine.
 *
 * vm_eval walks an RPN byte program, keeps an operand stack on its own frame,
 * and dispatches per opcode. Three things are worth proving about such a routine
 * and each maps onto a framework feature:
 *   1. it computes the right answer for representative programs (plain ASSERT_EQ);
 *   2. it honours the ABI across its loop — every callee-saved register it uses
 *      to hold live state is restored (ASSERT_ABI_PRESERVED on captured regs);
 *   3. it never reads past the program: placing the bytecode flush against a
 *      guard page turns any one-byte over-read into a reported fault, so a clean
 *      pass is positive evidence the cursor stops exactly at code[n].
 */
#include "asmtest.h"

#include <string.h>

extern long vm_eval(const signed char *code, long n);

/* Opcode mnemonics matching the encoding in vm.s. */
#define ADD ((signed char)-1)
#define SUB ((signed char)-2)
#define MUL ((signed char)-3)
#define NEG ((signed char)-4)

/* --- 1. correctness over representative programs ------------------------- */
TEST(vm, pushes_a_literal) {
    signed char prog[] = {42};
    ASSERT_EQ(vm_eval(prog, 1), 42);
}

TEST(vm, empty_program_is_zero) {
    signed char prog[] = {0};
    ASSERT_EQ(vm_eval(prog, 0), 0); /* n=0: nothing executes */
}

TEST(vm, adds_two_literals) {
    signed char prog[] = {3, 4, ADD};
    ASSERT_EQ(vm_eval(prog, 3), 7);
}

TEST(vm, subtraction_keeps_operand_order) {
    signed char prog[] = {10, 3, SUB}; /* a - b, not b - a */
    ASSERT_EQ(vm_eval(prog, 3), 7);
}

TEST(vm, unary_negate) {
    signed char prog[] = {5, NEG};
    ASSERT_EQ(vm_eval(prog, 2), -5);
}

TEST(vm, evaluates_a_nested_expression) {
    /* (3 + 4) * 5 = 35 */
    signed char prog[] = {3, 4, ADD, 5, MUL};
    ASSERT_EQ(vm_eval(prog, 5), 35);
}

TEST(vm, evaluates_a_deeper_expression) {
    /* (2 + 3) * (4 + 5) = 45 */
    signed char prog[] = {2, 3, ADD, 4, 5, ADD, MUL};
    ASSERT_EQ(vm_eval(prog, 7), 45);
}

TEST(vm, ignores_unknown_opcodes) {
    /* -7 is not a defined opcode; it must be skipped, leaving 1 + 2 = 3. */
    signed char prog[] = {1, 2, (signed char)-7, ADD};
    ASSERT_EQ(vm_eval(prog, 4), 3);
}

/* --- 2. ABI discipline across the interpreter loop ----------------------- */
TEST(vm, preserves_callee_saved_registers) {
    static const signed char prog[] = {3, 4, ADD, 5, MUL}; /* 35 */
    regs_t r;
    ASM_CALL2(&r, vm_eval, prog, 5);
    ASSERT_EQ((long)r.ret, 35);
    ASSERT_ABI_PRESERVED(&r); /* rbx/r12..r15 (x86) or x19..x28 restored */
}

/* --- 3. the cursor never runs off the end of the program ----------------- */
TEST(vm, never_reads_past_the_program) {
    /* Copy the program to the very end of a guarded buffer: code[n-1] is the
     * last accessible byte and code[n] is the inaccessible guard page. A correct
     * interpreter touches only code[0..n), so this returns cleanly; an
     * off-by-one in the cursor would fault and be reported as a failure. */
    const signed char src[] = {6, 7, ADD}; /* 13 */
    size_t n = sizeof src;
    signed char *prog = asmtest_guarded_alloc(n);
    memcpy(prog, src, n);
    ASSERT_EQ(vm_eval(prog, (long)n), 13);
    asmtest_guarded_free(prog, n);
}
