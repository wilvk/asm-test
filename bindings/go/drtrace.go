// In-process native runtime tracing for Go, backed by DynamoRIO.
//
// This is the Go counterpart of the Python wrapper (bindings/python/asmtest/
// drtrace.py) and the C API in include/asmtest_drtrace.h. Where the emulator tier
// (Emu / Trace, in asmtest.go) traces isolated guest bytes, NativeTrace traces
// host-native code as it runs **inside this Go process**: initialize DynamoRIO
// once, materialize host-native machine code, mark a region, call into it, and
// read back basic-block coverage / the instruction stream.
//
// The DynamoRIO tier lives in its own shared library, libasmtest_drapp, which is
// built only when DynamoRIO is present and so may be absent. Exactly like the
// optional in-line assembler in asmtest.go, this file therefore does NOT link the
// lib (-lasmtest_drapp is intentionally omitted): it dlopen()s it at RUN TIME —
// from $ASMTEST_DRAPP_LIB, else <repo>/build/libasmtest_drapp.so — and dlsym()s
// the entry points into static function pointers. If the lib (or, inside it,
// libdynamorio) can't be resolved, NativeTraceAvailable() returns false and the
// whole tier self-skips cleanly. Linux x86-64 only; advanced and opt-in.
//
// libasmtest_drapp re-exports the trace handle + accessors (asmtest_trace_new and
// friends) from the same trace.o the emulator uses, so coverage is read back in
// the identical {Covered, BlocksLen, InsnsTotal} shape.
package asmtest

