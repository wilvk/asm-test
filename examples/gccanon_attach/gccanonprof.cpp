// gccanonprof.cpp — the FEED half of F4 increment 1: an attach-mode CLR profiler that turns
// ICorProfilerCallback4::MovedReferences2 into STAMPED {old, new, len, step} triples for the ptrace
// live-attach tier's asmtest_gcmove_canonicalize.
//
// It is examples/gcfence_probe/gcfenceprof.cpp's sibling (same CINTERFACE C-vtable + generic-stub
// array-fill, same strict QI-below-Callback5 rule, same InitializeForAttach entry — see
// examples/attachprof_probe/attachprof.cpp for why each is load-bearing) with its measuring job
// replaced by a producing one:
//
//   GarbageCollectionStarted   sample S0 = the tracer's live step counter, from inside the fence.
//   MovedReferences2           append every RELOCATING range, stamped with that S0.
//   GarbageCollectionFinished  close the window.
//
// WHY S0 AND NOT A DRAIN-TIME COUNT. f4-gc-fence-freeze-probe-findings.md measured the freeze
// assumption ("a suspended EE means the stepped thread retires nothing, so the counter can be read
// afterwards") and found it FALSE on 47 of 47 fences — 342-558 instructions, because single-stepping
// a futex-blocked thread is what un-blocks it. S0 is exact under both outcomes and costs nothing.
//
// WHY old != new IS FILTERED, NOT ASSUMED. MovedReferences2 reports non-relocating ranges too; the
// attach probe's first sample range came back old == new. Feeding those to the canonicalizer would
// be harmless but counting them as evidence would be vacuous, so they are counted apart.
//
// Everything done inside a GC callback is integer loads/stores on an already-mapped shm page: no
// allocation, no locks, no calls back into the runtime — the EE is suspended there and the profiler
// contract forbids all three. No DynamoRIO: this is the out-of-band ptrace tier.
#define CINTERFACE
// The C (CINTERFACE) vtable form uses these Windows RPC macros for vtable layout; the CoreCLR PAL
// doesn't define them (they're only needed in the C-interface path). No-ops / `const` on any ABI.
#define BEGIN_INTERFACE
#define END_INTERFACE
#define CONST_VTBL const
#include "cor.h"
#include "corprof.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gccanon_shm.h"

