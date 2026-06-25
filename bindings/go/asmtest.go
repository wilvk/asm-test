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
extern unsigned long long asmtest_emu_result_fault_addr(void *r);
extern int   asmtest_emu_result_fault_kind(void *r);
extern unsigned long long asmtest_emu_x86_reg(void *r, const char *name);
extern double asmtest_emu_x86_xmm_f64(void *r, int index, int lane);
extern float  asmtest_emu_x86_xmm_f32(void *r, int index, int lane);

// Extended x86-64 emulator calls (array form: explicit code + length, so raw
// machine-code bytes run directly): wide integer args, FP args, vector args, the
// Win64 convention, and trace recording.
extern int emu_call(void *e, void *code, size_t len, long *args, int nargs, unsigned long long mi, void *out);
extern int emu_call_fp(void *e, void *code, size_t len, long *ia, int ni, double *fa, int nf, unsigned long long mi, void *out);
extern int emu_call_vec(void *e, void *code, size_t len, long *ia, int ni, void *va, int nv, unsigned long long mi, void *out);
extern int emu_call_win64(void *e, void *code, size_t len, long *args, int nargs, unsigned long long mi, void *out);
extern int emu_call_traced(void *e, void *code, size_t len, long *args, int nargs, unsigned long long mi, void *out, void *trace);

// Cross-arch guests (raw bytes, emulated regardless of host): AArch64, RISC-V,
// ARM32. Each opens its own engine and writes a per-arch result struct, read by
// the opaque accessors below.
extern void *emu_arm64_open(void); extern void emu_arm64_close(void *e);
extern int emu_arm64_call(void *e, void *code, size_t len, long *args, int nargs, unsigned long long mi, void *out);
extern int emu_arm64_call_fp(void *e, void *code, size_t len, long *ia, int ni, double *fa, int nf, unsigned long long mi, void *out);
extern int emu_arm64_call_vec(void *e, void *code, size_t len, long *ia, int ni, void *va, int nv, unsigned long long mi, void *out);
extern int emu_arm64_call_traced(void *e, void *code, size_t len, long *args, int nargs, unsigned long long mi, void *out, void *trace);
extern void *emu_riscv_open(void); extern void emu_riscv_close(void *e);
extern int emu_riscv_call(void *e, void *code, size_t len, long *args, int nargs, unsigned long long mi, void *out);
extern int emu_riscv_call_fp(void *e, void *code, size_t len, long *ia, int ni, double *fa, int nf, unsigned long long mi, void *out);
extern int emu_riscv_call_traced(void *e, void *code, size_t len, long *args, int nargs, unsigned long long mi, void *out, void *trace);
extern void *emu_arm_open(void); extern void emu_arm_close(void *e);
extern int emu_arm_call(void *e, void *code, size_t len, long *args, int nargs, unsigned long long mi, void *out);
extern int emu_arm_call_fp(void *e, void *code, size_t len, long *ia, int ni, double *fa, int nf, unsigned long long mi, void *out);
extern int emu_arm_call_vec(void *e, void *code, size_t len, long *ia, int ni, void *va, int nv, unsigned long long mi, void *out);
extern int emu_arm_call_traced(void *e, void *code, size_t len, long *args, int nargs, unsigned long long mi, void *out, void *trace);

// Per-arch opaque result handles + register accessors.
extern void *asmtest_emu_arm64_result_new(void); extern void asmtest_emu_arm64_result_free(void *r);
extern unsigned long long asmtest_emu_arm64_reg(void *r, const char *name);
extern double asmtest_emu_arm64_vec_f64(void *r, int index, int lane);
extern void *asmtest_emu_riscv_result_new(void); extern void asmtest_emu_riscv_result_free(void *r);
extern unsigned long long asmtest_emu_riscv_reg(void *r, const char *name);
extern double asmtest_emu_riscv_f_f64(void *r, int index, int lane);
extern void *asmtest_emu_arm_result_new(void); extern void asmtest_emu_arm_result_free(void *r);
extern unsigned long long asmtest_emu_arm_reg(void *r, const char *name);
extern double asmtest_emu_arm_q_f64(void *r, int index, int lane);

// Opaque execution-trace handle.
extern void *asmtest_emu_trace_new(size_t ic, size_t bc); extern void asmtest_emu_trace_free(void *t);
extern int asmtest_emu_trace_covered(void *t, unsigned long long off);
extern unsigned long long asmtest_emu_trace_insns_total(void *t);
extern unsigned long long asmtest_emu_trace_blocks_len(void *t);

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

// --- helpers for passing Go slices to C (no Go pointers inside the slices) --- //

