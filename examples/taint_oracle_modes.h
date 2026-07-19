/*
 * taint_oracle_modes.h — the shared per-mode DRIVER CONFIG for the libdft64 differential
 * oracle. So the DR side (examples/taint_oracle_diff.c) and the libdft side
 * (examples/pin_taint.c) drive each fixture with BYTE-IDENTICAL inputs (same buffers,
 * args, seed range), the mode table lives here, next to the shared fixture bytes
 * (taint_fixtures.h + taint_simd_fixtures.h). Included ONLY by the two oracle drivers —
 * NOT by dr_taint.c / dr_taint_simd.c (whose modes are hand-written) — so no unused-const
 * warning arises.
 *
 * The GP/integer modes are ASSERTED (DR must equal libdft, byte-for-byte). The XMM/SSE
 * modes are INFORMATIONAL (informational=1, gap_token "libdft-partial-sse-avx"): libdft's
 * SSE rules are "basic" + unverified upstream, so the diff reports the delta and never
 * fails on it (T6 — the honest boundary).
 *
 * <stdint.h>/<string.h>-only.
 */
#ifndef ASMTEST_TAINT_ORACLE_MODES_H
#define ASMTEST_TAINT_ORACLE_MODES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "taint_fixtures.h"
#include "taint_simd_fixtures.h"

enum at_arg1_kind { ARG1_SCALAR = 0, ARG1_BUF2 = 1, ARG1_ZERO = 2 };

/* One per driver mode. buf/buf2 and the seed range mirror dr_taint.c / dr_taint_simd.c. */
struct at_fixmode {
    const char *name;
    const uint8_t *code;
    size_t code_len;
    long buf_val;    /* initial *buf when seed_bytes == NULL (first 8 bytes)  */
    int arg1_kind;   /* ARG1_SCALAR(5) / ARG1_BUF2 / ARG1_ZERO               */
    int do_seed;     /* seed [buf+seed_off, +seed_len) with AT_TAG_TAINTED   */
    int seed_off;    /* seed base offset from buf (4 for highbyte)           */
    int seed_len;    /* 8 GP / 4 highbyte / 16 SIMD                          */
    int sink_family; /* a sink is watched (register the report)              */
    long want_result;
    int check_result;
    int expect_hits;  /* sink hits the DR side produces (the reference)      */
    uint64_t hit_off; /* the sink offset, when expect_hits == 1             */
    uint8_t hit_kind; /* the sink kind (0 mem-len, 1 branch, 2 call-arg)    */
    int buf_size;     /* buffer size in bytes (8 GP, 16 XMM)               */
    const uint8_t
        *seed_bytes; /* NULL -> *buf = buf_val; else memcpy buf_size */
    int informational; /* 1 = named-skip (report the DR/libdft delta, never fail) */
    const char *gap_token; /* the printed gap token for an informational mode */
};

#define AT_FIX_SZ(a) (sizeof(a))
#define AT_SSE_GAP   "libdft-partial-sse-avx"
static const struct at_fixmode AT_ORACLE_MODES[] = {
    /* --- GP/integer-memory subset: ASSERTED (DR must equal libdft) --- */
    {"seeded", taint_chain, AT_FIX_SZ(taint_chain), 7, ARG1_SCALAR, 1, 0, 8, 0,
     12, 1, 0, 0, 0, 8, NULL, 0, NULL},
    {"negative", taint_chain, AT_FIX_SZ(taint_chain), 7, ARG1_SCALAR, 0, 0, 8,
     0, 12, 1, 0, 0, 0, 8, NULL, 0, NULL},
    {"sink", taint_sink_chain, AT_FIX_SZ(taint_sink_chain), 7, ARG1_SCALAR, 1,
     0, 8, 1, 12, 1, 1, SINK_OFF, 1, 8, NULL, 0, NULL},
    {"sink-negative", taint_sink_chain, AT_FIX_SZ(taint_sink_chain), 7,
     ARG1_SCALAR, 0, 0, 8, 1, 12, 1, 0, 0, 0, 8, NULL, 0, NULL},
    {"heapstore", taint_heapstore, AT_FIX_SZ(taint_heapstore), 7, ARG1_BUF2, 1,
     0, 8, 0, 7, 1, 0, 0, 0, 8, NULL, 0, NULL},
    {"highbyte", taint_chain, AT_FIX_SZ(taint_chain), 7, ARG1_SCALAR, 1, 4, 4,
     0, 12, 1, 0, 0, 0, 8, NULL, 0, NULL},
    {"callarg", taint_callarg, AT_FIX_SZ(taint_callarg), 7, ARG1_ZERO, 1, 0, 8,
     1, 7, 1, 1, CALLARG_OFF, 2, 8, NULL, 0, NULL},
    {"callarg-negative", taint_callarg, AT_FIX_SZ(taint_callarg), 7, ARG1_ZERO,
     0, 0, 8, 1, 7, 1, 0, 0, 0, 8, NULL, 0, NULL},
    {"memlen", taint_memlen, AT_FIX_SZ(taint_memlen), 4, ARG1_BUF2, 1, 0, 8, 1,
     0, 1, 1, MEMLEN_OFF, 0, 8, NULL, 0, NULL},
    {"memlen-negative", taint_memlen, AT_FIX_SZ(taint_memlen), 4, ARG1_BUF2, 0,
     0, 8, 1, 0, 1, 0, 0, 0, 8, NULL, 0, NULL},
    /* --- XMM/SSE subset: INFORMATIONAL named skip (libdft SSE rules unverified) --- */
    {"simd-copy", simd_copy, AT_FIX_SZ(simd_copy), 0, ARG1_BUF2, 1, 0, 16, 0,
     (long)SEED_LO64, 1, 0, 0, 0, 16, SEED16, 1, AT_SSE_GAP},
    {"simd-sink", simd_sink, AT_FIX_SZ(simd_sink), 0, ARG1_BUF2, 1, 0, 16, 1,
     (long)SEED_LO64, 1, 1, SIMD_BRANCH_OFF, 1, 16, SEED16, 1, AT_SSE_GAP},
};
#define AT_ORACLE_NMODES (sizeof(AT_ORACLE_MODES) / sizeof(AT_ORACLE_MODES[0]))

static inline const struct at_fixmode *at_find_mode(const char *name) {
    for (size_t i = 0; i < AT_ORACLE_NMODES; i++)
        if (strcmp(AT_ORACLE_MODES[i].name, name) == 0)
            return &AT_ORACLE_MODES[i];
    return NULL;
}

#endif /* ASMTEST_TAINT_ORACLE_MODES_H */
