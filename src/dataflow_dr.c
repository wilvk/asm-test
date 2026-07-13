/*
 * dataflow_dr.c — the DynamoRIO L0 VALUE producer (Phase 5, increment 1, goal (b)):
 * captures a per-step value trace of a routine running NATIVELY, IN-BAND, whole-
 * process under DynamoRIO, and fills the SAME asmtest_valtrace_t the emulator
 * (dataflow_emu.c) and scoped ptrace (dataflow_ptrace.c) producers fill — so the
 * shared L1 def-use + L2 slicer (dataflow.c) work UNCHANGED on an in-band capture.
 * See docs/internal/plans/data-flow-tracing-plan.md, Phase 5.
 *
 * This is the APP side. It reuses the native-trace tier's DR lifecycle (drtrace_app.c:
 * asmtest_dr_init/start/shutdown + the W^X asmtest_exec_alloc) but loads a DEDICATED
 * value-capture client (src/dataflow_dr_client.c, ASMTEST_DRVAL_CLIENT) instead of the
 * control-flow client. The flow mirrors the in-process native-trace smoke path:
 *   dr_init(value-client) -> dr_start -> asmtest_dr_valcapture_marker(base,len,&drval)
 *   -> run the routine (its per-instruction clean calls append register/memory
 *      snapshots into the app-owned at_drval_t) -> dr_shutdown.
 * DynamoRIO allows ONE in-process lifecycle per process (re-attach is unreliable —
 * drtrace_app.c), so a process captures ONE region's value trace; call again from a
 * fresh process for another.
 *
 * The correctness contract (why the cross-check against the emulator oracle holds):
 * value capture DR-side is just a per-step register-file SNAPSHOT plus each memory
 * load's address+value. This file then REPLAYS those snapshots through the exact
 * operand enumerator (asmtest_operands) and effective-address math the scoped ptrace
 * producer uses — so the per-step read/write RECORD SET (reg ids, implicit eflags/rsp,
 * memory operand addresses) is built identically to the oracle's, and asmtest_defuse_
 * build (which keys on those locations, not on the captured values) yields the same
 * def-use graph and the same slices. Only the value ANNOTATIONS come from DR.
 *
 * Minimal first increment (breadth deferred, and said so): a deterministic, single-
 * threaded LEAF routine of up to six integer arguments over ONE registered region;
 * GP register values (read source state + one-step-deferred write state) and explicit
 * memory-LOAD values (segment-free) are captured. Store VALUES, XMM/YMM vector values
 * (the DR_MC_MULTIMEDIA path), fs:/gs: segment EAs, DR-side taint shadowing, and
 * whole-process breadth beyond the region are LATER increments; store/reg LOCATIONS
 * still enter def-use, resolved from the snapshot, so slices are complete.
 *
 * Requires Capstone (the operand enumerator) and Linux x86-64; off-platform / without
 * Capstone the entry points return DF_DR_ENOSYS and callers self-skip, exactly like
 * the emulator and ptrace producers. Like them it ships NO public header — a value-
 * trace PRODUCER is a tier, not part of the shared asmtest_valtrace.h sink API — so
 * its test re-declares the entry points and these return codes.
 */
#include "asmtest_valtrace.h"

#include "dataflow_dr.h"

/* Return codes from the DR value producer (kept in step with the test's copy). */
#define DF_DR_OK    0 /* returned cleanly; a complete region value trace   */
#define DF_DR_FAULT 1 /* routine faulted; a partial trace is filled        */
#define DF_DR_EINVAL                                                           \
    (-1) /* bad arguments                                     */
#define DF_DR_ENOSYS                                                           \
    (-3) /* off Linux x86-64 / no Capstone: self-skip         */
#define DF_DR_ENODR                                                            \
    (-4) /* no libdynamorio / value client resolvable: self-skip */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The app-side marker the value client resolves by PC and wraps (reading its SysV
 * argument registers at entry). A real exported function — noinline + default
 * visibility give it a stable entry PC dr_get_proc_address can find, and the volatile
 * side effect keeps the body from folding away. Defined unconditionally so the symbol
 * is always present; only the (gated) producer below ever calls it. */
static volatile unsigned long g_valmarker_sink;

