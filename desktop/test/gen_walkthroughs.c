/*
 * gen_walkthroughs.c — the Learn door's bundled walkthroughs, as recordings
 * (docs/internal/gui/06-doors-and-learning.md T2/T3).
 *
 * A walkthrough is not a document beside a recording — it IS a recording, with
 * ordered `stop:true` notes in it. That is the whole design: the player is a
 * reader of the same format every other view reads, so a walkthrough cannot
 * drift away from the run it narrates, and a stop that points past the recorded
 * window is a thing the player can DETECT rather than something the author has
 * to remember.
 *
 * Every routine here is assembled from the AT&T source embedded below rather
 * than compiled, so the recording is provably of exactly the text the stops
 * talk about — the loud-drop contract (include/asmtest_assemble.h:83) turns a
 * dialect slip into a build failure instead of a walkthrough about code nobody
 * wrote.
 *
 * DETERMINISM (D6). The writer runs in deterministic mode (no ts/pid/cmd), the
 * arguments are fixed, and the event order is fixed, so regenerating is
 * byte-identical — `make asmtrace-walkthroughs` proves it by writing twice and
 * comparing. Keystone is pinned, so "regenerate in-lane" is a real rule and not
 * a hope; regenerate inside `make docker-desktop`.
 *
 * Usage: gen_walkthroughs <output-directory>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asmtest_assemble.h"
#include "asmtest_emu.h"
#include "asmtrace_ndjson.h"

/* ------------------------------------------------------------------ */
/* The stories                                                         */
/* ------------------------------------------------------------------ */

/* One ordered stop. `step_anchor` < 0 is UNANCHORED — a stop about the run as a
 * whole rather than about an instruction. `expected`/`got` are the failure
 * framing; both NULL for an ordinary explanatory stop. */
typedef struct {
    long step_anchor;
    const char *title;
    const char *body;
    const char *expected;
    const char *got;
} stop_t;

/* square(2) — docs/getting-started/quickstart.md:22, and its §5 "see a failure"
 * moment, which is the first thing the quickstart asks a reader to do. */
static const char SQUARE_SRC[] = "movq %rdi, %rax\n"
                                 "imulq %rdi, %rax\n"
                                 "ret\n";
static const stop_t SQUARE_STOPS[] = {
    {0, "the ABI is the contract",
     "SysV: the argument arrives in rdi, the answer must end in rax. Nothing "
     "in the routine says so — the calling convention does, and every "
     "assertion in this framework is ultimately about it.",
     NULL, NULL},
    {1, "the multiply", "rax = rax * rdi. Two reads of the same argument, "
                        "which is what makes this a square rather than a "
                        "product of two inputs.",
     NULL, NULL},
    {2, "return", "ret pops the return address. rax holds the answer; "
                  "everything else is the caller's business.",
     NULL, NULL},
    {-1, "now break it",
     "The quickstart asks you to edit the assertion to expect 5 and re-run, so "
     "you see what a failure looks like before you need to read one in anger. "
     "This is that failure.",
     "5", "4"},
};

/* demo-fail — the emulator retelling of imax_wrong
 * (examples/test_failure_demo.c:11, "returns the minimum (a bug)"). The C demo
 * runs the real routine; this recording is what the GUI deep-links INTO when
 * that demo goes red. */
static const char DEMOFAIL_SRC[] = "movq %rdi, %rax\n"
                                   "cmpq %rsi, %rdi\n"
                                   "cmovgq %rsi, %rax\n"
                                   "ret\n";
static const stop_t DEMOFAIL_STOPS[] = {
    {0, "rax starts as a", "imax(3, 4) should return 4. The routine begins by "
                           "moving the FIRST argument into the return "
                           "register — reasonable so far.",
     NULL, NULL},
    {1, "the compare sets the flags",
     "cmp rdi, rsi computes rdi - rsi and throws the result away, keeping only "
     "the flags. 3 - 4 is negative, so SF != OF and `greater` is false.",
     NULL, NULL},
    {2, "the predicate is inverted",
     "cmovg moves when the compare said GREATER — so when a > b it overwrites "
     "the larger value with the smaller one. The author wanted cmovl (or "
     "wanted the operands the other way round).",
     "rax=4", "rax=3"},
    {3, "this is the failure `make demo-fail` reports",
     "In the GUI a failing test deep-links to exactly this step: the recording "
     "is the evidence, and the step is where the wrong value was made.",
     NULL, NULL},
};

