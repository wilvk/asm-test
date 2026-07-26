/*
 * asmtrace_record.c — the Author-mode conformance-corpus recorder.
 *
 * Runs each routine of the cross-language conformance corpus under the
 * DETERMINISTIC emulator L0 value producer and writes one `.asmtrace`
 * recording per routine: `trace` (the ordered executed-instruction stream, the
 * trace canvas's input), `df_step` (per executed step, with its operand
 * read/write values and — where Capstone is linked — its disassembly, D10)
 * plus `df_edge` (the L1 last-writer def-use graph).
 *
 * With `--steps=<cap>` (default 0 = off) it ALSO re-runs each routine under the
 * emu_t per-step register ring (09-teaching-producers.md T2) and emits one
 * `regstate` event per held pre-state — the full x86-64 register file BEFORE
 * each instruction, by descriptor reference (`emu_x86_regs_t@x86_64/sysv`), the
 * per-step producer the teaching scrubber and ABI x-ray replay from. When more
 * steps run than the ring holds, the earliest are evicted and the footer says
 * so (`truncated` + `drops.lost`): an over-cap deck is honest data, never a
 * silent short one.
 *
 * Deterministic by construction, which is what makes the output a GOLDEN
 * corpus rather than a sample: asmtest_dataflow_emu_run zeroes the guest GP
 * file and maps at fixed bases, emu_call likewise zeroes the register file and
 * bases the stack, the argument table below is fixed, and the writer runs in
 * deterministic mode (no ts/pid/cmd). Regenerating must be byte-identical — see
 * `make asmtrace-golden-check`.
 *
 * Contract: docs/internal/gui/asmtrace-schema.md.
 * Usage: asmtrace_record [--steps=<cap>] <output-directory>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asmtest_emu.h"      /* emu_disas (D10) */
#include "asmtest_valtrace.h" /* the L0 sink + L1 def-use graph */
#include "asmtrace_ndjson.h"  /* the shared writer (field order lives there) */

/* The emulator L0 producer. It is a TIER producer, not part of the pure public
 * sink surface, so it ships no public header and consumers re-declare it (the
 * examples/test_dataflow_emu.c precedent). */
int asmtest_dataflow_emu_run(const uint8_t *code, size_t code_len,
                             const long *args, int nargs, uint64_t max_insns,
                             asmtest_valtrace_t *vt);

/* name -> routine pointer, from the conformance corpus. */
void *asmtest_corpus_routine(const char *name);

/* The address the producer maps the routine bytes at (src/dataflow_emu.c), so
 * PC-relative operands disassemble to the same absolute targets the run saw. */
#define REC_CODE_BASE 0x00100000UL

/* The conformance emulator convention: a corpus routine is captured as a fixed
 * 64-byte window from its entry (bindings/conformance/conformance.c). */
#define REC_WINDOW 64

/* --steps=<cap>: the default per-step register-ring cap for the corpus loop
 * (0 = off, the golden default). Set once from argv; a routine's own steps_cap
 * below applies when this is 0, so `make asmtrace-golden` (no flag) still emits
 * the one worked example while an explicit --steps=N arms the whole corpus. */
static size_t g_steps_cap = 0;

/* Fixed routines and arguments. INTEGER-ARG routines only — the emulator L0
 * producer marshals integer arguments (rdi/rsi/rdx/rcx/r8/r9); widening the
 * corpus to FP/vector routines needs a producer change, not a table entry.
 * `steps_cap` is the per-step register-ring cap baked into the golden for that
 * routine (0 = no regstate); add_signed carries the worked example so the
 * committed corpus exercises the `regstate` kind without --steps. */
typedef struct {
    const char *name;
    long args[3];
    int nargs;
    size_t steps_cap;
} rec_routine_t;

static const rec_routine_t ROUTINES[] = {
    {"add_signed", {40, 2, 0}, 2, 8},   {"sum_via_rbx", {40, 2, 0}, 2, 0},
    {"clobbers_rbx", {40, 2, 0}, 2, 0}, {"sum3", {1, 2, 3}, 3, 0},
    {"set_carry", {0, 0, 0}, 0, 0},     {"clear_carry", {0, 0, 0}, 0, 0},
};
#define N_ROUTINES ((int)(sizeof ROUTINES / sizeof ROUTINES[0]))

/* ------------------------------------------------------------------ */
/* The Loom's golden fixtures (docs/internal/gui/05-loom-day-one.md T7) */
/*                                                                     */
/* Unlike the corpus routines above these are BYTE LITERALS, because    */
/* the two walkthroughs the doc builds on are hand-derivable on paper   */
/* and must stay so: their listings are right here beside the bytes.    */
/* ------------------------------------------------------------------ */

