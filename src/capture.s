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

#elif defined(__riscv) && __riscv_xlen == 64
/* --- RISC-V rv64 (LP64D psABI) --------------------------------------------
 * out -> a0, fn -> a1, args -> a2
 * args[0..5] -> a0..a5 (6 slots, the portable API width; mirrors AArch64's
 * x0..x5). regs_t offsets: ret 0, a1 8, s0 16, s1 24, ... s11 104, flags 112.
 * t-registers and a-registers are caller-saved — never live across the jalr;
 * out/fn are reloaded from the frame. No flags to preserve, so the captures are
 * plain stores.
 */
    addi    sp, sp, -128           /* 16-aligned frame */
    sd      ra, 0(sp)
    sd      s0, 8(sp)
    sd      s1, 16(sp)
    sd      s2, 24(sp)
    sd      s3, 32(sp)
    sd      s4, 40(sp)
    sd      s5, 48(sp)
    sd      s6, 56(sp)
    sd      s7, 64(sp)
    sd      s8, 72(sp)
    sd      s9, 80(sp)
    sd      s10, 88(sp)
    sd      s11, 96(sp)
    sd      a0, 104(sp)            /* stash out across the call */
    sd      a1, 112(sp)            /* stash fn  across the call */

    /* Marshal args[0..5] into a0..a5. */
    mv      t1, a2
    ld      a0, 0(t1)
    ld      a1, 8(t1)
    ld      a2, 16(t1)
    ld      a3, 24(t1)
    ld      a4, 32(t1)
    ld      a5, 40(t1)

    /* Seed callee-saved registers (s0-s11) with sentinels. */
    li      s0, 0x1111111111111111
    li      s1, 0x2222222222222222
    li      s2, 0x3333333333333333
    li      s3, 0x4444444444444444
    li      s4, 0x5555555555555555
    li      s5, 0x6666666666666666
    li      s6, 0x7777777777777777
    li      s7, 0x8888888888888888
    li      s8, 0x9999999999999999
    li      s9, 0xAAAAAAAAAAAAAAAA
    li      s10, 0xBBBBBBBBBBBBBBBB
    li      s11, 0xCCCCCCCCCCCCCCCC

    ld      t0, 112(sp)            /* fn */
    jalr    ra, 0(t0)

    /* Capture results (plain stores; rv64 has no flags to disturb). */
    ld      t1, 104(sp)            /* reload out */
    sd      a0, 0(t1)              /* ret = a0 */
    sd      a1, 8(t1)              /* a1 = second return register */
    sd      s0, 16(t1)
    sd      s1, 24(t1)
    sd      s2, 32(t1)
    sd      s3, 40(t1)
    sd      s4, 48(t1)
    sd      s5, 56(t1)
    sd      s6, 64(t1)
    sd      s7, 72(t1)
    sd      s8, 80(t1)
    sd      s9, 88(t1)
    sd      s10, 96(t1)
    sd      s11, 104(t1)
    sd      zero, 112(t1)          /* flags == 0: RISC-V has none */

    ld      ra, 0(sp)
    ld      s0, 8(sp)
    ld      s1, 16(sp)
    ld      s2, 24(sp)
    ld      s3, 32(sp)
    ld      s4, 40(sp)
    ld      s5, 48(sp)
    ld      s6, 56(sp)
    ld      s7, 64(sp)
    ld      s8, 72(sp)
    ld      s9, 80(sp)
    ld      s10, 88(sp)
    ld      s11, 96(sp)
    addi    sp, sp, 128
    ret

#else
#  error "capture.s supports x86-64, AArch64 and RV64 only"
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

#elif defined(__riscv) && __riscv_xlen == 64
/* out -> a0, fn -> a1, iargs -> a2, fargs -> a3
 * Marshals 8 doubles into fa0..fa7 and captures the FP return fa0 into
 * out->fret (offset 120). T3: rv64 has no _vec path, so the FP callee-saved
 * check (ASSERT_ABI_PRESERVED_VEC) rides HERE — this seeds fs0-fs11 (f8/f9,
 * f18-f27) with their f-register numbers and captures each into
 * out->vec[reg#].u64[0], so asmtest_check_abi_vec verifies vec[i].u64[0] == i
 * for i in {8,9,18..27}. 224-byte frame: 128 as before + 96 for fs0-fs11. */
    addi    sp, sp, -224
    sd      ra, 0(sp)
    sd      s0, 8(sp)
    sd      s1, 16(sp)
    sd      s2, 24(sp)
    sd      s3, 32(sp)
    sd      s4, 40(sp)
    sd      s5, 48(sp)
    sd      s6, 56(sp)
    sd      s7, 64(sp)
    sd      s8, 72(sp)
    sd      s9, 80(sp)
    sd      s10, 88(sp)
    sd      s11, 96(sp)
    sd      a0, 104(sp)            /* stash out */
    sd      a1, 112(sp)            /* stash fn  */
    /* Save the caller's fs0-fs11 (callee-saved FP), which we clobber to seed. */
    fsd     fs0, 120(sp)
    fsd     fs1, 128(sp)
    fsd     fs2, 136(sp)
    fsd     fs3, 144(sp)
    fsd     fs4, 152(sp)
    fsd     fs5, 160(sp)
    fsd     fs6, 168(sp)
    fsd     fs7, 176(sp)
    fsd     fs8, 184(sp)
    fsd     fs9, 192(sp)
    fsd     fs10, 200(sp)
    fsd     fs11, 208(sp)

    /* Float args: fargs (a3) -> fa0..fa7 (before a3 is reused for marshalling). */
    fld     fa0, 0(a3)
    fld     fa1, 8(a3)
    fld     fa2, 16(a3)
    fld     fa3, 24(a3)
    fld     fa4, 32(a3)
    fld     fa5, 40(a3)
    fld     fa6, 48(a3)
    fld     fa7, 56(a3)

    /* Integer args: iargs (a2) -> a0..a5. */
    mv      t1, a2
    ld      a0, 0(t1)
    ld      a1, 8(t1)
    ld      a2, 16(t1)
    ld      a3, 24(t1)
    ld      a4, 32(t1)
    ld      a5, 40(t1)

    li      s0, 0x1111111111111111
    li      s1, 0x2222222222222222
    li      s2, 0x3333333333333333
    li      s3, 0x4444444444444444
    li      s4, 0x5555555555555555
    li      s5, 0x6666666666666666
    li      s6, 0x7777777777777777
    li      s7, 0x8888888888888888
    li      s8, 0x9999999999999999
    li      s9, 0xAAAAAAAAAAAAAAAA
    li      s10, 0xBBBBBBBBBBBBBBBB
    li      s11, 0xCCCCCCCCCCCCCCCC

    /* Seed fs0-fs11 with their f-register numbers (fs0=f8, fs1=f9, fs2-fs11 =
     * f18-f27) as integer bit patterns, so a restored register reads back == i. */
    li      t0, 8
    fmv.d.x fs0, t0
    li      t0, 9
    fmv.d.x fs1, t0
    li      t0, 18
    fmv.d.x fs2, t0
    li      t0, 19
    fmv.d.x fs3, t0
    li      t0, 20
    fmv.d.x fs4, t0
    li      t0, 21
    fmv.d.x fs5, t0
    li      t0, 22
    fmv.d.x fs6, t0
    li      t0, 23
    fmv.d.x fs7, t0
    li      t0, 24
    fmv.d.x fs8, t0
    li      t0, 25
    fmv.d.x fs9, t0
    li      t0, 26
    fmv.d.x fs10, t0
    li      t0, 27
    fmv.d.x fs11, t0

    ld      t0, 112(sp)            /* fn */
    jalr    ra, 0(t0)

    ld      t1, 104(sp)            /* out */
    sd      a0, 0(t1)
    sd      a1, 8(t1)
    sd      s0, 16(t1)
    sd      s1, 24(t1)
    sd      s2, 32(t1)
    sd      s3, 40(t1)
    sd      s4, 48(t1)
    sd      s5, 56(t1)
    sd      s6, 64(t1)
    sd      s7, 72(t1)
    sd      s8, 80(t1)
    sd      s9, 88(t1)
    sd      s10, 96(t1)
    sd      s11, 104(t1)
    sd      zero, 112(t1)          /* flags == 0 */
    fsd     fa0, 120(t1)           /* fret = fa0 */
    /* Capture fs0-fs11 into out->vec[reg#].u64[0] (vec@128, 16B stride); zero the
     * high lane. fs0->vec[8], fs1->vec[9], fs2..fs11->vec[18..27]. */
    fsd     fs0, 256(t1)           /* vec[8].u64[0]  */
    sd      zero, 264(t1)
    fsd     fs1, 272(t1)           /* vec[9].u64[0]  */
    sd      zero, 280(t1)
    fsd     fs2, 416(t1)           /* vec[18].u64[0] */
    sd      zero, 424(t1)
    fsd     fs3, 432(t1)           /* vec[19] */
    sd      zero, 440(t1)
    fsd     fs4, 448(t1)           /* vec[20] */
    sd      zero, 456(t1)
    fsd     fs5, 464(t1)           /* vec[21] */
    sd      zero, 472(t1)
    fsd     fs6, 480(t1)           /* vec[22] */
    sd      zero, 488(t1)
    fsd     fs7, 496(t1)           /* vec[23] */
    sd      zero, 504(t1)
    fsd     fs8, 512(t1)           /* vec[24] */
    sd      zero, 520(t1)
    fsd     fs9, 528(t1)           /* vec[25] */
    sd      zero, 536(t1)
    fsd     fs10, 544(t1)          /* vec[26] */
    sd      zero, 552(t1)
    fsd     fs11, 560(t1)          /* vec[27] */
    sd      zero, 568(t1)

    /* Restore the caller's fs0-fs11. */
    fld     fs0, 120(sp)
    fld     fs1, 128(sp)
    fld     fs2, 136(sp)
    fld     fs3, 144(sp)
    fld     fs4, 152(sp)
    fld     fs5, 160(sp)
    fld     fs6, 168(sp)
    fld     fs7, 176(sp)
    fld     fs8, 184(sp)
    fld     fs9, 192(sp)
    fld     fs10, 200(sp)
    fld     fs11, 208(sp)

    ld      ra, 0(sp)
    ld      s0, 8(sp)
    ld      s1, 16(sp)
    ld      s2, 24(sp)
    ld      s3, 32(sp)
    ld      s4, 40(sp)
    ld      s5, 48(sp)
    ld      s6, 56(sp)
    ld      s7, 64(sp)
    ld      s8, 72(sp)
    ld      s9, 80(sp)
    ld      s10, 88(sp)
    ld      s11, 96(sp)
    addi    sp, sp, 224
    ret