/* ct_eq — the ladder's capstone (T3). Instruction for instruction the x86-64
 * body of examples/ct_eq.s, so the walkthrough narrates the routine the suite
 * tests rather than a lookalike. If you change one, change both.
 *
 * ONE spelling difference, measured not assumed: the shipped .s uses GNU as's
 * NUMERIC LOCAL labels (`jge 2f` / `jmp 1b`), which the pinned Keystone rejects
 * outright ("Invalid label (KS_ERR_ASM_LABEL_INVALID)"). Named labels are the
 * same jumps to the same places; only the label SPELLING differs. */
static const char CT_EQ_SRC[] = "xorl %eax, %eax\n"
                                "xorl %ecx, %ecx\n"
                                "loop:\n"
                                "cmpq %rdx, %rcx\n"
                                "jge done\n"
                                "movzbl (%rdi,%rcx,1), %r8d\n"
                                "movzbl (%rsi,%rcx,1), %r9d\n"
                                "xorl %r9d, %r8d\n"
                                "orl %r8d, %eax\n"
                                "incq %rcx\n"
                                "jmp loop\n"
                                "done:\n"
                                "cmpl $1, %eax\n"
                                "sbbl %eax, %eax\n"
                                "negl %eax\n"
                                "cltq\n"
                                "ret\n";

/* The negative control, matching examples/ct_eq.s's leaky_eq: the naive
 * early-exit compare, whose exit branch depends on WHERE the buffers differ. */
static const char LEAKY_EQ_SRC[] = "xorl %ecx, %ecx\n"
                                   "loop:\n"
                                   "cmpq %rdx, %rcx\n"
                                   "jge equal\n"
                                   "movzbl (%rdi,%rcx,1), %r8d\n"
                                   "movzbl (%rsi,%rcx,1), %r9d\n"
                                   "cmpl %r9d, %r8d\n"
                                   "jne differ\n"
                                   "incq %rcx\n"
                                   "jmp loop\n"
                                   "differ:\n"
                                   "xorl %eax, %eax\n"
                                   "ret\n"
                                   "equal:\n"
                                   "movl $1, %eax\n"
                                   "ret\n";

static const stop_t CT_EQ_STOPS[] = {
    {0, "XOR-accumulate, never compare",
     "acc = 0, then acc |= a[i] ^ b[i] for every byte. Equal bytes contribute "
     "zero; a difference sets a bit and can never be cleared. No byte's value "
     "changes what the code does next — only what acc holds.",
     NULL, NULL},
    {2, "the loop branch depends on the PUBLIC length",
     "cmp rcx, rdx compares the index against n. n is public — it is the "
     "buffer size, not the secret — so branching on it leaks nothing about "
     "the contents.",
     NULL, NULL},
    {-1, "the branchless collapse",
     "cmp $1, acc sets the carry flag iff acc == 0; sbb acc, acc turns that "
     "into 0 or -1; neg makes it 1 or 0. A conditional jump here would be a "
     "branch on the secret, which is exactly what the routine exists to avoid.",
     NULL, NULL},
    {-1, "the coverage union does not grow",
     "This recording is the union of three runs: equal, differing at the first "
     "byte, and differing at the last. The block set after run 1 is the same "
     "block set after runs 2 and 3 — no basic block depended on the secret.",
     "no new blocks", "no new blocks"},
    {-1, "and the control shows the assertion has teeth",
     "leaky_eq — the naive early-exit compare — run over the same three inputs "
     "DOES add a block, at its early-exit offset. That is a constant-time "
     "violation, and it is what proves the check above is measuring something.",
     "no new blocks", "+1 block at the early-exit offset"},
};

typedef struct {
    const char *name; /* output basename, without .asmtrace */
    const char *title;
    const char *src;
    long args[3];
    int nargs;
    const stop_t *stops;
    int nstops;
    int truncate_at; /* >0: cap the recorded steps here (the D7 fixture) */
} walkthrough_t;

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

/* Emit one stop as the schema's `note` kind: text/off/step/stop/title/expected/
 * got, in that order, with every absent field OMITTED. */
