// attachprof.cpp — MINIMAL CLR profiler for the F4 ATTACH-MODE GC-move PROBE (go/no-go).
//
// The question (live-attach-dataflow-followup-plan.md, F4): `CORECLR_ENABLE_PROFILING` is read
// at STARTUP, so the ptrace live-attach tier — which attaches to processes asmspy did NOT launch
// — cannot use the DR tier's startup-env-var profiler wiring. Can a profiler instead be attached
// to an ALREADY-RUNNING dotnet process (via the diagnostics port), and STILL receive
// ICorProfilerCallback4::MovedReferences2 {old,new,len} GC-move triples?
//
// This is examples/gcprofiler_probe/gcprofiler.cpp's sibling with the ONE structural difference
// that matters: an ATTACHING profiler never gets ICorProfilerCallback::Initialize. It gets
//     ICorProfilerCallback3::InitializeForAttach(IUnknown *pInfoUnk, void *pvClientData, UINT cb)
// followed by ICorProfilerCallback3::ProfilerAttachComplete(). The event mask must be set from
// InitializeForAttach, and the runtime is contractually allowed to reject masks that are not in
// COR_PRF_ALLOWABLE_AFTER_ATTACH (CORPROF_E_UNSUPPORTED_FOR_ATTACHING_PROFILER, 0x80131363) —
// which is exactly one of the probe's kill criteria, hence the hr is logged verbatim.
//
// Deliberately carries NO DynamoRIO / shm handshake: this is the out-of-band ptrace tier, and the
// probe answers only the attach+delivery question. It logs to stderr; a driver greps ATTACHPROF.
//
// Uses the CINTERFACE (C vtable) form of corprof.h so every unimplemented slot can be filled with
// one generic S_OK stub (the array-fill trick) instead of hand-overriding ~90 pure-virtuals.
#define CINTERFACE
// The C (CINTERFACE) vtable form uses these Windows RPC macros for vtable layout; the CoreCLR
// PAL doesn't define them (they're only needed in the C-interface path). They are no-ops /
// `const` on any sane ABI.
#define BEGIN_INTERFACE
#define END_INTERFACE
#define CONST_VTBL const
#include "cor.h"
#include "corprof.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <unistd.h>