/* examples/test_dataflow_emu.c:38 — the def-use chain the whole doc walks:
 *   0x00 mov rax, rdi          0x03 mov [rsp-8], rax     0x08 mov rcx, [rsp-8]
 *   0x0d lea rdx, [rcx+rsi]    0x11 mov rax, rdx         0x14 ret            */
static const uint8_t LOOM_DF_CHAIN[] = {
    0x48, 0x89, 0xf8, 0x48, 0x89, 0x44, 0x24, 0xf8, 0x48, 0x8b, 0x4c,
    0x24, 0xf8, 0x48, 0x8d, 0x14, 0x31, 0x48, 0x89, 0xd0, 0xc3,
};

/* fork_demo(a)  [rdi=a] — one dimmed, one hot, one control divergence:
 *   0x00  mov rdx, rdi      ; hot: the value differs across takes
 *   0x03  shr rdx, 63       ; DIMMED: 0 for both non-negative args
 *   0x07  mov rax, rdi      ; hot
 *   0x0a  cmp rax, 10       ; hot (EFLAGS value differs)
 *   0x0e  jle 0x13          ; last aligned step
 *   0x10  neg rax           ; runs only when a > 10
 *   0x13  ret
 * desktop/test/test_loom_forks.cpp assembles this listing with Keystone and
 * asserts the result is byte-identical to the table below — so the "hand-verify
 * the encodings once" step is a test rather than a promise. */
static const uint8_t LOOM_FORK_DEMO[] = {
    0x48, 0x89, 0xfa,       /* 0x00 mov rdx, rdi  */
    0x48, 0xc1, 0xea, 0x3f, /* 0x03 shr rdx, 63   */
    0x48, 0x89, 0xf8,       /* 0x07 mov rax, rdi  */
    0x48, 0x83, 0xf8, 0x0a, /* 0x0a cmp rax, 10   */
    0x7e, 0x03,             /* 0x0e jle 0x13      */
    0x48, 0xf7, 0xd8,       /* 0x10 neg rax       */
    0xc3,                   /* 0x13 ret           */
};

/* One per-step register snapshot as a `regstate` event, by descriptor
 * reference (schema: `desc` names a state descriptor, never a bare inline
 * register list). The `values` object carries the full x86-64 INTEGER file —
 * the 16 GP registers plus rip and rflags, in emu_x86_regs_t declaration order,
 * each a decimal u64. The 128-bit XMM file is a documented v1 omission: a wide
 * value is not a bare JSON integer (exactly the df_step `wide` limit), and the
 * descriptor mechanism absorbs an FP/vector deck later. See the descriptor
 * entry appended to docs/internal/gui/asmtrace-schema.md. */
static void emit_regstate(asmtrace_writer_t *w, const emu_x86_regs_t *r) {
    char body[512];
    snprintf(body, sizeof body,
             "\"desc\":\"emu_x86_regs_t@x86_64/sysv\",\"values\":{"
             "\"rax\":%llu,\"rbx\":%llu,\"rcx\":%llu,\"rdx\":%llu,"
             "\"rsi\":%llu,\"rdi\":%llu,\"rbp\":%llu,\"rsp\":%llu,"
             "\"r8\":%llu,\"r9\":%llu,\"r10\":%llu,\"r11\":%llu,"
             "\"r12\":%llu,\"r13\":%llu,\"r14\":%llu,\"r15\":%llu,"
             "\"rip\":%llu,\"rflags\":%llu}",
             (unsigned long long)r->rax, (unsigned long long)r->rbx,
             (unsigned long long)r->rcx, (unsigned long long)r->rdx,
             (unsigned long long)r->rsi, (unsigned long long)r->rdi,
             (unsigned long long)r->rbp, (unsigned long long)r->rsp,
             (unsigned long long)r->r8, (unsigned long long)r->r9,
             (unsigned long long)r->r10, (unsigned long long)r->r11,
             (unsigned long long)r->r12, (unsigned long long)r->r13,
             (unsigned long long)r->r14, (unsigned long long)r->r15,
             (unsigned long long)r->rip, (unsigned long long)r->rflags);
    asmtrace_emit(w, "regstate", body);
}

