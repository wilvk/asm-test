// conformance_test.go — asm-test Go binding (Track G).
//
// Replays the cross-language conformance corpus through `go test`, the idiomatic
// Go runner, using the opaque-handle FFI layer. The cases mirror the C reference
// (bindings/conformance/conformance.c) and the other language bindings: integer
// capture + ABI preservation, an ABI violation that must be detected, flag
// capture, FP capture, and an x86-64 emulator case (faults as data).
package asmtest

import (
	"fmt"
	"testing"
)

// --- Tier 1: corpus replay (capture trampoline) ----------------------------

func TestCapture(t *testing.T) {
	cases := []struct {
		name    string
		routine string
		a0, a1  int64
		wantRet int64
	}{
		{"add_signed", "add_signed", 40, 2, 42},
		{"sum_via_rbx", "sum_via_rbx", 20, 22, 42},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			r := NewRegs()
			defer r.Free()
			r.Capture6(CorpusRoutine(c.routine), c.a0, c.a1)
			AssertRet(t, r, c.wantRet)
			AssertABIPreserved(t, r)
		})
	}
}

func TestABIViolationDetected(t *testing.T) {
	r := NewRegs()
	defer r.Free()
	r.Capture6(CorpusRoutine("clobbers_rbx"), 1, 2)
	if r.ABIPreserved() {
		t.Fatal("clobbers_rbx: expected an ABI-preservation violation, got none")
	}
}

func TestFlags(t *testing.T) {
	set := NewRegs()
	defer set.Free()
	set.Capture6(CorpusRoutine("set_carry"))
	AssertFlag(t, set, "CF", true)

	clear := NewRegs()
	defer clear.Free()
	clear.Capture6(CorpusRoutine("clear_carry"))
	AssertFlag(t, clear, "CF", false)
}

func TestFP(t *testing.T) {
	r := NewRegs()
	defer r.Free()
	r.CaptureFP2(CorpusRoutine("fp_add"), 1.5, 2.25)
	AssertFP(t, r, 3.75)
}

func TestVec(t *testing.T) {
	r := NewRegs()
	defer r.Free()
	r.CaptureVecF32(CorpusRoutine("vec_add4f"), [][4]float32{{1, 2, 3, 4}, {10, 20, 30, 40}})
	AssertVecF32(t, r, 0, [4]float32{11, 22, 33, 44})
}

// --- Tier 1: corpus replay (emulator, x86-64 guest) ------------------------

func TestEmu(t *testing.T) {
	e := NewEmu()
	defer e.Close()
	res := NewEmuResult()
	defer res.Free()
	e.Call2(CorpusRoutine("add_signed"), 40, 2, res)
	AssertNoFault(t, res)
	AssertEmuReg(t, res, "rax", 42)
}

// read_fault dereferences an unmapped guest address: the fault is data — where
// (FaultAddr) and why (FaultKind) — not a crash.
func TestEmuFault(t *testing.T) {
	const faultAddr = 0x00DEAD00
	e := NewEmu()
	defer e.Close()
	res := NewEmuResult()
	defer res.Free()
	e.Call2(CorpusRoutine("read_fault"), faultAddr, 0, res)
	AssertFault(t, res)
	if got := res.FaultAddr(); got != faultAddr {
		t.Fatalf("fault_addr: got %#x, want %#x", got, faultAddr)
	}
	if got := res.FaultKind(); got != FaultRead {
		t.Fatalf("fault_kind: got %d, want %d (read)", got, FaultRead)
	}
}

// int_to_double lands (double)42 in xmm0, so the emulator's XMM file is readable
// beyond the GP registers; a clean run also keeps rflags live (bit 1 always set).
func TestEmuXmmAndRflags(t *testing.T) {
	e := NewEmu()
	defer e.Close()
	res := NewEmuResult()
	defer res.Free()
	e.Call2(CorpusRoutine("int_to_double"), 42, 0, res)
	AssertNoFault(t, res)
	if got := res.XmmF64(0, 0); got != 42.0 {
		t.Fatalf("xmm0.f64[0]: got %v, want 42", got)
	}
	if res.X86Reg("rflags")&0x2 == 0 {
		t.Fatal("rflags: reserved bit 1 should be set (extended register read)")
	}
}

