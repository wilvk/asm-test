/*
 * capture.s — register/flags capture trampoline.
 *
 *   void asm_call_capture(regs_t *out, void *fn, const long args[6]);
 *
 * Seeds the callee-saved registers with known sentinels, marshals up to six
 * integer args into the ABI argument registers, calls fn, then snapshots the
 * return register, the callee-saved registers (to verify the routine restored
 * them), and the condition flags into *out.
 *
 * Sentinels and struct offsets MUST match asmtest.h.
 */
#include "asm.h"

ASM_FUNC asm_call_capture

#if defined(__x86_64__)
/* --- System V AMD64 ABI ---------------------------------------------------
 * out -> %rdi, fn -> %rsi, args -> %rdx
 * args[0..5] -> rdi,rsi,rdx,rcx,r8,r9. regs_t offsets:
 *   ret 0, rdx 8, rbx 16, rbp 24, r12 32, r13 40, r14 48, r15 56, flags 64
 */
    pushq   %rbx
    pushq   %rbp
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    /* 48 bytes pushed; reserve 24 more so rsp is 16-aligned before `call`. */
    subq    $24, %rsp
    movq    %rdi, 0(%rsp)           /* stash out across the call */
    movq    %rsi, 8(%rsp)           /* stash fn  across the call */

    /* Marshal args[0..5] into the integer argument registers. */
    movq    %rdx, %rax              /* rax = &args (scratch) */
    movq    0(%rax),  %rdi
    movq    8(%rax),  %rsi
    movq    24(%rax), %rcx
    movq    32(%rax), %r8
    movq    40(%rax), %r9
    movq    16(%rax), %rdx          /* load rdx last */

    /* Seed callee-saved registers with sentinels. */
    movabsq $0x1111111111111111, %rbx
    movabsq $0x2222222222222222, %rbp
    movabsq $0x3333333333333333, %r12
    movabsq $0x4444444444444444, %r13
    movabsq $0x5555555555555555, %r14
    movabsq $0x6666666666666666, %r15

    movq    8(%rsp), %r11
    xorl    %eax, %eax              /* variadic-safe: 0 vector regs */
    call    *%r11

    /* Capture results. MOV/loads below do not disturb RFLAGS. */
    movq    0(%rsp), %r11           /* reload out */
    movq    %rax, 0(%r11)
    movq    %rdx, 8(%r11)
    movq    %rbx, 16(%r11)
    movq    %rbp, 24(%r11)
    movq    %r12, 32(%r11)
    movq    %r13, 40(%r11)
    movq    %r14, 48(%r11)
    movq    %r15, 56(%r11)
    pushfq
    popq    %rax
    movq    %rax, 64(%r11)

    addq    $24, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbp
    popq    %rbx
    ret

