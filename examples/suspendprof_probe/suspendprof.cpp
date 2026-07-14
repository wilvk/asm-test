// suspendprof.cpp — MINIMAL CLR profiler for the MANAGED-ATTACH SAFEPOINT plan, Increment 1
// (dynamorio-managed-attach-safepoint-plan.md). Proves the SUSPENSION PRIMITIVE: can a co-loaded
// CLR profiler park all managed threads at GC-safe points (ICorProfilerInfo10::SuspendRuntime) and
// resume them (ResumeRuntime), in a loop, WITHOUT crashing/hanging — first natively, then under a
// DynamoRIO that is already coexisting (drrun -c <client> -- dotnet). If SuspendRuntime cannot even
// be driven cleanly under DR-launch coexistence, Option 2 is dead here (the Increment-1 kill
// criterion); if it survives N cycles clean, the suspend-then-SEIZE ordering (Increment 2) is worth
// building.
//
// Reuses the gcprofiler_probe scaffold verbatim: the CINTERFACE (C vtable) form + the GenericStub
// array-fill trick for the ~90 unimplemented callback slots; only QueryInterface/AddRef/Release/
// Initialize/Shutdown are real. The one new thing is QI'ing for ICorProfilerInfo10 (which carries
// SuspendRuntime/ResumeRuntime) and a profiler-created native thread that drives the cycle.
#define CINTERFACE
#define BEGIN_INTERFACE
#define END_INTERFACE
#define CONST_VTBL const
#include "cor.h"
#include "corprof.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

