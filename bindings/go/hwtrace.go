// Hardware-tier native runtime tracing for Go (single-step / Intel PT / AMD).
//
// This is the Go counterpart of the Python wrapper (bindings/python/asmtest/
// hwtrace.py) and the C API in include/asmtest_hwtrace.h. Like the DynamoRIO tier
// (drtrace.go) it traces host-native code as it runs **inside this Go process** —
// initialize once, materialize host-native machine code, mark a region, call into
// it, and read back basic-block coverage / the instruction stream — but it
// observes the **real CPU** and, unlike the DynamoRIO wrapper, needs no DynamoRIO
// install.
//
// Four backends share one API, selected by enum. SINGLESTEP (EFLAGS.TF #DB ->
// SIGTRAP) is the portable default: exact and complete on ANY x86-64 Linux
// (Intel, any-Zen AMD, VM, CI, container) with no PMU, no perf_event, no
// privilege, and no decoder library — so it is what this binding's self-test
// exercises live. INTEL_PT / CORESIGHT / AMD_LBR are hardware branch-trace
// backends that self-skip off the specific bare-metal hardware they need.
//
// The hardware-trace tier lives in its own shared library, libasmtest_hwtrace,
// which is built separately (`make shared-hwtrace`) and so may be absent. Exactly
// like the DynamoRIO wrapper, this file therefore does NOT link the lib: it
// dlopen()s it at RUN TIME — from $ASMTEST_HWTRACE_LIB, else
// <cwd>/build/libasmtest_hwtrace.so — and dlsym()s the entry points into static
// function pointers. If the lib can't be resolved, HwTraceAvailable() returns
// false and the whole tier self-skips cleanly.
//
// libasmtest_hwtrace re-exports the trace handle + accessors (asmtest_trace_new
// and friends) from the same trace.o the emulator uses, so coverage is read back
// in the identical {Covered, BlocksLen, InsnsTotal} shape.
package asmtest