/* Per-step register ring (09-teaching-producers.md T2). When steps_cap > 0,
 * re-run the SAME bytes under the emu_t handle with an armed capture ring and
 * emit one `regstate` event per held pre-state (oldest first). Returns the
 * number of earliest steps the ring EVICTED (emu_step_dropped): the caller
 * folds a non-zero count into the footer as truncation, so an over-cap run is
 * honest data — the held deck is steps [dropped, dropped+count) and the footer
 * says how many fell off the front. The ring is x86-64-guest only, exactly the
 * corpus's own arch gate; where emu_step_capture cannot arm (non-x86-64 or an
 * allocation failure) nothing is written and 0 is returned. */
static uint64_t emit_regstates(asmtrace_writer_t *w, const uint8_t *code,
                               size_t code_len, const long *args, int nargs,
                               size_t steps_cap) {
    emu_t *e;
    emu_result_t res;
    uint64_t dropped = 0;
    if (steps_cap == 0)
        return 0;
    e = emu_open();
    if (e == NULL)
        return 0;
    if (emu_step_capture(e, steps_cap)) {
        size_t held, i;
        memset(&res, 0, sizeof res);
        emu_call(e, code, code_len, args, nargs, 0, &res);
        held = emu_step_count(e);
        for (i = 0; i < held; i++) {
            emu_x86_regs_t regs;
            if (emu_step_at(e, i, NULL, &regs))
                emit_regstate(w, &regs);
        }
        dropped = emu_step_dropped(e);
    }
    emu_close(e);
    return dropped;
}

/*
 * Record `code[0..code_len)` under the producer and write <dir>/<out>.asmtrace.
 * `label` is what the recording's `note` calls the routine; `recs_cap` bounds
 * the operand buffer (a SMALL cap is how the truncated loom fixture is made —
 * the producer then flips `truncated` and the footer says so, which is a D7
 * dishonesty fixture rather than a hand-edited file). `steps_cap` arms the
 * per-step register ring (0 = no regstate); a small cap over a longer routine
 * is how the register-ring truncation fixture is made, the same way.
 * Returns 0 on success, -1 on a setup/write failure.
 */
