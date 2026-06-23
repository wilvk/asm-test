; flags.asm — NASM counterpart of flags.s (x86-64, System V ABI).
%include "asm_nasm.inc"

; long set_carry(void); sets CF, returns 0. (mov does not disturb flags)
ASM_FUNC set_carry
    stc
    mov     rax, 0
    ret
ASM_ENDFUNC set_carry

; long clear_carry(void); clears CF, returns 0.
ASM_FUNC clear_carry
    clc
    mov     rax, 0
    ret
ASM_ENDFUNC clear_carry

; long sum_via_rbx(long a, long b); uses rbx but restores it — ABI-compliant.
ASM_FUNC sum_via_rbx
    push    rbx
    mov     rbx, rdi
    add     rbx, rsi
    mov     rax, rbx
    pop     rbx
    ret
ASM_ENDFUNC sum_via_rbx

; long clobbers_rbx(long a, long b); trashes rbx — violates the ABI (for demo).
ASM_FUNC clobbers_rbx
    mov     rbx, rdi
    add     rbx, rsi
    mov     rax, rbx
    ret
ASM_ENDFUNC clobbers_rbx
