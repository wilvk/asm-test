/*
 * asmtest_valtrace_shm.h — the fixed cross-address-space value-trace channel for the
 * Intel Pin PROBE-MODE argument/return capture lane (PIN-3, pin-probe-mode-capture.md).
 *
 * The Pin probe tool (tools/pintool → pintool/probe_capture.cpp, splices a jump at the
 * target routine's entry/exit and runs the app natively between probes) is the PRODUCER:
 * it records the SysV integer/FP argument registers at entry and the return register(s)
 * + flags at exit as at_val_rec_t records into this channel. An OUT-OF-PROCESS validator
 * (examples/pin_probe_validator.c) is the CONSUMER: after `pin` exits it maps the same
 * POSIX shm segment, rebuilds an asmtest_valtrace_t from recs[]/wide[], and diffs the
 * capture against an independent ptrace single-step observation of the same routine.
 *
 * CROSS-PROCESS RULE (mirrors include/asmtest_taint_shm.h and pintool/pintool_shm.h):
 * the segment is mapped at DIFFERENT virtual addresses in producer and consumer, so any
 * stored POINTER would be meaningless to the reader. Everything here is read strictly by
 * OFFSET — the fixed recs[]/wide[] arrays and the scalar counters — never by a producer-
 * space pointer. at_val_rec_t is plain scalars (no embedded pointers), so it is shm-safe
 * verbatim; the validator points a fresh asmtest_valtrace_t's caller-owned arrays at
 * heap copies of recs[]/wide[] read out of the segment by offset.
 *
 * The reg ids the tool writes into at_val_rec_t.reg are Capstone x86_reg enum values as
 * LITERALS (the tool builds under PinCRT and links no Capstone), so the capture shares
 * the id space of every other value producer and compares field-for-field.
 *
 * Plain C11 / C++-safe: <stdint.h> plus asmtest_valtrace.h (for at_val_rec_t) only, so
 * the C fixtures/validator and the C++ Pintool all include it unchanged.
 */
#ifndef ASMTEST_VALTRACE_SHM_H
#define ASMTEST_VALTRACE_SHM_H

#include <stdint.h>

#include "asmtest_valtrace.h" /* at_val_rec_t (POD; shm-safe verbatim) */

/* Default POSIX shm name (leading slash, no other slashes). The tool and validator
 * both take an override (-shm knob / argv); this is the fallback both default to. */
#define AV_SHM_NAME       "/asmtest_valtrace_pin"
#define AV_SHM_RECS_CAP   64   /* entry args + exit results, ample            */
#define AV_SHM_WIDE_CAP   8192 /* two 4 KiB pointer buffers (T4)              */
#define AV_SHM_MAGIC      0x50564C54u /* "PVLT": the tool checks the layout   */
#define AV_PTRCAP_DEFAULT 4096u /* default pointed-to-buffer read cap (T4) */

/* Why a captured target was NOT recorded — an EXPLICIT per-target skip with a reason,
 * never a silent miss (T5). AV_SKIP_NONE means the capture succeeded. */
typedef enum av_skip_reason {
    AV_SKIP_NONE = 0,      /* captured                                   */
    AV_SKIP_TOO_SHORT = 1, /* routine too short to hold a probe          */
    AV_SKIP_NOT_RELOCATABLE = 2, /* not safe in-place and not relocatable    */
    AV_SKIP_NOT_FOUND = 3, /* the named routine is not in the image      */
} av_skip_reason_t;

/* The shared channel. done transitions 0 -> 1 (release) once the exit probe has captured
 * the return (or, on a skip, once the tool has decided the target cannot be probed); the
 * validator spins on it (acquire) before draining. */
typedef struct av_shm_channel {
    uint32_t magic; /* AV_SHM_MAGIC — tool validates before writing   */
    volatile uint32_t done; /* 0 -> 1 (release) once exit captured / skipped  */
    int64_t result;         /* the fixture's return value (a liveness sanity) */
    uint32_t recs_len;      /* records actually written (<= AV_SHM_RECS_CAP)  */
    uint32_t wide_len;      /* bytes used in wide[] (<= AV_SHM_WIDE_CAP)      */
    uint32_t truncated;     /* a cap (recs[] or wide[]) overflowed            */
    uint32_t skip;          /* 0 = captured; else an av_skip_reason_t (T5)    */
    char func[64];          /* NUL-terminated: the target routine name        */
    at_val_rec_t recs[AV_SHM_RECS_CAP];
    uint8_t wide[AV_SHM_WIDE_CAP];
} av_shm_channel_t;

/* ------------------------------------------------------------------------ */
/* Shared capture-fixture contract (examples/pin_probe_victim.c ↔ validator) */
/* ------------------------------------------------------------------------ */
/* The fixture routine is
 *   long capref(long a, long b, double d, const char *buf);
 * SysV places a->RDI, b->RSI, d->XMM0, buf->RDX and returns
 *   a + b + (long)d + buf[0]
 * in RAX (buf[0] is added only for a plausibly-valid pointer, so the deliberately
 * invalid-pointer T4 sub-run cannot fault the victim). Both the Pin-run and the
 * ptrace-reference invocations call capref with THESE constants, so the two
 * independent producers observe identical process-independent values (a in RDI,
 * b in RSI, and the RAX return) — the pointer arg (RDX) and unused arg regs are
 * process-specific and are NOT cross-asserted for equality. */
#define AV_CAPREF_A   11L
#define AV_CAPREF_B   22L
#define AV_CAPREF_D   3.5
#define AV_CAPREF_BUF "asmtest-capref-buffer" /* buf[0] == 'a' == 97 */
#define AV_CAPREF_RET                                                          \
    (AV_CAPREF_A + AV_CAPREF_B + (long)AV_CAPREF_D + 'a') /* 133 */

#endif /* ASMTEST_VALTRACE_SHM_H */
