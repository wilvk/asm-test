; fp.asm — NASM counterpart of fp.s (x86-64, System V ABI).
;
; double fp_add(double a, double b);  a -> xmm0, b -> xmm1, result -> xmm0
; double fp_mul(double a, double b);
%include "asm_nasm.inc"

ASM_FUNC fp_add
    addsd   xmm0, xmm1          ; xmm0 = xmm0 + xmm1
    ret
ASM_ENDFUNC fp_add

ASM_FUNC fp_mul
    mulsd   xmm0, xmm1          ; xmm0 = xmm0 * xmm1
    ret
ASM_ENDFUNC fp_mul
