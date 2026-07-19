/*
 * fuzz.c — coverage-guided input generation + mutation testing over the
 * emulator (Track E). Closes the loop between the basic-block coverage the
 * emulator already records (emu_trace_t) and input generation, and proves a
 * test's input set actually catches a perturbed routine.
 *
 * Both run the routine INSIDE the emulator, so a pathological input or a broken
 * mutant (an infinite loop, a wild branch, a bad access) is contained by the
 * instruction cap and the fault hooks instead of taking down the host.
 *
 * Self-contained: it uses the framework's seedable splitmix64 RNG (asmtest.h,
 * already linked) and the emulator's coverage trace (asmtest_emu.h) — no extra
 * dependency. Kept in its own translation unit so it links only where wanted.
 */
#include "asmtest.h"
#include "asmtest_emu.h"

#include <stdlib.h>
#include <string.h>

/* Internal (defined in emu.c, which owns `struct emu`): hand the coverage-growing
 * corpus to the handle so it outlives this call without the stat owning it. */
void emu_fuzz_set_corpus(emu_t *e, long *corpus, size_t n);

/* Cap a mutant/candidate run so a runaway routine can't spin forever; real
 * routines under test return in far fewer instructions than this. */
#define FUZZ_INSN_CAP 100000ULL

/* ------------------------------------------------------------------ */
/* Coverage-guided generation                                          */
/* ------------------------------------------------------------------ */

bool emu_fuzz_cover1(emu_t *e, const void *code, size_t code_len, long lo,
                     long hi, uint64_t iters, uint64_t seed, emu_trace_t *uni,
                     emu_fuzz_stat_t *stat) {
    if (e == NULL || code == NULL || uni == NULL || uni->blocks == NULL ||
        hi < lo)
        return false;
    asmtest_rng_t rng = {seed};
    /* Corpus of inputs that each grew coverage; new candidates are drawn fresh
     * or by mutating a corpus member — the feedback that makes this "guided".
     * Each iteration adds at most one entry, so `iters` slots suffice. */
    long *corpus = (long *)malloc((iters ? iters : 1) * sizeof *corpus);
    if (corpus == NULL)
        return false;
    size_t ncorpus = 0;
    uint64_t tried = 0;

    for (uint64_t i = 0; i < iters; i++) {
        long in;
        if (ncorpus == 0 || (asmtest_rng_u64(&rng) & 1)) {
            in = asmtest_rng_range(&rng, lo, hi); /* fresh draw */
        } else {
            long base = corpus[asmtest_rng_u64(&rng) % ncorpus];
            if (asmtest_rng_u64(&rng) & 1) {
                in = base ^
                     (1L << (asmtest_rng_u64(&rng) % 62)); /* flip a bit */
            } else {
                /* base + a signed offset overflows (UB) when base sits
                 * within 4 of LONG_MAX/LONG_MIN -- which a fresh draw over a
                 * boundary range (hi == LONG_MAX or lo == LONG_MIN)
                 * guarantees will eventually happen. Mirror the unsigned
                 * idiom asmtest_rng_range itself uses (src/asmtest.c): the
                 * wrap is defined in uint64_t, and the clamp just below
                 * folds any wrapped value back into [lo, hi] -- bit-identical
                 * result to the old signed form for every non-boundary
                 * range. */
                in = (long)((unsigned long)base +
                            (unsigned long)asmtest_rng_range(&rng, -4, 4));
            }
            if (in < lo)
                in = lo;
            if (in > hi)
                in = hi;
        }
        size_t before = uni->blocks_len;
        emu_result_t r;
        long args[1] = {in};
        emu_call_traced(e, code, code_len, args, 1, FUZZ_INSN_CAP, &r, uni);
        tried++;
        if (uni->blocks_len > before) /* this input entered a new block */
            corpus[ncorpus++] = in;
    }

    if (stat != NULL) {
        stat->blocks_reached = uni->blocks_len;
        stat->corpus_len = ncorpus;
        stat->iterations = tried;
    }
    /* Hand the kept inputs to the handle, which owns them until the next fuzz run
     * or emu_close — so the caller can read them back with emu_fuzz_corpus and the
     * stat stays a plain value type (no owned pointer to copy/free). */
    emu_fuzz_set_corpus(e, corpus, ncorpus);
    return true;
}

