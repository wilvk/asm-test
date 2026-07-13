/*
 * dataflow_dr_client_inlined.c — the INLINED data-flow L0 VALUE producer client
 * (libasmtest_drval_client_inlined.so) for the DynamoRIO taint tier
 * (dynamorio-taint-tier-plan.md, Increment 3).
 *
 * This is the re-platform of the shipped clean-call client (dataflow_dr_client.c)
 * onto DynamoRIO's standard extension stack: drmgr phased instrumentation, drreg
 * scratch-register reservation, and a drx_buf per-thread trace buffer — the
 * drcachesim/memtrace idiom. It fills the SAME app-owned at_drval_t (dataflow_dr.h)
 * the clean-call client fills, so the app-side replay (dataflow_dr.c) and the
 * emulator-oracle cross-check (dr_valtrace) validate it UNCHANGED: point
 * ASMTEST_DRVAL_CLIENT at this .so instead and the same gate runs. The shipped
 * clean-call client stays intact as the fallback/oracle during the swap.
 *
 * WHY IT IS FASTER. The clean-call client pays a full machine-context save/restore
 * per instrumented instruction (dr_insert_clean_call). This client emits the
 * capture inline: drreg hands out a few scratch GPRs (spilling only those), the
 * capture is a handful of movs/leas, and drx_buf batches the records with one
 * bounds check per step, flushing to at_drval_t in a C callback.
 *
 * WHAT IS CAPTURED (byte-identical to the clean-call client on the fixtures):
 *   - off, rip: COMPILE-TIME constants for each instrumented instruction (its
 *     region offset and app PC), emitted as immediates — no runtime read.
 *   - gpr[16]: the app value of each GP register, via drreg_get_app_value (which
 *     restores from drreg's spill slot the ones drreg borrowed for scratch).
 *   - explicit memory SOURCE operands: EA via an inlined lea over the
 *     app-restored base/index (drreg_restore_app_values), value via an inlined
 *     load. mem_n / sizes are compile-time and emitted as immediates.
 *
 * DELIBERATE DIVERGENCES from the clean call, each principled, not a bug:
 *   - rflags VALUE is clean-call-only and is stored 0 here. The only inline way to
 *     read the full rflags is pushfq, which writes the app red zone ([rsp-8]) — and
 *     the fixture literally stores its live value there, so pushfq would corrupt it.
 *     lahf/seto give only the arith subset, not mc.xflags. Flag def-use LOCATIONS
 *     still enter the graph (the enumerator emits them); only the flag VALUE is
 *     absent. The fixtures do not consume flag values, so the oracle gate is
 *     unaffected. (Surfacing this: full-context fields like rflags are a genuine
 *     reason to keep a narrow clean call — noted for the taint tier.)
 *   - The inlined memory-value load assumes a valid EA (a faulting load crashes the
 *     app, where the clean call's dr_safe_read fails gracefully). The enumeration
 *     gate now bounds this to GENUINE loads: `instr_reads_memory` skips no-load
 *     forms (lea agen, `nop [mem]`, prefetch) whose EA the app never dereferences,
 *     so the only remaining exposure is a real load of a genuinely-unmapped address
 *     — where the app instruction itself faults one step later anyway, so there is
 *     no divergence from unmodified execution. A fully fault-safe inline load is
 *     still a later increment, matching the clean-call client's "breadth deferred"
 *     posture.
 *   - A captured memory operand whose base/index aliases the drx_buf-pointer
 *     register (s_ptr) is skipped (value left unfilled): drreg_restore_app_values
 *     restores that register in place and would destroy the buffer pointer. Rare
 *     (needs drreg to pick a base/index register as scratch under register
 *     pressure); the location still enters def-use via the app-side enumerator.
 *   - RIP-relative / far / segmented (fs:/gs:) memory operands are skipped inline
 *     (the clean call resolves RIP-relative via opnd_compute_address; the fixtures
 *     have none). Same far/segmented exclusion the clean-call client documents.
 *   - DEAD register slots are not literal. dr_get_mcontext (the clean call) snapshots
 *     the actual register file including registers dead at that step; drreg treats a
 *     dead register as free scratch, so drreg_get_app_value returns a scratch/stale
 *     value for it. This affects ONLY never-consumed slots: a register's value that
 *     the value trace never reads (a dead-on-entry incoming value). Every value the
 *     def-use graph / slices actually consume — every live read, and every write
 *     valued from the next step's LIVE snapshot — matches the clean call, which is
 *     why the emulator-oracle cross-check passes identically. Byte-for-byte identity
 *     of the full literal register file is therefore a clean-call-only property, like
 *     rflags; the *semantic* value trace is identical.
 *
 * Linux x86-64 only. Uses the BSD-clean extension stack proved to load by
 * Increment 2 (drmgr/drreg/drx); NO umbra (LGPL-2.1) and NO drwrap — marker/arg
 * resolution stays PC-resolved via dr_get_proc_address + a SysV-arg clean call,
 * exactly as the clean-call client does.
 *
 * ============================ TAINT TIER (Increment 4) ============================
 * Built a SECOND time from this SAME TU under -DASMTEST_TAINT to ship
 * libasmtest_drtaint_client.so (drclient/CMakeLists.txt). Every taint line is under
 * `#ifdef ASMTEST_TAINT`, so with the flag OFF this file compiles byte-for-byte to the
 * Increment-3 value client above and dr-valtrace-inlined-test stays provably untouched.
 *
 * What the flag ADDS (all additive over the value capture, which still runs unchanged):
 *  - A hand-rolled BSD 2-level create-on-touch tag shadow (g_dir -> 1 MiB leaves) over
 *    DR-core dr_raw_mem_alloc — one at_tag_t per app byte, AT_TAG_CLEAN = 0. No umbra
 *    (LGPL-2.1); the tier stays fully BSD. This is the localized growth seam to Inc5.
 *  - A per-thread flat reg-tag file in a drmgr TLS slot (16 GP containers + 1 eflags),
 *    keyed by the DR reg id canonicalized to its 64-bit container. Per-thread => never
 *    races; the memory shadow is process-global with the tolerated-benign-race single-
 *    byte-store policy (aligned at_tag_t writes atomic on x86-64; union monotone within
 *    a seed epoch => a lost update is a conservative MISS, never a false clean->tainted).
 *  - Inline dst_tag = union(src_tags) propagation emitted as an extra phase of THIS
 *    insertion pass, placed after the value capture's mem loop and BEFORE the buffer
 *    pointer advances, so the per-step `taint` witness rides the same drx_buf record
 *    (surfaced parallel-to-steps via dv->step_taint[]). No hot-path clean call.
 *  - on_seed: a rare PC-resolved clean call (the on_marker pattern, no drwrap) that
 *    paints tag_ptr(base..+len) = color at seed time (pre-traced-code, no concurrency).
 *  - Sink slice: on_sink_register (PC-resolved, rdi = at_taint_report_t*) records the
 *    report; a branch-condition sink (kind = 1) appends one at_taint_hit_t at each in-
 *    region conditional branch whose flag is tainted, via a transparent clean call that
 *    reads the eflags tag from this thread's reg-tag file (off the per-instruction path;
 *    seed_off/depth are left 0 and filled app-side by the validator's def-use BFS). The
 *    guarded INLINE skip (no call when the flag is clean) and other sink kinds (mem-len /
 *    call-arg watching a passed-in operand tag) are the next refinement.
 *
 * Store-tag broadcast is real CREATE-ON-TOUCH: the inline fast path stores the tag when
 * the leaf exists, else a first-touch SLOWPATH clean call (on_store_slow) allocates the
 * leaf and writes it — so arbitrary store targets (the managed heap, Increment 5) are
 * handled with no pre-touch, and the slowpath is taken at most once per 1 MiB page.
 *
 * Memory operand tags are BYTE-GRANULAR: a source read unions all `size` shadow bytes
 * into the result (taint in any byte reaches it) and a store writes all `size` bytes.
 * Registers keep whole-register (1-byte) tags this increment. Leaves are allocated one
 * guard page larger than their span so a per-byte access straddling a leaf boundary is
 * fault-safe (the straddling bytes land in the guard = a conservative miss, never a
 * fault or false positive). No first-slice simplifications remain in the native scope;
 * XMM/YMM (SIMD) tags are Increment 8.
 */