/*
#cgo linux LDFLAGS: -ldl
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <dlfcn.h>

// The one struct the API takes by pointer, redefined here so this file needs no
// header on the cgo include path (mirrors asmtest_hwtrace_options_t from
// include/asmtest_hwtrace.h). ASMTEST_HW_OK == 0. Default backend SINGLESTEP=3.
typedef struct {
    int backend;
    size_t aux_size;
    size_t data_size;
    int snapshot;
    const char *object_hint;
} asmtest_hwtrace_options_t;

// The cross-tier orchestrator's resolved-choice struct, redefined here (mirrors
// asmtest_trace_choice_t from include/asmtest_trace_auto.h): exactly three
// int-sized enum fields, no padding, so it marshals as three consecutive C ints.
typedef struct {
    int tier;
    int backend;
    int fidelity;
} asmtest_trace_choice_t;

// Entry-point typedefs (one per exported symbol in libasmtest_hwtrace).
typedef int  (*hw_available_fn)(int);
typedef void (*hw_skip_reason_fn)(int, char *, size_t);
typedef size_t (*hw_resolve_fn)(int, int *, size_t);
typedef int  (*hw_auto_fn)(int);
// Cross-tier orchestrator entry points (asmtest_trace_auto.h), from the SAME lib.
typedef size_t (*hw_trace_resolve_fn)(unsigned, asmtest_trace_choice_t *, size_t);
typedef int    (*hw_trace_auto_fn)(unsigned, asmtest_trace_choice_t *);
typedef int  (*hw_init_fn)(const asmtest_hwtrace_options_t *);
typedef int  (*hw_register_fn)(const char *, void *, size_t, void *);
typedef void (*hw_marker_fn)(const char *);
typedef void (*hw_shutdown_fn)(void);
typedef int  (*hw_exec_alloc_fn)(const void *, size_t, void **, size_t *);
typedef void (*hw_exec_free_fn)(void *, size_t);
typedef void *(*hw_trace_new_fn)(size_t, size_t);
typedef void  (*hw_trace_free_fn)(void *);
typedef int   (*hw_trace_covered_fn)(void *, uint64_t);
typedef unsigned long long (*hw_trace_blocks_len_fn)(void *);
typedef unsigned long long (*hw_trace_insns_total_fn)(void *);
typedef unsigned long long (*hw_trace_insns_len_fn)(void *);
typedef int                (*hw_trace_truncated_fn)(void *);
typedef unsigned long long (*hw_trace_block_at_fn)(void *, size_t);
typedef unsigned long long (*hw_trace_insn_at_fn)(void *, size_t);

// The jitdump entry struct, redefined here (mirrors asmtest_jitdump_entry_t from
// include/asmtest_ptrace.h): exactly four uint64 fields, no padding, so it
// marshals as four consecutive little-endian u64s — a JIT method's load address,
// size, timestamp, and the JIT's unique index.
typedef struct {
    uint64_t code_addr;
    uint64_t code_size;
    uint64_t timestamp;
    uint64_t code_index;
} asmtest_jitdump_entry_t;

// asmtest_ptrace.h — out-of-process / foreign-process tracing toolkit, from the
// SAME libasmtest_hwtrace. One typedef per exported symbol.
typedef int  (*pt_available_fn)(void);
typedef void (*pt_skip_reason_fn)(char *, size_t);
typedef int  (*pt_trace_call_fn)(const void *, size_t, const long *, int, long *, void *);
typedef int  (*pt_trace_attached_fn)(int, const void *, size_t, long *, void *);
typedef int  (*proc_region_by_addr_fn)(int, const void *, void **, size_t *);
typedef int  (*proc_perfmap_symbol_fn)(int, const char *, void **, size_t *);
typedef int  (*jitdump_find_fn)(const char *, int, const char *, asmtest_jitdump_entry_t *, uint8_t *, size_t, size_t *);

static hw_available_fn        p_hw_available;
static hw_skip_reason_fn      p_hw_skip_reason;
static hw_resolve_fn          p_hw_resolve;
static hw_auto_fn             p_hw_auto;
static hw_trace_resolve_fn    p_trace_resolve;
static hw_trace_auto_fn       p_trace_auto;
static hw_init_fn             p_hw_init;
static hw_register_fn         p_hw_register;
static hw_marker_fn           p_hw_begin;
static hw_marker_fn           p_hw_end;
static hw_shutdown_fn         p_hw_shutdown;
static hw_exec_alloc_fn       p_hw_exec_alloc;
static hw_exec_free_fn        p_hw_exec_free;
static hw_trace_new_fn        p_hw_trace_new;
static hw_trace_free_fn       p_hw_trace_free;
static hw_trace_covered_fn    p_hw_trace_covered;
static hw_trace_blocks_len_fn p_hw_trace_blocks_len;
static hw_trace_insns_total_fn p_hw_trace_insns_total;
static hw_trace_insns_len_fn  p_hw_trace_insns_len;
static hw_trace_truncated_fn  p_hw_trace_truncated;
static hw_trace_block_at_fn   p_hw_trace_block_at;
static hw_trace_insn_at_fn    p_hw_trace_insn_at;
// asmtest_ptrace.h — out-of-process / foreign-process tracing toolkit.
static pt_available_fn        p_pt_available;
static pt_skip_reason_fn      p_pt_skip_reason;
static pt_trace_call_fn       p_pt_trace_call;
static pt_trace_attached_fn   p_pt_trace_attached;
static proc_region_by_addr_fn p_proc_region_by_addr;
static proc_perfmap_symbol_fn p_proc_perfmap_symbol;
static jitdump_find_fn        p_jitdump_find;

static int g_hw_loaded;   // 1 once dlopen + every dlsym succeeded.

// Resolve libasmtest_hwtrace from $ASMTEST_HWTRACE_LIB, else
// build/libasmtest_hwtrace.so (relative to the cwd, like the Python loader's last
// candidate). dlopen it RTLD_GLOBAL so the trampolines below find the symbols,
// then dlsym each entry point. On any miss the tier stays unloaded and
// asmtest_hw_is_loaded() is 0, so the binding self-skips rather than crashing.
// Idempotent.
static void asmtest_hw_resolve(void) {
    if (g_hw_loaded) return;
    const char *env = getenv("ASMTEST_HWTRACE_LIB");
    void *h = dlopen(env && *env ? env : "build/libasmtest_hwtrace.so",
                     RTLD_NOW | RTLD_GLOBAL);
    if (!h) return;
    p_hw_available       = (hw_available_fn)dlsym(h, "asmtest_hwtrace_available");
    p_hw_skip_reason     = (hw_skip_reason_fn)dlsym(h, "asmtest_hwtrace_skip_reason");
    p_hw_resolve         = (hw_resolve_fn)dlsym(h, "asmtest_hwtrace_resolve");
    p_hw_auto            = (hw_auto_fn)dlsym(h, "asmtest_hwtrace_auto");
    p_trace_resolve      = (hw_trace_resolve_fn)dlsym(h, "asmtest_trace_resolve");
    p_trace_auto         = (hw_trace_auto_fn)dlsym(h, "asmtest_trace_auto");
    p_hw_init            = (hw_init_fn)dlsym(h, "asmtest_hwtrace_init");
    p_hw_register        = (hw_register_fn)dlsym(h, "asmtest_hwtrace_register_region");
    p_hw_begin           = (hw_marker_fn)dlsym(h, "asmtest_hwtrace_begin");
    p_hw_end             = (hw_marker_fn)dlsym(h, "asmtest_hwtrace_end");
    p_hw_shutdown        = (hw_shutdown_fn)dlsym(h, "asmtest_hwtrace_shutdown");
    p_hw_exec_alloc      = (hw_exec_alloc_fn)dlsym(h, "asmtest_hwtrace_exec_alloc");
    p_hw_exec_free       = (hw_exec_free_fn)dlsym(h, "asmtest_hwtrace_exec_free");
    p_hw_trace_new       = (hw_trace_new_fn)dlsym(h, "asmtest_trace_new");
    p_hw_trace_free      = (hw_trace_free_fn)dlsym(h, "asmtest_trace_free");
    p_hw_trace_covered   = (hw_trace_covered_fn)dlsym(h, "asmtest_trace_covered");
    p_hw_trace_blocks_len  = (hw_trace_blocks_len_fn)dlsym(h, "asmtest_emu_trace_blocks_len");
    p_hw_trace_insns_total = (hw_trace_insns_total_fn)dlsym(h, "asmtest_emu_trace_insns_total");
    p_hw_trace_insns_len   = (hw_trace_insns_len_fn)dlsym(h, "asmtest_emu_trace_insns_len");
    p_hw_trace_truncated   = (hw_trace_truncated_fn)dlsym(h, "asmtest_emu_trace_truncated");
    p_hw_trace_block_at    = (hw_trace_block_at_fn)dlsym(h, "asmtest_emu_trace_block_at");
    p_hw_trace_insn_at     = (hw_trace_insn_at_fn)dlsym(h, "asmtest_emu_trace_insn_at");
    // asmtest_ptrace.h — resolved from the same already-loaded handle.
    p_pt_available         = (pt_available_fn)dlsym(h, "asmtest_ptrace_available");
    p_pt_skip_reason       = (pt_skip_reason_fn)dlsym(h, "asmtest_ptrace_skip_reason");
    p_pt_trace_call        = (pt_trace_call_fn)dlsym(h, "asmtest_ptrace_trace_call");
    p_pt_trace_attached    = (pt_trace_attached_fn)dlsym(h, "asmtest_ptrace_trace_attached");
    p_proc_region_by_addr  = (proc_region_by_addr_fn)dlsym(h, "asmtest_proc_region_by_addr");
    p_proc_perfmap_symbol  = (proc_perfmap_symbol_fn)dlsym(h, "asmtest_proc_perfmap_symbol");
    p_jitdump_find         = (jitdump_find_fn)dlsym(h, "asmtest_jitdump_find");
    g_hw_loaded = p_hw_available && p_hw_skip_reason && p_hw_resolve &&
                  p_hw_auto && p_trace_resolve && p_trace_auto && p_hw_init &&
                  p_hw_register && p_hw_begin && p_hw_end && p_hw_shutdown &&
                  p_hw_exec_alloc && p_hw_exec_free && p_hw_trace_new &&
                  p_hw_trace_free && p_hw_trace_covered && p_hw_trace_blocks_len &&
                  p_hw_trace_insns_total && p_hw_trace_insns_len &&
                  p_hw_trace_truncated && p_hw_trace_block_at && p_hw_trace_insn_at &&
                  p_pt_available && p_pt_skip_reason && p_pt_trace_call &&
                  p_pt_trace_attached && p_proc_region_by_addr &&
                  p_proc_perfmap_symbol && p_jitdump_find;
}

static int asmtest_hw_is_loaded(void) { return g_hw_loaded; }

// static wrappers Go calls — each guards the (already-resolved) pointer so a
// partial load can never dereference NULL.
static int  asmtest_hw_go_available(int backend) { return p_hw_available ? p_hw_available(backend) : 0; }
static void asmtest_hw_go_skip_reason(int backend, char *buf, size_t buflen) {
    if (p_hw_skip_reason) p_hw_skip_reason(backend, buf, buflen);
    else if (buflen) buf[0] = 0;
}
static size_t asmtest_hw_go_resolve(int policy, int *out, size_t cap) {
    return p_hw_resolve ? p_hw_resolve(policy, out, cap) : 0;
}
static int  asmtest_hw_go_auto(int policy) {
    return p_hw_auto ? p_hw_auto(policy) : -3; // ASMTEST_HW_EUNAVAIL
}
// Cross-tier orchestrator bridges (asmtest_trace_auto.h).
static size_t asmtest_go_trace_resolve(unsigned policy, asmtest_trace_choice_t *out, size_t cap) {
    return p_trace_resolve ? p_trace_resolve(policy, out, cap) : 0;
}
static int  asmtest_go_trace_auto(unsigned policy, asmtest_trace_choice_t *out) {
    return p_trace_auto ? p_trace_auto(policy, out) : -3; // ASMTEST_HW_EUNAVAIL
}
static int  asmtest_hw_go_init(int backend) {
    asmtest_hwtrace_options_t o;
    o.backend = backend;
    o.aux_size = 0;
    o.data_size = 0;
    o.snapshot = 0;
    o.object_hint = NULL;
    return p_hw_init ? p_hw_init(&o) : -1;
}
static int  asmtest_hw_go_register(const char *name, void *base, size_t len, void *trace) {
    return p_hw_register ? p_hw_register(name, base, len, trace) : -1;
}
static void asmtest_hw_go_begin(const char *name) { if (p_hw_begin) p_hw_begin(name); }
static void asmtest_hw_go_end(const char *name)   { if (p_hw_end) p_hw_end(name); }
static void asmtest_hw_go_shutdown(void) { if (p_hw_shutdown) p_hw_shutdown(); }
static int  asmtest_hw_go_exec_alloc(const void *bytes, size_t len, void **base_out, size_t *len_out) {
    return p_hw_exec_alloc ? p_hw_exec_alloc(bytes, len, base_out, len_out) : -1;
}
static void asmtest_hw_go_exec_free(void *base, size_t len) { if (p_hw_exec_free) p_hw_exec_free(base, len); }
// NOTE: insns capacity FIRST, blocks SECOND (matches asmtest_trace_new's signature).
static void *asmtest_hw_go_trace_new(size_t insns, size_t blocks) {
    return p_hw_trace_new ? p_hw_trace_new(insns, blocks) : NULL;
}
static void  asmtest_hw_go_trace_free(void *t) { if (p_hw_trace_free) p_hw_trace_free(t); }
static int   asmtest_hw_go_trace_covered(void *t, uint64_t off) {
    return p_hw_trace_covered ? p_hw_trace_covered(t, off) : 0;
}
static unsigned long long asmtest_hw_go_blocks_len(void *t) {
    return p_hw_trace_blocks_len ? p_hw_trace_blocks_len(t) : 0;
}
static unsigned long long asmtest_hw_go_insns_total(void *t) {
    return p_hw_trace_insns_total ? p_hw_trace_insns_total(t) : 0;
}
static unsigned long long asmtest_hw_go_insns_len(void *t) {
    return p_hw_trace_insns_len ? p_hw_trace_insns_len(t) : 0;
}
static int asmtest_hw_go_truncated(void *t) {
    return p_hw_trace_truncated ? p_hw_trace_truncated(t) : 0;
}
static unsigned long long asmtest_hw_go_block_at(void *t, size_t i) {
    return p_hw_trace_block_at ? p_hw_trace_block_at(t, i) : 0;
}
static unsigned long long asmtest_hw_go_insn_at(void *t, size_t i) {
    return p_hw_trace_insn_at ? p_hw_trace_insn_at(t, i) : 0;
}

// Trampoline that invokes the generated host-native code through a function
// pointer under the SysV integer ABI (two long args -> long result).
static long asmtest_hw_call2(void *p, long a, long b) {
    return ((long (*)(long, long))p)(a, b);
}

// asmtest_ptrace.h bridges — each NULL-guards its (already-resolved) pointer so a
// partial load can never dereference NULL.
static int asmtest_go_pt_available(void) { return p_pt_available ? p_pt_available() : 0; }
static void asmtest_go_pt_skip_reason(char *buf, size_t buflen) {
    if (p_pt_skip_reason) p_pt_skip_reason(buf, buflen);
    else if (buflen) buf[0] = 0;
}
static int asmtest_go_pt_trace_call(const void *code, size_t len, const long *args,
                                    int nargs, long *result, void *trace) {
    return p_pt_trace_call ? p_pt_trace_call(code, len, args, nargs, result, trace) : -1;
}
static int asmtest_go_pt_trace_attached(int pid, const void *base, size_t len,
                                        long *result, void *trace) {
    return p_pt_trace_attached ? p_pt_trace_attached(pid, base, len, result, trace) : -1;
}
static int asmtest_go_proc_region_by_addr(int pid, const void *addr,
                                          void **base_out, size_t *len_out) {
    return p_proc_region_by_addr ? p_proc_region_by_addr(pid, addr, base_out, len_out) : -1;
}
static int asmtest_go_proc_perfmap_symbol(int pid, const char *name,
                                          void **base_out, size_t *len_out) {
    return p_proc_perfmap_symbol ? p_proc_perfmap_symbol(pid, name, base_out, len_out) : -1;
}
static int asmtest_go_jitdump_find(const char *path, int pid, const char *name,
                                   asmtest_jitdump_entry_t *out, uint8_t *bytes_out,
                                   size_t bytes_cap, size_t *bytes_len) {
    return p_jitdump_find ? p_jitdump_find(path, pid, name, out, bytes_out, bytes_cap, bytes_len) : -1;
}
*/
import "C"

