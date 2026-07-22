/*
 * dataflow_pt.c — the PT + CODE-IMAGE + UNICORN-REPLAY value tier (F5): the
 * least-perturbing L0 value producer in the data-flow design. It reconstructs
 * per-instruction values ENTIRELY OUT OF BAND — no PTRACE_SINGLESTEP, no
 * PTRACE_SINGLEBLOCK, no stop of the target at all. The executed instruction
 * stream is decoded from an Intel PT trace (captured with ZERO single-steps),
 * the bytes that were live at trace time come from the code-image recorder, and
 * that exact path is REPLAYED through the Unicorn emulator to derive the values.
 * The result fills the SAME asmtest_valtrace_t every other producer fills, so the
 * shared def-use (L1) and slice (L2) analysis run over it UNCHANGED (see
 * docs/internal/implementations/dataflow-pt-replay-tier.md).
 *
 * THE HONEST BOUNDARY (inherited from F1/F2). PT gives CONTROL FLOW and BYTES,
 * never VALUES. All values come from the replay, so a region that consumes an
 * unrecorded input — a syscall result, a concurrent sibling write, any
 * nondeterminism — cannot be reconstructed from PT alone and is TRUNCATED
 * HONESTLY, never guessed. Unlike the block-step tier (dataflow_blockstep.c), F5
 * has NO single-step fallback: it is fully out-of-band, so a region it cannot
 * replay is truncated, not stepped. The STRUCTURAL residual, inherent to
 * PT+replay and not liftable by more code:
 *   - Syscall results — F2's record-and-inject needs a BOUNDARY at which the
 *     kernel's retired value is read (PTRACE_SINGLEBLOCK gives it for free because
 *     `syscall` is a control transfer). F5 takes no ptrace stops at all, so there
 *     is no such boundary; the syscall's def cannot be injected. This is the one
 *     place F5 is NARROWER than F2 — a property of being fully out-of-band, not a
 *     gap to fill. The purity gate (T3) declines such a region.
 *   - Concurrency — a sibling thread writing memory the region loads, with no
 *     syscall to anchor it, is unrecordable; the cross-check detects the resulting
 *     path divergence when it changes control flow and truncates, but a divergence
 *     that only changes a VALUE (not the path) is the documented residual the whole
 *     live-attach plan carries.
 *   - Nondeterminism — any input from an unrecorded source makes Unicorn's branch
 *     resolution diverge from the PT path -> the cross-check truncates.
 *
 * OWNERSHIP (binding, doc-set position 9). There is exactly ONE Intel PT arm. The
 * self-trace perf_event_open / AUX open/mmap/drain helpers are owned by
 * intel-pt-whole-window-substrate; intel-pt-attach-foreign-pid extends them for
 * foreign pids. THIS TIER OPENS NO PERF EVENT AND ADDS NO PT CAPTURE CODE. It
 * consumes a captured AUX blob plus a code-image and produces values.
 *
 * It is a SELF-CONTAINED Unicorn client (its own uc_engine, the SAME x86-64 SysV
 * guest layout as src/dataflow_emu.c) rather than an extension of the emulator
 * tier — F5 is its own tier, like blockstep is; it copies the emulator's hook
 * shape and value-capture timing exactly so its records are BYTE-IDENTICAL to the
 * emulator L0 for a deterministic region and the same inputs, and does NOT couple
 * to dataflow_emu.c.
 *
 * Requires Linux x86-64 + Capstone (the operand enumerator + the reused blockstep
 * purity/replayability scan) + Unicorn (the replay); off any of those it compiles
 * to a DF_PT_ENOSYS stub. The PT DECODE bridge (asmtest_dataflow_pt_replay) also
 * needs libipt at compile time; its decode call site is gated on
 * ASMTEST_HAVE_LIBIPT so a Unicorn-only build still links (T1's _replay_path stays
 * usable directly). The synthetic AUX fixture (asmtest_pt_encode_fixture) makes
 * the whole decode->rebase->materialize->replay bridge CI-testable with NO PT
 * hardware; real PT CAPTURE is the one hardware gate (bare-metal Intel PT), owned
 * by the sibling doc.
 */
#include "asmtest_grow.h" /* asmtest_grow / _pow2 — overflow-checked pool growth (S6) */
#include <stddef.h> /* offsetof — the re-declared-struct layout guard */
#include <stdint.h>
#include <string.h> /* memset/memcpy — the ENOSYS stub needs it on every platform */