#include "dr_api.h"
#include "drmgr.h"
#include "drreg.h"
#include "drx.h"

#include "dataflow_dr.h"

#include <string.h>

/* Up to this many explicit memory source operands captured per instruction. The
 * app allocates mem_cap = 4*steps_cap (dataflow_dr.c), so 4/insn matches. */
#define AT_INLINE_MAXMEM 4

/* The fixed-size per-step record the inlined instrumentation writes into the
 * drx_buf trace buffer; the flush callback reconstructs at_drval_t from it. Kept
 * flat and fixed so drx_buf's stride is a compile-time constant. */
typedef struct {
    uint64_t off;
    uint64_t rip;
    uint64_t gpr[AT_GPR_COUNT];
    uint32_t mem_n;
    uint32_t pad0;
    uint64_t mem_ea[AT_INLINE_MAXMEM];
    uint64_t mem_val[AT_INLINE_MAXMEM];
    uint16_t mem_size[AT_INLINE_MAXMEM];
    uint8_t mem_valid[AT_INLINE_MAXMEM];
#ifdef ASMTEST_TAINT
    /* Per-step taint witness (Increment 4): the union tag observed at this step by the
     * inline propagation phase, written via the same buffer pointer before it advances,
     * drained to dv->step_taint[] in buf_flush. Additive at the struct tail so the
     * flag-off record layout (and every offsetof above) is byte-identical. */
    uint8_t taint;
    uint8_t pad1[3];
#else
    uint8_t pad1[4];
#endif
} raw_step_t;

/* Single registered value-capture region (same contract as the clean-call client:
 * set once before traced code runs, read unlocked on the hot path). */
typedef struct {
    app_pc base;
    size_t len;
    at_drval_t *drval;
} region_t;
static region_t g_region;
static void *g_lock;

static app_pc pc_marker;
static drx_buf_t *g_buf;

/* GP snapshot order (dataflow_dr.h) in DynamoRIO reg ids. */
static const reg_id_t g_gpr_order[AT_GPR_COUNT] = {
    DR_REG_RAX, DR_REG_RBX, DR_REG_RCX, DR_REG_RDX, DR_REG_RSI, DR_REG_RDI,
    DR_REG_RBP, DR_REG_RSP, DR_REG_R8,  DR_REG_R9,  DR_REG_R10, DR_REG_R11,
    DR_REG_R12, DR_REG_R13, DR_REG_R14, DR_REG_R15};

#ifdef ASMTEST_TAINT
/* ================================================================== */
/* Taint tier (Increment 4): BSD 2-level shadow + per-thread reg tags   */
/* ================================================================== */

/* BSD 2-level create-on-touch shadow, 1:1 byte scale. A static directory of leaf
 * pointers (one dr_raw_mem_alloc, demand-zero) indexes 1 MiB leaves allocated
 * zero-filled on first touch and installed by an atomic CAS (the one mandatory-atomic
 * mutation). Canonical user VA (0..2^47) only — covers the raw C stack + heap the
 * fixture uses, so no arena crutch. This IS the localized umbra-swap growth seam. */
#define AT_LEAF_BITS 20
#define AT_LEAF_SPAN                                                           \
    ((size_t)1 << AT_LEAF_BITS) /* 1 MiB per leaf                */
#define AT_LEAF_MASK (AT_LEAF_SPAN - 1)
/* Each leaf is allocated one guard page LARGER than its 1 MiB span, so an inline
 * per-byte tag access on an operand STRADDLING a leaf boundary (offset + size > SPAN)
 * reads/writes into the mapped guard instead of faulting past the mmap. The straddling
 * high bytes then miss the next leaf's real tags (a conservative miss, never a fault or
 * a false positive); the create-on-touch slowpath maps each byte independently, so it
 * has no straddle gap. */
#define AT_LEAF_GUARD 4096
#define AT_LEAF_ALLOC (AT_LEAF_SPAN + AT_LEAF_GUARD)
#define AT_VA_BITS    47 /* canonical x86-64 user VA      */
#define AT_DIR_LEN                                                             \
    ((size_t)1 << (AT_VA_BITS - AT_LEAF_BITS)) /* 2^27 leaf ptrs */

static at_tag_t *
    *g_dir; /* [AT_DIR_LEN], dr_raw_mem_alloc'd once, demand-zero      */

/* Branchless-fallback ZERO region for the inline shadow READ accessor: a null leaf makes
 * a source read hit g_zero_pad (reads clean = 0, a no-op OR — unwritten memory is clean).
 * Sized for a full per-byte operand read (up to 8 bytes) so a multi-byte OR off a null
 * leaf stays in-bounds. Store writes take a create-on-touch slowpath instead, so they
 * need no fallback. g_zero_pad MUST stay all-zero. */
static const uint8_t g_zero_pad[64];

/* Per-thread flat reg-tag file (drmgr TLS): 16 GP containers + 1 eflags slot, keyed by
 * the DR reg id canonicalized to its 64-bit container (whole-register tags this
 * increment). eflags is a location too, so flag-carried flow is representable. */
#define AT_RT_GPR_BASE 0
#define AT_RT_EFLAGS   16
#define AT_RT_COUNT    17
static int g_tls_regfile = -1;

/* App-emitted seed/sink marker PCs (resolved by dr_get_proc_address, like pc_marker),
 * and the app-owned sink report the client appends hits into (registered at the sink
 * marker). Single-threaded native fixture this increment, so appends are unlocked; the
 * launched multithreaded workload (Increment 5) backs g_report with shared memory. */
static app_pc pc_seed_marker;
static app_pc pc_sink_marker;
static at_taint_report_t *g_report;

/* Map a DR reg id to its reg-tag-file index (canonical 64-bit GP container -> 0..15),
 * or -1 if not a tracked GP register. */
static int rt_index(reg_id_t reg) {
    if (!reg_is_gpr(reg))
        return -1;
    reg_id_t r64 = reg_to_pointer_sized(reg);
    int idx = (int)(r64 - DR_REG_RAX);
    return (idx >= 0 && idx < 16) ? (AT_RT_GPR_BASE + idx) : -1;
}

/* Install `leaf` at directory slot i via CAS; on loss free our spare and return the
 * winner. The lone mandatory-atomic shadow mutation (compiler builtin -> lock cmpxchg,
 * no libc). Called only off the hot path (on_seed / thread-init pre-touch). */
