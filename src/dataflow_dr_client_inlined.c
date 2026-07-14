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
 * GP registers keep whole-register (1-byte) tags. Leaves are allocated one guard page larger
 * than their span so a per-byte access straddling a leaf boundary is fault-safe (the straddling
 * bytes land in the guard = a conservative miss, never a fault or false positive).
 *
 * Increment 8 (XMM/SIMD taint, FIRST slice): the reg-tag file additionally carries 16 XMM
 * registers with PER-BYTE lane tags (see the AT_RT_XMM_* granularity note), and the taint phase
 * unions/broadcasts XMM register tags plus 16-byte SSE memory loads/stores (movdqu/movdqa/movq
 * ...) — so taint flows through an XMM register and an SSE vectorized copy. The PROPAGATION is
 * whole-register scalar union+broadcast (conservative); lane-independent SIMD flow, YMM/ZMM
 * upper lanes, and VSIB vector-gather EA math are deferred. All additive under -DASMTEST_TAINT.
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

/* Registered value-capture regions (Increment 6: a SET of instrumented ranges, replacing
 * the single region of Increments 1-5). Each entry is a half-open range [base, base+len);
 * the client instruments an app instruction only if it lies in SOME registered range
 * (scope=ranges, the default) — or, under scope=whole, anywhere in the window spanning
 * them. Ranges are appended by the region marker (native fixtures) or the method-load
 * poller (managed workloads, the dotnet slice). g_nregions is published with RELEASE after
 * an entry's fields are written, so a concurrent reader (a bb-build on another thread) sees
 * a complete entry or none. The per-range set-once-before-traced-code contract still holds.
 * Read only at instrumentation (bb-build) time, never on the runtime hot path. */
#define AT_MAX_REGIONS 256
typedef struct {
    app_pc base;
    size_t len;
} region_t;
static region_t g_regions[AT_MAX_REGIONS];
static volatile int g_nregions;

/* Single shared value/taint capture buffer (was per-region): latched at the first region
 * marker; every registered range's captured steps land here. */
static at_drval_t *g_drval;

/* Offset origin for at_vstep_t.off. With multiple ranges, a region-relative offset would
 * collide across ranges, so all ranges share ONE offset space rooted at the LOWEST
 * registered base — which, for a fixture laid out as one contiguous blob with disjoint
 * instrumented sub-ranges, equals the emulator oracle's blob-relative offsets, keeping the
 * out-of-process taint-set diff exact. Latched/lowered as ranges register (before traced
 * code runs, per the contract). */
static app_pc g_origin;

/* Scoping cost metric + toggle (Increment 6 exit criterion). g_inscount counts the app
 * instructions the client actually instruments; scope=whole instruments the entire window
 * spanning the registered ranges (gaps included), scope=ranges (default) only the ranges
 * themselves. So g_inscount(ranges) < g_inscount(whole) demonstrates that method-range
 * scoping bounds the instrumented-instruction count (the ~10-50x band assumes we are NOT
 * tag-tracking the un-instrumented gaps). Reported at client exit; bumped at bb-build time
 * (off the runtime hot path), atomically since bb builds can race across threads. */
static volatile uint64_t g_inscount;
static int g_scope_whole; /* set by the `scope=whole` client option */

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
 * the DR reg id canonicalized to its 64-bit container (whole-register tags for GP), then
 * (Increment 8, SIMD) 16 XMM registers with PER-BYTE lane tags (16 bytes each). eflags is a
 * location too, so flag-carried flow is representable.
 *
 * ===== Increment 8 (XMM/SIMD taint) GRANULARITY DECISION — per-byte, documented in-code =====
 * XMM lane tags are stored PER-BYTE (AT_RT_XMM_BYTES = 16 tag bytes per 128-bit register),
 * mirroring the integer MEMORY shadow's 1:1 byte scale rather than the GP file's whole-
 * register single byte. Rationale: an SSE load/store moves a whole 16-byte span to/from the
 * per-byte memory shadow, so per-byte XMM lanes let a later increment carry lane-precise
 * taint end-to-end WITHOUT another storage/ABI change (the storage is the forward-compatible
 * seam, exactly as the memory shadow is already per-byte). COST, as the plan warns: this
 * MULTIPLIES the reg-tag traffic — a 16-byte XMM source/dest is 16 byte OR/store ops rather
 * than one. What this FIRST SLICE actually PROPAGATES is still the whole-register scalar
 * union+broadcast used for GP: every source lane (and every mem/GP source) is unioned into the
 * single scalar accumulator s_t, and s_t is broadcast to ALL 16 dest lane bytes. So this slice
 * is conservative (any tainted lane taints the whole dest; a narrow SIMD write like movq/movd
 * over-taints the untouched high lanes) and lane-INDEPENDENT flow (pshufd, per-lane arithmetic,
 * narrow-write masking) is DEFERRED — the per-byte storage is where that refinement lands. This
 * matches the GP whole-register posture and keeps the oracle diff exact at STEP granularity
 * (all asmtest_slice_forward distinguishes). YMM/ZMM upper lanes and VSIB vector-gather EA math
 * remain deferred (XMM-only this slice). */