#include "asmtest_codeimage.h" /* the temporal byte source the bridge materializes from */
#include "asmtest_valtrace.h" /* the shared L0 sink + asmtest_trace.h (asmtest_trace_t) */

/* Return codes from the PT replay producer (kept in step with the suite's copy). Mirrors
 * dataflow_blockstep.c's DF_BLOCKSTEP_* vocabulary so a caller can share the self-skip logic. */
#define DF_PT_OK 0 /* clean replay; a complete value trace                   */
#define DF_PT_FAULT                                                            \
    1 /* path divergence / gate declined / Unicorn fault: partial, truncated */
#define DF_PT_EINVAL (-1) /* bad arguments                                   */
#define DF_PT_ENOSYS (-3) /* off Linux x86-64 / no Capstone / no Unicorn     */

/* Capture telemetry, filled on every non-EINVAL return. The tier ships NO header (a value-trace
 * PRODUCER is a tier, not part of the shared asmtest_valtrace.h sink API); examples/test_dataflow_pt.c
 * re-declares this struct field-for-field, so it carries the same silent-skew hazard the F6/blockstep
 * layout guard exists for. */
typedef struct {
    int pure;             /* the region-scan verdict carried through (T3)     */
    const char *reason;   /* why the replay was declined/truncated, else NULL */
    uint64_t steps;       /* instructions replayed into the trace             */
    uint64_t path_len;    /* offsets in the supplied PT path                  */
    uint64_t diverged_at; /* step index of the first path mismatch, or 0    */
    int vec_seeded;       /* reserved: vector seeding parity with blockstep */
} asmtest_dataflow_pt_info_t;

/* The re-declared-struct layout guard (F6 hazard defence — `size` ALONE does not catch a field
 * landing in tail padding; `last_off`, the final field's offset, moves whenever any earlier field
 * is added/removed/resized). Defined UNCONDITIONALLY (outside the platform gate) so both the real
 * build and the ENOSYS stub report the same true layout, exactly as
 * asmtest_dataflow_blockstep_info_layout does. */
void asmtest_dataflow_pt_info_layout(size_t *size, size_t *last_off) {
    if (size != NULL)
        *size = sizeof(asmtest_dataflow_pt_info_t);
    if (last_off != NULL)
        *last_off = offsetof(asmtest_dataflow_pt_info_t, vec_seeded);
}

/* T3: reuse the blockstep tier's SINGLE, mutation-proven purity/replayability verdicts rather
 * than growing a second scanner that could disagree (F1's central lesson). dataflow_blockstep.c
 * ships no header, so re-declare the two entry points here (and in the suite). Both return 1 (ok),
 * 0 (declined; *reason set), or a negative DF_BLOCKSTEP_* on a bad argument. */
int asmtest_dataflow_blockstep_is_pure(const uint8_t *code, size_t code_len,
                                       const char **reason);
int asmtest_dataflow_blockstep_is_replayable(const uint8_t *code,
                                             size_t code_len,
                                             const char **reason);

#if defined(ASMTEST_HAVE_LIBIPT)
/* T2: the PT DECODE entry lives in pt_backend.c and ships no header (the hwtrace suite
 * re-declares it too). Re-declare the 6-parameter signature — with the base_ip_out offset-origin
 * out-param that intel-pt-whole-window-substrate#T2 already landed — and the one decode return
 * code F5 inspects. Only referenced when libipt is present; a Unicorn-only build never sees it,
 * so the object still links (the bridge below returns DF_PT_FAULT/truncated there). */
#define ASMTEST_HW_EDECODE (-8)
int asmtest_pt_decode_window(const uint8_t *aux, size_t aux_len,
                             const asmtest_codeimage_t *img, uint64_t when,
                             asmtest_trace_t *trace, uint64_t *base_ip_out);
#endif

#if defined(__linux__) && defined(__x86_64__) &&                               \
    defined(ASMTEST_HAVE_CAPSTONE) && defined(ASMTEST_HAVE_UNICORN)

#include <capstone/capstone.h> /* X86_REG_* ids from the operand enumerator */
#include <stdlib.h>
#include <unicorn/unicorn.h>

/* Guest layout — IDENTICAL to src/dataflow_emu.c's constants (which mirror src/emu.c), so a
 * replay of the same bytes/inputs produces byte-identical records, absolute stack addresses and
 * all (both key on offsets, and both map the stack at the same fixed base). */
