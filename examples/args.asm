; args.asm — NASM counterpart of args.s (x86-64). Stack args at [rsp+8]...
%include "asm_nasm.inc"

ASM_FUNC sum3
    mov     rax, rdi
    add     rax, rsi
    add     rax, rdx
    ret
ASM_ENDFUNC sum3

ASM_FUNC sum8
    mov     rax, rdi
    add     rax, rsi
    add     rax, rdx
    add     rax, rcx
    add     rax, r8
    add     rax, r9
    add     rax, [rsp + 8]      ; 7th arg
    add     rax, [rsp + 16]     ; 8th arg
    ret
ASM_ENDFUNC sum8

ASM_FUNC sum10
    mov     rax, rdi
    add     rax, rsi
    add     rax, rdx
    add     rax, rcx
    add     rax, r8
    add     rax, r9
    add     rax, [rsp + 8]      ; 7th
    add     rax, [rsp + 16]     ; 8th
    add     rax, [rsp + 24]     ; 9th
    add     rax, [rsp + 32]     ; 10th
    ret
ASM_ENDFUNC sum10
