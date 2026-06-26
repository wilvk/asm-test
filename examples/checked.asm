; checked.asm — NASM counterpart of checked.s (x86-64).
; long checked_add(long a, long b); rax = a+b (wrapping), OF set on signed overflow.
; mov-immediate and ret leave the flags alone, so OF survives to the capture.
%include "asm_nasm.inc"

ASM_FUNC checked_add
    mov     rax, rdi
    add     rax, rsi            ; sets OF on signed overflow; sum in rax
    ret
ASM_ENDFUNC checked_add
