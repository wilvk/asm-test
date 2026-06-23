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