// --- Tier 1: cross-arch emulator guests (raw bytes, any host) --------------

// Each guest runs a tiny `add` routine assembled for its ISA (checked-in bytes),
// emulated regardless of the host arch. Mirrors the C reference's emu_<arch>.add.
func TestCrossArchEmu(t *testing.T) {
	cases := []struct {
		arch string
		code []byte
		reg  string
	}{
		{"arm64", []byte{0x00, 0x00, 0x01, 0x8B, 0xC0, 0x03, 0x5F, 0xD6}, "x0"},
		{"riscv", []byte{0x33, 0x05, 0xB5, 0x00, 0x67, 0x80, 0x00, 0x00}, "a0"},
		{"arm", []byte{0x01, 0x00, 0x80, 0xE0, 0x1E, 0xFF, 0x2F, 0xE1}, "r0"},
	}
	for _, c := range cases {
		t.Run(c.arch, func(t *testing.T) {
			g := NewGuest(c.arch)
			defer g.Close()
			res := NewGuestResult(c.arch)
			defer res.Free()
			g.Call(c.code, []int64{40, 2}, res)
			if res.Faulted() {
				t.Fatalf("%s add: unexpected fault", c.arch)
			}
			AssertGuestReg(t, res, c.reg, 42)
		})
	}
}

// --- Tier 1: extended x86-64 emulator calls (raw bytes) --------------------

func TestEmuWideInt(t *testing.T) {
	// mov rax,rdi; add rax,rsi; add rax,rdx; ret — three args via CallBytes.
	code := []byte{0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x01, 0xD0, 0xC3}
	e := NewEmu()
	defer e.Close()
	res := NewEmuResult()
	defer res.Free()
	e.CallBytes(code, []int64{10, 20, 12}, res)
	AssertEmuReg(t, res, "rax", 42)
}

func TestEmuFP(t *testing.T) {
	code := []byte{0xF2, 0x0F, 0x58, 0xC1, 0xC3} // addsd xmm0,xmm1; ret
	e := NewEmu()
	defer e.Close()
	res := NewEmuResult()
	defer res.Free()
	e.CallFP(code, nil, []float64{1.5, 2.25}, res)
	if got := res.XmmF64(0, 0); got != 3.75 {
		t.Fatalf("xmm0: got %v, want 3.75", got)
	}
}

func TestEmuVec(t *testing.T) {
	code := []byte{0x0F, 0x58, 0xC1, 0xC3} // addps xmm0,xmm1; ret
	e := NewEmu()
	defer e.Close()
	res := NewEmuResult()
	defer res.Free()
	e.CallVec(code, nil, [][4]float32{{1, 2, 3, 4}, {10, 20, 30, 40}}, res)
	want := [4]float32{11, 22, 33, 44}
	for i := 0; i < 4; i++ {
		if got := res.XmmF32(0, i); got != want[i] {
			t.Fatalf("xmm0 lane %d: got %v, want %v", i, got, want[i])
		}
	}
}

func TestEmuWin64(t *testing.T) {
	code := []byte{0x48, 0x89, 0xC8, 0x48, 0x01, 0xD0, 0xC3} // mov rax,rcx; add rax,rdx; ret
	e := NewEmu()
	defer e.Close()
	res := NewEmuResult()
	defer res.Free()
	e.CallWin64(code, []int64{40, 2}, res)
	AssertEmuReg(t, res, "rax", 42)
}

// --- Tier 1: execution trace / coverage (cross-arch arm64) -----------------

// A two-block select (x0==0 ? 42 : 99). With x0=0 the entry block (offset 0) and
// the .zero block (offset 12) are entered, the x0!=0 block (offset 4) is not.
func TestEmuTrace(t *testing.T) {
	code := []byte{
		0x60, 0x00, 0x00, 0xB4, 0x60, 0x0C, 0x80, 0xD2, 0xC0, 0x03, 0x5F, 0xD6,
		0x40, 0x05, 0x80, 0xD2, 0xC0, 0x03, 0x5F, 0xD6,
	}
	g := NewGuest("arm64")
	defer g.Close()
	res := NewGuestResult("arm64")
	defer res.Free()
	tr := NewTrace(64, 64)
	defer tr.Free()
	g.CallTraced(code, []int64{0}, tr, res)
	AssertGuestReg(t, res, "x0", 42)
	AssertCovered(t, tr, 0)
	AssertCovered(t, tr, 12)
	if tr.Covered(4) {
		t.Fatal("block 4 should not be covered for x0=0")
	}
}

