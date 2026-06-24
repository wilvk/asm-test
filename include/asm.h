/*
 * asm.h — portable assembly helpers for x86-64 and AArch64.
 *
 * Included by the assembly sources (assembled with the C preprocessor, via
 * `-x assembler-with-cpp`). Hides the symbol-decoration differences between
 * object formats so one source builds everywhere:
 *   - Mach-O (macOS): C symbols are underscore-prefixed; no .type/.size.
 *   - ELF (Linux):    no prefix; functions annotated with .type/.size, and the
 *                     type symbol is @function on x86 but %function on ARM.
 *
 * These are assembler (.macro) macros, not cpp macros, because cpp cannot emit
 * the newlines a multi-directive expansion needs (and ';' is a comment, not a
 * statement separator, on AArch64). The C preprocessor still selects which
 * definition below is emitted for the target.
 *
 *   ASM_FUNC name         begin a global function `name` (matches C `name`)
 *   ASM_ENDFUNC name      end it (emits .size on ELF)
 */
#ifndef ASMTEST_ASM_H
#define ASMTEST_ASM_H

#if defined(__APPLE__)

    .macro ASM_FUNC name
    .text
    .globl _\name
_\name:
    .endm

    .macro ASM_ENDFUNC name
    .endm

#elif defined(__aarch64__) /* ELF, AArch64 */

    .macro ASM_FUNC name
    .text
    .globl \name
    .type \name, %function
\name:
    .endm

    .macro ASM_ENDFUNC name
    .size \name, .-\name
    .endm

#else /* ELF, x86-64 */

    .macro ASM_FUNC name
    .text
    .globl \name
    .type \name, @function
\name:
    .endm

    .macro ASM_ENDFUNC name
    .size \name, .-\name
    .endm

#endif

/* Mark the stack non-executable (ELF/Linux). Without a .note.GNU-stack section
 * the linker warns "missing .note.GNU-stack section implies executable stack"
 * and may map an executable stack. Emitted once per translation unit (asm.h has
 * an include guard); ASM_FUNC re-selects .text for the code that follows. The
 * section type prefix is %progbits on ARM, @progbits elsewhere — mirroring the
 * %function/@function split above. Not applicable to Mach-O (macOS). */
#if !defined(__APPLE__)
# if defined(__aarch64__) || defined(__arm__)
    .section .note.GNU-stack,"",%progbits
# else
    .section .note.GNU-stack,"",@progbits
# endif
    .text
#endif

#endif /* ASMTEST_ASM_H */
