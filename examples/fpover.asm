; fpover.asm — NASM counterpart of fpover.s (x86-64, System V AMD64 ABI).
;
; double fp_sum10(double a0 .. a9);  sum of all ten doubles
; double fp_stack2(double a0 .. a9); a8 + a9 only (the two stack args)
; vec128 vec_sum10(vec128 a0 .. a9); lane-wise sum of all ten vectors
;
; a0..a7 arrive in xmm0..7; the stack args sit just above the return address:
; doubles at [rsp+8]/[rsp+16], 16-byte vectors at [rsp+8]/[rsp+24].
%include "asm_nasm.inc"

ASM_FUNC fp_sum10
    addsd   xmm0, xmm1
    addsd   xmm0, xmm2
    addsd   xmm0, xmm3
    addsd   xmm0, xmm4
    addsd   xmm0, xmm5
    addsd   xmm0, xmm6
    addsd   xmm0, xmm7
    addsd   xmm0, [rsp + 8]        ; a8
    addsd   xmm0, [rsp + 16]       ; a9
    ret
ASM_ENDFUNC fp_sum10

ASM_FUNC fp_stack2
    movsd   xmm0, [rsp + 8]        ; a8
    addsd   xmm0, [rsp + 16]       ; a9
    ret
ASM_ENDFUNC fp_stack2

ASM_FUNC vec_sum10
    addps   xmm0, xmm1
    addps   xmm0, xmm2
    addps   xmm0, xmm3
    addps   xmm0, xmm4
    addps   xmm0, xmm5
    addps   xmm0, xmm6
    addps   xmm0, xmm7
    addps   xmm0, [rsp + 8]        ; a8 (16-byte aligned)
    addps   xmm0, [rsp + 24]       ; a9
    ret
ASM_ENDFUNC vec_sum10
