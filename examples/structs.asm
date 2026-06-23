; structs.asm — NASM counterpart of structs.s (x86-64).
%include "asm_nasm.inc"

; struct big make_big(long a, long b, long c); rdi=result, a=rsi, b=rdx, c=rcx
ASM_FUNC make_big
    mov     [rdi + 0], rsi
    mov     [rdi + 8], rdx
    mov     [rdi + 16], rcx
    mov     rax, rdi            ; return the result pointer
    ret
ASM_ENDFUNC make_big

; struct pair make_pair(long a, long b); a=rdi, b=rsi -> rax:rdx
ASM_FUNC make_pair
    mov     rax, rdi
    mov     rdx, rsi
    ret
ASM_ENDFUNC make_pair