#else
#  error "capture.s supports x86-64, AArch64 and RV64 only"
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
    sub     sp, sp, #176
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    str     x0, [sp, #96]
    str     x1, [sp, #104]

    /* Preserve the caller's callee-saved FP regs (low 64 bits of d8-d15 per
     * AAPCS64 6.1.2); they are seeded below and restored after capture so a
     * clobber by fn cannot leak into the C caller's live doubles. */
    stp     d8, d9,   [sp, #112]
    stp     d10, d11, [sp, #128]
    stp     d12, d13, [sp, #144]
    stp     d14, d15, [sp, #160]

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

    /* Seed d8-d15 low 64 bits with sentinels 8..15 so ASSERT_ABI_PRESERVED_VEC
     * detects a callee that fails to preserve them (mirrors Win64 xmm6-15). */
    mov     x9, #8
    fmov    d8, x9
    mov     x9, #9
    fmov    d9, x9
    mov     x9, #10
    fmov    d10, x9
    mov     x9, #11
    fmov    d11, x9
    mov     x9, #12
    fmov    d12, x9
    mov     x9, #13
    fmov    d13, x9
    mov     x9, #14
    fmov    d14, x9
    mov     x9, #15
    fmov    d15, x9

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

    /* Restore the caller's d8-d15 (captured above into vec[8..15]). */
    ldp     d8, d9,   [sp, #112]
    ldp     d10, d11, [sp, #128]
    ldp     d12, d13, [sp, #144]
    ldp     d14, d15, [sp, #160]

    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #176
    ret

#elif defined(__riscv) && __riscv_xlen == 64
/* rv64gc has no vector registers; a stub so the symbol resolves. Never called:
 * asmtest_cpu_has_vec128() is false on rv64, so the ASM_VCALL macros self-skip. */
    ret
#else
#  error "capture.s supports x86-64, AArch64 and RV64 only"
#endif

ASM_ENDFUNC asm_call_capture_vec

/*
 * void asm_call_capture_fp_n(regs_t *out, void *fn, const long iargs[6],
 *                            const double *fargs, int nfargs);
 * Like asm_call_capture_fp but with arbitrary FP arity: the first 8 doubles go
 * in the FP argument registers, the rest spill onto the stack. Uses the
 * frame-pointer register to unwind the variable-size outgoing-argument area, so
 * that register (rbp / x29) is reported preserved but not independently checked.
 */
ASM_FUNC asm_call_capture_fp_n

#if defined(__x86_64__)
/* out -> %rdi, fn -> %rsi, iargs -> %rdx, fargs -> %rcx, nfargs -> %r8 */
    pushq   %rbp
    movq    %rsp, %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    subq    $48, %rsp
    movq    %rdi, -48(%rbp)        /* out    */
    movq    %rsi, -56(%rbp)        /* fn     */
    movq    %rdx, -64(%rbp)        /* iargs  */
    movq    %rcx, -72(%rbp)        /* fargs  */
    movq    %r8,  -80(%rbp)        /* nfargs */

    /* n_stack = max(0, nfargs - 8) -> r10 */
    movq    %r8, %r10
    subq    $8, %r10
    jg      60f
    xorq    %r10, %r10
60:
    /* 16-align rsp, then reserve round_up(n_stack*8, 16) bytes. */
    andq    $-16, %rsp
    movq    %r10, %rax
    shlq    $3, %rax
    addq    $15, %rax
    andq    $-16, %rax
    subq    %rax, %rsp

    /* Copy overflow doubles: [rsp + i*8] = fargs[8 + i]. */
    movq    -72(%rbp), %r8
    xorq    %rcx, %rcx
61:
    cmpq    %r10, %rcx
    jge     62f
    movq    64(%r8,%rcx,8), %rax
    movq    %rax, (%rsp,%rcx,8)
    incq    %rcx
    jmp     61b
62:
    /* Load FP register args xmm0..7 (only as many as nfargs). */
    movq    -72(%rbp), %r11        /* fargs  */
    movq    -80(%rbp), %r10        /* nfargs */
    cmpq    $1, %r10
    jl      63f
    movsd   0(%r11),  %xmm0
    cmpq    $2, %r10
    jl      63f
    movsd   8(%r11),  %xmm1
    cmpq    $3, %r10
    jl      63f
    movsd   16(%r11), %xmm2
    cmpq    $4, %r10
    jl      63f
    movsd   24(%r11), %xmm3
    cmpq    $5, %r10
    jl      63f
    movsd   32(%r11), %xmm4
    cmpq    $6, %r10
    jl      63f
    movsd   40(%r11), %xmm5
    cmpq    $7, %r10
    jl      63f
    movsd   48(%r11), %xmm6
    cmpq    $8, %r10
    jl      63f
    movsd   56(%r11), %xmm7
63:
    /* Integer args: iargs -> rdi,rsi,rdx,rcx,r8,r9 (6 register slots). */
    movq    -64(%rbp), %rax
    movq    0(%rax),  %rdi
    movq    8(%rax),  %rsi
    movq    24(%rax), %rcx
    movq    32(%rax), %r8
    movq    40(%rax), %r9
    movq    16(%rax), %rdx

    movabsq $0x1111111111111111, %rbx
    movabsq $0x3333333333333333, %r12
    movabsq $0x4444444444444444, %r13
    movabsq $0x5555555555555555, %r14
    movabsq $0x6666666666666666, %r15

    /* al = min(nfargs, 8): variadic-ABI vector-register count. */
    movq    -80(%rbp), %rax
    cmpq    $8, %rax
    jle     64f
    movl    $8, %eax
64:
    movq    -56(%rbp), %r11        /* fn */
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
    movsd   %xmm0, 72(%r11)        /* fret = xmm0 */

    leaq    -40(%rbp), %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret

#elif defined(__aarch64__)
/* out -> x0, fn -> x1, iargs -> x2, fargs -> x3, nfargs -> x4 */
    stp     x29, x30, [sp, #-160]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]
    str     x0, [sp, #96]          /* out    */
    str     x1, [sp, #104]         /* fn     */
    str     x2, [sp, #112]         /* iargs  */
    str     x3, [sp, #120]         /* fargs  */
    str     x4, [sp, #128]         /* nfargs */

    /* n_stack = max(0, nfargs - 8) -> x9 */
    subs    x9, x4, #8
    b.gt    60f
    mov     x9, #0
60:
    lsl     x10, x9, #3
    add     x10, x10, #15
    and     x10, x10, #-16
    sub     sp, sp, x10

    /* Copy overflow doubles: [sp + i*8] = fargs[8 + i]. */
    ldr     x11, [x29, #120]
    mov     x12, #0
61:
    cmp     x12, x9
    b.ge    62f
    add     x13, x12, #8
    ldr     x14, [x11, x13, lsl #3]
    str     x14, [sp, x12, lsl #3]
    add     x12, x12, #1
    b       61b
62:
    /* Load FP register args d0..7 (only as many as nfargs). */
    ldr     x11, [x29, #120]       /* fargs  */
    ldr     x10, [x29, #128]       /* nfargs */
    cmp     x10, #1
    b.lt    63f
    ldr     d0, [x11, #0]
    cmp     x10, #2
    b.lt    63f
    ldr     d1, [x11, #8]
    cmp     x10, #3
    b.lt    63f
    ldr     d2, [x11, #16]
    cmp     x10, #4
    b.lt    63f
    ldr     d3, [x11, #24]
    cmp     x10, #5
    b.lt    63f
    ldr     d4, [x11, #32]
    cmp     x10, #6
    b.lt    63f
    ldr     d5, [x11, #40]
    cmp     x10, #7
    b.lt    63f
    ldr     d6, [x11, #48]
    cmp     x10, #8
    b.lt    63f
    ldr     d7, [x11, #56]
63:
    /* Integer args: iargs -> x0..x5. */
    ldr     x9, [x29, #112]
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
    str     d0, [x11, #104]        /* fret = d0 */

    mov     sp, x29
    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldp     x23, x24, [sp, #48]
    ldp     x25, x26, [sp, #64]
    ldp     x27, x28, [sp, #80]
    ldp     x29, x30, [sp], #160
    ret

#elif defined(__riscv) && __riscv_xlen == 64
/* out -> a0, fn -> a1, iargs -> a2, fargs -> a3, nfargs -> a4
 * First 8 doubles -> fa0..fa7; doubles 9,10 -> a6,a7 as raw bit patterns (the
 * psABI "FP args after the FP registers are exhausted use the integer
 * convention" rule — a0..a5 already hold iargs, so a6,a7 are the next free
 * integer registers); doubles 11+ spill onto the stack at 0(sp),8(sp),...
 * s0 is the frame pointer (reported preserved, not independently checked).
 * T3: also seeds/captures fs0-fs11 (like _fp), so ASSERT_ABI_PRESERVED_VEC is
 * valid after an _fp_n capture too. 256-byte frame: 160 as before + 96 for the
 * fs save area at 144(s0)..232(s0). */
    addi    sp, sp, -256
    sd      ra, 0(sp)
    sd      s0, 8(sp)              /* save caller's s0 (frame pointer) */
    mv      s0, sp                 /* s0 = frame pointer */
    sd      s1, 16(sp)
    sd      s2, 24(sp)
    sd      s3, 32(sp)
    sd      s4, 40(sp)
    sd      s5, 48(sp)
    sd      s6, 56(sp)
    sd      s7, 64(sp)
    sd      s8, 72(sp)
    sd      s9, 80(sp)
    sd      s10, 88(sp)
    sd      s11, 96(sp)
    sd      a0, 104(sp)            /* out    */
    sd      a1, 112(sp)            /* fn     */
    sd      a2, 120(sp)            /* iargs  */
    sd      a3, 128(sp)            /* fargs  */
    sd      a4, 136(sp)            /* nfargs */
    /* Save caller's fs0-fs11 (relative to the frame pointer). */
    fsd     fs0, 144(sp)
    fsd     fs1, 152(sp)
    fsd     fs2, 160(sp)
    fsd     fs3, 168(sp)
    fsd     fs4, 176(sp)
    fsd     fs5, 184(sp)
    fsd     fs6, 192(sp)
    fsd     fs7, 200(sp)
    fsd     fs8, 208(sp)
    fsd     fs9, 216(sp)
    fsd     fs10, 224(sp)
    fsd     fs11, 232(sp)

    /* n_stack = max(0, nfargs - 10) -> t1 */
    ld      t0, 136(s0)
    li      t1, 0
    li      t2, 10
    ble     t0, t2, 1f
    sub     t1, t0, t2
1:
    /* reserve round_up(n_stack*8, 16) bytes (sp stays 16-aligned). */
    slli    t2, t1, 3
    addi    t2, t2, 15
    andi    t2, t2, -16
    sub     sp, sp, t2

    /* Copy overflow doubles: [sp + i*8] = fargs[10 + i] (raw bit patterns). */
    ld      t2, 128(s0)
    li      t3, 0
2:
    bge     t3, t1, 3f
    addi    t4, t3, 10
    slli    t5, t4, 3
    add     t5, t2, t5
    ld      t6, 0(t5)
    slli    t4, t3, 3
    add     t4, sp, t4
    sd      t6, 0(t4)
    addi    t3, t3, 1
    j       2b
3:
    /* Load FP register args fa0..fa7 (only as many as nfargs). */
    ld      t2, 128(s0)            /* fargs  */
    ld      t0, 136(s0)            /* nfargs */
    li      t3, 1
    blt     t0, t3, 4f
    fld     fa0, 0(t2)
    li      t3, 2
    blt     t0, t3, 4f
    fld     fa1, 8(t2)
    li      t3, 3
    blt     t0, t3, 4f
    fld     fa2, 16(t2)
    li      t3, 4
    blt     t0, t3, 4f
    fld     fa3, 24(t2)
    li      t3, 5
    blt     t0, t3, 4f
    fld     fa4, 32(t2)
    li      t3, 6
    blt     t0, t3, 4f
    fld     fa5, 40(t2)
    li      t3, 7
    blt     t0, t3, 4f
    fld     fa6, 48(t2)
    li      t3, 8
    blt     t0, t3, 4f
    fld     fa7, 56(t2)
4:
    /* Doubles 9,10 -> a6,a7 as raw bit patterns (integer convention). */
    li      t3, 9
    blt     t0, t3, 5f
    ld      a6, 64(t2)             /* fargs[8] */
    li      t3, 10
    blt     t0, t3, 5f
    ld      a7, 72(t2)             /* fargs[9] */
5:
    /* Integer args: iargs -> a0..a5. */
    ld      t2, 120(s0)
    ld      a0, 0(t2)
    ld      a1, 8(t2)
    ld      a2, 16(t2)
    ld      a3, 24(t2)
    ld      a4, 32(t2)
    ld      a5, 40(t2)

    /* Seed callee-saved sentinels (s0 is the frame pointer, seeded via out). */
    li      s1, 0x2222222222222222
    li      s2, 0x3333333333333333
    li      s3, 0x4444444444444444
    li      s4, 0x5555555555555555
    li      s5, 0x6666666666666666
    li      s6, 0x7777777777777777
    li      s7, 0x8888888888888888
    li      s8, 0x9999999999999999
    li      s9, 0xAAAAAAAAAAAAAAAA
    li      s10, 0xBBBBBBBBBBBBBBBB
    li      s11, 0xCCCCCCCCCCCCCCCC

    /* Seed fs0-fs11 with their f-register numbers (fs0=f8, fs1=f9, fs2-fs11 =
     * f18-f27), so a restored register reads back == i (see _fp). */
    li      t0, 8
    fmv.d.x fs0, t0
    li      t0, 9
    fmv.d.x fs1, t0
    li      t0, 18
    fmv.d.x fs2, t0
    li      t0, 19
    fmv.d.x fs3, t0
    li      t0, 20
    fmv.d.x fs4, t0
    li      t0, 21
    fmv.d.x fs5, t0
    li      t0, 22
    fmv.d.x fs6, t0
    li      t0, 23
    fmv.d.x fs7, t0
    li      t0, 24
    fmv.d.x fs8, t0
    li      t0, 25
    fmv.d.x fs9, t0
    li      t0, 26
    fmv.d.x fs10, t0
    li      t0, 27
    fmv.d.x fs11, t0

    ld      t0, 112(s0)            /* fn */
    jalr    ra, 0(t0)

    ld      t1, 104(s0)            /* out */
    sd      a0, 0(t1)
    sd      a1, 8(t1)
    li      t2, 0x1111111111111111 /* s0: reported preserved, not checked */
    sd      t2, 16(t1)
    sd      s1, 24(t1)
    sd      s2, 32(t1)
    sd      s3, 40(t1)
    sd      s4, 48(t1)
    sd      s5, 56(t1)
    sd      s6, 64(t1)
    sd      s7, 72(t1)
    sd      s8, 80(t1)
    sd      s9, 88(t1)
    sd      s10, 96(t1)
    sd      s11, 104(t1)
    sd      zero, 112(t1)          /* flags == 0 */
    fsd     fa0, 120(t1)           /* fret = fa0 */
    /* Capture fs0-fs11 into out->vec[8],[9],[18..27]; zero the high lane. */
    fsd     fs0, 256(t1)
    sd      zero, 264(t1)
    fsd     fs1, 272(t1)
    sd      zero, 280(t1)
    fsd     fs2, 416(t1)
    sd      zero, 424(t1)
    fsd     fs3, 432(t1)
    sd      zero, 440(t1)
    fsd     fs4, 448(t1)
    sd      zero, 456(t1)
    fsd     fs5, 464(t1)
    sd      zero, 472(t1)
    fsd     fs6, 480(t1)
    sd      zero, 488(t1)
    fsd     fs7, 496(t1)
    sd      zero, 504(t1)
    fsd     fs8, 512(t1)
    sd      zero, 520(t1)
    fsd     fs9, 528(t1)
    sd      zero, 536(t1)
    fsd     fs10, 544(t1)
    sd      zero, 552(t1)
    fsd     fs11, 560(t1)
    sd      zero, 568(t1)
    /* Restore caller's fs0-fs11 (from the frame pointer). */
    fld     fs0, 144(s0)
    fld     fs1, 152(s0)
    fld     fs2, 160(s0)
    fld     fs3, 168(s0)
    fld     fs4, 176(s0)
    fld     fs5, 184(s0)
    fld     fs6, 192(s0)
    fld     fs7, 200(s0)
    fld     fs8, 208(s0)
    fld     fs9, 216(s0)
    fld     fs10, 224(s0)
    fld     fs11, 232(s0)

    mv      sp, s0                 /* drop the variable area */
    ld      ra, 0(sp)
    ld      s1, 16(sp)
    ld      s2, 24(sp)
    ld      s3, 32(sp)
    ld      s4, 40(sp)
    ld      s5, 48(sp)
    ld      s6, 56(sp)
    ld      s7, 64(sp)
    ld      s8, 72(sp)
    ld      s9, 80(sp)
    ld      s10, 88(sp)
    ld      s11, 96(sp)
    ld      s0, 8(sp)              /* restore caller's s0 last */
    addi    sp, sp, 256
    ret

#else
#  error "capture.s supports x86-64, AArch64 and RV64 only"
#endif

ASM_ENDFUNC asm_call_capture_fp_n

/*
 * void asm_call_capture_vec_n(regs_t *out, void *fn, const long iargs[6],
 *                             const vec128_t *vargs, int nvargs);
 * Like asm_call_capture_vec but with arbitrary vector arity: the first 8
 * 128-bit vectors go in the vector argument registers, the rest spill onto the
 * stack (16-byte slots). Captures the whole vector file. Uses the frame-pointer
 * register (rbp / x29), reported preserved but not independently checked.
 */
ASM_FUNC asm_call_capture_vec_n

#if defined(__x86_64__)
/* out -> %rdi, fn -> %rsi, iargs -> %rdx, vargs -> %rcx, nvargs -> %r8 */
    pushq   %rbp
    movq    %rsp, %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    subq    $48, %rsp
    movq    %rdi, -48(%rbp)        /* out    */
    movq    %rsi, -56(%rbp)        /* fn     */
    movq    %rdx, -64(%rbp)        /* iargs  */
    movq    %rcx, -72(%rbp)        /* vargs  */
    movq    %r8,  -80(%rbp)        /* nvargs */

    /* n_stack = max(0, nvargs - 8) -> r10 (each slot is 16 bytes). */
    movq    %r8, %r10
    subq    $8, %r10
    jg      70f
    xorq    %r10, %r10
70:
    andq    $-16, %rsp
    movq    %r10, %rax
    shlq    $4, %rax               /* n_stack * 16 (already 16-aligned) */
    subq    %rax, %rsp

    /* Copy overflow vectors as 8-byte words: words = n_stack*2,
       [rsp + j*8] = vargs[128 + j*8]. */
    movq    %r10, %r9
    shlq    $1, %r9
    movq    -72(%rbp), %r8
    xorq    %rcx, %rcx
71:
    cmpq    %r9, %rcx
    jge     72f
    movq    128(%r8,%rcx,8), %rax
    movq    %rax, (%rsp,%rcx,8)
    incq    %rcx
    jmp     71b
72:
    /* Load vector register args xmm0..7 (only as many as nvargs). */
    movq    -72(%rbp), %r11        /* vargs  */
    movq    -80(%rbp), %r10        /* nvargs */
    cmpq    $1, %r10
    jl      73f
    movdqu  0(%r11),   %xmm0
    cmpq    $2, %r10
    jl      73f
    movdqu  16(%r11),  %xmm1
    cmpq    $3, %r10
    jl      73f
    movdqu  32(%r11),  %xmm2
    cmpq    $4, %r10
    jl      73f
    movdqu  48(%r11),  %xmm3
    cmpq    $5, %r10
    jl      73f
    movdqu  64(%r11),  %xmm4
    cmpq    $6, %r10
    jl      73f
    movdqu  80(%r11),  %xmm5
    cmpq    $7, %r10
    jl      73f
    movdqu  96(%r11),  %xmm6
    cmpq    $8, %r10
    jl      73f
    movdqu  112(%r11), %xmm7
73:
    /* Integer args: iargs -> rdi,rsi,rdx,rcx,r8,r9 (6 register slots). */
    movq    -64(%rbp), %rax
    movq    0(%rax),  %rdi
    movq    8(%rax),  %rsi
    movq    24(%rax), %rcx
    movq    32(%rax), %r8
    movq    40(%rax), %r9
    movq    16(%rax), %rdx

    movabsq $0x1111111111111111, %rbx
    movabsq $0x3333333333333333, %r12
    movabsq $0x4444444444444444, %r13
    movabsq $0x5555555555555555, %r14
    movabsq $0x6666666666666666, %r15

    /* al = min(nvargs, 8): variadic-ABI vector-register count. */
    movq    -80(%rbp), %rax
    cmpq    $8, %rax
    jle     74f
    movl    $8, %eax
74:
    movq    -56(%rbp), %r11        /* fn */
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
    movsd   %xmm0, 72(%r11)        /* fret */

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

    leaq    -40(%rbp), %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret

#elif defined(__aarch64__)
/* out -> x0, fn -> x1, iargs -> x2, vargs -> x3, nvargs -> x4 */
    stp     x29, x30, [sp, #-224]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]
    str     x0, [sp, #96]          /* out    */
    str     x1, [sp, #104]         /* fn     */
    str     x2, [sp, #112]         /* iargs  */
    str     x3, [sp, #120]         /* vargs  */
    str     x4, [sp, #128]         /* nvargs */

    /* Preserve the caller's callee-saved FP regs d8-d15 (x29-relative, above the
     * variable overflow area); seeded below, restored after capture. */
    stp     d8, d9,   [x29, #160]
    stp     d10, d11, [x29, #176]
    stp     d12, d13, [x29, #192]
    stp     d14, d15, [x29, #208]

    /* n_stack = max(0, nvargs - 8) -> x9 (each slot is 16 bytes). */
    subs    x9, x4, #8
    b.gt    70f
    mov     x9, #0
70:
    lsl     x10, x9, #4            /* n_stack * 16 */
    sub     sp, sp, x10

    /* Copy overflow vectors as 8-byte words: words = n_stack*2,
       [sp + j*8] = vargs[128 + j*8]. */
    lsl     x9, x9, #1
    ldr     x11, [x29, #120]
    mov     x12, #0
71:
    cmp     x12, x9
    b.ge    72f
    add     x13, x12, #16
    ldr     x14, [x11, x13, lsl #3]
    str     x14, [sp, x12, lsl #3]
    add     x12, x12, #1
    b       71b
72:
    /* Load vector register args q0..7 (only as many as nvargs). */
    ldr     x11, [x29, #120]       /* vargs  */
    ldr     x10, [x29, #128]       /* nvargs */
    cmp     x10, #1
    b.lt    73f
    ldr     q0, [x11, #0]
    cmp     x10, #2
    b.lt    73f
    ldr     q1, [x11, #16]
    cmp     x10, #3
    b.lt    73f
    ldr     q2, [x11, #32]
    cmp     x10, #4
    b.lt    73f
    ldr     q3, [x11, #48]
    cmp     x10, #5
    b.lt    73f
    ldr     q4, [x11, #64]
    cmp     x10, #6
    b.lt    73f
    ldr     q5, [x11, #80]
    cmp     x10, #7
    b.lt    73f
    ldr     q6, [x11, #96]
    cmp     x10, #8
    b.lt    73f
    ldr     q7, [x11, #112]
73:
    /* Integer args: iargs -> x0..x5. */
    ldr     x9, [x29, #112]
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

    /* Seed d8-d15 low 64 bits with sentinels 8..15 (ASSERT_ABI_PRESERVED_VEC). */
    mov     x9, #8
    fmov    d8, x9
    mov     x9, #9
    fmov    d9, x9
    mov     x9, #10
    fmov    d10, x9
    mov     x9, #11
    fmov    d11, x9
    mov     x9, #12
    fmov    d12, x9
    mov     x9, #13
    fmov    d13, x9
    mov     x9, #14
    fmov    d14, x9
    mov     x9, #15
    fmov    d15, x9

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
    str     d0, [x11, #104]        /* fret */

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

    /* Restore the caller's d8-d15 (captured above into vec[8..15]). */
    ldp     d8, d9,   [x29, #160]
    ldp     d10, d11, [x29, #176]
    ldp     d12, d13, [x29, #192]
    ldp     d14, d15, [x29, #208]

    mov     sp, x29
    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldp     x23, x24, [sp, #48]
    ldp     x25, x26, [sp, #64]
    ldp     x27, x28, [sp, #80]
    ldp     x29, x30, [sp], #224
    ret

#elif defined(__riscv) && __riscv_xlen == 64
/* rv64gc has no vector registers; a stub so the symbol resolves. Never called:
 * asmtest_cpu_has_vec128() is false on rv64, so the ASM_VCALL macros self-skip. */
    ret
#else
#  error "capture.s supports x86-64, AArch64 and RV64 only"
#endif

ASM_ENDFUNC asm_call_capture_vec_n

/*
 * void asm_call_capture_vec256(vec256_t *vec, void *fn, const long iargs[6],
 *                              const vec256_t vargs[8]);
 * The AVX2 analog of asm_call_capture_vec: marshal 8 full 256-bit vector args
 * into ymm0..7 and capture the ymm file (ymm0..15) into vec[0..15] (32 bytes
 * each; vec[0] = return). x86-64 + AVX2 only — the C wrapper / ASM_VCALL256*
 * macros gate it on asmtest_cpu_has_avx2(), so a non-AVX2 host never reaches the
 * VEX-encoded body. Captures the vector file only (the 128-bit path covers GP /
 * flags). vzeroupper on exit avoids the SSE/AVX transition penalty for any
 * legacy-SSE code that runs afterwards.
 */
ASM_FUNC asm_call_capture_vec256

#if defined(__x86_64__)
/* vec -> %rdi, fn -> %rsi, iargs -> %rdx, vargs -> %rcx */
    pushq   %rbx
    pushq   %rbp
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    subq    $24, %rsp
    movq    %rdi, 0(%rsp)          /* save vec out ptr */
    movq    %rsi, 8(%rsp)          /* save fn */

    /* Vector args: vargs (rcx) -> ymm0..ymm7 (full 256 bits). */
    vmovdqu 0(%rcx),   %ymm0
    vmovdqu 32(%rcx),  %ymm1
    vmovdqu 64(%rcx),  %ymm2
    vmovdqu 96(%rcx),  %ymm3
    vmovdqu 128(%rcx), %ymm4
    vmovdqu 160(%rcx), %ymm5
    vmovdqu 192(%rcx), %ymm6
    vmovdqu 224(%rcx), %ymm7

    /* Integer args: iargs (rdx) -> rdi,rsi,rdx,rcx,r8,r9. */
    movq    %rdx, %rax
    movq    0(%rax),  %rdi
    movq    8(%rax),  %rsi
    movq    24(%rax), %rcx
    movq    32(%rax), %r8
    movq    40(%rax), %r9
    movq    16(%rax), %rdx

    movq    8(%rsp), %r11
    movl    $8, %eax               /* variadic ABI: 8 vector registers */
    call    *%r11

    movq    0(%rsp), %r11          /* vec out ptr */
    /* Full ymm file: ymm0..15 -> vec[0..15] (32 bytes each). */
    vmovdqu %ymm0,  0(%r11)
    vmovdqu %ymm1,  32(%r11)
    vmovdqu %ymm2,  64(%r11)
    vmovdqu %ymm3,  96(%r11)
    vmovdqu %ymm4,  128(%r11)
    vmovdqu %ymm5,  160(%r11)
    vmovdqu %ymm6,  192(%r11)
    vmovdqu %ymm7,  224(%r11)
    vmovdqu %ymm8,  256(%r11)
    vmovdqu %ymm9,  288(%r11)
    vmovdqu %ymm10, 320(%r11)
    vmovdqu %ymm11, 352(%r11)
    vmovdqu %ymm12, 384(%r11)
    vmovdqu %ymm13, 416(%r11)
    vmovdqu %ymm14, 448(%r11)
    vmovdqu %ymm15, 480(%r11)
    vzeroupper

    addq    $24, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbp
    popq    %rbx
    ret

#else
/* AVX is x86-only; a stub so the symbol resolves on other arches. Never called:
 * asmtest_cpu_has_avx2() is false off x86, so the macros self-skip. */
    ret
#endif

ASM_ENDFUNC asm_call_capture_vec256

/*
 * void asm_call_capture_vec512(vec512_t *vec, void *fn, const long iargs[6],
 *                              const vec512_t vargs[8]);
 * The AVX-512 analog of asm_call_capture_vec256: marshal 8 full 512-bit vector args
 * into zmm0..7 and capture the zmm file (zmm0..31 — AVX-512 doubles the register
 * count) into vec[0..31] (64 bytes each; vec[0] = return). x86-64 + AVX-512F only —
 * the C wrapper / ASM_VCALL512* macros gate it on asmtest_cpu_has_avx512f(), so a
 * host without AVX-512 never reaches the EVEX-encoded body. vmovdqu64 is the
 * EVEX-encoded unaligned move (required to reach zmm16..31). vzeroupper on exit.
 */
ASM_FUNC asm_call_capture_vec512

#if defined(__x86_64__)
/* vec -> %rdi, fn -> %rsi, iargs -> %rdx, vargs -> %rcx */
    pushq   %rbx
    pushq   %rbp
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    subq    $24, %rsp
    movq    %rdi, 0(%rsp)          /* save vec out ptr */
    movq    %rsi, 8(%rsp)          /* save fn */

    /* Vector args: vargs (rcx) -> zmm0..zmm7 (full 512 bits). */
    vmovdqu64 0(%rcx),   %zmm0
    vmovdqu64 64(%rcx),  %zmm1
    vmovdqu64 128(%rcx), %zmm2
    vmovdqu64 192(%rcx), %zmm3
    vmovdqu64 256(%rcx), %zmm4
    vmovdqu64 320(%rcx), %zmm5
    vmovdqu64 384(%rcx), %zmm6
    vmovdqu64 448(%rcx), %zmm7

    /* Integer args: iargs (rdx) -> rdi,rsi,rdx,rcx,r8,r9. */
    movq    %rdx, %rax
    movq    0(%rax),  %rdi
    movq    8(%rax),  %rsi
    movq    24(%rax), %rcx
    movq    32(%rax), %r8
    movq    40(%rax), %r9
    movq    16(%rax), %rdx

    movq    8(%rsp), %r11
    movl    $8, %eax               /* variadic ABI: 8 vector registers */
    call    *%r11

    movq    0(%rsp), %r11          /* vec out ptr */
    /* Full zmm file: zmm0..31 -> vec[0..31] (64 bytes each). */
    vmovdqu64 %zmm0,  0(%r11)
    vmovdqu64 %zmm1,  64(%r11)
    vmovdqu64 %zmm2,  128(%r11)
    vmovdqu64 %zmm3,  192(%r11)
    vmovdqu64 %zmm4,  256(%r11)
    vmovdqu64 %zmm5,  320(%r11)
    vmovdqu64 %zmm6,  384(%r11)
    vmovdqu64 %zmm7,  448(%r11)
    vmovdqu64 %zmm8,  512(%r11)
    vmovdqu64 %zmm9,  576(%r11)
    vmovdqu64 %zmm10, 640(%r11)
    vmovdqu64 %zmm11, 704(%r11)
    vmovdqu64 %zmm12, 768(%r11)
    vmovdqu64 %zmm13, 832(%r11)
    vmovdqu64 %zmm14, 896(%r11)
    vmovdqu64 %zmm15, 960(%r11)
    vmovdqu64 %zmm16, 1024(%r11)
    vmovdqu64 %zmm17, 1088(%r11)
    vmovdqu64 %zmm18, 1152(%r11)
    vmovdqu64 %zmm19, 1216(%r11)
    vmovdqu64 %zmm20, 1280(%r11)
    vmovdqu64 %zmm21, 1344(%r11)
    vmovdqu64 %zmm22, 1408(%r11)
    vmovdqu64 %zmm23, 1472(%r11)
    vmovdqu64 %zmm24, 1536(%r11)
    vmovdqu64 %zmm25, 1600(%r11)
    vmovdqu64 %zmm26, 1664(%r11)
    vmovdqu64 %zmm27, 1728(%r11)
    vmovdqu64 %zmm28, 1792(%r11)
    vmovdqu64 %zmm29, 1856(%r11)
    vmovdqu64 %zmm30, 1920(%r11)
    vmovdqu64 %zmm31, 1984(%r11)
    vzeroupper

    addq    $24, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbp
    popq    %rbx
    ret

#else
/* AVX-512 is x86-only; a stub so the symbol resolves on other arches. Never called:
 * asmtest_cpu_has_avx512f() is false off x86, so the macros self-skip. */
    ret
#endif

ASM_ENDFUNC asm_call_capture_vec512

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

#elif defined(__riscv) && __riscv_xlen == 64
/* out -> a0, fn -> a1, args -> a2, nargs -> a3
 * 8 register args a0..a7; overflow args[8+] spill to 0(sp),8(sp),... s0 is the
 * frame pointer (reported preserved, not independently checked). */
    addi    sp, sp, -160
    sd      ra, 0(sp)
    sd      s0, 8(sp)
    mv      s0, sp
    sd      s1, 16(sp)
    sd      s2, 24(sp)
    sd      s3, 32(sp)
    sd      s4, 40(sp)
    sd      s5, 48(sp)
    sd      s6, 56(sp)
    sd      s7, 64(sp)
    sd      s8, 72(sp)
    sd      s9, 80(sp)
    sd      s10, 88(sp)
    sd      s11, 96(sp)
    sd      a0, 104(sp)            /* out   */
    sd      a1, 112(sp)            /* fn    */
    sd      a2, 120(sp)            /* args  */
    sd      a3, 128(sp)            /* nargs */

    /* n_stack = max(0, nargs - 8) -> t1 */
    ld      t0, 128(s0)
    li      t1, 0
    li      t2, 8
    ble     t0, t2, 1f
    sub     t1, t0, t2
1:
    slli    t2, t1, 3
    addi    t2, t2, 15
    andi    t2, t2, -16
    sub     sp, sp, t2

    /* Copy overflow args: [sp + i*8] = args[8 + i]. */
    ld      t2, 120(s0)
    li      t3, 0
2:
    bge     t3, t1, 3f
    addi    t4, t3, 8
    slli    t5, t4, 3
    add     t5, t2, t5
    ld      t6, 0(t5)
    slli    t4, t3, 3
    add     t4, sp, t4
    sd      t6, 0(t4)
    addi    t3, t3, 1
    j       2b
3:
    /* Load the register args a0..a7 (only as many as nargs). */
    ld      t2, 120(s0)
    ld      t0, 128(s0)
    li      t3, 1
    blt     t0, t3, 4f
    ld      a0, 0(t2)
    li      t3, 2
    blt     t0, t3, 4f
    ld      a1, 8(t2)
    li      t3, 3
    blt     t0, t3, 4f
    ld      a2, 16(t2)
    li      t3, 4
    blt     t0, t3, 4f
    ld      a3, 24(t2)
    li      t3, 5
    blt     t0, t3, 4f
    ld      a4, 32(t2)
    li      t3, 6
    blt     t0, t3, 4f
    ld      a5, 40(t2)
    li      t3, 7
    blt     t0, t3, 4f
    ld      a6, 48(t2)
    li      t3, 8
    blt     t0, t3, 4f
    ld      a7, 56(t2)
4:
    /* Seed callee-saved sentinels (s0 is the frame pointer, seeded via out). */
    li      s1, 0x2222222222222222
    li      s2, 0x3333333333333333
    li      s3, 0x4444444444444444
    li      s4, 0x5555555555555555
    li      s5, 0x6666666666666666
    li      s6, 0x7777777777777777
    li      s7, 0x8888888888888888
    li      s8, 0x9999999999999999
    li      s9, 0xAAAAAAAAAAAAAAAA
    li      s10, 0xBBBBBBBBBBBBBBBB
    li      s11, 0xCCCCCCCCCCCCCCCC

    ld      t0, 112(s0)            /* fn */
    jalr    ra, 0(t0)

    ld      t1, 104(s0)            /* out */
    sd      a0, 0(t1)
    sd      a1, 8(t1)
    li      t2, 0x1111111111111111 /* s0: reported preserved, not checked */
    sd      t2, 16(t1)
    sd      s1, 24(t1)
    sd      s2, 32(t1)
    sd      s3, 40(t1)
    sd      s4, 48(t1)
    sd      s5, 56(t1)
    sd      s6, 64(t1)
    sd      s7, 72(t1)
    sd      s8, 80(t1)
    sd      s9, 88(t1)
    sd      s10, 96(t1)
    sd      s11, 104(t1)
    sd      zero, 112(t1)          /* flags == 0 */

    mv      sp, s0
    ld      ra, 0(sp)
    ld      s1, 16(sp)
    ld      s2, 24(sp)
    ld      s3, 32(sp)
    ld      s4, 40(sp)
    ld      s5, 48(sp)
    ld      s6, 56(sp)
    ld      s7, 64(sp)
    ld      s8, 72(sp)
    ld      s9, 80(sp)
    ld      s10, 88(sp)
    ld      s11, 96(sp)
    ld      s0, 8(sp)
    addi    sp, sp, 160
    ret

#else
#  error "capture.s supports x86-64, AArch64 and RV64 only"
#endif

ASM_ENDFUNC asm_call_capture_args

/*
 * void asm_call_capture_sret(regs_t *out, void *fn, void *result,
 *                            const long *args, int nargs);
 * Calls fn which returns a large struct via the hidden result pointer (rdi on
 * x86-64, x8 on AArch64). The visible integer args follow: x86-64 uses 5
 * register slots (rsi..r9), AArch64 uses 8 (x0..x7), then the stack.
 */
ASM_FUNC asm_call_capture_sret

#if defined(__x86_64__)
/* out -> %rdi, fn -> %rsi, result -> %rdx, args -> %rcx, nargs -> %r8 */
    pushq   %rbp
    movq    %rsp, %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    subq    $48, %rsp
    movq    %rdi, -48(%rbp)        /* out    */
    movq    %rsi, -56(%rbp)        /* fn     */
    movq    %rdx, -64(%rbp)        /* result */
    movq    %rcx, -72(%rbp)        /* args   */
    movq    %r8,  -80(%rbp)        /* nargs  */

    /* n_stack = max(0, nargs - 5)  (5 visible register args: rsi..r9) */
    movq    %r8, %r10
    subq    $5, %r10
    jg      30f
    xorq    %r10, %r10
30:
    andq    $-16, %rsp
    movq    %r10, %rax
    shlq    $3, %rax
    addq    $15, %rax
    andq    $-16, %rax
    subq    %rax, %rsp

    /* Copy overflow args: [rsp + i*8] = args[5 + i]. */
    movq    -72(%rbp), %r8
    xorq    %rcx, %rcx
31:
    cmpq    %r10, %rcx
    jge     32f
    movq    40(%r8,%rcx,8), %rax
    movq    %rax, (%rsp,%rcx,8)
    incq    %rcx
    jmp     31b
32:
    /* Load visible register args into rsi,rdx,rcx,r8,r9 (only as many as nargs). */
    movq    -72(%rbp), %r11
    movq    -80(%rbp), %r10
    cmpq    $1, %r10
    jl      33f
    movq    0(%r11), %rsi
    cmpq    $2, %r10
    jl      33f
    movq    8(%r11), %rdx
    cmpq    $3, %r10
    jl      33f
    movq    16(%r11), %rcx
    cmpq    $4, %r10
    jl      33f
    movq    24(%r11), %r8
    cmpq    $5, %r10
    jl      33f
    movq    32(%r11), %r9
33:
    movq    -64(%rbp), %rdi        /* hidden result pointer */
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
    movabsq $0x2222222222222222, %rax
    movq    %rax, 24(%r11)
    movq    %r12, 32(%r11)
    movq    %r13, 40(%r11)
    movq    %r14, 48(%r11)
    movq    %r15, 56(%r11)
    pushfq
    popq    %rax
    movq    %rax, 64(%r11)

    leaq    -40(%rbp), %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret

#elif defined(__aarch64__)
/* out -> x0, fn -> x1, result -> x2, args -> x3, nargs -> x4 */
    stp     x29, x30, [sp, #-176]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]
    str     x0, [sp, #96]          /* out    */
    str     x1, [sp, #104]         /* fn     */
    str     x2, [sp, #112]         /* result */
    str     x3, [sp, #120]         /* args   */
    str     x4, [sp, #128]         /* nargs  */

    /* n_stack = max(0, nargs - 8) */
    subs    x9, x4, #8
    b.gt    40f
    mov     x9, #0
40:
    lsl     x10, x9, #3
    add     x10, x10, #15
    and     x10, x10, #-16
    sub     sp, sp, x10

    ldr     x11, [x29, #120]
    mov     x12, #0
41:
    cmp     x12, x9
    b.ge    42f
    add     x13, x12, #8
    ldr     x14, [x11, x13, lsl #3]
    str     x14, [sp, x12, lsl #3]
    add     x12, x12, #1
    b       41b
42:
    ldr     x11, [x29, #120]
    ldr     x10, [x29, #128]
    cmp     x10, #1
    b.lt    43f
    ldr     x0, [x11, #0]
    cmp     x10, #2
    b.lt    43f
    ldr     x1, [x11, #8]
    cmp     x10, #3
    b.lt    43f
    ldr     x2, [x11, #16]
    cmp     x10, #4
    b.lt    43f
    ldr     x3, [x11, #24]
    cmp     x10, #5
    b.lt    43f
    ldr     x4, [x11, #32]
    cmp     x10, #6
    b.lt    43f
    ldr     x5, [x11, #40]
    cmp     x10, #7
    b.lt    43f
    ldr     x6, [x11, #48]
    cmp     x10, #8
    b.lt    43f
    ldr     x7, [x11, #56]
43:
    ldr     x8, [x29, #112]        /* hidden result pointer (x8) */
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
    ldr     x12, =0xBBBBBBBBBBBBBBBB
    str     x12, [x11, #88]
    mrs     x12, nzcv
    str     x12, [x11, #96]

    mov     sp, x29
    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldp     x23, x24, [sp, #48]
    ldp     x25, x26, [sp, #64]
    ldp     x27, x28, [sp, #80]
    ldp     x29, x30, [sp], #176
    ret

#elif defined(__riscv) && __riscv_xlen == 64
/* out -> a0, fn -> a1, result -> a2, args -> a3, nargs -> a4
 * The hidden struct-return pointer is the implicit FIRST argument in a0 (the
 * x86-64/rdi shape, NOT AArch64's dedicated x8); the visible args follow in
 * a1..a7 (7 register slots), overflow args[7+] spilling to the stack. s0 is the
 * frame pointer (reported preserved, not independently checked). */
    addi    sp, sp, -176
    sd      ra, 0(sp)
    sd      s0, 8(sp)
    mv      s0, sp
    sd      s1, 16(sp)
    sd      s2, 24(sp)
    sd      s3, 32(sp)
    sd      s4, 40(sp)
    sd      s5, 48(sp)
    sd      s6, 56(sp)
    sd      s7, 64(sp)
    sd      s8, 72(sp)
    sd      s9, 80(sp)
    sd      s10, 88(sp)
    sd      s11, 96(sp)
    sd      a0, 104(sp)            /* out    */
    sd      a1, 112(sp)            /* fn     */
    sd      a2, 120(sp)            /* result */
    sd      a3, 128(sp)            /* args   */
    sd      a4, 136(sp)            /* nargs  */

    /* n_stack = max(0, nargs - 7)  (7 visible register args: a1..a7) */
    ld      t0, 136(s0)
    li      t1, 0
    li      t2, 7
    ble     t0, t2, 1f
    sub     t1, t0, t2
1:
    slli    t2, t1, 3
    addi    t2, t2, 15
    andi    t2, t2, -16
    sub     sp, sp, t2

    /* Copy overflow args: [sp + i*8] = args[7 + i]. */
    ld      t2, 128(s0)
    li      t3, 0
2:
    bge     t3, t1, 3f
    addi    t4, t3, 7
    slli    t5, t4, 3
    add     t5, t2, t5
    ld      t6, 0(t5)
    slli    t4, t3, 3
    add     t4, sp, t4
    sd      t6, 0(t4)
    addi    t3, t3, 1
    j       2b
3:
    /* Load visible register args into a1..a7 (only as many as nargs). */
    ld      t2, 128(s0)
    ld      t0, 136(s0)
    li      t3, 1
    blt     t0, t3, 4f
    ld      a1, 0(t2)
    li      t3, 2
    blt     t0, t3, 4f
    ld      a2, 8(t2)
    li      t3, 3
    blt     t0, t3, 4f
    ld      a3, 16(t2)
    li      t3, 4
    blt     t0, t3, 4f
    ld      a4, 24(t2)
    li      t3, 5
    blt     t0, t3, 4f
    ld      a5, 32(t2)
    li      t3, 6
    blt     t0, t3, 4f
    ld      a6, 40(t2)
    li      t3, 7
    blt     t0, t3, 4f
    ld      a7, 48(t2)
4:
    ld      a0, 120(s0)            /* hidden result pointer -> a0 */

    /* Seed callee-saved sentinels (s0 is the frame pointer, seeded via out). */
    li      s1, 0x2222222222222222
    li      s2, 0x3333333333333333
    li      s3, 0x4444444444444444
    li      s4, 0x5555555555555555
    li      s5, 0x6666666666666666
    li      s6, 0x7777777777777777
    li      s7, 0x8888888888888888
    li      s8, 0x9999999999999999
    li      s9, 0xAAAAAAAAAAAAAAAA
    li      s10, 0xBBBBBBBBBBBBBBBB
    li      s11, 0xCCCCCCCCCCCCCCCC

    ld      t0, 112(s0)            /* fn */
    jalr    ra, 0(t0)

    ld      t1, 104(s0)            /* out */
    sd      a0, 0(t1)
    sd      a1, 8(t1)
    li      t2, 0x1111111111111111 /* s0: reported preserved, not checked */
    sd      t2, 16(t1)
    sd      s1, 24(t1)
    sd      s2, 32(t1)
    sd      s3, 40(t1)
    sd      s4, 48(t1)
    sd      s5, 56(t1)
    sd      s6, 64(t1)
    sd      s7, 72(t1)
    sd      s8, 80(t1)
    sd      s9, 88(t1)
    sd      s10, 96(t1)
    sd      s11, 104(t1)
    sd      zero, 112(t1)          /* flags == 0 */

    mv      sp, s0
    ld      ra, 0(sp)
    ld      s1, 16(sp)
    ld      s2, 24(sp)
    ld      s3, 32(sp)
    ld      s4, 40(sp)
    ld      s5, 48(sp)
    ld      s6, 56(sp)
    ld      s7, 64(sp)
    ld      s8, 72(sp)
    ld      s9, 80(sp)
    ld      s10, 88(sp)
    ld      s11, 96(sp)
    ld      s0, 8(sp)
    addi    sp, sp, 176
    ret

#else
#  error "capture.s supports x86-64, AArch64 and RV64 only"
#endif

ASM_ENDFUNC asm_call_capture_sret

#if defined(__x86_64__)
/*
 * void asm_bigstruct_x86(regs_t *out, void *fn, const long *iargs, int niargs,
 *                        const void *sptr, size_t ssize);
 * SysV-only helper: pass niargs integer register args, then copy an ssize-byte
 * by-value struct inline onto the stack (memory class). AArch64 uses a pointer
 * instead, handled in C (asm_call_capture_bigstruct).
 *   out -> %rdi, fn -> %rsi, iargs -> %rdx, niargs -> %rcx, sptr -> %r8,
 *   ssize -> %r9
 */
ASM_FUNC asm_bigstruct_x86
    pushq   %rbp
    movq    %rsp, %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    subq    $48, %rsp
    movq    %rdi, -48(%rbp)        /* out    */
    movq    %rsi, -56(%rbp)        /* fn     */
    movq    %rdx, -64(%rbp)        /* iargs  */
    movq    %rcx, -72(%rbp)        /* niargs */
    movq    %r8,  -80(%rbp)        /* sptr   */
    movq    %r9,  -88(%rbp)        /* ssize  */

    /* 16-align rsp, reserve round_up(ssize, 16). */
    andq    $-16, %rsp
    movq    %r9, %rax
    addq    $15, %rax
    andq    $-16, %rax
    subq    %rax, %rsp

    /* Byte-copy the struct: [rsp + i] = sptr[i]. */
    movq    -80(%rbp), %r8
    movq    -88(%rbp), %r9
    xorq    %rcx, %rcx
50:
    cmpq    %r9, %rcx
    jge     51f
    movb    (%r8,%rcx,1), %al
    movb    %al, (%rsp,%rcx,1)
    incq    %rcx
    jmp     50b
51:
    /* Load niargs integer register args. */
    movq    -64(%rbp), %r11
    movq    -72(%rbp), %r10
    cmpq    $1, %r10
    jl      52f
    movq    0(%r11), %rdi
    cmpq    $2, %r10
    jl      52f
    movq    8(%r11), %rsi
    cmpq    $3, %r10
    jl      52f
    movq    16(%r11), %rdx
    cmpq    $4, %r10
    jl      52f
    movq    24(%r11), %rcx
    cmpq    $5, %r10
    jl      52f
    movq    32(%r11), %r8
    cmpq    $6, %r10
    jl      52f
    movq    40(%r11), %r9
52:
    movabsq $0x1111111111111111, %rbx
    movabsq $0x3333333333333333, %r12
    movabsq $0x4444444444444444, %r13
    movabsq $0x5555555555555555, %r14
    movabsq $0x6666666666666666, %r15

    movq    -56(%rbp), %r11
    xorl    %eax, %eax
    call    *%r11

    movq    -48(%rbp), %r11
    movq    %rax, 0(%r11)
    movq    %rdx, 8(%r11)
    movq    %rbx, 16(%r11)
    movabsq $0x2222222222222222, %rax
    movq    %rax, 24(%r11)
    movq    %r12, 32(%r11)
    movq    %r13, 40(%r11)
    movq    %r14, 48(%r11)
    movq    %r15, 56(%r11)
    pushfq
    popq    %rax
    movq    %rax, 64(%r11)

    leaq    -40(%rbp), %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret
ASM_ENDFUNC asm_bigstruct_x86
#endif
