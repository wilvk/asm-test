; capture_win64.asm — register/flags capture trampoline, Microsoft x64 ABI.
;
;   void asm_call_capture_win64(win64_regs_t *out, void *fn,
;                               const long long *args);  [6 int args]
;
; The Win64 counterpart of src/capture.s's asm_call_capture. Seeds the Win64
; callee-saved registers with sentinels, marshals up to six integer args into
; the Microsoft x64 argument locations (rcx, rdx, r8, r9, then the stack above a
; 32-byte shadow space), calls fn, then snapshots the return register, the
; callee-saved registers (to verify the routine restored them), and RFLAGS.
;
; Assembles unchanged under `-f win64` (the PE/Wine path) and under
; `-f macho64`/`-f elf64` (the ms_abi native lane). Sentinels and struct offsets
; MUST match tests/win64/win64_regs.h.
;
; Win64 deltas from System V (all also modeled in src/emu.c): args in rcx, rdx,
; r8, r9 (not rdi, rsi, ...); a 32-byte shadow space reserved below the return
; address at the call site; rdi and rsi join the callee-saved set.

%include "asm_nasm.inc"

; Inbound (Win64): out = rcx, fn = rdx, args = r8.
ASM_FUNC asm_call_capture_win64
    push    rbx
    push    rbp
    push    rsi
    push    rdi
    push    r12
    push    r13
    push    r14
    push    r15
    ; 64 bytes pushed -> rsp is 8 (mod 16). Reserve 72 (also 8 mod 16) so rsp is
    ; 16-aligned at the `call`, as Win64 requires.
    ;   [rsp+0..31]  32-byte shadow space (owned by fn)
    ;   [rsp+32]     stack arg slot for args[4] (5th int arg)
    ;   [rsp+40]     stack arg slot for args[5] (6th int arg)
    ;   [rsp+48]     stash: out
    ;   [rsp+56]     stash: fn
    ;   [rsp+64]     padding
    sub     rsp, 72
    mov     [rsp+48], rcx           ; stash out across the call
    mov     [rsp+56], rdx           ; stash fn  across the call

    ; Marshal args[0..5]. r8 holds &args on entry; copy it to rax first, and load
    ; r8 (= args[2]) last so the base pointer survives the earlier loads.
    mov     rax, r8                 ; rax = &args
    mov     rcx, [rax+0]            ; args[0] -> rcx
    mov     rdx, [rax+8]            ; args[1] -> rdx
    mov     r9,  [rax+24]           ; args[3] -> r9
    mov     r10, [rax+32]           ; args[4] -> 5th-arg stack slot
    mov     [rsp+32], r10
    mov     r10, [rax+40]           ; args[5] -> 6th-arg stack slot
    mov     [rsp+40], r10
    mov     r8,  [rax+16]           ; args[2] -> r8 (last)

    ; Seed the Win64 callee-saved set with sentinels.
    mov     rbx, 0x1111111111111111
    mov     rbp, 0x2222222222222222
    mov     rsi, 0xCCCCCCCCCCCCCCCC
    mov     rdi, 0xDDDDDDDDDDDDDDDD
    mov     r12, 0x3333333333333333
    mov     r13, 0x4444444444444444
    mov     r14, 0x5555555555555555
    mov     r15, 0x6666666666666666

    mov     r11, [rsp+56]           ; fn
    call    r11

    ; Capture. The loads/stores below do not disturb RFLAGS before pushfq.
    mov     r11, [rsp+48]           ; reload out
    mov     [r11+0],  rax           ; ret
    mov     [r11+8],  rdx
    mov     [r11+16], rbx
    mov     [r11+24], rbp
    mov     [r11+32], rdi
    mov     [r11+40], rsi
    mov     [r11+48], r12
    mov     [r11+56], r13
    mov     [r11+64], r14
    mov     [r11+72], r15
    pushfq
    pop     rax
    mov     [r11+80], rax           ; flags

    add     rsp, 72
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rdi
    pop     rsi
    pop     rbp
    pop     rbx
    ret
ASM_ENDFUNC asm_call_capture_win64


