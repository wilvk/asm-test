/*
 * probe_capture.cpp — the Intel Pin PROBE-MODE argument/return capture tool
 * (PIN-3, pin-probe-mode-capture.md).
 *
 * In probe mode Pin splices a jump ("probe") at the target routine's first bytes
 * into a small analysis trampoline; the application's own code runs NATIVELY between
 * probes, with no software code cache. This tool probes a named function, records the
 * SysV integer/FP ARGUMENT registers at entry and the RETURN register(s) + flags at
 * exit as at_val_rec_t records into a POSIX shm channel (include/asmtest_valtrace_shm.h),
 * and — for a pointer argument — captures up to a cap of the pointed-to buffer, never
 * faulting (T4). A function too short / non-relocatable to probe is reported as an
 * explicit per-target skip with a reason (T5). An out-of-process validator
 * (examples/pin_probe_validator.c) then diffs the capture against the ptrace stepper.
 *
 * PinCRT: the kit compiles tools with -DPIN_CRT=1 -nostdlib -fno-exceptions -fno-rtti,
 * so this sticks to the PinCRT subset — plain POSIX open/ftruncate/mmap, no iostream.
 * Filling at_val_rec_t needs no Capstone: the reg ids are Capstone x86_reg values as
 * LITERALS (the CS_* table below), so the trace shares the ptrace producer's id space
 * and compares field-for-field. Single-threaded fixture: no locks.
 */
#include "pin.H"

#include "asmtest_valtrace_shm.h" /* -I../include reaches include/ (makefile.rules) */

#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

/* Capstone x86_reg enum ids as LITERALS (verified against Capstone 5.0.1 x86.h; the
 * PinCRT image links no Capstone). Matches the ptrace producer's id space. */
enum {
    CS_RAX = 35,
    CS_EFLAGS = 25,
    CS_RCX = 38,
    CS_RDI = 39,
    CS_RDX = 40,
    CS_RSI = 43,
    CS_R8 = 106,
    CS_R9 = 107,
    CS_XMM0 = 122,
    CS_XMM1 = 123,
};

static KNOB<std::string> KnobFunc(KNOB_MODE_WRITEONCE, "pintool", "func",
                                  "capref", "target routine to probe");
static KNOB<std::string> KnobShm(KNOB_MODE_WRITEONCE, "pintool", "shm",
                                 AV_SHM_NAME, "POSIX shm channel name");
static KNOB<UINT32> KnobPtrCap(KNOB_MODE_WRITEONCE, "pintool", "ptrcap", "4096",
                               "pointed-to buffer read cap (bytes)");

static av_shm_channel_t *g_chan =
    0; /* created + mapped in main()             */
static uint32_t g_ptrcap = AV_PTRCAP_DEFAULT; /* -ptrcap, set in main()   */

static void say(const char *msg) {
    size_t n = 0;
    while (msg[n])
        n++;
    ssize_t w = write(2, msg, n);
    (void)w;
}

/* Record a terminal per-target skip with its reason and release the validator: no
 * probe fired, so the exit handshake happens here instead (T5). */
static void report_skip(av_skip_reason_t why) {
    g_chan->skip = (uint32_t)why;
    __atomic_store_n(&g_chan->done, 1u, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------ */
/* Record helpers — the channel is zeroed at creation, so each writer  */
/* sets only its non-zero fields (append-only into a zeroed array).    */
/* ------------------------------------------------------------------ */
static void put_reg(uint32_t reg, uint64_t val, bool is_write) {
    if (g_chan->recs_len >= AV_SHM_RECS_CAP) {
        g_chan->truncated = 1;
        return;
    }
    at_val_rec_t *r = &g_chan->recs[g_chan->recs_len++];
    r->kind = AT_LOC_REG;
    r->reg = reg;
    r->size = 8;
    r->is_write = is_write;
    r->value_valid = true;
    r->value = val;
}

static int is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}
static uint64_t hex_val(char c) {
    if (c >= '0' && c <= '9')
        return (uint64_t)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (uint64_t)(c - 'a' + 10);
    return (uint64_t)(c - 'A' + 10);
}

/* Is `ptr` inside a READABLE mapping of this address space? In probe mode Pin runs
 * the tool IN the application's address space and PIN_SafeCopy is JIT-mode-only (the
 * kit marks it "Mode: JIT"), so the design note's "validate against the target's
 * mapped ranges" is done directly against /proc/self/maps — which here IS the
 * target's own map. On a hit returns 1 and sets *end to the mapping's end (so the
 * read clamps to it and can never over-read). Fail-closed: any parse/read hiccup
 * returns 0 (refuse), never a guess. */
static int readable_extent(uint64_t ptr, uint64_t *end) {
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0)
        return 0;
    static char buf[1 << 18]; /* 256 KiB: ample for a fixture's map table */
    size_t total = 0;
    ssize_t n;
    while (total < sizeof(buf) &&
           (n = read(fd, buf + total, sizeof(buf) - total)) > 0)
        total += (size_t)n;
    close(fd);
    size_t i = 0;
    while (i < total) {
        uint64_t start = 0, e = 0;
        int any = 0;
        while (i < total && is_hex(buf[i])) {
            start = start * 16 + hex_val(buf[i]);
            i++;
            any = 1;
        }
        if (any && i < total && buf[i] == '-') {
            i++;
            while (i < total && is_hex(buf[i])) {
                e = e * 16 + hex_val(buf[i]);
                i++;
            }
            while (i < total && buf[i] == ' ')
                i++;
            char perm_r = (i < total) ? buf[i] : '-';
            if (perm_r == 'r' && ptr >= start && ptr < e) {
                *end = e;
                return 1;
            }
        }
        while (i < total && buf[i] != '\n')
            i++;
        if (i < total)
            i++;
    }
    return 0;
}

