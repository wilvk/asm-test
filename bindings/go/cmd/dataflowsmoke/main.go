// Go data-flow binding smoke (Phase 6 + F7): GC-move canonicalizer + method
// resolver, mirroring the other language suites — and (F7) a REAL live attach to a
// victim process by pid. cgo dlopen's libasmtest_dataflow at runtime (like the
// hwtrace.go binding).
//
//	cd bindings/go && ASMTEST_DATAFLOW_LIB=<lib> ASMTEST_DATAFLOW_VICTIM=<victim> \
//	  go run ./cmd/dataflowsmoke
package main

/*
#cgo LDFLAGS: -ldl
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>

typedef struct { uint64_t old_base; uint64_t new_base; uint64_t len; uint32_t step; } df_gcmove_t;
typedef struct { uint64_t addr; uint64_t size; const char *name; uint64_t version; } df_method_t;

static uint64_t (*p_canon)(const df_gcmove_t*, size_t, uint32_t, uint64_t);
static int (*p_resolve)(const df_method_t*, size_t, uint64_t);

// The L0 sink handle is opaque here — passed around, never inspected.
static void *(*p_vt_new)(size_t, size_t, size_t);
static void  (*p_vt_free)(void*);
static size_t (*p_vt_steps)(const void*);
static size_t (*p_vt_recs)(const void*);

// F7 — the LIVE-ATTACH producer entry points (src/dataflow_ptrace.c). The producer
// ships NO header on purpose (a value-trace PRODUCER is a tier, not part of the
// shared sink API), so — exactly as its own C suite does — this binding re-declares
// them. Nothing cross-checks these signatures but the assertions in main(), so keep
// them in step with that file. No struct crosses by value; `img` (the versioned-
// decode code image) is opaque and always NULL here.
//
// NB: line comments ONLY inside this preamble. The preamble is itself delimited by
// a C-style block comment, and those do not nest — the first close-comment token
// anywhere in here (even inside a line comment like this one) ends the preamble,
// and everything after it gets parsed as Go. Hence no block comments below.
static int (*p_attach_pid)(int, uint64_t, size_t, uint64_t, long*, void*);
static int (*p_attach_pid_tid)(int, int, uint64_t, size_t, uint64_t, long*, void*);
static int (*p_attach_jit)(int, int, uint64_t, size_t, void*, uint64_t, uint64_t, long*, int*, void*);

// One operand read/write record (include/asmtest_valtrace.h at_val_rec_t). The
// three bool fields are declared `unsigned char` (not `_Bool`) purely to keep
// cgo's Go-side type mapping simple; the ABI only cares that a byte holding 0/1
// lands at the verified offset. Layout verified via offsetof on this build: kind
// 0, reg/base/index 4/8/12, scale 16, disp 24, addr 32, size 40, the three flags
// 42/43/44, wide_off 48, value 56, step 64 — 72 bytes.
typedef struct {
    int32_t kind;
    uint32_t reg;
    uint32_t base;
    uint32_t index;
    int32_t scale;
    int64_t disp;
    uint64_t addr;
    uint16_t size;
    unsigned char is_write;
    unsigned char value_valid;
    unsigned char wide;
    uint32_t wide_off;
    uint64_t value;
    uint32_t step;
} df_valrec_t;

// L1 def-use graph + L2 slice (analysis pipeline, src/dataflow.c). The seed
// crosses BY POINTER (asmtest_slice_forward_seed/_backward_seed) — the binding is
// cgo-only, which could pass df_valrec_t by value, but the pointer form keeps
// this call's shape uniform with the sibling bindings that cannot.
static void  (*p_vt_append)(void*, uint64_t, const df_valrec_t*, size_t);
static void *(*p_defuse_build)(const void*);
static void  (*p_defuse_free)(void*);
static void *(*p_slice_forward_seed)(const void*, const df_valrec_t*);
static void *(*p_slice_backward_seed)(const void*, const df_valrec_t*);
static void  (*p_slice_free)(void*);
static int   (*p_slice_contains)(const void*, uint32_t);

static int df_load(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) return -1;
    p_canon   = (uint64_t(*)(const df_gcmove_t*, size_t, uint32_t, uint64_t))dlsym(h, "asmtest_gcmove_canon");
    p_resolve = (int(*)(const df_method_t*, size_t, uint64_t))dlsym(h, "asmtest_method_resolve_pc");
    p_vt_new   = (void*(*)(size_t, size_t, size_t))dlsym(h, "asmtest_valtrace_new");
    p_vt_free  = (void(*)(void*))dlsym(h, "asmtest_valtrace_free");
    p_vt_steps = (size_t(*)(const void*))dlsym(h, "asmtest_valtrace_steps");
    p_vt_recs  = (size_t(*)(const void*))dlsym(h, "asmtest_valtrace_recs");
    p_attach_pid = (int(*)(int, uint64_t, size_t, uint64_t, long*, void*))
        dlsym(h, "asmtest_dataflow_ptrace_attach_pid");
    p_attach_pid_tid = (int(*)(int, int, uint64_t, size_t, uint64_t, long*, void*))
        dlsym(h, "asmtest_dataflow_ptrace_attach_pid_tid");
    p_attach_jit = (int(*)(int, int, uint64_t, size_t, void*, uint64_t, uint64_t, long*, int*, void*))
        dlsym(h, "asmtest_dataflow_ptrace_attach_jit");
    p_vt_append = (void(*)(void*, uint64_t, const df_valrec_t*, size_t))
        dlsym(h, "asmtest_valtrace_append");
    p_defuse_build = (void*(*)(const void*))dlsym(h, "asmtest_defuse_build");
    p_defuse_free = (void(*)(void*))dlsym(h, "asmtest_defuse_free");
    p_slice_forward_seed = (void*(*)(const void*, const df_valrec_t*))
        dlsym(h, "asmtest_slice_forward_seed");
    p_slice_backward_seed = (void*(*)(const void*, const df_valrec_t*))
        dlsym(h, "asmtest_slice_backward_seed");
    p_slice_free = (void(*)(void*))dlsym(h, "asmtest_slice_free");
    p_slice_contains = (int(*)(const void*, uint32_t))dlsym(h, "asmtest_slice_contains");
    return (p_canon && p_resolve && p_vt_new && p_vt_free && p_vt_steps && p_vt_recs &&
            p_attach_pid && p_attach_pid_tid && p_attach_jit && p_vt_append &&
            p_defuse_build && p_defuse_free && p_slice_forward_seed &&
            p_slice_backward_seed && p_slice_free && p_slice_contains) ? 0 : -1;
}
static uint64_t df_canon(const df_gcmove_t *m, size_t n, uint32_t s, uint64_t phys) { return p_canon(m, n, s, phys); }
static int df_resolve(const df_method_t *m, size_t n, uint64_t pc) { return p_resolve(m, n, pc); }

static void *df_vt_new(size_t sc, size_t rc, size_t wc) { return p_vt_new(sc, rc, wc); }
static void  df_vt_free(void *v) { p_vt_free(v); }
static size_t df_vt_steps(const void *v) { return p_vt_steps(v); }
static size_t df_vt_recs(const void *v) { return p_vt_recs(v); }
static void df_vt_append(void *v, uint64_t off, const df_valrec_t *recs, size_t n) {
    p_vt_append(v, off, recs, n);
}
static void *df_defuse_build(const void *v) { return p_defuse_build(v); }
static void  df_defuse_free(void *g) { p_defuse_free(g); }
static void *df_slice_forward_seed(const void *g, const df_valrec_t *seed) {
    return p_slice_forward_seed(g, seed);
}
static void *df_slice_backward_seed(const void *g, const df_valrec_t *seed) {
    return p_slice_backward_seed(g, seed);
}
static void df_slice_free(void *s) { p_slice_free(s); }
static int df_slice_contains(const void *s, uint32_t step) { return p_slice_contains(s, step); }
static int df_attach_pid(int pid, uint64_t base, size_t len, uint64_t mi, long *res, void *vt) {
    return p_attach_pid(pid, base, len, mi, res, vt);
}
static int df_attach_pid_tid(int pid, int tid, uint64_t base, size_t len, uint64_t mi, long *res, void *vt) {
    return p_attach_pid_tid(pid, tid, base, len, mi, res, vt);
}
static int df_attach_jit(int pid, int tid, uint64_t base, size_t len, uint64_t when,
                         uint64_t mi, long *res, int *surv, void *vt) {
    return p_attach_jit(pid, tid, base, len, NULL, when, mi, res, surv, vt);
}
*/
import "C"

