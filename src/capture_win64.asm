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


; void asm_call_capture_fp_win64(win64_regs_t *out, void *fn,
;                                const long long *iargs, const double *fargs);
;
; The Win64 mirror of asm_call_capture_fp: like asm_call_capture_win64 but also
; marshals 4 double args into xmm0..3 and captures the FP return (xmm0) into
; out->fret. Win64 has no callee-saved xmm to seed at this tier (that is _vec's
; job), so rbp stays a seeded/checked GP callee-saved as in the core variant.
;
; Inbound (Win64): out = rcx, fn = rdx, iargs = r8, fargs = r9.
ASM_FUNC asm_call_capture_fp_win64
    push    rbx
    push    rbp
    push    rsi
    push    rdi
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 72                 ; same frame as the core variant
    mov     [rsp+48], rcx           ; stash out
    mov     [rsp+56], rdx           ; stash fn

    ; Float args: fargs (r9) -> xmm0..3 (load before r9 is reused for iargs).
    movsd   xmm0, [r9+0]
    movsd   xmm1, [r9+8]
    movsd   xmm2, [r9+16]
    movsd   xmm3, [r9+24]

    ; Integer args: iargs (r8) -> rcx, rdx, r8, r9 + two stack slots.
    mov     rax, r8                 ; rax = &iargs
    mov     rcx, [rax+0]
    mov     rdx, [rax+8]
    mov     r9,  [rax+24]
    mov     r10, [rax+32]
    mov     [rsp+32], r10           ; iargs[4] (5th arg)
    mov     r10, [rax+40]
    mov     [rsp+40], r10           ; iargs[5] (6th arg)
    mov     r8,  [rax+16]

    mov     rbx, 0x1111111111111111
    mov     rbp, 0x2222222222222222
    mov     rsi, 0xCCCCCCCCCCCCCCCC
    mov     rdi, 0xDDDDDDDDDDDDDDDD
    mov     r12, 0x3333333333333333
    mov     r13, 0x4444444444444444
    mov     r14, 0x5555555555555555
    mov     r15, 0x6666666666666666

    mov     r11, [rsp+56]
    call    r11

    mov     r11, [rsp+48]
    mov     [r11+0],  rax
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
    mov     [r11+80], rax
    movsd   [r11+88], xmm0          ; fret

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
ASM_ENDFUNC asm_call_capture_fp_win64


; void asm_call_capture_fp_n_win64(win64_regs_t *out, void *fn,
;                                  const long long *iargs, const double *fargs,
;                                  int nfargs);
;
; The Win64 mirror of asm_call_capture_fp_n: arbitrary FP arity — the first 4
; doubles go in xmm0..3, the rest spill onto the stack above the shadow space.
; 4 integer args go in rcx/rdx/r8/r9. rbp frames the variable area (reported
; preserved, not checked). nfargs is the 5th arg, so it arrives on the stack at
; [rbp+48] (after the saved rbp, return address, and 32-byte shadow space).
;
; Inbound (Win64): out = rcx, fn = rdx, iargs = r8, fargs = r9, nfargs @ [rbp+48].
ASM_FUNC asm_call_capture_fp_n_win64
    push    rbp
    mov     rbp, rsp
    push    rbx
    push    rsi
    push    rdi
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 64
    mov     [rbp-64], rcx           ; out
    mov     [rbp-72], rdx           ; fn
    mov     [rbp-80], r8            ; iargs
    mov     [rbp-88], r9            ; fargs
    movsxd  rax, dword [rbp+48]     ; nfargs (5th arg, on the stack)
    mov     [rbp-96], rax

    ; n_stack = max(0, nfargs - 4) -> r10  (Win64 has 4 FP arg registers)
    mov     r10, rax
    sub     r10, 4
    jg      .fpn_have_stack
    xor     r10, r10
.fpn_have_stack:
    and     rsp, -16
    mov     rax, r10
    shl     rax, 3
    add     rax, 32                 ; shadow space
    add     rax, 15
    and     rax, -16
    sub     rsp, rax

    ; Copy overflow doubles: [rsp+32 + i*8] = fargs[4 + i].
    mov     r8, [rbp-88]            ; fargs
    xor     rcx, rcx
