// gcfenceprof.cpp — the MEASURING half of the F4 GC-fence FREEZE probe.
//
// THE QUESTION (live-attach-dataflow-followup-plan.md, F4). F4 canonicalizes managed memory def-use
// across GC compactions by feeding MovedReferences2 {old,new,len} triples into
// asmtest_gcmove_canonicalize as asmtest_gcmove_t records. Each record carries a `step` — "the
// value-trace step boundary the compaction takes effect at" — which is nothing but an index into
// insn_off[], i.e. HOW MANY in-region instructions the tracer has recorded so far. The design
// assumes: during a GC fence the EE is fully suspended, so a ptrace-single-stepped managed thread
// retires ZERO instructions, the step counter is FROZEN across the compaction, and the boundary can
// therefore be read AT DRAIN TIME (after the fact) and still be exact.
//
// That assumption may be false. CoreCLR parks threads at GC-safe points, but HOW matters: a thread
// that BLOCKS (futex/wait) retires nothing and the claim holds; a thread that SPIN-WAITS at a GC
// poll retires instructions and drain-time stamping is silently wrong. So: MEASURE IT.
//
// This profiler is examples/attachprof_probe/attachprof.cpp's sibling (same CINTERFACE C-vtable +
// generic-stub array-fill, same strict QI-below-Callback5 rule, same InitializeForAttach entry —
// see that file for why each is load-bearing) plus exactly one new job: it samples the TRACER's
// step counter out of a POSIX-shm channel at BOTH ends of TWO windows, which are NOT the same window
// and must not be conflated —
//   GC WINDOW    S0 at ICorProfilerCallback2::GarbageCollectionStarted
//                S1 at ICorProfilerCallback2::GarbageCollectionFinished     (COR_PRF_MONITOR_GC)
//                The GC as a profiler sees it: the window drain-time stamping would stamp against.
//   EE-SUSPENDED S0 at ICorProfilerCallback::RuntimeSuspendFinished  (every thread now parked)
//                S1 at ICorProfilerCallback::RuntimeResumeStarted    (COR_PRF_MONITOR_SUSPENDS)
//                Where the EE is LITERALLY fully suspended — the assumption's own wording.
// Each window is appended as a record {S0, S1, S1-S0, the MovedReferences2 ranges} for the tracer to
// drain. Both masks are in COR_PRF_ALLOWABLE_AFTER_ATTACH and come back S_OK post-attach.
//
// S1 - S0 == 0  -> the freeze assumption HOLDS; drain-time stamping is exact.
// S1 - S0  > 0  -> it is FALSE; F4 must stamp with the profiler-sampled S0 instead.
// (S0-stamping is robust EITHER WAY, so a non-zero result refines F4 rather than killing it.)
//
// `traced` / `traced_close` are load-bearing, not bookkeeping: a window that opens or closes while
// the tracer is NOT stepping reads a DEAD counter at that end, and a dead counter at both ends
// reports a FALSE ZERO — i.e. it would fabricate exactly the answer the design hopes for. Only
// windows with traced && traced_close are measurements. (Measured: the deltas are non-zero.)
//
// Relocating ranges (old != new) are counted SEPARATELY: MovedReferences2 legitimately reports
// non-relocating ranges, which would make a "ranges delivered" assert vacuous.
//
// Everything this file does inside a GC callback is integer loads/stores on an already-mapped shm
// page + clock_gettime: no allocation, no locks, no calls back into the runtime — the EE is
// suspended there and the profiler contract forbids all three. No DynamoRIO: this is the
// out-of-band ptrace tier.
#define CINTERFACE
// The C (CINTERFACE) vtable form uses these Windows RPC macros for vtable layout; the CoreCLR PAL
// doesn't define them (they're only needed in the C-interface path). No-ops / `const` on any ABI.
#define BEGIN_INTERFACE
#define END_INTERFACE
#define CONST_VTBL const
#include "cor.h"
#include "corprof.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "gcfence_shm.h"