static void emit_stop(asmtrace_writer_t *w, const stop_t *s) {
    char body[4096], esc[1024];
    size_t n = 0;

    asmtrace_escape(esc, sizeof esc, s->body);
    n = (size_t)snprintf(body, sizeof body, "\"text\":\"%s\"", esc);
    if (s->step_anchor >= 0)
        n += (size_t)snprintf(body + n, sizeof body - n, ",\"step\":%ld",
                              s->step_anchor);
    n += (size_t)snprintf(body + n, sizeof body - n, ",\"stop\":true");
    if (s->title != NULL) {
        asmtrace_escape(esc, sizeof esc, s->title);
        n += (size_t)snprintf(body + n, sizeof body - n, ",\"title\":\"%s\"",
                              esc);
    }
    if (s->expected != NULL) {
        asmtrace_escape(esc, sizeof esc, s->expected);
        n += (size_t)snprintf(body + n, sizeof body - n, ",\"expected\":\"%s\"",
                              esc);
    }
    if (s->got != NULL) {
        asmtrace_escape(esc, sizeof esc, s->got);
        snprintf(body + n, sizeof body - n, ",\"got\":\"%s\"", esc);
    }
    asmtrace_emit(w, "note", body);
}

/* The x86-64 register deck, as a `regstate` event: the state the run ENDED in.
 * `desc` is a reference to a descriptor, never an inline register list — that
 * is what lets one viewer render a deck it was not compiled against. */
static void emit_regstate(asmtrace_writer_t *w, const emu_result_t *r) {
    char body[1024];
    snprintf(body, sizeof body,
             "\"desc\":\"emu_x86_regs_t@x86_64/sysv\",\"values\":{"
             "\"rax\":%llu,\"rbx\":%llu,\"rcx\":%llu,\"rdx\":%llu,"
             "\"rsi\":%llu,\"rdi\":%llu}",
             (unsigned long long)r->regs.rax, (unsigned long long)r->regs.rbx,
             (unsigned long long)r->regs.rcx, (unsigned long long)r->regs.rdx,
             (unsigned long long)r->regs.rsi, (unsigned long long)r->regs.rdi);
    asmtrace_emit(w, "regstate", body);
}