; void asm_call_capture_args_win64(win64_regs_t *out, void *fn,
;                                  const long long *args, int nargs);
;
; The Win64 mirror of asm_call_capture_args: passes `nargs` integer args — the
; first four in rcx/rdx/r8/r9, the rest on the stack above the 32-byte shadow
; space. Uses rbp as a frame pointer to unwind the variable-size outgoing-arg
; area, so rbp is reported preserved but not independently checked.
;
; Inbound (Win64): out = rcx, fn = rdx, args = r8, nargs = r9d.
ASM_FUNC asm_call_capture_args_win64
    push    rbp
    mov     rbp, rsp
    push    rbx
    push    rsi
    push    rdi
    push    r12
    push    r13
    push    r14
    push    r15                     ; 7 saved regs at [rbp-8 .. rbp-56]
    movsxd  r9, r9d                 ; normalise nargs to 64-bit
    sub     rsp, 64                 ; scratch for the four stashes
    mov     [rbp-64], rcx           ; out
    mov     [rbp-72], rdx           ; fn
    mov     [rbp-80], r8            ; args
    mov     [rbp-88], r9            ; nargs

    ; n_stack = max(0, nargs - 4) -> r10
    mov     r10, r9
    sub     r10, 4
    jg      .args_have_stack
    xor     r10, r10
.args_have_stack:
    ; 16-align rsp, then reserve 32-byte shadow + round_up(n_stack*8, 16).
    and     rsp, -16
    mov     rax, r10
    shl     rax, 3                  ; n_stack * 8
    add     rax, 32                 ; + shadow space
    add     rax, 15
    and     rax, -16
    sub     rsp, rax

    ; Copy overflow args: [rsp+32 + i*8] = args[4 + i].
    mov     r8, [rbp-80]            ; args
    xor     rcx, rcx
.args_copy:
    cmp     rcx, r10
    jge     .args_copy_done
    mov     rax, [r8 + 32 + rcx*8]  ; args[4 + i]  (byte 32 = 4*8)
    mov     [rsp + 32 + rcx*8], rax
    inc     rcx
    jmp     .args_copy
.args_copy_done:
    ; Load the register args (only as many as nargs).
    mov     r11, [rbp-80]           ; args
    mov     r10, [rbp-88]           ; nargs
    cmp     r10, 1
    jl      .args_seed
    mov     rcx, [r11+0]
    cmp     r10, 2
    jl      .args_seed
    mov     rdx, [r11+8]
    cmp     r10, 3
    jl      .args_seed
    mov     r8,  [r11+16]
    cmp     r10, 4
    jl      .args_seed
    mov     r9,  [r11+24]
.args_seed:
    mov     rbx, 0x1111111111111111
    mov     rsi, 0xCCCCCCCCCCCCCCCC
    mov     rdi, 0xDDDDDDDDDDDDDDDD
    mov     r12, 0x3333333333333333
    mov     r13, 0x4444444444444444
    mov     r14, 0x5555555555555555
    mov     r15, 0x6666666666666666

    mov     r11, [rbp-72]           ; fn
    call    r11

    mov     r11, [rbp-64]           ; out
    mov     [r11+0],  rax
    mov     [r11+8],  rdx
    mov     [r11+16], rbx
    mov     rax, 0x2222222222222222 ; rbp reported preserved, not checked
    mov     [r11+24], rax
    mov     [r11+32], rdi
    mov     [r11+40], rsi
    mov     [r11+48], r12
    mov     [r11+56], r13
    mov     [r11+64], r14
    mov     [r11+72], r15
    pushfq
    pop     rax
    mov     [r11+80], rax

    lea     rsp, [rbp-56]           ; drop the variable area + locals
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rdi
    pop     rsi
    pop     rbx
    pop     rbp
    ret
ASM_ENDFUNC asm_call_capture_args_win64