// --- Tier 1: in-line assembly (Keystone), only when the lib carries it ------

func TestInlineAsm(t *testing.T) {
	if !AsmAvailable() {
		t.Skip("in-line assembler not in this build (run via `make go-asm-test`)")
	}
	e := NewEmu()
	defer e.Close()

	res := NewEmuResult()
	defer res.Free()
	if err := e.CallAsm("mov rax, rdi; add rax, rsi; ret", []int64{40, 2}, SyntaxIntel, 0, res); err != nil {
		t.Fatalf("CallAsm: %v", err)
	}
	AssertNoFault(t, res)
	AssertEmuReg(t, res, "rax", 42)

	// Widened shim: AT&T syntax + a third arg (rdi+rsi+rdx).
	att := NewEmuResult()
	defer att.Free()
	if err := e.CallAsm("mov %rdi, %rax; add %rsi, %rax; add %rdx, %rax; ret",
		[]int64{10, 20, 12}, SyntaxAtt, 0, att); err != nil {
		t.Fatalf("CallAsm (AT&T): %v", err)
	}
	AssertEmuReg(t, att, "rax", 42)

	// Failure path: a bad string returns an error carrying the diagnostic.
	bad := NewEmuResult()
	defer bad.Free()
	if err := e.CallAsm("mov rax, nonsense_token", nil, SyntaxIntel, 0, bad); err == nil {
		t.Fatal("CallAsm: expected an error on un-assemblable source")
	}

	// Multi-arch assemble-to-bytes: AArch64 `ret` is C0 03 5F D6.
	a64, err := Assemble("ret", ArchArm64, SyntaxIntel, 0x00100000)
	if err != nil {
		t.Fatalf("Assemble arm64: %v", err)
	}
	if len(a64) != 4 || a64[0] != 0xC0 || a64[3] != 0xD6 {
		t.Fatalf("arm64 `ret`: got % x", a64)
	}
}

// TestDisas exercises the disassembler tier (Capstone): decode known x86-64
// bytes back to text. Runs by default — libasmtest_emu (the superset) carries
// Capstone; the DisasAvailable() probe only self-skips against an older/leaner
// lib pointed to by ASMTEST_LIB.
func TestDisas(t *testing.T) {
	if !DisasAvailable() {
		t.Skip("disassembler not in this build (run via `make go-asm-test`)")
	}
	code := []byte{0x48, 0x31, 0xC0, 0xC3} // xor rax, rax ; ret
	if got := Disas(code, 0, ArchX8664, 0x00100000); got != "xor rax, rax" {
		t.Fatalf("disas @0: got %q", got)
	}
	if got := Disas(code, 3, ArchX8664, 0x00100000); got != "ret" {
		t.Fatalf("disas @3: got %q", got)
	}
	if got := Disas([]byte{0x90}, 0, ArchX8664, 0x00100000); got != "nop" {
		t.Fatalf("disas nop: got %q", got)
	}
}

// --- Tier 2: idiomatic assertions pass on good input -----------------------

func TestTier2Pass(t *testing.T) {
	r := NewRegs()
	defer r.Free()
	r.Capture6(CorpusRoutine("add_signed"), 40, 2)
	AssertRet(t, r, 42)
	AssertABIPreserved(t, r)

	f := NewRegs()
	defer f.Free()
	f.CaptureFP2(CorpusRoutine("fp_add"), 1.5, 2.25)
	AssertFP(t, f, 3.75)
}

// recorder is a TB stub that captures a failure instead of aborting, so a test
// can prove an assertion bites without failing itself.
type recorder struct {
	failed bool
	msg    string
}

func (r *recorder) Helper() {}
func (r *recorder) Fatalf(format string, args ...interface{}) {
	r.failed = true
	r.msg = fmt.Sprintf(format, args...)
}