static int record_bytes(const char *dir, const char *out, const char *label,
                        const uint8_t *code, size_t code_len, const long *args,
                        int nargs, size_t recs_cap, size_t steps_cap) {
    asmtrace_prov_t prov = {"emu-l0", 1, "exact", 0, NULL, 0};
    asmtrace_writer_t w;
    asmtest_valtrace_t *vt = NULL;
    asmtest_defuse_t *g = NULL;
    char path[1024], body[65536];
    size_t nsteps, nrecs, cur = 0;
    uint64_t step_dropped = 0;
    int rc;

    vt = asmtest_valtrace_new(4096, recs_cap, 4096);
    if (!vt) {
        fprintf(stderr, "asmtrace_record: out of memory\n");
        return -1;
    }
    rc = asmtest_dataflow_emu_run(code, code_len, args, nargs, 0, vt);
    if (rc < 0) {
        /* The producer could not even set up (no Unicorn at run time). That is
         * a lane failure, not a recording: fail loudly rather than commit an
         * empty golden file. */
        fprintf(stderr, "asmtrace_record: emulator producer failed for %s\n",
                out);
        asmtest_valtrace_free(vt);
        return -1;
    }
    g = asmtest_defuse_build(vt);

    snprintf(path, sizeof path, "%s/%s.asmtrace", dir, out);
    if (asmtrace_open(&w, path, 1 /* deterministic */) != 0) {
        fprintf(stderr, "asmtrace_record: cannot write %s\n", path);
        asmtest_defuse_free(g);
        asmtest_valtrace_free(vt);
        return -1;
    }
    asmtrace_header(&w, "asmtrace_record", &prov, 0, NULL);

    /* A note naming the routine and its arguments, so the recording explains
     * itself without a sidecar: a walkthrough is a recording (schema `note`). */
    {
        char text[256];
        int o = snprintf(text, sizeof text, "%s(", label);
        for (int i = 0; i < nargs; i++)
            o += snprintf(text + o, sizeof text - (size_t)o, "%s%ld",
                          i ? ", " : "", args[i]);
        snprintf(text + o, sizeof text - (size_t)o,
                 ") under the deterministic emulator L0 producer");
        asmtrace_escape(body, sizeof body, text);
        asmtrace_emitf(&w, "note", "\"text\":\"%s\"", body);
    }

    nsteps = vt->steps_len;
    nrecs = vt->recs_len;

    /* The ordered executed-instruction stream, as `trace` events. This is the
     * SAME measurement the df_step events carry (vt->insn_off[]), written in
     * the kind the trace canvas reads — so the golden corpus feeds the viewer's
     * heat map with real recorded data rather than a hand-authored imitation.
     *
     * basis is "rel": the producer maps the routine's 64-byte window at a fixed
     * base and these offsets are relative to its entry (the asmtest_trace_t
     * contract, include/asmtest_trace.h:41).
     *
     * There is deliberately NO `coverage` event. The L0 value producer records
     * executed STEPS, not basic blocks, and block starts cannot be recovered
     * from an offset stream without instruction lengths — reconstructing them
     * would be a guess wearing a measurement's clothes. A producer that
     * measures blocks writes the kind; this one does not, so it stays silent. */
    for (size_t s = 0; s < nsteps; s++) {
        char dis[160] = "";
        if (emu_disas_available())
            emu_disas(EMU_ARCH_X86_64, code, code_len, REC_CODE_BASE,
                      vt->insn_off[s], dis, sizeof dis);
        if (dis[0]) {
            asmtrace_escape(body, sizeof body, dis);
            asmtrace_emitf(&w, "trace",
                           "\"basis\":\"rel\",\"kind\":\"insn\",\"off\":%llu,"
                           "\"disasm\":\"%s\"",
                           (unsigned long long)vt->insn_off[s], body);
        } else {
            asmtrace_emitf(&w, "trace",
                           "\"basis\":\"rel\",\"kind\":\"insn\",\"off\":%llu",
                           (unsigned long long)vt->insn_off[s]);
        }
    }

    for (size_t s = 0; s < nsteps; s++) {
        char dis[160] = "";
        size_t first;
        if (emu_disas_available())
            emu_disas(EMU_ARCH_X86_64, code, code_len, REC_CODE_BASE,
                      vt->insn_off[s], dis, sizeof dis);
        while (cur < nrecs && vt->recs[cur].step < s)
            cur++;
        first = cur;
        while (cur < nrecs && vt->recs[cur].step == s)
            cur++;
        asmtrace_df_step_body(body, sizeof body, (unsigned)s, vt->insn_off[s],
                              dis, &vt->recs[first], cur - first);
        asmtrace_emit(&w, "df_step", body);
    }
    for (size_t i = 0; g && i < g->n; i++) {
        asmtrace_df_edge_body(body, sizeof body, &g->edges[i]);
        asmtrace_emit(&w, "df_edge", body);
    }

    /* Per-step register states from the ring (T2), after the value stream.
     * A non-zero eviction count is truncation just like a full recs buffer. */
    step_dropped = emit_regstates(&w, code, code_len, args, nargs, steps_cap);
    if (vt->truncated || step_dropped > 0)
        w.truncated = 1;

    /* rc == 1 means the guest faulted or errored: a PARTIAL trace was still
     * produced, so the recording is real — but it must say so rather than look
     * like a clean run. */
    if (rc > 0) {
        asmtrace_prov_t skip = {"emu-l0",
                                1,
                                "exact",
                                1,
                                "guest faulted or errored; trace is partial",
                                0};
        asmtrace_close(&w, step_dropped, 0, &skip);
    } else {
        asmtrace_close(&w, step_dropped, 0, NULL);
    }

    {
        size_t nedges = g ? g->n : (size_t)0; /* read BEFORE the free */
        int trunc = vt->truncated || step_dropped > 0;
        asmtest_defuse_free(g);
        asmtest_valtrace_free(vt);
        printf("  %-20s %zu steps, %zu records, %zu def-use edges", out, nsteps,
               nrecs, nedges);
        if (steps_cap > 0)
            printf(", regstate cap %zu (dropped %llu)", steps_cap,
                   (unsigned long long)step_dropped);
        printf("%s\n", trunc ? "  [TRUNCATED]" : "");
    }
    return 0;
}

/* Record one CORPUS routine (host-arch assembly, captured as a fixed window).
 * An explicit --steps=N (g_steps_cap) arms the whole corpus; otherwise the
 * routine's own baked-in steps_cap applies (add_signed's worked example). */
static int record_one(const char *dir, const rec_routine_t *r) {
    uint8_t code[REC_WINDOW];
    size_t steps_cap = g_steps_cap ? g_steps_cap : r->steps_cap;
    const void *fn = asmtest_corpus_routine(r->name);
    if (!fn) {
        fprintf(stderr, "asmtrace_record: no corpus routine '%s'\n", r->name);
        return -1;
    }
    memcpy(code, fn, sizeof code);
    return record_bytes(dir, r->name, r->name, code, sizeof code, r->args,
                        r->nargs, 65536, steps_cap);
}