// IID_ICorProfilerInfo (28B5557D-3F3F-48b4-90B2-5F9EEA2F6C48) — to fetch SetEventMask.
static const GUID IID_Info = {0x28B5557D,0x3F3F,0x48b4,{0x90,0xB2,0x5F,0x9E,0xEA,0x2F,0x6C,0x48}};
// Our object implements exactly IUnknown + ICorProfilerCallback..4. QI MUST reject Callback5+, else
// the CLR vcalls a slot past our vtable. Callback3 is required, not optional: the attach path QIs
// for it (InitializeForAttach lives there) and refuses a profiler that does not answer.
static const GUID IID_IUnknown_ = {0x00000000,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID IID_Callback  = {0x176FBED1,0xA55C,0x4796,{0x98,0xCA,0xA9,0xDA,0x0E,0xF8,0x83,0xE7}};
static const GUID IID_Callback2 = {0x8A8CC829,0xCCF2,0x49fe,{0xBB,0xAE,0x0F,0x02,0x22,0x28,0x07,0x1A}};
static const GUID IID_Callback3 = {0x4FD2ED52,0x7731,0x4b8d,{0x94,0x69,0x03,0xD2,0xCC,0x30,0x86,0xC5}};
static const GUID IID_Callback4 = {0x7B63B2E3,0x107D,0x4d48,{0xB2,0xF6,0xF6,0x1E,0x22,0x94,0x70,0xD2}};
static bool guid_eq(REFIID a, const GUID &b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }

static ICorProfilerCallback4Vtbl g_vtbl;
struct Prof { const ICorProfilerCallback4Vtbl *lpVtbl; long ref; ICorProfilerInfo *info; };
static Prof g_prof;

static gccanon_channel_t *g_ch;    // the tracer handshake; NULL if the map failed
static bool g_in_fence;
static uint32_t g_gc_seq;          // 1-based GC index
static uint32_t g_cur_s0;          // S0 of the GC currently open
static uint32_t g_cur_s0_recs;     // the trace's recs_len when it opened — see gccanon_gcinfo_t
static uint32_t g_cur_traced;      // was the tracer live when it opened?
static uint32_t g_cur_reloc;       // relocating ranges in the GC currently open

// Map (creating if needed) the shm channel. Called ONCE from the attach entry — a normal runtime
// thread with the EE running, never from a GC callback, where open/mmap would be reckless. The
// tracer may not exist yet (the profiler is attached FIRST, because its attach travels the
// diagnostics IPC socket, which the runtime must be RUNNING to service, whereas the tracer stops
// its target); that is fine — the segment is created here and the tracer opens the same name later.
static void map_channel(void) {
    int fd = shm_open(GCCANON_SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { fprintf(stderr, "GCCANONPROF: shm_open failed errno=%d\n", errno); fflush(stderr); return; }
    if (ftruncate(fd, (off_t)sizeof(gccanon_channel_t)) != 0) {
        // Losing the size race with the tracer is fine (same size); a real failure is not.
        struct stat st;
        if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(gccanon_channel_t)) {
            fprintf(stderr, "GCCANONPROF: ftruncate failed errno=%d\n", errno); fflush(stderr);
            close(fd); return;
        }
    }
    void *p = mmap(NULL, sizeof(gccanon_channel_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { fprintf(stderr, "GCCANONPROF: mmap failed errno=%d\n", errno); fflush(stderr); return; }
    g_ch = (gccanon_channel_t *)p;
    fprintf(stderr, "GCCANONPROF: shm channel %s mapped (%zu bytes) — step counter readable, moves publishable\n",
            GCCANON_SHM_NAME, sizeof(gccanon_channel_t));
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
        fprintf(stderr, "GCCANONPROF: %s QI(ICorProfilerInfo) FAILED hr=0x%08x\n", how, (unsigned)hr);
        fflush(stderr);
        return hr;
    }
    // COR_PRF_MONITOR_GC (0x80) is in COR_PRF_ALLOWABLE_AFTER_ATTACH (pinned v8.0.8 corprof.h L579)
    // and is proven to come back S_OK post-attach (f4-attach-profiler-probe-findings.md). It carries
    // both the GC-window callbacks and MovedReferences2; the high GC_MOVED_OBJECTS mask is not
    // needed. This tier wants no suspend callbacks — S0 at GarbageCollectionStarted is the stamp.
    hr = p->info->lpVtbl->SetEventMask(p->info, COR_PRF_MONITOR_GC);
    fprintf(stderr, "GCCANONPROF: %s — SetEventMask(COR_PRF_MONITOR_GC=0x80) hr=0x%08x%s\n",
            how, (unsigned)hr, (hr < 0 ? "  <-- FAILED" : "  (S_OK)"));
    fflush(stderr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Prof_InitializeForAttach(ICorProfilerCallback4 *This,
        IUnknown *pInfoUnk, void *pvClientData, UINT cbClientData) {
    (void)pvClientData;
    fprintf(stderr, "GCCANONPROF: InitializeForAttach ENTERED pid=%d cbClientData=%u\n",
            (int)getpid(), (unsigned)cbClientData);
    fflush(stderr);
    map_channel();
    return set_mask((Prof*)This, pInfoUnk, "InitializeForAttach");
}
static HRESULT STDMETHODCALLTYPE Prof_ProfilerAttachComplete(ICorProfilerCallback4 *This) {
    (void)This;
    fprintf(stderr, "GCCANONPROF: ProfilerAttachComplete — GC-move feed live\n");
    fflush(stderr);
    return S_OK;
}
// Present only so a mis-wired lane (startup env vars) fails loudly instead of looking like attach.
static HRESULT STDMETHODCALLTYPE Prof_Initialize(ICorProfilerCallback4 *This, IUnknown *pInfoUnk) {
    fprintf(stderr, "GCCANONPROF: Initialize (STARTUP path — NOT the attach path)\n"); fflush(stderr);
    map_channel();
    return set_mask((Prof*)This, pInfoUnk, "Initialize");
}

// ---- THE FEED ------------------------------------------------------------------------------
// Fires on the GC thread, inside the fence. THE stamp: S0 is the tracer's step counter right now.
static HRESULT STDMETHODCALLTYPE Prof_GCStarted(ICorProfilerCallback4 *This, int cGenerations,
        BOOL generationCollected[], COR_PRF_GC_REASON reason) {
    (void)This; (void)cGenerations; (void)generationCollected; (void)reason;
    g_gc_seq++;
    g_cur_reloc = 0;
    g_cur_s0 = 0;
    g_cur_s0_recs = 0;
    g_cur_traced = 0;
    if (g_ch) {
        g_cur_traced = (g_ch->magic == GCCANON_MAGIC) ? 1u : 0u;
        g_cur_s0 = g_ch->step_counter;   // <-- S0
        g_cur_s0_recs = g_ch->recs_counter;
        g_ch->fence_active = 1;
        g_ch->gcs_seen++;
        if (g_cur_traced) { g_ch->gcs_traced++; g_ch->last_s0 = g_cur_s0; }
    }
    g_in_fence = true;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Prof_MovedReferences2(ICorProfilerCallback4 *This, ULONG c,
        ObjectID oldS[], ObjectID newS[], SIZE_T len[]) {
    (void)This;
    if (!g_in_fence || !g_ch) return S_OK;
    for (ULONG i = 0; i < c; i++) {
        if (oldS[i] == newS[i]) { g_ch->nonreloc_total++; continue; }  // non-relocating: vacuous
        g_ch->moves_total++;
        g_cur_reloc++;
        // Only a GC whose S0 was sampled against a LIVE tracer can be stamped. A GC that ran before
        // the region was entered has a counter of 0, and a step-0 stamp is inert anyway (canon
        // forwards a record only when the move's step is strictly GREATER than the record's) — so
        // recording them would just crowd out the ones that matter.
        if (!g_cur_traced || g_cur_s0 == 0) continue;
        uint32_t n = g_ch->nmoves;
        if (n >= GCCANON_MAX_MOVES) continue;  // overflow stays visible via moves_total
        g_ch->moves[n].old_base = (uint64_t)oldS[i];
        g_ch->moves[n].new_base = (uint64_t)newS[i];
        g_ch->moves[n].len      = (uint64_t)len[i];
        g_ch->moves[n].step     = g_cur_s0;
        g_ch->moves[n].gc_seq   = g_gc_seq;
        g_ch->nmoves = n + 1;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Prof_GCFinished(ICorProfilerCallback4 *This) {
    (void)This;
    if (!g_in_fence) return S_OK;
    if (g_ch) {
        // THE EVIDENCE FOR CHAINING (increment 2). Close this GC's fence record: the live trace's
        // counters as they were at the START of the fence and as they are NOW, at its end. The
        // tracer keeps only the GCs of the window under test (by seq) and requires all their samples
        // to agree — which is what says no record was appended across the fences or the gaps
        // between them, i.e. that no record can lie BETWEEN two of the window's moves. Recorded for
        // TRACED, non-zero-S0 GCs only: the same liveness rule the move ranges use, and it keeps the
        // victim's out-of-window fragmentation GCs (whose counter legitimately differs) out of it.
        if (g_cur_traced && g_cur_s0 != 0 && g_ch->ngcinfo < GCCANON_MAX_GCINFO) {
            uint32_t n = g_ch->ngcinfo;
            g_ch->gcs[n].seq = g_gc_seq;
            g_ch->gcs[n].s0_steps = g_cur_s0;
            g_ch->gcs[n].s0_recs = g_cur_s0_recs;
            g_ch->gcs[n].s1_steps = g_ch->step_counter;
            g_ch->gcs[n].s1_recs = g_ch->recs_counter;
            g_ch->ngcinfo = n + 1;
        }
        g_ch->fence_active = 0;
    }
    g_in_fence = false;
    // One line per GC, so the raw feed survives even if the shm drain is lost.
    fprintf(stderr, "GCCANONPROF: GC seq=%u traced=%u S0=%u reloc_ranges=%u\n",
            g_gc_seq, g_cur_traced, g_cur_s0, g_cur_reloc);
    fflush(stderr);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Prof_Shutdown(ICorProfilerCallback4 *This) {
    (void)This;
    fprintf(stderr, "GCCANONPROF: Shutdown (gcs=%u recorded_moves=%u)\n",
            g_gc_seq, g_ch ? g_ch->nmoves : 0u);
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
        fprintf(stderr, "GCCANONPROF: CreateInstance — profiler object handed to CLR\n"); fflush(stderr);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};
static Factory g_factory;

extern "C" HRESULT STDMETHODCALLTYPE DllGetClassObject(REFCLSID, REFIID, void **ppv) {
    *ppv = static_cast<IClassFactory*>(&g_factory);
    fprintf(stderr, "GCCANONPROF: DllGetClassObject — profiler .so loaded into the RUNNING process\n");
    fflush(stderr);
    return S_OK;
}
