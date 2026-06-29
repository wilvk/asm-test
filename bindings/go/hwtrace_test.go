// Tests for the single-step hardware-trace wrapper (hwtrace.go), mirroring the
// Python suite (bindings/python/tests/test_hwtrace.py).
//
// Unlike the DynamoRIO wrapper (which needs a DynamoRIO install) and the PT/AMD
// backends (which need specific bare-metal hardware), the SINGLESTEP backend runs
// on ANY x86-64 Linux — so this asserts a real, live trace here and in
// CI/containers, self-skipping only off x86-64 Linux (or when libasmtest_hwtrace
// is not built / resolvable via $ASMTEST_HWTRACE_LIB).
package asmtest

import "testing"

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
var hwtraceRoutine = []byte{
	0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00,
	0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3,
}

// mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret  (a tight back-edge loop)
var hwtraceLoop = []byte{
	0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00,
	0x48, 0x01, 0xF8, 0x48, 0xFF, 0xCE, 0x75, 0xF8, 0xC3,
}

func TestHwtrace(t *testing.T) {
	if !HwTraceAvailable(SingleStep) {
		t.Skipf("single-step backend unavailable: %s", HwTraceSkipReason(SingleStep))
	}
	if err := HwTraceInit(SingleStep); err != nil {
		t.Skipf("hwtrace init failed: %v", err)
	}
	defer HwTraceShutdown()

	// --- two-block routine: full instruction stream ---------------------------
	code, err := HwNativeCodeFromBytes(hwtraceRoutine)
	if err != nil {
		t.Fatalf("HwNativeCodeFromBytes: %v", err)
	}
	tr := NewHwTrace(64, 64) // blocks=64, instructions=64
	if err := tr.Register("add2", code); err != nil {
		t.Fatalf("Register: %v", err)
	}

	var r int64
	tr.Region("add2", func() { r = code.Call(20, 22) }) // 42 <= 100 -> jle taken, dec skipped
	if r != 42 {
		t.Fatalf("Call(20,22): got %d, want 42", r)
	}

	// Byte-for-byte the Unicorn/DynamoRIO/PT/AMD result for this fixture.
	wantInsns := []uint64{0x0, 0x3, 0x6, 0xC, 0x11}
	gotInsns := tr.InsnOffsets()
	if len(gotInsns) != len(wantInsns) {
		t.Fatalf("InsnOffsets: got %v, want %v", gotInsns, wantInsns)
	}
	for i := range wantInsns {
		if gotInsns[i] != wantInsns[i] {
			t.Fatalf("InsnOffsets: got %v, want %v", gotInsns, wantInsns)
		}
	}
	if tr.InsnsTotal() != 5 {
		t.Fatalf("InsnsTotal: got %d, want 5", tr.InsnsTotal())
	}
	if !tr.Covered(0) || !tr.Covered(0x11) {
		t.Fatalf("expected offsets 0 and 0x11 covered (covered(0)=%v, covered(0x11)=%v)",
			tr.Covered(0), tr.Covered(0x11))
	}
	if tr.BlocksLen() != 2 {
		t.Fatalf("BlocksLen: got %d, want 2", tr.BlocksLen())
	}
	if tr.Truncated() {
		t.Fatalf("Truncated: got true, want false")
	}

	tr.Free()
	code.Free()

	// --- loop: many back-edges, all captured (no depth ceiling) ---------------
	loop, err := HwNativeCodeFromBytes(hwtraceLoop)
	if err != nil {
		t.Fatalf("HwNativeCodeFromBytes(loop): %v", err)
	}
	tr2 := NewHwTrace(64, 256) // blocks=64, instructions=256
	if err := tr2.Register("loop", loop); err != nil {
		t.Fatalf("Register(loop): %v", err)
	}

	var r2 int64
	tr2.Region("loop", func() { r2 = loop.Call(1, 20) })
	if r2 != 20 {
		t.Fatalf("Call(1,20): got %d, want 20", r2)
	}
	if tr2.InsnsTotal() != 62 { // 1 + 20*3 + 1, all captured
		t.Fatalf("InsnsTotal: got %d, want 62", tr2.InsnsTotal())
	}
	if !tr2.Covered(0) || !tr2.Covered(0x7) {
		t.Fatalf("expected offsets 0 and 0x7 covered (covered(0)=%v, covered(0x7)=%v)",
			tr2.Covered(0), tr2.Covered(0x7))
	}
	if tr2.BlocksLen() != 2 {
		t.Fatalf("BlocksLen: got %d, want 2", tr2.BlocksLen())
	}
	if tr2.Truncated() {
		t.Fatalf("Truncated: got true, want false")
	}

	tr2.Free()
	loop.Free()
}
