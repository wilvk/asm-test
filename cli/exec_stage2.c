/* exec_stage2.c — the binary exec_victim execve()s into (asmspy-plan Theme B).
 *
 * postexec_fn exists ONLY here. asmspy can name it only if it re-read the
 * symbol table at the exec-stop: the table it loaded at attach was
 * exec_victim's, and this image's text sits at a different load bias entirely.
 * So "postexec_fn [exec_stage2] appeared in the stream" is a direct, positive
 * proof of re-resolution — it cannot be produced by the stale table.
 *
 * FREESTANDING ON PURPOSE (-nostdlib, raw syscalls, our own _start).
 *
 * This binary is single-stepped from the exec-stop onwards, so everything it
 * runs before postexec_fn is charged to the smoke's step budget. MEASURED, back
 * when it was an ordinary -static glibc program:
 *
 *     pre-exec                   ~  1,224 steps
 *     post-exec glibc startup      134,848 steps  <- before main() ever ran
 *       of which eh_frame FDE      118,941 (88%)  classify_object_over_fdes
 *                                                 + read_encoded_value_with_base
 *     first postexec_fn at         136,072 steps
 *
 * 87% of the entire run was glibc registering the unwind tables of a static libc
 * this test never calls. That left only a 1.47x margin against the budget, on a
 * count that measurably moves with the environment (136,629 -> 140,893 from
 * ifunc dispatch alone) — which is what turned CI red.
 *
 * Worth recording: the reasoning that made this -static in the first place —
 * "ld.so costs >20k instructions before main" — traded 20k for 135k. Static
 * glibc was WORSE here, not better. Freestanding drops the loader AND libc, so
 * postexec_fn is reached ~20 steps after the exec and the budget stops being a
 * variable. None of the proof changes: this is still a separate image, at its
 * own load bias, with its own .symtab, which the pre-exec table cannot name.
 */

/* Linux syscall numbers — no libc headers are available here. */
#if defined(__aarch64__)
#define SYS_write      64
#define SYS_nanosleep  101
#define SYS_exit_group 94
#else
#define SYS_write      1
#define SYS_nanosleep  35
#define SYS_exit_group 231
#endif

static long sys3(long n, long a, long b, long c) {
#if defined(__aarch64__)
    /* AArch64: number in x8, args in x0..x5, return in x0, trap via `svc #0`. */
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2)
                     : "memory");
    return x0;
#else
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
#endif
}

/* The symbol the whole test turns on: present only in THIS image. noinline so it
 * keeps a real .symtab entry, with a size, for asmspy to resolve into. */
__attribute__((noinline)) long postexec_fn(long x) { return x * 7 + 5; }

__attribute__((noreturn)) void stage2_main(void);
void stage2_main(void) {
    static const char msg[] = "exec_stage2 running (freestanding)\n";
    sys3(SYS_write, 2, (long)msg, (long)(sizeof msg - 1));

    volatile long sink = 0;
    for (;;) {
        /* A BATCH per sleep, deliberately: --graph counts CALLS, so one call per
         * 2ms sleep would turn its budget into a wall-clock wait. Under a
         * single-stepper the whole batch is still only a few thousand steps. */
        for (int i = 0; i < 256; i++)
            sink += postexec_fn(i);
        struct {
            long tv_sec, tv_nsec;
        } ts = {0, 2 * 1000 * 1000}; /* 2ms: idle politely when not traced */
        sys3(SYS_nanosleep, (long)&ts, 0, 0);
    }
}

/* Our own entry point: with -nostdlib there is no crt1.o to set one up. The
 * kernel enters here with a 16-byte-aligned SP; the ABI keeps it 16-aligned.
 * stage2_main never returns. */
#if defined(__aarch64__)
__asm__(".globl _start\n"
        "_start:\n"
        "  mov x29, #0\n" /* clear the frame pointer: the backtrace root */
        "  bl stage2_main\n"
        "  brk #0\n"); /* never reached; a trap net if stage2_main returned */
#else
__asm__(".globl _start\n"
        "_start:\n"
        "  xorl %ebp, %ebp\n"
        "  andq $-16, %rsp\n"
        "  call stage2_main\n"
        "  hlt\n");
#endif
