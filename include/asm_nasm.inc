; asm_nasm.inc — portable helpers for the NASM backend (x86-64 only).
;
; The NASM sources are the Intel-syntax counterpart of the GAS .s files, used
; when building with ASM_SYNTAX=nasm. NASM has its own preprocessor, so this is
; %include'd (not the C-preprocessor asm.h).
;
;   ASM_FUNC name       begin a global function `name` (matches C `name`)
;   ASM_ENDFUNC name    end it
;
; Mach-O underscore-prefixes C symbols; ELF does not. __OUTPUT_FORMAT__ lets us
; pick at assemble time so one source serves both.

%ifndef ASMTEST_ASM_NASM_INC
%define ASMTEST_ASM_NASM_INC

%ifidn __OUTPUT_FORMAT__, macho64
    %define SYM(x) _ %+ x
%else
    %define SYM(x) x
%endif

%macro ASM_FUNC 1
    section .text
    global SYM(%1)
SYM(%1):
%endmacro

%macro ASM_ENDFUNC 1
%endmacro

%endif ; ASMTEST_ASM_NASM_INC