.fpn_copy:
    cmp     rcx, r10
    jge     .fpn_copy_done
    mov     rax, [r8 + 32 + rcx*8]  ; fargs[4 + i] (byte 32 = 4*8)
    mov     [rsp + 32 + rcx*8], rax
    inc     rcx
    jmp     .fpn_copy
.fpn_copy_done:
    ; Load FP register args xmm0..3 (only as many as nfargs).
    mov     r11, [rbp-88]           ; fargs
    mov     r10, [rbp-96]           ; nfargs
    cmp     r10, 1
    jl      .fpn_loadint
    movsd   xmm0, [r11+0]
    cmp     r10, 2
    jl      .fpn_loadint
    movsd   xmm1, [r11+8]
    cmp     r10, 3
    jl      .fpn_loadint
    movsd   xmm2, [r11+16]
    cmp     r10, 4
    jl      .fpn_loadint
    movsd   xmm3, [r11+24]
.fpn_loadint:
    ; Integer args: iargs -> rcx/rdx/r8/r9.
    mov     rax, [rbp-80]
    mov     rcx, [rax+0]
    mov     rdx, [rax+8]
    mov     r8,  [rax+16]
    mov     r9,  [rax+24]

    mov     rbx, 0x1111111111111111
    mov     rsi, 0xCCCCCCCCCCCCCCCC
    mov     rdi, 0xDDDDDDDDDDDDDDDD
    mov     r12, 0x3333333333333333
    mov     r13, 0x4444444444444444
    mov     r14, 0x5555555555555555
    mov     r15, 0x6666666666666666

    mov     r11, [rbp-72]
    call    r11

    mov     r11, [rbp-64]
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
ASM_ENDFUNC asm_call_capture_fp_n_win64


; void asm_call_capture_sret_win64(win64_regs_t *out, void *fn, void *result,
;                                  const long long *args, int nargs);
;
; The Win64 mirror of asm_call_capture_sret: fn returns a large struct via a
; hidden result pointer, which on Win64 is the *first* argument (rcx). The
; visible integer args therefore shift to rdx/r8/r9 (3 register slots), then the
; stack. The callee returns the result pointer in rax. rbp frames the variable
; area (reported preserved, not checked); nargs is the 5th arg at [rbp+48].
;
; Inbound (Win64): out = rcx, fn = rdx, result = r8, args = r9, nargs @ [rbp+48].
ASM_FUNC asm_call_capture_sret_win64
    push    rbp
    mov     rbp, rsp
    push    rbx
    push    rsi
    push    rdi
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 64
    mov     [rbp-64], rcx           ; out
    mov     [rbp-72], rdx           ; fn
    mov     [rbp-80], r8            ; result (hidden sret pointer)
    mov     [rbp-88], r9            ; args
    movsxd  rax, dword [rbp+48]     ; nargs (5th arg, on the stack)
    mov     [rbp-96], rax

    ; n_stack = max(0, nargs - 3)  (3 visible register slots: rdx, r8, r9)
    mov     r10, rax
    sub     r10, 3
    jg      .sret_have_stack
    xor     r10, r10
.sret_have_stack:
    and     rsp, -16
    mov     rax, r10
    shl     rax, 3
    add     rax, 32                 ; shadow space
    add     rax, 15
    and     rax, -16
    sub     rsp, rax

    ; Copy overflow args: [rsp+32 + i*8] = args[3 + i].
    mov     r8, [rbp-88]            ; args
    xor     rcx, rcx
.sret_copy:
    cmp     rcx, r10
    jge     .sret_copy_done
    mov     rax, [r8 + 24 + rcx*8]  ; args[3 + i] (byte 24 = 3*8)
    mov     [rsp + 32 + rcx*8], rax
    inc     rcx
    jmp     .sret_copy
.sret_copy_done:
    ; Visible register args -> rdx, r8, r9 (only as many as nargs).
    mov     r11, [rbp-88]           ; args
    mov     r10, [rbp-96]           ; nargs
    cmp     r10, 1
    jl      .sret_seed
    mov     rdx, [r11+0]
    cmp     r10, 2
    jl      .sret_seed
    mov     r8,  [r11+8]
    cmp     r10, 3
    jl      .sret_seed
    mov     r9,  [r11+16]
