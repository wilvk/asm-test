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

; void asm_call_capture_vec256(vec256_t *vec, void *fn, const long iargs[6],
;                              const vec256_t vargs[8]);
;   vec -> rdi, fn -> rsi, iargs -> rdx, vargs -> rcx
; AVX2: marshals 8 full 256-bit vectors into ymm0-7 and captures ymm0-15 into
; vec[0..15] (32 bytes each; vec[0] = return). x86-64 + AVX2 only — gated by the
; C wrapper / ASM_VCALL256* on asmtest_cpu_has_avx2(). Captures the vector file
; only (the 128-bit path covers GP/flags). vzeroupper on exit.
ASM_FUNC asm_call_capture_vec256
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 24
    mov     [rsp + 0], rdi          ; save vec out ptr
    mov     [rsp + 8], rsi          ; save fn

    ; Vector args: vargs (rcx) -> ymm0..ymm7
    vmovdqu ymm0, [rcx + 0]
    vmovdqu ymm1, [rcx + 32]
    vmovdqu ymm2, [rcx + 64]
    vmovdqu ymm3, [rcx + 96]
    vmovdqu ymm4, [rcx + 128]
    vmovdqu ymm5, [rcx + 160]
    vmovdqu ymm6, [rcx + 192]
    vmovdqu ymm7, [rcx + 224]

    ; Integer args: iargs (rdx) -> rdi,rsi,rdx,rcx,r8,r9
    mov     rax, rdx
    mov     rdi, [rax + 0]
    mov     rsi, [rax + 8]
    mov     rcx, [rax + 24]
    mov     r8,  [rax + 32]
    mov     r9,  [rax + 40]
    mov     rdx, [rax + 16]

    mov     r11, [rsp + 8]
    mov     eax, 8                  ; variadic ABI: 8 vector registers
    call    r11

    mov     r11, [rsp + 0]          ; vec out ptr
    ; Full ymm file: ymm0..15 -> vec[0..15] (32 bytes each)
    vmovdqu [r11 + 0],   ymm0
    vmovdqu [r11 + 32],  ymm1
    vmovdqu [r11 + 64],  ymm2
    vmovdqu [r11 + 96],  ymm3
    vmovdqu [r11 + 128], ymm4
    vmovdqu [r11 + 160], ymm5
    vmovdqu [r11 + 192], ymm6
    vmovdqu [r11 + 224], ymm7
    vmovdqu [r11 + 256], ymm8
    vmovdqu [r11 + 288], ymm9
    vmovdqu [r11 + 320], ymm10
    vmovdqu [r11 + 352], ymm11
    vmovdqu [r11 + 384], ymm12
    vmovdqu [r11 + 416], ymm13
    vmovdqu [r11 + 448], ymm14
    vmovdqu [r11 + 480], ymm15
    vzeroupper

    add     rsp, 24
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret
ASM_ENDFUNC asm_call_capture_vec256

