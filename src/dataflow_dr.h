/*
 * dataflow_dr.h — the capture ABI shared by the DynamoRIO L0 VALUE producer's two
 * halves: the app-side driver (src/dataflow_dr.c) and the in-process value-capture
 * DynamoRIO client (src/dataflow_dr_client.c). See docs/internal/plans/
 * data-flow-tracing-plan.md, Phase 5 (increment 1).
 *
 * The producer captures, per instrumented instruction, a register-file SNAPSHOT
 * (the app machine context at the clean call, read via dr_get_mcontext) plus the
 * effective address + loaded value of each explicit memory SOURCE operand (via the
 * client's own decode + opnd_compute_address + dr_safe_read). The app side then
 * REPLAYS those snapshots through the SAME operand enumerator (asmtest_operands)
 * and effective-address math the scoped ptrace producer uses (src/dataflow_ptrace.c),
 * so the shared L1 def-use + L2 slicer (src/dataflow.c) build an identical graph on
 * the whole-process in-band capture — the cross-validation against the emulator L0
 * oracle rests on that shared record-building path.
 *
 * This header carries ONLY the capture buffer layout (the app allocates it; the
 * client fills it in place, mirroring the append/truncate discipline of
 * asmtest_trace_t). It is deliberately dependency-free — <stdint.h> / <stddef.h>
 * only, and no <stdbool.h> (whose `bool` clashes with DynamoRIO's own `bool`) — so
 * the client can include it directly alongside dr_api.h, unlike asmtest_valtrace.h.
 */
#ifndef ASMTEST_DATAFLOW_DR_H
#define ASMTEST_DATAFLOW_DR_H

#include <stddef.h>
#include <stdint.h>

/* Exported app-side marker the client resolves by PC (dr_get_proc_address) and
 * wraps: reading its SysV argument registers at entry (rdi=base, rsi=len,
 * rdx=at_drval_t*) tells the client which range to instrument and where to append
 * the capture. Mirrors drtrace_app.c's asmtest_dr_register_region_marker pattern. */
#define AT_DRVAL_MARKER_SYM "asmtest_dr_valcapture_marker"

/* GP register-file snapshot order — the index into at_vstep_t.gpr[]. The client
 * fills it with reg_get_value(); the app side maps a Capstone reg id to the same
 * index (at_drval_gp_index) to read a value back. */
enum {
    AT_GPR_RAX = 0,
    AT_GPR_RBX,
    AT_GPR_RCX,
    AT_GPR_RDX,
    AT_GPR_RSI,
    AT_GPR_RDI,
    AT_GPR_RBP,
    AT_GPR_RSP,
    AT_GPR_R8,
    AT_GPR_R9,
    AT_GPR_R10,
    AT_GPR_R11,
    AT_GPR_R12,
    AT_GPR_R13,
    AT_GPR_R14,
    AT_GPR_R15,
    AT_GPR_COUNT
};

/* One captured memory SOURCE (load) reference: its resolved effective address, the
 * access width, and up to 8 bytes of the loaded value read at the clean call (the
 * pre-instruction memory state — a load's datum is in memory BEFORE it executes).
 * Store values are NOT captured here (their datum only exists post-instruction); a
 * store's location still enters def-use, resolved app-side from the snapshot. */
typedef struct at_vmem {
    uint64_t ea;
    uint64_t value; /* low min(size, 8) bytes, zero-extended */
    uint16_t size;
    uint8_t valid; /* value populated (dr_safe_read succeeded) */
    uint8_t pad[5];
} at_vmem_t;

/* One captured step: the instruction's region offset, the GP register file at the
 * step (pre-instruction / source state), and the [mem_first, mem_first+mem_n) slice
 * of the shared memory pool holding this step's load references. */
typedef struct at_vstep {
    uint64_t off;
    uint64_t gpr[AT_GPR_COUNT];
    uint64_t rip;
    uint64_t rflags;
    uint32_t mem_first;
    uint32_t mem_n;
} at_vstep_t;

/* The app-owned capture buffer the client fills in place. steps[] and mem[] are
 * caller-allocated with fixed caps; the client appends and flips `truncated` on
 * overflow (the honest-overflow contract of asmtest_trace_t / asmtest_valtrace_t),
 * keeping the *_total counters advancing past the cap. */
typedef struct at_drval {
    at_vstep_t *steps;
    size_t steps_cap;
    size_t steps_len;
    uint64_t steps_total;
    at_vmem_t *mem;
    size_t mem_cap;
    size_t mem_len;
    uint8_t truncated;
} at_drval_t;

#endif /* ASMTEST_DATAFLOW_DR_H */
