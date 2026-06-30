// Tests for the single-step hardware-trace wrapper (hwtrace.go), mirroring the
// Python suite (bindings/python/tests/test_hwtrace.py).
//
// Unlike the DynamoRIO wrapper (which needs a DynamoRIO install) and the PT/AMD
// backends (which need specific bare-metal hardware), the SINGLESTEP backend runs
// on ANY x86-64 Linux — so this asserts a real, live trace here and in
// CI/containers, self-skipping only off x86-64 Linux (or when libasmtest_hwtrace
// is not built / resolvable via $ASMTEST_HWTRACE_LIB).
package asmtest

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
	"unsafe"
)

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

// TestHwtraceAutoSelection mirrors the Python suite's selection invariants
// (test_auto_resolve_selection_invariants): these hold on EVERY host, even where
// all backends self-skip and the cascade is empty.
func TestHwtraceAutoSelection(t *testing.T) {
	best := HwTraceResolve(Best)
	cf := HwTraceResolve(CeilingFree)

	// Every resolved backend is actually available, ordered by descending fidelity
	// (ascending enum), with no duplicates.
	for i, b := range best {
		if !HwTraceAvailable(b) {
			t.Fatalf("HwTraceResolve(Best) returned unavailable backend %d", b)
		}
		if i > 0 && b <= best[i-1] {
			t.Fatalf("HwTraceResolve(Best) not strictly ascending: %v", best)
		}
	}

	// CeilingFree drops the one fixed-window backend (AMD LBR) and is otherwise a
	// subset of Best.
	for _, b := range cf {
		if b == AmdLBR {
			t.Fatalf("HwTraceResolve(CeilingFree) must not select AmdLBR: %v", cf)
		}
		inBest := false
		for _, bb := range best {
			if bb == b {
				inBest = true
				break
			}
		}
		if !inBest {
			t.Fatalf("HwTraceResolve(CeilingFree) %v is not a subset of Best %v", cf, best)
		}
	}

	// auto(policy) is the head of resolve(policy), or HwEUnavail when empty.
	ab := HwTraceAuto(Best)
	want := HwEUnavail
	if len(best) > 0 {
		want = best[0]
	}
	if ab != want {
		t.Fatalf("HwTraceAuto(Best): got %d, want %d", ab, want)
	}
}

// TestCrossTierResolveInvariants mirrors the Python suite's
// test_cross_tier_resolve_invariants: the cross-tier orchestrator (resolve over
// hwtrace + DynamoRIO + emulator) holds its structural invariants on every host.
func TestCrossTierResolveInvariants(t *testing.T) {
	best := ResolveTiers(TraceBest)
	nat := ResolveTiers(TraceNativeOnly)
	cf := ResolveTiers(TraceCeilingFree)

	if len(best) == 0 {
		t.Skip("cross-tier cascade empty (libasmtest_hwtrace not loaded?)")
	}

	// Every HW choice satisfies the hardware-tier probe; the fidelity class matches
	// the tier (only the emulator tier is virtual).
	for _, c := range best {
		if c.Tier == TierHwtrace && !HwTraceAvailable(c.Backend) {
			t.Fatalf("ResolveTiers(TraceBest) HW choice backend %d not available", c.Backend)
		}
		wantFidelity := FidelityNative
		if c.Tier == TierEmulator {
			wantFidelity = FidelityVirtual
		}
		if c.Fidelity != wantFidelity {
			t.Fatalf("choice %+v: fidelity %d, want %d", c, c.Fidelity, wantFidelity)
		}
	}

	// The single VIRTUAL emulator floor is the last entry under BEST, and appears
	// exactly once.
	if best[len(best)-1].Tier != TierEmulator {
		t.Fatalf("ResolveTiers(TraceBest) last entry tier %d, want emulator (%d)",
			best[len(best)-1].Tier, TierEmulator)
	}
	emuCount := 0
	for _, c := range best {
		if c.Tier == TierEmulator {
			emuCount++
		}
	}
	if emuCount != 1 {
		t.Fatalf("ResolveTiers(TraceBest): %d emulator entries, want exactly 1", emuCount)
	}

	// NATIVE_ONLY forbids the native->emulator crossing: it is BEST minus the floor.
	for _, c := range nat {
		if c.Tier == TierEmulator {
			t.Fatalf("ResolveTiers(TraceNativeOnly) must not include the emulator floor: %+v", nat)
		}
	}
	if len(nat) != len(best)-1 {
		t.Fatalf("ResolveTiers(TraceNativeOnly) len %d, want len(best)-1 = %d", len(nat), len(best)-1)
	}

	// CEILING_FREE drops AMD LBR.
	for _, c := range cf {
		if c.Tier == TierHwtrace && c.Backend == AmdLBR {
			t.Fatalf("ResolveTiers(TraceCeilingFree) must not select AMD LBR: %+v", cf)
		}
	}

	// auto(policy) is the head of resolve(policy).
	one, ok := AutoTier(TraceBest)
	if !ok {
		t.Fatalf("AutoTier(TraceBest) returned EUNAVAIL but resolve was non-empty: %+v", best)
	}
	if one.Tier != best[0].Tier || one.Backend != best[0].Backend {
		t.Fatalf("AutoTier(TraceBest) = %+v, want head of resolve %+v", one, best[0])
	}
}