import (
	"fmt"
	"unsafe"
)

// hwOK is the success status returned by the lifecycle / registration calls
// (mirrors the C macro ASMTEST_HW_OK).
const hwOK = 0

// HwEUnavail is the status HwTraceAuto returns when no hardware-trace backend is
// available on this host (mirrors the C macro ASMTEST_HW_EUNAVAIL).
const HwEUnavail = -3

// asmtest_trace_backend_t — the four hardware-trace backends. SINGLESTEP is the
// portable default that runs on any x86-64 Linux.
const (
	IntelPT    = 0
	CoreSight  = 1
	AmdLBR     = 2
	SingleStep = 3
)

// asmtest_hwtrace_policy_t — the backend auto-selection policy for HwTraceResolve
// / HwTraceAuto. BEST is the most faithful available backend; CEILING_FREE is the
// same but skips the one fixed-window backend (AMD LBR) — re-resolve under it
// after a trace comes back truncated.
const (
	Best        = 0
	CeilingFree = 1
)

// asmtest_trace_tier_t — the CROSS-TIER orchestrator's trace tiers (over the
// hardware + DynamoRIO + emulator tiers), most-faithful to least. See
// include/asmtest_trace_auto.h and the Python wrapper's resolve_tiers/auto_tier.
const (
	TierHwtrace   = 0 // HW branch trace / single-step (real CPU)
	TierDynamoRIO = 1 // in-process software DBI (real CPU)
	TierEmulator  = 2 // Unicorn virtual CPU (isolated guest)
)

