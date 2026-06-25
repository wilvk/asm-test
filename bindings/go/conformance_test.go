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