// TestCrossTierNativeOnlyResolvesOnLinuxAmd64 mirrors the Python suite's
// test_cross_tier_native_only_resolves_on_linux_x86_64: on any x86-64 Linux host
// the single-step backend is a native floor, so even NATIVE_ONLY resolves (the
// cascade never collapses to nothing here).
func TestCrossTierNativeOnlyResolvesOnLinuxAmd64(t *testing.T) {
	if !HwTraceAvailable(SingleStep) {
		t.Skipf("single-step backend unavailable: %s", HwTraceSkipReason(SingleStep))
	}

	nat := ResolveTiers(TraceNativeOnly)
	pick, ok := AutoTier(TraceNativeOnly)
	if len(nat) == 0 || !ok {
		t.Fatalf("NATIVE_ONLY resolved nothing on x86-64 Linux: resolve=%+v ok=%v", nat, ok)
	}
	if pick.Fidelity != FidelityNative {
		t.Fatalf("AutoTier(TraceNativeOnly) fidelity %d, want native (%d)", pick.Fidelity, FidelityNative)
	}

	found := false
	for _, c := range nat {
		if c.Tier == TierHwtrace && c.Backend == SingleStep {
			found = true
			break
		}
	}
	if !found {
		t.Fatalf("NATIVE_ONLY cascade missing the single-step floor: %+v", nat)
	}
}

// TestHwtraceAutoLive mirrors test_auto_resolve_traces_live: on any x86-64 Linux
// host the cascade is non-empty (single-step floor), so auto() resolves a usable
// backend; trace the shared fixture through whatever it picked.
func TestHwtraceAutoLive(t *testing.T) {
	if !HwTraceAvailable(SingleStep) {
		t.Skipf("single-step backend unavailable: %s", HwTraceSkipReason(SingleStep))
	}

	best := HwTraceResolve(Best)
	ab := HwTraceAuto(Best)
	if len(best) == 0 || ab < 0 { // single-step keeps the cascade non-empty here
		t.Fatalf("auto resolved nothing on x86-64 Linux: resolve=%v auto=%d", best, ab)
	}

	if err := HwTraceInit(ab); err != nil {
		t.Skipf("hwtrace init failed: %v", err)
	}
	defer HwTraceShutdown()

	code, err := HwNativeCodeFromBytes(hwtraceRoutine)
	if err != nil {
		t.Fatalf("HwNativeCodeFromBytes: %v", err)
	}
	tr := NewHwTrace(64, 64)
	if err := tr.Register("auto", code); err != nil {
		t.Fatalf("Register: %v", err)
	}

	var r int64
	tr.Region("auto", func() { r = code.Call(20, 22) })
	if r != 42 {
		t.Fatalf("Call(20,22): got %d, want 42", r)
	}
	if !tr.Covered(0) {
		t.Fatalf("expected offset 0 covered")
	}
	if ab == SingleStep { // the pick off PT/AMD hosts: byte-exact parity
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
	}

	tr.Free()
	code.Free()
}

