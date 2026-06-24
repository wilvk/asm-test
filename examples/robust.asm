; robust.asm — NASM (Intel-syntax) counterpart of robust.s (x86-64).
;
; spin_forever(): never returns (the per-test timeout must catch it).
; crash_null():   stores through a null pointer -> SIGSEGV.
%include "asm_nasm.inc"

ASM_FUNC spin_forever
.spin:
    jmp     .spin
ASM_ENDFUNC spin_forever

ASM_FUNC crash_null
    xor     rax, rax
    mov     qword [rax], 0
    ret
ASM_ENDFUNC crash_null