/*
#cgo linux LDFLAGS: -ldl
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <dlfcn.h>

// The two tiny structs the API takes by pointer, redefined here so this file
// needs no header on the cgo include path (mirrors asmtest_drtrace_options_t and
// asmtest_exec_code_t from include/asmtest_drtrace.h). ASMTEST_DR_OK == 0.
typedef struct {
    const char *dynamorio_home;
    const char *client_path;
    const char *client_options;
    int mode;
} asmtest_dr_options_t;

typedef struct {
    void *base;
    size_t len;
} asmtest_dr_exec_code_t;

// Entry-point typedefs (one per exported symbol in libasmtest_drapp).
typedef int  (*dr_available_fn)(void);
typedef int  (*dr_init_fn)(const asmtest_dr_options_t *);
typedef int  (*dr_start_fn)(void);
typedef int  (*dr_stop_fn)(void);
typedef void (*dr_shutdown_fn)(void);
typedef int  (*dr_register_fn)(const char *, void *, size_t, void *);
typedef int  (*dr_unregister_fn)(const char *);
typedef void (*dr_marker_fn)(const char *);
typedef int  (*dr_marker_error_fn)(void);
typedef int  (*dr_exec_alloc_fn)(const uint8_t *, size_t, asmtest_dr_exec_code_t *);
typedef void (*dr_exec_free_fn)(asmtest_dr_exec_code_t *);
typedef void *(*dr_trace_new_fn)(size_t, size_t);
typedef void  (*dr_trace_free_fn)(void *);
typedef int   (*dr_trace_covered_fn)(void *, uint64_t);
typedef unsigned long long (*dr_trace_blocks_len_fn)(void *);
typedef unsigned long long (*dr_trace_insns_total_fn)(void *);
typedef unsigned long long (*dr_trace_insns_len_fn)(void *);
typedef unsigned long long (*dr_trace_block_at_fn)(void *, size_t);
typedef unsigned long long (*dr_trace_insn_at_fn)(void *, size_t);

static dr_available_fn          p_dr_available;
static dr_init_fn               p_dr_init;
static dr_start_fn              p_dr_start;
static dr_stop_fn               p_dr_stop;
static dr_shutdown_fn           p_dr_shutdown;
static dr_register_fn           p_dr_register;
static dr_unregister_fn         p_dr_unregister;
static dr_marker_fn             p_dr_begin;
static dr_marker_fn             p_dr_end;
static dr_marker_error_fn       p_dr_marker_error;
static dr_exec_alloc_fn         p_dr_exec_alloc;
static dr_exec_free_fn          p_dr_exec_free;
static dr_trace_new_fn          p_dr_trace_new;
static dr_trace_free_fn         p_dr_trace_free;
static dr_trace_covered_fn      p_dr_trace_covered;
static dr_trace_blocks_len_fn   p_dr_trace_blocks_len;
static dr_trace_insns_total_fn  p_dr_trace_insns_total;
static dr_trace_insns_len_fn    p_dr_trace_insns_len;
static dr_trace_block_at_fn     p_dr_trace_block_at;
static dr_trace_insn_at_fn      p_dr_trace_insn_at;

static int g_dr_loaded;   // 1 once dlopen + every dlsym succeeded.

// Resolve libasmtest_drapp from $ASMTEST_DRAPP_LIB, else build/libasmtest_drapp.so
// (relative to the cwd, like the Python loader's last candidate). dlopen it
// RTLD_GLOBAL so the trampolines below find the symbols, then dlsym each entry
// point. On any miss the tier stays unloaded and asmtest_dr_is_loaded() is 0, so
// the binding self-skips rather than crashing. Idempotent.
static void asmtest_dr_resolve(void) {
    if (g_dr_loaded) return;
    const char *env = getenv("ASMTEST_DRAPP_LIB");
    void *h = dlopen(env && *env ? env : "build/libasmtest_drapp.so",
                     RTLD_NOW | RTLD_GLOBAL);
    if (!h) return;
    p_dr_available       = (dr_available_fn)dlsym(h, "asmtest_dr_available");
    p_dr_init            = (dr_init_fn)dlsym(h, "asmtest_dr_init");
    p_dr_start           = (dr_start_fn)dlsym(h, "asmtest_dr_start");
    p_dr_stop            = (dr_stop_fn)dlsym(h, "asmtest_dr_stop");
    p_dr_shutdown        = (dr_shutdown_fn)dlsym(h, "asmtest_dr_shutdown");
    p_dr_register        = (dr_register_fn)dlsym(h, "asmtest_dr_register_region");
    p_dr_unregister      = (dr_unregister_fn)dlsym(h, "asmtest_dr_unregister_region");
    p_dr_begin           = (dr_marker_fn)dlsym(h, "asmtest_trace_begin");
    p_dr_end             = (dr_marker_fn)dlsym(h, "asmtest_trace_end");
    p_dr_marker_error    = (dr_marker_error_fn)dlsym(h, "asmtest_dr_marker_error");
    p_dr_exec_alloc      = (dr_exec_alloc_fn)dlsym(h, "asmtest_exec_alloc");
    p_dr_exec_free       = (dr_exec_free_fn)dlsym(h, "asmtest_exec_free");
    p_dr_trace_new       = (dr_trace_new_fn)dlsym(h, "asmtest_trace_new");
    p_dr_trace_free      = (dr_trace_free_fn)dlsym(h, "asmtest_trace_free");
    p_dr_trace_covered   = (dr_trace_covered_fn)dlsym(h, "asmtest_trace_covered");
    p_dr_trace_blocks_len  = (dr_trace_blocks_len_fn)dlsym(h, "asmtest_emu_trace_blocks_len");
    p_dr_trace_insns_total = (dr_trace_insns_total_fn)dlsym(h, "asmtest_emu_trace_insns_total");
    p_dr_trace_insns_len   = (dr_trace_insns_len_fn)dlsym(h, "asmtest_emu_trace_insns_len");
    p_dr_trace_block_at    = (dr_trace_block_at_fn)dlsym(h, "asmtest_emu_trace_block_at");
    p_dr_trace_insn_at     = (dr_trace_insn_at_fn)dlsym(h, "asmtest_emu_trace_insn_at");
    g_dr_loaded = p_dr_available && p_dr_init && p_dr_start && p_dr_stop &&
                  p_dr_shutdown && p_dr_register && p_dr_unregister && p_dr_begin &&
                  p_dr_end && p_dr_marker_error && p_dr_exec_alloc &&
                  p_dr_exec_free && p_dr_trace_new && p_dr_trace_free &&
                  p_dr_trace_covered && p_dr_trace_blocks_len &&
                  p_dr_trace_insns_total && p_dr_trace_insns_len &&
                  p_dr_trace_block_at && p_dr_trace_insn_at;
}

static int asmtest_dr_is_loaded(void) { return g_dr_loaded; }

// static wrappers Go calls — each guards the (already-resolved) pointer so a
// partial load can never dereference NULL.
static int  asmtest_dr_go_available(void) { return p_dr_available ? p_dr_available() : 0; }
static int  asmtest_dr_go_init(const char *home, const char *client, const char *opts, int mode) {
    asmtest_dr_options_t o;
    o.dynamorio_home = home;
    o.client_path = client;
    o.client_options = opts;
    o.mode = mode;
    return p_dr_init ? p_dr_init(&o) : -1;
}
static int  asmtest_dr_go_start(void)    { return p_dr_start ? p_dr_start() : -1; }
static void asmtest_dr_go_shutdown(void) { if (p_dr_shutdown) p_dr_shutdown(); }
static int  asmtest_dr_go_marker_error(void) { return p_dr_marker_error ? p_dr_marker_error() : 0; }
static int  asmtest_dr_go_register(const char *name, void *base, size_t len, void *trace) {
    return p_dr_register ? p_dr_register(name, base, len, trace) : -1;
}
static int  asmtest_dr_go_unregister(const char *name) {
    return p_dr_unregister ? p_dr_unregister(name) : -1;
}
static void asmtest_dr_go_begin(const char *name) { if (p_dr_begin) p_dr_begin(name); }
static void asmtest_dr_go_end(const char *name)   { if (p_dr_end) p_dr_end(name); }
static int  asmtest_dr_go_exec_alloc(const uint8_t *bytes, size_t len, asmtest_dr_exec_code_t *out) {
    return p_dr_exec_alloc ? p_dr_exec_alloc(bytes, len, out) : -1;
}
static void asmtest_dr_go_exec_free(asmtest_dr_exec_code_t *code) { if (p_dr_exec_free) p_dr_exec_free(code); }
// NOTE: insns capacity FIRST, blocks SECOND (matches asmtest_trace_new's signature).
static void *asmtest_dr_go_trace_new(size_t insns, size_t blocks) {
    return p_dr_trace_new ? p_dr_trace_new(insns, blocks) : NULL;
}
static void  asmtest_dr_go_trace_free(void *t) { if (p_dr_trace_free) p_dr_trace_free(t); }
static int   asmtest_dr_go_trace_covered(void *t, uint64_t off) {
    return p_dr_trace_covered ? p_dr_trace_covered(t, off) : 0;
}
static unsigned long long asmtest_dr_go_blocks_len(void *t) {
    return p_dr_trace_blocks_len ? p_dr_trace_blocks_len(t) : 0;
}
static unsigned long long asmtest_dr_go_insns_total(void *t) {
    return p_dr_trace_insns_total ? p_dr_trace_insns_total(t) : 0;
}
static unsigned long long asmtest_dr_go_insns_len(void *t) {
    return p_dr_trace_insns_len ? p_dr_trace_insns_len(t) : 0;
}
static unsigned long long asmtest_dr_go_block_at(void *t, size_t i) {
    return p_dr_trace_block_at ? p_dr_trace_block_at(t, i) : 0;
}
static unsigned long long asmtest_dr_go_insn_at(void *t, size_t i) {
    return p_dr_trace_insn_at ? p_dr_trace_insn_at(t, i) : 0;
}

// Trampoline that invokes the generated host-native code through a function
// pointer under the SysV integer ABI (two long args -> long result).
static long asmtest_dr_call2(void *p, long a, long b) {
    return ((long (*)(long, long))p)(a, b);
}
*/
import "C"