#define AT_RT_GPR_BASE  0
#define AT_RT_EFLAGS    16
#define AT_RT_XMM_BASE  17 /* first XMM lane-tag byte                       */
#define AT_RT_XMM_BYTES 16 /* per-byte lanes: 16 tag bytes per 128-bit reg  */
#define AT_RT_XMM_COUNT 16 /* XMM0..XMM15                                   */
#define AT_RT_COUNT     (AT_RT_XMM_BASE + AT_RT_XMM_COUNT * AT_RT_XMM_BYTES)
static int g_tls_regfile = -1;

/* App-emitted seed/sink marker PCs (resolved by dr_get_proc_address, like pc_marker),
 * and the app-owned sink report the client appends hits into (registered at the sink
 * marker). Single-threaded native fixture this increment, so appends are unlocked; the
 * launched multithreaded workload (Increment 5) backs g_report with shared memory. */
static app_pc pc_seed_marker;
static app_pc pc_sink_marker;
static at_taint_report_t *g_report;

/* Managed method-load auto-registration (Increment 6 dotnet slice). A launched managed
 * workload calls NO region marker, so the client learns which JIT'd method ranges to
 * instrument from the .NET perfmap (`DOTNET_PerfMapEnabled=1` writes /tmp/perf-<pid>.map,
 * one `<hex start> <hex size> <symbol>` line per method as it is JIT'd — the same channel
 * src/ptrace_backend.c parses). The `methodscan=<substr>` client option arms this: a client
 * poller thread re-reads the perfmap and registers every method whose symbol contains the
 * substring, so range-count > 1 arises with no per-range C marker. */
static char g_methodscan[96];
static int g_methodscan_active;
/* The APP's process id, captured at client init on the app thread. The poller must NOT call
 * dr_get_process_id() itself: a DR client thread has its OWN pid on Linux (dr_tools.h), so
 * from the poller dr_get_process_id() returns the client-thread pid, not the app's — and the
 * .NET perfmap is /tmp/perf-<app-pid>.map. */
static int g_app_pid;
static volatile int g_poller_stop;
static volatile int g_poller_done;

/* Throwaway capture sink for the marker-less managed path: instrumentation needs a drval to
 * write into (so it runs + counts via g_inscount), but the managed workload registers none
 * and we are NOT oracle-diffing managed code here (that is Increments 5(3)/9). A small
 * client-internal buffer suffices; it overflows honestly and is never read. */
static at_vstep_t g_self_steps[256];
static at_tag_t g_self_taint[256];
static at_drval_t g_self_drval;

/* Map a DR reg id to its reg-tag-file index (canonical 64-bit GP container -> 0..15),
 * or -1 if not a tracked GP register. */
static int rt_index(reg_id_t reg) {
    if (!reg_is_gpr(reg))
        return -1;
    reg_id_t r64 = reg_to_pointer_sized(reg);
    int idx = (int)(r64 - DR_REG_RAX);
    return (idx >= 0 && idx < 16) ? (AT_RT_GPR_BASE + idx) : -1;
}

/* Increment 8 (SIMD): map an XMM register id to the byte offset of its lane-0 tag in the
 * reg-tag file, or -1 if not a tracked XMM register. XMM0..XMM15 are contiguous DR reg ids;
 * a YMM/ZMM operand (upper lanes) is intentionally NOT mapped here (deferred) so it falls
 * through as untracked rather than aliasing the low 128 bits inconsistently. */
