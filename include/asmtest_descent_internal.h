/*
 * asmtest_descent_internal.h — PRIVATE call-descent handle layout + the mutators the
 * descending step loop drives. Shared by src/descent.c (owns the pools, alloc/free, the
 * public setters/accessors) and src/ptrace_backend.c (the step loop that fills them).
 *
 * This is NOT a public header and NOT a bindings-parity tier header — it exposes the
 * struct internals so the loop can append to the pools without a function call per field,
 * exactly the way ss_backend/ptrace share the trace stream. Bindings never see it; they
 * read the handle through the scalar accessors in asmtest_ptrace.h.
 */
#ifndef ASMTEST_DESCENT_INTERNAL_H
#define ASMTEST_DESCENT_INTERNAL_H

#include "asmtest_ptrace.h"

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>

/* A (base, len) region for the allow-set / deny-set. */
typedef struct {
    uint64_t base;
    uint64_t len;
} asmtest_descent_region_t;

/* Shared incremental block-boundary tracker: a new block starts at `off` iff the
 * previous instruction's fall-through does not land exactly there. Every instruction-
 * stream materializer needs this exact rule (ptrace's normalize(), the two windowed
 * materializers, and asmtest_descent_frame_record() below) — one copy instead of four. */
typedef struct {
    int have_prev;
    uint64_t expected_next;
} asmtest_blockseq_t;

/* Returns 1 if `off` starts a new block given the tracker's prior state, then advances
 * the tracker for the next call: `len == 0` (undecodable) resets have_prev so the NEXT
 * address unconditionally opens a fresh block; otherwise expected_next becomes off+len. */
static inline int asmtest_blockseq_boundary(asmtest_blockseq_t *s, uint64_t off,
                                            size_t len) {
    int new_block = !s->have_prev || off != s->expected_next;
    if (len == 0) {
        s->have_prev = 0;
    } else {
        s->expected_next = off + len;
        s->have_prev = 1;
    }
    return new_block;
}

/* One recorded call the tracer did NOT follow (a stepped-over call-out at level >= 1). */
typedef struct {
    uint64_t site;      /* call-site byte offset within its calling frame  */
    uint64_t target;    /* absolute callee address                         */
    uint32_t depth;     /* depth of the calling frame (0 = root)           */
    int32_t from_frame; /* index of the calling frame                     */
} asmtest_descent_edge_t;

/* One descended frame: a self-contained trace whose offsets are relative to base.
 * Frame 0 is always the root registered region (a superset mirror of the flat trace). */
typedef struct {
    uint64_t base; /* absolute region base                                */
    uint64_t len;  /* region byte length                                  */
    uint32_t depth;
    int32_t parent; /* parent frame index; -1 for the root                */
    uint64_t *insns;
    size_t insns_len, insns_cap;
    uint64_t *blocks;
    size_t blocks_len, blocks_cap;
    /* Incremental block-normalization state (mirrors ptrace normalize()). */
    asmtest_blockseq_t seq;
} asmtest_descent_frame_t;

struct asmtest_descent {
    asmtest_descent_level_t level;
    uint32_t max_depth;
    uint64_t insn_budget;
    uint32_t watchdog_ms;
    asmtest_descent_region_t *allow;
    size_t allow_len, allow_cap;
    asmtest_descent_region_t *deny;
    size_t deny_len, deny_cap;
    asmtest_descent_resolver_fn resolver;
    void *resolver_user;
    asmtest_descent_denylist_fn denylist;
    void *denylist_user;
    asmtest_descent_edge_t *edges;
    size_t edges_len, edges_cap;
    asmtest_descent_frame_t *frames;
    size_t frames_len, frames_cap;
    int truncated;
    int depth_capped;
    /* asmtest_descent_use_default_denylist: populate `deny` at trace start with
     * the built-in GC/JIT/PLT-resolver/blocking-libc set. `applied` keeps a
     * reused handle from appending the same regions on every trace. */
    int use_default_denylist;
    int default_denylist_applied;
};

/* Conservative defaults (used when a setter is passed 0, and at allocation). L3's budget
 * is deliberately far below PTRACE_STREAM_CAP so a runaway descend self-truncates fast. */
#define ASMTEST_DESCENT_DEFAULT_MAX_DEPTH   8u
#define ASMTEST_DESCENT_DEFAULT_INSN_BUDGET 4096ull
#define ASMTEST_DESCENT_DEFAULT_WATCHDOG_MS 2000u

/* --- Mutators the step loop calls (defined in src/descent.c) --- */

/* Push a new frame [base, base+len) at `depth` under `parent`. Returns the new frame's
 * index, or -1 on OOM (which also sets truncated). */
int32_t asmtest_descent_push_frame(asmtest_descent_t *d, uint64_t base,
                                   uint64_t len, uint32_t depth,
                                   int32_t parent);

/* Record one executed instruction at offset `off` (bytes-relative to the frame base) in
 * frame `fi`, whose instruction is `insn_len` bytes (0 => undecodable, flags truncated).
 * Derives block boundaries from fall-through discontinuity exactly like normalize().
 * Returns 1 if this instruction started a NEW block, else 0 (so the caller can mirror the
 * same block decision into the flat asmtest_trace_t for frame 0). */
int asmtest_descent_frame_record(asmtest_descent_t *d, int32_t fi, uint64_t off,
                                 size_t insn_len);

/* Append a stepped-over call edge. */
void asmtest_descent_add_edge(asmtest_descent_t *d, uint64_t site,
                              uint64_t target, uint32_t depth,
                              int32_t from_frame);

void asmtest_descent_mark_truncated(asmtest_descent_t *d);
void asmtest_descent_mark_depth_capped(asmtest_descent_t *d);

/* If `addr` lies in one of the `n` regions, return 1 and (optionally) its extent. */
int asmtest_descent_region_contains(const asmtest_descent_region_t *arr,
                                    size_t n, uint64_t addr, uint64_t *base_out,
                                    uint64_t *len_out);

/* --- T5 test hooks (defined in src/ptrace_backend.c) --- */

/* Number of REAL /proc/<pid>/maps parses the L3 maps-snapshot cache has performed
 * since the last reset. A healthy L3 descent parses once per descent (plus one retry
 * per genuine cache miss), regardless of call-out count. */
uint64_t asmtest_descend_maps_parse_count(void);
void asmtest_descend_maps_parse_count_reset(void);

/* Thin wrappers around the file-static watchdog arm/disarm so a test can prove the
 * single-descent-at-a-time guard ("arm; arm again while active [refused]; disarm")
 * without driving two genuinely concurrent tracees. Same contract as the internal
 * descend_watchdog_arm/disarm: arm returns 1 if it actually armed the timer, 0 if a
 * descent was already active (refused, timer untouched); disarm always clears. */
int asmtest_descend_watchdog_arm_test(struct sigaction *saved_sa,
                                      struct itimerval *saved_it, uint32_t ms);
void asmtest_descend_watchdog_disarm_test(const struct sigaction *saved_sa,
                                          const struct itimerval *saved_it);

#endif /* ASMTEST_DESCENT_INTERNAL_H */