// IID_ICorProfilerInfo10 {2F1B5152-C869-40C9-AA5F-3ABE026BD720} — carries SuspendRuntime/ResumeRuntime.
static const GUID IID_Info10 = {0x2F1B5152,0xC869,0x40C9,{0xAA,0x5F,0x3A,0xBE,0x02,0x6B,0xD7,0x20}};
// Interfaces our object implements (vtbl is ICorProfilerCallback4). QI MUST reject Callback5+ (else
// the CLR vcalls a slot past our vtable -> crash) — the gcprofiler_probe gotcha.
static const GUID IID_IUnknown_ = {0x00000000,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID IID_Callback  = {0x176FBED1,0xA55C,0x4796,{0x98,0xCA,0xA9,0xDA,0x0E,0xF8,0x83,0xE7}};
static const GUID IID_Callback2 = {0x8A8CC829,0xCCF2,0x49fe,{0xBB,0xAE,0x0F,0x02,0x22,0x28,0x07,0x1A}};
static const GUID IID_Callback3 = {0x4FD2ED52,0x7731,0x4b8d,{0x94,0x69,0x03,0xD2,0xCC,0x30,0x86,0xC5}};
static const GUID IID_Callback4 = {0x7B63B2E3,0x107D,0x4d48,{0xB2,0xF6,0xF6,0x1E,0x22,0x94,0x70,0xD2}};
static bool guid_eq(REFIID a, const GUID &b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }

// How many suspend/resume cycles to drive (SUSPENDPROF_CYCLES env overrides).
static int g_cycles = 5;
static ICorProfilerInfo10 *g_info; // the suspend/resume vtable

// The profiler's own NATIVE thread (never a managed thread — the safe place to call SuspendRuntime;
// SuspendRuntime stops all OTHER threads and this one keeps running, then ResumeRuntime on the SAME
// thread). It sleeps first so the runtime is fully up + the victim's work loop is running.
//
// MODE=cycle (Increment 1, default): loop SuspendRuntime -> brief stop-the-world -> ResumeRuntime,
// logging each so the lane counts — the primitive under DR coexistence.
static void suspend_cycle(void) {
    for (int i = 0; i < g_cycles; i++) {
        HRESULT hs = g_info->lpVtbl->SuspendRuntime(g_info);
        fprintf(stderr, "SUSPENDPROF: SuspendRuntime #%d hr=0x%08x\n", i, (unsigned)hs);
        fflush(stderr);
        usleep(120 * 1000); // hold the world stopped briefly
        HRESULT hr = g_info->lpVtbl->ResumeRuntime(g_info);
        fprintf(stderr, "SUSPENDPROF: ResumeRuntime  #%d hr=0x%08x\n", i, (unsigned)hr);
        fflush(stderr);
        usleep(600 * 1000); // let the victim run between cycles
    }
    fprintf(stderr, "SUSPENDPROF: cycles_done=%d\n", g_cycles);
    fflush(stderr);
}

// MODE=hold (Increment 2, suspend-then-SEIZE ordering): SuspendRuntime ONCE, publish a "suspended"
// sentinel so the harness can DR-attach WHILE the managed threads are parked at GC-safe points, then
// hold until the harness (after the attach delivers) publishes the "resume" sentinel, then
// ResumeRuntime ONCE. This is the choreography that tests whether seizing a SUSPENDED runtime
// avoids the arbitrary-state-takeover crash the Increment-6 probe hit.
static void suspend_hold(void) {
    const char *susf = getenv("SUSPENDPROF_SUSPENDED_FILE");
    const char *resf = getenv("SUSPENDPROF_RESUME_FILE");
    HRESULT hs = g_info->lpVtbl->SuspendRuntime(g_info);
    fprintf(stderr, "SUSPENDPROF: HOLD SuspendRuntime hr=0x%08x — runtime parked, awaiting attach\n",
            (unsigned)hs);
    fflush(stderr);
    if (susf != NULL) { FILE *f = fopen(susf, "w"); if (f) { fputs("1", f); fclose(f); } }
    int waited = 0;
    while (resf != NULL) { // poll for the harness's resume sentinel (native thread, not suspended)
        struct stat st;
        if (stat(resf, &st) == 0) break;
        usleep(100 * 1000);
        if (++waited > 400) break; // ~40 s cap so it always terminates
    }
    HRESULT hr = g_info->lpVtbl->ResumeRuntime(g_info);
    fprintf(stderr, "SUSPENDPROF: HOLD ResumeRuntime hr=0x%08x (held ~%d ms)\n",
            (unsigned)hr, waited * 100);
    fflush(stderr);
}

static void *suspend_thread(void *arg) {
    (void)arg;
    usleep(3000 * 1000); // let the CLR finish starting + the victim begin its loop (slow under DR)
    const char *mode = getenv("SUSPENDPROF_MODE");
    if (mode != NULL && strcmp(mode, "hold") == 0)
        suspend_hold();
    else
        suspend_cycle();
    return nullptr;
}

// ---- the profiler object: an ICorProfilerCallback4 (C vtable) ---------------------------
static ICorProfilerCallback4Vtbl g_vtbl;
struct Prof { const ICorProfilerCallback4Vtbl *lpVtbl; long ref; ICorProfilerInfo10 *info; };
static Prof g_prof;

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
static HRESULT STDMETHODCALLTYPE Prof_Initialize(ICorProfilerCallback4 *This, IUnknown *pInfoUnk) {
    Prof *p = (Prof*)This;
    const char *cy = getenv("SUSPENDPROF_CYCLES");
    if (cy != NULL) { int v = atoi(cy); if (v > 0 && v <= 100) g_cycles = v; }
    // QI the PAL C++ IUnknown for the C-vtable ICorProfilerInfo10 (SuspendRuntime/ResumeRuntime).
    HRESULT hr = pInfoUnk->QueryInterface(IID_Info10, (void**)&p->info);
    if (hr < 0 || p->info == NULL) {
        fprintf(stderr, "SUSPENDPROF: Initialize QI(ICorProfilerInfo10) FAILED hr=0x%08x "
                        "(runtime too old for SuspendRuntime?)\n", (unsigned)hr);
        return hr;
    }
    p->info->lpVtbl->SetEventMask(p->info, 0 /*COR_PRF_MONITOR_NONE*/);
    g_info = p->info;
    fprintf(stderr, "SUSPENDPROF: Initialize OK (ICorProfilerInfo10) — starting suspend thread "
                    "(cycles=%d)\n", g_cycles);
    fflush(stderr);
    pthread_t t;
    if (pthread_create(&t, NULL, suspend_thread, NULL) == 0)
        pthread_detach(t);
    else
        fprintf(stderr, "SUSPENDPROF: pthread_create FAILED\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Prof_Shutdown(ICorProfilerCallback4 *This) {
    (void)This; fprintf(stderr, "SUSPENDPROF: Shutdown\n"); fflush(stderr); return S_OK;
}

static void build_vtbl(void) {
    void **slots = (void**)&g_vtbl;
    size_t n = sizeof(g_vtbl) / sizeof(void*);
    for (size_t i = 0; i < n; i++) slots[i] = (void*)GenericStub;
    g_vtbl.QueryInterface = Prof_QI;
    g_vtbl.AddRef = Prof_AddRef;
    g_vtbl.Release = Prof_Release;
    g_vtbl.Initialize = Prof_Initialize;
    g_vtbl.Shutdown = Prof_Shutdown;
}

// ---- class factory + DllGetClassObject (the CLR entry point) -----------------------------
struct Factory : public IClassFactory {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **ppv) override { *ppv = this; return S_OK; }
    ULONG   STDMETHODCALLTYPE AddRef() override  { return 1; }
    ULONG   STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *, REFIID, void **ppv) override {
        build_vtbl();
        g_prof.lpVtbl = &g_vtbl; g_prof.ref = 1; g_prof.info = NULL;
        *ppv = &g_prof;
        fprintf(stderr, "SUSPENDPROF: CreateInstance — profiler object handed to CLR\n"); fflush(stderr);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};
static Factory g_factory;

extern "C" HRESULT STDMETHODCALLTYPE DllGetClassObject(REFCLSID, REFIID, void **ppv) {
    *ppv = static_cast<IClassFactory*>(&g_factory);
    fprintf(stderr, "SUSPENDPROF: DllGetClassObject — profiler .so loaded by CLR\n"); fflush(stderr);
    return S_OK;
}
