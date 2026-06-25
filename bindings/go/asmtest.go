// Package asmtest is the Go binding (Track G) for the asm-test framework.
//
// Like the other dynamic-language bindings (Node, Ruby, Lua, ...), it drives the
// framework's *opaque-handle FFI layer* (src/ffi.c + emu.c) rather than mirroring
// any C struct layout: cgo declares the handful of scalar/pointer entry points,
// links the prebuilt shared libraries, and the framework keeps regs_t /
// emu_result_t entirely C-side. So no per-arch offset table lives in Go.
//
//   asmtest_corpus_routine          name -> routine address (the code under test)
//   asmtest_regs_new/_free          allocate / release a capture handle
//   asmtest_capture6 / _fp2         call a routine through the real ABI + capture
//   asmtest_regs_ret/_fret/_flag_set  read fields by accessor
//   asmtest_check_abi               non-jumping ABI-preservation verdict
//   emu_open/_close, asmtest_emu_*  run a routine in the emulator (faults as data)
//
// The build links libasmtest_emu (capture + emulator + accessors) and
// libasmtest_corpus (the routine fixtures). At run time the dynamic loader finds
// them via LD_LIBRARY_PATH / DYLD_LIBRARY_PATH (set by `make go-test`); the
// CGO_LDFLAGS -L below makes a default `build/` tree work without the Makefile.
package asmtest

/*
#cgo LDFLAGS: -L${SRCDIR}/../../build -lasmtest_emu -lasmtest_corpus
#cgo linux LDFLAGS: -ldl
#include <stdlib.h>
#include <stddef.h>
#include <dlfcn.h>

// Opaque-handle FFI surface, declared here (not via the C headers) so no struct
// layout is mirrored on the Go side.
extern void *asmtest_corpus_routine(const char *name);
extern void *asmtest_regs_new(void);
extern void  asmtest_regs_free(void *r);
extern void  asmtest_capture6(void *out, void *fn, long a0, long a1, long a2, long a3, long a4, long a5);
extern void  asmtest_capture_fp2(void *out, void *fn, double f0, double f1);
extern void  asmtest_capture_vec_f32(void *out, void *fn, float *lanes, int nvec);
extern unsigned long asmtest_regs_ret(void *r);
extern double asmtest_regs_fret(void *r);
extern float asmtest_regs_vec_f32(void *r, int index, int lane);
extern int   asmtest_regs_flag_set(void *r, const char *name);
extern int   asmtest_check_abi(void *r, char *msg, size_t n);
extern void *emu_open(void);
extern void  emu_close(void *e);
extern void *asmtest_emu_result_new(void);
extern void  asmtest_emu_result_free(void *r);
extern int   asmtest_emu_call2(void *e, void *fn, long a0, long a1, void *out);
extern int   asmtest_emu_result_faulted(void *r);
extern unsigned long long asmtest_emu_x86_reg(void *r, const char *name);

// In-line assembler entry points live only in the emu+asm lib (libasmtest_emu_asm),
// which the default build does not link. Resolve them at run time from the lib the
// binding loads (ASMTEST_LIB): dlopen it RTLD_GLOBAL, then dlsym. Against the plain
// libasmtest_emu these stay NULL and CallAsm/Assemble self-skip — matching how the
// dlopen-based bindings (Ruby, Node, ...) degrade.
typedef int (*asmtest_call_asm6_fn)(void *, const char *, int, long, long, long, long, long, long, int, unsigned long long, void *);
typedef int (*asmtest_asm_bytes_fn)(int, int, const char *, unsigned long long, unsigned char *, int);
typedef const char *(*asmtest_asm_err_fn)(void);

static asmtest_call_asm6_fn p_call_asm6;
static asmtest_asm_bytes_fn p_asm_bytes;
static asmtest_asm_err_fn   p_asm_err;

static void asmtest_resolve_asm(void) {
    const char *lib = getenv("ASMTEST_LIB");
    if (lib) dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    p_call_asm6 = (asmtest_call_asm6_fn)dlsym(RTLD_DEFAULT, "asmtest_emu_call_asm6");
    p_asm_bytes = (asmtest_asm_bytes_fn)dlsym(RTLD_DEFAULT, "asmtest_asm_bytes");
    p_asm_err   = (asmtest_asm_err_fn)dlsym(RTLD_DEFAULT, "asmtest_asm_last_error");
}
static int asmtest_has_asm(void) { return p_call_asm6 != NULL; }
static int asmtest_go_call_asm6(void *e, const char *src, int syn, long a0, long a1, long a2, long a3, long a4, long a5, int n, unsigned long long mi, void *out) {
    return p_call_asm6 ? p_call_asm6(e, src, syn, a0, a1, a2, a3, a4, a5, n, mi, out) : 0;
}
static int asmtest_go_asm_bytes(int arch, int syn, const char *src, unsigned long long addr, unsigned char *buf, int cap) {
    return p_asm_bytes ? p_asm_bytes(arch, syn, src, addr, buf, cap) : 0;
}
static const char *asmtest_go_asm_err(void) { return p_asm_err ? p_asm_err() : ""; }
*/
import "C"

