; branch.asm — NASM counterpart of branch.s (x86-64, System V ABI).
;
; long classify(long x);  returns -1 if x < 0, 0 if x == 0, +1 if x > 0.  x -> rdi
%include "asm_nasm.inc"

ASM_FUNC classify
    test    rdi, rdi
    js      .neg                ; x < 0
    jz      .zero               ; x == 0
    mov     rax, 1              ; x > 0
    ret
.neg:
    mov     rax, -1
    ret
.zero:
    xor     rax, rax
    ret
ASM_ENDFUNC classify
