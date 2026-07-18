/*
 * fault.s — a routine that faults under the emulator, for the conformance
 * corpus's fault-as-data case. read_fault(p) dereferences its pointer arg, so
 * the emulator turns a load of an unmapped guest address into a recorded fault
 * (fault_addr == p, fault_kind == read) rather than a process crash.
 *
 *   long read_fault(const long *p);   return *p   (one load; faults if p unmapped)
 */
#include "asm.h"

ASM_FUNC read_fault
#if defined(__x86_64__)
    movq    (%rdi), %rax        /* load *p — faults if p is unmapped */
    ret
#elif defined(__aarch64__)
    ldr     x0, [x0]            /* load *p */
    ret
#elif defined(__riscv) && __riscv_xlen == 64
    ld      a0, 0(a0)           /* load *p — faults if p is unmapped */
    ret
#endif
ASM_ENDFUNC read_fault
