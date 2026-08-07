/* evex_victim.c — a victim whose hot routines write the UPPER SIXTEEN vector
 * registers (ymm16-31 / xmm16-31), the half of the vector file the live ptrace
 * dataflow producer used to refuse.
 *
 * WHY THIS SHAPE. The refusal was not a hardware wall, it was three `idx > 15`
 * guards in src/dataflow_ptrace.c sitting BELOW a classification that already
 * admitted XMM0..+31 / YMM0..+31. The cost is concrete: glibc's EVEX string and
 * memory routines are 256-bit and keep their vectors in the high half — measured
 * on this host, /lib/x86_64-linux-gnu/libc.so.6 contains 2090 instructions
 * naming ymm16-31 (`objdump -d | grep -c 'ymm1[6-9]|ymm2[0-9]|ymm3[01]'`) — so a
 * dataflow capture over any memcpy-heavy region recorded value_valid:false where
 * the values belong. This victim puts KNOWN bytes in those registers so the
 * smoke can assert the RECORDED values, not merely that a marker went away.
 *
 * THE PATTERNS ARE THE TEST, AND THEY ARE ANNOUNCED. The upper sixteen live in
 * XSAVE component 7 (Hi16_ZMM), whose offset is discovered at runtime from
 * CPUID(0xd,7) — measured 1408 here, stride 64 (the FULL zmm16..31, so ymm16 is
 * its first 32 bytes). A wrong component or a wrong stride does not fail loudly;
 * it yields plausible garbage. So each register gets a DISTINCT pattern with no
 * repeating 16-byte lane, and the victim prints them in hex on stderr: the smoke
 * compares what the producer recorded against what the victim SAYS it wrote,
 * rather than against a second hardcoded copy that could drift.
 *
 *   ymm0/ymm1  DECOYS, written FIRST. Component 2 (AVX / YMM_Hi128, offset 576)
 *              is the component read_ymm already used for the low sixteen. If the
 *              high-register read lands there instead, it returns these bytes —
 *              a specific wrong answer, not zeros, so "recorded == ymm16 pattern"
 *              genuinely excludes it.
 *   ymm16/17   the 256-bit case (read_ymm). Adjacent indices with patterns that
 *              differ in every byte, so a stride error (16 instead of 64) is
 *              caught, not just a component error.
 *   xmm20      the 128-bit case (read_xmm), whose low 128 bits live in the SAME
 *              component — a separate code path, and a non-adjacent index so an
 *              off-by-a-few in the index arithmetic shows up.
 *
 * evex_gap() covers the THIRD site: the gap barrier's alias-slice path, which
 * runs only when the capture elides an out-of-region excursion. It writes ymm18,
 * then calls evex_clobber() — a separate, out-of-region function that overwrites
 * ymm18 with a fourth known pattern. The producer steps OVER that call and must
 * emit a gap-barrier step recording ymm18's new value: exactly the shape of a
 * region that calls glibc's EVEX memmove, but with a deterministic answer.
 *
 * AVX-512 IS A REAL GATE. Without it CPUID(0xd) reports no component 7 and the
 * producer's correct behaviour is to decline. The guard below is a hardware gate
 * under CLAUDE.md, not a missing dependency: it exits 0 with the reason stated so
 * the smoke's own guard can name the same feature.
 *
 * Opts in to foreign attach via PR_SET_PTRACER_ANY like every other victim here:
 * Yama ptrace_scope=1 is the Ubuntu default and is NOT namespaced.
 */
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

/* A fixed 32-byte salt (one LCG walk from a constant seed) XORed with a
 * per-register tag byte. XOR is position-preserving, so two tags differ in EVERY
 * byte AND no pattern is a shifted copy of another. That second property is the
 * one that matters: a first attempt used a linear ramp, and ymm16's upper half
 * came out byte-identical to the decoy's lower half — exactly the coincidence
 * that would let a half-lane offset error read as a near-miss instead of a miss. */
static void make_pattern(unsigned char *p, unsigned tag) {
    unsigned s = 0x1234567u;
    for (unsigned i = 0; i < 32; i++) {
        s = s * 1103515245u + 12345u;
        p[i] = (unsigned char)(((s >> 16) & 0xffu) ^ (tag * 0x11u + 0x5au));
    }
}

static void print_hex(const char *name, const unsigned char *p, unsigned n) {
    fprintf(stderr, " %s=", name);
    for (unsigned i = 0; i < n; i++)
        fprintf(stderr, "%02x", p[i]);
}

