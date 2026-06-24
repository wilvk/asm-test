; routines_win64.asm — tiny routines under test, written to the Microsoft x64
; ABI (Phase 1 slice). The capture trampoline calls these through the Win64
; convention: 1st int arg in rcx, 2nd in rdx, ...; return in rax. They are never
; called directly from C — only their addresses are handed to the trampoline.
;
; Assembles under `-f win64` (PE/Wine) and `-f macho64`/`-f elf64` (ms_abi lane).

%include "asm_nasm.inc"

; long long win64_ret_arg0(a, ...) -> a   (the 1st integer arg, rcx)
ASM_FUNC win64_ret_arg0
    mov     rax, rcx
    ret
ASM_ENDFUNC win64_ret_arg0

; long long win64_sum2(a, b) -> a + b   (rcx + rdx)
ASM_FUNC win64_sum2
    lea     rax, [rcx + rdx]
    ret
ASM_ENDFUNC win64_sum2

; Well-behaved: uses rbx but saves/restores it, returns a (rcx). The
; ABI-preservation check must see rbx's sentinel intact afterwards.
ASM_FUNC win64_preserve_rbx
    push    rbx
    mov     rbx, rcx
    mov     rax, rbx
    pop     rbx
    ret
ASM_ENDFUNC win64_preserve_rbx

; Misbehaved: clobbers rbx without restoring it (the ABI violation the framework
; is built to catch). Still returns a (rcx).
ASM_FUNC win64_clobber_rbx
    xor     rbx, rbx
    mov     rax, rcx
    ret
ASM_ENDFUNC win64_clobber_rbx

; Returns the 5th integer arg (index 4), which arrives on the stack above the
; 32-byte shadow space. On entry: [rsp]=retaddr, [rsp+8..39]=shadow,
; [rsp+40]=arg5. Exercises stack-arg marshalling + shadow-space correctness.
ASM_FUNC win64_ret_arg4
    mov     rax, [rsp+40]
    ret
ASM_ENDFUNC win64_ret_arg4
