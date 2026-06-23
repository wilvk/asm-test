; structparam.asm — NASM counterpart of structparam.s (x86-64).
%include "asm_nasm.inc"

ASM_FUNC pst2
    mov     rax, rdi
    add     rax, rsi
    ret
ASM_ENDFUNC pst2

ASM_FUNC pst_mixed
    cvttsd2si rax, xmm0
    add     rax, rdi
    ret
ASM_ENDFUNC pst_mixed

ASM_FUNC bigsum
    mov     rax, [rsp + 8]
    add     rax, [rsp + 16]
    add     rax, [rsp + 24]
    ret
ASM_ENDFUNC bigsum