// asmtest_trace_fidelity_t — execution fidelity of a resolved tier choice.
const (
	FidelityNative  = 0 // runs the real bytes on the real CPU in-process
	FidelityVirtual = 1 // isolated guest on an emulated CPU
)

// Cross-tier policy bitmask for ResolveTiers / AutoTier. TraceBest is the most-
// faithful available (emulator floor allowed); TraceCeilingFree drops the one
// fixed-window backend (AMD LBR); TraceNativeOnly forbids the native->emulator
// fidelity crossing (drops the emulator floor).
const (
	TraceBest        = 0x0
	TraceCeilingFree = 0x1
	TraceNativeOnly  = 0x2
)

// Resolve the optional hardware-trace tier the first time the package loads,
// exactly as drtrace.go resolves the DynamoRIO tier. A miss is silent —
// HwTraceAvailable() reports it.
func init() { C.asmtest_hw_resolve() }

// HwTraceAvailable reports whether the chosen backend can run on this host: the
// libasmtest_hwtrace lib loaded AND the backend's full detect-and-skip chain
// passes (asmtest_hwtrace_available(backend) == 1). It never panics, so callers
// (and the test) self-skip cleanly when the lib or the hardware is absent.
func HwTraceAvailable(backend int) bool {
	return C.asmtest_hw_is_loaded() != 0 && C.asmtest_hw_go_available(C.int(backend)) != 0
}