; void asm_call_capture_vec512(vec512_t *vec, void *fn, const long iargs[6],
;                              const vec512_t vargs[8]);
;   vec -> rdi, fn -> rsi, iargs -> rdx, vargs -> rcx
; AVX-512: marshals 8 full 512-bit vectors into zmm0-7 and captures zmm0-31 (AVX-512
; doubles the count) into vec[0..31] (64 bytes each; vec[0] = return). x86-64 +
; AVX-512F only — gated by the C wrapper / ASM_VCALL512* on asmtest_cpu_has_avx512f().
; vmovdqu64 is the EVEX-encoded unaligned move (required to reach zmm16..31).
; Captures the vector file only (the 128-bit path covers GP/flags). vzeroupper on exit.
ASM_FUNC asm_call_capture_vec512
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 24
    mov     [rsp + 0], rdi          ; save vec out ptr
    mov     [rsp + 8], rsi          ; save fn

    ; Vector args: vargs (rcx) -> zmm0..zmm7
    vmovdqu64 zmm0, [rcx + 0]
    vmovdqu64 zmm1, [rcx + 64]
    vmovdqu64 zmm2, [rcx + 128]
    vmovdqu64 zmm3, [rcx + 192]
    vmovdqu64 zmm4, [rcx + 256]
    vmovdqu64 zmm5, [rcx + 320]
    vmovdqu64 zmm6, [rcx + 384]
    vmovdqu64 zmm7, [rcx + 448]

    ; Integer args: iargs (rdx) -> rdi,rsi,rdx,rcx,r8,r9
    mov     rax, rdx
    mov     rdi, [rax + 0]
    mov     rsi, [rax + 8]
    mov     rcx, [rax + 24]
    mov     r8,  [rax + 32]
    mov     r9,  [rax + 40]
    mov     rdx, [rax + 16]

    mov     r11, [rsp + 8]
    mov     eax, 8                  ; variadic ABI: 8 vector registers
    call    r11

    mov     r11, [rsp + 0]          ; vec out ptr
    ; Full zmm file: zmm0..31 -> vec[0..31] (64 bytes each)
    vmovdqu64 [r11 + 0],    zmm0
    vmovdqu64 [r11 + 64],   zmm1
    vmovdqu64 [r11 + 128],  zmm2
    vmovdqu64 [r11 + 192],  zmm3
    vmovdqu64 [r11 + 256],  zmm4
    vmovdqu64 [r11 + 320],  zmm5
    vmovdqu64 [r11 + 384],  zmm6
    vmovdqu64 [r11 + 448],  zmm7
    vmovdqu64 [r11 + 512],  zmm8
    vmovdqu64 [r11 + 576],  zmm9
    vmovdqu64 [r11 + 640],  zmm10
    vmovdqu64 [r11 + 704],  zmm11
    vmovdqu64 [r11 + 768],  zmm12
    vmovdqu64 [r11 + 832],  zmm13
    vmovdqu64 [r11 + 896],  zmm14
    vmovdqu64 [r11 + 960],  zmm15
    vmovdqu64 [r11 + 1024], zmm16
    vmovdqu64 [r11 + 1088], zmm17
    vmovdqu64 [r11 + 1152], zmm18
    vmovdqu64 [r11 + 1216], zmm19
    vmovdqu64 [r11 + 1280], zmm20
    vmovdqu64 [r11 + 1344], zmm21
    vmovdqu64 [r11 + 1408], zmm22
    vmovdqu64 [r11 + 1472], zmm23
    vmovdqu64 [r11 + 1536], zmm24
    vmovdqu64 [r11 + 1600], zmm25
    vmovdqu64 [r11 + 1664], zmm26
    vmovdqu64 [r11 + 1728], zmm27
    vmovdqu64 [r11 + 1792], zmm28
    vmovdqu64 [r11 + 1856], zmm29
    vmovdqu64 [r11 + 1920], zmm30
    vmovdqu64 [r11 + 1984], zmm31
    vzeroupper

    add     rsp, 24
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret
ASM_ENDFUNC asm_call_capture_vec512

; void asm_call_capture_fp_n(regs_t *out, void *fn, const long iargs[6],
;                            const double *fargs, int nfargs);
;   out -> rdi, fn -> rsi, iargs -> rdx, fargs -> rcx, nfargs -> r8
; First 8 doubles in xmm0-7, the rest on the stack. rbp is the frame pointer
; (so rbp is reported preserved, not independently checked).
ASM_FUNC asm_call_capture_fp_n
    push    rbp
    mov     rbp, rsp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 48
    mov     [rbp - 48], rdi         ; out
    mov     [rbp - 56], rsi         ; fn
    mov     [rbp - 64], rdx         ; iargs
    mov     [rbp - 72], rcx         ; fargs
    mov     [rbp - 80], r8          ; nfargs

    ; n_stack = max(0, nfargs - 8) -> r10
    mov     r10, r8
    sub     r10, 8
    jg      .have_nstack
    xor     r10, r10
.have_nstack:
    and     rsp, -16
    mov     rax, r10
    shl     rax, 3
    add     rax, 15
    and     rax, -16
    sub     rsp, rax

    ; Copy overflow doubles: [rsp + i*8] = fargs[8 + i]
    mov     r8, [rbp - 72]
    xor     rcx, rcx
.copy:
    cmp     rcx, r10
    jge     .copydone
    mov     rax, [r8 + rcx*8 + 64]
    mov     [rsp + rcx*8], rax
    inc     rcx
    jmp     .copy
.copydone:
    ; Load FP register args xmm0..7 (only as many as nfargs)
    mov     r11, [rbp - 72]        ; fargs
    mov     r10, [rbp - 80]        ; nfargs
    cmp     r10, 1
    jl      .regdone
    movsd   xmm0, [r11 + 0]
    cmp     r10, 2
    jl      .regdone
    movsd   xmm1, [r11 + 8]
    cmp     r10, 3
    jl      .regdone
    movsd   xmm2, [r11 + 16]
    cmp     r10, 4
    jl      .regdone
    movsd   xmm3, [r11 + 24]
    cmp     r10, 5
    jl      .regdone
    movsd   xmm4, [r11 + 32]
    cmp     r10, 6
    jl      .regdone
    movsd   xmm5, [r11 + 40]
    cmp     r10, 7
    jl      .regdone
    movsd   xmm6, [r11 + 48]
    cmp     r10, 8
    jl      .regdone
    movsd   xmm7, [r11 + 56]