import (
	"bufio"
	"encoding/binary"
	"fmt"
	"os"
	"os/exec"
	"runtime"
	"strconv"
	"strings"
	"time"
	"unsafe"
)

var n int
var failed bool

func check(cond bool, desc string) {
	n++
	if cond {
		fmt.Printf("ok %d - %s\n", n, desc)
	} else {
		fmt.Printf("not ok %d - %s\n", n, desc)
		failed = true
	}
}

func gcmove(moves [][4]uint64, step uint32, phys uint64) uint64 {
	if len(moves) == 0 {
		return uint64(C.df_canon(nil, 0, C.uint32_t(step), C.uint64_t(phys)))
	}
	arr := make([]C.df_gcmove_t, len(moves))
	for i, m := range moves {
		arr[i].old_base = C.uint64_t(m[0])
		arr[i].new_base = C.uint64_t(m[1])
		arr[i].len = C.uint64_t(m[2])
		arr[i].step = C.uint32_t(m[3])
	}
	return uint64(C.df_canon(&arr[0], C.size_t(len(arr)), C.uint32_t(step), C.uint64_t(phys)))
}

type meth struct {
	addr, size uint64
	name       string
	version    uint64
}

func method(methods []meth, pc uint64) int {
	if len(methods) == 0 {
		return int(C.df_resolve(nil, 0, C.uint64_t(pc)))
	}
	arr := make([]C.df_method_t, len(methods))
	cnames := make([]*C.char, len(methods))
	for i, m := range methods {
		cnames[i] = C.CString(m.name)
		arr[i].addr = C.uint64_t(m.addr)
		arr[i].size = C.uint64_t(m.size)
		arr[i].name = cnames[i]
		arr[i].version = C.uint64_t(m.version)
	}
	r := int(C.df_resolve(&arr[0], C.size_t(len(arr)), C.uint64_t(pc)))
	for _, p := range cnames {
		C.free(unsafe.Pointer(p))
	}
	return r
}

