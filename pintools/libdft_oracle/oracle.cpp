/*
 * oracle.cpp — the libdft64 DIFFERENTIAL-ORACLE Pintool (pin-libdft-taint-oracle.md,
 * track PIN-4). Running under Intel Pin with libdft64 linked, it independently
 * reproduces the DynamoRIO taint client's SINK set for the shared seed/sink fixtures,
 * so a separate validator can diff the two engines byte-for-byte.
 *
 * It (a) SEEDS [base,len) in libdft's tagmap when the app calls the seed marker
 * (AT_TAINT_SEED_SYM), and (b) at each watched SINK site inside the fixture region,
 * appends one at_taint_hit_t iff the watched register's libdft tag is tainted — in the
 * exact {off, ea, tag, kind} shape the DR client produces
 * (src/dataflow_dr_client_inlined.c). Three sink kinds, mirroring the DR client:
 *   - kind 1 (branch condition): libdft IGNORES eflags, so instead of the flag we watch
 *     the GP register that DEFINED the flag (the branch's source operand) — the sanctioned
 *     kind-1 approximation. A pure eflags-only divergence is the libdft-gap-eflags class.
 *   - kind 2 (call arg): the first SysV arg register rdi at each in-region `call`.
 *   - kind 0 (mem-len): the rep-movs count register rcx at each in-region `rep movs`.
 * The sink guard is inserted BEFORE libdft's own propagation for the same instruction, so
 * it observes the reg-tag file as left by the PRIOR instruction (matching the DR client's
 * "guard placed first" discipline).
 *
 * The hits ride a POSIX shm channel (include/asmtest_taint_oracle_shm.h). The tool
 * ATTACHES (never creates) the channel lazily — robust to the tool's main() running
 * before the app creates it — and, mirroring the pin-xed tools, opens /dev/shm/<name>
 * with plain open()+mmap() (no shm_open, to avoid dragging librt into the PinCRT link).
 *
 * PinCRT: the kit compiles tools with -DPIN_CRT=1 -nostdlib -fno-exceptions -fno-rtti, so
 * this sticks to the PinCRT subset — plain POSIX open/mmap, no iostream. libdft is
 * TEST/ORACLE-ONLY: this tool is never linked into a shipped package.
 */
#include "pin.H"

#include "libdft_api.h" /* libdft_init */
#include "tagmap.h" /* tagmap_setb, tagmap_getn_reg, tag_alloc, tag_is_empty */

#include "ins_helper.h" /* REG_INDX, DFT_REG_* */

#include "asmtest_taint.h" /* at_tag_t, AT_TAG_TAINTED, AT_TAINT_*_SYM   */
#include "asmtest_taint_oracle_shm.h" /* at_oracle_shm_t, AT_ORACLE_* (via -I)      */

#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static KNOB<std::string>
    KnobShm(KNOB_MODE_WRITEONCE, "pintool", "shm", AT_ORACLE_SHM_NAME,
            "POSIX shm channel name for the oracle sink report");

static at_oracle_shm_t *g_shm = 0; /* attached lazily; 0 until mapped */

static void say(const char *msg) {
    size_t n = 0;
    while (msg[n])
        n++;
    ssize_t w = write(2, msg, n);
    (void)w;
}

/* Attach (never create) the channel at /dev/shm/<name> with plain open+mmap — the app or
 * the diff orchestrator creates + zeroes it. Robust to being called before the segment
 * exists: returns 0 and retries on a later callback. */