// --- Tier 2: the assertions actually fail when they should -----------------

func TestTier2HaveTeeth(t *testing.T) {
	r := NewRegs()
	defer r.Free()
	r.Capture6(CorpusRoutine("add_signed"), 40, 2)

	rec := &recorder{}
	AssertRet(rec, r, 99) // wrong on purpose
	if !rec.failed {
		t.Fatal("AssertRet did not fail on a wrong return value")
	}
}

// Track F: mid-execution guards (byte-literal routines).
func TestGuards(t *testing.T) {
	e := NewEmu()
	defer e.Close()
	res := NewEmuResult()
	defer res.Free()
	twoWrites := []byte{0x48, 0x89, 0x07, 0x48, 0x89, 0x87, 0x00, 0x08, 0x00, 0x00, 0xC3}
	if !e.Map(0x400000, 0x1000) {
		t.Fatal("Map failed")
	}
	w := e.WatchWrites(0x400000, 8, "only")
	defer w.Free()
	e.CallBytes(twoWrites, []int64{0x400000}, res)
	e.WatchClear()
	if !w.Violated() || w.Addr() != 0x400800 || w.RipOff() != 3 {
		t.Fatalf("watch: violated=%v addr=%#x rip=%d", w.Violated(), w.Addr(), w.RipOff())
	}
	clobber := []byte{0x48, 0xC7, 0xC3, 0x99, 0x00, 0x00, 0x00, 0xEB, 0x00, 0xC3}
	g, ok := e.GuardReg("rbx", 0)
	if !ok {
		t.Fatal("GuardReg rbx failed")
	}
	defer g.Free()
	e.CallBytes(clobber, nil, res)
	e.GuardRegClear()
	if !g.Violated() || g.Got() != 0x99 {
		t.Fatalf("regguard: violated=%v got=%#x", g.Violated(), g.Got())
	}
	if _, ok := e.GuardReg("nope", 0); ok {
		t.Fatal("GuardReg accepted an unknown register")
	}
}

// Track E: coverage-guided fuzzing + mutation testing over classify3.
func TestFuzzMutation(t *testing.T) {
	classify3 := []byte{
		0x31, 0xC0, 0x48, 0x85, 0xFF, 0x78, 0x0B, 0x48, 0x85, 0xFF, 0x74, 0x05,
		0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3}
	e := NewEmu()
	defer e.Close()
	fixed, _ := e.FuzzCover(classify3, 5, 5, 1)
	guided, _ := e.FuzzCover(classify3, -50, 50, 2000)
	if guided <= fixed {
		t.Fatalf("fuzz: guided %d <= fixed %d", guided, fixed)
	}
	_, weakS := e.MutationTest(classify3, []int64{5})
	_, strongS := e.MutationTest(classify3, []int64{-7, 0, 9})
	if weakS == 0 || strongS >= weakS {
		t.Fatalf("mutation: weak survived %d, strong survived %d", weakS, strongS)
	}
}

// Track D: AVX2 256-bit capture (self-skips off-AVX2).
func TestVec256(t *testing.T) {
	if !CPUHasAVX2() {
		t.Skip("AVX2 not available on this host")
	}
	out := CaptureVec256(CorpusRoutine("vec_add4d"),
		[][4]float64{{1, 2, 3, 4}, {10, 20, 30, 40}})
	if want := [4]float64{11, 22, 33, 44}; out != want {
		t.Fatalf("vec256: got %v, want %v", out, want)
	}
}

// Track D: AVX-512 512-bit capture (self-skips off-AVX-512F).
func TestVec512(t *testing.T) {
	if !CPUHasAVX512F() {
		t.Skip("AVX-512F not available on this host")
	}
	out := CaptureVec512(vecAdd8dRoutine(),
		[][8]float64{{1, 2, 3, 4, 5, 6, 7, 8}, {10, 20, 30, 40, 50, 60, 70, 80}})
	if want := [8]float64{11, 22, 33, 44, 55, 66, 77, 88}; out != want {
		t.Fatalf("vec512: got %v, want %v", out, want)
	}
}