// at_loc_kind_t: the location space of an operand.
const (
	locReg    = 0 // a register (key = Capstone reg id)
	locMemAbs = 1 // memory at an absolute effective address (key = addr)
	locMemOff = 2 // memory at a routine offset
)

// loc is (kind, key): key is a reg id for locReg, else an absolute address.
func mkRec(kind int32, key uint64, isWrite bool) C.df_valrec_t {
	var r C.df_valrec_t
	r.kind = C.int32_t(kind)
	if kind == locReg {
		r.reg = C.uint32_t(key)
	} else {
		r.addr = C.uint64_t(key)
	}
	if isWrite {
		r.is_write = 1
	}
	return r
}

// A hand-built (or live-attach-filled) L0 value trace plus its cached L1 def-use
// graph and L2 slicer — mirrors the Python/Ruby/Lua/Zig/Rust ValueTrace.
type valueTrace struct {
	v      unsafe.Pointer
	g      unsafe.Pointer
	nSteps uint32
}

func newValueTrace(stepsCap, recsCap int) *valueTrace {
	v := C.df_vt_new(C.size_t(stepsCap), C.size_t(recsCap), 0)
	if v == nil {
		panic("asmtest_valtrace_new failed")
	}
	return &valueTrace{v: v}
}

// step appends one executed instruction at offset `off` with its pre-built
// operand records (read-set then write-set — see mkRec).
func (vt *valueTrace) step(off uint64, recs []C.df_valrec_t) {
	var p *C.df_valrec_t
	if len(recs) > 0 {
		p = &recs[0]
	}
	C.df_vt_append(vt.v, C.uint64_t(off), p, C.size_t(len(recs)))
	vt.nSteps++
	vt.invalidateDefuse()
}

// postAttach resyncs the step count and drops any stale def-use graph after a
// live producer appends behind our back (unlike step, which counts as it goes).
// Not yet called here — T3 wires this into the live-captured memory-edge
// assertion.
func (vt *valueTrace) postAttach() {
	vt.nSteps = uint32(C.df_vt_steps(vt.v))
	vt.invalidateDefuse()
}

func (vt *valueTrace) invalidateDefuse() {
	if vt.g != nil {
		C.df_defuse_free(vt.g)
		vt.g = nil
	}
}

// defuse builds the L1 last-writer def-use graph once, caching it until the
// next step()/attach invalidates it.
func (vt *valueTrace) defuse() unsafe.Pointer {
	if vt.g == nil {
		vt.g = C.df_defuse_build(vt.v)
		if vt.g == nil {
			panic("asmtest_defuse_build failed")
		}
	}
	return vt.g
}

