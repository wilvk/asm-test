; callback.asm — NASM counterpart of callback.s (x86-64). See callback.s for the
; routine contracts and the C callbacks/assertions in test_callback.c.
%include "asm_nasm.inc"

ASM_FUNC sum_map
    push    rbx                 ; arr cursor
    push    r12                 ; remaining count
    push    r13                 ; callback pointer
    push    r14                 ; accumulator
    push    r15                 ; align: 5 pushes -> rsp 16-aligned at call
    mov     rbx, rdi
    mov     r12, rsi
    mov     r13, rdx
    xor     r14, r14
.loop:
    test    r12, r12
    jle     .done               ; signed: n <= 0 ends the loop
    mov     rdi, [rbx]          ; marshal arr[i] into the 1st int arg
    add     rbx, 8
    call    r13                 ; clobbers caller-saved regs; ours are safe
    add     r14, rax
    dec     r12
    jmp     .loop
.done:
    mov     rax, r14
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret
ASM_ENDFUNC sum_map

ASM_FUNC count_if
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    mov     rbx, rdi
    mov     r12, rsi
    mov     r13, rdx
    xor     r14, r14
.loop:
    test    r12, r12
    jle     .done
    mov     rdi, [rbx]
    add     rbx, 8
    call    r13
    test    rax, rax            ; predicate nonzero?
    jz      .skip
    inc     r14
.skip:
    dec     r12
    jmp     .loop
.done:
    mov     rax, r14
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret
ASM_ENDFUNC count_if
