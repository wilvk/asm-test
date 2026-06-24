/*
 * robust.s — misbehaving routines for the Phase 8 robustness demo. They prove
 * the runner contains a hang or a crash instead of being taken down by it:
 *   - spin_forever(): never returns; the per-test alarm() must report a timeout.
 *   - crash_null():   stores through a null pointer -> SIGSEGV, reported as a
 *                     failed (and, under fork, isolated) test.
 * Portable across x86-64 and AArch64 (ASM_FUNC handles symbol decoration).
 */
#include "asm.h"

ASM_FUNC spin_forever
#if defined(__x86_64__)
0:  jmp     0b
#elif defined(__aarch64__)
0:  b       0b
#endif
ASM_ENDFUNC spin_forever

ASM_FUNC crash_null
#if defined(__x86_64__)
    xorq    %rax, %rax
    movq    $0, (%rax)
    ret
#elif defined(__aarch64__)
    mov     x0, #0
    str     x0, [x0]
    ret
#endif
ASM_ENDFUNC crash_null
