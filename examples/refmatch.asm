; refmatch.asm — NASM counterpart of refmatch.s (x86-64). See refmatch.s for the
; routine contracts and the C reference models in test_refmatch.c.
%include "asm_nasm.inc"

ASM_FUNC imax
    mov     rax, rdi
    cmp     rdi, rsi
    cmovl   rax, rsi            ; if a < b, result = b
    ret
ASM_ENDFUNC imax

ASM_FUNC iabs
    mov     rax, rdi
    mov     rdx, rdi
    sar     rdx, 63             ; rdx = sign mask
    xor     rax, rdx
    sub     rax, rdx            ; (x ^ mask) - mask == |x|
    ret
ASM_ENDFUNC iabs

ASM_FUNC iclamp
    mov     rax, rdi
    cmp     rax, rsi
    cmovl   rax, rsi            ; rax = max(x, lo)
    cmp     rax, rdx
    cmovg   rax, rdx            ; rax = min(rax, hi)
    ret
ASM_ENDFUNC iclamp

ASM_FUNC imax_wrong
    mov     rax, rdi
    cmp     rdi, rsi
    cmovg   rax, rsi            ; BUG: keeps the smaller operand
    ret
ASM_ENDFUNC imax_wrong