func (vt *valueTrace) slice(origin uint32, forward bool) []uint32 {
	g := vt.defuse()
	var seed C.df_valrec_t
	seed.step = C.uint32_t(origin)
	var s unsafe.Pointer
	if forward {
		s = C.df_slice_forward_seed(g, &seed)
	} else {
		s = C.df_slice_backward_seed(g, &seed)
	}
	out := make([]uint32, 0, vt.nSteps)
	for i := uint32(0); i < vt.nSteps; i++ {
		if C.df_slice_contains(s, C.uint32_t(i)) != 0 {
			out = append(out, i)
		}
	}
	C.df_slice_free(s)
	return out
}

// forwardSlice returns the steps influenced by the value defined at step
// `origin` (origin included).
func (vt *valueTrace) forwardSlice(origin uint32) []uint32 { return vt.slice(origin, true) }

// backwardSlice returns the steps that produced the value used at step `sink`
// (sink included).
func (vt *valueTrace) backwardSlice(sink uint32) []uint32 { return vt.slice(sink, false) }

func (vt *valueTrace) free() {
	if vt.v != nil {
		vt.invalidateDefuse()
		C.df_vt_free(vt.v)
		vt.v = nil
	}
}

// T2 — def-use/slice surface (by-pointer seed, src/dataflow.c). Pure C, no
// ptrace, so it runs unconditionally. Not a counted TAP assertion (T3 adds
// those, plus the live-captured memory-edge case) — this is a build-time proof
// that the wrapper actually slices correctly, not just that it links.
func defuseSliceSmoke() {
	vt := newValueTrace(64, 512)
	// A register chain r10 -> r11 -> r12 (mirrors the Python round-trip test).
	vt.step(0, []C.df_valrec_t{mkRec(locReg, 10, true)})
	vt.step(1, []C.df_valrec_t{mkRec(locReg, 10, false), mkRec(locReg, 11, true)})
	vt.step(2, []C.df_valrec_t{mkRec(locReg, 11, false), mkRec(locReg, 12, true)})
	fwd := vt.forwardSlice(0)
	bwd := vt.backwardSlice(2)
	if len(fwd) != 3 || len(bwd) != 3 {
		panic(fmt.Sprintf("defuseSliceSmoke: forward=%v backward=%v, want len 3 each", fwd, bwd))
	}
	vt.free()
}

func main() {
	path := os.Getenv("ASMTEST_DATAFLOW_LIB")
	if path == "" {
		path = "../../build/libasmtest_dataflow.so"
	}
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))
	if C.df_load(cpath) != 0 {
		fmt.Println("# SKIP go dataflow: cannot dlopen libasmtest_dataflow")
		fmt.Println("1..0 # skipped")
		return
	}

	// GC-move canonicalizer
	check(gcmove(nil, 0, 0x1234) == 0x1234, "gcmove: empty move set is identity")
	mv := [][4]uint64{{0x1000, 0x2000, 0x100, 5}}
	check(gcmove(mv, 3, 0x1010) == 0x2010, "gcmove: pre-move addr forwards to final")
	check(gcmove(mv, 3, 0x1000) == 0x2000, "gcmove: object base forwards")
	check(gcmove(mv, 3, 0x10FF) == 0x20FF, "gcmove: last byte of half-open window forwards")
	check(gcmove(mv, 3, 0x1100) == 0x1100, "gcmove: one past the window not forwarded")
	check(gcmove(mv, 5, 0x1010) == 0x1010, "gcmove: at-move-step observation not forwarded")
	check(gcmove(mv, 3, 0x3000) == 0x3000, "gcmove: out-of-range addr unchanged")
	mv2 := [][4]uint64{{0x1000, 0x2000, 0x100, 3}, {0x2000, 0x3000, 0x100, 6}}
	check(gcmove(mv2, 1, 0x1010) == 0x3010, "gcmove: two compactions compose to final")

	// method resolver
	ms := []meth{{0x1000, 0x40, "Foo", 3}, {0x2000, 0x20, "Bar", 1}, {0x3000, 0, "Baz", 2}}
	check(method(ms, 0x1000) == 0, "method: Foo range start")
	check(method(ms, 0x103F) == 0, "method: Foo last byte (half-open)")
	check(method(ms, 0x1040) == -1, "method: one past Foo -> none")
	check(method(ms, 0x2010) == 1, "method: Bar range")
	check(method(ms, 0x3000) == 2, "method: Baz point match")
	check(method(ms, 0x3001) == -1, "method: Baz is point-only")
	rj := []meth{{0x1000, 0x40, "Foo", 1}, {0x1000, 0x40, "Foo", 5}}
	check(method(rj, 0x1010) == 1, "method: tiered re-JIT newest version wins")
	check(method(nil, 0x1000) == -1, "method: empty map -> -1")

	defuseSliceSmoke()
	liveAttachTests()

	fmt.Printf("1..%d\n", n)
	if failed {
		os.Exit(1)
	}
}

