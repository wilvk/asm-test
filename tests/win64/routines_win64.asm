; routines_win64.asm — tiny routines under test, written to the Microsoft x64
; ABI (Phase 1 slice). The capture trampoline calls these through the Win64
; convention: 1st int arg in rcx, 2nd in rdx, ...; return in rax. They are never
; called directly from C — only their addresses are handed to the trampoline.
;
; Assembles under `-f win64` (PE/Wine) and `-f macho64`/`-f elf64` (ms_abi lane).

%include "asm_nasm.inc"

; long long win64_ret_arg0(a, ...) -> a   (the 1st integer arg, rcx)
ASM_FUNC win64_ret_arg0
    mov     rax, rcx
    ret
ASM_ENDFUNC win64_ret_arg0

; long long win64_sum2(a, b) -> a + b   (rcx + rdx)
ASM_FUNC win64_sum2
    lea     rax, [rcx + rdx]
    ret
ASM_ENDFUNC win64_sum2

; Well-behaved: uses rbx but saves/restores it, returns a (rcx). The
; ABI-preservation check must see rbx's sentinel intact afterwards.
ASM_FUNC win64_preserve_rbx
    push    rbx
    mov     rbx, rcx
    mov     rax, rbx
    pop     rbx
    ret
ASM_ENDFUNC win64_preserve_rbx

; Misbehaved: clobbers rbx without restoring it (the ABI violation the framework
; is built to catch). Still returns a (rcx).
ASM_FUNC win64_clobber_rbx
    xor     rbx, rbx
    mov     rax, rcx
    ret
ASM_ENDFUNC win64_clobber_rbx

; Returns the 5th integer arg (index 4), which arrives on the stack above the
; 32-byte shadow space. On entry: [rsp]=retaddr, [rsp+8..39]=shadow,
; [rsp+40]=arg5. Exercises stack-arg marshalling + shadow-space correctness.
ASM_FUNC win64_ret_arg4
    mov     rax, [rsp+40]
    ret
ASM_ENDFUNC win64_ret_arg4

; long long win64_sum6(a,b,c,d,e,f) -> a+b+c+d+e+f. Args 1-4 in rcx/rdx/r8/r9;
; args 5-6 on the stack above the shadow space ([rsp+40], [rsp+48] on entry).
; Exercises asm_call_capture_args_win64's register + overflow marshalling.
ASM_FUNC win64_sum6
    lea     rax, [rcx + rdx]
    add     rax, r8
    add     rax, r9
    add     rax, [rsp+40]
    add     rax, [rsp+48]
    ret
ASM_ENDFUNC win64_sum6

; double win64_addsd(a, b) -> a + b. Win64 passes the first two FP args in
; xmm0, xmm1; the FP return is in xmm0.
ASM_FUNC win64_addsd
    addsd   xmm0, xmm1
    ret
ASM_ENDFUNC win64_addsd

; Well-behaved: uses callee-saved xmm6 but saves/restores it. Returns arg0
; (xmm0) unchanged; the xmm6..15 sentinels must survive.
ASM_FUNC win64_vec_preserve_xmm6
    sub     rsp, 16
    movdqu  [rsp], xmm6
    movsd   xmm6, xmm0
    movsd   xmm0, xmm6
    movdqu  xmm6, [rsp]
    add     rsp, 16
    ret
ASM_ENDFUNC win64_vec_preserve_xmm6

; Misbehaved: clobbers callee-saved xmm6 without restoring it (the FP analogue
; of win64_clobber_rbx). Returns arg0 (xmm0) unchanged.
ASM_FUNC win64_vec_clobber_xmm6
    xorps   xmm6, xmm6
    ret
ASM_ENDFUNC win64_vec_clobber_xmm6

; double win64_addsd6(a,b,c,d,e,f) -> a+b+c+d+e+f. Args 1-4 in xmm0..3; args 5-6
; on the stack above the shadow space ([rsp+40], [rsp+48] on entry). Exercises
; asm_call_capture_fp_n_win64's FP register + overflow marshalling.
ASM_FUNC win64_addsd6
    addsd   xmm0, xmm1
    addsd   xmm0, xmm2
    addsd   xmm0, xmm3
    addsd   xmm0, [rsp+40]
    addsd   xmm0, [rsp+48]
    ret
ASM_ENDFUNC win64_addsd6

; struct { long long a, b; } win64_sret_make(long long x, long long y).
; Win64 sret: the hidden result pointer is the 1st arg (rcx); the visible args x,
; y shift to rdx, r8. Writes {x, y} through the pointer and returns it in rax.
ASM_FUNC win64_sret_make
    mov     [rcx],   rdx
    mov     [rcx+8], r8
    mov     rax, rcx
    ret
ASM_ENDFUNC win64_sret_make
