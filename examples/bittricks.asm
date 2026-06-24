; bittricks.asm — NASM counterpart of bittricks.s (x86-64, System V ABI).
; Built with ASM_SYNTAX=nasm. See bittricks.s for the routine descriptions and
; test_bittricks.c for the property-test reference models.
%include "asm_nasm.inc"

; unsigned long popcount64(unsigned long x);  x -> rdi, result -> rax (SWAR)
ASM_FUNC popcount64
    mov     rax, rdi
    mov     rcx, rdi
    shr     rcx, 1
    mov     rdx, 0x5555555555555555
    and     rcx, rdx
    sub     rax, rcx
    mov     rdx, 0x3333333333333333
    mov     rcx, rax
    shr     rcx, 2
    and     rax, rdx
    and     rcx, rdx
    add     rax, rcx
    mov     rcx, rax
    shr     rcx, 4
    add     rax, rcx
    mov     rdx, 0x0F0F0F0F0F0F0F0F
    and     rax, rdx
    mov     rdx, 0x0101010101010101
    imul    rax, rdx
    shr     rax, 56
    ret
ASM_ENDFUNC popcount64

; unsigned long next_pow2(unsigned long x);  smallest power of two >= x (x >= 1)
ASM_FUNC next_pow2
    mov     rax, rdi
    dec     rax
    mov     rcx, rax
    shr     rcx, 1
    or      rax, rcx
    mov     rcx, rax
    shr     rcx, 2
    or      rax, rcx
    mov     rcx, rax
    shr     rcx, 4
    or      rax, rcx
    mov     rcx, rax
    shr     rcx, 8
    or      rax, rcx
    mov     rcx, rax
    shr     rcx, 16
    or      rax, rcx
    mov     rcx, rax
    shr     rcx, 32
    or      rax, rcx
    inc     rax
    ret
ASM_ENDFUNC next_pow2

; unsigned long reverse_byte(unsigned long x);  reverse the low 8 bits
ASM_FUNC reverse_byte
    movzx   eax, dil
    mov     rcx, rax
    shr     rcx, 1
    and     rcx, 0x55
    and     rax, 0x55
    shl     rax, 1
    or      rax, rcx
    mov     rcx, rax
    shr     rcx, 2
    and     rcx, 0x33
    and     rax, 0x33
    shl     rax, 2
    or      rax, rcx
    mov     rcx, rax
    shr     rcx, 4
    and     rcx, 0x0F
    and     rax, 0x0F
    shl     rax, 4
    or      rax, rcx
    ret
ASM_ENDFUNC reverse_byte