.regdone:
    ; Integer args: iargs -> rdi,rsi,rdx,rcx,r8,r9
    mov     rax, [rbp - 64]
    mov     rdi, [rax + 0]
    mov     rsi, [rax + 8]
    mov     rcx, [rax + 24]
    mov     r8,  [rax + 32]
    mov     r9,  [rax + 40]
    mov     rdx, [rax + 16]

    mov     rbx, 0x1111111111111111
    mov     r12, 0x3333333333333333
    mov     r13, 0x4444444444444444
    mov     r14, 0x5555555555555555
    mov     r15, 0x6666666666666666

    ; al = min(nfargs, 8): variadic-ABI vector-register count
    mov     rax, [rbp - 80]
    cmp     rax, 8
    jle     .alok
    mov     eax, 8
.alok:
    mov     r11, [rbp - 56]
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
    movsd   [r11 + 72], xmm0        ; fret = xmm0

    lea     rsp, [rbp - 40]
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp
    ret
ASM_ENDFUNC asm_call_capture_fp_n

; void asm_call_capture_vec_n(regs_t *out, void *fn, const long iargs[6],
;                             const vec128_t *vargs, int nvargs);
;   out -> rdi, fn -> rsi, iargs -> rdx, vargs -> rcx, nvargs -> r8
; First 8 vectors in xmm0-7, the rest on the stack (16-byte slots). rbp is the
; frame pointer (reported preserved, not independently checked).
ASM_FUNC asm_call_capture_vec_n
    push    rbp
    mov     rbp, rsp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 48
    mov     [rbp - 48], rdi         ; out
    mov     [rbp - 56], rsi         ; fn
    mov     [rbp - 64], rdx         ; iargs
    mov     [rbp - 72], rcx         ; vargs
    mov     [rbp - 80], r8          ; nvargs

    ; n_stack = max(0, nvargs - 8) -> r10 (each slot is 16 bytes)
    mov     r10, r8
    sub     r10, 8
    jg      .have_nstack
    xor     r10, r10
.have_nstack:
    and     rsp, -16
    mov     rax, r10
    shl     rax, 4                  ; n_stack * 16 (already 16-aligned)
    sub     rsp, rax

    ; Copy overflow vectors as 8-byte words: words = n_stack*2,
    ; [rsp + j*8] = vargs[128 + j*8]
    mov     r9, r10
    shl     r9, 1
    mov     r8, [rbp - 72]
    xor     rcx, rcx
.copy:
    cmp     rcx, r9
    jge     .copydone
    mov     rax, [r8 + rcx*8 + 128]
    mov     [rsp + rcx*8], rax
    inc     rcx
    jmp     .copy
.copydone:
    ; Load vector register args xmm0..7 (only as many as nvargs)
    mov     r11, [rbp - 72]        ; vargs
    mov     r10, [rbp - 80]        ; nvargs
    cmp     r10, 1
    jl      .regdone
    movdqu  xmm0, [r11 + 0]
    cmp     r10, 2
    jl      .regdone
    movdqu  xmm1, [r11 + 16]
    cmp     r10, 3
    jl      .regdone
    movdqu  xmm2, [r11 + 32]
    cmp     r10, 4
    jl      .regdone
    movdqu  xmm3, [r11 + 48]
    cmp     r10, 5
    jl      .regdone
    movdqu  xmm4, [r11 + 64]
    cmp     r10, 6
    jl      .regdone
    movdqu  xmm5, [r11 + 80]
    cmp     r10, 7
    jl      .regdone
    movdqu  xmm6, [r11 + 96]
    cmp     r10, 8
    jl      .regdone
    movdqu  xmm7, [r11 + 112]
.regdone:
    ; Integer args: iargs -> rdi,rsi,rdx,rcx,r8,r9
    mov     rax, [rbp - 64]
    mov     rdi, [rax + 0]
    mov     rsi, [rax + 8]
    mov     rcx, [rax + 24]
    mov     r8,  [rax + 32]
    mov     r9,  [rax + 40]
    mov     rdx, [rax + 16]

    mov     rbx, 0x1111111111111111
    mov     r12, 0x3333333333333333
    mov     r13, 0x4444444444444444
    mov     r14, 0x5555555555555555
    mov     r15, 0x6666666666666666

    ; al = min(nvargs, 8)
    mov     rax, [rbp - 80]
    cmp     rax, 8
    jle     .alok
    mov     eax, 8
.alok:
    mov     r11, [rbp - 56]
    call    r11

    mov     r11, [rbp - 48]
    mov     [r11 + 0], rax
    mov     [r11 + 8], rdx
    mov     [r11 + 16], rbx
    mov     rax, 0x2222222222222222
    mov     [r11 + 24], rax
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

    lea     rsp, [rbp - 40]
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp
    ret
ASM_ENDFUNC asm_call_capture_vec_n

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