// HwTraceSkipReason is a human-readable reason HwTraceAvailable(backend) is false
// (or "available"). Useful for the self-skip message.
func HwTraceSkipReason(backend int) string {
	if C.asmtest_hw_is_loaded() == 0 {
		return "libasmtest_hwtrace not loaded (set ASMTEST_HWTRACE_LIB or build with `make shared-hwtrace`)"
	}
	buf := make([]byte, 160)
	C.asmtest_hw_go_skip_reason(C.int(backend), (*C.char)(unsafe.Pointer(&buf[0])), C.size_t(len(buf)))
	return C.GoString((*C.char)(unsafe.Pointer(&buf[0])))
}

// HwTraceResolve is this host's hardware-trace fallback cascade: the available
// backends, most-faithful first (IntelPT > AmdLBR > SingleStep > CoreSight),
// honoring policy. Empty only off x86-64 Linux (single-step is the floor there)
// or when libasmtest_hwtrace is not loaded. CeilingFree drops the depth-bounded
// backend (AMD LBR).
func HwTraceResolve(policy int) []int {
	var out [4]C.int
	n := C.asmtest_hw_go_resolve(C.int(policy), &out[0], C.size_t(len(out)))
	bes := make([]int, int(n))
	for i := range bes {
		bes[i] = int(out[i])
	}
	return bes
}

// HwTraceAuto is the single most-preferred available backend under policy (a
// backend enum >= 0, ready to pass to HwTraceInit), or HwEUnavail (-3) when no
// hardware-trace backend is available on this host.
func HwTraceAuto(policy int) int {
	return int(C.asmtest_hw_go_auto(C.int(policy)))
}

// TierChoice is one resolved cross-tier trace option: which Tier to use, which
// hardware Backend within it (meaningful only when Tier == TierHwtrace), and the
// Fidelity class (FidelityNative vs FidelityVirtual). Mirrors
// asmtest_trace_choice_t / the Python wrapper's TierChoice.
type TierChoice struct {
	Tier     int
	Backend  int
	Fidelity int
}

