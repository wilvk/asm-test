; bench.asm — NASM counterpart of bench.s (x86-64, System V ABI).
;
; long sum_to_n(long n);  n -> rdi, result -> rax
%include "asm_nasm.inc"

ASM_FUNC sum_to_n
    xor     rax, rax            ; acc = 0
    test    rdi, rdi            ; n <= 0 ? return 0
    jle     .done
.loop:
    add     rax, rdi            ; acc += n
    dec     rdi
    jnz     .loop
.done:
    ret
ASM_ENDFUNC sum_to_n