static int rt_xmm_base(reg_id_t reg) {
    if (reg >= DR_REG_XMM0 && reg <= DR_REG_XMM15)
        return AT_RT_XMM_BASE + (int)(reg - DR_REG_XMM0) * AT_RT_XMM_BYTES;
    return -1;
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

#ifdef ASMTEST_TAINT_GCREMAP
/* Resolve a shadow byte pointer for `ea` WITHOUT creating its leaf: returns NULL for a
 * non-canonical address or an as-yet-untouched (null-leaf) page. Read-only companion to
 * tag_ptr_create — the GC-move remap uses it to read source tags and to clear the old
 * range without materializing leaves for never-touched memory. Behind the disabled
 * ASMTEST_TAINT_GCREMAP flag so the default taint client is unchanged. */
static at_tag_t *tag_ptr_lookup(uint64_t ea) {
    size_t i = (size_t)(ea >> AT_LEAF_BITS);
    if (i >= AT_DIR_LEN)
        return NULL;
    at_tag_t *lf = __atomic_load_n(&g_dir[i], __ATOMIC_ACQUIRE);
    if (lf == NULL)
        return NULL;
    return lf + (ea & AT_LEAF_MASK);
}
#endif /* ASMTEST_TAINT_GCREMAP */

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

#ifdef ASMTEST_TAINT_GCREMAP
/* ================================================================== */
/* Increment 7 (PARTIAL slice): GC-move umbra shadow remap             */
/* ================================================================== */
/*
 * When a compacting .NET GC relocates an object, its shadow tags must move with it or a
 * post-compaction read sees stale/aliased taint. This is the LANDABLE PARTIAL slice of
 * Increment 7: the remap CODE PATH plus a synthetic-triple unit test, behind the DISABLED
 * compile flag ASMTEST_TAINT_GCREMAP so the default value/taint clients are byte-identical
 * (dr-valtrace-inlined-test, dr-taint-native-test stay green) and no hot-path clean call
 * is added (the inline-gate count is unchanged). DEFERRED as externally hard-blocked on
 * Phase 4: the live GCBulkMovedObjectRanges {old,new,len} feed via EventPipe, the coherence
 * canary over a real forced GC, and the launch-under-DR CI gate. This slice proves ONLY
 * that, GIVEN a {old,new,len} triple, the tag survives at the NEW address and is absent at
 * the OLD one.
 *
 * SEQUENCING vs the Increment-4 concurrent-writer policy: the remap is a BULK shadow
 * mutation, not a hot-path byte store, so it is serialized under g_lock and bracketed by
 * SEQ_CST fences against in-flight inline tag stores. The FULL live path would additionally
 * dr_suspend_all_other_threads()/resume around the copy (a true world-stop quiesce, since
 * the hot-path tag stores are plain non-atomic byte writes) — deferred with the rest of the
 * live GC path. In the synthetic unit test the client is single-threaded (the remap runs at
 * client init, before the app's threads start), so the lock+fence is sufficient and exact.
 */

/* Remap the shadow for one moved range: copy tags from [old_base, +len) to
 * [new_base, +len), then CLEAR the old range so no stale tag aliases the freed memory.
 * A source-snapshot (dr_global_alloc) makes it correct for ARBITRARY old/new overlap: read
 * all source tags first, clear old, then paint new from the snapshot — an address in both
 * ranges ends with its snapshot value, never a spurious clear. Off the hot path. */
static void at_gc_remap(uint64_t old_base, uint64_t new_base, uint64_t len) {
    if (len == 0 || old_base == new_base)
        return;
    dr_mutex_lock(g_lock);
    __atomic_thread_fence(__ATOMIC_SEQ_CST); /* order vs in-flight inline tag stores */

    at_tag_t *snap = (at_tag_t *)dr_global_alloc((size_t)len);
    if (snap == NULL) {
        dr_mutex_unlock(g_lock);
        return; /* conservative: leave the shadow untouched rather than corrupt it */
    }
    for (uint64_t k = 0; k < len; k++) {
        at_tag_t *s = tag_ptr_lookup(old_base + k);
        snap[k] = (s != NULL) ? *s : AT_TAG_CLEAN;
    }
    /* Clear the old range first (never-touched bytes stay null-leaf; no leaf is created
     * just to write CLEAN), so an overlapped byte is re-established by the new-range paint. */
    for (uint64_t k = 0; k < len; k++) {
        at_tag_t *s = tag_ptr_lookup(old_base + k);
        if (s != NULL)
            *s = AT_TAG_CLEAN;
    }
    /* Paint the new range from the snapshot (create-on-touch for the destination leaves). */
    for (uint64_t k = 0; k < len; k++) {
        if (snap[k] == AT_TAG_CLEAN)
            continue; /* nothing to move; do not materialize a clean leaf */
        at_tag_t *d = tag_ptr_create(new_base + k);
        if (d != NULL)
            *d = snap[k];
    }
    dr_global_free(snap, (size_t)len);

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    dr_mutex_unlock(g_lock);
}

/* Synthetic-triple unit test (Increment 7 partial): hand-provided {old,new,len} triples,
 * NO live GC. For each: seed the OLD range, remap, and assert the tag is readable at the
 * NEW address and absent at the OLD one. Emits TAP on STDERR + a grep-able summary line the
 * dr-taint-gcremap-test lane checks. Runs once at client init (single-threaded). */
static int g_gcr_checks, g_gcr_fails;
static void gcr_check(int cond, const char *msg) {
    g_gcr_checks++;
    dr_fprintf(STDERR, "%s %d - %s\n", cond ? "ok" : "not ok", g_gcr_checks, msg);
    if (!cond)
        g_gcr_fails++;
}
static at_tag_t gcr_rd(uint64_t ea) {
    at_tag_t *p = tag_ptr_lookup(ea);
    return (p != NULL) ? *p : AT_TAG_CLEAN;
}

/* Synthetic shadow VAs: canonical, page-spread, never dereferenced (only their shadow is
 * touched), chosen to sit in distinct 1 MiB leaves and not collide with app memory. */
static void run_gcremap_selftest(void) {
    dr_fprintf(STDERR, "# gcremap-selftest: synthetic {old,new,len} triples "
                       "(no live GC; Phase-4-blocked full path deferred)\n");

    /* T1: disjoint move, uniform color. Tag survives at NEW, absent at OLD. */
    {
        uint64_t old = 0x40000000ull, nw = 0x50000000ull, len = 16;
        on_seed(old, len, AT_TAG_TAINTED);
        gcr_check(gcr_rd(nw) == AT_TAG_CLEAN, "T1 pre: new range clean before remap");
        at_gc_remap(old, nw, len);
        int new_ok = 1, old_ok = 1;
        for (uint64_t k = 0; k < len; k++) {
            if (gcr_rd(nw + k) != AT_TAG_TAINTED)
                new_ok = 0;
            if (gcr_rd(old + k) != AT_TAG_CLEAN)
                old_ok = 0;
        }
        gcr_check(new_ok, "T1: tag readable at NEW address after remap");
        gcr_check(old_ok, "T1: no stale tag at OLD address after remap");
    }

    /* T2: per-byte colour fidelity — distinct tag per byte survives the move 1:1. */
    {
        uint64_t old = 0x41000000ull, nw = 0x52000000ull, len = 8;
        for (uint64_t k = 0; k < len; k++) {
            at_tag_t *p = tag_ptr_create(old + k);
            if (p != NULL)
                *p = (at_tag_t)(0x80u + k); /* distinct non-clean colours */
        }
        at_gc_remap(old, nw, len);
        int fid = 1;
        for (uint64_t k = 0; k < len; k++)
            if (gcr_rd(nw + k) != (at_tag_t)(0x80u + k) ||
                gcr_rd(old + k) != AT_TAG_CLEAN)
                fid = 0;
        gcr_check(fid, "T2: per-byte colours move 1:1 (new match, old cleared)");
    }

    /* T3: NEGATIVE — an unseeded OLD range must not conjure taint at NEW. */
    {
        uint64_t old = 0x43000000ull, nw = 0x54000000ull, len = 16;
        at_gc_remap(old, nw, len);
        int clean = 1;
        for (uint64_t k = 0; k < len; k++)
            if (gcr_rd(nw + k) != AT_TAG_CLEAN)
                clean = 0;
        gcr_check(clean, "T3 negative: unseeded move leaves NEW clean (no phantom taint)");
    }

    /* T4: OVERLAPPING move (compaction slides an object down/up over itself). Snapshot +
     * clear-then-paint must keep the overlap byte's tag, clear only the non-overlap tail. */
    {
        uint64_t old = 0x60000000ull, len = 32, nw = old + 16; /* 16-byte overlap */
        on_seed(old, len, AT_TAG_TAINTED);
        at_gc_remap(old, nw, len);
        int new_all = 1;
        for (uint64_t k = 0; k < len; k++)
            if (gcr_rd(nw + k) != AT_TAG_TAINTED)
                new_all = 0;
        gcr_check(new_all, "T4 overlap: whole NEW range tainted after slide");
        int head_clear = 1;
        for (uint64_t k = 0; k < 16; k++) /* [old, new) is the freed non-overlap head */
            if (gcr_rd(old + k) != AT_TAG_CLEAN)
                head_clear = 0;
        gcr_check(head_clear, "T4 overlap: freed non-overlap head cleared (no stale alias)");
    }

    dr_fprintf(STDERR, "1..%d\n", g_gcr_checks);
    dr_fprintf(STDERR, "ASMTEST_GCREMAP_SELFTEST checks=%d fails=%d\n",
               g_gcr_checks, g_gcr_fails);
}
#endif /* ASMTEST_TAINT_GCREMAP */

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

/* Increment 8 (SIMD): union ALL 16 per-byte lane tags of an XMM source register at
 * reg-tag-file byte offset `base` into s_t (whole-register collapse — any tainted lane
 * taints the result). 16 byte-ORs: the per-byte reg-tag traffic the granularity note flags.
 * t_rf = regfile base. */
static void emit_xmm_regtag_or(void *dc, instrlist_t *bb, instr_t *where,
                               reg_id_t t_rf, int base, reg_id_t s_t) {
    for (int k = 0; k < AT_RT_XMM_BYTES; k++)
        instrlist_meta_preinsert(
            bb, where,
            INSTR_CREATE_or(
                dc, opnd_create_reg(reg_resize_to_opsz(s_t, OPSZ_1)),
                opnd_create_base_disp(t_rf, DR_REG_NULL, 0, base + k, OPSZ_1)));
}

/* Increment 8 (SIMD): broadcast s_t's low tag byte to ALL 16 per-byte lane tags of an XMM
 * dst register at reg-tag-file byte offset `base` (whole-register broadcast — conservative,
 * see the granularity note). 16 byte-stores. */
static void emit_xmm_regtag_store(void *dc, instrlist_t *bb, instr_t *where,
                                  reg_id_t t_rf, int base, reg_id_t s_t) {
    for (int k = 0; k < AT_RT_XMM_BYTES; k++)
        instrlist_meta_preinsert(
            bb, where,
            INSTR_CREATE_mov_st(
                dc,
                opnd_create_base_disp(t_rf, DR_REG_NULL, 0, base + k, OPSZ_1),
                opnd_create_reg(reg_resize_to_opsz(s_t, OPSZ_1))));
}
#endif /* ASMTEST_TAINT */

/* ------------------------------------------------------------------ */
/* drx_buf flush: drain raw records into the app-owned at_drval_t        */
/* ------------------------------------------------------------------ */

static void buf_flush(void *drcontext, void *buf_base, size_t size) {
    (void)drcontext;
    at_drval_t *dv = g_drval;
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

/* Append [base, base+len) to the instrumented range set (deduped by base), lower the offset
 * origin, and (re)instrument the range. Shared by the region marker (on_marker) and the
 * managed method-load poller (scan_perfmap); safe to call repeatedly with the same base. */
static void register_range(app_pc base, size_t len) {
    int fresh = 0;
    dr_mutex_lock(g_lock);
    int n = g_nregions;
    int dup = 0;
    for (int i = 0; i < n; i++)
        if (g_regions[i].base == base) {
            dup = 1;
            break;
        }
    if (!dup && n < AT_MAX_REGIONS) {
        if (g_origin == NULL || base < g_origin)
            g_origin = base; /* offset origin = lowest registered base */
        g_regions[n].base = base;
        g_regions[n].len = len;
        __atomic_store_n(&g_nregions, n + 1, __ATOMIC_RELEASE); /* publish */
        fresh = 1;
    }
    dr_mutex_unlock(g_lock);
    /* (Re)instrument a freshly-registered (or freshly-mmap'd, never-executed) range so it
     * picks up the value/taint instrumentation on its next execution. */
    if (fresh)
        dr_delay_flush_region(base, len, 0, NULL);
}

static void on_marker(app_pc base, size_t len, at_drval_t *drval) {
    dr_mutex_lock(g_lock);
    if (g_drval == NULL)
        g_drval = drval; /* first marker latches the shared capture buffer */
    dr_mutex_unlock(g_lock);
    register_range(base, len);
}

#ifdef ASMTEST_TAINT
/* ------------------------------------------------------------------ */
/* Managed method-load auto-registration (Increment 6 dotnet slice)     */
/* ------------------------------------------------------------------ */

/* Case-sensitive substring test — no libc under DR's private loader. */
static int str_contains(const char *hay, const char *needle) {
    if (needle[0] == '\0')
        return 1;
    for (const char *h = hay; *h != '\0'; h++) {
        const char *a = h;
        const char *b = needle;
        while (*a != '\0' && *b != '\0' && *a == *b) {
            a++;
            b++;
        }
        if (*b == '\0')
            return 1;
    }
    return 0;
}

/* Parse one lower/upper hex integer at *sp, advancing past it; returns 0 if no hex digit. */
static int parse_hex(const char **sp, uint64_t *out) {
    const char *s = *sp;
    uint64_t v = 0;
    int any = 0;
    /* Skip an optional 0x/0X prefix — the .NET perfmap writes the start address as
     * `0x<hex>` (the size field is bare hex), so without this every line mis-parses. */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    for (;; s++) {
        char c = *s;
        int d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            break;
        v = v * 16 + (uint64_t)d;
        any = 1;
    }
    *out = v;
    *sp = s;
    return any;
}

/* Read /tmp/perf-<pid>.map and register every method whose symbol contains g_methodscan
 * (deduped in register_range). Off the hot path (poller thread only). A method too late to
 * fit the read buffer is simply picked up on a later poll once earlier ones are gone or by
 * a larger read; for the tiny scan-target set this reads the whole map in one pass. */
static void scan_perfmap(void) {
    if (!g_methodscan_active)
        return;
    char path[64];
    dr_snprintf(path, sizeof path, "/tmp/perf-%d.map", g_app_pid);
    path[sizeof path - 1] = '\0';
    file_t f = dr_open_file(path, DR_FILE_READ);
    if (f == INVALID_FILE)
        return;
    static char buf
        [1 << 18]; /* 256 KiB; poller is single-threaded so a static is fine */
    ssize_t got = dr_read_file(f, buf, sizeof buf - 1);
    dr_close_file(f);
    if (got <= 0)
        return;
    buf[got] = '\0';
    for (char *p = buf; *p != '\0';) {
        char *nl = p;
        while (*nl != '\0' && *nl != '\n')
            nl++;
        char saved = *nl;
        *nl = '\0';
        const char *s = p;
        uint64_t start = 0, size = 0;
        while (*s == ' ' || *s == '\t')
            s++;
        if (parse_hex(&s, &start)) {
            while (*s == ' ' || *s == '\t')
                s++;
            if (parse_hex(&s, &size) && size > 0) {
                while (*s == ' ' || *s == '\t')
                    s++;
                if (str_contains(s, g_methodscan))
                    register_range((app_pc)(uintptr_t)start, (size_t)size);
            }
        }
        if (saved == '\0')
            break;
        p = nl + 1;
    }
}

/* Client poller thread: re-scan the perfmap until the workload exits (or a ~10 s cap), so
 * methods JIT'd mid-run are registered + flushed into instrumentation as they appear. Does
 * not run app code — pure DR API (open/read/flush/sleep). */
static void poller_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < 1000 && !g_poller_stop; i++) {
        scan_perfmap();
        dr_sleep(10);
    }
    g_poller_done = 1;
}
#endif /* ASMTEST_TAINT */

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
/* Increment 8 (SIMD): the taint phase's shadow accessors (emit_shadow_or/emit_shadow_store)
 * are per-byte loops and so handle any width, UNLIKE the value pass's inline GP load/store
 * which is bounded to 1/2/4/8 (capturable_mem_size). This width gate additionally admits the
 * 16-byte SSE operand (movdqu/movdqa/movups...) for TAINT ONLY — the value capture still
 * skips it (no 16-byte GP load exists), which is why the SIMD memory SOURCE tag is unioned by
 * a dedicated inline-EA loop below rather than read back from the value record. Same base+disp
 * / non-far / non-segmented restriction as the value pass. 32-byte YMM is deferred. */