/* ------------------------------------------------------------------ */
/* Mutation testing                                                    */
/* ------------------------------------------------------------------ */

/* Behavioral fingerprint of one run, for differential comparison. A routine
 * whose result is a function of its argument (the differential-testing premise)
 * yields the same signature run to run for a given input. */
typedef struct {
    bool ok;
    bool faulted;
    uint64_t ret; /* rax */
} run_sig_t;

static run_sig_t run_sig(emu_t *e, const void *code, size_t code_len, long in) {
    emu_result_t r;
    long args[1] = {in};
    emu_call(e, code, code_len, args, 1, FUZZ_INSN_CAP, &r);
    run_sig_t s = {r.ok, r.faulted, r.regs.rax};
    return s;
}

static bool sig_eq(run_sig_t a, run_sig_t b) {
    return a.ok == b.ok && a.faulted == b.faulted && a.ret == b.ret;
}

size_t emu_mutation_test1(emu_t *e, const void *code, size_t code_len,
                          const long *inputs, size_t ninputs,
                          uint64_t max_mutants, uint64_t seed,
                          emu_mutation_stat_t *stat) {
    if (e == NULL || code == NULL || inputs == NULL || ninputs == 0 ||
        code_len == 0)
        return 0;
    run_sig_t *base = (run_sig_t *)malloc(ninputs * sizeof *base);
    uint8_t *mut = (uint8_t *)malloc(code_len);
    if (base == NULL || mut == NULL) {
        free(base);
        free(mut);
        return 0;
    }
    /* The original routine's signature on each input — the oracle every mutant
     * is compared against. */
    for (size_t i = 0; i < ninputs; i++)
        base[i] = run_sig(e, code, code_len, inputs[i]);

    uint64_t total_bits = (uint64_t)code_len * 8;
    uint64_t want = (max_mutants == 0 || max_mutants > total_bits)
                        ? total_bits
                        : max_mutants;
    asmtest_rng_t rng = {seed};
    size_t killed = 0, survived = 0;
    for (uint64_t m = 0; m < want; m++) {
        /* Sweep every bit when running them all; otherwise sample (seeded) so a
         * cap still gives a representative spread. */
        uint64_t bit =
            (want == total_bits) ? m : (asmtest_rng_u64(&rng) % total_bits);
        memcpy(mut, code, code_len);
        mut[bit / 8] ^= (uint8_t)(1u << (bit % 8));

        bool distinguished = false;
        for (size_t i = 0; i < ninputs && !distinguished; i++) {
            run_sig_t s = run_sig(e, mut, code_len, inputs[i]);
            if (!sig_eq(s, base[i]))
                distinguished = true;
        }
        if (distinguished)
            killed++;
        else
            survived++;
    }

    if (stat != NULL) {
        stat->mutants = (size_t)want;
        stat->killed = killed;
        stat->survived = survived;
    }
    free(base);
    free(mut);
    return survived;
}

/* ------------------------------------------------------------------ */
/* Coverage-extraction seam (external-engine harnesses)               */
/* ------------------------------------------------------------------ */

/* One tested seam so every external-engine harness (libFuzzer / AFL shim) runs
 * an input and bumps its own coverage map through ONE place, rather than each
 * re-deriving emu_trace_t handling. The deduped block set is written straight
 * into the caller's buffer (trace.blocks), so there is nothing to copy back; the
 * return is just trace.blocks_len. Every recorded offset is < code_len (offsets
 * are measured from routine entry), so the caller can size an external counter
 * array to code_len and index it directly — no hash. */
size_t emu_cover_hits(emu_t *e, const void *code, size_t code_len,
                      const long *args, int nargs, uint64_t max_insns,
                      uint64_t *block_offs, size_t cap) {
    if (e == NULL || code == NULL || block_offs == NULL)
        return 0;
    emu_trace_t trace;
    memset(&trace, 0, sizeof trace);
    trace.blocks = block_offs;
    trace.blocks_cap = cap;
    emu_result_t r;
    emu_call_traced(e, code, code_len, args, nargs,
                    max_insns ? max_insns : FUZZ_INSN_CAP, &r, &trace);
    return trace.blocks_len;
}
