; capture.asm — NASM counterpart of capture.s, x86-64 only (System V AMD64 ABI).
;
;   void asm_call_capture(regs_t *out, void *fn, const long args[6]);
;     out -> rdi, fn -> rsi, args -> rdx
;     args[0..5] -> rdi,rsi,rdx,rcx,r8,r9
;
; regs_t offsets (must match asmtest.h):
;   ret 0, rdx 8, rbx 16, rbp 24, r12 32, r13 40, r14 48, r15 56, flags 64
; Sentinels must match ASMTEST_SENTINEL_* in asmtest.h.
%include "asm_nasm.inc"

ASM_FUNC asm_call_capture
    ; Preserve the caller's callee-saved registers.
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    ; 48 bytes pushed; reserve 24 more so rsp is 16-aligned before `call`.
    sub     rsp, 24
    mov     [rsp + 0], rdi          ; stash out
    mov     [rsp + 8], rsi          ; stash fn

    ; Marshal args[0..5] into the integer argument registers.
    mov     rax, rdx                ; rax = &args
    mov     rdi, [rax + 0]
    mov     rsi, [rax + 8]
    mov     rcx, [rax + 24]
    mov     r8,  [rax + 32]
    mov     r9,  [rax + 40]
    mov     rdx, [rax + 16]         ; load rdx last

    ; Seed callee-saved registers with sentinels.
    mov     rbx, 0x1111111111111111
    mov     rbp, 0x2222222222222222
    mov     r12, 0x3333333333333333
    mov     r13, 0x4444444444444444
    mov     r14, 0x5555555555555555
    mov     r15, 0x6666666666666666

    mov     r11, [rsp + 8]          ; fn
    xor     eax, eax                ; variadic-safe: 0 vector regs
    call    r11

    ; Capture results. MOV/loads below do not disturb RFLAGS.
    mov     r11, [rsp + 0]          ; reload out
    mov     [r11 + 0],  rax
    mov     [r11 + 8],  rdx
    mov     [r11 + 16], rbx
    mov     [r11 + 24], rbp
    mov     [r11 + 32], r12
    mov     [r11 + 40], r13
    mov     [r11 + 48], r14
    mov     [r11 + 56], r15
    pushfq
    pop     rax
    mov     [r11 + 64], rax

    add     rsp, 24
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret
ASM_ENDFUNC asm_call_capture

; void asm_call_capture_fp(regs_t *out, void *fn, const long iargs[6],
;                          const double fargs[8]);
;   out -> rdi, fn -> rsi, iargs -> rdx, fargs -> rcx
; Marshals 8 doubles into xmm0-7 and captures the FP return (xmm0) at out+72.
ASM_FUNC asm_call_capture_fp
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 24
    mov     [rsp + 0], rdi
    mov     [rsp + 8], rsi

    ; Float args: fargs (rcx) -> xmm0..xmm7
    movsd   xmm0, [rcx + 0]
    movsd   xmm1, [rcx + 8]
    movsd   xmm2, [rcx + 16]
    movsd   xmm3, [rcx + 24]
    movsd   xmm4, [rcx + 32]
    movsd   xmm5, [rcx + 40]
    movsd   xmm6, [rcx + 48]
    movsd   xmm7, [rcx + 56]

    ; Integer args: iargs (rdx) -> rdi,rsi,rdx,rcx,r8,r9
    mov     rax, rdx
    mov     rdi, [rax + 0]
    mov     rsi, [rax + 8]
    mov     rcx, [rax + 24]
    mov     r8,  [rax + 32]
    mov     r9,  [rax + 40]
    mov     rdx, [rax + 16]

    mov     rbx, 0x1111111111111111
    mov     rbp, 0x2222222222222222
    mov     r12, 0x3333333333333333
    mov     r13, 0x4444444444444444
    mov     r14, 0x5555555555555555
    mov     r15, 0x6666666666666666

    mov     r11, [rsp + 8]
    mov     eax, 8                  ; variadic ABI: 8 vector registers
    call    r11

    mov     r11, [rsp + 0]
    mov     [r11 + 0],  rax
    mov     [r11 + 8],  rdx
    mov     [r11 + 16], rbx
    mov     [r11 + 24], rbp
    mov     [r11 + 32], r12
    mov     [r11 + 40], r13
    mov     [r11 + 48], r14
    mov     [r11 + 56], r15
    pushfq
    pop     rax
    mov     [r11 + 64], rax
    movsd   [r11 + 72], xmm0        ; fret = xmm0

    add     rsp, 24
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret
ASM_ENDFUNC asm_call_capture_fp