.sret_seed:
    mov     rcx, [rbp-80]           ; hidden result pointer -> rcx (1st arg)
    mov     rbx, 0x1111111111111111
    mov     rsi, 0xCCCCCCCCCCCCCCCC
    mov     rdi, 0xDDDDDDDDDDDDDDDD
    mov     r12, 0x3333333333333333
    mov     r13, 0x4444444444444444
    mov     r14, 0x5555555555555555
    mov     r15, 0x6666666666666666

    mov     r11, [rbp-72]
    call    r11

    mov     r11, [rbp-64]
    mov     [r11+0],  rax           ; ret (= result pointer per the ABI)
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
ASM_ENDFUNC asm_call_capture_sret_win64


; void asm_call_capture_vec_n_win64(win64_regs_t *out, void *fn,
;                                   const long long *iargs,
;                                   const win64_vec128_t *vargs, int nvargs);
;
; The Win64 mirror of asm_call_capture_vec_n: arbitrary vector arity using the
; vectorcall-style xmm convention (matching _vec) — the first 4 vectors go in
; xmm0..3, the rest spill onto the stack as full 128-bit values above the shadow
; space. 4 int args in rcx/rdx/r8/r9. Saves/seeds/captures/restores the
; callee-saved xmm6..15 and captures the whole vector file, as in _vec. rbp
; frames the variable area; nvargs is the 5th arg at [rbp+48]. The fixed xmm6..15
; save area lives at [rbp-272 .. rbp-128], addressed via rbp.
;
; Inbound (Win64): out = rcx, fn = rdx, iargs = r8, vargs = r9, nvargs @ [rbp+48].
ASM_FUNC asm_call_capture_vec_n_win64
    push    rbp
    mov     rbp, rsp
    push    rbx
    push    rsi
    push    rdi
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 224                ; fixed area: stashes + xmm6..15 save
    mov     [rbp-64], rcx           ; out
    mov     [rbp-72], rdx           ; fn
    mov     [rbp-80], r8            ; iargs
    mov     [rbp-88], r9            ; vargs
    movsxd  rax, dword [rbp+48]     ; nvargs (5th arg, on the stack)
    mov     [rbp-96], rax

    ; Preserve callee-saved xmm6..15 into the fixed area.
    movdqu  [rbp-272], xmm6
    movdqu  [rbp-256], xmm7
    movdqu  [rbp-240], xmm8
    movdqu  [rbp-224], xmm9
    movdqu  [rbp-208], xmm10
    movdqu  [rbp-192], xmm11
    movdqu  [rbp-176], xmm12
    movdqu  [rbp-160], xmm13
    movdqu  [rbp-144], xmm14
    movdqu  [rbp-128], xmm15

    ; n_stack = max(0, nvargs - 4) -> r10
    mov     r10, rax
    sub     r10, 4
    jg      .vn_have_stack
    xor     r10, r10
.vn_have_stack:
    and     rsp, -16
    mov     rax, r10
    shl     rax, 4                  ; n_stack * 16
    add     rax, 32                 ; shadow space
    add     rax, 15
    and     rax, -16
    sub     rsp, rax

    ; Copy overflow vectors: [rsp+32 + i*16] = vargs[4 + i] (16 bytes).
    mov     r8, [rbp-88]            ; vargs
    xor     rcx, rcx
.vn_copy:
    cmp     rcx, r10
    jge     .vn_copy_done
    mov     rax, rcx
    shl     rax, 4                  ; i * 16
    movdqu  xmm4, [r8 + 64 + rax]   ; vargs[4 + i] (byte 64 = 4*16)
    movdqu  [rsp + 32 + rax], xmm4
    inc     rcx
    jmp     .vn_copy
.vn_copy_done:
    ; Vector register args xmm0..3 (only as many as nvargs).
    mov     r11, [rbp-88]           ; vargs
    mov     r10, [rbp-96]           ; nvargs
    cmp     r10, 1
    jl      .vn_loadint
    movdqu  xmm0, [r11+0]
    cmp     r10, 2
    jl      .vn_loadint
    movdqu  xmm1, [r11+16]
    cmp     r10, 3
    jl      .vn_loadint
    movdqu  xmm2, [r11+32]
    cmp     r10, 4
    jl      .vn_loadint
    movdqu  xmm3, [r11+48]