import (
	"fmt"
	"unsafe"
)

// ASMTEST_DR_OK is the success status returned by the lifecycle / registration
// calls (mirrors the C macro of the same name).
const drOK = 0

// Resolve the optional DynamoRIO tier the first time the package loads, exactly
// as asmtest.go resolves the optional in-line assembler. A miss is silent —
// NativeTraceAvailable() reports it.
func init() { C.asmtest_dr_resolve() }

// NativeTraceAvailable reports whether the in-process DynamoRIO native-trace tier
// can run: the libasmtest_drapp lib loaded AND libdynamorio is resolvable inside
// it (asmtest_dr_available() == 1). It never panics, so callers (and the test)
// self-skip cleanly when the lib or DynamoRIO is absent.
func NativeTraceAvailable() bool {
	return C.asmtest_dr_is_loaded() != 0 && C.asmtest_dr_go_available() != 0
}

// NativeTraceInitialize brings DynamoRIO up in-process and takes over: it fills
// the options struct, calls asmtest_dr_init, then asmtest_dr_start. client is the
// path to libasmtest_drclient.so — an empty string is passed as NULL, so the C
// side falls back to $ASMTEST_DRCLIENT. dynamorioHome (else $ASMTEST_DR_LIB /
// rpath) lets the C side find libdynamorio; clientOptions are extra client
// options; mode is the process-init default recording mode (0 = blocks).
func NativeTraceInitialize(client, dynamorioHome, clientOptions string, mode int) error {
	// Empty string -> NULL pointer (the C side then consults the env), matching
	// the Python wrapper's `(client or "").encode() or None`.
	cHome := cStringOrNil(dynamorioHome)
	cClient := cStringOrNil(client)
	cOpts := cStringOrNil(clientOptions)
	defer C.free(unsafe.Pointer(cHome))
	defer C.free(unsafe.Pointer(cClient))
	defer C.free(unsafe.Pointer(cOpts))
	if rc := C.asmtest_dr_go_init(cHome, cClient, cOpts, C.int(mode)); rc != drOK {
		return fmt.Errorf("asmtest_dr_init failed: %d", int(rc))
	}
	if rc := C.asmtest_dr_go_start(); rc != drOK {
		return fmt.Errorf("asmtest_dr_start failed: %d", int(rc))
	}
	return nil
}