#define DF_CODE_BASE  0x00100000UL
#define DF_CODE_SIZE  0x00010000UL
#define DF_STACK_BASE 0x00200000UL
#define DF_STACK_SIZE 0x00010000UL
#define DF_RET_MAGIC  0x00f00000UL

/* A one-step scratch buffer of operand records, finalized + appended when the next step begins
 * (or the run ends) — the exact deferred-write discipline of dataflow_emu.c. */
typedef struct {
    at_val_rec_t *v;
    size_t n, cap;
} df_recbuf;

typedef struct {
    uc_engine *uc;
    asmtest_valtrace_t *vt;
    const uint8_t *code;
    size_t code_len;
    uint64_t base;
    int have_cur;
    uint64_t cur_off;
    df_recbuf cur;
    /* --- the PT cross-check (T1) --- */
    const uint64_t
        *path; /* the ordered executed-offset path from the decoded PT trace */
    size_t path_len;
    uint64_t
        step; /* how many instructions have entered df_on_code so far        */
    int diverged; /* set once the executed offset stops matching path[step]      */
    uint64_t
        diverged_at; /* the step index of the first mismatch                        */
} df_pt_ctx;

static void df_recbuf_push(df_recbuf *rb, const at_val_rec_t *r) {
    if (rb->n == rb->cap &&
        !asmtest_grow((void **)&rb->v, &rb->cap, rb->n + 1, sizeof *rb->v))
        return;
    rb->v[rb->n++] = *r;
}

/* Map a Capstone x86 register id to its Unicorn UC_X86_REG_* id, folding a 32/16/8-bit
 * sub-register to its 64-bit container. Returns -1 for an unmodelled register (its value is then
 * simply left uncaptured). Copied verbatim from dataflow_emu.c so the records match. */
static int df_cap_x86_to_uc(uint32_t r) {
    switch (r) {
    case X86_REG_RAX:
    case X86_REG_EAX:
    case X86_REG_AX:
    case X86_REG_AL:
    case X86_REG_AH:
        return UC_X86_REG_RAX;
    case X86_REG_RBX:
    case X86_REG_EBX:
    case X86_REG_BX:
    case X86_REG_BL:
    case X86_REG_BH:
        return UC_X86_REG_RBX;
    case X86_REG_RCX:
    case X86_REG_ECX:
    case X86_REG_CX:
    case X86_REG_CL:
    case X86_REG_CH:
        return UC_X86_REG_RCX;
    case X86_REG_RDX:
    case X86_REG_EDX:
    case X86_REG_DX:
    case X86_REG_DL:
    case X86_REG_DH:
        return UC_X86_REG_RDX;
    case X86_REG_RSI:
    case X86_REG_ESI:
    case X86_REG_SI:
    case X86_REG_SIL:
        return UC_X86_REG_RSI;
    case X86_REG_RDI:
    case X86_REG_EDI:
    case X86_REG_DI:
    case X86_REG_DIL:
        return UC_X86_REG_RDI;
    case X86_REG_RBP:
    case X86_REG_EBP:
    case X86_REG_BP:
    case X86_REG_BPL:
        return UC_X86_REG_RBP;
    case X86_REG_RSP:
    case X86_REG_ESP:
    case X86_REG_SP:
    case X86_REG_SPL:
        return UC_X86_REG_RSP;
    case X86_REG_R8:
        return UC_X86_REG_R8;
    case X86_REG_R9:
        return UC_X86_REG_R9;
    case X86_REG_R10:
        return UC_X86_REG_R10;
    case X86_REG_R11:
        return UC_X86_REG_R11;
    case X86_REG_R12:
        return UC_X86_REG_R12;
    case X86_REG_R13:
        return UC_X86_REG_R13;
    case X86_REG_R14:
        return UC_X86_REG_R14;
    case X86_REG_R15:
        return UC_X86_REG_R15;
    case X86_REG_RIP:
        return UC_X86_REG_RIP;
    case X86_REG_EFLAGS:
        return UC_X86_REG_EFLAGS;
    default:
        return -1;
    }
}

static bool df_reg_read(uc_engine *uc, uint32_t cap_reg, uint64_t *out) {
    int uc_id = df_cap_x86_to_uc(cap_reg);
    if (uc_id < 0)
        return false;
    uint64_t v = 0;
    if (uc_reg_read(uc, uc_id, &v) != UC_ERR_OK)
        return false;
    *out = v;
    return true;
}