static at_oracle_shm_t *ensure_shm() {
    if (g_shm)
        return g_shm;
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
    if (fd < 0)
        return 0; /* not created yet */
    /* Only map once the creator has ftruncate'd it to the full size — mapping an
     * under-sized segment and touching it would SIGBUS (a create-then-ftruncate race the
     * standalone pin_taint path exposes; the diff orchestrator sizes it before forking). */
    struct stat stbuf;
    if (fstat(fd, &stbuf) != 0 ||
        (size_t)stbuf.st_size < sizeof(at_oracle_shm_t)) {
        close(fd);
        return 0; /* not sized yet; retry on a later callback */
    }
    void *m = mmap(0, sizeof(at_oracle_shm_t), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        return 0;
    g_shm = (at_oracle_shm_t *)m;
    return g_shm;
}

/* ---- seed: paint [base, base+len) in libdft's tagmap ---------------------- */
static VOID on_seed(ADDRINT base, ADDRINT len, ADDRINT color) {
    (void)
        color; /* the fixtures seed one color (bit0 tainted); libdft is tainted/clean */
    for (ADDRINT k = 0; k < len; k++) {
        /* nonzero offset => non-empty tag under BOTH the BDD and uint8 tag types
         * (tag_alloc<uint8_t>(off) == off>0). */
        tag_t t = tag_alloc<tag_t>((unsigned int)(k + 1));
        tagmap_setb(base + k, t);
    }
    ensure_shm();
}

/* The DR client registers the app-owned report here; this tool writes to shm instead. */
static VOID on_sink_register(ADDRINT report_ptr) {
    (void)report_ptr;
    ensure_shm();
}

/* ---- sink: append one hit iff the watched register tag is tainted --------- */
/* first_iter gates rep-string sinks to ONE hit: Pin calls IPOINT_BEFORE on a REP
 * instruction ONCE PER ITERATION, but the DR client fires its rep-movs guard once, so we
 * fire only on the first rep iteration (IARG_FIRST_REP_ITERATION). Non-rep sinks pass 1. */
static VOID append_hit(THREADID tid, ADDRINT off, UINT32 dft_reg, UINT32 kind,
                       UINT32 first_iter) {
    if (!first_iter)
        return;
    at_oracle_shm_t *s = ensure_shm();
    if (!s)
        return;
    tag_t t = tagmap_getn_reg(tid, dft_reg, 8); /* 8 bytes of the GP reg */
    if (tag_is_empty(t))
        return;
    /* Append with the same append/truncate discipline as at_taint_report_t. */
    uint64_t idx = s->report.hits_total;
    s->report.hits_total = idx + 1;
    if (idx < AT_ORACLE_HITS_CAP) {
        at_taint_hit_t *h = &s->hits[idx];
        h->off = off;
        h->ea = 0; /* register/flag sink */
        h->seed_off = 0;
        h->tag = AT_TAG_TAINTED;
        h->kind = (uint8_t)kind;
        h->pad[0] = 0;
        h->pad[1] = 0;
        h->depth = 0;
        s->report.hits_len = idx + 1;
    } else {
        s->report.truncated = 1;
    }
}

/* ---- flag-def tracking + sink-shape predicates --------------------------- */
static bool ins_writes_flags(INS ins) {
    UINT32 n = INS_MaxNumWRegs(ins);
    for (UINT32 i = 0; i < n; i++)
        if (REG_FullRegName(INS_RegW(ins, i)) == REG_RFLAGS)
            return true;
    return false;
}
static REG ins_first_gp_wreg(INS ins) {
    UINT32 n = INS_MaxNumWRegs(ins);
    for (UINT32 i = 0; i < n; i++) {
        REG r = REG_FullRegName(INS_RegW(ins, i));
        if (REG_is_gr(r))
            return r;
    }
    return REG_INVALID();
}
static bool is_cbr(
    INS ins) { /* conditional branch: has a taken target AND a fall-through */
    return INS_IsBranch(ins) && INS_HasFallThrough(ins);
}
static bool
is_rep_movs(INS ins) { /* rep movs: read+write memory under a real rep prefix */
    return INS_HasRealRep(ins) && INS_IsMemoryRead(ins) &&
           INS_IsMemoryWrite(ins);
}

/* ---- trace instrumentation: arm the sinks inside the fixture region ------- */
static VOID trace_instrument(TRACE trace, VOID *v) {
    at_oracle_shm_t *s = ensure_shm();
    if (!s)
        return;
    ADDRINT rb = s->region_base, rl = s->region_len;
    if (rb == 0 || rl == 0)
        return; /* fixture region not registered yet (pre-fixture traces) */
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
        REG last_flagdef = REG_INVALID();
        for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
            ADDRINT ia = INS_Address(ins);
            if (ia >= rb && ia < rb + rl) {
                if (is_cbr(ins)) {
                    if (last_flagdef != REG_INVALID()) {
                        UINT32 dft = (UINT32)REG_INDX(last_flagdef);
                        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)append_hit,
                                       IARG_THREAD_ID, IARG_ADDRINT, ia - rb,
                                       IARG_UINT32, dft, IARG_UINT32, (UINT32)1,
                                       IARG_UINT32, (UINT32)1, IARG_END);
                    }
                } else if (INS_IsCall(ins)) {
                    UINT32 dft = (UINT32)REG_INDX(REG_RDI);
                    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)append_hit,
                                   IARG_THREAD_ID, IARG_ADDRINT, ia - rb,
                                   IARG_UINT32, dft, IARG_UINT32, (UINT32)2,
                                   IARG_UINT32, (UINT32)1, IARG_END);
                } else if (is_rep_movs(ins)) {
                    UINT32 dft = (UINT32)REG_INDX(REG_RCX);
                    /* rep-string: fire ONLY on the first iteration (Pin calls
                     * IPOINT_BEFORE per rep iteration) so it matches the DR client's
                     * single rep-movs hit. */
                    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)append_hit,
                                   IARG_THREAD_ID, IARG_ADDRINT, ia - rb,
                                   IARG_UINT32, dft, IARG_UINT32, (UINT32)0,
                                   IARG_FIRST_REP_ITERATION, IARG_END);
                }
            }
            /* Update the flag-def register AFTER this instruction, so a following cbr in
             * the same BBL reads the register that most recently defined the flags. */
            if (ins_writes_flags(ins)) {
                REG d = ins_first_gp_wreg(ins);
                if (d != REG_INVALID())
                    last_flagdef = d;
            }
        }
    }
}

/* ---- resolve the seed/sink markers by name (the app is -rdynamic) --------- */
static VOID on_image(IMG img, VOID *v) {
    RTN seed = RTN_FindByName(img, AT_TAINT_SEED_SYM);
    if (RTN_Valid(seed)) {
        RTN_Open(seed);
        RTN_InsertCall(seed, IPOINT_BEFORE, (AFUNPTR)on_seed,
                       IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                       IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
                       IARG_FUNCARG_ENTRYPOINT_VALUE, 2, IARG_END);
        RTN_Close(seed);
    }
    RTN sink = RTN_FindByName(img, AT_TAINT_SINK_SYM);
    if (RTN_Valid(sink)) {
        RTN_Open(sink);
        RTN_InsertCall(sink, IPOINT_BEFORE, (AFUNPTR)on_sink_register,
                       IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_END);
        RTN_Close(sink);
    }
}

int main(int argc, char *argv[]) {
    PIN_InitSymbols();
    if (PIN_Init(argc, argv)) {
        say("oracle: PIN_Init failed (bad command line?)\n");
        return 1;
    }
    /* Register OUR sink instrumentation BEFORE libdft's propagation, so the sink guard
     * observes the reg-tag file as left by the PRIOR instruction (the DR client places
     * its guard first for the same reason). */
    TRACE_AddInstrumentFunction(trace_instrument, 0);
    if (libdft_init() != 0) {
        say("oracle: libdft_init failed\n");
        return 1;
    }
    IMG_AddInstrumentFunction(on_image, 0);
    PIN_StartProgram();
    return 0;
}