import (
	"fmt"
	"unsafe"
)

// Resolve the optional in-line assembler the first time the package loads.
func init() { C.asmtest_resolve_asm() }

// Routine is an opaque pointer to a routine under test. Every handle the binding
// passes back to C is C-allocated, so it is exempt from cgo's pointer-passing
// rules.
type Routine = unsafe.Pointer

// CorpusRoutine resolves a canonical corpus routine (e.g. "add_signed") to its
// address, or nil if the name is unknown.
func CorpusRoutine(name string) Routine {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	return Routine(C.asmtest_corpus_routine(cs))
}

// Regs is a captured register/flags snapshot. Read its fields via the methods;
// release it with Free.
type Regs struct{ h unsafe.Pointer }

// NewRegs allocates a capture handle.
func NewRegs() *Regs { return &Regs{h: C.asmtest_regs_new()} }

// Free releases the handle. Safe to call more than once.
func (r *Regs) Free() {
	if r.h != nil {
		C.asmtest_regs_free(r.h)
		r.h = nil
	}
}

// Capture6 calls fn through the real ABI with up to six integer args (missing
// args default to zero) and captures the result into r.
func (r *Regs) Capture6(fn Routine, args ...int64) {
	var a [6]int64
	copy(a[:], args)
	C.asmtest_capture6(r.h, fn,
		C.long(a[0]), C.long(a[1]), C.long(a[2]),
		C.long(a[3]), C.long(a[4]), C.long(a[5]))
}

// CaptureFP2 calls fn with two double args, capturing the FP return.
func (r *Regs) CaptureFP2(fn Routine, f0, f1 float64) {
	C.asmtest_capture_fp2(r.h, fn, C.double(f0), C.double(f1))
}

// CaptureVecF32 calls fn with up to eight 128-bit vector args (each four float32
// lanes), capturing the vector register file. The vector return is read back
// with VecF32(0).
func (r *Regs) CaptureVecF32(fn Routine, vectors [][4]float32) {
	flat := make([]float32, 0, len(vectors)*4)
	for _, v := range vectors {
		flat = append(flat, v[0], v[1], v[2], v[3])
	}
	var p *C.float
	if len(flat) > 0 {
		p = (*C.float)(unsafe.Pointer(&flat[0]))
	}
	C.asmtest_capture_vec_f32(r.h, fn, p, C.int(len(vectors)))
}

// VecF32 returns the four float32 lanes of vector register index (0 = the
// vector return).
func (r *Regs) VecF32(index int) [4]float32 {
	var out [4]float32
	for lane := 0; lane < 4; lane++ {
		out[lane] = float32(C.asmtest_regs_vec_f32(r.h, C.int(index), C.int(lane)))
	}
	return out
}