.vn_loadint:
    mov     rax, [rbp-80]           ; iargs
    mov     rcx, [rax+0]
    mov     rdx, [rax+8]
    mov     r8,  [rax+16]
    mov     r9,  [rax+24]

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

    mov     r11, [rbp-72]
    call    r11

    mov     r11, [rbp-64]
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

    ; Restore callee-saved xmm6..15 for our caller.
    movdqu  xmm6,  [rbp-272]
    movdqu  xmm7,  [rbp-256]
    movdqu  xmm8,  [rbp-240]
    movdqu  xmm9,  [rbp-224]
    movdqu  xmm10, [rbp-208]
    movdqu  xmm11, [rbp-192]
    movdqu  xmm12, [rbp-176]
    movdqu  xmm13, [rbp-160]
    movdqu  xmm14, [rbp-144]
    movdqu  xmm15, [rbp-128]

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
ASM_ENDFUNC asm_call_capture_vec_n_win64


; void asm_call_capture_bigstruct_win64(win64_regs_t *out, void *fn,
;                                       const long long *iargs, int niargs,
;                                       const void *sptr, size_t ssize);
;
; Win64 passes large structs BY REFERENCE (unlike System V's inline memory
; class): the caller makes a private copy and passes a pointer to it. This
; trampoline copies the ssize-byte struct and passes the copy's pointer as the
; argument right after the niargs plain integer args. niargs must be <= 3 so the
; struct pointer lands in a register slot (rcx/rdx/r8/r9). rbp frames the copy;
; reported preserved, not checked.
;
; Inbound (Win64): out=rcx, fn=rdx, iargs=r8, niargs=r9d, sptr@[rbp+48],
;                  ssize@[rbp+56].
ASM_FUNC asm_call_capture_bigstruct_win64
    push    rbp
    mov     rbp, rsp
    push    rbx
    push    rsi
    push    rdi
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 64
    mov     [rbp-64], rcx           ; out
    mov     [rbp-72], rdx           ; fn
    mov     [rbp-80], r8            ; iargs
    movsxd  rax, r9d
    mov     [rbp-88], rax           ; niargs
    mov     rax, [rbp+48]           ; sptr  (5th arg, pointer)
    mov     [rbp-96], rax
    mov     rax, [rbp+56]           ; ssize (6th arg)
    mov     [rbp-104], rax

    ; Reserve 32-byte shadow + round_up(ssize, 16); the struct copy at [rsp+32].
    add     rax, 15
    and     rax, -16
    add     rax, 32
    and     rsp, -16
    sub     rsp, rax

    ; Byte-copy the struct: [rsp+32 + i] = sptr[i].
    mov     r8, [rbp-96]            ; sptr
    mov     r9, [rbp-104]           ; ssize
    xor     rcx, rcx
.bs_copy:
    cmp     rcx, r9
    jge     .bs_copy_done
    mov     al, [r8 + rcx]
    mov     [rsp + 32 + rcx], al
    inc     rcx
    jmp     .bs_copy
.bs_copy_done:
    lea     rax, [rsp+32]           ; pointer to the struct copy
    mov     [rbp-112], rax

    ; Place iargs[0..niargs-1] then the struct pointer at slot `niargs` (<= 3).
    mov     r11, [rbp-80]           ; iargs
    mov     r10, [rbp-88]           ; niargs
    cmp     r10, 1
    jge     .bs_s0int
    mov     rcx, [rbp-112]          ; niargs==0: struct ptr -> rcx
    jmp     .bs_slots_done
.bs_s0int:
    mov     rcx, [r11+0]
    cmp     r10, 2
    jge     .bs_s1int
    mov     rdx, [rbp-112]          ; niargs==1: struct ptr -> rdx
    jmp     .bs_slots_done
.bs_s1int:
    mov     rdx, [r11+8]
    cmp     r10, 3
    jge     .bs_s2int
    mov     r8, [rbp-112]           ; niargs==2: struct ptr -> r8
    jmp     .bs_slots_done
