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
 * so (`truncated` + `drops.lost`): an over-cap deck is faithful data, never a
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
 * Usage: asmtrace_record [--steps=<cap>] [--mem] [--fpregs] [--blame] [--statediff] <output-directory>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asmtest_emu.h"      /* emu_disas (D10) */
#include "asmtest_valtrace.h" /* the L0 sink + L1 def-use graph */
#include "asmtrace_ndjson.h"  /* the shared writer (field order lives there) */
#include "asmtrace_sha256.h"  /* routine-identity hash for the `code` header */

/* The emulator L0 producer. It is a TIER producer, not part of the pure public
 * sink surface, so it ships no public header and consumers re-declare it (the
 * examples/test_dataflow_emu.c precedent). */
int asmtest_dataflow_emu_run(const uint8_t *code, size_t code_len,
                             const long *args, int nargs, uint64_t max_insns,
                             asmtest_valtrace_t *vt);

/* R4 T3: the FP/vector-arg value producer — marshals `nfargs` SSE-class doubles
 * into xmm0..7 alongside the `niargs` integer args, so an FP-argument routine
 * records with its real XMM inputs (the operand values ride `wide[]`). */
int asmtest_dataflow_emu_run_fp(const uint8_t *code, size_t code_len,
                                const long *iargs, int niargs,
                                const double *fargs, int nfargs,
                                uint64_t max_insns, asmtest_valtrace_t *vt);

/* The per-guest entry (R5, docs/internal/archive/gui/32-per-guest-value-producer.md): the
 * SAME producer selecting a guest by arch. asmtest_dataflow_emu_run is this pinned
 * to x86-64; the arm64 golden below runs an AArch64 routine through the arm64
 * guest, so the value fabric is no longer x86-64-only. */
int asmtest_dataflow_emu_run_arch(asmtest_arch_t arch, const uint8_t *code,
                                  size_t code_len, const long *args, int nargs,
                                  uint64_t max_insns, asmtest_valtrace_t *vt);

/* name -> routine pointer, from the conformance corpus. */
void *asmtest_corpus_routine(const char *name);

/* The address the producer maps the routine bytes at (src/dataflow_emu.c), so
 * PC-relative operands disassemble to the same absolute targets the run saw. */
#define REC_CODE_BASE 0x00100000UL

/* The conformance emulator convention: a corpus routine is captured as a fixed
 * 64-byte window from its entry (bindings/conformance/conformance.c). */
#define REC_WINDOW 64

/* Arm the writer's `code` routine-identity header (28 R1 T1) from the fixed
 * bytes this recorder already holds: the SHA-256 of code[0..len) names the
 * routine by its bytes, so dt_diff refuses a wrong-routine pair. Deterministic
 * (the bytes and the window length are fixed), so it is part of the golden.
 * Call after asmtrace_open, before asmtrace_header. */
static void set_code_identity(asmtrace_writer_t *w, const char *name,
                              const uint8_t *code, size_t len) {
    char hex[65];
    asmtrace_sha256_hex(code, len, hex);
    asmtrace_writer_set_code(w, name, hex, (unsigned long long)len);
}

/* --steps=<cap>: the default per-step register-ring cap for the corpus loop
 * (0 = off, the golden default). Set once from argv; a routine's own steps_cap
 * below applies when this is 0, so `make asmtrace-golden` (no flag) still emits
 * the one worked example while an explicit --steps=N arms the whole corpus. */
static size_t g_steps_cap = 0;

/* --mem: arm the `mem` address-stream (29 R2) for the whole corpus loop. Off by
 * default (the golden default), so `make asmtrace-golden` (no flag) emits `mem`
 * only for the fixtures that bake it in, and every other golden is byte-unchanged.
 * A routine's own baked-in want_mem below applies when this is 0. */
static int g_mem = 0;

/* --fpregs: arm the wide FP/vector register deck (31 R4) for the whole corpus
 * loop — each `regstate` then also carries the 16 XMM registers + MXCSR. Off by
 * default (the golden default), so `make asmtrace-golden` (no flag) leaves every
 * existing regstate golden byte-identical; the dedicated FP fixture (T3) arms it
 * on its own. Only meaningful where the register ring is armed (--steps > 0). */
static int g_fpregs = 0;

/* Emit the `mem` address stream for one invocation (29 R2): one `mem` event per
 * memory-kind operand record in `recs[0..n)`, in the appended (step) order, gated
 * by the caller's opt-in. The record stream `df_step` already carries the resolved
 * effective address; this lifts it to a first-class per-access channel the 3D rich
 * rung keys on. Register records are not memory accesses and are skipped. The body
 * builder (asmtrace_ndjson.c) owns field order, so a golden `mem` and a live one
 * spell identically. Returns the number of events emitted. */
static size_t emit_mem_stream(asmtrace_writer_t *w, const at_val_rec_t *recs,
                              size_t n) {
    char body[128];
    size_t emitted = 0;
    for (size_t i = 0; i < n; i++) {
        const at_val_rec_t *r = &recs[i];
        if (r->kind != AT_LOC_MEM_ABS && r->kind != AT_LOC_MEM_OFF)
            continue;
        asmtrace_mem_body(body, sizeof body, r->step, r->addr, r->size,
                          r->is_write, r->kind);
        asmtrace_emit(w, "mem", body);
        emitted++;
    }
    return emitted;
}

/* --blame: arm the `blame` backward-attribution stream (33 R6 T1) for the whole
 * corpus loop. Off by default (the golden default), so `make asmtrace-golden`
 * emits `blame` only for the fixtures that bake it in. Like --mem/--steps, this is
 * a pure derived pass — no engine re-run. */
static int g_blame = 0;

/* --statediff: arm the `statediff` step-delta stream (33 R6 T2) for the whole
 * corpus loop. Off by default; a separate gate from --steps so an existing
 * regstate golden stays byte-identical (statediff rides ALONGSIDE regstate but is
 * armed independently). Only meaningful where the regstate ring is armed. */
static int g_statediff = 0;

/* Emit one `blame` backward-attribution event (33 R6 T1) for `seed` step: the
 * backward def-use slice (asmtest_slice_backward_seed — the SAME pure pass the
 * TUI and desktop slicers are parity-pinned against) is the cone of producing
 * steps, and the sink-alone case is the truthful born-of-untraced-state verdict
 * ("provenance starts at instrumentation"), never an empty cone. `loc` is the
 * seed step's write record (the blamed value), or NULL. The def-use graph `g` is
 * already built (record_bytes calls asmtest_defuse_build); this is a projection
 * over it. */
static void emit_blame(asmtrace_writer_t *w, const asmtest_valtrace_t *vt,
                       const asmtest_defuse_t *g, uint32_t seed) {
    at_val_rec_t seed_rec;
    asmtest_slice_t *sl;
    uint32_t *cone_steps;
    uint64_t *cone_offs;
    const at_val_rec_t *loc = NULL;
    char *body;
    size_t ncone = 0;

    memset(&seed_rec, 0, sizeof seed_rec);
    seed_rec.step = seed; /* the slicer reads only .step */
    sl = asmtest_slice_backward_seed(g, &seed_rec);
    if (sl == NULL)
        return;
    /* The cone is at most one entry per step, so it never exceeds sl->n; size the
     * buffers (and the body, ~24 bytes/entry) to the ACTUAL cone so a long slice
     * over a looping routine is serialized in full, never silently truncated. */
    cone_steps = (uint32_t *)malloc((sl->n ? sl->n : 1) * sizeof *cone_steps);
    cone_offs = (uint64_t *)malloc((sl->n ? sl->n : 1) * sizeof *cone_offs);
    body = (char *)malloc(4096 + sl->n * 32);
    if (cone_steps == NULL || cone_offs == NULL || body == NULL) {
        free(cone_steps);
        free(cone_offs);
        free(body);
        asmtest_slice_free(sl);
        return; /* fail closed: no blame beats a truncated one */
    }
    for (size_t i = 0; i < sl->n; i++) {
        uint32_t s = sl->steps[i];
        cone_steps[ncone] = s;
        cone_offs[ncone] = (s < vt->steps_len) ? vt->insn_off[s] : 0;
        ncone++;
    }
    /* The blamed value is the seed step's first register write. A value with no
     * write record (or a memory-only step) blames the step without a `loc`. */
    for (size_t i = 0; i < vt->recs_len; i++) {
        if (vt->recs[i].step == seed && vt->recs[i].is_write &&
            vt->recs[i].kind == AT_LOC_REG) {
            loc = &vt->recs[i];
            break;
        }
    }
    /* Born of untraced state: the backward slice reached only the sink, so no
     * traced instruction produced this value (it came from an argument, a
     * constant, or pre-existing state — the ancestry ends at instrumentation). */
    int born_untraced = sl->n <= 1;
    asmtrace_blame_body(body, 4096 + sl->n * 32, seed,
                        (seed < vt->steps_len) ? vt->insn_off[seed] : 0, loc,
                        cone_steps, cone_offs, ncone, born_untraced);
    asmtrace_emit(w, "blame", body);
    free(cone_steps);
    free(cone_offs);
    free(body);
    asmtest_slice_free(sl);
}

/* Fixed routines and arguments. These are INTEGER-ARG routines (the emulator L0
 * producer marshals rdi/rsi/rdx/rcx/r8/r9); the FP/vector case is retired by 31 R4
 * T3's `asmtest_dataflow_emu_run_fp` + record_bytes_fp, exercised by the fp-scale-add
 * byte-literal fixture below rather than a table entry (the corpus routines are
 * host-native integer symbols). `steps_cap` is the per-step register-ring cap baked
 * into the golden for that routine (0 = no regstate); add_signed carries the worked
 * example so the committed corpus exercises the `regstate` kind without --steps. */
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
/* The Loom's golden fixtures (docs/internal/archive/gui/05-loom-day-one.md T7) */
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

/* identity(a) = a — the born-of-untraced-state blame fixture (33 R6 T1):
 *   0x00 mov rax, rdi   ; 48 89 f8   (reads rdi, an ARGUMENT: no traced producer)
 *   0x03 ret            ; c3
 * The blame seed is the penultimate step (mov rax, rdi); its only input is rdi,
 * which no traced instruction wrote, so the backward slice is the sink ALONE and
 * born_untraced fires — provenance starts at instrumentation. */