// Ret returns the integer return value (rax / x0).
func (r *Regs) Ret() int64 { return int64(C.asmtest_regs_ret(r.h)) }

// FRet returns the scalar FP return value (xmm0 / d0).
func (r *Regs) FRet() float64 { return float64(C.asmtest_regs_fret(r.h)) }

// FlagSet reports whether a named condition flag (e.g. "CF", "ZF") is set.
func (r *Regs) FlagSet(name string) bool {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	return C.asmtest_regs_flag_set(r.h, cs) == 1
}

// ABIPreserved reports whether the callee-saved registers were restored, via the
// framework's non-jumping verdict shim (asmtest_check_abi).
func (r *Regs) ABIPreserved() bool { return C.asmtest_check_abi(r.h, nil, 0) == 0 }

// Emu is an open emulator handle (an x86-64 Unicorn guest). Close it with Close.
type Emu struct{ h unsafe.Pointer }

// NewEmu opens an emulator.
func NewEmu() *Emu { return &Emu{h: C.emu_open()} }

// Close releases the emulator. Safe to call more than once.
func (e *Emu) Close() {
	if e.h != nil {
		C.emu_close(e.h)
		e.h = nil
	}
}

// Call2 runs fn in the emulator with two integer args, filling out.
func (e *Emu) Call2(fn Routine, a0, a1 int64, out *EmuResult) {
	C.asmtest_emu_call2(e.h, fn, C.long(a0), C.long(a1), out.h)
}

// AsmArch / AsmSyntax select the target for Assemble (mirror asm_arch_t /
// asm_syntax_t). CallAsm always runs on the x86-64 guest, so it takes only a
// syntax.
type AsmArch int

// AsmSyntax is the input assembly syntax (x86 only).
type AsmSyntax int

const (
	ArchX8664   AsmArch = 0
	ArchArm64   AsmArch = 1
	ArchRiscv64 AsmArch = 2
	ArchArm32   AsmArch = 3
)

const (
	SyntaxIntel AsmSyntax = 0
	SyntaxAtt   AsmSyntax = 1
)

// AsmAvailable reports whether the loaded native lib carries the in-line
// assembler (Keystone) — i.e. ASMTEST_LIB points at libasmtest_emu_asm.
func AsmAvailable() bool { return C.asmtest_has_asm() != 0 }

// AsmError returns the Keystone diagnostic from the most recent assemble on this
// thread ("" on success).
func AsmError() string { return C.GoString(C.asmtest_go_asm_err()) }

// CallAsm assembles x86-64 src in syntax via Keystone and runs it with the
// integer args (up to six), stopping after maxInsns instructions (0 = run to
// ret), filling out. Returns an error carrying the Keystone diagnostic if src
// fails to assemble. Only meaningful when AsmAvailable.
func (e *Emu) CallAsm(src string, args []int64, syntax AsmSyntax, maxInsns uint64, out *EmuResult) error {
	if C.asmtest_has_asm() == 0 {
		return fmt.Errorf("in-line assembler not in this build")
	}
	cs := C.CString(src)
	defer C.free(unsafe.Pointer(cs))
	var a [6]int64
	n := copy(a[:], args)
	ok := C.asmtest_go_call_asm6(e.h, cs, C.int(syntax),
		C.long(a[0]), C.long(a[1]), C.long(a[2]),
		C.long(a[3]), C.long(a[4]), C.long(a[5]),
		C.int(n), C.ulonglong(maxInsns), out.h)
	if ok == 0 {
		return fmt.Errorf("in-line assembly failed: %s", AsmError())
	}
	return nil
}