func clongPtr(a []int64) *C.long {
	if len(a) == 0 {
		return nil
	}
	return (*C.long)(unsafe.Pointer(&a[0]))
}

func cbytePtr(b []byte) unsafe.Pointer {
	if len(b) == 0 {
		return nil
	}
	return unsafe.Pointer(&b[0])
}

func cdoublePtr(f []float64) *C.double {
	if len(f) == 0 {
		return nil
	}
	return (*C.double)(unsafe.Pointer(&f[0]))
}

// flattenVecs lays out vectors as contiguous 16-byte (four float32) lanes, the
// emu_vec128_t array layout the marshalling expects.
func flattenVecs(vargs [][4]float32) []float32 {
	flat := make([]float32, 0, len(vargs)*4)
	for _, v := range vargs {
		flat = append(flat, v[0], v[1], v[2], v[3])
	}
	return flat
}

func cvecPtr(flat []float32) unsafe.Pointer {
	if len(flat) == 0 {
		return nil
	}
	return unsafe.Pointer(&flat[0])
}

// CallBytes runs raw x86-64 machine-code bytes with up to six integer args (more
// than Call2), filling out.
func (e *Emu) CallBytes(code []byte, args []int64, out *EmuResult) {
	C.emu_call(e.h, cbytePtr(code), C.size_t(len(code)), clongPtr(args),
		C.int(len(args)), 0, out.h)
}

// CallFP runs raw x86-64 bytes marshalling doubles into the FP arg registers
// (xmm0..7); the scalar double return is out.XmmF64(0, 0).
func (e *Emu) CallFP(code []byte, iargs []int64, fargs []float64, out *EmuResult) {
	C.emu_call_fp(e.h, cbytePtr(code), C.size_t(len(code)), clongPtr(iargs),
		C.int(len(iargs)), cdoublePtr(fargs), C.int(len(fargs)), 0, out.h)
}

// CallVec runs raw x86-64 bytes marshalling 128-bit vectors into xmm0..7; a
// vector return is read with out.XmmF32(0, lane).
func (e *Emu) CallVec(code []byte, iargs []int64, vargs [][4]float32, out *EmuResult) {
	flat := flattenVecs(vargs)
	C.emu_call_vec(e.h, cbytePtr(code), C.size_t(len(code)), clongPtr(iargs),
		C.int(len(iargs)), cvecPtr(flat), C.int(len(vargs)), 0, out.h)
}

// CallWin64 runs raw x86-64 bytes under the Microsoft x64 convention (integer
// args in rcx, rdx, r8, r9), so a Win64 routine can be tested on a System V host.
func (e *Emu) CallWin64(code []byte, args []int64, out *EmuResult) {
	C.emu_call_win64(e.h, cbytePtr(code), C.size_t(len(code)), clongPtr(args),
		C.int(len(args)), 0, out.h)
}