__attribute__((noinline, visibility("default"))) void
asmtest_dr_valcapture_marker(void *base, size_t len, void *drval) {
    g_valmarker_sink += 0x77 + (unsigned long)(uintptr_t)base + len +
                        (unsigned long)(uintptr_t)drval;
}

#ifdef ASMTEST_TAINT
/* Taint tier (Increment 4): the app-side SEED marker the taint client resolves by PC
 * (AT_TAINT_SEED_SYM) and wraps — reading rdi=base, rsi=len, rdx=color to paint the
 * shadow before traced code runs. Same stable-entry-PC discipline as the value marker
 * (noinline + default visibility + a volatile side effect). Defined only in the taint
 * build; dr_taint links it and -rdynamic exports its PC. */
static volatile unsigned long g_seedmarker_sink;

__attribute__((noinline, visibility("default"))) void
asmtest_dr_taint_seed_marker(void *base, size_t len, unsigned long color) {
    g_seedmarker_sink += 0x91 + (unsigned long)(uintptr_t)base + len + color;
}

/* The app-side SINK marker the taint client resolves by PC (AT_TAINT_SINK_SYM) and
 * wraps — reading rdi = at_taint_report_t* to register the report the client appends
 * hits into. Same stable-entry-PC discipline as the value/seed markers. */
static volatile unsigned long g_sinkmarker_sink;

__attribute__((noinline, visibility("default"))) void
asmtest_dr_taint_sink_marker(void *report) {
    g_sinkmarker_sink += 0xA3 + (unsigned long)(uintptr_t)report;
}
#endif

#if defined(__linux__) && defined(__x86_64__) && defined(ASMTEST_HAVE_CAPSTONE)

#include "asmtest_drtrace.h" /* DR lifecycle + W^X asmtest_exec_alloc */

#include <capstone/capstone.h> /* X86_REG_* ids from the operand enumerator */

typedef long (*fn6_t)(long, long, long, long, long, long);

/* Map a Capstone x86 register id to its value in a captured GP snapshot, folding a
 * 32/16/8-bit sub-register to its 64-bit container (the record's size carries the real
 * operand width). Mirrors dataflow_ptrace.c's gp_value over the snapshot layout.
 * Returns false for a register not in the GP/flags/rip file (segment selectors, vector
 * regs), whose value is then left uncaptured — exactly as the ptrace producer does.
 *
 * KNOWN LIMITATION (high-byte sub-registers): AH/BH/CH/DH fold to the full 64-bit
 * container like every other sub-register, but they occupy bits 8-15, not the low
 * byte the record's size=1 implies — a consumer that masks the value to `size` bytes
 * reads AL, not AH. Value-only (the raw snapshot is lossless — bits 8-15 recoverable),
 * latent (no current consumer masks-by-size for a high byte), and identical across all
 * three producers (dr/ptrace/emu), so the oracle cannot expose it. A correct fix needs
 * a sub-register byte-offset in the record ABI; deferred until a consumer needs it. */