.bs_s2int:
    mov     r8, [r11+16]
    mov     r9, [rbp-112]           ; niargs==3: struct ptr -> r9
.bs_slots_done:
    mov     rbx, 0x1111111111111111
    mov     rsi, 0xCCCCCCCCCCCCCCCC
    mov     rdi, 0xDDDDDDDDDDDDDDDD
    mov     r12, 0x3333333333333333
    mov     r13, 0x4444444444444444
    mov     r14, 0x5555555555555555
    mov     r15, 0x6666666666666666

    mov     r11, [rbp-72]
    call    r11

    mov     r11, [rbp-64]
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
ASM_ENDFUNC asm_call_capture_bigstruct_win64

; void asm_call_capture_vec256_win64(vec256_t *vec, void *fn,
;                                    const long long *iargs,
;                                    const vec256_t *vargs);
;
; The Win64 AVX2 analog of asm_call_capture_vec256: marshals 4 integer args
; (rcx/rdx/r8/r9) and 4 full 256-bit vector args (ymm0..3), calls fn, then
; captures the whole ymm file ymm0..15 into vec[0..15] (32 bytes each; vec[0] =
; the return). The callee-saved low 128 of xmm6..15 is saved/restored so this
; trampoline honours Win64 to its own caller (the upper 128 of ymm6..15 is
; volatile per the ABI, and vzeroupper clears it on exit). Captures the vector
; file only — the 128-bit path covers GP / flags. AVX2 only: the caller gates on
; asmtest_cpu_has_avx2(), so a non-AVX2 host never reaches the VEX body.
;
; Inbound (Win64): vec = rcx, fn = rdx, iargs = r8, vargs = r9.
ASM_FUNC asm_call_capture_vec256_win64
    push    rbp
    mov     rbp, rsp                ; entry rsp == 8; after push rbp == 0 (mod 16)
    ; Reserve 224 (== 0 mod 16) so rsp stays 16-aligned at the call:
    ;   [rsp+0..31]    shadow space
    ;   [rsp+32]       stash vec (out)
    ;   [rsp+40]       stash fn
    ;   [rsp+48..207]  xmm6..15 save (10 * 16, low 128 only)
    ;   [rsp+208..223] padding
    sub     rsp, 224
    mov     [rsp+32], rcx           ; vec out
    mov     [rsp+40], rdx           ; fn

    ; Preserve the callee-saved vector registers (low 128) we are about to use.
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

    ; Vector args: vargs (r9) -> ymm0..3 (full 256 bits).
    vmovdqu ymm0, [r9+0]
    vmovdqu ymm1, [r9+32]
    vmovdqu ymm2, [r9+64]
    vmovdqu ymm3, [r9+96]

    ; Integer args: iargs (r8) -> rcx, rdx, r8, r9 (load the r8/r9 bases last).
    mov     rax, r8
    mov     rcx, [rax+0]
    mov     rdx, [rax+8]
    mov     r9,  [rax+24]
    mov     r8,  [rax+16]

    mov     r11, [rsp+40]           ; fn
    call    r11

    ; Full ymm file: ymm0..15 -> vec[0..15] (32 bytes each).
    mov     r11, [rsp+32]           ; vec out
    vmovdqu [r11+0],   ymm0
    vmovdqu [r11+32],  ymm1
    vmovdqu [r11+64],  ymm2
    vmovdqu [r11+96],  ymm3
    vmovdqu [r11+128], ymm4
    vmovdqu [r11+160], ymm5
    vmovdqu [r11+192], ymm6
    vmovdqu [r11+224], ymm7
    vmovdqu [r11+256], ymm8
    vmovdqu [r11+288], ymm9
    vmovdqu [r11+320], ymm10
    vmovdqu [r11+352], ymm11
    vmovdqu [r11+384], ymm12
    vmovdqu [r11+416], ymm13
    vmovdqu [r11+448], ymm14
    vmovdqu [r11+480], ymm15

    ; Restore the callee-saved low 128 of xmm6..15 for our caller.
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
    vzeroupper

    mov     rsp, rbp
    pop     rbp
    ret
ASM_ENDFUNC asm_call_capture_vec256_win64