/* Fill the current step's deferred register-WRITE values (readable now that the instruction has
 * executed) and append the step to the value trace. */
static void df_finalize(df_pt_ctx *c) {
    for (size_t i = 0; i < c->cur.n; i++) {
        at_val_rec_t *r = &c->cur.v[i];
        if (r->is_write && r->kind == AT_LOC_REG && !r->value_valid) {
            uint64_t val;
            if (df_reg_read(c->uc, r->reg, &val)) {
                r->value = val;
                r->value_valid = true;
            }
        }
    }
    asmtest_valtrace_append(c->vt, c->cur_off, c->cur.v, c->cur.n);
    c->have_cur = 0;
    c->cur.n = 0;
}

/* UC_HOOK_CODE: fires pre-instruction. Finalize the previous step, then CROSS-CHECK the current
 * executed offset against path[step] — the correctness proof (the replay took the real path) AND
 * the nondeterminism detector (a region whose branch depends on an unrecorded input resolves
 * differently in Unicorn than in the PT trace and DIVERGES here). On a mismatch: mark the trace
 * truncated, record diverged_at, STOP the engine, and record nothing for this step. Otherwise
 * open the step exactly as dataflow_emu.c does (register reads now = source state; register
 * writes deferred to the next hook; memory operands left to the mem hooks). */
static void df_on_code(uc_engine *uc, uint64_t address, uint32_t size,
                       void *user) {
    (void)size;
    df_pt_ctx *c = (df_pt_ctx *)user;
    if (c->diverged)
        return;
    if (c->have_cur)
        df_finalize(c);
    uint64_t off = address - c->base;

    if (c->step >= c->path_len || off != c->path[c->step]) {
        c->diverged = 1;
        c->diverged_at = c->step;
        if (c->vt != NULL)
            c->vt->truncated = true;
        uc_emu_stop(uc);
        return;
    }
    c->step++;

    c->cur.n = 0;
    c->cur_off = off;
    c->have_cur = 1;

    at_val_rec_t rd[64], wr[64];
    size_t nr = 64, nw = 64;
    asmtest_operands(ASMTEST_ARCH_X86_64, c->code, c->code_len, off, rd, &nr,
                     wr, &nw);
    for (size_t i = 0; i < nr; i++) {
        if (rd[i].kind != AT_LOC_REG)
            continue; /* mem reads arrive via the mem hook */
        at_val_rec_t r = rd[i];
        uint64_t val;
        if (df_reg_read(c->uc, r.reg, &val)) {
            r.value = val;
            r.value_valid = true;
        }
        df_recbuf_push(&c->cur, &r);
    }
    for (size_t i = 0; i < nw; i++) {
        if (wr[i].kind != AT_LOC_REG)
            continue; /* mem writes arrive via the mem hook */
        at_val_rec_t r = wr[i];
        r.value_valid = false; /* filled at the next code hook / at stop */
        df_recbuf_push(&c->cur, &r);
    }
}

/* UC_HOOK_MEM_READ_AFTER (delivers the loaded value) and UC_HOOK_MEM_WRITE: attach an
 * absolute-address memory record to the current step. */
static void df_on_mem(uc_engine *uc, uc_mem_type type, uint64_t addr, int size,
                      int64_t value, void *user) {
    (void)uc;
    df_pt_ctx *c = (df_pt_ctx *)user;
    if (!c->have_cur)
        return;
    at_val_rec_t r;
    memset(&r, 0, sizeof r);
    r.kind = AT_LOC_MEM_ABS;
    r.addr = addr;
    r.size = (uint16_t)(size < 0 ? 0 : size);
    r.is_write = (type == UC_MEM_WRITE);
    if (r.size <= 8) {
        r.value = (uint64_t)value;
        r.value_valid = true;
    }
    df_recbuf_push(&c->cur, &r);
}

static void df_zero_gp(uc_engine *uc) {
    static const int gp[] = {UC_X86_REG_RAX, UC_X86_REG_RBX, UC_X86_REG_RCX,
                             UC_X86_REG_RDX, UC_X86_REG_RSI, UC_X86_REG_RDI,
                             UC_X86_REG_RBP, UC_X86_REG_R8,  UC_X86_REG_R9,
                             UC_X86_REG_R10, UC_X86_REG_R11, UC_X86_REG_R12,
                             UC_X86_REG_R13, UC_X86_REG_R14, UC_X86_REG_R15};
    uint64_t z = 0;
    for (size_t i = 0; i < sizeof gp / sizeof gp[0]; i++)
        uc_reg_write(uc, gp[i], &z);
    uint64_t flags = 2; /* EFLAGS reserved bit 1 */
    uc_reg_write(uc, UC_X86_REG_EFLAGS, &flags);
}

