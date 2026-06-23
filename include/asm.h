/*
 * asm.h — portable assembly helpers for x86-64 (System V AMD64 ABI).
 *
 * Included by the assembly sources (assembled with the C preprocessor, via
 * `-x assembler-with-cpp`). The x86-64 SysV calling convention
 * is identical on Linux and macOS, so only symbol decoration differs: Mach-O
 * prefixes C symbols with an underscore and has no .type/.size; ELF does not
 * prefix and annotates function symbols. These macros hide that difference so
 * one assembly source builds on both.
 *
 *   ASM_FUNC(name)        begin a global function `name` (matches C `name`)
 *   ASM_ENDFUNC(name)     end it (emits .size on ELF)
 */
#ifndef ASMTEST_ASM_H
#define ASMTEST_ASM_H

#if defined(__APPLE__)
#  define ASM_SYM(name) _##name
#else
#  define ASM_SYM(name) name
#endif

#if defined(__APPLE__)
#  define ASM_FUNC(name)                                                       \
        .text                                                                ;\
        .globl ASM_SYM(name)                                                 ;\
        ASM_SYM(name):
#  define ASM_ENDFUNC(name)
#else
#  define ASM_FUNC(name)                                                       \
        .text                                                                ;\
        .globl ASM_SYM(name)                                                 ;\
        .type ASM_SYM(name), @function                                       ;\
        ASM_SYM(name):
#  define ASM_ENDFUNC(name)                                                    \
        .size ASM_SYM(name), .-ASM_SYM(name)
#endif

#endif /* ASMTEST_ASM_H */