static uint16_t taint_mem_size(opnd_t op) {
    if (!opnd_is_base_disp(op))
        return 0;
    if (opnd_is_far_memory_reference(op))
        return 0;
    if (opnd_get_segment(op) != DR_REG_NULL)
        return 0;
    uint16_t sz = (uint16_t)opnd_size_in_bytes(opnd_get_size(op));
    if (sz != 1 && sz != 2 && sz != 4 && sz != 8 && sz != 16)
        return 0;
    return sz;
}

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
            reg_id_t r = opnd_get_reg(op);
            int idx = rt_index(r);
            if (idx >= 0)
                emit_regtag_or(dc, bb, instr, t_rf, idx, s_t);
            else {
                int xb = rt_xmm_base(r); /* Increment 8: XMM source register */
                if (xb >= 0)
                    emit_xmm_regtag_or(dc, bb, instr, t_rf, xb, s_t);
            }
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

    /* Increment 8 (SIMD) memory sources: the value pass captures only 1/2/4/8-byte loads, so a
     * 16-byte SSE load's EA is NOT in this record. Enumerate the instruction's memory SOURCE
     * operands ourselves, and for any WIDER-than-8 (16-byte) one compute its EA inline (like
     * the store-dst path) and OR its per-byte shadow into s_t. The <=8 ones are already handled
     * above via the record, so gate on >8 to avoid double-processing. Same address-register
     * aliasing skip as the store path (a conservative miss). */
    if (instr_reads_memory(instr)) {
        for (int s = 0; s < instr_num_srcs(instr); s++) {
            opnd_t op = instr_get_src(instr, s);
            if (!opnd_is_memory_reference(op))
                continue;
            uint16_t ssz = taint_mem_size(op);
            if (ssz <= 8)
                continue; /* handled by the record-based loop above */
            reg_id_t bse = opnd_get_base(op), idxr = opnd_get_index(op);
            if (bse == s_ptr || bse == s_a || bse == s_b || bse == s_t ||
                bse == t_rf || idxr == s_ptr || idxr == s_a || idxr == s_b ||
                idxr == s_t || idxr == t_rf)
                continue;
            reg_id_t swap = DR_REG_NULL;
            drreg_restore_app_values(dc, bb, instr, op, &swap);
            instrlist_meta_preinsert(
                bb, instr,
                INSTR_CREATE_lea(dc, opnd_create_reg(s_a),
                                 opnd_create_base_disp(
                                     opnd_get_base(op), opnd_get_index(op),
                                     opnd_get_scale(op), opnd_get_disp(op),
                                     OPSZ_lea)));
            emit_shadow_or(dc, bb, instr, s_a, s_b, t_rf, s_t, ssz);
            if (swap != DR_REG_NULL)
                drreg_unreserve_register(dc, bb, instr, swap);
        }
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
            reg_id_t r = opnd_get_reg(op);
            int idx = rt_index(r);
            if (idx >= 0)
                emit_regtag_store(dc, bb, instr, t_rf, idx, s_t);
            else {
                int xb = rt_xmm_base(r); /* Increment 8: XMM dest register */
                if (xb >= 0)
                    emit_xmm_regtag_store(dc, bb, instr, t_rf, xb, s_t);
            }
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
        uint16_t dsz = opnd_is_memory_reference(op) ? taint_mem_size(op) : 0;
        if (dsz == 0)
            continue; /* Increment 8: taint_mem_size also admits the 16-byte SSE store */
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

/* Is `ipc` in the client's instrumentation scope? On yes, set *off to its offset in the
 * shared origin space (at_vstep_t.off). scope=ranges (default) tests membership in the
 * registered range set; scope=whole tests the single window [min base, max end) that spans
 * them, so it also instruments the un-instrumented GAPS between ranges — the extra cost the
 * scoped default avoids (the inscount-delta exit criterion). Runs at bb-build time, not the
 * runtime hot path, so a linear scan over <=AT_MAX_REGIONS ranges is fine. */
static int in_scope(app_pc ipc, uint64_t *off) {
    int n = __atomic_load_n(&g_nregions, __ATOMIC_ACQUIRE);
    if (n <= 0)
        return 0;
    if (g_scope_whole) {
        app_pc lo = g_regions[0].base,
               hi = g_regions[0].base + g_regions[0].len;
        for (int i = 1; i < n; i++) {
            app_pc e = g_regions[i].base + g_regions[i].len;
            if (g_regions[i].base < lo)
                lo = g_regions[i].base;
            if (e > hi)
                hi = e;
        }
        if (ipc >= lo && ipc < hi) {
            *off = (uint64_t)(ipc - g_origin);
            return 1;
        }
        return 0;
    }
    for (int i = 0; i < n; i++) {
        if (ipc >= g_regions[i].base &&
            ipc < g_regions[i].base + g_regions[i].len) {
            *off = (uint64_t)(ipc - g_origin);
            return 1;
        }
    }
    return 0;
}

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

    uint64_t off;
    if (!in_scope(ipc, &off))
        return DR_EMIT_DEFAULT;
    at_drval_t *dv = g_drval;
    if (dv == NULL)
        return DR_EMIT_DEFAULT; /* in scope but no capture buffer registered yet */
    __atomic_fetch_add(&g_inscount, 1, __ATOMIC_RELAXED);

#ifdef ASMTEST_TAINT
    /* Branch-condition SINK (kind = 1): at each in-region conditional branch, insert a
     * transparent clean call that appends a hit iff the flag it reads is tainted. Placed
     * FIRST (before the value/propagation instrumentation of this branch), so at runtime
     * it observes the reg-tag file as left by the PRIOR (flag-defining) instruction's
     * inline propagation; being a clean call it restores the app flags the branch then
     * reads. off is the branch's offset in the shared origin space; ea = 0 (a
     * register/flag sink). */
    if (instr_is_cbr(instr)) {
        dr_insert_clean_call(dc, bb, instr, (void *)on_sink, false, 3,
                             OPND_CREATE_INTPTR((ptr_uint_t)off),
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
    store_imm64(dc, bb, instr, s_ptr, s_a, off,
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
#ifdef ASMTEST_TAINT
    /* Signal the method-load poller to stop, but do NOT wait for it here. At process exit DR
     * suspends client threads (they run only while DR is executing the app), so spinning on
     * dr_sleep() for the poller's ack would deadlock DR's exit synchronization — empirically
     * the drrun-over-dotnet exit hangs iff BOTH a live poller client thread AND this wait are
     * present; removing either avoids it. DR terminates the client thread as part of the exit
     * sequence; we merely flip the stop flag. Because the poller may therefore still be live
     * (and holding g_lock inside register_range) while this runs, we deliberately DO NOT
     * destroy g_lock or free g_dir under methodscan — the OS reclaims them at process exit,
     * and leaking them avoids a teardown-vs-poller race. */
    g_poller_stop = 1;
#endif
    /* Scoping cost metric (Increment 6): report how many app instructions the client
     * instrumented and how many ranges it scoped to, so a scope=whole vs scope=ranges pair
     * of runs can prove the inscount delta (the cost bound). A single grep-able line. */
    dr_fprintf(STDERR,
               "ASMTEST_TAINT_INSCOUNT inscount=%llu regions=%d scope=%s\n",
               (unsigned long long)g_inscount, g_nregions,
               g_scope_whole ? "whole" : "ranges");
    if (g_buf != NULL)
        drx_buf_free(g_buf);
    drx_exit();
    drreg_exit();
    drmgr_exit();
#ifdef ASMTEST_TAINT
    /* Skip g_lock/g_dir teardown while the poller may still touch them (see above); leak at
     * exit. Without methodscan there is no poller, so tear them down normally. */
    if (!g_methodscan_active) {
        dr_mutex_destroy(g_lock);
        if (g_dir != NULL)
            dr_raw_mem_free(g_dir, AT_DIR_LEN * sizeof(at_tag_t *));
    }
#else
    dr_mutex_destroy(g_lock);
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
    /* Client options (drrun -c <client> [opts] -- <app>), scanned across all argv (argv[0]
     * is the client path): `scope=whole` instruments the whole window spanning the
     * registered ranges instead of only the ranges (Increment 6 inscount toggle);
     * `methodscan=<substr>` auto-registers .NET method ranges from the perfmap whose symbol
     * contains <substr> (Increment 6 managed method-load slice). */
    for (int i = 0; i < argc; i++) {
        if (argv[i] == NULL)
            continue;
        if (strcmp(argv[i], "scope=whole") == 0)
            g_scope_whole = 1;
#ifdef ASMTEST_TAINT
        else if (strncmp(argv[i], "methodscan=", 11) == 0) {
            dr_snprintf(g_methodscan, sizeof g_methodscan, "%s", argv[i] + 11);
            g_methodscan[sizeof g_methodscan - 1] = '\0';
            g_methodscan_active = (g_methodscan[0] != '\0');
        }
#endif
    }
#ifdef ASMTEST_TAINT_GCREMAP
    /* Client option (drrun -c <client> gcremap_selftest -- <app>): run the Increment 7
     * PARTIAL synthetic-triple GC-move remap unit test at init, then let the app run. */
    int gcremap_selftest = 0;
    for (int i = 0; i < argc; i++)
        if (argv[i] != NULL && strcmp(argv[i], "gcremap_selftest") == 0)
            gcremap_selftest = 1;
#endif
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

#ifdef ASMTEST_TAINT_GCREMAP
    /* Increment 7 (partial): run the synthetic GC-move remap unit test now, before the
     * app's threads exist (so the lock+fence quiesce in at_gc_remap is exact). */
    if (gcremap_selftest)
        run_gcremap_selftest();
#endif
#ifdef ASMTEST_TAINT
    /* Managed method-load auto-registration (Increment 6 dotnet slice): a launched managed
     * workload registers no C region marker, so point capture at a throwaway internal buffer
     * (we are not oracle-diffing managed code here) and spawn the perfmap poller that
     * discovers + registers JIT'd method ranges by symbol substring. */
    if (g_methodscan_active) {
        /* Capture the app pid HERE (dr_client_main runs on the app's thread), not in the
         * poller (a client thread has its own pid) — see g_app_pid. */
        g_app_pid = (int)dr_get_process_id();
        g_self_drval.steps = g_self_steps;
        g_self_drval.steps_cap = sizeof g_self_steps / sizeof g_self_steps[0];
        g_self_drval.step_taint = g_self_taint;
        g_self_drval.step_taint_cap =
            sizeof g_self_taint / sizeof g_self_taint[0];
        g_drval = &g_self_drval;
        if (!dr_create_client_thread(poller_thread, NULL))
            dr_fprintf(STDERR,
                       "drtaint-inlined: method-load poller create failed\n");
    }
#endif
}