static at_tag_t *leaf_install(size_t i, at_tag_t *leaf) {
    at_tag_t *expect = NULL;
    if (__atomic_compare_exchange_n(&g_dir[i], &expect, leaf, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return leaf; /* we won */
    dr_raw_mem_free(leaf, AT_LEAF_ALLOC);
    return expect; /* lost: the value now in the slot */
}

/* Resolve a shadow byte pointer for `ea`, creating its leaf on first touch. Off the
 * hot path only (clean-call / init contexts); the inline hot path reads g_dir directly
 * and treats a null leaf as clean/drop. Returns NULL for a non-canonical address or an
 * allocation failure. */
static at_tag_t *tag_ptr_create(uint64_t ea) {
    size_t i = (size_t)(ea >> AT_LEAF_BITS);
    if (i >= AT_DIR_LEN)
        return NULL;
    at_tag_t *lf = __atomic_load_n(&g_dir[i], __ATOMIC_ACQUIRE);
    if (lf == NULL) {
        at_tag_t *nl = (at_tag_t *)dr_raw_mem_alloc(
            AT_LEAF_ALLOC, DR_MEMPROT_READ | DR_MEMPROT_WRITE, NULL);
        if (nl == NULL)
            return NULL;
        lf = leaf_install(
            i, nl); /* zeroed by dr_raw_mem_alloc (mmap ANON)         */
    }
    return lf + (ea & AT_LEAF_MASK);
}

/* Store-tag broadcast SLOWPATH (rare; taken only when the inline fast path finds a null
 * leaf — i.e. the first tag write to a 1 MiB page). Creates the leaf on touch and writes
 * the tag; a real create-on-touch store shadow, so NO stack (or any other) pre-touch is
 * needed and arbitrary store targets (the managed heap, Increment 5) are handled. Off
 * the per-instruction path: after a page's first touch its leaf exists and every later
 * store to it takes the inline fast path. */
static void on_store_slow(uint64_t ea, uint64_t tag, uint64_t size) {
    for (uint64_t k = 0; k < size; k++) {
        at_tag_t *p = tag_ptr_create(ea + k); /* per-byte -> no straddle gap */
        if (p != NULL)
            *p = (at_tag_t)tag;
    }
}

/* Seed marker clean call (rare; not the hot path): paint [base, base+len) = color in
 * the shadow before traced code runs. Create-on-touch (tag_ptr_create) allocates the
 * seeded buffer's leaf; store leaves are created on first touch by on_store_slow, so no
 * pre-touch is required. */
static void on_seed(uint64_t base, uint64_t len, uint64_t color) {
    for (uint64_t i = 0; i < len; i++) {
        at_tag_t *p = tag_ptr_create(base + i);
        if (p != NULL)
            *p = (at_tag_t)color;
    }
}

static void event_thread_init(void *drcontext) {
    at_tag_t *rf = (at_tag_t *)dr_thread_alloc(drcontext, AT_RT_COUNT);
    if (rf != NULL)
        memset(rf, 0, AT_RT_COUNT);
    drmgr_set_tls_field(drcontext, g_tls_regfile, rf);
}

static void event_thread_exit(void *drcontext) {
    at_tag_t *rf = (at_tag_t *)drmgr_get_tls_field(drcontext, g_tls_regfile);
    if (rf != NULL)
        dr_thread_free(drcontext, rf, AT_RT_COUNT);
}

/* Sink marker clean call (rare; not the hot path): register the app-owned report the
 * sink appends hits into. Same on_marker pattern (rdi = at_taint_report_t*), no drwrap. */
static void on_sink_register(at_taint_report_t *report) {
    dr_mutex_lock(g_lock);
    g_report = report;
    dr_mutex_unlock(g_lock);
}

/* Sink append (rare; per watched-branch, NOT per-instruction — the propagation stays
 * inline). Inserted UNCONDITIONALLY at each in-region conditional branch (a transparent
 * clean call, so the app's flags the branch reads are preserved); the taint GUARD is
 * the data check below (append only when the watched operand is tainted). For a branch
 * (kind = 1) the watched operand is eflags, read from THIS thread's reg-tag file — set
 * by the flag-defining instruction's inline propagation, which already executed. off is
 * the branch's region offset; seed_off/depth are left 0 and filled app-side by the
 * validator's def-use BFS. (A guarded INLINE skip — no call when the flag is clean — is
 * the immediate refinement; unconditional-per-branch is correct and simpler here.) */
static void on_sink(uint64_t off, uint64_t ea, uint64_t kind) {
    if (g_report == NULL)
        return;
    void *dc = dr_get_current_drcontext();
    if (dc == NULL)
        return;
    at_tag_t *rf = (at_tag_t *)drmgr_get_tls_field(dc, g_tls_regfile);
    if (rf == NULL)
        return;
    at_tag_t tag = rf[AT_RT_EFLAGS]; /* watched operand for a branch sink */
    if (tag == AT_TAG_CLEAN)
        return; /* clean flow does not reach the sink */

    /* Thread-safe append (a launched managed workload sinks from many threads): reserve a
     * unique slot with an atomic fetch-add on hits_total, then fill that DISJOINT slot —
     * no lock on the append path. hits_total is the true count; hits_len is a best-effort
     * mirror (the reader uses min(hits_total, hits_cap)). Overflow past the cap flips
     * truncated (the honest-overflow contract of at_drval_t / asmtest_trace_t). */
    if (g_report->hits == NULL)
        return;
    uint64_t idx =
        __atomic_fetch_add(&g_report->hits_total, 1, __ATOMIC_RELAXED);
    if (idx >= g_report->hits_cap) {
        __atomic_store_n(&g_report->truncated, 1, __ATOMIC_RELAXED);
        return;
    }
    at_taint_hit_t *h = &g_report->hits[idx];
    memset(h, 0, sizeof *h);
    h->off = off;
    h->ea = ea;
    h->tag = tag;
    h->kind = (uint8_t)kind;
    __atomic_store_n(&g_report->hits_len, idx + 1, __ATOMIC_RELAXED);
}

/* ---- inline-emit helpers for the propagation phase ---------------------------- */

/* Emit the 2-level shadow lookup for `ea_reg` (clobbered) into `pp` (a valid byte
 * pointer: the real leaf slot, or `fallback` on a null leaf), using scratch r1/r2.
 * Branchless (cmov, no hot-path branch). `pp` may equal r2. */
static void emit_shadow_lookup(void *dc, instrlist_t *bb, instr_t *where,
                               reg_id_t ea_reg, reg_id_t r1, reg_id_t r2,
                               const void *fallback) {
    /* r1 = ea >> LEAF_BITS (leaf index) */
    instrlist_meta_preinsert(
        bb, where,
        INSTR_CREATE_mov_ld(dc, opnd_create_reg(r1), opnd_create_reg(ea_reg)));
    instrlist_meta_preinsert(bb, where,
                             INSTR_CREATE_shr(dc, opnd_create_reg(r1),
                                              OPND_CREATE_INT8(AT_LEAF_BITS)));
    /* r2 = g_dir; r2 = g_dir[r1] (leaf ptr, maybe null) */
    instrlist_meta_preinsert(
        bb, where,
        INSTR_CREATE_mov_imm(dc, opnd_create_reg(r2),
                             OPND_CREATE_INTPTR((ptr_uint_t)g_dir)));
    instrlist_meta_preinsert(
        bb, where,
        INSTR_CREATE_mov_ld(
            dc, opnd_create_reg(r2),
            opnd_create_base_disp(r2, r1, sizeof(at_tag_t *), 0, OPSZ_8)));
    /* ea_reg = ea & LEAF_MASK (offset within leaf) */
    instrlist_meta_preinsert(
        bb, where,
        INSTR_CREATE_and(dc, opnd_create_reg(ea_reg),
                         OPND_CREATE_INT32((int)AT_LEAF_MASK)));
    /* r1 = &fallback (flag-neutral, before test) */
    instrlist_meta_preinsert(
        bb, where,
        INSTR_CREATE_mov_imm(dc, opnd_create_reg(r1),
                             OPND_CREATE_INTPTR((ptr_uint_t)fallback)));
    /* test leaf; r2 = leaf + offset (lea is flag-neutral, preserves ZF); cmovz r2,r1 */
    instrlist_meta_preinsert(
        bb, where,
        INSTR_CREATE_test(dc, opnd_create_reg(r2), opnd_create_reg(r2)));
    instrlist_meta_preinsert(
        bb, where,
        INSTR_CREATE_lea(dc, opnd_create_reg(r2),
                         opnd_create_base_disp(r2, ea_reg, 1, 0, OPSZ_lea)));
    instrlist_meta_preinsert(bb, where,
                             INSTR_CREATE_cmovcc(dc, OP_cmovz,
                                                 opnd_create_reg(r2),
                                                 opnd_create_reg(r1)));
}

/* OR the tag of a `size`-byte memory operand at `ea_reg`'s shadow into s_t (a source
 * read): a per-byte union over all `size` shadow bytes, so a taint in ANY byte of the
 * operand reaches the result. Clobbers ea_reg/r1/r2. Null leaf -> reads g_zero_pad
 * (no-op OR); the guard page makes a leaf-straddling read fault-safe. */
static void emit_shadow_or(void *dc, instrlist_t *bb, instr_t *where,
                           reg_id_t ea_reg, reg_id_t r1, reg_id_t r2,
                           reg_id_t s_t, uint16_t size) {
    emit_shadow_lookup(dc, bb, where, ea_reg, r1, r2, &g_zero_pad[0]);
    for (uint16_t k = 0; k < size; k++)
        instrlist_meta_preinsert(
            bb, where,
            INSTR_CREATE_or(
                dc, opnd_create_reg(reg_resize_to_opsz(s_t, OPSZ_1)),
                opnd_create_base_disp(r2, DR_REG_NULL, 0, k, OPSZ_1)));
}

/* Store s_t's low tag byte to `ea_reg`'s shadow (a store dst broadcast), with real
 * CREATE-ON-TOUCH: if the leaf exists, store inline (fast path); if not, a first-touch
 * SLOWPATH clean call (on_store_slow) allocates the leaf and writes the tag. `ea_reg` is
 * PRESERVED (the slowpath passes it as the EA); r1/r2 are clobbered scratch. The clean
 * call is transparent, so s_t/ea_reg survive it; both paths reconverge at `done`. */
static void emit_shadow_store(void *dc, instrlist_t *bb, instr_t *where,
                              reg_id_t ea_reg, reg_id_t r1, reg_id_t r2,
                              reg_id_t s_t, uint16_t size) {
    instr_t *slow = INSTR_CREATE_label(dc);
    instr_t *done = INSTR_CREATE_label(dc);
    /* r1 = ea >> LEAF_BITS (leaf index) */
    instrlist_meta_preinsert(
        bb, where,
        INSTR_CREATE_mov_ld(dc, opnd_create_reg(r1), opnd_create_reg(ea_reg)));
    instrlist_meta_preinsert(bb, where,
                             INSTR_CREATE_shr(dc, opnd_create_reg(r1),
                                              OPND_CREATE_INT8(AT_LEAF_BITS)));
    /* r2 = g_dir[r1] (leaf ptr, maybe null) */
    instrlist_meta_preinsert(
        bb, where,
        INSTR_CREATE_mov_imm(dc, opnd_create_reg(r2),
                             OPND_CREATE_INTPTR((ptr_uint_t)g_dir)));
    instrlist_meta_preinsert(
        bb, where,
        INSTR_CREATE_mov_ld(
            dc, opnd_create_reg(r2),
            opnd_create_base_disp(r2, r1, sizeof(at_tag_t *), 0, OPSZ_8)));
    /* r1 = ea & LEAF_MASK (offset within leaf) */
    instrlist_meta_preinsert(
        bb, where,
        INSTR_CREATE_mov_ld(dc, opnd_create_reg(r1), opnd_create_reg(ea_reg)));
    instrlist_meta_preinsert(
        bb, where,
        INSTR_CREATE_and(dc, opnd_create_reg(r1),
                         OPND_CREATE_INT32((int)AT_LEAF_MASK)));
    /* if (leaf == NULL) goto slow */
    instrlist_meta_preinsert(
        bb, where,
        INSTR_CREATE_test(dc, opnd_create_reg(r2), opnd_create_reg(r2)));
    instrlist_meta_preinsert(
        bb, where, INSTR_CREATE_jcc(dc, OP_jz, opnd_create_instr(slow)));
    /* fast path: byte[leaf + offset + k] = s_t for k in [0, size); goto done. The guard
     * page makes a leaf-straddling write fault-safe (the straddling bytes land in the
     * guard = a conservative miss). */
    for (uint16_t k = 0; k < size; k++)
        instrlist_meta_preinsert(
            bb, where,
            INSTR_CREATE_mov_st(
                dc, opnd_create_base_disp(r2, r1, 1, k, OPSZ_1),
                opnd_create_reg(reg_resize_to_opsz(s_t, OPSZ_1))));
    instrlist_meta_preinsert(bb, where,
                             INSTR_CREATE_jmp(dc, opnd_create_instr(done)));
    /* slowpath: on_store_slow(ea, tag, size) creates the leaf(s) on first touch and
     * writes all size bytes (per-byte, so no straddle gap). */
    instrlist_meta_preinsert(bb, where, slow);
    dr_insert_clean_call(dc, bb, where, (void *)on_store_slow, false, 3,
                         opnd_create_reg(ea_reg),
                         opnd_create_reg(reg_resize_to_opsz(s_t, OPSZ_8)),
                         OPND_CREATE_INTPTR((ptr_uint_t)size));
    instrlist_meta_preinsert(bb, where, done);
}

/* OR reg-tag-file[idx]'s byte into s_t (a register source read). t_rf = regfile base. */
static void emit_regtag_or(void *dc, instrlist_t *bb, instr_t *where,
                           reg_id_t t_rf, int idx, reg_id_t s_t) {
    instrlist_meta_preinsert(
        bb, where,
        INSTR_CREATE_or(
            dc, opnd_create_reg(reg_resize_to_opsz(s_t, OPSZ_1)),
            opnd_create_base_disp(t_rf, DR_REG_NULL, 0, idx, OPSZ_1)));
}

/* Store s_t's low tag byte into reg-tag-file[idx] (a register dst broadcast). */
static void emit_regtag_store(void *dc, instrlist_t *bb, instr_t *where,
                              reg_id_t t_rf, int idx, reg_id_t s_t) {
    instrlist_meta_preinsert(
        bb, where,
        INSTR_CREATE_mov_st(
            dc, opnd_create_base_disp(t_rf, DR_REG_NULL, 0, idx, OPSZ_1),
            opnd_create_reg(reg_resize_to_opsz(s_t, OPSZ_1))));
}
#endif /* ASMTEST_TAINT */

/* ------------------------------------------------------------------ */
/* drx_buf flush: drain raw records into the app-owned at_drval_t        */
/* ------------------------------------------------------------------ */

static void buf_flush(void *drcontext, void *buf_base, size_t size) {
    (void)drcontext;
    at_drval_t *dv = g_region.drval;
    if (dv == NULL)
        return;
    size_t n = size / sizeof(raw_step_t);
    const raw_step_t *recs = (const raw_step_t *)buf_base;
    for (size_t i = 0; i < n; i++) {
        const raw_step_t *rs = &recs[i];
        dv->steps_total++;
        if (dv->steps == NULL || dv->steps_len >= dv->steps_cap) {
            dv->truncated = 1;
            continue;
        }
        at_vstep_t *st = &dv->steps[dv->steps_len];
        memset(st, 0, sizeof *st);
        st->off = rs->off;
        for (int g = 0; g < AT_GPR_COUNT; g++)
            st->gpr[g] = rs->gpr[g];
        st->rip = rs->rip;
        st->rflags = 0; /* clean-call-only field; see file header */
        st->mem_first = (uint32_t)dv->mem_len;
        st->mem_n = 0;
        uint32_t mn =
            rs->mem_n <= AT_INLINE_MAXMEM ? rs->mem_n : AT_INLINE_MAXMEM;
        for (uint32_t j = 0; j < mn; j++) {
            if (dv->mem == NULL || dv->mem_len >= dv->mem_cap) {
                dv->truncated = 1;
                break;
            }
            at_vmem_t *m = &dv->mem[dv->mem_len];
            memset(m, 0, sizeof *m);
            m->ea = rs->mem_ea[j];
            m->value = rs->mem_val[j];
            m->size = rs->mem_size[j];
            m->valid = rs->mem_valid[j];
            dv->mem_len++;
            st->mem_n++;
        }
#ifdef ASMTEST_TAINT
        /* Drain this step's taint witness parallel to steps[] (same index + honest
         * overflow); dropped only if steps[] itself did not truncate above. */
        if (dv->step_taint != NULL && dv->steps_len < dv->step_taint_cap)
            dv->step_taint[dv->steps_len] = rs->taint;
#endif
        dv->steps_len++;
    }
}

/* ------------------------------------------------------------------ */
/* Marker clean call (once; not the hot path) — learn region + buffer   */
/* ------------------------------------------------------------------ */

static void on_marker(app_pc base, size_t len, at_drval_t *drval) {
    dr_mutex_lock(g_lock);
    g_region.base = base;
    g_region.len = len;
    g_region.drval = drval;
    dr_mutex_unlock(g_lock);
    dr_delay_flush_region(base, len, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* Inlined per-instruction capture                                      */
/* ------------------------------------------------------------------ */

/* Materialize a 64-bit immediate into val_reg and store it into the buffer slot.
 * On x86-64 a [buf_ptr+disp] store needs no scratch, so pass DR_REG_NULL (the
 * bbbuf sample idiom). */
static void store_imm64(void *dc, instrlist_t *bb, instr_t *where,
                        reg_id_t buf_ptr, reg_id_t val_reg, uint64_t imm,
                        short offset) {
    instrlist_meta_preinsert(bb, where,
                             INSTR_CREATE_mov_imm(dc, opnd_create_reg(val_reg),
                                                  OPND_CREATE_INTPTR(imm)));
    drx_buf_insert_buf_store(dc, g_buf, bb, where, buf_ptr, DR_REG_NULL,
                             opnd_create_reg(val_reg), OPSZ_8, offset);
}

/* A memory operand we can resolve inline: a plain base+index*scale+disp load, not
 * RIP-relative, far, or segmented. Returns the byte size (0 = not capturable). */
static uint16_t capturable_mem_size(opnd_t op) {
    if (!opnd_is_base_disp(op))
        return 0; /* rip-rel abs-addr / other forms deferred */
    if (opnd_is_far_memory_reference(op))
        return 0; /* fs:/gs: segmented deferred */
    if (opnd_get_segment(op) != DR_REG_NULL)
        return 0;
    uint16_t sz = (uint16_t)opnd_size_in_bytes(opnd_get_size(op));
    if (sz != 1 && sz != 2 && sz != 4 && sz != 8)
        return 0; /* only 1/2/4/8 inline loads this increment */
    return sz;
}

#ifdef ASMTEST_TAINT
/* Emit the inline dst_tag = union(src_tags) propagation for one in-region app instr,
 * as an extra phase of the value-capture insertion pass. Runs AFTER the value pass's
 * memory loop (so mem-source EAs are already in this step's record at mem_ea[0..nmem))
 * and BEFORE the buffer pointer advances (so the step witness rides this record).
 *
 * Register contract at entry: s_ptr = buffer pointer (preserved); s_a/s_b = free
 * scratch (clobbered); s_t = union accumulator; t_rf = reg-tag-file base scratch;
 * aflags reserved. Uses the DR-native operand walk (NO Capstone) — congruence with the
 * app-side enumerator's read/write set is proven by the oracle diff, not assumed. */
static void emit_taint_phase(void *dc, instrlist_t *bb, instr_t *instr,
                             uint32_t nmem, const uint16_t *memsz,
                             reg_id_t s_ptr, reg_id_t s_a, reg_id_t s_b,
                             reg_id_t s_t, reg_id_t t_rf) {
    /* s_t = 0 (32-bit xor zeroes the whole container). */
    instrlist_meta_preinsert(
        bb, instr,
        INSTR_CREATE_xor(dc, opnd_create_reg(reg_resize_to_opsz(s_t, OPSZ_4)),
                         opnd_create_reg(reg_resize_to_opsz(s_t, OPSZ_4))));

    /* ---- union SOURCE tags into s_t --------------------------------------- */
    drmgr_insert_read_tls_field(dc, g_tls_regfile, bb, instr,
                                t_rf); /* regfile base */

    /* Register sources: every src reg, plus the base/index registers of every memory
     * operand (a load's OR a store's address is computed from registers that are READ,
     * matching the app-side enumerator's read set). */
    for (int s = 0; s < instr_num_srcs(instr); s++) {
        opnd_t op = instr_get_src(instr, s);
        if (opnd_is_reg(op)) {
            int idx = rt_index(opnd_get_reg(op));
            if (idx >= 0)
                emit_regtag_or(dc, bb, instr, t_rf, idx, s_t);
        } else if (opnd_is_memory_reference(op)) {
            int bi = rt_index(opnd_get_base(op)),
                ii = rt_index(opnd_get_index(op));
            if (bi >= 0)
                emit_regtag_or(dc, bb, instr, t_rf, bi, s_t);
            if (ii >= 0)
                emit_regtag_or(dc, bb, instr, t_rf, ii, s_t);
        }
    }
    for (int d = 0; d < instr_num_dsts(instr); d++) {
        opnd_t op = instr_get_dst(instr, d);
        if (opnd_is_memory_reference(op)) {
            int bi = rt_index(opnd_get_base(op)),
                ii = rt_index(opnd_get_index(op));
            if (bi >= 0)
                emit_regtag_or(dc, bb, instr, t_rf, bi, s_t);
            if (ii >= 0)
                emit_regtag_or(dc, bb, instr, t_rf, ii, s_t);
        }
    }
    if (instr_get_eflags(instr, DR_QUERY_DEFAULT) & EFLAGS_READ_ARITH)
        emit_regtag_or(dc, bb, instr, t_rf, AT_RT_EFLAGS, s_t);

    /* Memory sources: OR the shadow tag at each captured load EA (read back from this
     * step's record — decoupled from app-register aliasing). t_rf is free scratch here
     * (its regfile-base role is done until the dst broadcast re-reads it). */
    for (uint32_t j = 0; j < nmem; j++) {
        instrlist_meta_preinsert(
            bb, instr,
            INSTR_CREATE_mov_ld(dc, opnd_create_reg(s_a),
                                opnd_create_base_disp(
                                    s_ptr, DR_REG_NULL, 0,
                                    (int)(offsetof(raw_step_t, mem_ea) + j * 8),
                                    OPSZ_8)));
        emit_shadow_or(dc, bb, instr, s_a, s_b, t_rf, s_t, memsz[j]);
    }

    /* ---- step witness: raw_step_t.taint = s_t (rides this record) ---------- */
    instrlist_meta_preinsert(
        bb, instr,
        INSTR_CREATE_mov_st(
            dc,
            opnd_create_base_disp(s_ptr, DR_REG_NULL, 0,
                                  (int)offsetof(raw_step_t, taint), OPSZ_1),
            opnd_create_reg(reg_resize_to_opsz(s_t, OPSZ_1))));

    /* ---- broadcast s_t to every DST -------------------------------------- */
    drmgr_insert_read_tls_field(dc, g_tls_regfile, bb, instr,
                                t_rf); /* re-read base */
    for (int d = 0; d < instr_num_dsts(instr); d++) {
        opnd_t op = instr_get_dst(instr, d);
        if (opnd_is_reg(op)) {
            int idx = rt_index(opnd_get_reg(op));
            if (idx >= 0)
                emit_regtag_store(dc, bb, instr, t_rf, idx, s_t);
        }
    }
    if (instr_get_eflags(instr, DR_QUERY_DEFAULT) & EFLAGS_WRITE_ARITH)
        emit_regtag_store(dc, bb, instr, t_rf, AT_RT_EFLAGS, s_t);

    /* Store dsts: broadcast s_t into the destination-address shadow. EA computed inline
     * from the app base/index (drreg_restore_app_values). Skip if an address register
     * is one of our held/scratch regs (a conservative miss; the fixture's stack base
     * rsp is never a drreg scratch, so it never skips). */
    for (int d = 0; d < instr_num_dsts(instr); d++) {
        opnd_t op = instr_get_dst(instr, d);
        uint16_t dsz =
            opnd_is_memory_reference(op) ? capturable_mem_size(op) : 0;
        if (dsz == 0)
            continue;
        reg_id_t bse = opnd_get_base(op), idxr = opnd_get_index(op);
        if (bse == s_ptr || bse == s_a || bse == s_b || bse == s_t ||
            bse == t_rf || idxr == s_ptr || idxr == s_a || idxr == s_b ||
            idxr == s_t || idxr == t_rf)
            continue;
        reg_id_t swap = DR_REG_NULL;
        drreg_restore_app_values(dc, bb, instr, op, &swap);
        instrlist_meta_preinsert(
            bb, instr,
            INSTR_CREATE_lea(
                dc, opnd_create_reg(s_a),
                opnd_create_base_disp(opnd_get_base(op), opnd_get_index(op),
                                      opnd_get_scale(op), opnd_get_disp(op),
                                      OPSZ_lea)));
        emit_shadow_store(dc, bb, instr, s_a, s_b, t_rf, s_t, dsz);
        if (swap != DR_REG_NULL)
            drreg_unreserve_register(dc, bb, instr, swap);
    }
}
#endif /* ASMTEST_TAINT */

static dr_emit_flags_t event_insert(void *dc, void *tag, instrlist_t *bb,
                                    instr_t *instr, bool for_trace,
                                    bool translating, void *user_data) {
    (void)tag;
    (void)for_trace;
    (void)translating;
    (void)user_data;

    if (!instr_is_app(instr))
        return DR_EMIT_DEFAULT;
    app_pc ipc = instr_get_app_pc(instr);
    if (ipc == NULL)
        return DR_EMIT_DEFAULT;

    /* Marker: single SysV-arg clean call, PC-resolved, no drwrap (as clean-call). */
    if (ipc == pc_marker) {
        dr_insert_clean_call(dc, bb, instr, (void *)on_marker, false, 3,
                             opnd_create_reg(DR_REG_RDI),
                             opnd_create_reg(DR_REG_RSI),
                             opnd_create_reg(DR_REG_RDX));
        return DR_EMIT_DEFAULT;
    }
#ifdef ASMTEST_TAINT
    /* Seed marker: same rare PC-resolved SysV-arg clean call (no drwrap). Paints the
     * shadow (rdi=base, rsi=len, rdx=color) at seed time, before traced code runs. */
    if (ipc == pc_seed_marker) {
        dr_insert_clean_call(dc, bb, instr, (void *)on_seed, false, 3,
                             opnd_create_reg(DR_REG_RDI),
                             opnd_create_reg(DR_REG_RSI),
                             opnd_create_reg(DR_REG_RDX));
        return DR_EMIT_DEFAULT;
    }
    /* Sink marker: register the app-owned report (rdi = at_taint_report_t*). */
    if (ipc == pc_sink_marker) {
        dr_insert_clean_call(dc, bb, instr, (void *)on_sink_register, false, 1,
                             opnd_create_reg(DR_REG_RDI));
        return DR_EMIT_DEFAULT;
    }
#endif

    at_drval_t *dv = g_region.drval;
    app_pc base = g_region.base;
    size_t len = g_region.len;
    if (dv == NULL || ipc < base || ipc >= base + len)
        return DR_EMIT_DEFAULT;

#ifdef ASMTEST_TAINT
    /* Branch-condition SINK (kind = 1): at each in-region conditional branch, insert a
     * transparent clean call that appends a hit iff the flag it reads is tainted. Placed
     * FIRST (before the value/propagation instrumentation of this branch), so at runtime
     * it observes the reg-tag file as left by the PRIOR (flag-defining) instruction's
     * inline propagation; being a clean call it restores the app flags the branch then
     * reads. off is the branch's region offset; ea = 0 (a register/flag sink). */
    if (instr_is_cbr(instr)) {
        dr_insert_clean_call(dc, bb, instr, (void *)on_sink, false, 3,
                             OPND_CREATE_INTPTR((ptr_uint_t)(ipc - base)),
                             OPND_CREATE_INTPTR((ptr_uint_t)0),
                             OPND_CREATE_INTPTR((ptr_uint_t)1));
    }
#endif

    /* Reserve three scratch GPRs FIRST (buffer pointer + two work registers); the
     * memory-operand enumeration below needs s_ptr to skip operands whose base or
     * index aliases it. (Arithmetic flags are reserved later, only around the
     * trace-buffer update — the mov/lea/movzx capture here is flag-neutral.) */
    reg_id_t s_ptr, s_a, s_b;
    if (drreg_reserve_register(dc, bb, instr, NULL, &s_ptr) != DRREG_SUCCESS)
        return DR_EMIT_DEFAULT;
    if (drreg_reserve_register(dc, bb, instr, NULL, &s_a) != DRREG_SUCCESS) {
        drreg_unreserve_register(dc, bb, instr, s_ptr);
        return DR_EMIT_DEFAULT;
    }
    if (drreg_reserve_register(dc, bb, instr, NULL, &s_b) != DRREG_SUCCESS) {
        drreg_unreserve_register(dc, bb, instr, s_a);
        drreg_unreserve_register(dc, bb, instr, s_ptr);
        return DR_EMIT_DEFAULT;
    }

    /* Enumerate the capturable explicit memory SOURCE operands (count/sizes are
     * compile-time, emitted as immediates below). Two conservative gates beyond
     * capturable_mem_size — a skipped operand's VALUE is simply left unfilled; the
     * app-side enumerator still produces its read record and resolves the location
     * from the register snapshot, so def-use/slices are unaffected:
     *  - instr_reads_memory: skip NO-LOAD forms (lea agen, `nop [mem]`, prefetch)
     *    whose "source" memory operand the instruction never dereferences. An
     *    inline load of them would fault on an unmapped/non-pointer address the app
     *    itself never touches — unlike a real load, where the app would fault too,
     *    so the "assumes a valid EA" divergence (file header) stays bounded to
     *    genuine loads.
     *  - base/index != s_ptr: if the buffer-pointer register aliases this operand's
     *    base or index, drreg_restore_app_values (below) would restore it IN PLACE
     *    and destroy the buffer pointer — the same in-place-restore hazard the GPR
     *    loop guards against. Skip rather than corrupt the capture / app memory. */
    opnd_t memops[AT_INLINE_MAXMEM];
    uint16_t memsz[AT_INLINE_MAXMEM];
    uint32_t nmem = 0;
    bool reads_mem = instr_reads_memory(instr);
    for (int s = 0;
         reads_mem && s < instr_num_srcs(instr) && nmem < AT_INLINE_MAXMEM;
         s++) {
        opnd_t op = instr_get_src(instr, s);
        if (!opnd_is_memory_reference(op))
            continue;
        uint16_t sz = capturable_mem_size(op);
        if (sz == 0)
            continue;
        if (opnd_get_base(op) == s_ptr || opnd_get_index(op) == s_ptr)
            continue;
        memops[nmem] = op;
        memsz[nmem] = sz;
        nmem++;
    }

    drx_buf_insert_load_buf_ptr(dc, g_buf, bb, instr, s_ptr);

    /* off, rip: compile-time immediates. */
    store_imm64(dc, bb, instr, s_ptr, s_a, (uint64_t)(ipc - base),
                (short)offsetof(raw_step_t, off));
    store_imm64(dc, bb, instr, s_ptr, s_a, (uint64_t)(ptr_uint_t)ipc,
                (short)offsetof(raw_step_t, rip));

    /* mem_n immediate (materialize as a full 64-bit imm; store the low 4 bytes). */
    instrlist_meta_preinsert(
        bb, instr,
        INSTR_CREATE_mov_imm(dc, opnd_create_reg(s_a),
                             OPND_CREATE_INTPTR((ptr_uint_t)nmem)));
    drx_buf_insert_buf_store(dc, g_buf, bb, instr, s_ptr, DR_REG_NULL,
                             opnd_create_reg(reg_resize_to_opsz(s_a, OPSZ_4)),
                             OPSZ_4, (short)offsetof(raw_step_t, mem_n));

    /* gpr[16]: each register's APP value. drreg_get_app_value(X, s_a) restores X
     * IN PLACE, so capturing the register that backs s_ptr here would destroy the
     * buffer pointer — skip s_ptr in the loop and capture it last (below), after
     * copying the buffer pointer to s_b. s_a/s_b are safe to restore in place. */
    for (int g = 0; g < AT_GPR_COUNT; g++) {
        if (g_gpr_order[g] == s_ptr)
            continue;
        drreg_get_app_value(dc, bb, instr, g_gpr_order[g], s_a);
        drx_buf_insert_buf_store(dc, g_buf, bb, instr, s_ptr, DR_REG_NULL,
                                 opnd_create_reg(s_a), OPSZ_8,
                                 (short)(offsetof(raw_step_t, gpr) + g * 8));
    }

    /* memory operands: EA (inlined lea over app-restored base/index) + value. */
    for (uint32_t j = 0; j < nmem; j++) {
        opnd_t op = memops[j];
        reg_id_t swap = DR_REG_NULL;
        drreg_restore_app_values(dc, bb, instr, op, &swap);

        opnd_t addr = opnd_create_base_disp(
            opnd_get_base(op), opnd_get_index(op), opnd_get_scale(op),
            opnd_get_disp(op), OPSZ_lea);
        instrlist_meta_preinsert(
            bb, instr, INSTR_CREATE_lea(dc, opnd_create_reg(s_a), addr));
        drx_buf_insert_buf_store(dc, g_buf, bb, instr, s_ptr, DR_REG_NULL,
                                 opnd_create_reg(s_a), OPSZ_8,
                                 (short)(offsetof(raw_step_t, mem_ea) + j * 8));

        /* Inlined value load [s_a] -> s_b, zero-extended per size (see header:
         * assumes a valid EA). */
        opnd_t src;
        if (memsz[j] == 8) {
            src = opnd_create_base_disp(s_a, DR_REG_NULL, 0, 0, OPSZ_8);
            instrlist_meta_preinsert(
                bb, instr, INSTR_CREATE_mov_ld(dc, opnd_create_reg(s_b), src));
        } else if (memsz[j] == 4) {
            src = opnd_create_base_disp(s_a, DR_REG_NULL, 0, 0, OPSZ_4);
            instrlist_meta_preinsert(
                bb, instr,
                INSTR_CREATE_mov_ld(
                    dc, opnd_create_reg(reg_resize_to_opsz(s_b, OPSZ_4)), src));
        } else { /* 1 or 2 */
            src = opnd_create_base_disp(s_a, DR_REG_NULL, 0, 0,
                                        memsz[j] == 2 ? OPSZ_2 : OPSZ_1);
            instrlist_meta_preinsert(
                bb, instr, INSTR_CREATE_movzx(dc, opnd_create_reg(s_b), src));
        }
        drx_buf_insert_buf_store(
            dc, g_buf, bb, instr, s_ptr, DR_REG_NULL, opnd_create_reg(s_b),
            OPSZ_8, (short)(offsetof(raw_step_t, mem_val) + j * 8));

        if (swap != DR_REG_NULL)
            drreg_unreserve_register(dc, bb, instr, swap);

        /* size + valid: compile-time immediates (materialize 64-bit, store narrow). */
        instrlist_meta_preinsert(
            bb, instr,
            INSTR_CREATE_mov_imm(dc, opnd_create_reg(s_a),
                                 OPND_CREATE_INTPTR((ptr_uint_t)memsz[j])));
        drx_buf_insert_buf_store(
            dc, g_buf, bb, instr, s_ptr, DR_REG_NULL,
            opnd_create_reg(reg_resize_to_opsz(s_a, OPSZ_2)), OPSZ_2,
            (short)(offsetof(raw_step_t, mem_size) + j * 2));
        instrlist_meta_preinsert(bb, instr,
                                 INSTR_CREATE_mov_imm(dc, opnd_create_reg(s_a),
                                                      OPND_CREATE_INTPTR(1)));
        drx_buf_insert_buf_store(
            dc, g_buf, bb, instr, s_ptr, DR_REG_NULL,
            opnd_create_reg(reg_resize_to_opsz(s_a, OPSZ_1)), OPSZ_1,
            (short)(offsetof(raw_step_t, mem_valid) + j));
    }

#ifdef ASMTEST_TAINT
    /* Inline dst_tag = union(src_tags) propagation + per-step witness. s_ptr still holds
     * the buffer pointer and mem-source EAs are already in this record; s_a/s_b are free.
     * Reserve the union accumulator (s_t), the reg-tag-file base (t_rf), and aflags —
     * peak ~6 GPR + aflags with the value pass's s_ptr/s_a/s_b (drreg slots bumped under
     * the flag). On any drreg failure skip taint for this step (a conservative miss)
     * without disturbing the value capture below. */
    {
        reg_id_t s_t = DR_REG_NULL, t_rf = DR_REG_NULL;
        if (drreg_reserve_register(dc, bb, instr, NULL, &s_t) ==
            DRREG_SUCCESS) {
            if (drreg_reserve_register(dc, bb, instr, NULL, &t_rf) ==
                DRREG_SUCCESS) {
                if (drreg_reserve_aflags(dc, bb, instr) == DRREG_SUCCESS) {
                    emit_taint_phase(dc, bb, instr, nmem, memsz, s_ptr, s_a,
                                     s_b, s_t, t_rf);
                    drreg_unreserve_aflags(dc, bb, instr);
                }
                drreg_unreserve_register(dc, bb, instr, t_rf);
            }
            drreg_unreserve_register(dc, bb, instr, s_t);
        }
    }
#endif

    /* Capture the buffer-pointer register's own app value LAST: copy the buffer
     * pointer into s_b, restore app-s_ptr into s_a (clobbering s_ptr, now dead as
     * a pointer), and store it via the s_b copy. The buffer pointer now lives in
     * s_b for the update below. */
    for (int g = 0; g < AT_GPR_COUNT; g++) {
        if (g_gpr_order[g] != s_ptr)
            continue;
        instrlist_meta_preinsert(bb, instr,
                                 INSTR_CREATE_mov_ld(dc, opnd_create_reg(s_b),
                                                     opnd_create_reg(s_ptr)));
        drreg_get_app_value(dc, bb, instr, s_ptr, s_a);
        drx_buf_insert_buf_store(dc, g_buf, bb, instr, s_b, DR_REG_NULL,
                                 opnd_create_reg(s_a), OPSZ_8,
                                 (short)(offsetof(raw_step_t, gpr) + g * 8));
        break;
    }

    /* Advance the buffer pointer (via the s_b copy). This is the ONE piece of our
     * instrumentation that clobbers arithmetic flags (the trace buffer's fill
     * bounds check), so reserve aflags just for it — all the capture above is
     * flag-neutral mov/lea, so reserving aflags late keeps its lahf/rax spill from
     * perturbing the register capture. */
    drreg_status_t af = drreg_reserve_aflags(dc, bb, instr);
    drx_buf_insert_update_buf_ptr(dc, g_buf, bb, instr, s_b, s_a,
                                  sizeof(raw_step_t));
    if (af == DRREG_SUCCESS)
        drreg_unreserve_aflags(dc, bb, instr);

    drreg_unreserve_register(dc, bb, instr, s_b);
    drreg_unreserve_register(dc, bb, instr, s_a);
    drreg_unreserve_register(dc, bb, instr, s_ptr);
    return DR_EMIT_DEFAULT;
}

/* ------------------------------------------------------------------ */
/* Marker resolution + lifecycle                                        */
/* ------------------------------------------------------------------ */

static void try_resolve(module_handle_t h) {
    if (pc_marker == NULL)
        pc_marker = (app_pc)dr_get_proc_address(h, AT_DRVAL_MARKER_SYM);
#ifdef ASMTEST_TAINT
    if (pc_seed_marker == NULL)
        pc_seed_marker = (app_pc)dr_get_proc_address(h, AT_TAINT_SEED_SYM);
    if (pc_sink_marker == NULL)
        pc_sink_marker = (app_pc)dr_get_proc_address(h, AT_TAINT_SINK_SYM);
#endif
}

static void resolve_all_modules(void) {
    dr_module_iterator_t *it = dr_module_iterator_start();
    while (dr_module_iterator_hasnext(it)) {
        module_data_t *m = dr_module_iterator_next(it);
        try_resolve(m->handle);
        dr_free_module_data(m);
    }
    dr_module_iterator_stop(it);
}

static void event_module_load(void *drcontext, const module_data_t *info,
                              bool loaded) {
    (void)drcontext;
    (void)loaded;
    try_resolve(info->handle);
}

static dr_signal_action_t event_signal(void *drcontext, dr_siginfo_t *info) {
    (void)drcontext;
    (void)info;
    return DR_SIGNAL_DELIVER;
}

static void event_exit(void) {
    if (g_buf != NULL)
        drx_buf_free(g_buf);
    drx_exit();
    drreg_exit();
    drmgr_exit();
    dr_mutex_destroy(g_lock);
#ifdef ASMTEST_TAINT
    if (g_dir != NULL)
        dr_raw_mem_free(g_dir, AT_DIR_LEN * sizeof(at_tag_t *));
#endif
}

DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]) {
#ifdef ASMTEST_TAINT
    /* Taint build reserves more drreg slots: the value pass (3 GPR) plus the taint
     * phase (s_t + t_rf + a transient drreg_restore_app_values swap) + aflags. */
    drreg_options_t drreg_ops = {sizeof(drreg_ops), 10 /*scratch slots*/,
                                 false};
#else
    drreg_options_t drreg_ops = {sizeof(drreg_ops),
                                 5 /*scratch slots (3 GPR + aflags + margin)*/,
                                 false};
#endif
    (void)id;
    (void)argc;
    (void)argv;
#ifdef ASMTEST_TAINT
    dr_set_client_name("asm-test data-flow taint client (inlined)", "");
#else
    dr_set_client_name("asm-test data-flow value client (inlined)", "");
#endif

    if (!drmgr_init() || drreg_init(&drreg_ops) != DRREG_SUCCESS ||
        !drx_init()) {
        dr_fprintf(STDERR, "drval-inlined: extension init failed\n");
        dr_abort();
    }
    g_buf = drx_buf_create_trace_buffer((size_t)sizeof(raw_step_t) * 4096,
                                        buf_flush);
    if (g_buf == NULL) {
        dr_fprintf(STDERR, "drval-inlined: drx_buf create failed\n");
        dr_abort();
    }

#ifdef ASMTEST_TAINT
    /* Allocate the 2-level shadow directory (1 GiB VA, demand-zero -> ~0 RAM until
     * leaves are touched) and the per-thread reg-tag TLS slot; register the thread
     * init/exit events that manage the reg-tag file + stack-leaf pre-touch. */
    g_dir =
        (at_tag_t **)dr_raw_mem_alloc(AT_DIR_LEN * sizeof(at_tag_t *),
                                      DR_MEMPROT_READ | DR_MEMPROT_WRITE, NULL);
    if (g_dir == NULL) {
        dr_fprintf(STDERR, "drtaint-inlined: shadow directory alloc failed\n");
        dr_abort();
    }
    g_tls_regfile = drmgr_register_tls_field();
    if (g_tls_regfile == -1) {
        dr_fprintf(STDERR, "drtaint-inlined: tls field alloc failed\n");
        dr_abort();
    }
    if (!drmgr_register_thread_init_event(event_thread_init) ||
        !drmgr_register_thread_exit_event(event_thread_exit)) {
        dr_fprintf(STDERR,
                   "drtaint-inlined: thread-event registration failed\n");
        dr_abort();
    }
#endif

    g_lock = dr_mutex_create();
    resolve_all_modules();
    drmgr_register_module_load_event(event_module_load);
    drmgr_register_bb_instrumentation_event(NULL, event_insert, NULL);
    drmgr_register_signal_event(event_signal);
    drmgr_register_exit_event(event_exit);
}