static const uint8_t BLAME_UNTRACED[] = {
    0x48, 0x89, 0xf8, /* mov rax, rdi */
    0xc3,             /* ret          */
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

/* fp_scale_add(x, y) [xmm0=x, xmm1=y] — the FP/vector worked example (31 R4 T3),
 * retiring the "integer-arg only" corpus limit. A BYTE LITERAL like the loom
 * fixtures, because its FP arithmetic is hand-derivable on paper and must stay so:
 *   0x00  addsd xmm0, xmm1   ; f2 0f 58 c1   xmm0 <- x + y
 *   0x04  ret                ; c3
 * Recorded with SSE-class args x=1.5, y=2.25 (marshalled into xmm0/xmm1 by the FP
 * value producer) under the wide deck (--fpregs baked in): the regstate ring shows
 * xmm0 = 1.5 at the pre-state of `addsd` and xmm0 = 3.75 at the pre-state of `ret`
 * — the whole FP computation, countable off this listing. The `df_step` operand
 * values ride wide[] (28 R1 T3 hex bytes), and each held step carries an `fpenv`. */
static const uint8_t FP_SCALE_ADD[] = {
    0xf2, 0x0f, 0x58, 0xc1, /* 0x00 addsd xmm0, xmm1 */
    0xc3,                   /* 0x04 ret             */
};

/* ------------------------------------------------------------------ */
/* The 3D spacetime overview's golden SCENES                            */
/* (docs/internal/archive/gui/10-spacetime-3d-overview.md T7)                   */
/*                                                                      */
/* A BYTE LITERAL for the same reason the Loom's fixtures are: the whole */
/* point of a scene golden is that its terrain is hand-checkable — you   */
/* can count the loop's iterations off the listing and predict which     */
/* cell is hot — so the listing lives beside the bytes.                  */
/* ------------------------------------------------------------------ */

/* scene_hot_loop(n)  [rdi=n] — the smallest genuine source of HEAT. The
 * three-instruction loop body runs n times while the prologue and epilogue run
 * once each, so the terrain has a hot cell over cold ones rather than a uniform
 * plane. With n = 3 the executed stream is 13 steps and offset 0x06 is hit 3
 * times — countable off this listing:
 *   0x00  mov rax, rdi     ; 48 89 f8
 *   0x03  xor rcx, rcx     ; 48 31 c9
 *   0x06  add rcx, rax     ; 48 01 c1   <- the loop body: the hot cell
 *   0x09  dec rax          ; 48 ff c8
 *   0x0c  jne 0x06         ; 75 f8      (rel8 = 0x06 - 0x0e = -8)
 *   0x0e  mov rax, rcx     ; 48 89 c8
 *   0x11  ret              ; c3                                              */
static const uint8_t SCENE_HOT_LOOP[] = {
    0x48, 0x89, 0xf8, /* 0x00 mov rax, rdi */
    0x48, 0x31, 0xc9, /* 0x03 xor rcx, rcx */
    0x48, 0x01, 0xc1, /* 0x06 add rcx, rax */
    0x48, 0xff, 0xc8, /* 0x09 dec rax      */
    0x75, 0xf8,       /* 0x0c jne 0x06     */
    0x48, 0x89, 0xc8, /* 0x0e mov rax, rcx */
    0xc3,             /* 0x11 ret          */
};

/* ------------------------------------------------------------------ */
/* The AArch64 value-fabric golden                                      */
/* (docs/internal/archive/gui/32-per-guest-value-producer.md R5 T2)             */
/*                                                                      */
/* A BYTE LITERAL, hand-derivable on paper exactly like the Loom's      */
/* x86-64 fixtures: the whole point of this golden is to prove the       */
/* value fabric is NO LONGER x86-64-only, so the arm64 listing lives     */
/* right beside its bytes. Run through the arm64 `df_guest` under         */
/* Unicorn (which emulates AArch64 on any host), so it is generated on    */
/* the SAME x86-64 lane as every other golden — the guest is the arch,    */
/* not the host.                                                          */
/* ------------------------------------------------------------------ */

/* arm64_df_chain(a, b)  [x0=a, x1=b] — a straight-line def-use chain:
 *   0x00  add x2, x0, x1   ; 02 00 01 8b   x2 = a + b   (reads x0,x1; writes x2)
 *   0x04  add x3, x2, x2   ; 43 00 02 8b   x3 = 2 * x2  (edge x2; writes x3)
 *   0x08  mov x0, x3       ; e0 03 03 aa   x0 = x3      (edge x3; the return value)
 *   0x0c  ret              ; c0 03 5f d6   return via x30 (LR)
 * With a = 7, b = 5 the fabric is x2 = 12, x3 = 24, x0 = 24, and the def-use graph
 * has edges 0->1 (x2) and 1->2 (x3) — the same L0->L1 story the x86-64 df_chain
 * tells, under the arm64 guest. */
static const uint8_t ARM64_DF_CHAIN[] = {
    0x02, 0x00, 0x01, 0x8b, /* 0x00 add x2, x0, x1 */
    0x43, 0x00, 0x02, 0x8b, /* 0x04 add x3, x2, x2 */
    0xe0, 0x03, 0x03, 0xaa, /* 0x08 mov x0, x3     */
    0xc0, 0x03, 0x5f, 0xd6, /* 0x0c ret            */
};

/* One AArch64 per-step register snapshot as a `regstate` event, by descriptor
 * reference — the arm64 analogue of emit_regstate (R5,
 * docs/internal/archive/gui/32-per-guest-value-producer.md, the regstate/Scrubber
 * half). `values` names the raw physical register file: x0..x30 (AAPCS64
 * gives x29/x30 their FP/LR roles, but the descriptor names the register, not
 * the role — exactly as the x86-64 descriptor names rbp/rsp by register), plus
 * sp, pc, nzcv, in emu_arm64_regs_t declaration order. No vector/NEON deck: a
 * wide value is not a bare JSON integer (the same v1 omission the x86-64
 * descriptor recorded before R4's --fpregs), and capturing it is a further
 * seam, mirroring how the arm64 value fabric's own guest stayed integer-only
 * (32-per-guest-value-producer.md T2). Hand-rolled here rather than through a
 * shared cli/asmtrace_ndjson.c body-builder because there is (yet) only ONE
 * arm64 regstate producer — this one — exactly the historical shape
 * emit_regstate itself had before a live x86-64 producer existed to share
 * asmtrace_regstate_body with. */
static void emit_regstate_arm64(asmtrace_writer_t *w,
                                const emu_arm64_regs_t *r) {
    char body[2048];
    int o = snprintf(
        body, sizeof body,
        "\"desc\":\"emu_arm64_regs_t@aarch64/aapcs64\",\"values\":{"
        "\"x0\":%llu,\"x1\":%llu,\"x2\":%llu,\"x3\":%llu,\"x4\":%llu,"
        "\"x5\":%llu,\"x6\":%llu,\"x7\":%llu,\"x8\":%llu,\"x9\":%llu,"
        "\"x10\":%llu,\"x11\":%llu,\"x12\":%llu,\"x13\":%llu,\"x14\":%llu,"
        "\"x15\":%llu,\"x16\":%llu,\"x17\":%llu,\"x18\":%llu,\"x19\":%llu,"
        "\"x20\":%llu,\"x21\":%llu,\"x22\":%llu,\"x23\":%llu,\"x24\":%llu,"
        "\"x25\":%llu,\"x26\":%llu,\"x27\":%llu,\"x28\":%llu,\"x29\":%llu,"
        "\"x30\":%llu,\"sp\":%llu,\"pc\":%llu,\"nzcv\":%llu}",
        (unsigned long long)r->x[0], (unsigned long long)r->x[1],
        (unsigned long long)r->x[2], (unsigned long long)r->x[3],
        (unsigned long long)r->x[4], (unsigned long long)r->x[5],
        (unsigned long long)r->x[6], (unsigned long long)r->x[7],
        (unsigned long long)r->x[8], (unsigned long long)r->x[9],
        (unsigned long long)r->x[10], (unsigned long long)r->x[11],
        (unsigned long long)r->x[12], (unsigned long long)r->x[13],
        (unsigned long long)r->x[14], (unsigned long long)r->x[15],
        (unsigned long long)r->x[16], (unsigned long long)r->x[17],
        (unsigned long long)r->x[18], (unsigned long long)r->x[19],
        (unsigned long long)r->x[20], (unsigned long long)r->x[21],
        (unsigned long long)r->x[22], (unsigned long long)r->x[23],
        (unsigned long long)r->x[24], (unsigned long long)r->x[25],
        (unsigned long long)r->x[26], (unsigned long long)r->x[27],
        (unsigned long long)r->x[28], (unsigned long long)r->x[29],
        (unsigned long long)r->x[30], (unsigned long long)r->sp,
        (unsigned long long)r->pc, (unsigned long long)r->nzcv);
    if (o < 0)
        return;
    size_t n = (size_t)o >= sizeof body ? sizeof body - 1 : (size_t)o;
    body[n] = '\0';
    asmtrace_emit(w, "regstate", body);
}

/* Per-step AArch64 register ring (R5): when steps_cap > 0, run the SAME bytes
 * again under a FRESH emu_arm64_t handle with its per-step ring armed
 * (emu_arm64_step_capture) and emit one `regstate` event per held pre-state
 * (oldest first) — the arm64 analogue of emit_regstates. This is a SEPARATE
 * Unicorn engine from the value-fabric run above (df_guest's own uc_open), the
 * same two-engines-one-recording shape emit_regstates already has relative to
 * asmtest_dataflow_emu_run for x86-64; the fixed EMU_CODE_BASE/EMU_STACK_BASE
 * layout matches DF_CODE_BASE/DF_STACK_BASE numerically, so both engines see
 * the same addresses even though neither shares state with the other. Returns
 * the number of earliest steps the ring evicted (0 if steps_cap is 0 or the
 * ring could not arm), which the caller folds into truncation. No
 * vec/statediff/mem extension yet — further seams, not this one. */
static uint64_t emit_regstates_arm64(asmtrace_writer_t *w, const uint8_t *code,
                                     size_t code_len, const long *args,
                                     int nargs, size_t steps_cap) {
    emu_arm64_t *e;
    emu_arm64_result_t res;
    uint64_t dropped = 0;
    if (steps_cap == 0)
        return 0;
    e = emu_arm64_open();
    if (e == NULL)
        return 0;
    if (emu_arm64_step_capture(e, steps_cap)) {
        size_t held, i;
        memset(&res, 0, sizeof res);
        emu_arm64_call(e, code, code_len, args, nargs, 0, &res);
        held = emu_arm64_step_count(e);
        dropped = emu_arm64_step_dropped(e);
        for (i = 0; i < held; i++) {
            emu_arm64_regs_t regs;
            if (!emu_arm64_step_at(e, i, NULL, &regs))
                continue;
            emit_regstate_arm64(w, &regs);
        }
    }
    emu_arm64_close(e);
    return dropped;
}

/* Record one AArch64 routine as a value-fabric golden (R5 T2), written as
 * `note` + `trace` (rel) + `df_step` + `df_edge` through the arm64 `df_guest` —
 * plus, when `steps_cap > 0`, the R5 regstate/Scrubber half: one `regstate`
 * event per held pre-state of the arm64 per-step ring
 * (emu_arm64_regs_t@aarch64/aapcs64), so an arm64 Author-mode capture ALSO
 * drives the Scrubber's register time-travel, mirroring how the x86-64 ring
 * already does. The header's `arch` is accurately "aarch64" (the guest, via the
 * writer override) even though the recording is produced on an x86-64 lane. No
 * mem/blame: those ride their own opt-ins and are not armed here. The reader
 * needs NO change either way: the value fabric is already arch-parameterized
 * (descriptor + generic def-use over locations), and the Scrubber's
 * `regstate` reader (desktop/src/analysis/stepindex.cpp) already renders any
 * integer `values` field it does not specifically name, falling through to
 * its generic "extra keys, sorted" path for x0..x30/sp/pc/nzcv — a cosmetic
 * ordering follow-on, not a blocker, exactly as the value fabric's own
 * `loom_reg_name` degradation was left as one. Returns 0 / -1. */
static int record_arm64(const char *dir, const char *out, const char *label,
                        const uint8_t *code, size_t code_len, const long *args,
                        int nargs, size_t steps_cap) {
    asmtrace_prov_t prov = {"emu-l0", 1, "exact", 0, NULL, 0};
    asmtrace_writer_t w;
    asmtest_valtrace_t *vt = NULL;
    asmtest_defuse_t *g = NULL;
    char path[1024], body[65536];
    size_t nsteps, nrecs, cur = 0, s;
    int rc;

    vt = asmtest_valtrace_new(4096, 65536, 4096);
    if (!vt) {
        fprintf(stderr, "asmtrace_record: out of memory\n");
        return -1;
    }
    rc = asmtest_dataflow_emu_run_arch(ASMTEST_ARCH_ARM64, code, code_len, args,
                                       nargs, 0, vt);
    if (rc < 0) {
        fprintf(stderr,
                "asmtrace_record: arm64 emulator producer failed for %s\n",
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
    set_code_identity(&w, out, code, code_len);
    asmtrace_writer_set_arch(&w, "aarch64"); /* the guest, not the host (R5) */
    asmtrace_header(&w, "asmtrace_record", &prov, 0, NULL);

    {
        char text[256];
        int o = snprintf(text, sizeof text, "%s(", label);
        for (int i = 0; i < nargs; i++)
            o += snprintf(text + o, sizeof text - (size_t)o, "%s%ld",
                          i ? ", " : "", args[i]);
        snprintf(
            text + o, sizeof text - (size_t)o,
            ") under the deterministic emulator L0 producer, AArch64 guest");
        asmtrace_escape(body, sizeof body, text);
        asmtrace_emitf(&w, "note", "\"text\":\"%s\"", body);
    }

    nsteps = vt->steps_len;
    nrecs = vt->recs_len;

    for (s = 0; s < nsteps; s++) {
        char dis[160] = "";
        if (emu_disas_available())
            emu_disas(EMU_ARCH_ARM64, code, code_len, REC_CODE_BASE,
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

    for (s = 0; s < nsteps; s++) {
        char dis[160] = "";
        size_t first;
        if (emu_disas_available())
            emu_disas(EMU_ARCH_ARM64, code, code_len, REC_CODE_BASE,
                      vt->insn_off[s], dis, sizeof dis);
        while (cur < nrecs && vt->recs[cur].step < s)
            cur++;
        first = cur;
        while (cur < nrecs && vt->recs[cur].step == s)
            cur++;
        /* 37 T4: `when` is omitted (0) — this recorder holds no codeimage
         * timeline (it emits none), so its output never carries the field and
         * the golden corpus does not churn. */
        asmtrace_df_step_body(body, sizeof body, (unsigned)s, vt->insn_off[s],
                              REC_CODE_BASE, 0, dis, &vt->recs[first],
                              cur - first, vt->wide, vt->wide_len);
        asmtrace_emit(&w, "df_step", body);
    }

    for (size_t i = 0; g && i < g->n; i++) {
        asmtrace_df_edge_body(body, sizeof body, &g->edges[i]);
        asmtrace_emit(&w, "df_edge", body);
    }

    /* Per-step arm64 register states from the ring (R5), after the value
     * stream — the same ordering emit_regstates uses for x86-64. A non-zero
     * eviction count is truncation just like a full recs buffer. */
    uint64_t step_dropped =
        emit_regstates_arm64(&w, code, code_len, args, nargs, steps_cap);

    if (vt->truncated || step_dropped > 0)
        w.truncated = 1;
    asmtrace_writer_set_steps_total(&w, vt->steps_total);
    asmtrace_close(&w, step_dropped, 0, NULL);

    {
        size_t nedges = g ? g->n : (size_t)0;
        int trunc = vt->truncated || step_dropped > 0;
        asmtest_defuse_free(g);
        asmtest_valtrace_free(vt);
        printf("  %-20s %zu steps, %zu records, %zu def-use edges", out, nsteps,
               nrecs, nedges);
        if (steps_cap > 0)
            printf(", regstate cap %zu (dropped %llu)", steps_cap,
                   (unsigned long long)step_dropped);
        printf("  [arm64]%s\n", trunc ? " [TRUNCATED]" : "");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* ARM32 (A32) worked example (60-arm32-riscv-author-mode.md T1)       */
/*                                                                     */
/* Mirrors the arm64 listing immediately above: a BYTE LITERAL, hand-  */
/* derivable on paper. Run through the arm32 `df_guest` under Unicorn  */
/* (which emulates A32 on any host), generated on the SAME x86-64 lane */
/* as every other golden — the guest is the arch, not the host.        */
/* ------------------------------------------------------------------ */

/* arm32_df_chain(a, b)  [r0=a, r1=b] — the SAME straight-line def-use chain
 * shape as ARM64_DF_CHAIN, in A32:
 *   0x00  add r2, r0, r1   ; 01 20 80 e0   r2 = a + b   (reads r0,r1; writes r2)
 *   0x04  add r3, r2, r2   ; 02 30 82 e0   r3 = 2 * r2  (edge r2; writes r3)
 *   0x08  mov r0, r3       ; 03 00 a0 e1   r0 = r3      (edge r3; the return value)
 *   0x0c  bx lr            ; 1e ff 2f e1   return via lr
 * With a = 7, b = 5 the fabric is r2 = 12, r3 = 24, r0 = 24, and the def-use
 * graph has edges 0->1 (r2) and 1->2 (r3) — the same L0->L1 story the arm64
 * chain tells, under the arm32 guest. */
static const uint8_t ARM32_DF_CHAIN[] = {
    0x01, 0x20, 0x80, 0xe0, /* 0x00 add r2, r0, r1 */
    0x02, 0x30, 0x82, 0xe0, /* 0x04 add r3, r2, r2 */
    0x03, 0x00, 0xa0, 0xe1, /* 0x08 mov r0, r3     */
    0x1e, 0xff, 0x2f, 0xe1, /* 0x0c bx lr          */
};

/* One ARM32 per-step register snapshot as a `regstate` event, by descriptor
 * reference — the arm32 analogue of emit_regstate_arm64. `values` names the
 * raw physical register file: r0..r12, sp, lr, pc, cpsr, in emu_arm_regs_t
 * declaration order (r[0..15], then cpsr) — integer-only, no VFP/NEON deck,
 * mirroring both the arm64 descriptor and the arm32 value fabric's own
 * integer-only scope (60-arm32-riscv-author-mode.md T1). */
static void emit_regstate_arm32(asmtrace_writer_t *w, const emu_arm_regs_t *r) {
    char body[1024];
    int o = snprintf(
        body, sizeof body,
        "\"desc\":\"emu_arm32_regs_t@arm/aapcs32\",\"values\":{"
        "\"r0\":%llu,\"r1\":%llu,\"r2\":%llu,\"r3\":%llu,\"r4\":%llu,"
        "\"r5\":%llu,\"r6\":%llu,\"r7\":%llu,\"r8\":%llu,\"r9\":%llu,"
        "\"r10\":%llu,\"r11\":%llu,\"r12\":%llu,\"sp\":%llu,\"lr\":%llu,"
        "\"pc\":%llu,\"cpsr\":%llu}",
        (unsigned long long)r->r[0], (unsigned long long)r->r[1],
        (unsigned long long)r->r[2], (unsigned long long)r->r[3],
        (unsigned long long)r->r[4], (unsigned long long)r->r[5],
        (unsigned long long)r->r[6], (unsigned long long)r->r[7],
        (unsigned long long)r->r[8], (unsigned long long)r->r[9],
        (unsigned long long)r->r[10], (unsigned long long)r->r[11],
        (unsigned long long)r->r[12], (unsigned long long)r->r[13] /* sp */,
        (unsigned long long)r->r[14] /* lr */,
        (unsigned long long)r->r[15] /* pc */, (unsigned long long)r->cpsr);
    if (o < 0)
        return;
    size_t n = (size_t)o >= sizeof body ? sizeof body - 1 : (size_t)o;
    body[n] = '\0';
    asmtrace_emit(w, "regstate", body);
}

/* Per-step ARM32 register ring (T1): when steps_cap > 0, run the SAME bytes
 * again under a FRESH emu_arm_t handle with its per-step ring armed
 * (emu_arm_step_capture) and emit one `regstate` event per held pre-state
 * (oldest first) — the arm32 analogue of emit_regstates_arm64. A SEPARATE
 * Unicorn engine from the value-fabric run above, exactly like the arm64
 * pair; EMU_CODE_BASE/EMU_STACK_BASE match DF_CODE_BASE/DF_STACK_BASE
 * numerically, so both engines see the same addresses even though neither
 * shares state with the other. Returns the number of earliest steps the
 * ring evicted. */
static uint64_t emit_regstates_arm32(asmtrace_writer_t *w, const uint8_t *code,
                                     size_t code_len, const long *args,
                                     int nargs, size_t steps_cap) {
    emu_arm_t *e;
    emu_arm_result_t res;
    uint64_t dropped = 0;
    if (steps_cap == 0)
        return 0;
    e = emu_arm_open();
    if (e == NULL)
        return 0;
    if (emu_arm_step_capture(e, steps_cap)) {
        size_t held, i;
        memset(&res, 0, sizeof res);
        emu_arm_call(e, code, code_len, args, nargs, 0, &res);
        held = emu_arm_step_count(e);
        dropped = emu_arm_step_dropped(e);
        for (i = 0; i < held; i++) {
            emu_arm_regs_t regs;
            if (!emu_arm_step_at(e, i, NULL, &regs))
                continue;
            emit_regstate_arm32(w, &regs);
        }
    }
    emu_arm_close(e);
    return dropped;
}

/* Record one ARM32 routine as a value-fabric golden (60-arm32-riscv-author-
 * mode.md T1), written as `note` + `trace` (rel) + `df_step` + `df_edge`
 * through the arm32 `df_guest` — plus, when `steps_cap > 0`, the regstate/
 * Scrubber half: one `regstate` event per held pre-state of the arm32
 * per-step ring (emu_arm32_regs_t@arm/aapcs32). The header's `arch` is
 * accurately "arm" (the guest, via the writer override) even though the
 * recording is produced on an x86-64 lane. No mem/blame: those ride their
 * own opt-ins and are not armed here. The reader needs NO change either
 * way, mirroring record_arm64's own closing note. Returns 0 / -1. */
static int record_arm32(const char *dir, const char *out, const char *label,
                        const uint8_t *code, size_t code_len, const long *args,
                        int nargs, size_t steps_cap) {
    asmtrace_prov_t prov = {"emu-l0", 1, "exact", 0, NULL, 0};
    asmtrace_writer_t w;
    asmtest_valtrace_t *vt = NULL;
    asmtest_defuse_t *g = NULL;
    char path[1024], body[65536];
    size_t nsteps, nrecs, cur = 0, s;
    int rc;

    vt = asmtest_valtrace_new(4096, 65536, 4096);
    if (!vt) {
        fprintf(stderr, "asmtrace_record: out of memory\n");
        return -1;
    }
    rc = asmtest_dataflow_emu_run_arch(ASMTEST_ARCH_ARM32, code, code_len, args,
                                       nargs, 0, vt);
    if (rc < 0) {
        fprintf(stderr,
                "asmtrace_record: arm32 emulator producer failed for %s\n",
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
    set_code_identity(&w, out, code, code_len);
    asmtrace_writer_set_arch(&w, "arm"); /* the guest, not the host (T1) */
    asmtrace_header(&w, "asmtrace_record", &prov, 0, NULL);

    {
        char text[256];
        int o = snprintf(text, sizeof text, "%s(", label);
        for (int i = 0; i < nargs; i++)
            o += snprintf(text + o, sizeof text - (size_t)o, "%s%ld",
                          i ? ", " : "", args[i]);
        snprintf(text + o, sizeof text - (size_t)o,
                 ") under the deterministic emulator L0 producer, ARM32 guest");
        asmtrace_escape(body, sizeof body, text);
        asmtrace_emitf(&w, "note", "\"text\":\"%s\"", body);
    }

    nsteps = vt->steps_len;
    nrecs = vt->recs_len;

    for (s = 0; s < nsteps; s++) {
        char dis[160] = "";
        if (emu_disas_available())
            emu_disas(EMU_ARCH_ARM32, code, code_len, REC_CODE_BASE,
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

    for (s = 0; s < nsteps; s++) {
        char dis[160] = "";
        size_t first;
        if (emu_disas_available())
            emu_disas(EMU_ARCH_ARM32, code, code_len, REC_CODE_BASE,
                      vt->insn_off[s], dis, sizeof dis);
        while (cur < nrecs && vt->recs[cur].step < s)
            cur++;
        first = cur;
        while (cur < nrecs && vt->recs[cur].step == s)
            cur++;
        asmtrace_df_step_body(body, sizeof body, (unsigned)s, vt->insn_off[s],
                              REC_CODE_BASE, 0, dis, &vt->recs[first],
                              cur - first, vt->wide, vt->wide_len);
        asmtrace_emit(&w, "df_step", body);
    }

    for (size_t i = 0; g && i < g->n; i++) {
        asmtrace_df_edge_body(body, sizeof body, &g->edges[i]);
        asmtrace_emit(&w, "df_edge", body);
    }

    /* Per-step arm32 register states from the ring (T1), after the value
     * stream — the same ordering emit_regstates_arm64 uses. A non-zero
     * eviction count is truncation just like a full recs buffer. */
    uint64_t step_dropped =
        emit_regstates_arm32(&w, code, code_len, args, nargs, steps_cap);

    if (vt->truncated || step_dropped > 0)
        w.truncated = 1;
    asmtrace_writer_set_steps_total(&w, vt->steps_total);
    asmtrace_close(&w, step_dropped, 0, NULL);

    {
        size_t nedges = g ? g->n : (size_t)0;
        int trunc = vt->truncated || step_dropped > 0;
        asmtest_defuse_free(g);
        asmtest_valtrace_free(vt);
        printf("  %-20s %zu steps, %zu records, %zu def-use edges", out, nsteps,
               nrecs, nedges);
        if (steps_cap > 0)
            printf(", regstate cap %zu (dropped %llu)", steps_cap,
                   (unsigned long long)step_dropped);
        printf("  [arm32]%s\n", trunc ? " [TRUNCATED]" : "");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* RISC-V (RV64) worked example (60-arm32-riscv-author-mode.md T3)     */
/*                                                                     */
/* Mirrors the arm64/arm32 listings immediately above: a BYTE LITERAL, */
/* hand-derivable on paper (assembled with `.option norvc`, so every   */
/* instruction is the base 4-byte RV64I encoding — no compressed forms,*/
/* matching the fixed-stride offsets the arm64/arm32 chains have).     */
/* Run through the riscv64 `df_guest` under Unicorn (which emulates    */
/* RV64 on any host), generated on the SAME x86-64 lane as every other */
/* golden — the guest is the arch, not the host.                       */
/* ------------------------------------------------------------------ */

/* riscv_df_chain(a, b)  [a0=a, a1=b] — the SAME straight-line def-use chain
 * shape as ARM64_DF_CHAIN/ARM32_DF_CHAIN, in RV64I:
 *   0x00  add a2, a0, a1  ; 33 06 b5 00   a2 = a + b   (reads a0,a1; writes a2)
 *   0x04  add a3, a2, a2  ; b3 06 c6 00   a3 = 2 * a2  (edge a2; writes a3)
 *   0x08  mv  a0, a3      ; 13 85 06 00   a0 = a3      (edge a3; the return value)
 *   0x0c  ret             ; 67 80 00 00   return via ra (jalr x0, 0(ra))
 * With a = 7, b = 5 the fabric is a2 = 12, a3 = 24, a0 = 24, and the def-use
 * graph has edges 0->1 (a2) and 1->2 (a3) — the same L0->L1 story the
 * arm64/arm32 chains tell, under the riscv64 guest. */
static const uint8_t RISCV_DF_CHAIN[] = {
    0x33, 0x06, 0xb5, 0x00, /* 0x00 add a2, a0, a1 */
    0xb3, 0x06, 0xc6, 0x00, /* 0x04 add a3, a2, a2 */
    0x13, 0x85, 0x06, 0x00, /* 0x08 mv  a0, a3     */
    0x67, 0x80, 0x00, 0x00, /* 0x0c ret            */
};

/* One RISC-V per-step register snapshot as a `regstate` event, by descriptor
 * reference — the riscv64 analogue of emit_regstate_arm64/emit_regstate_arm32.
 * `values` names the integer register file only: x0..x31 and pc, in
 * emu_riscv_regs_t declaration order — integer-only, no F/D-extension deck,
 * mirroring both the arm64/arm32 descriptors and the riscv64 value fabric's
 * own integer-only scope (60-arm32-riscv-author-mode.md T3). */
static void emit_regstate_riscv(asmtrace_writer_t *w,
                                const emu_riscv_regs_t *r) {
    char body[2048];
    int o = snprintf(
        body, sizeof body,
        "\"desc\":\"emu_riscv_regs_t@riscv64/lp64\",\"values\":{"
        "\"x0\":%llu,\"x1\":%llu,\"x2\":%llu,\"x3\":%llu,\"x4\":%llu,"
        "\"x5\":%llu,\"x6\":%llu,\"x7\":%llu,\"x8\":%llu,\"x9\":%llu,"
        "\"x10\":%llu,\"x11\":%llu,\"x12\":%llu,\"x13\":%llu,\"x14\":%llu,"
        "\"x15\":%llu,\"x16\":%llu,\"x17\":%llu,\"x18\":%llu,\"x19\":%llu,"
        "\"x20\":%llu,\"x21\":%llu,\"x22\":%llu,\"x23\":%llu,\"x24\":%llu,"
        "\"x25\":%llu,\"x26\":%llu,\"x27\":%llu,\"x28\":%llu,\"x29\":%llu,"
        "\"x30\":%llu,\"x31\":%llu,\"pc\":%llu}",
        (unsigned long long)r->x[0], (unsigned long long)r->x[1],
        (unsigned long long)r->x[2], (unsigned long long)r->x[3],
        (unsigned long long)r->x[4], (unsigned long long)r->x[5],
        (unsigned long long)r->x[6], (unsigned long long)r->x[7],
        (unsigned long long)r->x[8], (unsigned long long)r->x[9],
        (unsigned long long)r->x[10], (unsigned long long)r->x[11],
        (unsigned long long)r->x[12], (unsigned long long)r->x[13],
        (unsigned long long)r->x[14], (unsigned long long)r->x[15],
        (unsigned long long)r->x[16], (unsigned long long)r->x[17],
        (unsigned long long)r->x[18], (unsigned long long)r->x[19],
        (unsigned long long)r->x[20], (unsigned long long)r->x[21],
        (unsigned long long)r->x[22], (unsigned long long)r->x[23],
        (unsigned long long)r->x[24], (unsigned long long)r->x[25],
        (unsigned long long)r->x[26], (unsigned long long)r->x[27],
        (unsigned long long)r->x[28], (unsigned long long)r->x[29],
        (unsigned long long)r->x[30], (unsigned long long)r->x[31],
        (unsigned long long)r->pc);
    if (o < 0)
        return;
    size_t n = (size_t)o >= sizeof body ? sizeof body - 1 : (size_t)o;
    body[n] = '\0';
    asmtrace_emit(w, "regstate", body);
}

/* Per-step RISC-V register ring (T3): when steps_cap > 0, run the SAME bytes
 * again under a FRESH emu_riscv_t handle with its per-step ring armed
 * (emu_riscv_step_capture) and emit one `regstate` event per held pre-state
 * (oldest first) — the riscv64 analogue of emit_regstates_arm64/
 * emit_regstates_arm32. A SEPARATE Unicorn engine from the value-fabric run
 * above, exactly like the arm64/arm32 pairs; EMU_CODE_BASE/EMU_STACK_BASE
 * match DF_CODE_BASE/DF_STACK_BASE numerically, so both engines see the same
 * addresses even though neither shares state with the other. Returns the
 * number of earliest steps the ring evicted. */
static uint64_t emit_regstates_riscv(asmtrace_writer_t *w, const uint8_t *code,
                                     size_t code_len, const long *args,
                                     int nargs, size_t steps_cap) {
    emu_riscv_t *e;
    emu_riscv_result_t res;
    uint64_t dropped = 0;
    if (steps_cap == 0)
        return 0;
    e = emu_riscv_open();
    if (e == NULL)
        return 0;
    if (emu_riscv_step_capture(e, steps_cap)) {
        size_t held, i;
        memset(&res, 0, sizeof res);
        emu_riscv_call(e, code, code_len, args, nargs, 0, &res);
        held = emu_riscv_step_count(e);
        dropped = emu_riscv_step_dropped(e);
        for (i = 0; i < held; i++) {
            emu_riscv_regs_t regs;
            if (!emu_riscv_step_at(e, i, NULL, &regs))
                continue;
            emit_regstate_riscv(w, &regs);
        }
    }
    emu_riscv_close(e);
    return dropped;
}

/* Record one RISC-V routine as a value-fabric golden (60-arm32-riscv-author-
 * mode.md T3), written as `note` + `trace` (rel) + `df_step` + `df_edge`
 * through the riscv64 `df_guest` — plus, when `steps_cap > 0`, the regstate/
 * Scrubber half: one `regstate` event per held pre-state of the riscv64
 * per-step ring (emu_riscv_regs_t@riscv64/lp64). The header's `arch` is
 * accurately "riscv64" (the guest, via the writer override) even though the
 * recording is produced on an x86-64 lane. No mem/blame: those ride their
 * own opt-ins and are not armed here. The reader needs NO change either
 * way, mirroring record_arm64's/record_arm32's own closing note. Returns
 * 0 / -1. */
static int record_riscv(const char *dir, const char *out, const char *label,
                        const uint8_t *code, size_t code_len,
                        const long *args, int nargs, size_t steps_cap) {
    asmtrace_prov_t prov = {"emu-l0", 1, "exact", 0, NULL, 0};
    asmtrace_writer_t w;
    asmtest_valtrace_t *vt = NULL;
    asmtest_defuse_t *g = NULL;
    char path[1024], body[65536];
    size_t nsteps, nrecs, cur = 0, s;
    int rc;

    vt = asmtest_valtrace_new(4096, 65536, 4096);
    if (!vt) {
        fprintf(stderr, "asmtrace_record: out of memory\n");
        return -1;
    }
    rc = asmtest_dataflow_emu_run_arch(ASMTEST_ARCH_RISCV64, code, code_len,
                                       args, nargs, 0, vt);
    if (rc < 0) {
        fprintf(stderr,
                "asmtrace_record: riscv64 emulator producer failed for %s\n",
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
    set_code_identity(&w, out, code, code_len);
    asmtrace_writer_set_arch(&w, "riscv64"); /* the guest, not the host (T3) */
    asmtrace_header(&w, "asmtrace_record", &prov, 0, NULL);

    {
        char text[256];
        int o = snprintf(text, sizeof text, "%s(", label);
        for (int i = 0; i < nargs; i++)
            o += snprintf(text + o, sizeof text - (size_t)o, "%s%ld",
                          i ? ", " : "", args[i]);
        snprintf(
            text + o, sizeof text - (size_t)o,
            ") under the deterministic emulator L0 producer, RISC-V guest");
        asmtrace_escape(body, sizeof body, text);
        asmtrace_emitf(&w, "note", "\"text\":\"%s\"", body);
    }

    nsteps = vt->steps_len;
    nrecs = vt->recs_len;

    for (s = 0; s < nsteps; s++) {
        char dis[160] = "";
        if (emu_disas_available())
            emu_disas(EMU_ARCH_RISCV64, code, code_len, REC_CODE_BASE,
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

    for (s = 0; s < nsteps; s++) {
        char dis[160] = "";
        size_t first;
        if (emu_disas_available())
            emu_disas(EMU_ARCH_RISCV64, code, code_len, REC_CODE_BASE,
                      vt->insn_off[s], dis, sizeof dis);
        while (cur < nrecs && vt->recs[cur].step < s)
            cur++;
        first = cur;
        while (cur < nrecs && vt->recs[cur].step == s)
            cur++;
        asmtrace_df_step_body(body, sizeof body, (unsigned)s, vt->insn_off[s],
                              REC_CODE_BASE, 0, dis, &vt->recs[first],
                              cur - first, vt->wide, vt->wide_len);
        asmtrace_emit(&w, "df_step", body);
    }

    for (size_t i = 0; g && i < g->n; i++) {
        asmtrace_df_edge_body(body, sizeof body, &g->edges[i]);
        asmtrace_emit(&w, "df_edge", body);
    }

    /* Per-step riscv64 register states from the ring (T3), after the value
     * stream — the same ordering emit_regstates_arm64/emit_regstates_arm32
     * use. A non-zero eviction count is truncation just like a full recs
     * buffer. */
    uint64_t step_dropped =
        emit_regstates_riscv(&w, code, code_len, args, nargs, steps_cap);

    if (vt->truncated || step_dropped > 0)
        w.truncated = 1;
    asmtrace_writer_set_steps_total(&w, vt->steps_total);
    asmtrace_close(&w, step_dropped, 0, NULL);

    {
        size_t nedges = g ? g->n : (size_t)0;
        int trunc = vt->truncated || step_dropped > 0;
        asmtest_defuse_free(g);
        asmtest_valtrace_free(vt);
        printf("  %-20s %zu steps, %zu records, %zu def-use edges", out, nsteps,
               nrecs, nedges);
        if (steps_cap > 0)
            printf(", regstate cap %zu (dropped %llu)", steps_cap,
                   (unsigned long long)step_dropped);
        printf("  [riscv64]%s\n", trunc ? " [TRUNCATED]" : "");
    }
    return 0;
}

/* One per-step register snapshot as a `regstate` event, by descriptor
 * reference (schema: `desc` names a state descriptor, never a bare inline
 * register list). The `values` object carries the full x86-64 INTEGER file —
 * the 16 GP registers plus rip and rflags, in emu_x86_regs_t declaration order,
 * each a decimal u64.
 *
 * R4 (31-wide-register-deck.md): when `want_vec`, the wide FP/vector deck rides
 * alongside — the 16 128-bit XMM registers (each a hex `bytes` string) plus MXCSR
 * (a u32) — appended to the SAME `values` object through the one field-order owner
 * asmtrace_regstate_vec_append, so the emulator and the live producer spell the
 * deck identically. Off by default (`--fpregs`), so an unarmed recording is
 * byte-identical to the pre-R4 golden (D6). The emulator's per-step ring already
 * captures xmm[16]; MXCSR comes from the parallel ring (emu_step_mxcsr_at). */
static void emit_regstate(asmtrace_writer_t *w, const emu_x86_regs_t *r,
                          int want_vec, uint32_t mxcsr) {
    char body[2048];
    size_t o = (size_t)snprintf(
        body, sizeof body,
        "\"desc\":\"emu_x86_regs_t@x86_64/sysv\",\"values\":{"
        "\"rax\":%llu,\"rbx\":%llu,\"rcx\":%llu,\"rdx\":%llu,"
        "\"rsi\":%llu,\"rdi\":%llu,\"rbp\":%llu,\"rsp\":%llu,"
        "\"r8\":%llu,\"r9\":%llu,\"r10\":%llu,\"r11\":%llu,"
        "\"r12\":%llu,\"r13\":%llu,\"r14\":%llu,\"r15\":%llu,"
        "\"rip\":%llu,\"rflags\":%llu",
        (unsigned long long)r->rax, (unsigned long long)r->rbx,
        (unsigned long long)r->rcx, (unsigned long long)r->rdx,
        (unsigned long long)r->rsi, (unsigned long long)r->rdi,
        (unsigned long long)r->rbp, (unsigned long long)r->rsp,
        (unsigned long long)r->r8, (unsigned long long)r->r9,
        (unsigned long long)r->r10, (unsigned long long)r->r11,
        (unsigned long long)r->r12, (unsigned long long)r->r13,
        (unsigned long long)r->r14, (unsigned long long)r->r15,
        (unsigned long long)r->rip, (unsigned long long)r->rflags);
    if (o >= sizeof body)
        o = sizeof body - 1;
    if (want_vec) {
        uint8_t xb[16][16];
        for (int i = 0; i < 16; i++)
            memcpy(xb[i], r->xmm[i].u8, 16);
        o = asmtrace_regstate_vec_append(body, sizeof body, o, xb, mxcsr);
    }
    if (o < sizeof body - 1)
        body[o++] = '}';
    body[o] = '\0';
    asmtrace_emit(w, "regstate", body);
}

/* Copy the emulator's per-step register file into the pure `asmtest_regfile_t`
 * the statediff body-builder diffs — the SAME 18 fields (rax..r15, rip, rflags),
 * so the delta uses one field-order owner (asmtrace_ndjson.c). */
static void regs_to_regfile(asmtest_regfile_t *rf, const emu_x86_regs_t *r) {
    rf->rax = r->rax;
    rf->rbx = r->rbx;
    rf->rcx = r->rcx;
    rf->rdx = r->rdx;
    rf->rsi = r->rsi;
    rf->rdi = r->rdi;
    rf->rbp = r->rbp;
    rf->rsp = r->rsp;
    rf->r8 = r->r8;
    rf->r9 = r->r9;
    rf->r10 = r->r10;
    rf->r11 = r->r11;
    rf->r12 = r->r12;
    rf->r13 = r->r13;
    rf->r14 = r->r14;
    rf->r15 = r->r15;
    rf->rip = r->rip;
    rf->rflags = r->rflags;
}

/* Per-step register ring (09-teaching-producers.md T2). When steps_cap > 0,
 * re-run the SAME bytes under the emu_t handle with an armed capture ring and
 * emit one `regstate` event per held pre-state (oldest first). Returns the
 * number of earliest steps the ring EVICTED (emu_step_dropped): the caller
 * folds a non-zero count into the footer as truncation, so an over-cap run is
 * faithful data — the held deck is steps [dropped, dropped+count) and the footer
 * says how many fell off the front. The ring is x86-64-guest only, exactly the
 * corpus's own arch gate; where emu_step_capture cannot arm (non-x86-64 or an
 * allocation failure) nothing is written and 0 is returned.
 *
 * When `want_statediff` (33 R6 T2), each `regstate` is paired 1:1 with a
 * `statediff` event carrying the CHANGED-register delta from the previous held
 * step. The first held step has no known predecessor (its true predecessor is
 * either genuine step 0 or the evicted prefix), so it emits `computed:false` with
 * an empty delta — a full delta there would be a D7 lie. statediff carries an
 * absolute step (dropped + held-index) so a two-recording merge aligns
 * truncation-robustly. Off by default so a regstate-only golden is byte-unchanged. */
static uint64_t emit_regstates(asmtrace_writer_t *w, const uint8_t *code,
                               size_t code_len, const long *args, int nargs,
                               const double *fargs, int nfargs,
                               size_t steps_cap, int want_statediff,
                               int want_vec) {
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
        asmtest_regfile_t prev, cur;
        int have_prev = 0;
        memset(&res, 0, sizeof res);
        /* R4 T3: with SSE-class args (nfargs > 0) the deck must re-run through the
         * FP call path so xmm0..7 carry the double args; else the integer path. */
        if (nfargs > 0)
            emu_call_fp(e, code, code_len, args, nargs, fargs, nfargs, 0, &res);
        else
            emu_call(e, code, code_len, args, nargs, 0, &res);
        held = emu_step_count(e);
        dropped = emu_step_dropped(e);
        for (i = 0; i < held; i++) {
            emu_x86_regs_t regs;
            uint32_t mxcsr = 0;
            if (!emu_step_at(e, i, NULL, &regs))
                continue;
            /* R4: the per-step MXCSR from the parallel ring — 0 if the ring could
             * not be allocated, which the vec serializer emits faithfully. */
            if (want_vec)
                emu_step_mxcsr_at(e, i, &mxcsr);
            emit_regstate(w, &regs, want_vec, mxcsr);
            /* R4 T2: the FP-environment, decoded from the same MXCSR the deck
             * carried — paired 1:1 with the regstate, only where the deck is armed
             * (a step with no captured MXCSR emits no fpenv, never a default). */
            if (want_vec) {
                char fb[256];
                asmtrace_fpenv_body(fb, sizeof fb, (unsigned)(dropped + i),
                                    mxcsr);
                asmtrace_emit(w, "fpenv", fb);
            }
            if (want_statediff) {
                char body[512];
                regs_to_regfile(&cur, &regs);
                asmtrace_statediff_body(body, sizeof body,
                                        (unsigned)(dropped + i),
                                        have_prev ? &prev : NULL, &cur);
                asmtrace_emit(w, "statediff", body);
                prev = cur;
                have_prev = 1;
            }
        }
    }
    emu_close(e);
    return dropped;
}

static int record_bytes_fp(const char *dir, const char *out, const char *label,
                           const uint8_t *code, size_t code_len,
                           const long *args, int nargs, const double *fargs,
                           int nfargs, int want_fpregs, size_t recs_cap,
                           size_t steps_cap, int want_mem, int want_blame,
                           int want_statediff);

/* The integer-arg recorder — the historical signature every corpus / loom / scene
 * caller uses (no FP args, no baked-in vector deck). Thin wrapper over the FP form
 * so those callsites stay unchanged and byte-stable. */
static int record_bytes(const char *dir, const char *out, const char *label,
                        const uint8_t *code, size_t code_len, const long *args,
                        int nargs, size_t recs_cap, size_t steps_cap,
                        int want_mem, int want_blame, int want_statediff) {
    return record_bytes_fp(dir, out, label, code, code_len, args, nargs, NULL,
                           0, 0, recs_cap, steps_cap, want_mem, want_blame,
                           want_statediff);
}

/*
 * Record `code[0..code_len)` under the producer and write <dir>/<out>.asmtrace.
 * `label` is what the recording's `note` calls the routine; `recs_cap` bounds
 * the operand buffer (a SMALL cap is how the truncated loom fixture is made —
 * the producer then flips `truncated` and the footer says so, which is a D7
 * low-fidelity fixture rather than a hand-edited file). `steps_cap` arms the
 * per-step register ring (0 = no regstate); a small cap over a longer routine
 * is how the register-ring truncation fixture is made, the same way.
 *
 * R4 T3: `fargs`/`nfargs` are SSE-class double args — non-zero routes BOTH the L0
 * value producer and the regstate ring through the FP call path so xmm0..7 carry
 * them. `want_fpregs` bakes in the wide XMM/MXCSR deck for this recording (like
 * add_signed bakes in its ring cap), independent of the corpus-wide --fpregs.
 * Returns 0 on success, -1 on a setup/write failure.
 */
static int record_bytes_fp(const char *dir, const char *out, const char *label,
                           const uint8_t *code, size_t code_len,
                           const long *args, int nargs, const double *fargs,
                           int nfargs, int want_fpregs, size_t recs_cap,
                           size_t steps_cap, int want_mem, int want_blame,
                           int want_statediff) {
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
    /* R4 T3: the FP producer when SSE-class args are present (xmm operand values
     * ride wide[]); the integer producer otherwise (byte-unchanged for the corpus). */
    rc = (nfargs > 0)
             ? asmtest_dataflow_emu_run_fp(code, code_len, args, nargs, fargs,
                                           nfargs, 0, vt)
             : asmtest_dataflow_emu_run(code, code_len, args, nargs, 0, vt);
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
    set_code_identity(&w, out, code, code_len);
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
        /* 37 T4: `when` is omitted (0) — this recorder holds no codeimage
         * timeline (it emits none), so its output never carries the field and
         * the golden corpus does not churn. */
        asmtrace_df_step_body(body, sizeof body, (unsigned)s, vt->insn_off[s],
                              REC_CODE_BASE, 0, dis, &vt->recs[first],
                              cur - first, vt->wide, vt->wide_len);
        asmtrace_emit(&w, "df_step", body);
    }

    /* The `mem` address stream (29 R2), when armed: a contiguous block of one
     * `mem` per memory access, after the df_step stream it projects. Off by
     * default (want_mem 0), so the golden corpus is unchanged unless a fixture or
     * --mem arms it — the same discipline --steps uses for regstate. */
    if (want_mem || g_mem)
        emit_mem_stream(&w, vt->recs, nrecs);

    for (size_t i = 0; g && i < g->n; i++) {
        asmtrace_df_edge_body(body, sizeof body, &g->edges[i]);
        asmtrace_emit(&w, "df_edge", body);
    }

    /* The `blame` backward-attribution (33 R6 T1), when armed: one blame event
     * over the def-use graph `g` just emitted, seeded at the PENULTIMATE step
     * (nsteps-2) — the last instruction before the `ret`. For an epilogue-free
     * routine (the dedicated blame fixtures) that is the return value's producer;
     * for a routine with a callee-save restore between the value and the `ret`
     * (`... mov rax, rbx; pop rbx; ret`) it seeds the restore instead. Either way
     * the emitted cone is the EXACT backward slice of the seeded step — the seed is
     * a which-value POLICY, never a source of fidelity loss. Whatever the seed, a sink
     * with no producer is the born-untraced verdict. Off by default so the golden
     * corpus is unchanged unless a fixture or --blame arms it; needs g (a routine
     * that produced no def-use graph carries none, e.g. the scene/ABI paths). */
    if ((want_blame || g_blame) && g != NULL && nsteps >= 1) {
        uint32_t seed =
            (nsteps >= 2) ? (uint32_t)(nsteps - 2) : (uint32_t)(nsteps - 1);
        emit_blame(&w, vt, g, seed);
    }

    /* Per-step register states from the ring (T2), after the value stream.
     * A non-zero eviction count is truncation just like a full recs buffer.
     * `want_statediff || g_statediff` pairs each regstate with a step-delta. */
    step_dropped = emit_regstates(
        &w, code, code_len, args, nargs, fargs, nfargs, steps_cap,
        want_statediff || g_statediff, want_fpregs || g_fpregs);
    if (vt->truncated || step_dropped > 0)
        w.truncated = 1;
    /* The total step count (28 R1 T2): the L0 producer saw them all (steps_total
     * counts past the cap), so a truncation banner can read "N of M". */
    asmtrace_writer_set_steps_total(&w, vt->steps_total);

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
                        r->nargs, 65536, steps_cap, 0 /* mem via g_mem */,
                        0 /* blame via g_blame */,
                        0 /* statediff via g_statediff */);
}

/* Record one 3D-overview golden SCENE (10-spacetime-3d-overview.md T7): the
 * SAME emulator L0 run record_bytes makes, written in the only two kinds the
 * scene consumes — `codeimage` (the region that becomes terrain) and
 * ABSOLUTE-basis `trace` (the PC trajectory) — and nothing else. No df_step /
 * df_edge / regstate: a scene golden that carried them would be pinning the
 * value producer's field order twice, which the corpus loop already does.
 *
 * The basis is accurately "abs". The producer maps the routine window at
 * REC_CODE_BASE (src/dataflow_emu.c), so REC_CODE_BASE + insn_off IS the
 * address the guest executed — not a rebasing invented at write time. The
 * corpus loop writes the same measurement region-relative because the trace
 * canvas reads offsets from a routine entry; the overview needs true addresses
 * to place a cell on the address-space plane, which is why this second spelling
 * exists rather than a reader guessing one from the other.
 *
 * `codeimage` carries the bytes the producer actually mapped, at version 0 and
 * when 0 — one snapshot, no churn: this scene is not a JIT fixture.
 *
 * trace_cap > 0 caps how many of the executed steps the recording HOLDS — a
 * filled trace buffer, exactly the append-until-full discipline
 * include/asmtest_trace.h documents. The writer then flips `truncated` and the
 * footer's drops.lost carries the steps that did not fit, so every touched
 * terrain cell is flagged TORN (a floor, never an erasure). Generated, never
 * hand-edited, for the loom-truncated reason: a truncation fixture nobody can
 * produce is a fixture nobody can trust.
 *
 * Returns 0 on success, -1 on a setup/write failure. */
static int record_scene_abs(const char *dir, const char *out, const char *label,
                            const uint8_t *code, size_t code_len,
                            const long *args, int nargs, size_t trace_cap) {
    asmtrace_prov_t prov = {"emu-l0", 1, "exact", 0, NULL, 0};
    asmtrace_writer_t w;
    asmtest_valtrace_t *vt = NULL;
    char path[1024], body[65536], text[1024];
    char hexbytes[2 * REC_WINDOW + 1];
    size_t nsteps, kept, s;
    unsigned long long lost = 0;
    int rc;

    if (code_len > REC_WINDOW) {
        fprintf(stderr,
                "asmtrace_record: scene %s exceeds the %d-byte window\n", out,
                REC_WINDOW);
        return -1;
    }
    vt = asmtest_valtrace_new(4096, 65536, 4096);
    if (!vt) {
        fprintf(stderr, "asmtrace_record: out of memory\n");
        return -1;
    }
    rc = asmtest_dataflow_emu_run(code, code_len, args, nargs, 0, vt);
    if (rc < 0) {
        fprintf(stderr, "asmtrace_record: emulator producer failed for %s\n",
                out);
        asmtest_valtrace_free(vt);
        return -1;
    }
    nsteps = vt->steps_len;
    kept = (trace_cap && trace_cap < nsteps) ? trace_cap : nsteps;
    lost = (unsigned long long)(nsteps - kept);

    snprintf(path, sizeof path, "%s/%s.asmtrace", dir, out);
    if (asmtrace_open(&w, path, 1 /* deterministic */) != 0) {
        fprintf(stderr, "asmtrace_record: cannot write %s\n", path);
        asmtest_valtrace_free(vt);
        return -1;
    }
    set_code_identity(&w, out, code, code_len);
    asmtrace_header(&w, "asmtrace_record", &prov, 0, NULL);

    /* A note that explains the scene without a sidecar: which routine, why the
     * basis is absolute, and — when capped — exactly what was lost. */
    {
        int o =
            snprintf(text, sizeof text, "3D-overview golden scene: %s(", label);
        for (int i = 0; i < nargs; i++)
            o += snprintf(text + o, sizeof text - (size_t)o, "%s%ld",
                          i ? ", " : "", args[i]);
        o += snprintf(text + o, sizeof text - (size_t)o,
                      ") under the deterministic emulator L0 producer, written "
                      "as `codeimage` + ABSOLUTE-basis `trace`. The producer "
                      "maps the routine window at 0x%lx, so each `off` is the "
                      "address the guest executed; the loop body's repeats are "
                      "the terrain's heat.",
                      REC_CODE_BASE);
        if (lost)
            snprintf(text + o, sizeof text - (size_t)o,
                     " The trace buffer holds %zu of the %zu executed steps: "
                     "the footer's truncated + drops.lost say so, and every "
                     "touched terrain cell is TORN — a floor, not an erasure.",
                     kept, nsteps);
        asmtrace_escape(body, sizeof body, text);
        asmtrace_emitf(&w, "note", "\"text\":\"%s\"", body);
    }

    for (s = 0; s < code_len; s++)
        snprintf(hexbytes + 2 * s, 3, "%02x", code[s]);
    hexbytes[2 * code_len] = '\0';
    asmtrace_emitf(&w, "codeimage",
                   "\"base\":%llu,\"len\":%llu,\"version\":0,\"when\":0,"
                   "\"bytes\":\"%s\"",
                   (unsigned long long)REC_CODE_BASE,
                   (unsigned long long)code_len, hexbytes);

    for (s = 0; s < kept; s++) {
        char dis[160] = "";
        unsigned long long addr =
            (unsigned long long)REC_CODE_BASE + vt->insn_off[s];
        if (emu_disas_available())
            emu_disas(EMU_ARCH_X86_64, code, code_len, REC_CODE_BASE,
                      vt->insn_off[s], dis, sizeof dis);
        if (dis[0]) {
            asmtrace_escape(body, sizeof body, dis);
            asmtrace_emitf(&w, "trace",
                           "\"basis\":\"abs\",\"kind\":\"insn\",\"off\":%llu,"
                           "\"disasm\":\"%s\"",
                           addr, body);
        } else {
            asmtrace_emitf(&w, "trace",
                           "\"basis\":\"abs\",\"kind\":\"insn\",\"off\":%llu",
                           addr);
        }
    }

    if (lost)
        w.truncated = 1;
    /* steps_total (28 R1 T2): the scene ran nsteps and holds `kept`; lost carries
     * the rest, so the total is nsteps. */
    asmtrace_writer_set_steps_total(&w, vt->steps_total);
    asmtrace_close(&w, lost, 0, NULL);
    asmtest_valtrace_free(vt);
    printf("  %-28s %zu of %zu step(s) held, 1 codeimage%s\n", out, kept,
           nsteps, lost ? "  [TRUNCATED]" : "");
    return 0;
}

/* Record one 3D-overview golden SCENE in the LIVE DATAFLOW shape (36 T4): the
 * same emulator L0 run as record_scene_abs, but written the way a live serve
 * `dataflow`/`auto` session writes it — an ABSOLUTE `codeimage` (the region that
 * becomes terrain) plus REGION-RELATIVE `df_step` events (the PC path), and NO
 * `trace`. This is the shape no other committed golden carries: the continuous
 * fixture has df_step but no codeimage, and every other df golden also carries a
 * rel `trace`, so 36's single-codeimage anchor fallback never fires against a
 * committed file until this one exists. The desktop reads it unconditionally —
 * the terrain places a labelled single-step residency rung anchored to the span,
 * and the trajectory anchors the df_step offsets onto the same plane.
 *
 * `codeimage` is emitted VERBATIM from record_scene_abs (same base/len/bytes);
 * the df_step offsets are `vt->insn_off[s]` — REGION-RELATIVE by construction,
 * exactly as dataflow_ptrace.c writes `pc - base_ip`. No df_edge/regstate/ops:
 * the scene consumes only the PC path + the region (all the anchor and the
 * residency rung need), and — like record_scene_abs's `trace` — writing operands
 * here would pin the value producer's field order a second time.
 *
 * Returns 0 on success, -1 on a setup/write failure. */
static int record_scene_df(const char *dir, const char *out, const char *label,
                           const uint8_t *code, size_t code_len,
                           const long *args, int nargs) {
    asmtrace_prov_t prov = {"emu-l0", 1, "exact", 0, NULL, 0};
    asmtrace_writer_t w;
    asmtest_valtrace_t *vt = NULL;
    char path[1024], body[65536], text[1024];
    char hexbytes[2 * REC_WINDOW + 1];
    size_t nsteps, s;
    int rc;

    if (code_len > REC_WINDOW) {
        fprintf(stderr,
                "asmtrace_record: scene %s exceeds the %d-byte window\n", out,
                REC_WINDOW);
        return -1;
    }
    vt = asmtest_valtrace_new(4096, 65536, 4096);
    if (!vt) {
        fprintf(stderr, "asmtrace_record: out of memory\n");
        return -1;
    }
    rc = asmtest_dataflow_emu_run(code, code_len, args, nargs, 0, vt);
    if (rc < 0) {
        fprintf(stderr, "asmtrace_record: emulator producer failed for %s\n",
                out);
        asmtest_valtrace_free(vt);
        return -1;
    }
    nsteps = vt->steps_len;

    snprintf(path, sizeof path, "%s/%s.asmtrace", dir, out);
    if (asmtrace_open(&w, path, 1 /* deterministic */) != 0) {
        fprintf(stderr, "asmtrace_record: cannot write %s\n", path);
        asmtest_valtrace_free(vt);
        return -1;
    }
    set_code_identity(&w, out, code, code_len);
    asmtrace_header(&w, "asmtrace_record", &prov, 0, NULL);

    {
        int o = snprintf(text, sizeof text,
                         "3D-overview golden scene (live dataflow shape): %s(",
                         label);
        for (int i = 0; i < nargs; i++)
            o += snprintf(text + o, sizeof text - (size_t)o, "%s%ld",
                          i ? ", " : "", args[i]);
        snprintf(
            text + o, sizeof text - (size_t)o,
            ") under the deterministic emulator L0 producer, written as a "
            "live serve dataflow session does: an absolute `codeimage` at "
            "0x%lx plus REGION-RELATIVE `df_step` offsets, and NO `trace`. "
            "36 anchors the offsets to the single codeimage span (base+off) "
            "and the terrain draws a labelled single-step residency rung.",
            REC_CODE_BASE);
        asmtrace_escape(body, sizeof body, text);
        asmtrace_emitf(&w, "note", "\"text\":\"%s\"", body);
    }

    for (s = 0; s < code_len; s++)
        snprintf(hexbytes + 2 * s, 3, "%02x", code[s]);
    hexbytes[2 * code_len] = '\0';
    asmtrace_emitf(&w, "codeimage",
                   "\"base\":%llu,\"len\":%llu,\"version\":0,\"when\":0,"
                   "\"bytes\":\"%s\"",
                   (unsigned long long)REC_CODE_BASE,
                   (unsigned long long)code_len, hexbytes);

    /* df_step: step + REGION-RELATIVE off + rbase (37, the region base) + disasm
     * (no ops). Field order is the schema's step, off, rbase?, disasm? — the
     * `"step":N,"off":N` prefix cli_smoke greps stays first, and rbase == the
     * codeimage base so a reader resolves the span from the wire. */
    for (s = 0; s < nsteps; s++) {
        char dis[160] = "";
        unsigned long long off = (unsigned long long)vt->insn_off[s];
        unsigned long long rbase = (unsigned long long)REC_CODE_BASE;
        if (emu_disas_available())
            emu_disas(EMU_ARCH_X86_64, code, code_len, REC_CODE_BASE,
                      vt->insn_off[s], dis, sizeof dis);
        if (dis[0]) {
            asmtrace_escape(body, sizeof body, dis);
            asmtrace_emitf(&w, "df_step",
                           "\"step\":%zu,\"off\":%llu,\"rbase\":%llu,"
                           "\"disasm\":\"%s\"",
                           s, off, rbase, body);
        } else {
            asmtrace_emitf(&w, "df_step",
                           "\"step\":%zu,\"off\":%llu,\"rbase\":%llu", s, off,
                           rbase);
        }
    }

    asmtrace_writer_set_steps_total(&w, vt->steps_total);
    asmtrace_close(&w, 0, 0, NULL);
    asmtest_valtrace_free(vt);
    printf("  %-28s %zu step(s), 1 codeimage, df_step (no trace)\n", out,
           nsteps);
    return 0;
}

/* Record a TWO-SPAN 3D-overview golden (37 T6): the multi-span shape 37 exists to
 * resolve, which no committed artifact carried. TWO absolute `codeimage` spans at
 * two bases plus REGION-RELATIVE `df_step` events tagged (rbase) to one span or
 * the other, and NO `trace`. 36's single-codeimage anchor MUST refuse this (it
 * cannot derive one base from two spans); 37 resolves each step from its stated
 * rbase, so both spans place. Synthetic by construction — the emulator runs one
 * routine and the two spans carry the SAME bytes at two bases with the steps split
 * between them — but the recorder controls its own map, so this is a REAL test, not
 * a lane that can only self-skip.
 *
 * Returns 0 on success, -1 on a setup/write failure. */
static int record_scene_df_multi(const char *dir, const char *out,
                                 const char *label, const uint8_t *code,
                                 size_t code_len, const long *args, int nargs) {
    asmtrace_prov_t prov = {"emu-l0", 1, "exact", 0, NULL, 0};
    asmtrace_writer_t w;
    asmtest_valtrace_t *vt = NULL;
    char path[1024], body[65536], text[1024];
    char hexbytes[2 * REC_WINDOW + 1];
    size_t nsteps, s;
    int rc;
    /* Two code spans: A at the producer's mapping base, B a fixed offset above. */
    const unsigned long long baseA = (unsigned long long)REC_CODE_BASE;
    const unsigned long long baseB =
        (unsigned long long)REC_CODE_BASE + 0x10000ULL;

    if (code_len > REC_WINDOW) {
        fprintf(stderr,
                "asmtrace_record: scene %s exceeds the %d-byte window\n", out,
                REC_WINDOW);
        return -1;
    }
    vt = asmtest_valtrace_new(4096, 65536, 4096);
    if (!vt) {
        fprintf(stderr, "asmtrace_record: out of memory\n");
        return -1;
    }
    rc = asmtest_dataflow_emu_run(code, code_len, args, nargs, 0, vt);
    if (rc < 0) {
        fprintf(stderr, "asmtrace_record: emulator producer failed for %s\n",
                out);
        asmtest_valtrace_free(vt);
        return -1;
    }
    nsteps = vt->steps_len;

    snprintf(path, sizeof path, "%s/%s.asmtrace", dir, out);
    if (asmtrace_open(&w, path, 1 /* deterministic */) != 0) {
        fprintf(stderr, "asmtrace_record: cannot write %s\n", path);
        asmtest_valtrace_free(vt);
        return -1;
    }
    set_code_identity(&w, out, code, code_len);
    asmtrace_header(&w, "asmtrace_record", &prov, 0, NULL);

    {
        int o = snprintf(
            text, sizeof text,
            "3D-overview golden scene (two-span dataflow shape): %s(", label);
        for (int i = 0; i < nargs; i++)
            o += snprintf(text + o, sizeof text - (size_t)o, "%s%ld",
                          i ? ", " : "", args[i]);
        snprintf(
            text + o, sizeof text - (size_t)o,
            ") — TWO absolute `codeimage` spans at 0x%llx and 0x%llx plus "
            "REGION-RELATIVE `df_step` events tagged (rbase) to one span or "
            "the other, NO `trace`. 36's single-span anchor MUST refuse this "
            "(two spans, no derivable base); 37 resolves each step from its "
            "stated rbase, so both spans place.",
            baseA, baseB);
        asmtrace_escape(body, sizeof body, text);
        asmtrace_emitf(&w, "note", "\"text\":\"%s\"", body);
    }

    for (s = 0; s < code_len; s++)
        snprintf(hexbytes + 2 * s, 3, "%02x", code[s]);
    hexbytes[2 * code_len] = '\0';
    /* Two codeimage spans, the SAME bytes, at two bases. */
    asmtrace_emitf(&w, "codeimage",
                   "\"base\":%llu,\"len\":%llu,\"version\":0,\"when\":0,"
                   "\"bytes\":\"%s\"",
                   baseA, (unsigned long long)code_len, hexbytes);
    asmtrace_emitf(&w, "codeimage",
                   "\"base\":%llu,\"len\":%llu,\"version\":0,\"when\":0,"
                   "\"bytes\":\"%s\"",
                   baseB, (unsigned long long)code_len, hexbytes);

    /* df_step: alternate the region tag between the two spans, so a reader must
     * resolve each step by its OWN rbase — precisely the case 36 refuses. */
    for (s = 0; s < nsteps; s++) {
        char dis[160] = "";
        unsigned long long off = (unsigned long long)vt->insn_off[s];
        unsigned long long rb = (s & 1) ? baseB : baseA;
        if (emu_disas_available())
            emu_disas(EMU_ARCH_X86_64, code, code_len, REC_CODE_BASE,
                      vt->insn_off[s], dis, sizeof dis);
        if (dis[0]) {
            asmtrace_escape(body, sizeof body, dis);
            asmtrace_emitf(&w, "df_step",
                           "\"step\":%zu,\"off\":%llu,\"rbase\":%llu,"
                           "\"disasm\":\"%s\"",
                           s, off, rb, body);
        } else {
            asmtrace_emitf(&w, "df_step",
                           "\"step\":%zu,\"off\":%llu,\"rbase\":%llu", s, off,
                           rb);
        }
    }

    asmtrace_writer_set_steps_total(&w, vt->steps_total);
    asmtrace_close(&w, 0, 0, NULL);
    asmtest_valtrace_free(vt);
    printf(
        "  %-28s %zu step(s), 2 codeimage spans, df_step tagged (no trace)\n",
        out, nsteps);
    return 0;
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
    set_code_identity(&w, out, code, code_len);
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
        /* The ABI x-ray legs stay GPR-only — arming the vector deck here would
         * churn their committed goldens for no marshalling story (R4 want_vec=0). */
        if (emu_step_at(e, i, NULL, &regs))
            emit_regstate(&w, &regs, 0, 0);
    }

    /* Stops last, so file order IS ordinal order — the player's contract. The
     * reference (SysV) leg carries them; the Win64 leg is pure paired data. */
    for (i = 0; i < (size_t)nstops; i++)
        emit_abi_stop(&w, &stops[i]);

    /* steps_total (28 R1 T2): the ring cap holds every step of a pair, so the
     * total is exactly the executed count — never truncated here. */
    asmtrace_writer_set_steps_total(&w, (unsigned long long)nsteps);
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
        else if (strcmp(argv[i], "--mem") == 0)
            g_mem =
                1; /* 29 R2: arm the mem[] address stream for the whole corpus */
        else if (strcmp(argv[i], "--fpregs") == 0)
            g_fpregs =
                1; /* 31 R4: arm the XMM/MXCSR deck for the whole corpus */
        else if (strcmp(argv[i], "--blame") == 0)
            g_blame =
                1; /* 33 R6 T1: arm the blame backward-attribution stream */
        else if (strcmp(argv[i], "--statediff") == 0)
            g_statediff = 1; /* 33 R6 T2: arm the statediff step-delta stream */
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
                         sizeof LOOM_DF_CHAIN, df_args, 2, 65536, 0, 0, 0,
                         0) != 0)
            failed++;
        /* The D7 low-fidelity fixture: a four-record operand buffer fills mid-run,
         * the producer flips `truncated`, and the footer declares it. Generated,
         * never hand-edited — a fixture nobody can produce is a fixture nobody
         * can trust. */
        if (record_bytes(dir, "loom-truncated", "df_chain (recs_cap = 4)",
                         LOOM_DF_CHAIN, sizeof LOOM_DF_CHAIN, df_args, 2, 4, 0,
                         0, 0, 0) != 0)
            failed++;
        if (record_bytes(dir, "loom-fork-demo", "fork_demo", LOOM_FORK_DEMO,
                         sizeof LOOM_FORK_DEMO, fork_args, 1, 65536, 0, 0, 0,
                         0) != 0)
            failed++;
        /* The `mem` address-stream golden (29 R2 T2): df_chain armed with --mem.
         * The listing has an explicit store (mov [rsp-8], rax) and load
         * (mov rcx, [rsp-8]), so the recording carries `mem` events with real
         * effective addresses — the 3D rich rung's per-access density input. The
         * value stream is uncapped, so it is the HAPPY path (never truncated). */
        if (record_bytes(dir, "mem-df-chain", "df_chain (--mem)", LOOM_DF_CHAIN,
                         sizeof LOOM_DF_CHAIN, df_args, 2, 65536, 0,
                         1 /* --mem */, 0, 0) != 0)
            failed++;
        /* The D7 torn twin: the SAME --mem capture with a five-record operand
         * buffer, sized so the store's `mem` write survives while the tail
         * truncates (footer flips). The rich rung then sees BOTH a `mem` stream and
         * a torn recording: the touched cells are a floor, flagged TORN, never a
         * silent full terrain. `-truncated` in the name so the golden test asserts
         * truncated IFF named. Generated, never hand-edited. */
        if (record_bytes(dir, "mem-df-chain-truncated",
                         "df_chain (--mem, recs_cap = 5)", LOOM_DF_CHAIN,
                         sizeof LOOM_DF_CHAIN, df_args, 2, 5, 0, 1 /* --mem */,
                         0, 0) != 0)
            failed++;
        /* The `blame` backward-attribution golden (33 R6 T1): df_chain armed with
         * --blame. Seeded at the penultimate step (`mov rax, rdx`), the cone is the
         * full value chain back to the argument — a TRACED cone (born_untraced
         * false). The consumer socket cones over the same df_edge stream. */
        if (record_bytes(dir, "blame-df-chain", "df_chain (--blame)",
                         LOOM_DF_CHAIN, sizeof LOOM_DF_CHAIN, df_args, 2, 65536,
                         0, 0, 1 /* --blame */, 0) != 0)
            failed++;
        /* The D7 fidelity fixture for T1: `identity(a) = a` (mov rax, rdi; ret).
         * Its penultimate step reads rdi — an ARGUMENT, produced by no traced
         * instruction — so the backward slice is the sink ALONE and the blame is
         * born_untraced: a truthful "provenance starts at instrumentation" verdict,
         * never an empty cone. */
        if (record_bytes(dir, "blame-untraced", "identity (--blame)",
                         BLAME_UNTRACED, sizeof BLAME_UNTRACED, df_args, 1,
                         65536, 0, 0, 1 /* --blame */, 0) != 0)
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
                             sizeof rbx, rbx_args, 2, 65536, 0, 0, 0, 0) != 0)
                failed++;
            /* The T2 register-ring low-fidelity fixture (09-teaching-producers.md):
             * the SAME sum_via_rbx bytes with a TWO-entry step ring, so it holds
             * only the last two pre-states and evicts the rest. The operand
             * buffer is UNcapped (65536), so this truncation is the register
             * ring's alone — the footer's `truncated` flips and `drops.lost`
             * carries the evicted count, exactly the D7 fidelity loom-truncated
             * shows for operands. The name ends in `-truncated` because the
             * golden corpus test (desktop/test/test_golden.cpp) requires a flat
             * golden to be truncated IFF so named. Generated, never hand-edited. */
            if (record_bytes(dir, "regstate-truncated",
                             "sum_via_rbx (steps_cap = 2)", rbx, sizeof rbx,
                             rbx_args, 2, 65536, 2, 0, 0, 0) != 0)
                failed++;
            /* The `statediff` step-delta golden (33 R6 T2): sum_via_rbx with the
             * register ring armed (steps_cap holds every step) AND --statediff, so
             * each regstate is paired 1:1 with its step-to-step delta. The first
             * held step carries computed:false (no known predecessor). */
            if (record_bytes(dir, "statediff-sum-via-rbx",
                             "sum_via_rbx (--steps --statediff)", rbx,
                             sizeof rbx, rbx_args, 2, 65536, 16, 0, 0,
                             1 /* --statediff */) != 0)
                failed++;
            /* The D7 torn twin: the SAME bytes with a TWO-entry step ring, so the
             * earliest pre-states are evicted. The first HELD statediff then has an
             * evicted predecessor — computed:false, empty delta — proving a
             * step-delta is never fabricated across a truncation boundary.
             * `-truncated` in the name so the golden test asserts truncated IFF. */
            if (record_bytes(dir, "statediff-sum-via-rbx-truncated",
                             "sum_via_rbx (steps_cap = 2, --statediff)", rbx,
                             sizeof rbx, rbx_args, 2, 65536, 2, 0, 0,
                             1 /* --statediff */) != 0)
                failed++;
        }
    }

    /* The FP/vector worked example (31 R4 T3): fp_scale_add(1.5, 2.25) recorded with
     * SSE-class args marshalled into xmm0/xmm1 and the wide XMM/MXCSR deck armed
     * (want_fpregs), so the committed corpus exercises the vector `regstate` deck +
     * `fpenv` without --fpregs — the FP analogue of add_signed's regstate example. */
    {
        static const double fp_args[2] = {1.5, 2.25};
        if (record_bytes_fp(dir, "fp-scale-add", "fp_scale_add", FP_SCALE_ADD,
                            sizeof FP_SCALE_ADD, NULL, 0, fp_args, 2,
                            1 /* want_fpregs */, 65536, 8 /* steps_cap */, 0, 0,
                            0) != 0)
            failed++;
    }

    /* The 3D spacetime overview's two golden SCENES
     * (10-spacetime-3d-overview.md T7): the coarse rung's happy path and its
     * torn twin, both from the SAME bytes so the only difference between them
     * is the truncation the second one declares. */
    {
        static const long scene_args[1] = {3};
        if (record_scene_abs(dir, "scene-abs-loop", "scene_hot_loop",
                             SCENE_HOT_LOOP, sizeof SCENE_HOT_LOOP, scene_args,
                             1, 0) != 0)
            failed++;
        /* Cap 6 of the 13 executed steps: the held prefix still covers three
         * distinct cells and hits the loop body twice, so the torn scene is a
         * real terrain with real heat that happens to be a lower bound — not a
         * degenerate one-cell file whose TORN flag is all there is to see. */
        if (record_scene_abs(dir, "scene-abs-loop-truncated", "scene_hot_loop",
                             SCENE_HOT_LOOP, sizeof SCENE_HOT_LOOP, scene_args,
                             1, 6) != 0)
            failed++;
        /* 36 T4: the SAME bytes in the live dataflow shape — absolute codeimage
         * + region-relative df_step, no trace. The one committed golden that
         * exercises 36's anchor + the terrain's df_step residency rung. */
        if (record_scene_df(dir, "scene-df-loop", "scene_hot_loop",
                            SCENE_HOT_LOOP, sizeof SCENE_HOT_LOOP, scene_args,
                            1) != 0)
            failed++;
        /* 37 T6: the two-span shape 36 must refuse and 37 resolves from the wire
         * — the end-to-end proof that rbase places a multi-span capture. */
        if (record_scene_df_multi(dir, "scene-df-two-span", "scene_hot_loop",
                                  SCENE_HOT_LOOP, sizeof SCENE_HOT_LOOP,
                                  scene_args, 1) != 0)
            failed++;
    }

    /* The AArch64 value-fabric golden (R5 T2, doc 32): the first non-x86-64
     * guest. Generated on the SAME x86-64 lane (Unicorn emulates the arm64
     * guest), so it rides the existing ASMTRACE_GOLDEN_OK gate; deterministic and
     * byte-stable like every other golden. The header's arch is accurately
     * "aarch64" (the guest), and the Loom / Slice / Timeline render its fabric
     * with no desktop change. Baked steps_cap = 8 comfortably holds all four
     * executed steps (add/add/mov/ret), so this is also the arm64 regstate
     * worked example (R5, the regstate/Scrubber half) — the arm64 analogue of
     * add_signed's own baked-in ring, exercising the `regstate` kind without a
     * separate --steps-style flag. */
    {
        static const long arm64_args[2] = {7, 5};
        if (record_arm64(dir, "arm64-df-chain", "arm64_df_chain",
                         ARM64_DF_CHAIN, sizeof ARM64_DF_CHAIN, arm64_args, 2,
                         8) != 0)
            failed++;
        /* The D7 low-fidelity fixture for the arm64 ring (mirrors
         * regstate-truncated.asmtrace): the SAME chain with a two-entry step
         * ring over four executed steps, so the earliest two pre-states are
         * evicted. The value fabric itself is uncapped, so this truncation is
         * the register ring's alone — the footer's `truncated` flips and
         * `drops.lost` counts the evicted prefix, exactly the D7 fidelity
         * `regstate-truncated.asmtrace` shows for x86-64. `-truncated` in the
         * name so the golden test (desktop/test/test_golden.cpp) requires it
         * to actually be truncated. Generated, never hand-edited. */
        if (record_arm64(dir, "arm64-regstate-truncated",
                         "arm64_df_chain (steps_cap = 2)", ARM64_DF_CHAIN,
                         sizeof ARM64_DF_CHAIN, arm64_args, 2, 2) != 0)
            failed++;
    }

    /* The ARM32 value-fabric golden (60-arm32-riscv-author-mode.md T1): the
     * second non-x86-64 guest, the doc-32-shaped mirror of the arm64 block
     * above. Generated on the SAME x86-64 lane (Unicorn emulates the A32
     * guest); deterministic and byte-stable. The header's arch is accurately
     * "arm" (the guest), and the Loom / Slice / Timeline render its fabric
     * with no desktop change. Baked steps_cap = 8 comfortably holds all four
     * executed steps (add/add/mov/bx), so this is also the arm32 regstate
     * worked example, exercising the `regstate` kind without a separate
     * --steps-style flag. */
    {
        static const long arm32_args[2] = {7, 5};
        if (record_arm32(dir, "arm32-df-chain", "arm32_df_chain",
                         ARM32_DF_CHAIN, sizeof ARM32_DF_CHAIN, arm32_args, 2,
                         8) != 0)
            failed++;
        /* The D7 low-fidelity fixture for the arm32 ring (mirrors
         * arm64-regstate-truncated.asmtrace): the SAME chain with a
         * two-entry step ring over four executed steps, so the earliest two
         * pre-states are evicted. `-truncated` in the name so the golden
         * test requires it to actually be truncated. Generated, never
         * hand-edited. */
        if (record_arm32(dir, "arm32-regstate-truncated",
                         "arm32_df_chain (steps_cap = 2)", ARM32_DF_CHAIN,
                         sizeof ARM32_DF_CHAIN, arm32_args, 2, 2) != 0)
            failed++;
    }

    /* The RISC-V (RV64) value-fabric golden (60-arm32-riscv-author-mode.md
     * T3): the third non-x86-64 guest, the doc-32-shaped mirror of the
     * arm64/arm32 blocks above — unblocked by this doc's own Keystone spike
     * (a pinned pre-release upstream commit with a verified-working RISCV
     * backend; see the doc's status section for the exact commit + evidence).
     * Generated on the SAME x86-64 lane (Unicorn emulates the RV64 guest);
     * deterministic and byte-stable. The header's arch is accurately
     * "riscv64" (the guest), and the Loom / Slice / Timeline render its
     * fabric with no desktop change. Baked steps_cap = 8 comfortably holds
     * all four executed steps (add/add/mv/ret), so this is also the riscv64
     * regstate worked example, exercising the `regstate` kind without a
     * separate --steps-style flag. */
    {
        static const long riscv_args[2] = {7, 5};
        if (record_riscv(dir, "riscv-df-chain", "riscv_df_chain",
                         RISCV_DF_CHAIN, sizeof RISCV_DF_CHAIN, riscv_args, 2,
                         8) != 0)
            failed++;
        /* The D7 low-fidelity fixture for the riscv64 ring (mirrors
         * arm64-regstate-truncated.asmtrace/arm32-regstate-truncated.asmtrace):
         * the SAME chain with a two-entry step ring over four executed
         * steps, so the earliest two pre-states are evicted. `-truncated`
         * in the name so the golden test requires it to actually be
         * truncated. Generated, never hand-edited. */
        if (record_riscv(dir, "riscv-regstate-truncated",
                         "riscv_df_chain (steps_cap = 2)", RISCV_DF_CHAIN,
                         sizeof RISCV_DF_CHAIN, riscv_args, 2, 2) != 0)
            failed++;
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
