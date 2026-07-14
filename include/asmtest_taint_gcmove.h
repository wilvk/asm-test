/*
 * asmtest_taint_gcmove.h — the cross-.so handshake for the taint tier's LIVE GC-move path
 * (dynamorio-taint-tier-plan.md, Increment 7). Under `drrun -c <taint client>.so gcmove --
 * dotnet <app>` with an in-process ICorProfilerCallback4 profiler attached
 * (CORECLR_PROFILER/CORECLR_PROFILER_PATH), TWO .so's live in the same process: the DR taint
 * client (holds the byte-granular tag shadow + at_gc_remap) and the profiler (gets the
 * compacting-GC {old,new,len} object-move ranges from MovedReferences2). The profiler must feed
 * those ranges to at_gc_remap at the GC fence so tags follow moved objects.
 *
 * The two .so's are in the SAME address space, so a function pointer is valid across them — the
 * only problem is DISCOVERY (the DR client is loaded by DR's private loader, so dlsym cannot see
 * its symbols). This tiny POSIX-shm segment is the discovery channel: the client publishes the
 * address of its gc_move entry (`gc_move`) + a ready `magic`; the profiler maps the same segment
 * by name, spins on `magic`, reads `gc_move`, and calls it per moved range. `moves` is a
 * liveness/diagnostic counter the client bumps on each remap (drained by the test lane).
 */
#ifndef ASMTEST_TAINT_GCMOVE_H
#define ASMTEST_TAINT_GCMOVE_H

#include <stdint.h>

#define AT_GCMOVE_SHM_NAME "/asmtest_taint_gcmove"
#define AT_GCMOVE_MAGIC    0x47434D56u /* "GCMV" — set once gc_move is published */

typedef struct at_gcmove_channel {
    volatile uint32_t
        magic; /* AT_GCMOVE_MAGIC once `gc_move` is valid                   */
    volatile uint32_t pad;
    volatile uint64_t
        gc_move; /* fn ptr: void (*)(uint64_t old, uint64_t nw, uint64_t len) */
    volatile uint64_t
        moves; /* # of moved ranges remapped (client bumps; diag/liveness)  */
} at_gcmove_channel_t;

#endif /* ASMTEST_TAINT_GCMOVE_H */