// ResolveTiers is this host's full CROSS-TIER cascade (asmtest_trace_resolve),
// most-faithful first: Intel PT -> AMD LBR -> DynamoRIO -> single-step ->
// CoreSight -> emulator, each included only if its tier is available, honoring
// policy. The emulator (TierEmulator, FidelityVirtual) is the universal floor and
// the last entry under TraceBest. TraceNativeOnly drops that floor (no
// native->emulator crossing); TraceCeilingFree drops AMD LBR. Empty only off a
// native host under TraceNativeOnly, or when libasmtest_hwtrace is not loaded.
func ResolveTiers(policy int) []TierChoice {
	var out [8]C.asmtest_trace_choice_t
	n := C.asmtest_go_trace_resolve(C.uint(policy), &out[0], C.size_t(len(out)))
	cs := make([]TierChoice, int(n))
	for i := range cs {
		cs[i] = TierChoice{
			Tier:     int(out[i].tier),
			Backend:  int(out[i].backend),
			Fidelity: int(out[i].fidelity),
		}
	}
	return cs
}

// AutoTier is the single most-preferred available cross-tier choice under policy
// (asmtest_trace_auto). The bool is false (meaning EUNAVAIL) when the cascade is
// empty — only off a native host under TraceNativeOnly, or when
// libasmtest_hwtrace is not loaded.
func AutoTier(policy int) (TierChoice, bool) {
	var c C.asmtest_trace_choice_t
	if rc := C.asmtest_go_trace_auto(C.uint(policy), &c); rc != hwOK {
		return TierChoice{}, false
	}
	return TierChoice{
		Tier:     int(c.tier),
		Backend:  int(c.backend),
		Fidelity: int(c.fidelity),
	}, true
}

// HwTraceInit selects a backend and initializes the tier. SingleStep is the
// portable default that runs on any x86-64 Linux.
func HwTraceInit(backend int) error {
	if rc := C.asmtest_hw_go_init(C.int(backend)); rc != hwOK {
		return fmt.Errorf("asmtest_hwtrace_init failed: %d", int(rc))
	}
	return nil
}

// HwTraceShutdown tears the tier down and returns it to the uninitialized state.
// Safe to call after a failed init.
func HwTraceShutdown() { C.asmtest_hw_go_shutdown() }

// HwNativeCode is host-native machine code in real executable (W^X) memory,
// materialized at its runtime address so PC-relative and branch targets resolve.
// It is the hardware-tier counterpart of drtrace.go's NativeCode (a distinct type
// because the two tiers own their executable memory through different allocators —
// here asmtest_hwtrace_exec_alloc / _free). Release it with Free.
type HwNativeCode struct {
	base unsafe.Pointer
	len  C.size_t
	free bool
}

// HwNativeCodeFromBytes maps executable memory and copies the host-native machine
// code bytes into it (offset 0 is the entry point), via asmtest_hwtrace_exec_alloc.
func HwNativeCodeFromBytes(b []byte) (*HwNativeCode, error) {
	if C.asmtest_hw_is_loaded() == 0 {
		return nil, fmt.Errorf("hardware-trace tier unavailable")
	}
	nc := &HwNativeCode{}
	var p unsafe.Pointer
	if len(b) > 0 {
		p = unsafe.Pointer(&b[0])
	}
	if rc := C.asmtest_hw_go_exec_alloc(p, C.size_t(len(b)), &nc.base, &nc.len); rc != hwOK {
		return nil, fmt.Errorf("asmtest_hwtrace_exec_alloc failed: %d", int(rc))
	}
	return nc, nil
}

// Base is the runtime address of the executable mapping (offset 0 = entry).
func (c *HwNativeCode) Base() uintptr { return uintptr(c.base) }

// Len is the number of code bytes.
func (c *HwNativeCode) Len() int { return int(c.len) }

// Call invokes the code through a function pointer under the SysV integer ABI:
// two long args, a long result.
func (c *HwNativeCode) Call(a, b int64) int64 {
	return int64(C.asmtest_hw_call2(c.base, C.long(a), C.long(b)))
}

// Free unmaps the executable memory. Safe to call more than once.
func (c *HwNativeCode) Free() {
	if !c.free {
		C.asmtest_hw_go_exec_free(c.base, c.len)
		c.free = true
	}
}

// HwTrace is an app-owned coverage recorder for a registered native region,
// reading back basic-block coverage / the ordered instruction stream as the
// region runs under the hardware tier. Release it with Free.
type HwTrace struct{ h unsafe.Pointer }

// NewHwTrace allocates a trace handle. instructions > 0 records the ordered
// instruction stream; blocks > 0 records basic-block coverage. (The underlying
// asmtest_trace_new takes insns first, blocks second; this constructor takes
// blocks first to mirror the Python wrapper's new(blocks=, instructions=).)
func NewHwTrace(blocks, instructions int) *HwTrace {
	return &HwTrace{h: C.asmtest_hw_go_trace_new(C.size_t(instructions), C.size_t(blocks))}
}

// Register records a non-overlapping native code range under name, recording
// coverage into this trace. The name may be freed after the call (it is copied).
func (t *HwTrace) Register(name string, code *HwNativeCode) error {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	rc := C.asmtest_hw_go_register(cs, code.base, code.len, t.h)
	if rc != hwOK {
		return fmt.Errorf("register_region(%q) failed: %d", name, int(rc))
	}
	return nil
}