static bool snap_gp(const at_vstep_t *st, uint32_t reg, uint64_t *out) {
    switch (reg) {
    case X86_REG_RAX:
    case X86_REG_EAX:
    case X86_REG_AX:
    case X86_REG_AL:
    case X86_REG_AH:
        *out = st->gpr[AT_GPR_RAX];
        return true;
    case X86_REG_RBX:
    case X86_REG_EBX:
    case X86_REG_BX:
    case X86_REG_BL:
    case X86_REG_BH:
        *out = st->gpr[AT_GPR_RBX];
        return true;
    case X86_REG_RCX:
    case X86_REG_ECX:
    case X86_REG_CX:
    case X86_REG_CL:
    case X86_REG_CH:
        *out = st->gpr[AT_GPR_RCX];
        return true;
    case X86_REG_RDX:
    case X86_REG_EDX:
    case X86_REG_DX:
    case X86_REG_DL:
    case X86_REG_DH:
        *out = st->gpr[AT_GPR_RDX];
        return true;
    case X86_REG_RSI:
    case X86_REG_ESI:
    case X86_REG_SI:
    case X86_REG_SIL:
        *out = st->gpr[AT_GPR_RSI];
        return true;
    case X86_REG_RDI:
    case X86_REG_EDI:
    case X86_REG_DI:
    case X86_REG_DIL:
        *out = st->gpr[AT_GPR_RDI];
        return true;
    case X86_REG_RBP:
    case X86_REG_EBP:
    case X86_REG_BP:
    case X86_REG_BPL:
        *out = st->gpr[AT_GPR_RBP];
        return true;
    case X86_REG_RSP:
    case X86_REG_ESP:
    case X86_REG_SP:
    case X86_REG_SPL:
        *out = st->gpr[AT_GPR_RSP];
        return true;
    case X86_REG_R8:
        *out = st->gpr[AT_GPR_R8];
        return true;
    case X86_REG_R9:
        *out = st->gpr[AT_GPR_R9];
        return true;
    case X86_REG_R10:
        *out = st->gpr[AT_GPR_R10];
        return true;
    case X86_REG_R11:
        *out = st->gpr[AT_GPR_R11];
        return true;
    case X86_REG_R12:
        *out = st->gpr[AT_GPR_R12];
        return true;
    case X86_REG_R13:
        *out = st->gpr[AT_GPR_R13];
        return true;
    case X86_REG_R14:
        *out = st->gpr[AT_GPR_R14];
        return true;
    case X86_REG_R15:
        *out = st->gpr[AT_GPR_R15];
        return true;
    case X86_REG_RIP:
        *out = st->rip;
        return true;
    case X86_REG_EFLAGS:
        *out = st->rflags;
        return true;
    default:
        return false;
    }
}

/* Resolve a memory operand's effective address from a captured register snapshot:
 * base + index*scale + disp, with the x86 RIP-relative fixup (the EA is relative to
 * the NEXT instruction, so add the instruction's byte length when the base is RIP).
 * Mirrors dataflow_ptrace.c's resolve_ea. Segment (fs:/gs:) bases are NOT in the DR
 * snapshot this increment, so a segmented operand resolves without its base (its value
 * simply will not match a captured load and stays unfilled — segment EAs are a later
 * increment); such operands are absent from the minimal fixtures. */
static uint64_t resolve_ea(const at_vstep_t *st, const at_val_rec_t *r,
                           size_t insn_len) {
    uint64_t ea = (uint64_t)r->disp;
    if (r->base != 0) {
        uint64_t bv;
        if (snap_gp(st, r->base, &bv)) {
            ea += bv;
            if (r->base == X86_REG_RIP)
                ea += insn_len;
        }
    }
    if (r->index != 0 && r->scale != 0) {
        uint64_t iv;
        if (snap_gp(st, r->index, &iv))
            ea += iv * (uint64_t)r->scale;
    }
    return ea;
}

/* Fill a memory READ record's value from the DR-captured load values for this step
 * (matched by effective address — the client and this file compute the identical EA
 * from the same snapshot, so the match is exact). */
static void fill_mem_read(const at_drval_t *dv, const at_vstep_t *st,
                          at_val_rec_t *r) {
    for (uint32_t j = 0; j < st->mem_n; j++) {
        const at_vmem_t *m = &dv->mem[st->mem_first + j];
        if (m->valid && m->ea == r->addr) {
            r->value = m->value;
            r->value_valid = true;
            return;
        }
    }
}

/* Replay the captured per-step snapshots into the shared value trace: at each step,
 * enumerate the read/write set (asmtest_operands), fill register reads from this
 * step's snapshot (source state) and register writes from the NEXT step's snapshot
 * (destination state — the deferred-write model the emulator/ptrace producers use),
 * resolve memory addresses from the snapshot, and annotate memory reads with the
 * DR-captured load value. The LAST step has no successor snapshot, so its register
 * writes stay value-unfilled (their locations still enter def-use).
 *
 * KNOWN LIMITATION (region-exit gap): the deferred-write model assumes steps[i+1] is
 * the dynamic successor of step i. This holds for all IN-region control flow (both
 * edges of a taken branch/back-edge are instrumented), but NOT across a call/jmp to a
 * target OUTSIDE [base,base+len): the un-instrumented callee produces no step, so
 * steps[i+1] is the post-return re-entry, and a `call`'s rsp/rip WRITE gets valued
 * from the return snapshot (rsp rebalanced, rip = return addr) rather than its true
 * post-state. Value-only on the boundary instruction; location-based def-use/slices
 * stay correct; latent on the current leaf/self-contained fixtures, and the emulator
 * oracle (single mapped region) cannot run out-of-region code so cannot expose it.
 * The proper fix is call-out step-over / whole-process capture (a later increment);
 * shared with the clean-call producer, so not inlined-specific. */