// ---------------------------------------------------------------------------
// F7 — live-attach data flow: capture over a REAL attached pid.
//
// Every assertion is POSITIVE and keyed to something only a working capture can
// produce (the region's return value, the exact step count, the survival report).
// Nothing hides behind "if we captured anything" — an EMPTY capture IS the failure
// signature, so a guard like that would skip exactly when it should shout.
// ---------------------------------------------------------------------------

// The producer's return codes, re-declared for the same reason the prototypes are.
const (
	ptraceOK     = 0  // a complete scoped trace
	ptraceEINVAL = -1 // bad arguments
	ptraceENOSYS = -3 // off Linux x86-64 / no Capstone: the tier is absent
	ptraceETRACE = -4 // ptrace/wait failure (seccomp/yama)
)

// A live victim: spawn it, learn its region base + its OWN reported pid (see
// bindings/dataflow_victim.c). a/b are OURS, so the expected result is a property
// of THIS run, not a constant a stubbed wrapper could hardcode.
type victim struct {
	cmd         *exec.Cmd
	base        uint64
	length      uint64
	pid         int
	counterPath string
}

func spawnVictim(exe, tag string, a, b int) *victim {
	counterPath := fmt.Sprintf("/tmp/asmtest-df-go-%s.counter", tag)
	cmd := exec.Command(exe, counterPath, strconv.Itoa(a), strconv.Itoa(b))
	out, err := cmd.StdoutPipe()
	if err != nil {
		panic(err)
	}
	if err := cmd.Start(); err != nil {
		panic(err)
	}
	// Blocks until the victim flushes its handshake and starts looping.
	line, err := bufio.NewReader(out).ReadString('\n')
	if err != nil {
		panic(fmt.Sprintf("victim handshake failed: %v", err))
	}
	f := strings.Fields(strings.TrimSpace(line))
	if len(f) != 3 {
		panic(fmt.Sprintf("victim handshake failed: %q", line))
	}
	base, err := strconv.ParseUint(strings.TrimPrefix(f[0], "base=0x"), 16, 64)
	if err != nil {
		panic(err)
	}
	length, err := strconv.ParseUint(strings.TrimPrefix(f[1], "len="), 10, 64)
	if err != nil {
		panic(err)
	}
	pid, err := strconv.Atoi(strings.TrimPrefix(f[2], "pid="))
	if err != nil {
		panic(err)
	}
	return &victim{cmd: cmd, base: base, length: length, pid: pid, counterPath: counterPath}
}

func (v *victim) counter() uint64 {
	b, err := os.ReadFile(v.counterPath)
	if err != nil || len(b) < 8 {
		return 0
	}
	return binary.LittleEndian.Uint64(b[:8])
}

func (v *victim) close() {
	_ = v.cmd.Process.Kill()
	_ = v.cmd.Wait()
}

// ETRACE is NOT a skip. ptrace is a capability the lane can be GIVEN
// (--cap-add=SYS_PTRACE / seccomp=unconfined), and the victim opts in via
// PR_SET_PTRACER_ANY, so a refusal means the lane is misconfigured — be loud.
func checkRc(rc C.int, desc string) {
	if int(rc) == ptraceETRACE {
		fmt.Printf("# %s: ptrace refused (ETRACE) — the lane needs --cap-add=SYS_PTRACE; "+
			"this is NOT a valid skip\n", desc)
	}
	check(int(rc) == ptraceOK, desc)
}

