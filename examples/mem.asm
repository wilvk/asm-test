; mem.asm — NASM counterpart of mem.s (x86-64, System V ABI).
;
; void fill_bytes(void *buf, long val, long n);  buf -> rdi, val -> rsi, n -> rdx
%include "asm_nasm.inc"

ASM_FUNC fill_bytes
    test    rdx, rdx            ; n == 0 ? nothing to do
    je      .done
    xor     rcx, rcx            ; i = 0
.loop:
    mov     byte [rdi + rcx], sil  ; buf[i] = (byte)val
    inc     rcx
    cmp     rcx, rdx
    jb      .loop
.done:
    ret
ASM_ENDFUNC fill_bytes