static void build_valtrace(asmtest_valtrace_t *vt, const uint8_t *code,
                           size_t code_len, const at_drval_t *dv) {
    vt->mem_space = AT_LOC_MEM_ABS;
    for (size_t i = 0; i < dv->steps_len; i++) {
        const at_vstep_t *st = &dv->steps[i];
        const at_vstep_t *nx =
            (i + 1 < dv->steps_len) ? &dv->steps[i + 1] : NULL;
        uint64_t off = st->off;

        at_val_rec_t rd[64], wr[64];
        size_t nr = 64, nw = 64;
        size_t ilen = asmtest_operands(ASMTEST_ARCH_X86_64, code, code_len, off,
                                       rd, &nr, wr, &nw);

        at_val_rec_t recs[128];
        size_t n = 0;
        for (size_t k = 0; k < nr && n < 128; k++) {
            at_val_rec_t r = rd[k];
            if (r.kind == AT_LOC_REG) {
                uint64_t v;
                if (snap_gp(st, r.reg, &v)) {
                    r.value = v;
                    r.value_valid = true;
                }
            } else {
                r.addr = resolve_ea(st, &r, ilen);
                fill_mem_read(dv, st, &r);
            }
            recs[n++] = r;
        }
        for (size_t k = 0; k < nw && n < 128; k++) {
            at_val_rec_t r = wr[k];
            r.value_valid = false;
            if (r.kind == AT_LOC_REG) {
                uint64_t v;
                if (nx != NULL && snap_gp(nx, r.reg, &v)) {
                    r.value = v;
                    r.value_valid = true;
                }
            } else {
                r.addr =
                    resolve_ea(st, &r, ilen); /* store value: later increment */
            }
            recs[n++] = r;
        }
        asmtest_valtrace_append(vt, off, recs, n);
    }
}

/* 1 if this build can run the DR value producer AND a value client + libdynamorio
 * resolve, else 0 — the self-skip probe (mirrors asmtest_dr_available). */
int asmtest_dataflow_dr_available(void) {
    const char *client = getenv("ASMTEST_DRVAL_CLIENT");
    if (client == NULL || client[0] == '\0')
        return 0;
    return asmtest_dr_available() ? 1 : 0;
}

/*
 * Run `code` (x86-64 SysV, integer args in rdi..r9) NATIVELY under DynamoRIO and fill
 * *vt with its per-step value trace. `result` (if non-NULL) receives the routine's
 * return value. `max_insns` sizes the capture (0 = a generous default). Returns
 * DF_DR_OK on a clean capture, DF_DR_ENODR when DR / the value client is unavailable
 * (self-skip), or a DF_DR_* error.
 */