/* ------------------------------------------------------------------ */
/* The ABI x-ray's paired goldens (09-teaching-producers.md T4)        */
/*                                                                     */
/* The classroom flagship records the SAME corpus routine twice — once  */
/* through emu_call_traced (System V) and once through                  */
/* emu_call_win64_traced (Microsoft x64) — each under the T1 per-step   */
/* register ring, so the two recordings are a step-aligned PAIR whose    */
/* register files diverge exactly where the two conventions marshal      */
/* arguments differently. The desktop ABI x-ray view (abixray.cpp) locks  */
/* one playhead across both and an authored walkthrough (the `note` stops */
/* below, carried in the SysV/reference leg) narrates the contrast the     */
/* register deltas show. Unlike the corpus loop these run emu_call_*_traced */
/* (not the L0 value producer), so a pair carries `trace` + `regstate` +   */
/* `note` stops — the ABI x-ray reads exactly those; no df_step/coverage   */
/* is written, because the marshalling story needs neither.                */
/* ------------------------------------------------------------------ */

/* One ordered walkthrough stop (06-doors-and-learning.md `note` model). A
 * step_anchor < 0 is UNANCHORED — a remark about the call as a whole rather
 * than one instruction; the x-ray leaves the playhead where the last anchored
 * stop put it. */
typedef struct {
    long step_anchor;
    const char *title;
    const char *body;
} abi_stop_t;

/* make_pair(a, b) -> struct pair{long a, b} (examples/structs.s): a 16-byte
 * aggregate, the eightbyte-classification contrast made visible in the register
 * files. SysV splits it into two INTEGER eightbytes (rdi:rsi inbound, rax:rdx on
 * return); Win64 has no eightbyte split and returns anything over 8 bytes by a
 * hidden pointer, shifting its integer args one register later. */
static const abi_stop_t MAKE_PAIR_STOPS[] = {
    {0, "arguments arrive in different registers",
     "make_pair(a=7, b=11) returns a 16-byte struct {long a; long b}. System V "
     "places the first two integer arguments in rdi and rsi; Microsoft x64 "
     "places them in rcx and rdx. Before a single instruction runs the "
     "register "
     "files already disagree: a0 -> rdi (SysV) vs rcx (Win64), a1 -> rsi vs "
     "rdx."},
    {1, "the first eightbyte is staged",
     "movq rdi, rax stages the struct's first 8-byte chunk into the return "
     "register. Under SysV rdi held a, so rax becomes 7; under Win64 rdi is a "
     "nonvolatile register the caller never loaded, so rax becomes 0 — the "
     "same "
     "instruction, a different input, because each convention chose a "
     "different "
     "argument register."},
    {2, "the second eightbyte, and how each ABI returns the struct",
     "movq rsi, rdx stages the second chunk. SysV classifies the 16-byte "
     "return "
     "as two INTEGER eightbytes and hands it back in the pair rax:rdx, so "
     "rax:rdx = (a, b) IS the returned struct. Win64 classifies any aggregate "
     "larger than 8 bytes as MEMORY and returns it through a hidden pointer "
     "the "
     "caller passes in rcx — which is why its integer arguments start one "
     "register later, in rdx."},
    {-1, "eightbyte classification, side by side",
     "The System V AMD64 ABI splits an aggregate into 8-byte 'eightbytes' and "
     "classifies each INTEGER, SSE or MEMORY; two INTEGER eightbytes ride in "
     "rdi:rsi inbound and rax:rdx on return. Microsoft x64 has no eightbyte "
     "split — anything that is not 1, 2, 4 or 8 bytes is passed and returned "
     "by "
     "reference. The register deltas above are that one rule, made "
     "mechanical."},
};

/* sum3(a, b, c) (examples/args.s): three integer args, register-only under both
 * ABIs and straight-line — the fundamental a0 -> rdi vs rcx contrast, the
 * rsi/rdi callee-saved role reversal, and where each convention's stack spill
 * would begin. */