#elif defined(__aarch64__)
/* --- AAPCS64 --------------------------------------------------------------
 * out -> x0, fn -> x1, args -> x2
 * args[0..5] -> x0..x5. regs_t offsets:
 *   ret 0, x19 8, x20 16, ... x28 80, x29 88, flags 96
 */
    sub     sp, sp, #112
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]     /* save frame pointer and link register */
    str     x0, [sp, #96]           /* stash out across the call */
    str     x1, [sp, #104]          /* stash fn  across the call */

    /* Marshal args[0..5] into x0..x5. */
    mov     x9, x2
    ldp     x0, x1, [x9, #0]
    ldp     x2, x3, [x9, #16]
    ldp     x4, x5, [x9, #32]

    /* Seed callee-saved registers (x19-x29) with sentinels. */
    ldr     x19, =0x1111111111111111
    ldr     x20, =0x2222222222222222
    ldr     x21, =0x3333333333333333
    ldr     x22, =0x4444444444444444
    ldr     x23, =0x5555555555555555
    ldr     x24, =0x6666666666666666
    ldr     x25, =0x7777777777777777
    ldr     x26, =0x8888888888888888
    ldr     x27, =0x9999999999999999
    ldr     x28, =0xAAAAAAAAAAAAAAAA
    ldr     x29, =0xBBBBBBBBBBBBBBBB

    ldr     x10, [sp, #104]         /* fn */
    blr     x10

    /* Capture results. Loads/stores below do not disturb NZCV. */
    ldr     x11, [sp, #96]          /* reload out */
    str     x0,  [x11, #0]          /* ret */
    str     x19, [x11, #8]
    str     x20, [x11, #16]
    str     x21, [x11, #24]
    str     x22, [x11, #32]
    str     x23, [x11, #40]
    str     x24, [x11, #48]
    str     x25, [x11, #56]
    str     x26, [x11, #64]
    str     x27, [x11, #72]
    str     x28, [x11, #80]
    str     x29, [x11, #88]
    mrs     x12, nzcv
    str     x12, [x11, #96]

    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #112
    ret

#else
#  error "capture.s supports x86-64 and AArch64 only"
#endif

ASM_ENDFUNC asm_call_capture

/*
 * void asm_call_capture_fp(regs_t *out, void *fn, const long iargs[6],
 *                          const double fargs[8]);
 * Like asm_call_capture but also marshals 8 double args into the FP argument
 * registers and captures the FP return (xmm0 / d0) into out->fret (offset 72
 * on x86-64, 104 on AArch64).
 */
ASM_FUNC asm_call_capture_fp

#if defined(__x86_64__)
/* out -> %rdi, fn -> %rsi, iargs -> %rdx, fargs -> %rcx */
    pushq   %rbx
    pushq   %rbp
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    subq    $24, %rsp
    movq    %rdi, 0(%rsp)           /* stash out */
    movq    %rsi, 8(%rsp)           /* stash fn  */

    /* Float args: fargs (rcx) -> xmm0..xmm7. */
    movsd   0(%rcx),  %xmm0
    movsd   8(%rcx),  %xmm1
    movsd   16(%rcx), %xmm2
    movsd   24(%rcx), %xmm3
    movsd   32(%rcx), %xmm4
    movsd   40(%rcx), %xmm5
    movsd   48(%rcx), %xmm6
    movsd   56(%rcx), %xmm7

    /* Integer args: iargs (rdx) -> rdi,rsi,rdx,rcx,r8,r9. */
    movq    %rdx, %rax
    movq    0(%rax),  %rdi
    movq    8(%rax),  %rsi
    movq    24(%rax), %rcx
    movq    32(%rax), %r8
    movq    40(%rax), %r9
    movq    16(%rax), %rdx

    movabsq $0x1111111111111111, %rbx
    movabsq $0x2222222222222222, %rbp
    movabsq $0x3333333333333333, %r12
    movabsq $0x4444444444444444, %r13
    movabsq $0x5555555555555555, %r14
    movabsq $0x6666666666666666, %r15

    movq    8(%rsp), %r11
    movl    $8, %eax                /* variadic ABI: 8 vector registers */
    call    *%r11

    movq    0(%rsp), %r11
    movq    %rax, 0(%r11)
    movq    %rdx, 8(%r11)
    movq    %rbx, 16(%r11)
    movq    %rbp, 24(%r11)
    movq    %r12, 32(%r11)
    movq    %r13, 40(%r11)
    movq    %r14, 48(%r11)
    movq    %r15, 56(%r11)
    pushfq
    popq    %rax
    movq    %rax, 64(%r11)
    movsd   %xmm0, 72(%r11)         /* fret = xmm0 */

    addq    $24, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbp
    popq    %rbx
    ret

#elif defined(__aarch64__)
/* out -> x0, fn -> x1, iargs -> x2, fargs -> x3 */
    sub     sp, sp, #112
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    str     x0, [sp, #96]
    str     x1, [sp, #104]

    /* Float args: fargs (x3) -> d0..d7. */
    ldr     d0, [x3, #0]
    ldr     d1, [x3, #8]
    ldr     d2, [x3, #16]
    ldr     d3, [x3, #24]
    ldr     d4, [x3, #32]
    ldr     d5, [x3, #40]
    ldr     d6, [x3, #48]
    ldr     d7, [x3, #56]

    /* Integer args: iargs (x2) -> x0..x5. */
    mov     x9, x2
    ldp     x0, x1, [x9, #0]
    ldp     x2, x3, [x9, #16]
    ldp     x4, x5, [x9, #32]

    ldr     x19, =0x1111111111111111
    ldr     x20, =0x2222222222222222
    ldr     x21, =0x3333333333333333
    ldr     x22, =0x4444444444444444
    ldr     x23, =0x5555555555555555
    ldr     x24, =0x6666666666666666
    ldr     x25, =0x7777777777777777
    ldr     x26, =0x8888888888888888
    ldr     x27, =0x9999999999999999
    ldr     x28, =0xAAAAAAAAAAAAAAAA
    ldr     x29, =0xBBBBBBBBBBBBBBBB

    ldr     x10, [sp, #104]
    blr     x10

    ldr     x11, [sp, #96]
    str     x0,  [x11, #0]
    str     x19, [x11, #8]
    str     x20, [x11, #16]
    str     x21, [x11, #24]
    str     x22, [x11, #32]
    str     x23, [x11, #40]
    str     x24, [x11, #48]
    str     x25, [x11, #56]
    str     x26, [x11, #64]
    str     x27, [x11, #72]
    str     x28, [x11, #80]
    str     x29, [x11, #88]
    mrs     x12, nzcv
    str     x12, [x11, #96]
    str     d0, [x11, #104]         /* fret = d0 */

    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #112
    ret

#else
#  error "capture.s supports x86-64 and AArch64 only"
#endif

ASM_ENDFUNC asm_call_capture_fp

/*
 * void asm_call_capture_vec(regs_t *out, void *fn, const long iargs[6],
 *                           const vec128_t vargs[8]);
 * Marshals 8 full 128-bit vector args into the vector argument registers and
 * captures the entire vector register file into out->vec[] (xmm0..15 / v0..31;
 * vec[0] = vector return), alongside the GP registers and flags.
 *   x86-64 vec[] starts at offset 80; AArch64 at offset 112 (16 bytes each).
 */
ASM_FUNC asm_call_capture_vec

#if defined(__x86_64__)
/* out -> %rdi, fn -> %rsi, iargs -> %rdx, vargs -> %rcx */
    pushq   %rbx
    pushq   %rbp
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    subq    $24, %rsp
    movq    %rdi, 0(%rsp)
    movq    %rsi, 8(%rsp)

    /* Vector args: vargs (rcx) -> xmm0..xmm7 (full 128 bits). */
    movdqu  0(%rcx),   %xmm0
    movdqu  16(%rcx),  %xmm1
    movdqu  32(%rcx),  %xmm2
    movdqu  48(%rcx),  %xmm3
    movdqu  64(%rcx),  %xmm4
    movdqu  80(%rcx),  %xmm5
    movdqu  96(%rcx),  %xmm6
    movdqu  112(%rcx), %xmm7

    /* Integer args: iargs (rdx) -> rdi,rsi,rdx,rcx,r8,r9. */
    movq    %rdx, %rax
    movq    0(%rax),  %rdi
    movq    8(%rax),  %rsi
    movq    24(%rax), %rcx
    movq    32(%rax), %r8
    movq    40(%rax), %r9
    movq    16(%rax), %rdx

    movabsq $0x1111111111111111, %rbx
    movabsq $0x2222222222222222, %rbp
    movabsq $0x3333333333333333, %r12
    movabsq $0x4444444444444444, %r13
    movabsq $0x5555555555555555, %r14
    movabsq $0x6666666666666666, %r15

    movq    8(%rsp), %r11
    movl    $8, %eax
    call    *%r11

    movq    0(%rsp), %r11
    movq    %rax, 0(%r11)
    movq    %rdx, 8(%r11)
    movq    %rbx, 16(%r11)
    movq    %rbp, 24(%r11)
    movq    %r12, 32(%r11)
    movq    %r13, 40(%r11)
    movq    %r14, 48(%r11)
    movq    %r15, 56(%r11)
    pushfq
    popq    %rax
    movq    %rax, 64(%r11)
    movsd   %xmm0, 72(%r11)         /* fret */

    /* Full vector file: xmm0..15 -> vec[0..15] at offset 80. */
    movdqu  %xmm0,  80(%r11)
    movdqu  %xmm1,  96(%r11)
    movdqu  %xmm2,  112(%r11)
    movdqu  %xmm3,  128(%r11)
    movdqu  %xmm4,  144(%r11)
    movdqu  %xmm5,  160(%r11)
    movdqu  %xmm6,  176(%r11)
    movdqu  %xmm7,  192(%r11)
    movdqu  %xmm8,  208(%r11)
    movdqu  %xmm9,  224(%r11)
    movdqu  %xmm10, 240(%r11)
    movdqu  %xmm11, 256(%r11)
    movdqu  %xmm12, 272(%r11)
    movdqu  %xmm13, 288(%r11)
    movdqu  %xmm14, 304(%r11)
    movdqu  %xmm15, 320(%r11)

    addq    $24, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbp
    popq    %rbx
    ret

#elif defined(__aarch64__)
/* out -> x0, fn -> x1, iargs -> x2, vargs -> x3 */
    sub     sp, sp, #112
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    str     x0, [sp, #96]
    str     x1, [sp, #104]

    /* Vector args: vargs (x3) -> v0..v7 (full 128 bits). */
    ldr     q0, [x3, #0]
    ldr     q1, [x3, #16]
    ldr     q2, [x3, #32]
    ldr     q3, [x3, #48]
    ldr     q4, [x3, #64]
    ldr     q5, [x3, #80]
    ldr     q6, [x3, #96]
    ldr     q7, [x3, #112]

    /* Integer args: iargs (x2) -> x0..x5. */
    mov     x9, x2
    ldp     x0, x1, [x9, #0]
    ldp     x2, x3, [x9, #16]
    ldp     x4, x5, [x9, #32]

    ldr     x19, =0x1111111111111111
    ldr     x20, =0x2222222222222222
    ldr     x21, =0x3333333333333333
    ldr     x22, =0x4444444444444444
    ldr     x23, =0x5555555555555555
    ldr     x24, =0x6666666666666666
    ldr     x25, =0x7777777777777777
    ldr     x26, =0x8888888888888888
    ldr     x27, =0x9999999999999999
    ldr     x28, =0xAAAAAAAAAAAAAAAA
    ldr     x29, =0xBBBBBBBBBBBBBBBB

    ldr     x10, [sp, #104]
    blr     x10

    ldr     x11, [sp, #96]
    str     x0,  [x11, #0]
    str     x19, [x11, #8]
    str     x20, [x11, #16]
    str     x21, [x11, #24]
    str     x22, [x11, #32]
    str     x23, [x11, #40]
    str     x24, [x11, #48]
    str     x25, [x11, #56]
    str     x26, [x11, #64]
    str     x27, [x11, #72]
    str     x28, [x11, #80]
    str     x29, [x11, #88]
    mrs     x12, nzcv
    str     x12, [x11, #96]
    str     d0, [x11, #104]         /* fret */

    /* Full vector file: v0..31 -> vec[0..31] at offset 112. */
    str     q0,  [x11, #112]
    str     q1,  [x11, #128]
    str     q2,  [x11, #144]
    str     q3,  [x11, #160]
    str     q4,  [x11, #176]
    str     q5,  [x11, #192]
    str     q6,  [x11, #208]
    str     q7,  [x11, #224]
    str     q8,  [x11, #240]
    str     q9,  [x11, #256]
    str     q10, [x11, #272]
    str     q11, [x11, #288]
    str     q12, [x11, #304]
    str     q13, [x11, #320]
    str     q14, [x11, #336]
    str     q15, [x11, #352]
    str     q16, [x11, #368]
    str     q17, [x11, #384]
    str     q18, [x11, #400]
    str     q19, [x11, #416]
    str     q20, [x11, #432]
    str     q21, [x11, #448]
    str     q22, [x11, #464]
    str     q23, [x11, #480]
    str     q24, [x11, #496]
    str     q25, [x11, #512]
    str     q26, [x11, #528]
    str     q27, [x11, #544]
    str     q28, [x11, #560]
    str     q29, [x11, #576]
    str     q30, [x11, #592]
    str     q31, [x11, #608]

    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #112
    ret

#else
#  error "capture.s supports x86-64 and AArch64 only"
#endif

ASM_ENDFUNC asm_call_capture_vec

/*
 * void asm_call_capture_args(regs_t *out, void *fn, const long *args,
 *                            int nargs);
 * Passes `nargs` integer args: the first 6 (x86-64) / 8 (AArch64) in registers,
 * the remainder on the stack per the ABI. Uses the frame-pointer register to
 * unwind the variable-size outgoing-argument area, so that register (rbp / x29)
 * is reported as preserved but not independently checked.
 */
ASM_FUNC asm_call_capture_args

#if defined(__x86_64__)
/* out -> %rdi, fn -> %rsi, args -> %rdx, nargs -> %rcx */
    pushq   %rbp
    movq    %rsp, %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    subq    $32, %rsp
    movq    %rdi, -48(%rbp)        /* out   */
    movq    %rsi, -56(%rbp)        /* fn    */
    movq    %rdx, -64(%rbp)        /* args  */
    movq    %rcx, -72(%rbp)        /* nargs */

    /* n_stack = max(0, nargs - 6) -> r10 */
    movq    %rcx, %r10
    subq    $6, %r10
    jg      10f
    xorq    %r10, %r10
10:
    /* 16-align rsp, then reserve round_up(n_stack*8, 16) bytes. */
    andq    $-16, %rsp
    movq    %r10, %rax
    shlq    $3, %rax
    addq    $15, %rax
    andq    $-16, %rax
    subq    %rax, %rsp

    /* Copy overflow args: [rsp + i*8] = args[6 + i]. */
    movq    -64(%rbp), %r8
    xorq    %rcx, %rcx
11:
    cmpq    %r10, %rcx
    jge     12f
    movq    48(%r8,%rcx,8), %rax
    movq    %rax, (%rsp,%rcx,8)
    incq    %rcx
    jmp     11b
12:
    /* Load the register args (only as many as nargs). */
    movq    -64(%rbp), %r11        /* args  */
    movq    -72(%rbp), %r10        /* nargs */
    cmpq    $1, %r10
    jl      13f
    movq    0(%r11), %rdi
    cmpq    $2, %r10
    jl      13f
    movq    8(%r11), %rsi
    cmpq    $3, %r10
    jl      13f
    movq    16(%r11), %rdx
    cmpq    $4, %r10
    jl      13f
    movq    24(%r11), %rcx
    cmpq    $5, %r10
    jl      13f
    movq    32(%r11), %r8
    cmpq    $6, %r10
    jl      13f
    movq    40(%r11), %r9
13:
    /* Seed callee-saved sentinels (rbp is the frame pointer). */
    movabsq $0x1111111111111111, %rbx
    movabsq $0x3333333333333333, %r12
    movabsq $0x4444444444444444, %r13
    movabsq $0x5555555555555555, %r14
    movabsq $0x6666666666666666, %r15

    movq    -56(%rbp), %r11        /* fn */
    xorl    %eax, %eax
    call    *%r11

    movq    -48(%rbp), %r11        /* out */
    movq    %rax, 0(%r11)
    movq    %rdx, 8(%r11)
    movq    %rbx, 16(%r11)
    movabsq $0x2222222222222222, %rax  /* rbp: reported preserved, not checked */
    movq    %rax, 24(%r11)
    movq    %r12, 32(%r11)
    movq    %r13, 40(%r11)
    movq    %r14, 48(%r11)
    movq    %r15, 56(%r11)
    pushfq
    popq    %rax
    movq    %rax, 64(%r11)

    leaq    -40(%rbp), %rsp        /* drop the variable area + locals */
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret

#elif defined(__aarch64__)
/* out -> x0, fn -> x1, args -> x2, nargs -> x3 */
    stp     x29, x30, [sp, #-160]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]
    str     x0, [sp, #96]          /* out   */
    str     x1, [sp, #104]         /* fn    */
    str     x2, [sp, #112]         /* args  */
    str     x3, [sp, #120]         /* nargs */

    /* n_stack = max(0, nargs - 8) -> x9 */
    subs    x9, x3, #8
    b.gt    20f
    mov     x9, #0
20:
    /* reserve round_up(n_stack*8, 16) bytes (sp stays 16-aligned). */
    lsl     x10, x9, #3
    add     x10, x10, #15
    and     x10, x10, #-16
    sub     sp, sp, x10

    /* Copy overflow args: [sp + i*8] = args[8 + i]. */
    ldr     x11, [x29, #112]
    mov     x12, #0
21:
    cmp     x12, x9
    b.ge    22f
    add     x13, x12, #8
    ldr     x14, [x11, x13, lsl #3]
    str     x14, [sp, x12, lsl #3]
    add     x12, x12, #1
    b       21b
22:
    /* Load the register args (only as many as nargs). */
    ldr     x11, [x29, #112]
    ldr     x10, [x29, #120]
    cmp     x10, #1
    b.lt    23f
    ldr     x0, [x11, #0]
    cmp     x10, #2
    b.lt    23f
    ldr     x1, [x11, #8]
    cmp     x10, #3
    b.lt    23f
    ldr     x2, [x11, #16]
    cmp     x10, #4
    b.lt    23f
    ldr     x3, [x11, #24]
    cmp     x10, #5
    b.lt    23f
    ldr     x4, [x11, #32]
    cmp     x10, #6
    b.lt    23f
    ldr     x5, [x11, #40]
    cmp     x10, #7
    b.lt    23f
    ldr     x6, [x11, #48]
    cmp     x10, #8
    b.lt    23f
    ldr     x7, [x11, #56]
23:
    /* Seed callee-saved sentinels (x29 is the frame pointer). */
    ldr     x19, =0x1111111111111111
    ldr     x20, =0x2222222222222222
    ldr     x21, =0x3333333333333333
    ldr     x22, =0x4444444444444444
    ldr     x23, =0x5555555555555555
    ldr     x24, =0x6666666666666666
    ldr     x25, =0x7777777777777777
    ldr     x26, =0x8888888888888888
    ldr     x27, =0x9999999999999999
    ldr     x28, =0xAAAAAAAAAAAAAAAA

    ldr     x10, [x29, #104]       /* fn */
    blr     x10

    ldr     x11, [x29, #96]        /* out */
    str     x0,  [x11, #0]
    str     x19, [x11, #8]
    str     x20, [x11, #16]
    str     x21, [x11, #24]
    str     x22, [x11, #32]
    str     x23, [x11, #40]
    str     x24, [x11, #48]
    str     x25, [x11, #56]
    str     x26, [x11, #64]
    str     x27, [x11, #72]
    str     x28, [x11, #80]
    ldr     x12, =0xBBBBBBBBBBBBBBBB  /* x29: reported preserved, not checked */
    str     x12, [x11, #88]
    mrs     x12, nzcv
    str     x12, [x11, #96]

    mov     sp, x29                /* drop the variable area */
    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldp     x23, x24, [sp, #48]
    ldp     x25, x26, [sp, #64]
    ldp     x27, x28, [sp, #80]
    ldp     x29, x30, [sp], #160
    ret

#else
#  error "capture.s supports x86-64 and AArch64 only"
#endif

ASM_ENDFUNC asm_call_capture_args