/*
 * T1 — the PT-path Unicorn replay core.
 *
 * Given the ordered executed-instruction-offset stream `path` (offset 0 = the region's first
 * byte — exactly what a decoded PT trace yields), the region bytes `code`, and SysV integer
 * seed inputs `args`, replay the region through Unicorn, cross-check each executed offset against
 * path[step], and fill *vt with the per-step value trace — byte-for-byte the records the
 * emulator L0 (asmtest_dataflow_emu_run) produces for the same deterministic region and inputs.
 *
 * T3 — before seeding Unicorn, DECLINE any region PT+replay cannot faithfully reconstruct, using
 * the blockstep tier's own verdicts (no second scanner): a NON-REPLAYABLE region (any VEX/EVEX
 * encoding — Unicorn mis-executes VEX-128 SILENTLY with UC_ERR_OK and a wrong answer, so this is
 * a CORRECTNESS gate, not an optimization) and an IMPURE region (a syscall/rdtsc/cpuid whose
 * retired value PT never carries — the emulator would fabricate one). Either truncates honestly
 * with the offending reason and executes NOTHING.
 *
 * Returns DF_PT_OK (clean, path matched step-for-step), DF_PT_FAULT (gate declined / path
 * divergence / Unicorn fault — a partial, truncated trace), DF_PT_EINVAL (bad arguments), or
 * DF_PT_ENOSYS (off platform / no engine — the stub below).
 */