// IID_ICorProfilerInfo (28B5557D-3F3F-48b4-90B2-5F9EEA2F6C48) — to fetch SetEventMask.
static const GUID IID_Info = {0x28B5557D,0x3F3F,0x48b4,{0x90,0xB2,0x5F,0x9E,0xEA,0x2F,0x6C,0x48}};
// Interfaces our object actually implements (vtbl is ICorProfilerCallback4). QI MUST reject
// anything above Callback4 (else the CLR treats us as e.g. Callback8 and vcalls a slot past
// our vtable -> out-of-bounds -> crash). Callback3 is LOAD-BEARING here, not optional: the
// attach path QIs specifically for ICorProfilerCallback3 (InitializeForAttach lives there) and
// refuses to attach a profiler that does not answer it.
static const GUID IID_IUnknown_    = {0x00000000,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID IID_Callback     = {0x176FBED1,0xA55C,0x4796,{0x98,0xCA,0xA9,0xDA,0x0E,0xF8,0x83,0xE7}};
static const GUID IID_Callback2    = {0x8A8CC829,0xCCF2,0x49fe,{0xBB,0xAE,0x0F,0x02,0x22,0x28,0x07,0x1A}};
static const GUID IID_Callback3    = {0x4FD2ED52,0x7731,0x4b8d,{0x94,0x69,0x03,0xD2,0xCC,0x30,0x86,0xC5}};
static const GUID IID_Callback4    = {0x7B63B2E3,0x107D,0x4d48,{0xB2,0xF6,0xF6,0x1E,0x22,0x94,0x70,0xD2}};
static bool guid_eq(REFIID a, const GUID &b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }

// ---- the profiler object: an ICorProfilerCallback4 (C vtable) ---------------------------
static ICorProfilerCallback4Vtbl g_vtbl;
struct Prof { const ICorProfilerCallback4Vtbl *lpVtbl; long ref; ICorProfilerInfo *info; };
static Prof g_prof;
static unsigned long g_moved_calls;
static unsigned long g_moved_ranges;
// MovedReferences2 legitimately reports ranges with old == new (a compaction can leave the head of
// a segment in place). Those are useless to the F4 canonicalization, which exists precisely to
// follow objects whose address CHANGED — so count the RELOCATING ranges separately and sample one,
// otherwise "ranges delivered" could be a vacuous pass.
static unsigned long g_reloc_ranges;

static HRESULT STDMETHODCALLTYPE GenericStub(void) { return S_OK; }

static ULONG STDMETHODCALLTYPE Prof_AddRef(ICorProfilerCallback4 *This)  { return (ULONG)++((Prof*)This)->ref; }
static ULONG STDMETHODCALLTYPE Prof_Release(ICorProfilerCallback4 *This) { return (ULONG)--((Prof*)This)->ref; }
static HRESULT STDMETHODCALLTYPE Prof_QI(ICorProfilerCallback4 *This, REFIID riid, void **ppv) {
    // Accept IUnknown + ICorProfilerCallback..4 (our vtbl layout). REJECT Callback5+ so the
    // CLR falls back to Callback4 instead of vcalling past our vtable.
    if (guid_eq(riid, IID_IUnknown_) || guid_eq(riid, IID_Callback) ||
        guid_eq(riid, IID_Callback2) || guid_eq(riid, IID_Callback3) ||
        guid_eq(riid, IID_Callback4)) {
        *ppv = This; Prof_AddRef(This); return S_OK;
    }
    *ppv = NULL; return E_NOINTERFACE;
}

// Shared by both entry paths; `how` distinguishes them in the log.
static HRESULT set_mask(Prof *p, IUnknown *pInfoUnk, const char *how) {
    HRESULT hr = pInfoUnk->QueryInterface(IID_Info, (void**)&p->info);
    if (hr < 0 || p->info == NULL) {
        fprintf(stderr, "ATTACHPROF: %s QI(ICorProfilerInfo) FAILED hr=0x%08x\n", how, (unsigned)hr);
        fflush(stderr);
        return hr;
    }
    // COR_PRF_MONITOR_GC (0x80) is in COR_PRF_ALLOWABLE_AFTER_ATTACH, so this SHOULD succeed
    // post-attach. If it comes back 0x80131363 (CORPROF_E_UNSUPPORTED_FOR_ATTACHING_PROFILER)
    // that is the probe's kill criterion #2 — log the raw hr either way and let the driver judge.
    hr = p->info->lpVtbl->SetEventMask(p->info, COR_PRF_MONITOR_GC);
    fprintf(stderr, "ATTACHPROF: %s — SetEventMask(COR_PRF_MONITOR_GC=0x80) hr=0x%08x%s\n",
            how, (unsigned)hr, (hr < 0 ? "  <-- FAILED" : "  (S_OK)"));
    fflush(stderr);
    return hr;
}

// THE attach entry point. An attaching profiler gets THIS, never Initialize.
static HRESULT STDMETHODCALLTYPE Prof_InitializeForAttach(ICorProfilerCallback4 *This,
        IUnknown *pInfoUnk, void *pvClientData, UINT cbClientData) {
    fprintf(stderr, "ATTACHPROF: InitializeForAttach ENTERED pid=%d cbClientData=%u\n",
            (int)getpid(), (unsigned)cbClientData);
    fflush(stderr);
    (void)pvClientData;
    HRESULT hr = set_mask((Prof*)This, pInfoUnk, "InitializeForAttach");
    // Returning a failure here makes the CLR abandon the attach; propagate so a mask rejection
    // shows up as an attach failure rather than a silently deaf profiler.
    return hr;
}
static HRESULT STDMETHODCALLTYPE Prof_ProfilerAttachComplete(ICorProfilerCallback4 *This) {
    (void)This;
    fprintf(stderr, "ATTACHPROF: ProfilerAttachComplete — attach finished, callbacks live\n");
    fflush(stderr);
    return S_OK;
}
// Present only so a mis-wired lane (startup env vars) fails loudly instead of looking like attach.
static HRESULT STDMETHODCALLTYPE Prof_Initialize(ICorProfilerCallback4 *This, IUnknown *pInfoUnk) {
    fprintf(stderr, "ATTACHPROF: Initialize (STARTUP path — NOT the attach path)\n"); fflush(stderr);
    return set_mask((Prof*)This, pInfoUnk, "Initialize");
}

static HRESULT STDMETHODCALLTYPE Prof_MovedReferences2(ICorProfilerCallback4 *This, ULONG c,
        ObjectID oldS[], ObjectID newS[], SIZE_T len[]) {
    (void)This;
    g_moved_calls++;
    g_moved_ranges += (unsigned long)c;
    // Find the first range that actually RELOCATED (old != new) and count them all.
    ULONG reloc_here = 0, first = c;
    for (ULONG i = 0; i < c; i++)
        if (oldS[i] != newS[i]) { if (first == c) first = i; reloc_here++; }
    g_reloc_ranges += (unsigned long)reloc_here;
    fprintf(stderr, "ATTACHPROF: MovedReferences2 ranges=%lu relocating=%lu",
            (unsigned long)c, (unsigned long)reloc_here);
    if (first < c)
        fprintf(stderr, "  reloc[0]={old=0x%zx new=0x%zx len=%zu delta=%+ld}",
                (size_t)oldS[first], (size_t)newS[first], (size_t)len[first],
                (long)((intptr_t)newS[first] - (intptr_t)oldS[first]));
    else if (c > 0)
        fprintf(stderr, "  range[0]={old=0x%zx new=0x%zx len=%zu} (none relocated)",
                (size_t)oldS[0], (size_t)newS[0], (size_t)len[0]);
    fprintf(stderr, "  (call #%lu, %lu ranges / %lu relocating total)\n",
            g_moved_calls, g_moved_ranges, g_reloc_ranges);
    fflush(stderr);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Prof_GCFinished(ICorProfilerCallback4 *This) {
    (void)This; fprintf(stderr, "ATTACHPROF: GarbageCollectionFinished\n"); fflush(stderr); return S_OK;
}
static HRESULT STDMETHODCALLTYPE Prof_Shutdown(ICorProfilerCallback4 *This) {
    (void)This;
    fprintf(stderr, "ATTACHPROF: Shutdown (moved_calls=%lu moved_ranges=%lu reloc_ranges=%lu)\n",
            g_moved_calls, g_moved_ranges, g_reloc_ranges);
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
    g_vtbl.MovedReferences2 = Prof_MovedReferences2;
    g_vtbl.GarbageCollectionFinished = Prof_GCFinished;
    g_vtbl.Shutdown = Prof_Shutdown;
}

// ---- class factory + DllGetClassObject (the CLR entry point) -----------------------------
// IClassFactory is the PAL's C++ COM interface; implement it as a small C++ class (5 methods).
// Its CreateInstance hands back the C-vtable profiler object — the CLR's C++ vcall on that
// object works because the MIDL C vtable has the same layout as the C++ interface vtable.
struct Factory : public IClassFactory {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **ppv) override { *ppv = this; return S_OK; }
    ULONG   STDMETHODCALLTYPE AddRef() override  { return 1; }
    ULONG   STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *, REFIID, void **ppv) override {
        build_vtbl();
        g_prof.lpVtbl = &g_vtbl; g_prof.ref = 1; g_prof.info = NULL;
        *ppv = &g_prof;
        fprintf(stderr, "ATTACHPROF: CreateInstance — profiler object handed to CLR\n"); fflush(stderr);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};
static Factory g_factory;

extern "C" HRESULT STDMETHODCALLTYPE DllGetClassObject(REFCLSID, REFIID, void **ppv) {
    *ppv = static_cast<IClassFactory*>(&g_factory);
    fprintf(stderr, "ATTACHPROF: DllGetClassObject — profiler .so loaded into the RUNNING process\n");
    fflush(stderr);
    return S_OK;
}
