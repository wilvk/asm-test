/*
 * asmtest_pintool.cpp — the Intel Pin tool for the XED-decoded trace tier (PIN-2).
 *
 * Resolves the launched workload's exported asmtest_trace_begin / _end markers by
 * symbol (RTN_FindByName — the Pin analog of the DynamoRIO client's
 * dr_get_proc_address), gates recording on them, instruments every instruction in
 * the registered region, and appends offsets to the POSIX shm channel with the
 * repo's asmtest_trace_t append / dedup / truncated discipline. An out-of-process
 * validator (examples/pin_trace_validator.c) then diffs the result against the
 * in-process single-step and DynamoRIO backends — Pin ≡ DR ≡ single-step.
 *
 * PinCRT: the kit compiles tools with -DPIN_CRT=1 -nostdlib -fno-exceptions
 * -fno-rtti, so this sticks to the PinCRT subset — plain POSIX open/mmap, no
 * libstdc++ iostream. Single-threaded (the fixture is single-threaded): no locks.
 */
#include "pin.H"

#include "pintool_shm.h"

#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

static KNOB<std::string> KnobShm(KNOB_MODE_WRITEONCE, "pintool", "shm",
                                 PIN_SHM_NAME, "POSIX shm channel name");

/* ------------------------------------------------------------------ */
/* Tool state (single-threaded fixture — no locks, v1)                 */
/* ------------------------------------------------------------------ */
static asmtest_pin_channel_t *g_chan = 0; /* mapped lazily in on_begin     */
static ADDRINT g_region_base = 0;
static ADDRINT g_region_len = 0;
static int g_active = 0; /* 1 between begin/end: record region insns       */

/* Sequential-predecessor state for the block-head rule; reset each on_begin. */
static int g_have_prev = 0;
static ADDRINT g_prev_off = 0;
static UINT32 g_prev_size = 0;
static int g_prev_was_cf = 0;

static void say(const char *msg) {
    size_t n = 0;
    while (msg[n])
        n++;
    ssize_t w = write(2, msg, n);
    (void)w;
}

/* ------------------------------------------------------------------ */
/* Marker analysis routines                                            */
/* ------------------------------------------------------------------ */

/* Lazily map the shm channel the first time recording is enabled: the fixture
 * publishes region_base/region_len BEFORE the first begin and only executes the
 * region between markers, so by the time any region code is JITted the base/len
 * are already in the channel (see the workload's ORDERING comment). */