; void asm_call_capture_sret(regs_t *out, void *fn, void *result,
;                            const long *args, int nargs);
;   out -> rdi, fn -> rsi, result -> rdx, args -> rcx, nargs -> r8
; Hidden result pointer goes in rdi; visible args use rsi..r9 then the stack.
ASM_FUNC asm_call_capture_sret
    push    rbp
    mov     rbp, rsp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 48
    mov     [rbp - 48], rdi         ; out
    mov     [rbp - 56], rsi         ; fn
    mov     [rbp - 64], rdx         ; result
    mov     [rbp - 72], rcx         ; args
    mov     [rbp - 80], r8          ; nargs

    mov     r10, r8
    sub     r10, 5
    jg      .have_nstack
    xor     r10, r10
.have_nstack:
    and     rsp, -16
    mov     rax, r10
    shl     rax, 3
    add     rax, 15
    and     rax, -16
    sub     rsp, rax

    mov     r8, [rbp - 72]
    xor     rcx, rcx
.copy:
    cmp     rcx, r10
    jge     .copydone
    mov     rax, [r8 + rcx*8 + 40]
    mov     [rsp + rcx*8], rax
    inc     rcx
    jmp     .copy
.copydone:
    mov     r11, [rbp - 72]
    mov     r10, [rbp - 80]
    cmp     r10, 1
    jl      .regdone
    mov     rsi, [r11 + 0]
    cmp     r10, 2
    jl      .regdone
    mov     rdx, [r11 + 8]
    cmp     r10, 3
    jl      .regdone
    mov     rcx, [r11 + 16]
    cmp     r10, 4
    jl      .regdone
    mov     r8, [r11 + 24]
    cmp     r10, 5
    jl      .regdone
    mov     r9, [r11 + 32]
.regdone:
    mov     rdi, [rbp - 64]         ; hidden result pointer
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
    mov     rax, 0x2222222222222222
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
ASM_ENDFUNC asm_call_capture_sret

; void asm_bigstruct_x86(regs_t *out, void *fn, const long *iargs, int niargs,
;                        const void *sptr, size_t ssize);
;   out -> rdi, fn -> rsi, iargs -> rdx, niargs -> rcx, sptr -> r8, ssize -> r9
ASM_FUNC asm_bigstruct_x86
    push    rbp
    mov     rbp, rsp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 48
    mov     [rbp - 48], rdi         ; out
    mov     [rbp - 56], rsi         ; fn
    mov     [rbp - 64], rdx         ; iargs
    mov     [rbp - 72], rcx         ; niargs
    mov     [rbp - 80], r8          ; sptr
    mov     [rbp - 88], r9          ; ssize

    and     rsp, -16
    mov     rax, r9
    add     rax, 15
    and     rax, -16
    sub     rsp, rax

    mov     r8, [rbp - 80]
    mov     r9, [rbp - 88]
    xor     rcx, rcx
.bcopy:
    cmp     rcx, r9
    jge     .bdone
    mov     al, [r8 + rcx]
    mov     [rsp + rcx], al
    inc     rcx
    jmp     .bcopy
.bdone:
    mov     r11, [rbp - 64]
    mov     r10, [rbp - 72]
    cmp     r10, 1
    jl      .rdone
    mov     rdi, [r11 + 0]
    cmp     r10, 2
    jl      .rdone
    mov     rsi, [r11 + 8]
    cmp     r10, 3
    jl      .rdone
    mov     rdx, [r11 + 16]
    cmp     r10, 4
    jl      .rdone
    mov     rcx, [r11 + 24]
    cmp     r10, 5
    jl      .rdone
    mov     r8, [r11 + 32]
    cmp     r10, 6
    jl      .rdone
    mov     r9, [r11 + 40]
.rdone:
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
    mov     rax, 0x2222222222222222
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
ASM_ENDFUNC asm_bigstruct_x86

; void asm_call_capture_sve(svec_t *z, spred_t *p, void *fn, const long *iargs,
;                           const svec_t *zargs, const spred_t *pargs);
; SVE is AArch64-Linux-only; NASM builds are x86-64 only, so this is a ret stub
; purely to resolve the symbol. Never called: asmtest_cpu_has_sve() is false on
; x86-64, so ASM_SVCALL_* self-skip and the C wrapper is gated. (Real body lives
; in the GAS twin capture.s.)
ASM_FUNC asm_call_capture_sve
    ret
ASM_ENDFUNC asm_call_capture_sve

; unsigned long asmtest_sve_rdvl(void) — 0 on x86-64 (no SVE). Clear eax first
; so a misuse returns 0, not garbage.
ASM_FUNC asmtest_sve_rdvl
    xor     eax, eax
    ret
ASM_ENDFUNC asmtest_sve_rdvl