; void asm_call_capture_vec(regs_t *out, void *fn, const long iargs[6],
;                           const vec128_t vargs[8]);
;   out -> rdi, fn -> rsi, iargs -> rdx, vargs -> rcx
; Marshals 8 full 128-bit vectors into xmm0-7 and captures xmm0-15 at out+80.
ASM_FUNC asm_call_capture_vec
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 24
    mov     [rsp + 0], rdi
    mov     [rsp + 8], rsi

    ; Vector args: vargs (rcx) -> xmm0..xmm7
    movdqu  xmm0, [rcx + 0]
    movdqu  xmm1, [rcx + 16]
    movdqu  xmm2, [rcx + 32]
    movdqu  xmm3, [rcx + 48]
    movdqu  xmm4, [rcx + 64]
    movdqu  xmm5, [rcx + 80]
    movdqu  xmm6, [rcx + 96]
    movdqu  xmm7, [rcx + 112]

    ; Integer args: iargs (rdx) -> rdi,rsi,rdx,rcx,r8,r9
    mov     rax, rdx
    mov     rdi, [rax + 0]
    mov     rsi, [rax + 8]
    mov     rcx, [rax + 24]
    mov     r8,  [rax + 32]
    mov     r9,  [rax + 40]
    mov     rdx, [rax + 16]

    mov     rbx, 0x1111111111111111
    mov     rbp, 0x2222222222222222
    mov     r12, 0x3333333333333333
    mov     r13, 0x4444444444444444
    mov     r14, 0x5555555555555555
    mov     r15, 0x6666666666666666

    mov     r11, [rsp + 8]
    mov     eax, 8
    call    r11

    mov     r11, [rsp + 0]
    mov     [r11 + 0],  rax
    mov     [r11 + 8],  rdx
    mov     [r11 + 16], rbx
    mov     [r11 + 24], rbp
    mov     [r11 + 32], r12
    mov     [r11 + 40], r13
    mov     [r11 + 48], r14
    mov     [r11 + 56], r15
    pushfq
    pop     rax
    mov     [r11 + 64], rax
    movsd   [r11 + 72], xmm0        ; fret

    ; Full vector file: xmm0..15 -> vec[0..15] at offset 80
    movdqu  [r11 + 80],  xmm0
    movdqu  [r11 + 96],  xmm1
    movdqu  [r11 + 112], xmm2
    movdqu  [r11 + 128], xmm3
    movdqu  [r11 + 144], xmm4
    movdqu  [r11 + 160], xmm5
    movdqu  [r11 + 176], xmm6
    movdqu  [r11 + 192], xmm7
    movdqu  [r11 + 208], xmm8
    movdqu  [r11 + 224], xmm9
    movdqu  [r11 + 240], xmm10
    movdqu  [r11 + 256], xmm11
    movdqu  [r11 + 272], xmm12
    movdqu  [r11 + 288], xmm13
    movdqu  [r11 + 304], xmm14
    movdqu  [r11 + 320], xmm15

    add     rsp, 24
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret
ASM_ENDFUNC asm_call_capture_vec

; void asm_call_capture_args(regs_t *out, void *fn, const long *args, int nargs);
;   out -> rdi, fn -> rsi, args -> rdx, nargs -> rcx
; First 6 integer args in registers, the rest on the stack. rbp is the frame
; pointer (so rbp is reported preserved, not independently checked).
ASM_FUNC asm_call_capture_args
    push    rbp
    mov     rbp, rsp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 32
    mov     [rbp - 48], rdi         ; out
    mov     [rbp - 56], rsi         ; fn
    mov     [rbp - 64], rdx         ; args
    mov     [rbp - 72], rcx         ; nargs

    ; n_stack = max(0, nargs - 6) -> r10
    mov     r10, rcx
    sub     r10, 6
    jg      .have_nstack
    xor     r10, r10
.have_nstack:
    and     rsp, -16
    mov     rax, r10
    shl     rax, 3
    add     rax, 15
    and     rax, -16
    sub     rsp, rax

    ; Copy overflow args: [rsp + i*8] = args[6 + i]
    mov     r8, [rbp - 64]
    xor     rcx, rcx
.copy:
    cmp     rcx, r10
    jge     .copydone
    mov     rax, [r8 + rcx*8 + 48]
    mov     [rsp + rcx*8], rax
    inc     rcx
    jmp     .copy
.copydone:
    ; Load register args (only as many as nargs)
    mov     r11, [rbp - 64]
    mov     r10, [rbp - 72]
    cmp     r10, 1
    jl      .regdone
    mov     rdi, [r11 + 0]
    cmp     r10, 2
    jl      .regdone
    mov     rsi, [r11 + 8]
    cmp     r10, 3
    jl      .regdone
    mov     rdx, [r11 + 16]
    cmp     r10, 4
    jl      .regdone
    mov     rcx, [r11 + 24]
    cmp     r10, 5
    jl      .regdone
    mov     r8, [r11 + 32]
    cmp     r10, 6
    jl      .regdone
    mov     r9, [r11 + 40]
.regdone:
    mov     rbx, 0x1111111111111111
    mov     r12, 0x3333333333333333
    mov     r13, 0x4444444444444444
    mov     r14, 0x5555555555555555
    mov     r15, 0x6666666666666666

    mov     r11, [rbp - 56]
    xor     eax, eax
    call    r11

    mov     r11, [rbp - 48]
    mov     [r11 + 0], rax
    mov     [r11 + 8], rdx
    mov     [r11 + 16], rbx
    mov     rax, 0x2222222222222222 ; rbp: reported preserved, not checked
    mov     [r11 + 24], rax
    mov     [r11 + 32], r12
    mov     [r11 + 40], r13
    mov     [r11 + 48], r14
    mov     [r11 + 56], r15
    pushfq
    pop     rax
    mov     [r11 + 64], rax

    lea     rsp, [rbp - 40]
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp
    ret
ASM_ENDFUNC asm_call_capture_args