int asmtest_dataflow_dr_run(const uint8_t *code, size_t code_len,
                            const long *args, int nargs, uint64_t max_insns,
                            long *result, asmtest_valtrace_t *vt) {
    if (vt == NULL || code == NULL || code_len == 0 || nargs < 0 || nargs > 6 ||
        (nargs > 0 && args == NULL))
        return DF_DR_EINVAL;
    vt->mem_space = AT_LOC_MEM_ABS;

    const char *client = getenv("ASMTEST_DRVAL_CLIENT");
    if (client == NULL || client[0] == '\0' || !asmtest_dr_available())
        return DF_DR_ENODR; /* no value client / libdynamorio: caller self-skips */

    size_t steps_cap = max_insns ? (size_t)max_insns : 4096;
    at_drval_t dv;
    memset(&dv, 0, sizeof dv);
    dv.steps = (at_vstep_t *)calloc(steps_cap, sizeof *dv.steps);
    dv.mem = (at_vmem_t *)calloc(steps_cap * 4, sizeof *dv.mem);
    if (dv.steps == NULL || dv.mem == NULL) {
        free(dv.steps);
        free(dv.mem);
        return DF_DR_ENODR;
    }
    dv.steps_cap = steps_cap;
    dv.mem_cap = steps_cap * 4;

    asmtest_drtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.client_path = client;
    if (asmtest_dr_init(&opts) != ASMTEST_DR_OK) {
        free(dv.steps);
        free(dv.mem);
        return DF_DR_ENODR;
    }
    if (asmtest_dr_start() != ASMTEST_DR_OK) {
        asmtest_dr_shutdown();
        free(dv.steps);
        free(dv.mem);
        return DF_DR_ENODR;
    }

    /* Materialize the routine into real executable memory (under DR, like the native-
     * trace smoke path), register it + the capture buffer with the client, run it, and
     * tear DR down (one lifecycle per process). */
    int rc = DF_DR_OK;
    asmtest_exec_code_t exec;
    if (asmtest_exec_alloc(code, code_len, &exec) != ASMTEST_DR_OK) {
        asmtest_dr_shutdown();
        free(dv.steps);
        free(dv.mem);
        return DF_DR_ENODR;
    }
    long a[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < nargs; i++)
        a[i] = args[i];

    /* Optional capture-window timer (ASMTEST_DRVAL_BENCH): brackets ONLY the
     * instrumented run + DR shutdown (the inlined client's drx_buf flush happens
     * at shutdown), isolating the asymmetric per-instruction CAPTURE cost from the
     * symmetric DR init and app-side replay (build_valtrace). Emits one
     * "DRVAL_CAPTURE_NS <n>" line to stderr for the dr-valtrace-bench lane. */
    const char *bench_env = getenv("ASMTEST_DRVAL_BENCH");
    struct timespec cap_t0, cap_t1;
    asmtest_dr_valcapture_marker(exec.base, exec.len, &dv);
    if (bench_env != NULL && bench_env[0] != '\0')
        clock_gettime(CLOCK_MONOTONIC, &cap_t0);
    long r = ((fn6_t)exec.base)(a[0], a[1], a[2], a[3], a[4], a[5]);
    asmtest_dr_shutdown();
    if (bench_env != NULL && bench_env[0] != '\0') {
        clock_gettime(CLOCK_MONOTONIC, &cap_t1);
        unsigned long long cap_ns =
            (unsigned long long)(cap_t1.tv_sec - cap_t0.tv_sec) *
                1000000000ull +
            (unsigned long long)(cap_t1.tv_nsec - cap_t0.tv_nsec);
        fprintf(stderr, "DRVAL_CAPTURE_NS %llu\n", cap_ns);
    }
    if (result != NULL)
        *result = r;

    build_valtrace(vt, code, code_len, &dv);
    if (dv.truncated)
        vt->truncated = true;

    asmtest_exec_free(&exec);
    free(dv.steps);
    free(dv.mem);
    return rc;
}

#ifdef ASMTEST_TAINT
/*
 * Taint tier (Increment 4): run `code` under the DynamoRIO taint client, seeding
 * [seed_base, seed_base+seed_len) with `seed_color` before the routine runs (seed_len=0
 * = the unseeded negative control). Fills *vt with the per-step value trace (identical
 * to asmtest_dataflow_dr_run — the taint capture is ADDITIVE) AND `step_taint[i]` with
 * the union tag the client's inline propagation observed at step i. Returns DF_DR_OK on
 * a clean capture, DF_DR_ENODR when DR/the client is unavailable (self-skip), else a
 * DF_DR_* error. `seed_base` is a REAL app address (the fixture reads from it), so the
 * caller allocates the buffer and passes its address here AND as args[0].
 */
