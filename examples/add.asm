; add.asm — NASM (Intel-syntax) counterpart of add.s (x86-64, System V ABI).
;
; long add_signed(long a, long b);  a -> rdi, b -> rsi, result -> rax
%include "asm_nasm.inc"

ASM_FUNC add_signed
    mov     rax, rdi            ; rax = a
    add     rax, rsi            ; rax = a + b
    ret
ASM_ENDFUNC add_signed
