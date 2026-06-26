; qadd.asm — NASM counterpart of qadd.s (x86-64 SSE2).
; vec128 qadd_u8x16(vec128 a, vec128 b); per-byte unsigned saturating add.
;   a -> xmm0, b -> xmm1, result -> xmm0
%include "asm_nasm.inc"

ASM_FUNC qadd_u8x16
    paddusb xmm0, xmm1          ; packed add unsigned saturate, bytes
    ret
ASM_ENDFUNC qadd_u8x16
