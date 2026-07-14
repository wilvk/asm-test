// gcprofiler.cpp — MINIMAL CLR profiler for the DR-vs-profiler GC-move coexistence PROBE
// (go/no-go for adopting ICorProfilerCallback4::MovedReferences2 to feed the DR taint
// client's shadow remap; see docs/internal/analysis/gc-move-range-extraction-findings.md).
//
// It does the smallest thing that answers the two questions:
//   (1) does a CLR profiler .so load + Initialize + get GC callbacks inside a dotnet process
//       that is ALSO running under DynamoRIO on Linux? and
//   (2) does MovedReferences2 actually deliver the per-range {old,new,len} triples?
// It logs to stderr; a driver greps for GCPROBE lines.
//
// Uses the CINTERFACE (C vtable) form of corprof.h so we can fill every unimplemented slot
// with a single generic S_OK stub (the array-fill trick) instead of hand-overriding ~90
// pure-virtual methods. Only QueryInterface/AddRef/Release/Initialize/MovedReferences2/
// GarbageCollectionFinished/Shutdown are real.
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
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "asmtest_taint_gcmove.h" // live GC-move handshake with the DR taint client (Increment 7)

// LIVE path (Increment 7): if the DR taint client published its gc_move entry (client option
// `gcmove` -> the shm handshake), drive at_gc_remap with every moved range so tags follow moved
// objects. When there is no handshake (the bare coexistence PROBE), this stays inert and the
// profiler only logs — so the go/no-go probe is unaffected.
typedef void (*gcmove_fn)(uint64_t /*old*/, uint64_t /*new*/, uint64_t /*len*/);
static gcmove_fn g_gc_move;
static void ensure_gcmove(void) {
    if (g_gc_move != nullptr)
        return;
    int fd = shm_open(AT_GCMOVE_SHM_NAME, O_RDONLY, 0600);
    if (fd < 0)
        return;
    at_gcmove_channel_t *ch = (at_gcmove_channel_t *)mmap(
        nullptr, sizeof *ch, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (ch == MAP_FAILED)
        return;
    if (ch->magic == AT_GCMOVE_MAGIC && ch->gc_move != 0) {
        g_gc_move = (gcmove_fn)(uintptr_t)ch->gc_move;
        fprintf(stderr, "GCPROBE: gc_move handshake found — driving live shadow remap\n");
        fflush(stderr);
    }
    munmap(ch, sizeof *ch);
}

// Our CLSID — the workload sets CORECLR_PROFILER to this. {A4B2C1D0-1111-2222-3333-444455556666}
// IID_ICorProfilerInfo (28B5557D-3F3F-48b4-90B2-5F9EEA2F6C48) — to fetch SetEventMask.
static const GUID IID_Info = {0x28B5557D,0x3F3F,0x48b4,{0x90,0xB2,0x5F,0x9E,0xEA,0x2F,0x6C,0x48}};
// Interfaces our object actually implements (vtbl is ICorProfilerCallback4). QI MUST reject
// anything above Callback4 (else the CLR treats us as e.g. Callback8 and vcalls a slot past
// our vtable -> out-of-bounds -> crash).
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
static HRESULT STDMETHODCALLTYPE Prof_Initialize(ICorProfilerCallback4 *This, IUnknown *pInfoUnk) {
    Prof *p = (Prof*)This;
    // pInfoUnk is the PAL's C++ IUnknown; QI it (C++ call) for the C-vtable ICorProfilerInfo.
    HRESULT hr = pInfoUnk->QueryInterface(IID_Info, (void**)&p->info);
    if (hr < 0 || p->info == NULL) {
        fprintf(stderr, "GCPROBE: Initialize QI(ICorProfilerInfo) FAILED hr=0x%08x\n", (unsigned)hr);
        return hr;
    }
    hr = p->info->lpVtbl->SetEventMask(p->info, COR_PRF_MONITOR_GC);
    fprintf(stderr, "GCPROBE: Initialize OK — SetEventMask(COR_PRF_MONITOR_GC=0x80) hr=0x%08x\n", (unsigned)hr);
    fflush(stderr);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Prof_MovedReferences2(ICorProfilerCallback4 *This, ULONG c,
        ObjectID oldS[], ObjectID newS[], SIZE_T len[]) {
    (void)This;
    fprintf(stderr, "GCPROBE: MovedReferences2 ranges=%lu", (unsigned long)c);
    if (c > 0)
        fprintf(stderr, "  range[0]={old=0x%zx new=0x%zx len=%zu}",
                (size_t)oldS[0], (size_t)newS[0], (size_t)len[0]);
    fprintf(stderr, "\n"); fflush(stderr);
    // LIVE path: feed each moved range to the DR taint client's shadow remap (if published).
    // The runtime is suspended here, so this is the natural quiesce fence for the bulk remap.
    ensure_gcmove();
    if (g_gc_move != nullptr)
        for (ULONG i = 0; i < c; i++)
            g_gc_move((uint64_t)oldS[i], (uint64_t)newS[i], (uint64_t)len[i]);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Prof_GCFinished(ICorProfilerCallback4 *This) {
    (void)This; fprintf(stderr, "GCPROBE: GarbageCollectionFinished\n"); fflush(stderr); return S_OK;
}
static HRESULT STDMETHODCALLTYPE Prof_Shutdown(ICorProfilerCallback4 *This) {
    (void)This; fprintf(stderr, "GCPROBE: Shutdown\n"); fflush(stderr); return S_OK;
}

static void build_vtbl(void) {
    void **slots = (void**)&g_vtbl;
    size_t n = sizeof(g_vtbl) / sizeof(void*);
    for (size_t i = 0; i < n; i++) slots[i] = (void*)GenericStub;
    g_vtbl.QueryInterface = Prof_QI;
    g_vtbl.AddRef = Prof_AddRef;
    g_vtbl.Release = Prof_Release;
    g_vtbl.Initialize = Prof_Initialize;
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
        fprintf(stderr, "GCPROBE: CreateInstance — profiler object handed to CLR\n"); fflush(stderr);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};
static Factory g_factory;

extern "C" HRESULT STDMETHODCALLTYPE DllGetClassObject(REFCLSID, REFIID, void **ppv) {
    *ppv = static_cast<IClassFactory*>(&g_factory);
    fprintf(stderr, "GCPROBE: DllGetClassObject — profiler .so loaded by CLR\n"); fflush(stderr);
    return S_OK;
}