// NativeTraceInitializeDefault is NativeTraceInitialize with all defaults (no
// client/home/options, blocks mode) — the client and libdynamorio then come from
// the environment ($ASMTEST_DRCLIENT, $ASMTEST_DR_LIB / DYNAMORIO_HOME).
func NativeTraceInitializeDefault() error {
	return NativeTraceInitialize("", "", "", 0)
}

// NativeTraceShutdown performs dr_app_stop_and_cleanup and returns DynamoRIO to
// the uninitialized state. Safe to call after a failed initialize.
func NativeTraceShutdown() { C.asmtest_dr_go_shutdown() }

// NativeTraceMarkerError is the count of illegal marker operations observed (an
// end without a matching begin, or a mismatched end) since init. 0 means every
// Begin/End was balanced.
func NativeTraceMarkerError() int { return int(C.asmtest_dr_go_marker_error()) }

// NativeCode is host-native machine code in real executable (W^X) memory,
// materialized at its runtime address so PC-relative and branch targets resolve.
// Release it with Free.
type NativeCode struct {
	code C.asmtest_dr_exec_code_t
	free bool
}

// NativeCodeFromBytes maps executable memory and copies the host-native machine
// code bytes into it (offset 0 is the entry point).
func NativeCodeFromBytes(b []byte) (*NativeCode, error) {
	if !NativeTraceAvailable() {
		return nil, fmt.Errorf("DynamoRIO native-trace tier unavailable")
	}
	nc := &NativeCode{}
	var p *C.uint8_t
	if len(b) > 0 {
		p = (*C.uint8_t)(unsafe.Pointer(&b[0]))
	}
	if rc := C.asmtest_dr_go_exec_alloc(p, C.size_t(len(b)), &nc.code); rc != drOK {
		return nil, fmt.Errorf("asmtest_exec_alloc failed: %d", int(rc))
	}
	return nc, nil
}

// Base is the runtime address of the executable mapping (offset 0 = entry).
func (c *NativeCode) Base() uintptr { return uintptr(c.code.base) }

// Len is the number of code bytes.
func (c *NativeCode) Len() int { return int(c.code.len) }

// Call invokes the code through a function pointer under the SysV integer ABI:
// two long args, a long result.
func (c *NativeCode) Call(a, b int64) int64 {
	return int64(C.asmtest_dr_call2(c.code.base, C.long(a), C.long(b)))
}

// Free unmaps the executable memory. It does NOT unregister the range — if the
// code was registered, Unregister the region FIRST, then Free. Safe to call more
// than once.
func (c *NativeCode) Free() {
	if !c.free {
		C.asmtest_dr_go_exec_free(&c.code)
		c.free = true
	}
}