// CallTraced runs raw x86-64 bytes recording an execution trace / coverage into
// tr, filling out.
func (e *Emu) CallTraced(code []byte, args []int64, tr *Trace, out *EmuResult) {
	C.emu_call_traced(e.h, cbytePtr(code), C.size_t(len(code)), clongPtr(args),
		C.int(len(args)), 0, out.h, tr.h)
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

// FaultKind classifies an invalid access; only meaningful when Faulted.
type FaultKind int

const (
	FaultNone  FaultKind = iota // 0: no fault
	FaultRead                   // 1: invalid read
	FaultWrite                  // 2: invalid write
	FaultFetch                  // 3: invalid instruction fetch
)

// FaultAddr is the faulting guest address; only meaningful when Faulted.
func (r *EmuResult) FaultAddr() uint64 { return uint64(C.asmtest_emu_result_fault_addr(r.h)) }

// FaultKind reports why the access was invalid (read/write/fetch); only
// meaningful when Faulted.
func (r *EmuResult) FaultKind() FaultKind {
	return FaultKind(C.asmtest_emu_result_fault_kind(r.h))
}

// X86Reg reads an x86-64 guest register by name — the GP file plus "rip" and
// "rflags" (e.g. "rax", "rip", "rflags").
func (r *EmuResult) X86Reg(name string) uint64 {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	return uint64(C.asmtest_emu_x86_reg(r.h, cs))
}

// XmmF64 reads lane (0..1) of guest XMM register index (0..15) as a double — the
// FP/vector side of the file (a scalar double return is XmmF64(0, 0)).
func (r *EmuResult) XmmF64(index, lane int) float64 {
	return float64(C.asmtest_emu_x86_xmm_f64(r.h, C.int(index), C.int(lane)))
}

// XmmF32 reads lane (0..3) of guest XMM register index (0..15) as a float32.
func (r *EmuResult) XmmF32(index, lane int) float32 {
	return float32(C.asmtest_emu_x86_xmm_f32(r.h, C.int(index), C.int(lane)))
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

// AssertFault fails t unless the run faulted (a fault is data, not a crash).
func AssertFault(t TB, r *EmuResult) {
	t.Helper()
	if !r.Faulted() {
		t.Fatalf("expected a fault, but the run completed cleanly")
	}
}

// AssertEmuReg fails t unless the named guest register equals want.
func AssertEmuReg(t TB, r *EmuResult, name string, want uint64) {
	t.Helper()
	if got := r.X86Reg(name); got != want {
		t.Fatalf("emu %s: got %d, want %d", name, got, want)
	}
}

// ------------------------------------------------------------------ //
// Execution trace / coverage                                          //
// ------------------------------------------------------------------ //

// Trace is an opaque execution-trace / basic-block coverage recorder. Pass it to
// (*Emu).CallTraced or (*Guest).CallTraced; release it with Free.
type Trace struct{ h unsafe.Pointer }

// NewTrace allocates a trace handle with the given instruction / block buffer
// capacities.
func NewTrace(insnsCap, blocksCap int) *Trace {
	return &Trace{h: C.asmtest_emu_trace_new(C.size_t(insnsCap), C.size_t(blocksCap))}
}

// Free releases the handle. Safe to call more than once.
func (t *Trace) Free() {
	if t.h != nil {
		C.asmtest_emu_trace_free(t.h)
		t.h = nil
	}
}

// Covered reports whether the basic block at byte-offset off (from the routine
// entry) was entered.
func (t *Trace) Covered(off uint64) bool {
	return C.asmtest_emu_trace_covered(t.h, C.ulonglong(off)) != 0
}

// InsnsTotal is the number of instructions executed (counts past the buffer cap).
func (t *Trace) InsnsTotal() uint64 { return uint64(C.asmtest_emu_trace_insns_total(t.h)) }

// BlocksLen is the number of distinct basic blocks recorded.
func (t *Trace) BlocksLen() uint64 { return uint64(C.asmtest_emu_trace_blocks_len(t.h)) }

// ------------------------------------------------------------------ //
// Cross-arch emulator guests (AArch64 / RISC-V / ARM32)               //
// ------------------------------------------------------------------ //

// Guest is a cross-arch Unicorn guest ("arm64", "riscv", or "arm") that runs raw
// machine-code bytes — emulated regardless of host architecture. Close it with
// Close.
type Guest struct {
	h    unsafe.Pointer
	arch string
}

// NewGuest opens a guest emulator for arch ("arm64", "riscv", "arm"); nil on an
// unknown arch.
func NewGuest(arch string) *Guest {
	var h unsafe.Pointer
	switch arch {
	case "arm64":
		h = C.emu_arm64_open()
	case "riscv":
		h = C.emu_riscv_open()
	case "arm":
		h = C.emu_arm_open()
	default:
		return nil
	}
	return &Guest{h: h, arch: arch}
}

// Close releases the guest. Safe to call more than once.
func (g *Guest) Close() {
	if g.h == nil {
		return
	}
	switch g.arch {
	case "arm64":
		C.emu_arm64_close(g.h)
	case "riscv":
		C.emu_riscv_close(g.h)
	case "arm":
		C.emu_arm_close(g.h)
	}
	g.h = nil
}

// Call runs raw machine-code bytes with integer args in the guest ABI arg
// registers, filling res.
func (g *Guest) Call(code []byte, args []int64, res *GuestResult) {
	cb, n := cbytePtr(code), C.size_t(len(code))
	la, ln := clongPtr(args), C.int(len(args))
	switch g.arch {
	case "arm64":
		C.emu_arm64_call(g.h, cb, n, la, ln, 0, res.h)
	case "riscv":
		C.emu_riscv_call(g.h, cb, n, la, ln, 0, res.h)
	case "arm":
		C.emu_arm_call(g.h, cb, n, la, ln, 0, res.h)
	}
}

// CallFP runs raw bytes marshalling doubles into the guest FP arg registers; the
// scalar double return is res.VecF64(0, 0).
func (g *Guest) CallFP(code []byte, iargs []int64, fargs []float64, res *GuestResult) {
	cb, n := cbytePtr(code), C.size_t(len(code))
	la, ln := clongPtr(iargs), C.int(len(iargs))
	fa, fn := cdoublePtr(fargs), C.int(len(fargs))
	switch g.arch {
	case "arm64":
		C.emu_arm64_call_fp(g.h, cb, n, la, ln, fa, fn, 0, res.h)
	case "riscv":
		C.emu_riscv_call_fp(g.h, cb, n, la, ln, fa, fn, 0, res.h)
	case "arm":
		C.emu_arm_call_fp(g.h, cb, n, la, ln, fa, fn, 0, res.h)
	}
}

// CallVec runs raw bytes marshalling 128-bit vectors into the guest vector arg
// registers (arm64 / arm only; the RISC-V guest has no vector file).
func (g *Guest) CallVec(code []byte, iargs []int64, vargs [][4]float32, res *GuestResult) {
	flat := flattenVecs(vargs)
	cb, n := cbytePtr(code), C.size_t(len(code))
	la, ln := clongPtr(iargs), C.int(len(iargs))
	va, vn := cvecPtr(flat), C.int(len(vargs))
	switch g.arch {
	case "arm64":
		C.emu_arm64_call_vec(g.h, cb, n, la, ln, va, vn, 0, res.h)
	case "arm":
		C.emu_arm_call_vec(g.h, cb, n, la, ln, va, vn, 0, res.h)
	}
}

// CallTraced runs raw bytes recording an execution trace / coverage into tr.
func (g *Guest) CallTraced(code []byte, args []int64, tr *Trace, res *GuestResult) {
	cb, n := cbytePtr(code), C.size_t(len(code))
	la, ln := clongPtr(args), C.int(len(args))
	switch g.arch {
	case "arm64":
		C.emu_arm64_call_traced(g.h, cb, n, la, ln, 0, res.h, tr.h)
	case "riscv":
		C.emu_riscv_call_traced(g.h, cb, n, la, ln, 0, res.h, tr.h)
	case "arm":
		C.emu_arm_call_traced(g.h, cb, n, la, ln, 0, res.h, tr.h)
	}
}

// GuestResult carries a cross-arch run's outcome; registers are read by name.
// Release it with Free.
type GuestResult struct {
	h    unsafe.Pointer
	arch string
}

// NewGuestResult allocates a per-arch result handle for arch.
func NewGuestResult(arch string) *GuestResult {
	var h unsafe.Pointer
	switch arch {
	case "arm64":
		h = C.asmtest_emu_arm64_result_new()
	case "riscv":
		h = C.asmtest_emu_riscv_result_new()
	case "arm":
		h = C.asmtest_emu_arm_result_new()
	}
	return &GuestResult{h: h, arch: arch}
}

// Free releases the handle. Safe to call more than once.
func (r *GuestResult) Free() {
	if r.h == nil {
		return
	}
	switch r.arch {
	case "arm64":
		C.asmtest_emu_arm64_result_free(r.h)
	case "riscv":
		C.asmtest_emu_riscv_result_free(r.h)
	case "arm":
		C.asmtest_emu_arm_result_free(r.h)
	}
	r.h = nil
}

// Faulted reports whether the guest run took an invalid-memory fault.
func (r *GuestResult) Faulted() bool { return C.asmtest_emu_result_faulted(r.h) != 0 }

// Reg reads a guest register by name (e.g. "x0"/"sp" for arm64, "a0"/"x10" for
// riscv, "r0" for arm).
func (r *GuestResult) Reg(name string) uint64 {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	switch r.arch {
	case "arm64":
		return uint64(C.asmtest_emu_arm64_reg(r.h, cs))
	case "riscv":
		return uint64(C.asmtest_emu_riscv_reg(r.h, cs))
	case "arm":
		return uint64(C.asmtest_emu_arm_reg(r.h, cs))
	}
	return 0
}

// VecF64 reads lane (0..1) of guest vector register index as a double.
func (r *GuestResult) VecF64(index, lane int) float64 {
	switch r.arch {
	case "arm64":
		return float64(C.asmtest_emu_arm64_vec_f64(r.h, C.int(index), C.int(lane)))
	case "arm":
		return float64(C.asmtest_emu_arm_q_f64(r.h, C.int(index), C.int(lane)))
	case "riscv":
		return float64(C.asmtest_emu_riscv_f_f64(r.h, C.int(index), C.int(lane)))
	}
	return 0
}

// AssertGuestReg fails t unless the named guest register equals want.
func AssertGuestReg(t TB, r *GuestResult, name string, want uint64) {
	t.Helper()
	if got := r.Reg(name); got != want {
		t.Fatalf("guest %s: got %d, want %d", name, got, want)
	}
}

// AssertCovered fails t unless the basic block at byte-offset off was entered.
func AssertCovered(t TB, tr *Trace, off uint64) {
	t.Helper()
	if !tr.Covered(off) {
		t.Fatalf("block %d: expected covered", off)
	}
}