/* Run one walkthrough and write it. Returns 0 / -1. */
static int gen_one(const char *dir, const walkthrough_t *wt) {
    asmtrace_prov_t prov = {"emu-l0", 1, "exact", 0, NULL, 0};
    asmtrace_writer_t w;
    asm_result_t asmr;
    emu_result_t res;
    asmtest_trace_t tr;
    uint64_t insns[4096], blocks[256];
    char path[1024], body[8192], dis[160];
    emu_t *e = NULL;
    int rc = -1;

    memset(&asmr, 0, sizeof asmr);
    if (!asmtest_assemble(ASM_X86_64, ASM_SYNTAX_ATT, wt->src, EMU_CODE_BASE,
                          &asmr)) {
        /* The loud-drop contract at work: a dialect slip or a dropped statement
         * fails the GENERATOR rather than producing a walkthrough about code
         * nobody wrote. */
        fprintf(stderr, "gen_walkthroughs: %s: %s\n", wt->name, asmr.err);
        asmtest_asm_free(&asmr);
        return -1;
    }

    e = emu_open();
    if (e == NULL) {
        fprintf(stderr, "gen_walkthroughs: emu_open failed\n");
        asmtest_asm_free(&asmr);
        return -1;
    }

    memset(&tr, 0, sizeof tr);
    tr.insns = insns;
    tr.insns_cap = sizeof insns / sizeof insns[0];
    tr.blocks = blocks;
    tr.blocks_cap = sizeof blocks / sizeof blocks[0];
    memset(&res, 0, sizeof res);
    emu_call_traced(e, asmr.bytes, asmr.len, wt->args, wt->nargs, 0, &res, &tr);

    snprintf(path, sizeof path, "%s/%s.asmtrace", dir, wt->name);
    if (asmtrace_open(&w, path, 1 /* deterministic */) != 0) {
        fprintf(stderr, "gen_walkthroughs: cannot write %s\n", path);
        goto out;
    }
    asmtrace_header(&w, "asmtrace_record", &prov, 0, NULL);

    asmtrace_escape(body, sizeof body, wt->title);
    asmtrace_emitf(&w, "note", "\"text\":\"%s\"", body);

    /* The recorded window. `truncate_at` is how the D7 dishonesty fixture is
     * MADE rather than hand-edited: the recording really does stop early, and
     * the footer really does declare it. */
    size_t nsteps = tr.insns_len;
    if (wt->truncate_at > 0 && (size_t)wt->truncate_at < nsteps)
        nsteps = (size_t)wt->truncate_at;
    for (size_t i = 0; i < nsteps; i++) {
        dis[0] = '\0';
        if (emu_disas_available())
            emu_disas(EMU_ARCH_X86_64, asmr.bytes, asmr.len, EMU_CODE_BASE,
                      insns[i], dis, sizeof dis);
        if (dis[0]) {
            asmtrace_escape(body, sizeof body, dis);
            asmtrace_emitf(&w, "trace",
                           "\"basis\":\"rel\",\"kind\":\"insn\",\"off\":%llu,"
                           "\"disasm\":\"%s\"",
                           (unsigned long long)insns[i], body);
        } else {
            asmtrace_emitf(&w, "trace",
                           "\"basis\":\"rel\",\"kind\":\"insn\",\"off\":%llu",
                           (unsigned long long)insns[i]);
        }
    }
    {
        int n = snprintf(body, sizeof body, "\"basis\":\"rel\",\"blocks\":[");
        for (size_t i = 0; i < tr.blocks_len && n > 0 && (size_t)n < sizeof body;
             i++)
            n += snprintf(body + n, sizeof body - (size_t)n, "%s%llu",
                          i ? "," : "", (unsigned long long)tr.blocks[i]);
        if (n > 0 && (size_t)n < sizeof body)
            snprintf(body + n, sizeof body - (size_t)n,
                     "],\"blocks_total\":%llu,\"insns_total\":%llu,"
                     "\"truncated\":%s",
                     (unsigned long long)tr.blocks_total,
                     (unsigned long long)tr.insns_total,
                     (tr.truncated || nsteps < tr.insns_len) ? "true"
                                                             : "false");
        asmtrace_emit(&w, "coverage", body);
    }
    emit_regstate(&w, &res);

    /* Stops last, so file order IS ordinal order — the player's contract. */
    for (int i = 0; i < wt->nstops; i++)
        emit_stop(&w, &wt->stops[i]);

    if (tr.truncated || nsteps < tr.insns_len)
        w.truncated = 1;
    asmtrace_close(&w, 0, 0, NULL);
    printf("  %-20s %zu step(s), %d stop(s)%s\n", wt->name, nsteps, wt->nstops,
           (nsteps < tr.insns_len) ? "  [TRUNCATED fixture]" : "");
    rc = 0;
out:
    emu_close(e);
    asmtest_asm_free(&asmr);
    return rc;
}

/* ------------------------------------------------------------------ */
/* ct_eq: three runs into ONE accumulating trace                       */
/* ------------------------------------------------------------------ */

#define CT_DATA 0x00300000UL /* NOT 0x200000 — that is the emulator's stack */
#define CT_SIZE 0x1000
#define CT_A (CT_DATA + 0x000)
#define CT_B (CT_DATA + 0x100)
#define CT_LEN 16

/* Run `code` over the three secret-differing variants into ONE accumulating
 * trace. Returns the block-set size after run 3; *baseline gets it after run 1.
 * The union IS the constant-time oracle. */
static void ct_union(emu_t *e, const uint8_t *code, size_t len,
                     asmtest_trace_t *tr, emu_result_t *res, size_t *baseline,
                     size_t *after) {
    for (int variant = 0; variant < 3; variant++) {
        unsigned char a[CT_LEN], b[CT_LEN];
        long args[3] = {(long)CT_A, (long)CT_B, CT_LEN};
        memset(a, 0xa5, sizeof a);
        memcpy(b, a, sizeof b);
        if (variant == 1)
            b[0] ^= 0x01;
        else if (variant == 2)
            b[CT_LEN - 1] ^= 0x80;
        emu_write(e, CT_A, a, sizeof a);
        emu_write(e, CT_B, b, sizeof b);
        emu_call_traced(e, code, len, args, 3, 0, res, tr);
        if (variant == 0)
            *baseline = tr->blocks_len;
    }
    *after = tr->blocks_len;
}