static const abi_stop_t SUM3_STOPS[] = {
    {0, "a0 -> rdi (SysV) vs rcx (Win64)",
     "sum3(11, 22, 33) takes three integer arguments. System V assigns them to "
     "rdi, rsi, rdx (its integer sequence is rdi, rsi, rdx, rcx, r8, r9); "
     "Microsoft x64 assigns them to rcx, rdx, r8 (its sequence is rcx, rdx, "
     "r8, "
     "r9). Same three values, six different registers — the entry state shows "
     "a0 "
     "in rdi on the left and in rcx on the right."},
    {1, "rsi and rdi swap roles",
     "The routine adds rsi into the accumulator. Under SysV rsi is the second "
     "argument (22); under Win64 rsi is CALLEE-SAVED (nonvolatile) and never "
     "an "
     "argument, so it is 0 here. rsi and rdi are argument registers in System "
     "V "
     "but callee-saved in Microsoft x64 — the single commonest source of "
     "cross-ABI corruption."},
    {2, "the third add",
     "adds rdx. SysV's THIRD argument lives in rdx (33); Win64's SECOND "
     "argument "
     "also lives in rdx (22), because its integer sequence is rcx, rdx, r8, "
     "r9. "
     "The two accumulators diverge accordingly."},
    {-1, "where the stack would begin",
     "Neither convention has spilled to memory. System V passes its first six "
     "integer arguments in registers and the seventh onward at 8(%rsp), "
     "16(%rsp), ...; Microsoft x64 passes its first four in registers and the "
     "fifth onward on the stack ABOVE a 32-byte shadow area the caller always "
     "reserves. sum3's three args fit in registers under both — a seventh SysV "
     "or fifth Win64 argument would take the first stack slot."},
};

typedef struct {
    const char *routine; /* corpus routine name */
    long args[6];
    int nargs;
    size_t cap;             /* per-step ring cap (>= steps: a pair keeps all) */
    const char *sysv_intro; /* the SysV leg's leading note = the x-ray title  */
    const char
        *win64_intro; /* the Win64 leg's standalone identity note       */
    const abi_stop_t
        *stops; /* authored into the SysV (reference) leg only    */
    int nstops;
} abi_xray_t;

static const abi_xray_t ABI_XRAYS[] = {
    {"make_pair",
     {7, 11, 0, 0, 0, 0},
     2,
     8,
     "make_pair(7, 11) — the same call marshalled two ways: System V (left) "
     "returns the 16-byte struct in rax:rdx, Microsoft x64 (right) returns it "
     "by a hidden pointer. Step the walkthrough to watch the register files "
     "diverge.",
     "make_pair(7, 11) under Microsoft x64 (Win64)",
     MAKE_PAIR_STOPS,
     (int)(sizeof MAKE_PAIR_STOPS / sizeof MAKE_PAIR_STOPS[0])},
    {"sum3",
     {11, 22, 33, 0, 0, 0},
     3,
     8,
     "sum3(11, 22, 33) — the same call marshalled two ways: System V (left) "
     "reads rdi, rsi, rdx; Microsoft x64 (right) reads rcx, rdx, r8. Step the "
     "walkthrough to watch a0 land in a different register on each side.",
     "sum3(11, 22, 33) under Microsoft x64 (Win64)",
     SUM3_STOPS,
     (int)(sizeof SUM3_STOPS / sizeof SUM3_STOPS[0])},
};
#define N_ABI_XRAYS ((int)(sizeof ABI_XRAYS / sizeof ABI_XRAYS[0]))

/* Emit one walkthrough stop as the schema's `note` kind, fields in canonical
 * order (text, step, stop, title) with absent ones omitted — the same spelling
 * gen_walkthroughs.c uses, so a stop reads identically whichever tool wrote it. */
static void emit_abi_stop(asmtrace_writer_t *w, const abi_stop_t *s) {
    char body[4096], esc[2048];
    size_t n;
    asmtrace_escape(esc, sizeof esc, s->body);
    n = (size_t)snprintf(body, sizeof body, "\"text\":\"%s\"", esc);
    if (s->step_anchor >= 0)
        n += (size_t)snprintf(body + n, sizeof body - n, ",\"step\":%ld",
                              s->step_anchor);
    n += (size_t)snprintf(body + n, sizeof body - n, ",\"stop\":true");
    if (s->title != NULL) {
        asmtrace_escape(esc, sizeof esc, s->title);
        snprintf(body + n, sizeof body - n, ",\"title\":\"%s\"", esc);
    }
    asmtrace_emit(w, "note", body);
}

/* Record one leg of an ABI x-ray pair: run `code` under the per-step register
 * ring, through the System V or Microsoft x64 convention, and write
 * <dir>/<out>.asmtrace as `note` (intro) + `trace` + `regstate` [+ `note`
 * stops]. Both legs run the SAME bytes; only the convention — hence the
 * argument-register placement, hence the register files — differs. The ring cap
 * holds every step, so a pair is never truncated. Returns 0 / -1. */