// Begin opens hardware AUX capture for the named region. Markers must be balanced
// with End — prefer Region, which balances them for you. (MVP: only one region
// may be active at a time.)
func (t *HwTrace) Begin(name string) {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	C.asmtest_hw_go_begin(cs)
}

// End closes capture for the named region and decodes the captured trace.
func (t *HwTrace) End(name string) {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	C.asmtest_hw_go_end(cs)
}

// Region runs fn between a balanced Begin(name)/End(name) — the Go idiom for the
// scoped marker (Go has no RAII, so a func wrapper replaces Python's `with`). End
// runs even if fn panics.
func (t *HwTrace) Region(name string, fn func()) {
	t.Begin(name)
	defer t.End(name)
	fn()
}

// Covered reports whether the basic block at byte-offset off (from the region
// entry) was entered.
func (t *HwTrace) Covered(off uint64) bool {
	return C.asmtest_hw_go_trace_covered(t.h, C.uint64_t(off)) != 0
}

// BlocksLen is the number of distinct basic blocks recorded.
func (t *HwTrace) BlocksLen() uint64 { return uint64(C.asmtest_hw_go_blocks_len(t.h)) }

// InsnsTotal is the number of instructions executed (counts past the buffer cap).
func (t *HwTrace) InsnsTotal() uint64 { return uint64(C.asmtest_hw_go_insns_total(t.h)) }

// InsnsLen is the number of instruction offsets actually stored (up to the
// trace's instruction capacity; may be smaller than InsnsTotal).
func (t *HwTrace) InsnsLen() uint64 { return uint64(C.asmtest_hw_go_insns_len(t.h)) }

// Truncated reports whether the instruction stream overflowed its capacity (some
// executed offsets were not stored).
func (t *HwTrace) Truncated() bool { return C.asmtest_hw_go_truncated(t.h) != 0 }

// BlockOffsets is the distinct basic-block start offsets recorded, in first-seen
// order.
func (t *HwTrace) BlockOffsets() []uint64 {
	n := uint64(C.asmtest_hw_go_blocks_len(t.h))
	offs := make([]uint64, n)
	for i := uint64(0); i < n; i++ {
		offs[i] = uint64(C.asmtest_hw_go_block_at(t.h, C.size_t(i)))
	}
	return offs
}

// InsnOffsets is the ordered instruction-offset stream actually stored — each
// executed instruction's offset in execution order, up to the trace's insns
// capacity (InsnsLen, not the possibly-larger InsnsTotal).
func (t *HwTrace) InsnOffsets() []uint64 {
	n := uint64(C.asmtest_hw_go_insns_len(t.h))
	offs := make([]uint64, n)
	for i := uint64(0); i < n; i++ {
		offs[i] = uint64(C.asmtest_hw_go_insn_at(t.h, C.size_t(i)))
	}
	return offs
}

// Free releases the trace handle. Safe to call more than once.
func (t *HwTrace) Free() {
	if t.h != nil {
		C.asmtest_hw_go_trace_free(t.h)
		t.h = nil
	}
}

// ---- Out-of-process / foreign-process tracing (asmtest_ptrace.h) ----
//
// Single-step a forked or externally-attached target OUT OF BAND, and resolve
// the code region to trace from the OS — /proc/<pid>/maps, a JIT perf-map, or a
// binary jitdump. The managed-runtime path (JVM/.NET/Node on AMD, where Intel PT
// is unavailable and in-process DynamoRIO cannot seize the runtime's threads).
// Linux x86-64. Mirrors the Python wrapper's Ptrace class. These share the same
// already-dlopen'd libasmtest_hwtrace as the hardware-trace surface above.

// ptraceOK is the success status returned by the ptrace toolkit calls (mirrors
// the C macro ASMTEST_PTRACE_OK).
const ptraceOK = 0

// PtraceENOENT is the status meaning a region / symbol / method was not found
// (mirrors the C macro ASMTEST_PTRACE_ENOENT).
const PtraceENOENT = -7

// PtraceAvailable reports whether the out-of-process single-step tracer can run
// on this host (Linux x86-64) AND libasmtest_hwtrace loaded. It never panics, so
// callers (and the test) self-skip cleanly when the lib or the host is unfit.
func PtraceAvailable() bool {
	return C.asmtest_hw_is_loaded() != 0 && C.asmtest_go_pt_available() != 0
}

// PtraceSkipReason is a human-readable reason PtraceAvailable() is false (or
// "available"). Useful for the self-skip message.
func PtraceSkipReason() string {
	if C.asmtest_hw_is_loaded() == 0 {
		return "libasmtest_hwtrace not loaded (set ASMTEST_HWTRACE_LIB or build with `make shared-hwtrace`)"
	}
	buf := make([]byte, 160)
	C.asmtest_go_pt_skip_reason((*C.char)(unsafe.Pointer(&buf[0])), C.size_t(len(buf)))
	return C.GoString((*C.char)(unsafe.Pointer(&buf[0])))
}