// NativeTrace is an app-owned coverage recorder for a registered native region,
// reading back basic-block coverage / the ordered instruction stream as the
// region runs under DynamoRIO. Release it with Free.
type NativeTrace struct{ h unsafe.Pointer }

// NewNativeTrace allocates a trace handle. instructions > 0 records the ordered
// instruction stream; blocks > 0 records basic-block coverage. (The underlying
// asmtest_trace_new takes insns first, blocks second; this constructor takes
// blocks first to mirror the Python wrapper's new(blocks=, instructions=).)
func NewNativeTrace(blocks, instructions int) *NativeTrace {
	return &NativeTrace{h: C.asmtest_dr_go_trace_new(C.size_t(instructions), C.size_t(blocks))}
}

// Register records a non-overlapping native code range under name, recording
// coverage into this trace. The name may be freed after the call (it is copied).
func (t *NativeTrace) Register(name string, code *NativeCode) error {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	rc := C.asmtest_dr_go_register(cs, code.code.base, code.code.len, t.h)
	if rc != drOK {
		return fmt.Errorf("register_region(%q) failed: %d", name, int(rc))
	}
	return nil
}

// Unregister drops the named region (and the client's cached translation). Call
// it before NativeCode.Free.
func (t *NativeTrace) Unregister(name string) {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	C.asmtest_dr_go_unregister(cs)
}

// Begin opens recording for the named region on the calling thread. Markers must
// be balanced with End — prefer Region, which balances them for you.
func (t *NativeTrace) Begin(name string) {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	C.asmtest_dr_go_begin(cs)
}

// End closes recording for the named region on the calling thread.
func (t *NativeTrace) End(name string) {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	C.asmtest_dr_go_end(cs)
}

// Region runs fn between a balanced Begin(name)/End(name) — the Go idiom for the
// scoped marker (Go has no RAII, so a func wrapper replaces Python's `with`). End
// runs even if fn panics.
func (t *NativeTrace) Region(name string, fn func()) {
	t.Begin(name)
	defer t.End(name)
	fn()
}

// Covered reports whether the basic block at byte-offset off (from the region
// entry) was entered.
func (t *NativeTrace) Covered(off uint64) bool {
	return C.asmtest_dr_go_trace_covered(t.h, C.uint64_t(off)) != 0
}

// BlocksLen is the number of distinct basic blocks recorded.
func (t *NativeTrace) BlocksLen() uint64 { return uint64(C.asmtest_dr_go_blocks_len(t.h)) }

// InsnsTotal is the number of instructions executed (counts past the buffer cap).
func (t *NativeTrace) InsnsTotal() uint64 { return uint64(C.asmtest_dr_go_insns_total(t.h)) }

// BlockOffsets is the distinct basic-block start offsets recorded, in first-seen
// order.
func (t *NativeTrace) BlockOffsets() []uint64 {
	n := uint64(C.asmtest_dr_go_blocks_len(t.h))
	offs := make([]uint64, n)
	for i := uint64(0); i < n; i++ {
		offs[i] = uint64(C.asmtest_dr_go_block_at(t.h, C.size_t(i)))
	}
	return offs
}

// InsnOffsets is the ordered instruction-offset stream actually stored — each
// executed instruction's offset in execution order, up to the trace's insns
// capacity (insns_len, not the possibly-larger InsnsTotal).
func (t *NativeTrace) InsnOffsets() []uint64 {
	n := uint64(C.asmtest_dr_go_insns_len(t.h))
	offs := make([]uint64, n)
	for i := uint64(0); i < n; i++ {
		offs[i] = uint64(C.asmtest_dr_go_insn_at(t.h, C.size_t(i)))
	}
	return offs
}

// Free releases the trace handle. Safe to call more than once.
func (t *NativeTrace) Free() {
	if t.h != nil {
		C.asmtest_dr_go_trace_free(t.h)
		t.h = nil
	}
}

// cStringOrNil returns a C copy of s, or NULL for the empty string so the C side
// falls back to its environment-variable default (mirrors the Python wrapper).
// The caller frees a non-nil result.
func cStringOrNil(s string) *C.char {
	if s == "" {
		return nil
	}
	return C.CString(s)
}