// Assemble assembles src for arch/syntax at load address addr and returns the
// machine-code bytes. Multi-arch (unlike CallAsm, which runs on the x86-64
// guest). Returns an error carrying the Keystone diagnostic on failure.
func Assemble(src string, arch AsmArch, syntax AsmSyntax, addr uint64) ([]byte, error) {
	if C.asmtest_has_asm() == 0 {
		return nil, fmt.Errorf("in-line assembler not in this build")
	}
	cs := C.CString(src)
	defer C.free(unsafe.Pointer(cs))
	buf := make([]byte, 256)
	n := int(C.asmtest_go_asm_bytes(C.int(arch), C.int(syntax), cs,
		C.ulonglong(addr), (*C.uchar)(unsafe.Pointer(&buf[0])), C.int(len(buf))))
	if n == 0 {
		return nil, fmt.Errorf("assemble failed: %s", AsmError())
	}
	if n > len(buf) {
		buf = make([]byte, n)
		n = int(C.asmtest_go_asm_bytes(C.int(arch), C.int(syntax), cs,
			C.ulonglong(addr), (*C.uchar)(unsafe.Pointer(&buf[0])), C.int(n)))
	}
	return buf[:n], nil
}

// EmuResult carries an emulator run's outcome — faults surfaced as data rather
// than as a host crash. Release it with Free.
type EmuResult struct{ h unsafe.Pointer }

// NewEmuResult allocates a result handle.
func NewEmuResult() *EmuResult { return &EmuResult{h: C.asmtest_emu_result_new()} }

// Free releases the handle. Safe to call more than once.
func (r *EmuResult) Free() {
	if r.h != nil {
		C.asmtest_emu_result_free(r.h)
		r.h = nil
	}
}

// Faulted reports whether the routine took an invalid-memory fault.
func (r *EmuResult) Faulted() bool { return C.asmtest_emu_result_faulted(r.h) != 0 }

// X86Reg reads an x86-64 guest register by name (e.g. "rax").
func (r *EmuResult) X86Reg(name string) uint64 {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	return uint64(C.asmtest_emu_x86_reg(r.h, cs))
}

// TB is the subset of *testing.T the Tier-2 assertion helpers use. Declaring our
// own interface (rather than depending on *testing.T directly) keeps the helpers
// usable from any runner and, crucially, testable: the binding's own tests pass a
// recording stub to prove each assertion fails when it should.
type TB interface {
	Helper()
	Fatalf(format string, args ...interface{})
}

// AssertRet fails t unless r's integer return equals want.
func AssertRet(t TB, r *Regs, want int64) {
	t.Helper()
	if got := r.Ret(); got != want {
		t.Fatalf("ret: got %d, want %d", got, want)
	}
}

// AssertABIPreserved fails t unless r's callee-saved registers were restored.
func AssertABIPreserved(t TB, r *Regs) {
	t.Helper()
	if !r.ABIPreserved() {
		t.Fatalf("ABI not preserved")
	}
}

// AssertFlag fails t unless the named flag's state matches set.
func AssertFlag(t TB, r *Regs, name string, set bool) {
	t.Helper()
	if got := r.FlagSet(name); got != set {
		t.Fatalf("flag %s: got %v, want %v", name, got, set)
	}
}

// AssertFP fails t unless r's FP return equals want.
func AssertFP(t TB, r *Regs, want float64) {
	t.Helper()
	if got := r.FRet(); got != want {
		t.Fatalf("fp: got %v, want %v", got, want)
	}
}

// AssertVecF32 fails t unless the four lanes of vector register index equal want.
func AssertVecF32(t TB, r *Regs, index int, want [4]float32) {
	t.Helper()
	if got := r.VecF32(index); got != want {
		t.Fatalf("vec[%d]: got %v, want %v", index, got, want)
	}
}

// AssertNoFault fails t if the emulator run faulted.
func AssertNoFault(t TB, r *EmuResult) {
	t.Helper()
	if r.Faulted() {
		t.Fatalf("unexpected fault")
	}
}

// AssertEmuReg fails t unless the named guest register equals want.
func AssertEmuReg(t TB, r *EmuResult, name string, want uint64) {
	t.Helper()
	if got := r.X86Reg(name); got != want {
		t.Fatalf("emu %s: got %d, want %d", name, got, want)
	}
}