// ---- Out-of-process / foreign-process toolkit (asmtest_ptrace.h) ----
//
// Mirrors the four tests after the "foreign-process toolkit" banner in the Python
// suite (bindings/python/tests/test_hwtrace.py). They self-skip when the ptrace
// backend is unavailable (off x86-64 Linux, or when libasmtest_hwtrace is absent).

func skipIfNoPtrace(t *testing.T) {
	t.Helper()
	if !PtraceAvailable() {
		t.Skipf("ptrace backend unavailable: %s", PtraceSkipReason())
	}
}

// TestPtraceTraceCall forks a tracee, single-steps it out of process, and asserts
// the same offsets as the in-process stepper (Unicorn/DynamoRIO/PT/AMD parity).
func TestPtraceTraceCall(t *testing.T) {
	skipIfNoPtrace(t)
	code, err := HwNativeCodeFromBytes(hwtraceRoutine)
	if err != nil {
		t.Fatalf("HwNativeCodeFromBytes: %v", err)
	}
	defer code.Free()
	tr := NewHwTrace(64, 64) // blocks=64, instructions=64
	defer tr.Free()

	result, err := PtraceTraceCall(unsafe.Pointer(code.Base()), code.Len(), []int64{20, 22}, tr)
	if err != nil {
		t.Fatalf("PtraceTraceCall: %v", err)
	}
	if result != 42 {
		t.Fatalf("PtraceTraceCall(20,22): got %d, want 42", result)
	}
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
	if tr.Truncated() {
		t.Fatalf("Truncated: got true, want false")
	}
}

// TestPtraceRunTo probes the run-to-address primitive's FFI round-trip. run_to drives
// an attached target to a resolved method (software breakpoint); a live foreign attach
// is covered by the C suite (forking + ptrace of a foreign process is impractical here,
// same as PtraceTraceAttached), so this just confirms a NULL target address is rejected
// (EINVAL, non-zero) before any ptrace call.
func TestPtraceRunTo(t *testing.T) {
	skipIfNoPtrace(t)
	if rc := PtraceRunTo(os.Getpid(), 0); rc == 0 {
		t.Fatalf("PtraceRunTo(NULL addr): got OK, want a non-OK (EINVAL) rejection")
	}
}

// TestProcRegionByAddr discovers an executable region's extent from
// /proc/<pid>/maps by an interior address (this process).
func TestProcRegionByAddr(t *testing.T) {
	skipIfNoPtrace(t)
	code, err := HwNativeCodeFromBytes(hwtraceRoutine)
	if err != nil {
		t.Fatalf("HwNativeCodeFromBytes: %v", err)
	}
	defer code.Free()

	base, length, ok := ProcRegionByAddr(os.Getpid(), code.Base()+4)
	if !ok {
		t.Fatalf("ProcRegionByAddr returned !ok for an interior address")
	}
	if base != code.Base() {
		t.Fatalf("ProcRegionByAddr base: got %#x, want %#x", base, code.Base())
	}
	if length < uintptr(len(hwtraceRoutine)) {
		t.Fatalf("ProcRegionByAddr len: got %d, want >= %d", length, len(hwtraceRoutine))
	}
	if _, _, ok := ProcRegionByAddr(os.Getpid(), 0x1); ok { // nothing maps addr 1
		t.Fatalf("ProcRegionByAddr(addr=1): got ok, want !ok")
	}
}