static int record_abi_leg(const char *dir, const char *out, const char *intro,
                          const uint8_t *code, size_t code_len,
                          const long *args, int nargs, size_t cap, int win64,
                          const abi_stop_t *stops, int nstops) {
    asmtrace_prov_t prov = {"emu-l0", 1, "exact", 0, NULL, 0};
    asmtrace_writer_t w;
    emu_trace_t tr;
    emu_result_t res;
    uint64_t insns[256], blocks[64];
    char path[1024], body[8192];
    emu_t *e;
    size_t nsteps, held, i;

    e = emu_open();
    if (e == NULL) {
        fprintf(stderr, "asmtrace_record: emu_open failed for %s\n", out);
        return -1;
    }
    if (!emu_step_capture(e, cap)) {
        /* x86-64-guest only, exactly the corpus loop's gate: a host without the
         * emulator cannot produce the golden, so fail loudly rather than commit
         * a regstate-free pair that would silently defeat the x-ray. */
        fprintf(stderr, "asmtrace_record: step ring unavailable for %s\n", out);
        emu_close(e);
        return -1;
    }

    memset(&tr, 0, sizeof tr);
    tr.insns = insns;
    tr.insns_cap = sizeof insns / sizeof insns[0];
    tr.blocks = blocks;
    tr.blocks_cap = sizeof blocks / sizeof blocks[0];
    memset(&res, 0, sizeof res);
    if (win64)
        emu_call_win64_traced(e, code, code_len, args, nargs, 0, &res, &tr);
    else
        emu_call_traced(e, code, code_len, args, nargs, 0, &res, &tr);

    snprintf(path, sizeof path, "%s/%s.asmtrace", dir, out);
    if (asmtrace_open(&w, path, 1 /* deterministic */) != 0) {
        fprintf(stderr, "asmtrace_record: cannot write %s\n", path);
        emu_close(e);
        return -1;
    }
    asmtrace_header(&w, "asmtrace_record", &prov, 0, NULL);

    asmtrace_escape(body, sizeof body, intro);
    asmtrace_emitf(&w, "note", "\"text\":\"%s\"", body);

    /* The ordered executed-instruction stream (basis "rel", offsets from
     * EMU_CODE_BASE) — the same `trace` kind the corpus loop writes, so the
     * walkthrough player counts steps from it identically. */
    nsteps = tr.insns_len;
    for (i = 0; i < nsteps; i++) {
        char dis[160] = "";
        if (emu_disas_available())
            emu_disas(EMU_ARCH_X86_64, code, code_len, EMU_CODE_BASE, insns[i],
                      dis, sizeof dis);
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

    /* One `regstate` per held pre-state (oldest first) — the per-step register
     * file the x-ray's two scrubber panes replay. The cap holds every step, so
     * there is no eviction and no truncation. */
    held = emu_step_count(e);
    for (i = 0; i < held; i++) {
        emu_x86_regs_t regs;
        if (emu_step_at(e, i, NULL, &regs))
            emit_regstate(&w, &regs);
    }

    /* Stops last, so file order IS ordinal order — the player's contract. The
     * reference (SysV) leg carries them; the Win64 leg is pure paired data. */
    for (i = 0; i < (size_t)nstops; i++)
        emit_abi_stop(&w, &stops[i]);

    asmtrace_close(&w, 0, 0, NULL);
    emu_close(e);
    printf("  %-28s %zu step(s), %zu regstate(s), %d stop(s)  [%s]\n", out,
           nsteps, held, nstops, win64 ? "Win64" : "SysV");
    return 0;
}

/* Record both legs of one ABI x-ray pair from a corpus routine's window. */
static int record_abi_pair(const char *dir, const abi_xray_t *x) {
    uint8_t code[REC_WINDOW];
    char out[128];
    const void *fn = asmtest_corpus_routine(x->routine);
    int rc = 0;
    if (!fn) {
        fprintf(stderr, "asmtrace_record: no corpus routine '%s'\n",
                x->routine);
        return -1;
    }
    memcpy(code, fn, sizeof code);
    snprintf(out, sizeof out, "abixray-%s-sysv", x->routine);
    if (record_abi_leg(dir, out, x->sysv_intro, code, sizeof code, x->args,
                       x->nargs, x->cap, 0, x->stops, x->nstops) != 0)
        rc = -1;
    snprintf(out, sizeof out, "abixray-%s-win64", x->routine);
    if (record_abi_leg(dir, out, x->win64_intro, code, sizeof code, x->args,
                       x->nargs, x->cap, 1, NULL, 0) != 0)
        rc = -1;
    return rc;
}

int main(int argc, char **argv) {
    const char *dir = "tests/golden-asmtrace";
    int failed = 0;
    /* argv: [--steps=<cap>] [<output-directory>], in any order. --steps default
     * stays 0 (off) so the golden target — which passes no flag — emits regstate
     * only for the routines that bake it in (T2's worked example + fixture). */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--steps=", 8) == 0)
            g_steps_cap = (size_t)strtoul(argv[i] + 8, NULL, 10);
        else
            dir = argv[i];
    }
    if (!emu_disas_available())
        fprintf(
            stderr,
            "asmtrace_record: WARNING — no Capstone, so `disasm` fields are "
            "omitted (D10 degradation). These bytes are NOT the golden "
            "corpus; regenerate in the docker-cli image.\n");
    printf("asmtrace_record -> %s\n", dir);
    for (int i = 0; i < N_ROUTINES; i++)
        if (record_one(dir, &ROUTINES[i]) != 0)
            failed++;

    /* The Loom's four walkthrough-grade fixtures (05-loom-day-one.md T7). */
    {
        static const long df_args[2] = {7, 5};
        static const long fork_args[1] = {3};
        static const long rbx_args[2] = {2, 3};
        uint8_t rbx[REC_WINDOW];
        const void *fn;

        if (record_bytes(dir, "loom-df-chain", "df_chain", LOOM_DF_CHAIN,
                         sizeof LOOM_DF_CHAIN, df_args, 2, 65536, 0) != 0)
            failed++;
        /* The D7 dishonesty fixture: a four-record operand buffer fills mid-run,
         * the producer flips `truncated`, and the footer declares it. Generated,
         * never hand-edited — a fixture nobody can produce is a fixture nobody
         * can trust. */
        if (record_bytes(dir, "loom-truncated", "df_chain (recs_cap = 4)",
                         LOOM_DF_CHAIN, sizeof LOOM_DF_CHAIN, df_args, 2, 4,
                         0) != 0)
            failed++;
        if (record_bytes(dir, "loom-fork-demo", "fork_demo", LOOM_FORK_DEMO,
                         sizeof LOOM_FORK_DEMO, fork_args, 1, 65536, 0) != 0)
            failed++;
        /* Walkthrough #2: a callee-save spill (a stack band), an EFLAGS knot and
         * a restore — examples/flags.s:45. HOST-ROUTINE bytes, so this entry is
         * exactly as x86-64-gated as the corpus loop above. */
        fn = asmtest_corpus_routine("sum_via_rbx");
        if (!fn) {
            fprintf(stderr, "asmtrace_record: no corpus routine "
                            "'sum_via_rbx'\n");
            failed++;
        } else {
            memcpy(rbx, fn, sizeof rbx);
            if (record_bytes(dir, "loom-sum-via-rbx", "sum_via_rbx", rbx,
                             sizeof rbx, rbx_args, 2, 65536, 0) != 0)
                failed++;
            /* The T2 register-ring dishonesty fixture (09-teaching-producers.md):
             * the SAME sum_via_rbx bytes with a TWO-entry step ring, so it holds
             * only the last two pre-states and evicts the rest. The operand
             * buffer is UNcapped (65536), so this truncation is the register
             * ring's alone — the footer's `truncated` flips and `drops.lost`
             * carries the evicted count, exactly the D7 honesty loom-truncated
             * shows for operands. The name ends in `-truncated` because the
             * golden corpus test (desktop/test/test_golden.cpp) requires a flat
             * golden to be truncated IFF so named. Generated, never hand-edited. */
            if (record_bytes(dir, "regstate-truncated",
                             "sum_via_rbx (steps_cap = 2)", rbx, sizeof rbx,
                             rbx_args, 2, 65536, 2) != 0)
                failed++;
        }
    }

    /* The ABI x-ray's paired SysV/Win64 goldens (09-teaching-producers.md T4).
     * Independent of --steps: each pair bakes its own ring cap, so `make
     * asmtrace-golden` (no flag) emits them exactly as it does add_signed's
     * worked example. */
    for (int i = 0; i < N_ABI_XRAYS; i++)
        if (record_abi_pair(dir, &ABI_XRAYS[i]) != 0)
            failed++;

    if (failed) {
        fprintf(stderr, "asmtrace_record: %d routine(s) failed\n", failed);
        return 1;
    }
    return 0;
}
