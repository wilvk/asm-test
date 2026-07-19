/*
 * taint_oracle_modes.h — the shared per-mode DRIVER CONFIG for the libdft64 differential
 * oracle. So the DR side (examples/taint_oracle_diff.c) and the libdft side
 * (examples/pin_taint.c) drive each fixture with BYTE-IDENTICAL inputs (same buffers,
 * args, seed range), the mode table lives here, next to the shared fixture bytes
 * (taint_fixtures.h). Included ONLY by the two oracle drivers — NOT by dr_taint.c (whose
 * modes are hand-written) — so no unused-const warning arises.
 *
 * <stdint.h>/<string.h>-only.
 */
#ifndef ASMTEST_TAINT_ORACLE_MODES_H
#define ASMTEST_TAINT_ORACLE_MODES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "taint_fixtures.h"

enum at_arg1_kind { ARG1_SCALAR = 0, ARG1_BUF2 = 1, ARG1_ZERO = 2 };

/* One per driver mode. buf/buf2 and the seed range mirror dr_taint.c's per-mode setup. */
struct at_fixmode {
    const char *name;
    const uint8_t *code;
    size_t code_len;
    long buf_val;    /* initial *buf                                        */
    int arg1_kind;   /* ARG1_SCALAR(5) / ARG1_BUF2 / ARG1_ZERO              */
    int do_seed;     /* seed [buf+seed_off, +seed_len) with AT_TAG_TAINTED  */
    int seed_off;    /* seed base offset from buf (4 for highbyte)          */
    int seed_len;    /* 8 normally, 4 for highbyte                          */
    int sink_family; /* a sink is watched (register the report)             */
    long want_result;
    int check_result;
    int expect_hits; /* sink hits BOTH engines must agree on (covered subset) */
    uint64_t hit_off; /* the sink offset, when expect_hits == 1             */
    uint8_t hit_kind; /* the sink kind (0 mem-len, 1 branch, 2 call-arg)    */
};

#define AT_FIX_SZ(a) (sizeof(a))
static const struct at_fixmode AT_ORACLE_MODES[] = {
    /* name              code               len                       buf a1          seed so sl sink res chk eh  hoff      hk */
    {"seeded", taint_chain, AT_FIX_SZ(taint_chain), 7, ARG1_SCALAR, 1, 0, 8, 0,
     12, 1, 0, 0, 0},
    {"negative", taint_chain, AT_FIX_SZ(taint_chain), 7, ARG1_SCALAR, 0, 0, 8,
     0, 12, 1, 0, 0, 0},
    {"sink", taint_sink_chain, AT_FIX_SZ(taint_sink_chain), 7, ARG1_SCALAR, 1,
     0, 8, 1, 12, 1, 1, SINK_OFF, 1},
    {"sink-negative", taint_sink_chain, AT_FIX_SZ(taint_sink_chain), 7,
     ARG1_SCALAR, 0, 0, 8, 1, 12, 1, 0, 0, 0},
    {"heapstore", taint_heapstore, AT_FIX_SZ(taint_heapstore), 7, ARG1_BUF2, 1,
     0, 8, 0, 7, 1, 0, 0, 0},
    {"highbyte", taint_chain, AT_FIX_SZ(taint_chain), 7, ARG1_SCALAR, 1, 4, 4,
     0, 12, 1, 0, 0, 0},
    {"callarg", taint_callarg, AT_FIX_SZ(taint_callarg), 7, ARG1_ZERO, 1, 0, 8,
     1, 7, 1, 1, CALLARG_OFF, 2},
    {"callarg-negative", taint_callarg, AT_FIX_SZ(taint_callarg), 7, ARG1_ZERO,
     0, 0, 8, 1, 7, 1, 0, 0, 0},
    {"memlen", taint_memlen, AT_FIX_SZ(taint_memlen), 4, ARG1_BUF2, 1, 0, 8, 1,
     0, 1, 1, MEMLEN_OFF, 0},
    {"memlen-negative", taint_memlen, AT_FIX_SZ(taint_memlen), 4, ARG1_BUF2, 0,
     0, 8, 1, 0, 1, 0, 0, 0},
};
#define AT_ORACLE_NMODES (sizeof(AT_ORACLE_MODES) / sizeof(AT_ORACLE_MODES[0]))

static inline const struct at_fixmode *at_find_mode(const char *name) {
    for (size_t i = 0; i < AT_ORACLE_NMODES; i++)
        if (strcmp(AT_ORACLE_MODES[i].name, name) == 0)
            return &AT_ORACLE_MODES[i];
    return NULL;
}

#endif /* ASMTEST_TAINT_ORACLE_MODES_H */