// TestProcPerfmapSymbol parses a JIT perf-map (/tmp/perf-<pid>.map) and resolves a
// method by name.
func TestProcPerfmapSymbol(t *testing.T) {
	skipIfNoPtrace(t)
	pid := os.Getpid()
	path := filepath.Join("/tmp", "perf-"+itoa(pid)+".map")
	if err := os.WriteFile(path, []byte("400000 1a void demo(long, long)\n500000 8 other\n"), 0o644); err != nil {
		t.Fatalf("write perf-map: %v", err)
	}
	defer os.Remove(path)

	base, length, ok := ProcPerfmapSymbol(pid, "void demo(long, long)")
	if !ok || base != 0x400000 || length != 0x1A {
		t.Fatalf("ProcPerfmapSymbol: got (%#x, %#x, %v), want (0x400000, 0x1a, true)", base, length, ok)
	}
	if _, _, ok := ProcPerfmapSymbol(pid, "missing"); ok {
		t.Fatalf("ProcPerfmapSymbol(missing): got ok, want !ok")
	}
}

// TestJitdumpFind writes a binary jitdump (little-endian) per the Python layout
// and resolves a method to (addr,size,index,ts) + recorded code bytes.
func TestJitdumpFind(t *testing.T) {
	skipIfNoPtrace(t)
	dir := t.TempDir()
	path := filepath.Join(dir, "jit.dump")
	name := []byte("void demo(long, long)")

	var buf bytes.Buffer
	le := binary.LittleEndian
	w32 := func(v uint32) {
		var b [4]byte
		le.PutUint32(b[:], v)
		buf.Write(b[:])
	}
	w64 := func(v uint64) {
		var b [8]byte
		le.PutUint64(b[:], v)
		buf.Write(b[:])
	}
	// header: magic, version, total_size=40, elf_mach=62, pad1=0, pid=0, timestamp=0, flags=0
	w32(0x4A695444)
	w32(1)
	w32(40)
	w32(62)
	w32(0)
	w32(0)
	w64(0)
	w64(0)
	// JIT_CODE_LOAD record: id=0, total_size, timestamp=5
	total := uint32(16 + 40 + (len(name) + 1) + len(hwtraceRoutine))
	w32(0)
	w32(total)
	w64(5)
	// body: pid=0, tid=0, vma=0x2000, code_addr=0x2000, code_size, code_index=9
	w32(0)
	w32(0)
	w64(0x2000)
	w64(0x2000)
	w64(uint64(len(hwtraceRoutine)))
	w64(9)
	buf.Write(name)
	buf.WriteByte(0)
	buf.Write(hwtraceRoutine)

	if err := os.WriteFile(path, buf.Bytes(), 0o644); err != nil {
		t.Fatalf("write jitdump: %v", err)
	}

	m, ok := JitdumpFind(path, "void demo(long, long)", 0, 64)
	if !ok {
		t.Fatalf("JitdumpFind: got !ok, want a method")
	}
	if m.CodeAddr != 0x2000 || m.CodeSize != uint64(len(hwtraceRoutine)) ||
		m.CodeIndex != 9 || m.Timestamp != 5 {
		t.Fatalf("JitdumpFind: got (addr=%#x, size=%d, index=%d, ts=%d), want (0x2000, %d, 9, 5)",
			m.CodeAddr, m.CodeSize, m.CodeIndex, m.Timestamp, len(hwtraceRoutine))
	}
	if !bytes.Equal(m.Code, hwtraceRoutine) {
		t.Fatalf("JitdumpFind code: got %x, want %x", m.Code, hwtraceRoutine)
	}
	if _, ok := JitdumpFind(path, "missing", 0, 0); ok {
		t.Fatalf("JitdumpFind(missing): got ok, want !ok")
	}
}

// itoa renders a non-negative int as decimal (small helper to avoid pulling in
// strconv just for the perf-map path).
func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	var b [20]byte
	i := len(b)
	for n > 0 {
		i--
		b[i] = byte('0' + n%10)
		n /= 10
	}
	return string(b[i:])
}
