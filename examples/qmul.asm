; qmul.asm — NASM counterpart of qmul.s (x86-64).
; long qmul_q15(long a, long b);  (a*b + 0x4000) >> 15  — Q15 fixed-point multiply.
%include "asm_nasm.inc"

ASM_FUNC qmul_q15
    mov     rax, rdi
    imul    rax, rsi            ; a * b
    add     rax, 0x4000         ; + 0.5 ulp for round-to-nearest
    sar     rax, 15             ; >> 15, arithmetic (signed)
    ret
ASM_ENDFUNC qmul_q15
