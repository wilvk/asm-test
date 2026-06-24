; tests/win64/smoke_asm.asm — Phase 0 substrate smoke for the native Win64 tier.
;
; A trivial Win64-ABI leaf: under the Microsoft x64 convention the first integer
; argument arrives in RCX (not RDI as on System V), and the return value goes in
; RAX. Assembled with `nasm -f win64`, linked by x86_64-w64-mingw32-gcc, and run
; under Wine — this proves the exact cross-assemble -> cross-link -> Wine chain
; the Win64 capture trampoline (Phase 1) will use, before any trampoline lands.
;
; Win64 x86-64 uses no leading-underscore name decoration, so the C side links to
; `win64_add3` directly.

        global win64_add3
        section .text

win64_add3:                     ; long long win64_add3(long long x)  [x in rcx]
        lea     rax, [rcx + 3]  ; rax = x + 3
        ret