static unsigned char pat_dec0[32], pat_dec1[32];
static unsigned char pat_y16[32], pat_y17[32], pat_x20[32], pat_y18[32],
    pat_glue[32];
static unsigned char sink_buf[32];
static volatile long g_sink;

/* Writes ymm18 with a pattern of its own, so the gap barrier's diff has a known
 * BEFORE as well as a known AFTER. Deliberately NOT static-inlined into
 * evex_gap: the barrier only exists for a call that leaves the captured region. */
__attribute__((noinline, target("avx512f,avx512vl"))) static void
evex_clobber(void) {
    __asm__ volatile("vmovdqu64 (%0), %%ymm18\n\t"
                     :
                     : "r"(pat_glue)
                     : "memory", "ymm18");
}

/* The value-strict routine. Decoys first (so a misread of component 2 returns
 * THEM), then the three registers under test. The trailing store keeps the
 * loads from being dead and gives the last write a following instruction, which
 * the producer needs: it defers a write's value to the NEXT stop. */
__attribute__((noinline, target("avx512f,avx512vl"))) long evex_hot(long x) {
    __asm__ volatile("vmovdqu64 (%0), %%ymm0\n\t"
                     "vmovdqu64 (%1), %%ymm1\n\t"
                     "vmovdqu64 (%2), %%ymm16\n\t"
                     "vmovdqu64 (%3), %%ymm17\n\t"
                     "vmovdqu64 (%4), %%xmm20\n\t"
                     "vmovdqu64 %%ymm16, (%5)\n\t"
                     :
                     : "r"(pat_dec0), "r"(pat_dec1), "r"(pat_y16), "r"(pat_y17),
                       "r"(pat_x20), "r"(sink_buf)
                     : "memory", "ymm0", "ymm1", "ymm16", "ymm17", "xmm20");
    return x + sink_buf[0];
}

/* The gap-barrier routine: write ymm18, leave the region, come back changed. */
__attribute__((noinline, target("avx512f,avx512vl"))) long evex_gap(long x) {
    __asm__ volatile("vmovdqu64 (%0), %%ymm18\n\t"
                     :
                     : "r"(pat_y18)
                     : "memory", "ymm18");
    evex_clobber(); /* out-of-region: the capture elides it and must emit a barrier */
    __asm__ volatile("vmovdqu64 %%ymm18, (%0)\n\t"
                     :
                     : "r"(sink_buf)
                     : "memory");
    return x + sink_buf[0];
}

int main(void) {
    /* The hardware gate, checked before a single EVEX byte executes. Component 7
     * only exists on an AVX-512 machine; without it the producer's refusal is
     * correct and there is nothing here to measure. Exit 0 with the reason on
     * stdout so a smoke that captured this output can quote it verbatim. */
    if (!__builtin_cpu_supports("avx512f")) {
        printf(
            "# SKIP evex_victim: this CPU has no avx512f, so XSAVE component "
            "7 (Hi16_ZMM) does not exist and ymm16-31 are unreachable.\n");
        printf(
            "#   Nothing to install — this is a hardware gate, not a missing "
            "dependency.\n");
        fflush(stdout);
        return 0;
    }
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

    make_pattern(pat_dec0, 0);
    make_pattern(pat_dec1, 1);
    make_pattern(pat_y16, 16);
    make_pattern(pat_y17, 17);
    make_pattern(pat_x20, 20);
    make_pattern(pat_y18, 18);
    make_pattern(pat_glue, 31);

    fprintf(stderr, "evex_victim pid=%d evex_hot=%p evex_gap=%p\n",
            (int)getpid(), (void *)evex_hot, (void *)evex_gap);
    /* The expected values, on the wire, in the producer's own byte order: `bytes`
     * in a df_step op is the operand's memory image low byte first, and a
     * vmovdqu64 load puts memory byte 0 in register byte 0, so these hex strings
     * are literally what a correct capture must record. */
    fprintf(stderr, "evex_victim expect");
    print_hex("ymm16", pat_y16, 32);
    print_hex("ymm17", pat_y17, 32);
    print_hex("xmm20", pat_x20, 16);
    print_hex("ymm18", pat_y18, 32);
    print_hex("glue18", pat_glue, 32);
    print_hex("decoy0", pat_dec0, 32);
    fprintf(stderr, "\n");
    fflush(stderr);

    for (;;) {
        g_sink += evex_hot(1);
        g_sink += evex_gap(2);
        usleep(2 * 1000); /* ~500 entries/s: dense enough for any entry wait */
    }
    return 0;
}