int asmtest_dataflow_pt_replay_path(const uint8_t *code, size_t code_len,
                                    const uint64_t *path, size_t path_len,
                                    const long *args, int nargs,
                                    asmtest_valtrace_t *vt,
                                    asmtest_dataflow_pt_info_t *info) {
    if (info != NULL)
        memset(info, 0, sizeof *info);
    if (vt == NULL || code == NULL || code_len == 0 ||
        (path_len > 0 && path == NULL) || nargs < 0 || nargs > 6 ||
        (nargs > 0 && args == NULL))
        return DF_PT_EINVAL;
    vt->mem_space = AT_LOC_MEM_ABS;
    if (info != NULL)
        info->path_len = path_len;

    /* T3 gates, over the region bytes, BEFORE any execution. Order matters: is_replayable is a
     * correctness gate (a VEX-128 region would run silently WRONG), so it fires first. */
    const char *rreason = NULL;
    int replayable =
        asmtest_dataflow_blockstep_is_replayable(code, code_len, &rreason);
    if (replayable == 0) {
        if (info != NULL)
            info->reason = rreason;
        vt->truncated = true;
        return DF_PT_FAULT;
    }
    const char *preason = NULL;
    int pure = asmtest_dataflow_blockstep_is_pure(code, code_len, &preason);
    if (info != NULL)
        info->pure = (pure == 1) ? 1 : 0;
    if (pure == 0) {
        if (info != NULL)
            info->reason = preason;
        vt->truncated = true;
        return DF_PT_FAULT;
    }

    uc_engine *uc = NULL;
    if (uc_open(UC_ARCH_X86, UC_MODE_64, &uc) != UC_ERR_OK)
        return DF_PT_ENOSYS;
    if (uc_mem_map(uc, DF_CODE_BASE, DF_CODE_SIZE, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_map(uc, DF_STACK_BASE, DF_STACK_SIZE,
                   UC_PROT_READ | UC_PROT_WRITE) != UC_ERR_OK) {
        uc_close(uc);
        return DF_PT_ENOSYS;
    }
    size_t clen = code_len > DF_CODE_SIZE ? DF_CODE_SIZE : code_len;
    if (uc_mem_write(uc, DF_CODE_BASE, code, clen) != UC_ERR_OK) {
        uc_close(uc);
        return DF_PT_ENOSYS;
    }
    df_zero_gp(uc);

    /* SysV: rsp ≡ 8 (mod 16) at entry, sentinel return address on top. */
    static const int arg_regs[6] = {UC_X86_REG_RDI, UC_X86_REG_RSI,
                                    UC_X86_REG_RDX, UC_X86_REG_RCX,
                                    UC_X86_REG_R8,  UC_X86_REG_R9};
    uint64_t sp = DF_STACK_BASE + DF_STACK_SIZE - 8;
    uint64_t ret = DF_RET_MAGIC;
    uc_mem_write(uc, sp, &ret, sizeof ret);
    uc_reg_write(uc, UC_X86_REG_RSP, &sp);
    for (int i = 0; i < nargs && i < 6; i++) {
        uint64_t v = (uint64_t)args[i];
        uc_reg_write(uc, arg_regs[i], &v);
    }

    df_pt_ctx c;
    memset(&c, 0, sizeof c);
    c.uc = uc;
    c.vt = vt;
    c.code = code;
    c.code_len = code_len;
    c.base = DF_CODE_BASE;
    c.path = path;
    c.path_len = path_len;

    uc_hook hcode = 0, hread = 0, hwrite = 0;
    uc_hook_add(uc, &hcode, UC_HOOK_CODE, (void *)df_on_code, &c, 1, 0);
    uc_hook_add(uc, &hread, UC_HOOK_MEM_READ_AFTER, (void *)df_on_mem, &c, 1,
                0);
    uc_hook_add(uc, &hwrite, UC_HOOK_MEM_WRITE, (void *)df_on_mem, &c, 1, 0);

    /* Bounded by construction: df_on_code diverges (and stops) as soon as step >= path_len, so a
     * finite path caps the instruction count without a separate backstop. */
    uc_err err =
        uc_emu_start(uc, DF_CODE_BASE, DF_RET_MAGIC, 0, (size_t)path_len + 1);
    if (!c.diverged && c.have_cur)
        df_finalize(&c); /* last step's deferred writes + append */

    uc_hook_del(uc, hcode);
    uc_hook_del(uc, hread);
    uc_hook_del(uc, hwrite);
    free(c.cur.v);
    uc_close(uc);

    if (info != NULL) {
        info->steps = vt->steps_len;
        info->diverged_at = c.diverged_at;
    }
    if (c.diverged) {
        if (info != NULL && info->reason == NULL)
            info->reason = "path divergence (nondeterministic input?)";
        return DF_PT_FAULT;
    }
    if (err != UC_ERR_OK) {
        vt->truncated = true;
        if (info != NULL && info->reason == NULL)
            info->reason = "unicorn fault";
        return DF_PT_FAULT;
    }
    return DF_PT_OK;
}

/*
 * T2 — the full out-of-band bridge: decode -> rebase -> materialize -> replay.
 *
 * (a) Decode the captured AUX blob against the code-image at trace position `when` into an ordered
 *     instruction stream (offsets from the window's first decoded IP `base_ip`).
 * (b) Rebase those to offsets from `region_base` (insns[i] + base_ip - region_base), dropping any
 *     that fall outside [0, region_len) and truncating on an out-of-region IP (the region boundary).
 * (c) Materialize the region bytes LIVE AT `when` from the code-image (the recorder's whole reason
 *     for existing).
 * (d) Drive T1's asmtest_dataflow_pt_replay_path over that path + those bytes + the seed inputs.
 *
 * The decode needs libipt at compile time; the decode call site is gated on ASMTEST_HAVE_LIBIPT so
 * a Unicorn-only build still links (it then returns DF_PT_FAULT/truncated — there is no path to
 * replay). Real PT capture is hardware-gated and owned by the sibling doc; the synthetic
 * asmtest_pt_encode_fixture exercises this whole path in CI with no PT PMU.
 */
int asmtest_dataflow_pt_replay(const uint8_t *aux, size_t aux_len,
                               const asmtest_codeimage_t *img, uint64_t when,
                               uint64_t region_base, size_t region_len,
                               const long *args, int nargs,
                               asmtest_valtrace_t *vt,
                               asmtest_dataflow_pt_info_t *info) {
    if (info != NULL)
        memset(info, 0, sizeof *info);
    if (vt == NULL || img == NULL || aux == NULL || aux_len == 0 ||
        region_len == 0 || nargs < 0 || nargs > 6 ||
        (nargs > 0 && args == NULL))
        return DF_PT_EINVAL;
    vt->mem_space = AT_LOC_MEM_ABS;

#if defined(ASMTEST_HAVE_LIBIPT)
    /* (a) decode the AUX into an ordered instruction-offset stream. */
    asmtest_trace_t *tr = asmtest_trace_new(4096, 4096);
    if (tr == NULL)
        return DF_PT_ENOSYS;
    uint64_t base_ip = 0;
    int drc = asmtest_pt_decode_window(aux, aux_len, img, when, tr, &base_ip);
    if (drc < 0) { /* ASMTEST_HW_EDECODE / ENOSYS: no complete path to replay */
        asmtest_trace_free(tr);
        vt->truncated = true;
        if (info != NULL)
            info->reason = "pt decode failed (truncated/empty AUX)";
        return DF_PT_FAULT;
    }

    /* (b) rebase base_ip-relative offsets to region_base-relative, dropping/truncating on an IP
     * outside [region_base, region_base+region_len). */
    uint64_t *path = (uint64_t *)malloc((tr->insns_len + 1) * sizeof(uint64_t));
    if (path == NULL) {
        asmtest_trace_free(tr);
        return DF_PT_ENOSYS;
    }
    size_t path_len = 0;
    int out_of_region = 0;
    for (size_t i = 0; i < tr->insns_len; i++) {
        uint64_t abs_ip = base_ip + tr->insns[i];
        if (abs_ip < region_base || abs_ip >= region_base + region_len) {
            out_of_region =
                1; /* left the traced region: the path is truncated here */
            break;
        }
        path[path_len++] = abs_ip - region_base;
    }
    if (tr->truncated)
        out_of_region = 1; /* a dropped-trace AUX yields an incomplete path */
    asmtest_trace_free(tr);

    /* (c) materialize the region bytes live at `when` from the code-image. */
    const uint8_t *bytes = NULL;
    size_t avail = 0;
    int crc = asmtest_codeimage_bytes_at(
        img, (const void *)(uintptr_t)region_base, when, &bytes, &avail);
    if (crc != ASMTEST_CI_OK || bytes == NULL || avail == 0) {
        free(path);
        vt->truncated = true;
        if (info != NULL)
            info->reason = "code-image miss at region_base/when";
        return DF_PT_FAULT;
    }
    uint8_t *region = (uint8_t *)malloc(region_len);
    if (region == NULL) {
        free(path);
        return DF_PT_ENOSYS;
    }
    memset(region, 0, region_len);
    memcpy(region, bytes, avail < region_len ? avail : region_len);

    /* (d) replay the decoded path through T1's core. */
    int rc = asmtest_dataflow_pt_replay_path(region, region_len, path, path_len,
                                             args, nargs, vt, info);
    free(region);
    free(path);
    /* An in-region path that stopped at the region boundary is honestly truncated even when the
     * replayed prefix itself was clean. */
    if (out_of_region) {
        vt->truncated = true;
        if (rc == DF_PT_OK)
            rc = DF_PT_FAULT;
        if (info != NULL && info->reason == NULL)
            info->reason = "path left the traced region (truncated)";
    }
    return rc;
#else
    (void)when;
    (void)region_base;
    (void)args;
    (void)nargs;
    vt->truncated = true;
    if (info != NULL)
        info->reason =
            "libipt absent: no PT decode (exercised in docker-dataflow-pt)";
    return DF_PT_FAULT;
#endif /* ASMTEST_HAVE_LIBIPT */
}

#else /* not (Linux x86-64 + Capstone + Unicorn) */

int asmtest_dataflow_pt_replay_path(const uint8_t *code, size_t code_len,
                                    const uint64_t *path, size_t path_len,
                                    const long *args, int nargs,
                                    asmtest_valtrace_t *vt,
                                    asmtest_dataflow_pt_info_t *info) {
    (void)code;
    (void)code_len;
    (void)path;
    (void)path_len;
    (void)args;
    (void)nargs;
    (void)vt;
    if (info != NULL)
        memset(info, 0, sizeof *info);
    return DF_PT_ENOSYS;
}

int asmtest_dataflow_pt_replay(const uint8_t *aux, size_t aux_len,
                               const asmtest_codeimage_t *img, uint64_t when,
                               uint64_t region_base, size_t region_len,
                               const long *args, int nargs,
                               asmtest_valtrace_t *vt,
                               asmtest_dataflow_pt_info_t *info) {
    (void)aux;
    (void)aux_len;
    (void)img;
    (void)when;
    (void)region_base;
    (void)region_len;
    (void)args;
    (void)nargs;
    (void)vt;
    if (info != NULL)
        memset(info, 0, sizeof *info);
    return DF_PT_ENOSYS;
}

#endif
