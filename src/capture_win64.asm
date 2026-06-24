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
