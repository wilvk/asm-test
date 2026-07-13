/*
 * asmtest_taint.h — the byte-granular tag-shadow + seed/sink ABI for the
 * DynamoRIO in-band taint tier (dynamorio-taint-tier-plan.md, Increment 4). Shared
 * by the DR taint client (src/dataflow_dr_client_inlined.c built -DASMTEST_TAINT)
 * and the app-side driver / out-of-process validator.
 *
 * Taint is ADDITIVE over the Increment-3 value producer: the client keeps filling
 * the same at_drval_t (src/dataflow_dr.h) byte-for-byte, and layers a tag shadow +
 * this seed/sink surface on top. A tag is a small union-of-sources value per
 * application byte / per register: bit0 = tainted, the remaining bits carry an
 * up-to-7-color seed set, and union = bitwise OR. A union tag is MONOTONE within a
 * seed epoch — it only ever gains bits — which is what makes the tolerated-benign-
 * race single-byte store policy (naturally-aligned at_tag_t writes are atomic on
 * x86-64) sound: a lost update degrades to a conservative MISS, never a false
 * clean->tainted flip.
 *
 * Dependency-free by design — <stdint.h> / <stddef.h> only, and NO <stdbool.h>
 * (whose `bool` clashes with DynamoRIO's own), exactly like dataflow_dr.h — so the
 * client can include it directly alongside dr_api.h.
 */
#ifndef ASMTEST_TAINT_H
#define ASMTEST_TAINT_H

#include <stddef.h>
#include <stdint.h>

/* One tag per application byte / per register. 1-byte union-of-sources:
 *   bit0      = tainted
 *   bits1..7  = up-to-7-color seed set (union = bitwise OR)
 * A straight 1:1 byte map is the cheapest scale before a multi-color bitset; XMM/
 * YMM and per-byte register tags are deferred (Increment 8), matching the value
 * producer's GP + integer-memory scope. */
typedef uint8_t at_tag_t;
#define AT_TAG_CLEAN   0u
#define AT_TAG_TAINTED 1u /* bit0; a bare "tainted, color 0" seed */

/* App-emitted markers the client resolves by PC (dr_get_proc_address) and reads the
 * SysV argument registers at, exactly as the value client resolves
 * AT_DRVAL_MARKER_SYM (src/dataflow_dr_client.c) — NO drwrap. Under launch-under-DR
 * (Increment 5) the seed/sink config instead arrives via drrun client options. */
#define AT_TAINT_SEED_SYM                                                      \
    "asmtest_dr_taint_seed_marker" /* rdi=base rsi=len rdx=color */
#define AT_TAINT_SINK_SYM                                                      \
    "asmtest_dr_taint_sink_marker" /* rdi=at_taint_report_t*     */

/* A seeded source: [base, base+len) bytes get `color` in the shadow at seed time
 * (before traced code runs -> no concurrency). */
typedef struct at_taint_seed {
    uint64_t base;
    uint64_t len;
    at_tag_t color;
    uint8_t pad[7];
} at_taint_seed_t;

/* A sink hit: appended when a tainted value reaches a watched sink operand. `off`
 * is the sink instruction's region offset; `tag` is the union that arrived. `seed_off`
 * and `depth` (seed->sink propagation steps) are filled APP-SIDE by the validator's
 * def-use BFS — the client may leave them 0 — so the client stays inline and the
 * app-side oracle diff can compare the taint graph to the offline L2 slice. */
typedef struct at_taint_hit {
    uint64_t off; /* sink instruction offset within a registered range      */
    uint64_t ea;  /* sink operand effective address (0 for reg/branch)      */
    uint64_t
        seed_off; /* where the arriving tag was first seeded (app-side)      */
    at_tag_t tag; /* union tag observed at the sink                          */
    uint8_t kind; /* 0 = mem-len arg, 1 = branch cond, 2 = call arg, ...     */
    uint8_t pad[2];
    uint32_t
        depth; /* propagation steps seed->sink (app-side, for oracle diff) */
} at_taint_hit_t;

/* App-owned sink report the client fills in place — same append/truncate discipline
 * as at_drval_t / asmtest_trace_t (fixed cap, `truncated` on overflow, *_total keeps
 * counting past the cap). Under launch-under-DR (Increment 5) this is backed by a
 * POSIX shared-memory segment drained by a separate app-side validator. */
typedef struct at_taint_report {
    at_taint_hit_t *hits;
    size_t hits_cap;
    size_t hits_len;
    uint64_t hits_total;
    uint8_t truncated;
} at_taint_report_t;

#endif /* ASMTEST_TAINT_H */
