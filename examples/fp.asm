; fp.asm — NASM counterpart of fp.s (x86-64, System V ABI).
;
; double fp_add(double a, double b);  a -> xmm0, b -> xmm1, result -> xmm0
; double fp_mul(double a, double b);
; double mix_scale(long n, double x); (double)n * x — n -> rdi, x -> xmm0
%include "asm_nasm.inc"

ASM_FUNC fp_add
    addsd   xmm0, xmm1          ; xmm0 = xmm0 + xmm1
    ret
ASM_ENDFUNC fp_add

ASM_FUNC fp_mul
    mulsd   xmm0, xmm1          ; xmm0 = xmm0 * xmm1
    ret
ASM_ENDFUNC fp_mul

ASM_FUNC mix_scale
    cvtsi2sd xmm1, rdi          ; xmm1 = (double)n
    mulsd   xmm0, xmm1          ; xmm0 = x * (double)n
    ret
ASM_ENDFUNC mix_scale