; void asm_call_capture_vec_win64(win64_regs_t *out, void *fn,
;                                 const long long *iargs,
;                                 const win64_vec128_t *vargs);
;
; The Win64 mirror of asm_call_capture_vec: marshals 4 integer args (rcx/rdx/r8/
; r9) and 4 full 128-bit vector args (xmm0..3), seeds the *callee-saved* vector
; registers xmm6..15 with sentinels (lanes both = the register index), calls fn,
; then captures the GP set, RFLAGS, the FP return (xmm0), and the whole vector
; file xmm0..15 into out->vec[]. xmm6..15 are saved/restored around the call so
; this trampoline honours its own ABI to its caller.
;
; Inbound (Win64): out = rcx, fn = rdx, iargs = r8, vargs = r9.
ASM_FUNC asm_call_capture_vec_win64
    push    rbp
    mov     rbp, rsp
    push    rbx
    push    rsi
    push    rdi
    push    r12
    push    r13
    push    r14
    push    r15                     ; 7 saved regs; rsp now 8 (mod 16)
    ; Reserve 216 (8 mod 16) so rsp is 16-aligned at the call:
    ;   [rsp+0..31]    shadow space
    ;   [rsp+32]       stash out
    ;   [rsp+40]       stash fn
    ;   [rsp+48..207]  xmm6..15 save (10 * 16)
    ;   [rsp+208..215] padding
    sub     rsp, 216
    mov     [rsp+32], rcx           ; out
    mov     [rsp+40], rdx           ; fn

    ; Preserve the callee-saved vector registers we are about to seed.
    movdqu  [rsp+48],  xmm6
    movdqu  [rsp+64],  xmm7
    movdqu  [rsp+80],  xmm8
    movdqu  [rsp+96],  xmm9
    movdqu  [rsp+112], xmm10
    movdqu  [rsp+128], xmm11
    movdqu  [rsp+144], xmm12
    movdqu  [rsp+160], xmm13
    movdqu  [rsp+176], xmm14
    movdqu  [rsp+192], xmm15

    ; Vector args: vargs (r9) -> xmm0..3 (full 128 bits).
    movdqu  xmm0, [r9+0]
    movdqu  xmm1, [r9+16]
    movdqu  xmm2, [r9+32]
    movdqu  xmm3, [r9+48]

    ; Integer args: iargs (r8) -> rcx, rdx, r8, r9 (load r8/r9 bases last).
    mov     rax, r8                 ; rax = &iargs
    mov     rcx, [rax+0]
    mov     rdx, [rax+8]
    mov     r9,  [rax+24]
    mov     r8,  [rax+16]

    mov     rbx, 0x1111111111111111
    mov     rsi, 0xCCCCCCCCCCCCCCCC
    mov     rdi, 0xDDDDDDDDDDDDDDDD
    mov     r12, 0x3333333333333333
    mov     r13, 0x4444444444444444
    mov     r14, 0x5555555555555555
    mov     r15, 0x6666666666666666

    ; Seed xmm6..15 with sentinels: xmm(6+k) = { 6+k, 6+k }.
    mov     rax, 6
    movq    xmm6, rax
    movlhps xmm6, xmm6
    mov     rax, 7
    movq    xmm7, rax
    movlhps xmm7, xmm7
    mov     rax, 8
    movq    xmm8, rax
    movlhps xmm8, xmm8
    mov     rax, 9
    movq    xmm9, rax
    movlhps xmm9, xmm9
    mov     rax, 10
    movq    xmm10, rax
    movlhps xmm10, xmm10
    mov     rax, 11
    movq    xmm11, rax
    movlhps xmm11, xmm11
    mov     rax, 12
    movq    xmm12, rax
    movlhps xmm12, xmm12
    mov     rax, 13
    movq    xmm13, rax
    movlhps xmm13, xmm13
    mov     rax, 14
    movq    xmm14, rax
    movlhps xmm14, xmm14
    mov     rax, 15
    movq    xmm15, rax
    movlhps xmm15, xmm15

    mov     r11, [rsp+40]           ; fn
    call    r11

    mov     r11, [rsp+32]           ; out
    mov     [r11+0],  rax
    mov     [r11+8],  rdx
    mov     [r11+16], rbx
    mov     rax, 0x2222222222222222 ; rbp reported preserved, not checked
    mov     [r11+24], rax
    mov     [r11+32], rdi
    mov     [r11+40], rsi
    mov     [r11+48], r12
    mov     [r11+56], r13
    mov     [r11+64], r14
    mov     [r11+72], r15
    pushfq
    pop     rax
    mov     [r11+80], rax
    movsd   [r11+88], xmm0          ; fret

    ; Full vector file xmm0..15 -> vec[] at offset 96 (16 bytes each).
    movdqu  [r11+96],  xmm0
    movdqu  [r11+112], xmm1
    movdqu  [r11+128], xmm2
    movdqu  [r11+144], xmm3
    movdqu  [r11+160], xmm4
    movdqu  [r11+176], xmm5
    movdqu  [r11+192], xmm6
    movdqu  [r11+208], xmm7
    movdqu  [r11+224], xmm8
    movdqu  [r11+240], xmm9
    movdqu  [r11+256], xmm10
    movdqu  [r11+272], xmm11
    movdqu  [r11+288], xmm12
    movdqu  [r11+304], xmm13
    movdqu  [r11+320], xmm14
    movdqu  [r11+336], xmm15

    ; Restore the callee-saved vector registers for our caller.
    movdqu  xmm6,  [rsp+48]
    movdqu  xmm7,  [rsp+64]
    movdqu  xmm8,  [rsp+80]
    movdqu  xmm9,  [rsp+96]
    movdqu  xmm10, [rsp+112]
    movdqu  xmm11, [rsp+128]
    movdqu  xmm12, [rsp+144]
    movdqu  xmm13, [rsp+160]
    movdqu  xmm14, [rsp+176]
    movdqu  xmm15, [rsp+192]

    lea     rsp, [rbp-56]
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rdi
    pop     rsi
    pop     rbx
    pop     rbp
    ret
ASM_ENDFUNC asm_call_capture_vec_win64
