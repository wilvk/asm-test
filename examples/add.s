/*
 * add.s — example routine under test (x86-64, System V AMD64 ABI, GAS syntax).
 *
 * long add_signed(long a, long b);
 *   a -> %rdi, b -> %rsi, result -> %rax
 *
 * macOS prefixes C symbols with an underscore, hence _add_signed.
 */
    .text
    .globl _add_signed
_add_signed:
    movq    %rdi, %rax      /* rax = a            */
    addq    %rsi, %rax      /* rax = a + b        */
    ret