// IID_ICorProfilerInfo (28B5557D-3F3F-48b4-90B2-5F9EEA2F6C48) — to fetch SetEventMask.
static const GUID IID_Info = {0x28B5557D,0x3F3F,0x48b4,{0x90,0xB2,0x5F,0x9E,0xEA,0x2F,0x6C,0x48}};
// Our object implements exactly IUnknown + ICorProfilerCallback..4. QI MUST reject Callback5+, else
// the CLR vcalls a slot past our vtable. Callback3 is required: the attach path QIs for it
// (InitializeForAttach lives there) and refuses to attach a profiler that does not answer.
static const GUID IID_IUnknown_ = {0x00000000,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID IID_Callback  = {0x176FBED1,0xA55C,0x4796,{0x98,0xCA,0xA9,0xDA,0x0E,0xF8,0x83,0xE7}};
static const GUID IID_Callback2 = {0x8A8CC829,0xCCF2,0x49fe,{0xBB,0xAE,0x0F,0x02,0x22,0x28,0x07,0x1A}};
static const GUID IID_Callback3 = {0x4FD2ED52,0x7731,0x4b8d,{0x94,0x69,0x03,0xD2,0xCC,0x30,0x86,0xC5}};
static const GUID IID_Callback4 = {0x7B63B2E3,0x107D,0x4d48,{0xB2,0xF6,0xF6,0x1E,0x22,0x94,0x70,0xD2}};
static bool guid_eq(REFIID a, const GUID &b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }

static ICorProfilerCallback4Vtbl g_vtbl;
struct Prof { const ICorProfilerCallback4Vtbl *lpVtbl; long ref; ICorProfilerInfo *info; };
static Prof g_prof;

static gcfence_channel_t *g_ch;      // the tracer handshake; NULL if the map failed
static gcfence_rec_t g_cur;          // the GC window being accumulated (Started..Finished)
static gcfence_rec_t g_susp;         // the EE-suspend window (SuspendStarted..ResumeStarted)
static bool g_in_fence, g_in_susp;
static unsigned long g_gc_index, g_susp_index;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// Map (creating if needed) the shm channel. Called ONCE from the attach entry, i.e. on a normal
// runtime thread with the EE running — never from a GC callback, where mmap/open would be reckless.
// The tracer may not have started yet (the driver attaches the profiler FIRST, because profiler
// attach travels the diagnostics IPC socket which needs a RUNNING runtime to service, whereas the
// tracer stops its target); that is fine — the segment is created here and the tracer opens the
// same name later, publishing GCFENCE_MAGIC when it starts stepping.
static void map_channel(void) {
    int fd = shm_open(GCFENCE_SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { fprintf(stderr, "GCFENCEPROF: shm_open failed errno=%d\n", errno); fflush(stderr); return; }
    if (ftruncate(fd, (off_t)sizeof(gcfence_channel_t)) != 0) {
        // Losing the size race with the tracer is fine (same size); a real failure is not.
        struct stat st;
        if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(gcfence_channel_t)) {
            fprintf(stderr, "GCFENCEPROF: ftruncate failed errno=%d\n", errno); fflush(stderr);
            close(fd); return;
        }
    }
    void *p = mmap(NULL, sizeof(gcfence_channel_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { fprintf(stderr, "GCFENCEPROF: mmap failed errno=%d\n", errno); fflush(stderr); return; }
    g_ch = (gcfence_channel_t *)p;
    fprintf(stderr, "GCFENCEPROF: shm channel %s mapped (%zu bytes) — step counter is readable\n",
            GCFENCE_SHM_NAME, sizeof(gcfence_channel_t));
    fflush(stderr);
}

static HRESULT STDMETHODCALLTYPE GenericStub(void) { return S_OK; }
static ULONG STDMETHODCALLTYPE Prof_AddRef(ICorProfilerCallback4 *This)  { return (ULONG)++((Prof*)This)->ref; }
static ULONG STDMETHODCALLTYPE Prof_Release(ICorProfilerCallback4 *This) { return (ULONG)--((Prof*)This)->ref; }
static HRESULT STDMETHODCALLTYPE Prof_QI(ICorProfilerCallback4 *This, REFIID riid, void **ppv) {
    if (guid_eq(riid, IID_IUnknown_) || guid_eq(riid, IID_Callback) ||
        guid_eq(riid, IID_Callback2) || guid_eq(riid, IID_Callback3) ||
        guid_eq(riid, IID_Callback4)) {
        *ppv = This; Prof_AddRef(This); return S_OK;
    }
    *ppv = NULL; return E_NOINTERFACE;
}

static HRESULT set_mask(Prof *p, IUnknown *pInfoUnk, const char *how) {
    HRESULT hr = pInfoUnk->QueryInterface(IID_Info, (void**)&p->info);
    if (hr < 0 || p->info == NULL) {
        fprintf(stderr, "GCFENCEPROF: %s QI(ICorProfilerInfo) FAILED hr=0x%08x\n", how, (unsigned)hr);
        fflush(stderr);
        return hr;
    }
    // COR_PRF_MONITOR_GC (0x80) is in COR_PRF_ALLOWABLE_AFTER_ATTACH and is already proven to come
    // back S_OK post-attach (f4-attach-profiler-probe-findings.md). It carries both the GC-window
    // callbacks (GarbageCollectionStarted/Finished) and MovedReferences2.
    //
    // COR_PRF_MONITOR_SUSPENDS (0x40000) is ALSO in COR_PRF_ALLOWABLE_AFTER_ATTACH, and it is what
    // makes this probe honest: it delivers RuntimeSuspendFinished / RuntimeResumeStarted, i.e. the
    // window in which the EE is ACTUALLY fully suspended — which is NOT the same window as
    // GarbageCollectionStarted..Finished, and the assumption under test is worded about the former.
    // Ask for both; if the pair is rejected, fall back to GC alone and say so rather than measuring
    // a window we did not get.
    hr = p->info->lpVtbl->SetEventMask(p->info, COR_PRF_MONITOR_GC | COR_PRF_MONITOR_SUSPENDS);
    fprintf(stderr, "GCFENCEPROF: %s — SetEventMask(COR_PRF_MONITOR_GC|COR_PRF_MONITOR_SUSPENDS=0x40080) hr=0x%08x%s\n",
            how, (unsigned)hr, (hr < 0 ? "  <-- FAILED" : "  (S_OK)"));
    fflush(stderr);
    if (hr < 0) {
        hr = p->info->lpVtbl->SetEventMask(p->info, COR_PRF_MONITOR_GC);
        fprintf(stderr, "GCFENCEPROF: %s — FALLBACK SetEventMask(COR_PRF_MONITOR_GC=0x80) hr=0x%08x%s"
                        "  (no EE-suspend window will be measured)\n",
                how, (unsigned)hr, (hr < 0 ? "  <-- FAILED" : "  (S_OK)"));
        fflush(stderr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Prof_InitializeForAttach(ICorProfilerCallback4 *This,
        IUnknown *pInfoUnk, void *pvClientData, UINT cbClientData) {
    (void)pvClientData;
    fprintf(stderr, "GCFENCEPROF: InitializeForAttach ENTERED pid=%d cbClientData=%u\n",
            (int)getpid(), (unsigned)cbClientData);
    fflush(stderr);
    map_channel();
    return set_mask((Prof*)This, pInfoUnk, "InitializeForAttach");
}
static HRESULT STDMETHODCALLTYPE Prof_ProfilerAttachComplete(ICorProfilerCallback4 *This) {
    (void)This;
    fprintf(stderr, "GCFENCEPROF: ProfilerAttachComplete — GC fence callbacks live\n");
    fflush(stderr);
    return S_OK;
}
// Present only so a mis-wired lane (startup env vars) fails loudly instead of looking like attach.
static HRESULT STDMETHODCALLTYPE Prof_Initialize(ICorProfilerCallback4 *This, IUnknown *pInfoUnk) {
    fprintf(stderr, "GCFENCEPROF: Initialize (STARTUP path — NOT the attach path)\n"); fflush(stderr);
    map_channel();
    return set_mask((Prof*)This, pInfoUnk, "Initialize");
}

// ---- THE MEASUREMENT ---------------------------------------------------------------------
static void emit(const char *what, gcfence_rec_t *r) {
    if (g_ch) {
        uint32_t n = g_ch->nrec;
        if (n < GCFENCE_MAX_RECS) { g_ch->recs[n] = *r; g_ch->nrec = n + 1; }
    }
    // One line per window, so the raw measurement survives even if the shm drain is lost.
    fprintf(stderr, "GCFENCEPROF: %s seq=%u gens=0x%x reason=%u traced=%u traced_close=%u S0=%u S1=%u "
                    "DELTA=%d s_pre=%u s_move=%u moved_calls=%u ranges=%u reloc=%u us=%llu\n",
            what, r->seq, r->gens, r->reason, r->traced, r->traced_close, r->s0, r->s1,
            (int)(r->s1 - r->s0), r->s_pre, r->s_move, r->moved_calls, r->moved_ranges, r->reloc_ranges,
            (unsigned long long)((r->t1_ns - r->t0_ns) / 1000));
    fflush(stderr);
}

// Fires on the GC thread. S0 is sampled here — this is exactly the sample F4 would use if
// drain-time stamping turns out to be unsafe.
static HRESULT STDMETHODCALLTYPE Prof_GCStarted(ICorProfilerCallback4 *This, int cGenerations,
        BOOL generationCollected[], COR_PRF_GC_REASON reason) {
    (void)This;
    memset(&g_cur, 0, sizeof(g_cur));
    g_cur.kind = GCFENCE_KIND_GC;
    g_cur.seq = (uint32_t)++g_gc_index;
    g_cur.reason = (uint32_t)reason;
    for (int i = 0; i < cGenerations && i < 32; i++)
        if (generationCollected[i]) g_cur.gens |= 1u << i;
    if (g_ch) {
        g_cur.traced = (g_ch->magic == GCFENCE_MAGIC) ? 1u : 0u;
        g_cur.s0 = g_ch->step_counter;   // <-- S0
        g_cur.s_move = g_cur.s0;
        g_ch->fence_active = 1;          // tells the tracer's watcher to sample /proc state NOW
        g_ch->gcs_seen++;
    }
    g_cur.t0_ns = now_ns();
    g_in_fence = true;
    return S_OK;
}

// ---- the EE-suspension window (COR_PRF_MONITOR_SUSPENDS) ---------------------------------
// RuntimeSuspendStarted: the runtime has DECIDED to suspend; threads are not parked yet.
static HRESULT STDMETHODCALLTYPE Prof_SuspendStarted(ICorProfilerCallback4 *This,
        COR_PRF_SUSPEND_REASON reason) {
    (void)This;
    memset(&g_susp, 0, sizeof(g_susp));
    g_susp.kind = GCFENCE_KIND_SUSPEND;
    g_susp.seq = (uint32_t)++g_susp_index;
    g_susp.reason = (uint32_t)reason;
    g_susp.t0_ns = now_ns();
    if (g_ch) {
        g_susp.traced = (g_ch->magic == GCFENCE_MAGIC) ? 1u : 0u;
        g_susp.s_pre = g_ch->step_counter;
        g_susp.s0 = g_susp.s_pre;
    }
    g_in_susp = true;
    return S_OK;
}
// RuntimeSuspendFinished: EVERY managed thread is now parked. THE fence, literally.
static HRESULT STDMETHODCALLTYPE Prof_SuspendFinished(ICorProfilerCallback4 *This) {
    (void)This;
    if (!g_in_susp) return S_OK;
    if (g_ch) { g_susp.s0 = g_ch->step_counter; g_ch->ee_suspended = 1; }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Prof_SuspendAborted(ICorProfilerCallback4 *This) {
    (void)This;
    if (g_ch) g_ch->ee_suspended = 0;
    g_in_susp = false;
    fprintf(stderr, "GCFENCEPROF: RuntimeSuspendAborted\n"); fflush(stderr);
    return S_OK;
}
// RuntimeResumeStarted: still parked, about to let them go. Closes the EE-suspended window.
static HRESULT STDMETHODCALLTYPE Prof_ResumeStarted(ICorProfilerCallback4 *This) {
    (void)This;
    if (!g_in_susp) return S_OK;
    g_susp.t1_ns = now_ns();
    if (g_ch) {
        g_susp.s1 = g_ch->step_counter;
        g_susp.traced_close = (g_ch->magic == GCFENCE_MAGIC) ? 1u : 0u;
        g_ch->ee_suspended = 0;
        g_ch->susp_seen++;
    }
    g_in_susp = false;
    emit("SUSPEND", &g_susp);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Prof_MovedReferences2(ICorProfilerCallback4 *This, ULONG c,
        ObjectID oldS[], ObjectID newS[], SIZE_T len[]) {
    (void)This;
    if (!g_in_fence) return S_OK; // MovedReferences2 outside a fence would be a surprise; ignore.
    if (g_ch && g_cur.moved_calls == 0) g_cur.s_move = g_ch->step_counter;
    g_cur.moved_calls++;
    g_cur.moved_ranges += (uint32_t)c;
    for (ULONG i = 0; i < c; i++) {
        if (oldS[i] == newS[i]) continue;  // non-relocating: useless to F4's canonicalization
        g_cur.reloc_ranges++;
        if (g_cur.old0 == 0) { g_cur.old0 = (uint64_t)oldS[i]; g_cur.new0 = (uint64_t)newS[i]; g_cur.len0 = (uint64_t)len[i]; }
    }
    return S_OK;
}

// Fires on the GC thread before the EE resumes. S1 is sampled here; S1 - S0 is the number of
// instructions the traced managed thread retired across the GC window.
static HRESULT STDMETHODCALLTYPE Prof_GCFinished(ICorProfilerCallback4 *This) {
    (void)This;
    if (!g_in_fence) return S_OK;
    g_cur.t1_ns = now_ns();
    if (g_ch) {
        g_cur.s1 = g_ch->step_counter;   // <-- S1
        g_cur.traced_close = (g_ch->magic == GCFENCE_MAGIC) ? 1u : 0u;
        g_ch->fence_active = 0;
    }
    g_in_fence = false;
    emit("GCWINDOW", &g_cur);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Prof_Shutdown(ICorProfilerCallback4 *This) {
    (void)This;
    fprintf(stderr, "GCFENCEPROF: Shutdown (gc_windows=%lu ee_suspends=%lu)\n", g_gc_index, g_susp_index);
    fflush(stderr);
    return S_OK;
}

static void build_vtbl(void) {
    void **slots = (void**)&g_vtbl;
    size_t n = sizeof(g_vtbl) / sizeof(void*);
    for (size_t i = 0; i < n; i++) slots[i] = (void*)GenericStub;
    g_vtbl.QueryInterface = Prof_QI;
    g_vtbl.AddRef = Prof_AddRef;
    g_vtbl.Release = Prof_Release;
    g_vtbl.Initialize = Prof_Initialize;
    g_vtbl.InitializeForAttach = Prof_InitializeForAttach;
    g_vtbl.ProfilerAttachComplete = Prof_ProfilerAttachComplete;
    g_vtbl.GarbageCollectionStarted = Prof_GCStarted;
    g_vtbl.GarbageCollectionFinished = Prof_GCFinished;
    g_vtbl.MovedReferences2 = Prof_MovedReferences2;
    g_vtbl.RuntimeSuspendStarted = Prof_SuspendStarted;
    g_vtbl.RuntimeSuspendFinished = Prof_SuspendFinished;
    g_vtbl.RuntimeSuspendAborted = Prof_SuspendAborted;
    g_vtbl.RuntimeResumeStarted = Prof_ResumeStarted;
    g_vtbl.Shutdown = Prof_Shutdown;
}

// ---- class factory + DllGetClassObject (the CLR entry point) ------------------------------
struct Factory : public IClassFactory {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **ppv) override { *ppv = this; return S_OK; }
    ULONG   STDMETHODCALLTYPE AddRef() override  { return 1; }
    ULONG   STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *, REFIID, void **ppv) override {
        build_vtbl();
        g_prof.lpVtbl = &g_vtbl; g_prof.ref = 1; g_prof.info = NULL;
        *ppv = &g_prof;
        fprintf(stderr, "GCFENCEPROF: CreateInstance — profiler object handed to CLR\n"); fflush(stderr);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};
static Factory g_factory;

extern "C" HRESULT STDMETHODCALLTYPE DllGetClassObject(REFCLSID, REFIID, void **ppv) {
    *ppv = static_cast<IClassFactory*>(&g_factory);
    fprintf(stderr, "GCFENCEPROF: DllGetClassObject — profiler .so loaded into the RUNNING process\n");
    fflush(stderr);
    return S_OK;
}
