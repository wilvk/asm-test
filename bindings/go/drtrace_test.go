// Tests for the in-process DynamoRIO native-trace wrapper (drtrace.go), mirroring
// the Python suite (bindings/python/tests/test_drtrace.py).
//
// Self-skips unless the tier is built AND DynamoRIO is resolvable — i.e. unless
// ASMTEST_DRAPP_LIB (and ASMTEST_DRCLIENT + ASMTEST_DR_LIB / DYNAMORIO_HOME) point
// at a built libasmtest_drapp + client on a DynamoRIO-capable Linux x86-64 host.
// The `make docker-drtrace` lane sets these up in a container; on a dev box build
// with `make shared-drtrace drtrace-client DYNAMORIO_HOME=...` and export the env.
package asmtest

import "testing"

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
var drtraceRoutine = []byte{
	0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00,
	0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3,
}

func TestDrtrace(t *testing.T) {
	if !NativeTraceAvailable() {
		t.Skip("DynamoRIO native-trace tier unavailable (self-skip)")
	}
	if err := NativeTraceInitializeDefault(); err != nil {
		t.Skipf("dr_init/start failed: %v", err)
	}
	defer NativeTraceShutdown()

	// --- block coverage + accumulation across calls --------------------------
	code, err := NativeCodeFromBytes(drtraceRoutine)
	if err != nil {
		t.Fatalf("NativeCodeFromBytes: %v", err)
	}
	tr := NewNativeTrace(64, 0) // blocks=64, instructions=0
	if err := tr.Register("add2", code); err != nil {
		t.Fatalf("Register: %v", err)
	}

	var r int64
	tr.Region("add2", func() { r = code.Call(20, 22) })
	if r != 42 {
		t.Fatalf("Call(20,22): got %d, want 42", r)
	}
	if !tr.Covered(0) {
		t.Fatalf("entry block (offset 0) expected covered")
	}

	before := tr.BlocksLen()
	tr.Region("add2", func() { r = code.Call(60, 60) }) // 120 > 100 -> dec -> 119
	if r != 119 {
		t.Fatalf("Call(60,60): got %d, want 119", r)
	}
	if tr.BlocksLen() < before {
		t.Fatalf("BlocksLen shrank: %d < %d", tr.BlocksLen(), before)
	}
	if NativeTraceMarkerError() != 0 {
		t.Fatalf("marker error count: got %d, want 0", NativeTraceMarkerError())
	}

	tr.Unregister("add2")
	code.Free()
	tr.Free()

	// --- instruction mode ----------------------------------------------------
	code2, err := NativeCodeFromBytes(drtraceRoutine)
	if err != nil {
		t.Fatalf("NativeCodeFromBytes(2): %v", err)
	}
	tr2 := NewNativeTrace(64, 64) // blocks=64, instructions=64
	if err := tr2.Register("add2i", code2); err != nil {
		t.Fatalf("Register(add2i): %v", err)
	}
	var r2 int64
	tr2.Region("add2i", func() { r2 = code2.Call(1, 2) })
	if r2 != 3 {
		t.Fatalf("Call(1,2): got %d, want 3", r2)
	}
	if tr2.InsnsTotal() < 4 {
		t.Fatalf("InsnsTotal: got %d, want >= 4", tr2.InsnsTotal())
	}
	wantInsns := []uint64{0x0, 0x3, 0x6, 0xc, 0x11}
	gotInsns := tr2.InsnOffsets()
	if len(gotInsns) != len(wantInsns) {
		t.Fatalf("InsnOffsets: got %v, want %v", gotInsns, wantInsns)
	}
	for i := range wantInsns {
		if gotInsns[i] != wantInsns[i] {
			t.Fatalf("InsnOffsets: got %v, want %v", gotInsns, wantInsns)
		}
	}
	hasZero := false
	for _, off := range tr2.BlockOffsets() {
		if off == 0 {
			hasZero = true
			break
		}
	}
	if !hasZero {
		t.Fatalf("BlockOffsets %v: expected to contain 0", tr2.BlockOffsets())
	}

	tr2.Unregister("add2i")
	code2.Free()
	tr2.Free()
}