static int gen_ct_eq(const char *dir) {
    asmtrace_prov_t prov = {"emu-l0", 1, "exact", 0, NULL, 0};
    asmtrace_writer_t w;
    asm_result_t asmr;
    emu_result_t res;
    asmtest_trace_t tr;
    uint64_t insns[8192], blocks[256];
    char path[1024], body[16384], dis[160];
    emu_t *e = NULL;
    size_t baseline = 0, after = 0, leak_baseline = 0, leak_after = 0;
    int rc = -1;

    memset(&asmr, 0, sizeof asmr);
    if (!asmtest_assemble(ASM_X86_64, ASM_SYNTAX_ATT, CT_EQ_SRC, EMU_CODE_BASE,
                          &asmr)) {
        fprintf(stderr, "gen_walkthroughs: ct_eq: %s\n", asmr.err);
        asmtest_asm_free(&asmr);
        return -1;
    }
    e = emu_open();
    if (e == NULL || !emu_map(e, CT_DATA, CT_SIZE)) {
        fprintf(stderr, "gen_walkthroughs: ct_eq: cannot map the data page\n");
        goto out;
    }

    memset(&tr, 0, sizeof tr);
    tr.insns = insns;
    tr.insns_cap = sizeof insns / sizeof insns[0];
    tr.blocks = blocks;
    tr.blocks_cap = sizeof blocks / sizeof blocks[0];
    memset(&res, 0, sizeof res);

    /* Three secret-differing inputs into ONE trace: emu_call_traced ACCUMULATES
     * (include/asmtest_emu.h:172), which is what makes the block set a union. */
    ct_union(e, asmr.bytes, asmr.len, &tr, &res, &baseline, &after);

    /* The CONTROL, measured in the same run rather than asserted in prose: the
     * same three inputs through leaky_eq. Its blocks go into their OWN trace and
     * are deliberately NOT merged into this recording's coverage — they belong
     * to a different routine, and a union across two routines would mean
     * nothing. Only its NUMBERS reach the closing stop. */
    {
        asm_result_t lk;
        memset(&lk, 0, sizeof lk);
        if (asmtest_assemble(ASM_X86_64, ASM_SYNTAX_ATT, LEAKY_EQ_SRC,
                             EMU_CODE_BASE, &lk)) {
            uint64_t lblocks[256];
            asmtest_trace_t ltr;
            emu_result_t lres;
            memset(&ltr, 0, sizeof ltr);
            ltr.blocks = lblocks;
            ltr.blocks_cap = sizeof lblocks / sizeof lblocks[0];
            memset(&lres, 0, sizeof lres);
            ct_union(e, lk.bytes, lk.len, &ltr, &lres, &leak_baseline,
                     &leak_after);
        } else {
            fprintf(stderr, "gen_walkthroughs: leaky_eq: %s\n", lk.err);
        }
        asmtest_asm_free(&lk);
    }

    snprintf(path, sizeof path, "%s/ct_eq.asmtrace", dir);
    if (asmtrace_open(&w, path, 1) != 0) {
        fprintf(stderr, "gen_walkthroughs: cannot write %s\n", path);
        goto out;
    }
    asmtrace_header(&w, "asmtrace_record", &prov, 0, NULL);
    asmtrace_emitf(&w, "note",
                   "\"text\":\"ct_eq: three secret-differing inputs into one "
                   "accumulating trace — the coverage union is the "
                   "constant-time oracle\"");

    for (size_t i = 0; i < tr.insns_len; i++) {
        dis[0] = '\0';
        if (emu_disas_available())
            emu_disas(EMU_ARCH_X86_64, asmr.bytes, asmr.len, EMU_CODE_BASE,
                      insns[i], dis, sizeof dis);
        if (dis[0]) {
            asmtrace_escape(body, sizeof body, dis);
            asmtrace_emitf(&w, "trace",
                           "\"basis\":\"rel\",\"kind\":\"insn\",\"off\":%llu,"
                           "\"disasm\":\"%s\"",
                           (unsigned long long)insns[i], body);
        } else {
            asmtrace_emitf(&w, "trace",
                           "\"basis\":\"rel\",\"kind\":\"insn\",\"off\":%llu",
                           (unsigned long long)insns[i]);
        }
    }
    {
        int n = snprintf(body, sizeof body, "\"basis\":\"rel\",\"blocks\":[");
        for (size_t i = 0; i < tr.blocks_len && n > 0 && (size_t)n < sizeof body;
             i++)
            n += snprintf(body + n, sizeof body - (size_t)n, "%s%llu",
                          i ? "," : "", (unsigned long long)tr.blocks[i]);
        if (n > 0 && (size_t)n < sizeof body)
            snprintf(body + n, sizeof body - (size_t)n,
                     "],\"blocks_total\":%llu,\"insns_total\":%llu,"
                     "\"truncated\":%s",
                     (unsigned long long)tr.blocks_total,
                     (unsigned long long)tr.insns_total,
                     tr.truncated ? "true" : "false");
        asmtrace_emit(&w, "coverage", body);
    }
    emit_regstate(&w, &res);

    /* The coverage stop carries the MEASURED numbers, so a reader can check the
     * claim against the `coverage` event above rather than trusting the prose. */
    for (int i = 0; i < (int)(sizeof CT_EQ_STOPS / sizeof CT_EQ_STOPS[0]); i++) {
        stop_t s = CT_EQ_STOPS[i];
        char want[64], got[64];
        /* expected/got are written so a reader COMPARES them: identical text
         * means the union did not grow, different text means it did. Prose that
         * agreed while the numbers differed would defeat the whole framing. */
        if (i == 3) { /* the CT verdict */
            snprintf(want, sizeof want, "%zu blocks", baseline);
            snprintf(got, sizeof got, "%zu blocks", after);
            s.expected = want;
            s.got = got;
        } else if (i == 4) { /* the control's verdict, measured above */
            snprintf(want, sizeof want, "%zu blocks", leak_baseline);
            snprintf(got, sizeof got, "%zu blocks (+%zu at the early exit)",
                     leak_after, leak_after - leak_baseline);
            s.expected = want;
            s.got = got;
        }
        emit_stop(&w, &s);
    }
    asmtrace_close(&w, 0, 0, NULL);
    printf("  %-20s %zu step(s), blocks %zu -> %zu; control %zu -> %zu\n",
           "ct_eq", tr.insns_len, baseline, after, leak_baseline, leak_after);
    rc = 0;
out:
    emu_close(e);
    asmtest_asm_free(&asmr);
    return rc;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    const char *dir = argc >= 2 ? argv[1]
                                : "tests/golden-asmtrace/walkthroughs";
    int failed = 0;

    static const walkthrough_t WALKTHROUGHS[] = {
        {"square", "square(2) — the quickstart routine, and its first failure",
         SQUARE_SRC, {2, 0, 0}, 1, SQUARE_STOPS,
         (int)(sizeof SQUARE_STOPS / sizeof SQUARE_STOPS[0]), 0},
        {"demo-fail",
         "imax_wrong(3, 4) — why is rax wrong? (`make demo-fail`'s story)",
         DEMOFAIL_SRC, {3, 4, 0}, 2, DEMOFAIL_STOPS,
         (int)(sizeof DEMOFAIL_STOPS / sizeof DEMOFAIL_STOPS[0]), 0},
        /* The D7 dishonesty fixture: the SAME square run, recorded with the
         * window cut short. Its last stop is anchored at step 2, which is now
         * BEYOND what was recorded — the player must say so and refuse to
         * navigate, never silently clamp to the last recorded step. */
        {"square-truncated",
         "square(2) — recorded with the window cut short (dishonesty fixture)",
         SQUARE_SRC, {2, 0, 0}, 1, SQUARE_STOPS,
         (int)(sizeof SQUARE_STOPS / sizeof SQUARE_STOPS[0]), 2},
    };

    if (!emu_disas_available())
        fprintf(stderr,
                "gen_walkthroughs: WARNING — no Capstone, so `disasm` fields "
                "are omitted (D10 degradation). These bytes are NOT the "
                "committed goldens; regenerate in the docker-desktop image.\n");
    printf("gen_walkthroughs -> %s\n", dir);
    for (size_t i = 0; i < sizeof WALKTHROUGHS / sizeof WALKTHROUGHS[0]; i++)
        if (gen_one(dir, &WALKTHROUGHS[i]) != 0)
            failed++;
    if (gen_ct_eq(dir) != 0)
        failed++;

    if (failed) {
        fprintf(stderr, "gen_walkthroughs: %d walkthrough(s) failed\n", failed);
        return 1;
    }
    return 0;
}