/* Capture the buffer a pointer argument points at (T4): up to g_ptrcap bytes, clamped
 * to BOTH the end of the containing page AND the end of the validated readable
 * mapping, so neither a valid-but-short mapping nor a bad pointer can over-read or
 * fault. A pointer in no readable mapping is REFUSED — a zero-length MEM record, not a
 * dereference. AT_LOC_MEM_ABS, addr = the pointer value; bytes spill to wide[]. */
static void put_buffer(uint64_t ptr) {
    if (g_chan->recs_len >= AV_SHM_RECS_CAP) {
        g_chan->truncated = 1;
        return;
    }
    uint32_t off = g_chan->wide_len;
    uint32_t got = 0;
    uint64_t map_end = 0;
    if (readable_extent(ptr, &map_end)) {
        const uint64_t page = 4096;
        uint64_t page_end = (ptr / page + 1) * page;
        uint64_t avail = page_end - ptr;
        if (map_end - ptr < avail)
            avail = map_end - ptr;
        uint32_t want = g_ptrcap;
        if ((uint64_t)want > avail)
            want = (uint32_t)avail;
        if ((uint64_t)off + want <= AV_SHM_WIDE_CAP) {
            const uint8_t *src = (const uint8_t *)ptr;
            for (uint32_t k = 0; k < want; k++)
                g_chan->wide[off + k] = src[k];
            got = want;
            g_chan->wide_len = off + got;
        } else {
            g_chan->truncated = 1;
        }
    }
    at_val_rec_t *r = &g_chan->recs[g_chan->recs_len++];
    r->kind = AT_LOC_MEM_ABS;
    r->addr = ptr;
    r->size = (uint16_t)got;
    r->is_write = false;
    r->value_valid = (got > 0);
    r->wide = (got > 0);
    r->wide_off = off;
}

/* ------------------------------------------------------------------ */
/* Probe analysis routines                                             */
/* ------------------------------------------------------------------ */

/* Entry: the six SysV integer arg regs by value, plus the first FP argument.
 *
 * FP note: probe mode's IARG_CONTEXT carries the GPRs + flags but NOT the SSE
 * state (empirically PIN_GetContextRegval(REG_XMM0) reads back zero under -probe,
 * while REG_GFLAGS reads correctly), so the FP argument is captured via the ABI /
 * PROTOTYPE with IARG_FUNCARG_ENTRYPOINT_VALUE (index 2, the `double d`) rather
 * than from the context — reliable and exact. Recorded as an XMM0 record holding
 * the 8-byte double bit pattern inline. is_write = false (reads at the boundary). */
static VOID OnEntry(ADDRINT rdi, ADDRINT rsi, ADDRINT rdx, ADDRINT rcx,
                    ADDRINT r8, ADDRINT r9, double d) {
    if (g_chan == 0)
        return;
    put_reg(CS_RDI, (uint64_t)rdi, false);
    put_reg(CS_RSI, (uint64_t)rsi, false);
    put_reg(CS_RDX, (uint64_t)rdx, false);
    put_reg(CS_RCX, (uint64_t)rcx, false);
    put_reg(CS_R8, (uint64_t)r8, false);
    put_reg(CS_R9, (uint64_t)r9, false);
    union {
        double d;
        uint64_t u;
    } fp;
    fp.d = d;
    put_reg(CS_XMM0, fp.u,
            false); /* the FP arg's bit pattern, inline (8 bytes) */
    /* The fixture designates RDX (the 3rd int arg, `buf`) as a pointer: capture the
     * buffer it points at, validated + never faulting (T4). */
    put_buffer((uint64_t)rdx);
}

