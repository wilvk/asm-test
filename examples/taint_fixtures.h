/*
 * taint_fixtures.h — the shared GP/integer-memory seed/sink FIXTURES for the DynamoRIO
 * taint tier and its libdft64 differential oracle. A differential oracle is only
 * meaningful on BYTE-IDENTICAL inputs, so the fixture byte arrays + their region metadata
 * live here as a single source of truth, included by BOTH the DR-side harness
 * (examples/dr_taint.c) and the Pin/libdft driver (examples/pin_taint.c) and its diff
 * (examples/taint_oracle_diff.c).
 *
 * The bytes + offsets were extracted VERBATIM from dr_taint.c (Increment 4); the DR lane
 * (`make dr-taint-native-test`) is the proof that not a single fixture byte changed.
 *
 * <stdint.h>-only. static const arrays: each including TU gets its own copy (they are
 * tiny leaf routines), which is exactly the shared-input guarantee the oracle needs.
 *
 * Fixtures (leaf, x86-64 SysV — rdi=buf, rsi=b/buf2), all reading a SEEDED memory buffer
 * at step0 so taint originates in the shadow.
 */
#ifndef ASMTEST_TAINT_FIXTURES_H
#define ASMTEST_TAINT_FIXTURES_H

#include <stdint.h>

/* Propagation oracle; also the `highbyte` per-byte-union case (seeding only buf's high
 * bytes):  mov rax,[rdi] / mov [rsp-8],rax / mov rcx,[rsp-8] / lea rdx,[rcx+rsi] /
 * mov rax,rdx / ret */
static const uint8_t taint_chain[] = {
    0x48, 0x8b, 0x07,             /* 0x00 mov rax, [rdi]     (SEED origin) */
    0x48, 0x89, 0x44, 0x24, 0xf8, /* 0x03 mov [rsp-8], rax   (spill)       */
    0x48, 0x8b, 0x4c, 0x24, 0xf8, /* 0x08 mov rcx, [rsp-8]   (reload)      */
    0x48, 0x8d, 0x14, 0x31,       /* 0x0d lea rdx, [rcx+rsi]               */
    0x48, 0x89, 0xd0,             /* 0x11 mov rax, rdx                     */
    0xc3,                         /* 0x14 ret                              */
};

/* Sink fixture (rdi=buf seeded, rsi=b): derives the seeded value into a flag, then
 * branches on it — the tainted flag reaches a conditional-branch SINK (kind = 1).
 *   taint_sink_chain: mov rax,[rdi] / mov [rsp-8],rax / mov rcx,[rsp-8] /
 *                     add rcx,rsi / jz +3 / mov rax,rcx / ret
 * add taints rcx AND the flags; the jz at 0x10 reads the tainted ZF => one hit at 0x10.
 * forward(step0) = {0,1,2,3,4,5} (jz at step4, mov rax,rcx at step5; ret excluded). */
static const uint8_t taint_sink_chain[] = {
    0x48, 0x8b, 0x07,             /* 0x00 mov rax, [rdi]   (SEED origin)    */
    0x48, 0x89, 0x44, 0x24, 0xf8, /* 0x03 mov [rsp-8], rax (spill)          */
    0x48, 0x8b, 0x4c, 0x24, 0xf8, /* 0x08 mov rcx, [rsp-8] (reload)         */
    0x48, 0x01, 0xf1,             /* 0x0d add rcx, rsi     (rcx+flags taint)*/
    0x74, 0x03,                   /* 0x10 jz 0x15          (SINK: taint ZF) */
    0x48, 0x89, 0xc8,             /* 0x12 mov rax, rcx                      */
    0xc3,                         /* 0x15 ret                               */
};
#define SINK_OFF   0x10 /* the jz instruction's region offset */
#define SINK_DEPTH 4    /* def-use edges seed(step0) -> jz(step4)          */

/* Create-on-touch fixture (rdi=buf seeded, rsi=buf2 a FRESH heap buffer, never seeded
 * or otherwise touched in the shadow): stores the tainted value to buf2 then reloads it.
 * The store to buf2 hits a null leaf => the inline store-tag SLOWPATH must create the
 * leaf on touch, or the reloaded rcx would come back clean.
 *   taint_heapstore: mov rax,[rdi] / mov [rsi],rax / mov rcx,[rsi] / mov rax,rcx / ret
 * forward(step0) = {0,1,2,3} (ret excluded). */
static const uint8_t taint_heapstore[] = {
    0x48, 0x8b, 0x07, /* 0x00 mov rax, [rdi] (SEED origin)              */
    0x48, 0x89, 0x06, /* 0x03 mov [rsi], rax (store to fresh heap buf2) */
    0x48, 0x8b, 0x0e, /* 0x06 mov rcx, [rsi] (reload from buf2)         */
    0x48, 0x89, 0xc8, /* 0x09 mov rax, rcx                              */
    0xc3,             /* 0x0c ret                                       */
};

/* Call-arg sink fixture (rdi=buf seeded): moves the seeded value into rdi (SysV arg0), then
 * CALLs — a tainted value reaches a call ARGUMENT register (kind = 2). A direct call does not
 * machine-read its argument registers, so this sink is calling-convention-based (rdi watched at
 * the call site), decoupled from the value trace's machine-level def-use — hence, unlike the
 * branch/mem-len sinks, the CALL step itself is not in forward(seed); the arg's DEFINING move
 * is (asserted below). The nested call/ret is balanced (the leaf runs as a normal C function).
 *   taint_callarg: mov rax,[rdi] / mov rdi,rax / call +1 / ret / ret
 * one hit at off 0x06, kind 2; forward(step0) includes the mov rdi,rax at 0x03. */
static const uint8_t taint_callarg[] = {
    0x48, 0x8b, 0x07,             /* 0x00 mov rax, [rdi]  (SEED origin)      */
    0x48, 0x89, 0xc7,             /* 0x03 mov rdi, rax    (arg0 tainted)     */
    0xe8, 0x01, 0x00, 0x00, 0x00, /* 0x06 call 0x0c       (SINK: arg tainted)*/
    0xc3,                         /* 0x0b ret             (to harness)       */
    0xc3,                         /* 0x0c ret             (callee -> 0x0b)   */
};
#define CALLARG_OFF 0x06 /* the call instruction's region offset */

/* Mem-len sink fixture (rdi=buf seeded holding a SMALL byte count; rsi=buf2 a readable src):
 * loads the seeded value as a length into rcx (the string-copy count register), then rep movsb
 * — a tainted value reaches a memory-copy LENGTH (kind = 0). rcx IS a machine source of rep
 * movs, so def-use connects seed->rep-movs and the sink step is in forward(seed).
 *   taint_memlen: mov rcx,[rdi] / rep movsb / mov rax,rcx / ret
 * one hit at off 0x03, kind 0; forward(step0) = {0,1,2} (ret excluded), depth 1. */
static const uint8_t taint_memlen[] = {
    0x48, 0x8b, 0x0f, /* 0x00 mov rcx, [rdi] (SEED origin: tainted count)  */
    0xf3, 0xa4,       /* 0x03 rep movsb      (SINK: mem-copy length rcx)   */
    0x48, 0x89, 0xc8, /* 0x05 mov rax, rcx   (rax = 0 -> deterministic)    */
    0xc3,             /* 0x08 ret                                          */
};
#define MEMLEN_OFF   0x03 /* the rep movsb instruction's region offset      */
#define MEMLEN_DEPTH 1    /* def-use edges seed(step0) -> rep movsb(step1)  */

#endif /* ASMTEST_TAINT_FIXTURES_H */
