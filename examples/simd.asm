; simd.asm — NASM counterpart of simd.s (x86-64 SSE).
;
; vec128 vec_add4f(vec128 a, vec128 b);  a -> xmm0, b -> xmm1, result -> xmm0
%include "asm_nasm.inc"

ASM_FUNC vec_add4f
    addps   xmm0, xmm1          ; xmm0 = xmm0 + xmm1, packed single
    ret
ASM_ENDFUNC vec_add4f

; vec256 vec_add4d(vec256 a, vec256 b); four 64-bit doubles, AVX2
;   a -> ymm0, b -> ymm1, result -> ymm0
ASM_FUNC vec_add4d
    vaddpd  ymm0, ymm0, ymm1    ; ymm0 = ymm0 + ymm1, packed double
    ret
ASM_ENDFUNC vec_add4d