int asmtest_dataflow_dr_taint_run(const uint8_t *code, size_t code_len,
                                  const long *args, int nargs,
                                  uint64_t max_insns, uint64_t seed_base,
                                  uint64_t seed_len, at_tag_t seed_color,
                                  long *result, asmtest_valtrace_t *vt,
                                  at_tag_t *step_taint, size_t step_taint_cap,
                                  at_taint_report_t *report) {
    if (vt == NULL || code == NULL || code_len == 0 || nargs < 0 || nargs > 6 ||
        (nargs > 0 && args == NULL))
        return DF_DR_EINVAL;
    vt->mem_space = AT_LOC_MEM_ABS;

    const char *client = getenv("ASMTEST_DRVAL_CLIENT");
    if (client == NULL || client[0] == '\0' || !asmtest_dr_available())
        return DF_DR_ENODR;

    size_t steps_cap = max_insns ? (size_t)max_insns : 4096;
    at_drval_t dv;
    memset(&dv, 0, sizeof dv);
    dv.steps = (at_vstep_t *)calloc(steps_cap, sizeof *dv.steps);
    dv.mem = (at_vmem_t *)calloc(steps_cap * 4, sizeof *dv.mem);
    if (dv.steps == NULL || dv.mem == NULL) {
        free(dv.steps);
        free(dv.mem);
        return DF_DR_ENODR;
    }
    dv.steps_cap = steps_cap;
    dv.mem_cap = steps_cap * 4;
    /* Wire the caller-owned parallel taint witness (zeroed so untraced slots read
     * clean; the client fills [0, steps_len) in buf_flush). */
    if (step_taint != NULL && step_taint_cap > 0)
        memset(step_taint, 0, step_taint_cap * sizeof *step_taint);
    dv.step_taint = step_taint;
    dv.step_taint_cap = step_taint_cap;

    asmtest_drtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.client_path = client;
    if (asmtest_dr_init(&opts) != ASMTEST_DR_OK) {
        free(dv.steps);
        free(dv.mem);
        return DF_DR_ENODR;
    }
    if (asmtest_dr_start() != ASMTEST_DR_OK) {
        asmtest_dr_shutdown();
        free(dv.steps);
        free(dv.mem);
        return DF_DR_ENODR;
    }

    int rc = DF_DR_OK;
    asmtest_exec_code_t exec;
    if (asmtest_exec_alloc(code, code_len, &exec) != ASMTEST_DR_OK) {
        asmtest_dr_shutdown();
        free(dv.steps);
        free(dv.mem);
        return DF_DR_ENODR;
    }
    long a[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < nargs; i++)
        a[i] = args[i];

    /* Register the capture region + buffer, register the sink report, THEN paint the
     * seed (all rare PC-resolved clean calls in the client, off the hot path), THEN run
     * the routine. */
    asmtest_dr_valcapture_marker(exec.base, exec.len, &dv);
    if (report != NULL) {
        report->hits_len = 0;
        report->hits_total = 0;
        report->truncated = 0;
        asmtest_dr_taint_sink_marker(report);
    }
    if (seed_len > 0)
        asmtest_dr_taint_seed_marker((void *)(uintptr_t)seed_base,
                                     (size_t)seed_len,
                                     (unsigned long)seed_color);
    long r = ((fn6_t)exec.base)(a[0], a[1], a[2], a[3], a[4], a[5]);
    asmtest_dr_shutdown();
    if (result != NULL)
        *result = r;

    build_valtrace(vt, code, code_len, &dv);
    if (dv.truncated)
        vt->truncated = true;

    asmtest_exec_free(&exec);
    free(dv.steps);
    free(dv.mem);
    return rc;
}
#endif /* ASMTEST_TAINT */

#else /* not (Linux x86-64 + Capstone) */

int asmtest_dataflow_dr_available(void) { return 0; }

int asmtest_dataflow_dr_run(const uint8_t *code, size_t code_len,
                            const long *args, int nargs, uint64_t max_insns,
                            long *result, asmtest_valtrace_t *vt) {
    (void)code;
    (void)code_len;
    (void)args;
    (void)nargs;
    (void)max_insns;
    (void)result;
    (void)vt;
    return DF_DR_ENOSYS;
}

#ifdef ASMTEST_TAINT
int asmtest_dataflow_dr_taint_run(const uint8_t *code, size_t code_len,
                                  const long *args, int nargs,
                                  uint64_t max_insns, uint64_t seed_base,
                                  uint64_t seed_len, at_tag_t seed_color,
                                  long *result, asmtest_valtrace_t *vt,
                                  at_tag_t *step_taint, size_t step_taint_cap,
                                  at_taint_report_t *report) {
    (void)code;
    (void)code_len;
    (void)args;
    (void)nargs;
    (void)max_insns;
    (void)seed_base;
    (void)seed_len;
    (void)seed_color;
    (void)result;
    (void)vt;
    (void)step_taint;
    (void)step_taint_cap;
    (void)report;
    return DF_DR_ENOSYS;
}
#endif

#endif