static VOID on_begin() {
    if (g_chan == 0) {
        /* Build "/dev/shm" + KnobShm.Value() (the name starts with '/'). Plain
         * open+mmap, not shm_open, to avoid dragging librt into the PinCRT link. */
        char path[128];
        const char *pfx = "/dev/shm";
        const char *nm = KnobShm.Value().c_str();
        size_t i = 0;
        for (const char *p = pfx; *p && i + 1 < sizeof(path); p++)
            path[i++] = *p;
        for (const char *p = nm; *p && i + 1 < sizeof(path); p++)
            path[i++] = *p;
        path[i] = 0;
        int fd = open(path, O_RDWR);
        if (fd < 0) {
            say("asmtest_pintool: cannot open shm channel\n");
            return;
        }
        void *m = mmap(0, sizeof(asmtest_pin_channel_t), PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
        close(fd);
        if (m == MAP_FAILED) {
            say("asmtest_pintool: mmap shm failed\n");
            return;
        }
        asmtest_pin_channel_t *c = (asmtest_pin_channel_t *)m;
        if (c->magic != PIN_SHM_MAGIC) {
            say("asmtest_pintool: shm magic mismatch\n");
            munmap(m, sizeof(asmtest_pin_channel_t));
            return;
        }
        g_chan = c;
        g_region_base = (ADDRINT)c->region_base;
        g_region_len = (ADDRINT)c->region_len;
    }
    g_active = 1;
    g_have_prev = 0; /* reset the sequential-predecessor state */
}

static VOID on_end() { g_active = 0; }

/* ------------------------------------------------------------------ */
/* Per-instruction analysis (hot path)                                 */
/* ------------------------------------------------------------------ */
static VOID on_insn(ADDRINT off, BOOL is_cf, UINT32 size) {
    if (!g_active || g_chan == 0)
        return;
    asmtest_pin_channel_t *c = g_chan;

    /* Block-head rule — derived at runtime (NOT Pin's BBL partition, which splits
     * at trace boundaries that need not match the repo's block model). A head iff
     * (a) first in-region insn since begin, (b) the previous in-region insn was
     * control-flow, or (c) non-sequential entry (branch target / call-return
     * re-entry). Reproduces include/asmtest_trace.h's partition: a block ends after
     * every branch-class instruction, so a NOT-taken fall-through starts a block. */
    int is_head =
        (!g_have_prev) || g_prev_was_cf || (off != g_prev_off + g_prev_size);

    /* insns[]: record in order (truncate on overflow), always bump total. */
    if (c->insns_len < PIN_SHM_INSNS_CAP)
        c->insns[c->insns_len++] = (uint64_t)off;
    else
        c->truncated = 1;
    c->insns_total++;

    if (is_head) {
        /* blocks[]: linear-scan dedup, record if new (truncate on overflow),
         * always bump total (mirrors trace_append_block). */
        int seen = 0;
        for (uint64_t k = 0; k < c->blocks_len; k++)
            if (c->blocks[k] == (uint64_t)off) {
                seen = 1;
                break;
            }
        if (!seen) {
            if (c->blocks_len < PIN_SHM_BLOCKS_CAP)
                c->blocks[c->blocks_len++] = (uint64_t)off;
            else
                c->truncated = 1;
        }
        c->blocks_total++;
    }

    g_have_prev = 1;
    g_prev_off = off;
    g_prev_size = size;
    g_prev_was_cf = is_cf ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Instrumentation callbacks                                           */
/* ------------------------------------------------------------------ */

/* Resolve the exported markers in the main executable and gate on them. */
static VOID on_img(IMG img, VOID *) {
    if (!IMG_IsMainExecutable(img))
        return;
    RTN b = RTN_FindByName(img, "asmtest_trace_begin");
    if (RTN_Valid(b)) {
        RTN_Open(b);
        RTN_InsertCall(b, IPOINT_BEFORE, (AFUNPTR)on_begin, IARG_END);
        RTN_Close(b);
    }
    RTN e = RTN_FindByName(img, "asmtest_trace_end");
    if (RTN_Valid(e)) {
        RTN_Open(e);
        RTN_InsertCall(e, IPOINT_BEFORE, (AFUNPTR)on_end, IARG_END);
        RTN_Close(e);
    }
}

/* Instrument every in-region instruction. The channel is mapped only after
 * on_begin ran; region code cannot be JITted before that (the fixture executes the
 * region only between markers). If that assumption is ever observed violated,
 * PIN_RemoveInstrumentation() after on_begin would force re-JIT of already-seen
 * traces — not needed for this single-region fixture. */
static VOID on_trace(TRACE trace, VOID *) {
    if (g_chan == 0)
        return;
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
        for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
            ADDRINT a = INS_Address(ins);
            if (a < g_region_base || a >= g_region_base + g_region_len)
                continue;
            ADDRINT off = a - g_region_base;
            /* Branch-class = the repo's block-terminator set (JUMP/CALL/RET),
             * matching asmtest_disas_is_branch so the partition is byte-identical
             * to the single-step / DR backends. */
            BOOL is_cf = INS_IsBranch(ins) || INS_IsCall(ins) || INS_IsRet(ins);
            INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)on_insn, IARG_ADDRINT,
                           off, IARG_BOOL, is_cf, IARG_UINT32, INS_Size(ins),
                           IARG_END);
        }
    }
}

int main(int argc, char *argv[]) {
    PIN_InitSymbols(); /* RTN_FindByName needs the symbol tables */
    if (PIN_Init(argc, argv))
        return 1;
    say("asmtest_pintool: loaded\n");
    IMG_AddInstrumentFunction(on_img, 0);
    TRACE_AddInstrumentFunction(on_trace, 0);
    PIN_StartProgram(); /* never returns */
    return 0;
}