/* Exit: RAX (+ RDX for a 128-bit return) and RFLAGS — the integer return set,
 * is_write = true. The FP return register is not captured: probe-mode IARG_CONTEXT
 * has no SSE state (see the entry note) and IARG_FUNCARG addresses ARGUMENTS only,
 * not returns — an honest Pin-probe-mode limitation. Publishes done with a release
 * store (the taint-shm handshake). */
static VOID OnExit(ADDRINT rax, ADDRINT rdx, const CONTEXT *ctxt) {
    if (g_chan == 0)
        return;
    put_reg(CS_RAX, (uint64_t)rax, true);
    put_reg(CS_RDX, (uint64_t)rdx, true);
    put_reg(CS_EFLAGS,
            (uint64_t)PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GFLAGS), true);
    g_chan->result = (int64_t)rax;
    __atomic_store_n(&g_chan->done, 1u, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------ */
/* Instrumentation (probe requests at image load)                      */
/* ------------------------------------------------------------------ */
static VOID on_img(IMG img, VOID *) {
    if (g_chan == 0 || !IMG_IsMainExecutable(img))
        return;

    const char *fname = KnobFunc.Value().c_str();
    RTN rtn = RTN_FindByName(img, fname);
    if (!RTN_Valid(rtn)) {
        LOG("asmtest_probe: routine '" + KnobFunc.Value() +
            "' not found in image\n");
        report_skip(AV_SKIP_NOT_FOUND);
        return;
    }

    /* Probe mode does NOT use RTN_Open/RTN_Close (the kit's Probes examples and
     * Tests/ifunc_tst.cpp guard those with !RunInProbeMode).
     *
     * Refusal detection (T5). Pin has no refusal reason-code API, so synthesize one.
     *
     * Probe-FLOOR gate first. A probe may be up to 14 bytes, and probing a routine
     * shorter than that relies on Pin relocating the WHOLE routine — which the Pin
     * docs warn "may destabilize the application". Empirically Pin 4.2's
     * RTN_IsSafeForProbedInsertion is PERMISSIVE (it reports even a 1-byte routine
     * safe and then relocates), so it is not a reliable "too short" signal; RTN_Size
     * IS reliable (measured: a 2-byte routine reports size 2). So gate on RTN_Size
     * and refuse a sub-floor routine with an explicit TOO_SHORT reason rather than a
     * silent, possibly-destabilizing probe — exactly the doc's "synthesize from
     * RTN_Size (too short: below the 14-byte probe floor)". */
    if (RTN_Size(rtn) < AV_PROBE_FLOOR) {
        LOG("asmtest_probe: '" + KnobFunc.Value() +
            "' below probe floor (RTN_Size=" + decstr((INT64)RTN_Size(rtn)) +
            " < " + decstr((INT64)AV_PROBE_FLOOR) + ") -> skip=TOO_SHORT\n");
        report_skip(AV_SKIP_TOO_SHORT);
        return;
    }

    /* Two pre-checks pick the insertion MODE: request in-place (PROBE_MODE_DEFAULT
     * forbids relocation) first; if refused, retry with PROBE_MODE_ALLOW_RELOCATION
     * (Pin keeps the probe in place if the first basic block is long enough, else
     * relocates the whole routine). If BOTH refuse, that is a terminal per-target
     * skip; distinguish TOO_SHORT from NOT_RELOCATABLE via RTN_Size. The AUTHORITATIVE
     * gate is still the BOOL return of RTN_InsertCallProbedEx (Pin 4.2 returns BOOL —
     * a FALSE post-check is definitive), kept because a pre-check "does not
     * guarantee" safety. */
    PROBE_MODE mode = PROBE_MODE_DEFAULT;
    if (!RTN_IsSafeForProbedInsertion(rtn)) {
        if (RTN_IsSafeForProbedInsertionEx(rtn, PROBE_MODE_ALLOW_RELOCATION)) {
            mode = PROBE_MODE_ALLOW_RELOCATION;
        } else {
            av_skip_reason_t why = (RTN_Size(rtn) < 14)
                                       ? AV_SKIP_TOO_SHORT
                                       : AV_SKIP_NOT_RELOCATABLE;
            LOG("asmtest_probe: '" + KnobFunc.Value() +
                "' unprobeable (RTN_Size=" + decstr((INT64)RTN_Size(rtn)) +
                ", neither in-place nor relocatable) -> skip=" +
                decstr((INT64)why) + "\n");
            report_skip(why);
            return;
        }
    }

    /* IPOINT_AFTER needs the routine PROTOTYPE so Pin can place the return probe;
     * insert AFTER before BEFORE (the kit's before_after_fastcall.cpp order). */
    PROTO proto =
        PROTO_Allocate(PIN_PARG(long), CALLINGSTD_DEFAULT, fname,
                       PIN_PARG(long), PIN_PARG(long), PIN_PARG(double),
                       PIN_PARG(const char *), PIN_PARG_END());

    BOOL ok_exit = RTN_InsertCallProbedEx(
        rtn, IPOINT_AFTER, mode, (AFUNPTR)OnExit, IARG_PROTOTYPE, proto,
        IARG_REG_VALUE, LEVEL_BASE::REG_GAX, IARG_REG_VALUE,
        LEVEL_BASE::REG_GDX, IARG_CONTEXT, IARG_END);
    BOOL ok_entry = RTN_InsertCallProbedEx(
        rtn, IPOINT_BEFORE, mode, (AFUNPTR)OnEntry, IARG_REG_VALUE,
        LEVEL_BASE::REG_RDI, IARG_REG_VALUE, LEVEL_BASE::REG_RSI,
        IARG_REG_VALUE, LEVEL_BASE::REG_RDX, IARG_REG_VALUE,
        LEVEL_BASE::REG_RCX, IARG_REG_VALUE, LEVEL_BASE::REG_R8, IARG_REG_VALUE,
        LEVEL_BASE::REG_R9, IARG_PROTOTYPE, proto,
        IARG_FUNCARG_ENTRYPOINT_VALUE, 2, IARG_END);

    PROTO_Free(proto);

    if (!ok_entry || !ok_exit) {
        av_skip_reason_t why =
            (RTN_Size(rtn) < 14) ? AV_SKIP_TOO_SHORT : AV_SKIP_NOT_RELOCATABLE;
        LOG("asmtest_probe: '" + KnobFunc.Value() +
            "' insertion refused despite pre-check -> skip=" +
            decstr((INT64)why) + "\n");
        report_skip(why);
        return;
    }
    say("asmtest_probe: probes installed\n");
}

/* ------------------------------------------------------------------ */
/* Startup: create + map the shm channel, then run probed              */
/* ------------------------------------------------------------------ */

/* Create/size/zero the channel at /dev/shm/<name> with plain open+ftruncate+mmap
 * (no librt, no shm_open — mirrors the pin-xed tool's librt avoidance). Done in
 * main() BEFORE the app starts, so the IMG-load skip path and OnEntry both find a
 * mapped, magic-stamped channel. Returns 1 on success. */
static int map_channel() {
    char path[128];
    const char *pfx = "/dev/shm";
    const char *nm = KnobShm.Value().c_str();
    size_t i = 0;
    for (const char *p = pfx; *p && i + 1 < sizeof(path); p++)
        path[i++] = *p;
    for (const char *p = nm; *p && i + 1 < sizeof(path); p++)
        path[i++] = *p;
    path[i] = 0;

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        say("asmtest_probe: cannot create shm channel\n");
        return 0;
    }
    if (ftruncate(fd, (off_t)sizeof(av_shm_channel_t)) != 0) {
        say("asmtest_probe: ftruncate shm failed\n");
        close(fd);
        return 0;
    }
    void *m = mmap(0, sizeof(av_shm_channel_t), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        say("asmtest_probe: mmap shm failed\n");
        return 0;
    }
    av_shm_channel_t *c = (av_shm_channel_t *)m;
    uint8_t *raw = (uint8_t *)m;
    for (size_t k = 0; k < sizeof(av_shm_channel_t); k++)
        raw[k] = 0;
    c->magic = AV_SHM_MAGIC;
    const char *fn = KnobFunc.Value().c_str();
    size_t j = 0;
    for (; fn[j] && j + 1 < sizeof(c->func); j++)
        c->func[j] = fn[j];
    c->func[j] = 0;
    g_chan = c;
    return 1;
}

int main(int argc, char *argv[]) {
    PIN_InitSymbols(); /* RTN_FindByName needs the symbol tables */
    if (PIN_Init(argc, argv))
        return 1;
    say("asmtest_probe: loaded\n");
    g_ptrcap = KnobPtrCap.Value();
    if (!map_channel())
        return 2;
    IMG_AddInstrumentFunction(on_img, 0);
    PIN_StartProgramProbed(); /* probe mode; never returns */
    return 0;
}
