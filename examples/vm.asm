; vm.asm — NASM counterpart of vm.s (x86-64, System V ABI). A tiny RPN bytecode
; interpreter; see vm.s for the opcode encoding and test_vm.c for the programs.
;   long vm_eval(const signed char *code, long n);  code -> rdi, n -> rsi
%include "asm_nasm.inc"

ASM_FUNC vm_eval
    push    rbx                     ; i (program cursor)
    push    r12                     ; code pointer
    push    r13                     ; n
    push    r14                     ; operand-stack depth
    push    r15                     ; operand-stack base
    sub     rsp, 256                ; reserve the operand stack
    mov     r15, rsp
    mov     r12, rdi
    mov     r13, rsi
    xor     rbx, rbx
    xor     r14, r14
.loop:
    cmp     rbx, r13
    jge     .end
    movsx   rax, byte [r12 + rbx]   ; sign-extend code[i]
    inc     rbx
    test    rax, rax
    jns     .push                   ; >= 0 -> push the literal
    cmp     rax, -1
    je      .add
    cmp     rax, -2
    je      .sub
    cmp     rax, -3
    je      .mul
    cmp     rax, -4
    je      .neg
    jmp     .loop                   ; unknown opcode: ignore
.push:
    mov     [r15 + r14*8], rax
    inc     r14
    jmp     .loop
.add:
    dec     r14
    mov     rdx, [r15 + r14*8]      ; b (top)
    dec     r14
    mov     rax, [r15 + r14*8]      ; a
    add     rax, rdx
    mov     [r15 + r14*8], rax
    inc     r14
    jmp     .loop
.sub:
    dec     r14
    mov     rdx, [r15 + r14*8]
    dec     r14
    mov     rax, [r15 + r14*8]
    sub     rax, rdx
    mov     [r15 + r14*8], rax
    inc     r14
    jmp     .loop
.mul:
    dec     r14
    mov     rdx, [r15 + r14*8]
    dec     r14
    mov     rax, [r15 + r14*8]
    imul    rax, rdx
    mov     [r15 + r14*8], rax
    inc     r14
    jmp     .loop
.neg:
    dec     r14
    mov     rax, [r15 + r14*8]
    neg     rax
    mov     [r15 + r14*8], rax
    inc     r14
    jmp     .loop
.end:
    xor     rax, rax
    test    r14, r14
    jz      .done
    mov     rax, [r15 + r14*8 - 8]
.done:
    add     rsp, 256
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret
ASM_ENDFUNC vm_eval