// PtraceTraceCall forks a tracee that calls the code at codeBase (codeLen bytes,
// already executable in this process — e.g. an HwNativeCode's Base()/Len()) with
// up to six integer args per the SysV ABI, single-steps it OUT OF PROCESS, and
// fills trace. It returns the routine's return value (the child's RAX at the ret).
func PtraceTraceCall(codeBase unsafe.Pointer, codeLen int, args []int64, trace *HwTrace) (int64, error) {
	n := len(args)
	// asmtest_ptrace_trace_call reads `nargs` longs; pass a valid pointer even
	// for the zero-arg case.
	arr := make([]C.long, n)
	if n == 0 {
		arr = make([]C.long, 1)
	}
	for i, a := range args {
		arr[i] = C.long(a)
	}
	var result C.long
	rc := C.asmtest_go_pt_trace_call(codeBase, C.size_t(codeLen), &arr[0],
		C.int(n), &result, trace.h)
	if rc != ptraceOK {
		return 0, fmt.Errorf("asmtest_ptrace_trace_call failed: %d", int(rc))
	}
	return int64(result), nil
}

// PtraceTraceAttached traces a region [base, base+length) in a SEPARATE,
// already-ptrace-stopped process (the caller owns PTRACE_ATTACH/DETACH). The
// target's bytes are read via process_vm_readv. Returns the target's RAX at the
// region exit.
func PtraceTraceAttached(pid int, base uintptr, length int, trace *HwTrace) (int64, error) {
	var result C.long
	rc := C.asmtest_go_pt_trace_attached(C.int(pid), unsafe.Pointer(base),
		C.size_t(length), &result, trace.h)
	if rc != ptraceOK {
		return 0, fmt.Errorf("asmtest_ptrace_trace_attached failed: %d", int(rc))
	}
	return int64(result), nil
}

// ProcRegionByAddr finds the executable mapping in /proc/<pid>/maps that contains
// addr and returns its extent (base, len). ok is false when no executable mapping
// contains addr (or on a read failure).
func ProcRegionByAddr(pid int, addr uintptr) (base, length uintptr, ok bool) {
	var b unsafe.Pointer
	var l C.size_t
	rc := C.asmtest_go_proc_region_by_addr(C.int(pid), unsafe.Pointer(addr), &b, &l)
	if rc != ptraceOK {
		return 0, 0, false
	}
	return uintptr(b), uintptr(l), true
}

// ProcPerfmapSymbol resolves a JIT method by name in /tmp/perf-<pid>.map and
// returns its extent (base, len). ok is false when there is no such symbol or no
// map file.
func ProcPerfmapSymbol(pid int, name string) (base, length uintptr, ok bool) {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	var b unsafe.Pointer
	var l C.size_t
	rc := C.asmtest_go_proc_perfmap_symbol(C.int(pid), cs, &b, &l)
	if rc != ptraceOK {
		return 0, 0, false
	}
	return uintptr(b), uintptr(l), true
}

// JitMethod is a JIT method resolved from a jitdump: its load address, size, the
// JIT's timestamp/index, and (optionally) the recorded native code bytes. Mirrors
// asmtest_jitdump_entry_t + the Python wrapper's JitMethod.
type JitMethod struct {
	CodeAddr  uint64
	CodeSize  uint64
	Timestamp uint64
	CodeIndex uint64
	Code      []byte
}

// JitdumpFind reads the Linux perf jitdump at path (or /tmp/jit-<pid>.dump when
// path is empty) and resolves a method by name to its load address, size,
// timestamp/index, and — when wantBytes > 0 — up to wantBytes of the recorded
// native code. The latest re-JIT body (highest timestamp) wins. ok is false when
// no such method / no file / not a jitdump.
func JitdumpFind(path string, name string, pid int, wantBytes int) (JitMethod, bool) {
	var cPath *C.char
	if path != "" {
		cPath = C.CString(path)
		defer C.free(unsafe.Pointer(cPath))
	}
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))

	var entry C.asmtest_jitdump_entry_t
	var bytesOut *C.uint8_t
	var blen C.size_t
	var buf []byte
	if wantBytes > 0 {
		buf = make([]byte, wantBytes)
		bytesOut = (*C.uint8_t)(unsafe.Pointer(&buf[0]))
	}
	rc := C.asmtest_go_jitdump_find(cPath, C.int(pid), cName, &entry, bytesOut,
		C.size_t(wantBytes), &blen)
	if rc != ptraceOK {
		return JitMethod{}, false
	}
	m := JitMethod{
		CodeAddr:  uint64(entry.code_addr),
		CodeSize:  uint64(entry.code_size),
		Timestamp: uint64(entry.timestamp),
		CodeIndex: uint64(entry.code_index),
	}
	if wantBytes > 0 {
		m.Code = append([]byte(nil), buf[:int(blen)]...)
	}
	return m, true
}