func liveAttachTests() {
	// The tier is Linux x86-64 only (src/dataflow_ptrace.c's own #if). On such a
	// host the live tests MUST run: an unavailable tier there means the lib was
	// linked without Capstone — a build defect that has to be RED, not a skip.
	if runtime.GOOS != "linux" || runtime.GOARCH != "amd64" {
		fmt.Println("# SKIP live-attach: not linux/amd64 (the tier is Linux x86-64 only)")
		return
	}
	exe := os.Getenv("ASMTEST_DATAFLOW_VICTIM")
	if exe == "" {
		// The lane always exports this; missing means a misconfigured lane, and
		// silently skipping every live test is the hole this suite must not have.
		fmt.Println("Bail out! ASMTEST_DATAFLOW_VICTIM unset; run `make dataflow-go-test`")
		os.Exit(1)
	}

	// Probed, not a symbol-resolves check: EINVAL (real) vs ENOSYS (stub) — dlsym
	// finds the symbol either way, so only the return code tells them apart.
	{
		v := C.df_vt_new(1, 1, 0)
		var out C.long
		rc := C.df_attach_pid(0, 0, 0, 0, &out, v)
		C.df_vt_free(v)
		check(int(rc) != ptraceENOSYS, "live: tier is real on linux/amd64 (EINVAL, not ENOSYS)")
	}

	{
		vic := spawnVictim(exe, "1", 7, 5)
		v := C.df_vt_new(64, 512, 0)
		var out C.long
		checkRc(C.df_attach_pid(C.int(vic.pid), C.uint64_t(vic.base), C.size_t(vic.length), 0, &out, v),
			"live: attach_pid a FOREIGN running pid + stepped the region")
		// The region really executed IN the victim: rax = rdi + rsi.
		check(int(out) == 12, "live: attach_pid region returned 12 (rax = rdi + rsi)")
		// Exactly df_chain's six in-region instructions — not "some".
		check(uint64(C.df_vt_steps(v)) == 6, "live: six in-region steps captured over the victim")
		check(uint64(C.df_vt_recs(v)) > 0, "live: operand records captured")
		// SURVIVAL: we attached to a process we do not own; it must outlive the detach.
		c0 := vic.counter()
		time.Sleep(50 * time.Millisecond)
		check(vic.counter() > c0, "live: victim SURVIVED the detach (counter advanced)")
		C.df_vt_free(v)
		vic.close()
	}
	{
		// THE anti-hardcode control: a second victim, different args, same wrapper.
		vic := spawnVictim(exe, "2", 17, 25)
		v := C.df_vt_new(64, 512, 0)
		var out C.long
		checkRc(C.df_attach_pid(C.int(vic.pid), C.uint64_t(vic.base), C.size_t(vic.length), 0, &out, v),
			"live: attach_pid the second victim")
		check(int(out) == 42, "live: result TRACKS the victim's args (17+25=42)")
		check(uint64(C.df_vt_steps(v)) == 6, "live: six steps on the second victim too")
		C.df_vt_free(v)
		vic.close()
	}
	{
		vic := spawnVictim(exe, "3", 9, 4)
		v := C.df_vt_new(64, 512, 0)
		var out C.long
		// only_tid 0: step whichever thread enters the region (here, the only one).
		checkRc(C.df_attach_pid_tid(C.int(vic.pid), 0, C.uint64_t(vic.base), C.size_t(vic.length), 0, &out, v),
			"live: attach_pid_tid stepped the entering thread")
		check(int(out) == 13, "live: attach_pid_tid region returned 13 (9+4)")
		check(uint64(C.df_vt_steps(v)) == 6, "live: attach_pid_tid captured six steps")
		C.df_vt_free(v)
		vic.close()
	}
	{
		vic := spawnVictim(exe, "4", 20, 3)
		v := C.df_vt_new(64, 512, 0)
		var out C.long
		var survived C.int
		checkRc(C.df_attach_jit(C.int(vic.pid), 0, C.uint64_t(vic.base), C.size_t(vic.length),
			0, 0, &out, &survived, v), "live: attach_jit stepped the region")
		check(int(out) == 23, "live: attach_jit region returned 23 (20+3)")
		check(uint64(C.df_vt_steps(v)) == 6, "live: attach_jit captured six steps")
		// The producer's OWN survival report — the house rule that a foreign target
		// is never killed, asserted from its side.
		check(int(survived) == 1, "live: attach_jit reported the target as survived")
		c0 := vic.counter()
		time.Sleep(50 * time.Millisecond)
		check(vic.counter() > c0, "live: attach_jit victim kept running after detach")
		C.df_vt_free(v)
		vic.close()
	}
	{
		// Negative control: the wrapper must surface the producer's rejections
		// rather than manufacture success.
		v := C.df_vt_new(8, 8, 0)
		var out C.long
		check(int(C.df_attach_pid(12345, 0x1000, 0, 0, &out, v)) == ptraceEINVAL,
			"live: zero-length region is rejected (EINVAL)")
		check(int(C.df_attach_pid(0, 0x1000, 21, 0, &out, v)) == ptraceEINVAL,
			"live: pid 0 is rejected (EINVAL)")
		check(int(C.df_attach_pid(0x7FFFFFF0, 0x1000, 21, 0, &out, v)) != ptraceOK,
			"live: attaching to a nonexistent pid never returns OK")
		C.df_vt_free(v)
	}
}
