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
	"strings"
	"testing"
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
	if !tr2.Covered(0) || !tr2.Covered(0x7) || !tr2.Covered(0xf) {
		t.Fatalf("expected offsets 0, 0x7 and 0xf covered (covered(0)=%v, covered(0x7)=%v, covered(0xf)=%v)",
			tr2.Covered(0), tr2.Covered(0x7), tr2.Covered(0xf))
	}
	// Block partition {0, 0x7, 0xf} (entry / loop head / ret) — matches Unicorn/PT/DR
	// and the C reference (test_hwtrace's "single-step loop block partition
	// {0, 0x7, 0xf}" check).
	if tr2.BlocksLen() != 3 {
		t.Fatalf("BlocksLen: got %d, want 3", tr2.BlocksLen())
	}
	if tr2.Truncated() {
		t.Fatalf("Truncated: got true, want false")
	}

	tr2.Free()
	loop.Free()
}

// TestHwtraceScope exercises the closure-form scope construct: auto-name from the
// call site, render-on-close, and the thread-scope honesty bit.
func TestHwtraceScope(t *testing.T) {
	if !HwTraceAvailable(SingleStep) {
		t.Skipf("single-step backend unavailable: %s", HwTraceSkipReason(SingleStep))
	}
	if err := HwTraceInit(SingleStep); err != nil {
		t.Skipf("hwtrace init failed: %v", err)
	}
	defer HwTraceShutdown()

	code, err := HwNativeCodeFromBytes(hwtraceRoutine)
	if err != nil {
		t.Fatalf("HwNativeCodeFromBytes: %v", err)
	}
	defer code.Free()
	tr := NewHwTrace(64, 256)
	defer tr.Free()

	var r int64
	res := tr.Scope(code, false, func() { r = code.Call(20, 22) }) // auto-name at THIS line
	if r != 42 {
		t.Fatalf("Call(20,22): got %d, want 42", r)
	}
	if !res.Armed {
		t.Fatalf("scope did not arm on an available backend")
	}
	if res.Truncated {
		t.Fatalf("scope unexpectedly flagged truncated")
	}
	if res.Path == "" {
		t.Fatalf("render-on-close produced no text")
	}
	if lines := strings.Count(res.Path, "\n"); lines != 5 {
		t.Fatalf("rendered instruction lines: got %d, want 5", lines)
	}
	if !strings.HasPrefix(res.Name, "hwtrace_test.go:") {
		t.Fatalf("auto-name: got %q, want hwtrace_test.go:<line>", res.Name)
	}
}

// TestHwtraceCallScoped exercises the registry-free traced native call — arm + call +
// disarm entirely in native code (asmtest_hwtrace_call_scoped_ex). Mirrors
// test_hwtrace.py::test_call_scoped_traces_a_native_call.
func TestHwtraceCallScoped(t *testing.T) {
	if !HwTraceAvailable(SingleStep) {
		t.Skipf("single-step backend unavailable: %s", HwTraceSkipReason(SingleStep))
	}
	if err := HwTraceInit(SingleStep); err != nil {
		t.Skipf("hwtrace init failed: %v", err)
	}
	defer HwTraceShutdown()

	code, err := HwNativeCodeFromBytes(hwtraceRoutine)
	if err != nil {
		t.Fatalf("HwNativeCodeFromBytes: %v", err)
	}
	defer code.Free()

	res := CallScoped(code, 20, 22) // 42 <= 100 -> jle taken, dec skipped
	if !res.OK || res.RC != 0 {
		t.Fatalf("CallScoped rc: got %d (OK=%v), want 0", res.RC, res.OK)
	}
	if res.Result != 42 {
		t.Fatalf("CallScoped(20,22).Result: got %d, want 42", res.Result)
	}
	if res.Truncated {
		t.Fatalf("CallScoped unexpectedly flagged truncated")
	}
	if res.Path != "" { // decoder present
		if !strings.Contains(res.Path, "ret") {
			t.Fatalf("CallScoped listing missing the ret: %q", res.Path)
		}
		if lines := strings.Count(res.Path, "\n"); lines != 5 {
			t.Fatalf("CallScoped rendered instruction lines: got %d, want 5", lines)
		}
	}
	// Registry-free: 40 calls consume no MAX_REGIONS slot (each returns i+1).
	for i := int64(0); i < 40; i++ {
		if got := CallScoped(code, i, 1).Result; got != i+1 {
			t.Fatalf("CallScoped registry-free iteration %d: got %d, want %d", i, got, i+1)
		}
	}
}

// TestHwtraceWindow exercises the §Z1 region-free whole-window scope (the empty-ctor
// form). Window arms a REGION-FREE single-step capture on THIS thread (no HwNativeCode,
// no [base,len)), runs the closure, disarms, and renders. It is HONEST-BUT-NOISY — it
// steps the Go cgo glue too — so the traced leaf's addresses are a SUBSET of the listing
// and the generous 1M-insn ring may or may not overflow; both truncated outcomes are
// honest. Self-skips (Armed false) on a non-single-step backend; the closure still runs.
// Mirrors test_hwtrace.py::test_window_region_free_whole_window + node's window case.
func TestHwtraceWindow(t *testing.T) {
	if !HwTraceAvailable(SingleStep) {
		t.Skipf("single-step backend unavailable: %s", HwTraceSkipReason(SingleStep))
	}
	if err := HwTraceInit(SingleStep); err != nil {
		t.Skipf("hwtrace init failed: %v", err)
	}
	defer HwTraceShutdown()

	code, err := HwNativeCodeFromBytes(hwtraceRoutine)
	if err != nil {
		t.Fatalf("HwNativeCodeFromBytes: %v", err)
	}
	defer code.Free()

	var r int64
	res := Window(func() { r = code.Call(20, 22) }) // 42 <= 100 -> jle taken, dec skipped
	// The closure ALWAYS runs — armed or self-skipped, code.Call(20,22) == 42.
	if r != 42 {
		t.Fatalf("Window closure Call(20,22): got %d, want 42", r)
	}
	t.Logf("window: armed=%v truncated=%v pathLen=%d", res.Armed, res.Truncated, len(res.Path))
	// The single-step tier is up, so the region-free window arms here; a non-single-step
	// backend would self-skip (Armed false), still an honest outcome.
	if !res.Armed {
		t.Fatalf("window did not arm on an available single-step backend")
	}
	// When a decoder is present the (noisy) listing still contains the traced leaf's ret;
	// do not require non-empty text (no Capstone => an empty path is honest).
	if res.Path != "" && !strings.Contains(res.Path, "ret") {
		t.Fatalf("window listing present but missing a ret (pathLen=%d)", len(res.Path))
	}
}

// TestHwtraceScopeFanoutUntraced documents the Go-specific gap: work fanned out via
// `go func()` inside a scope runs on ANOTHER OS thread and is silently untraced —
// LockOSThread confines the trace to the arming thread, so the trace holds only the
// arming-thread instructions, not the fanned-out call's.
func TestHwtraceScopeFanoutUntraced(t *testing.T) {
	if !HwTraceAvailable(SingleStep) {
		t.Skipf("single-step backend unavailable: %s", HwTraceSkipReason(SingleStep))
	}
	if err := HwTraceInit(SingleStep); err != nil {
		t.Skipf("hwtrace init failed: %v", err)
	}
	defer HwTraceShutdown()

	code, err := HwNativeCodeFromBytes(hwtraceRoutine)
	if err != nil {
		t.Fatalf("HwNativeCodeFromBytes: %v", err)
	}
	defer code.Free()
	tr := NewHwTrace(64, 256)
	defer tr.Free()

	done := make(chan int64, 1)
	var mainRet int64
	tr.Scope(code, false, func() {
		go func() { done <- code.Call(200, 50) }() // fan-out: another OS thread, untraced
		mainRet = code.Call(20, 22)                // arming-thread work: 5 insns, traced
		<-done                                     // join the fan-out
	})
	if mainRet != 42 {
		t.Fatalf("arming-thread call: got %d, want 42", mainRet)
	}
	// Only the arming-thread call (add2(20,22) = 5 insns) is traced; the fan-out
	// (add2(200,50) = 6 insns) ran elsewhere and is silently untraced.
	if got := tr.InsnsTotal(); got != 5 {
		t.Fatalf("fan-out leaked into the trace: InsnsTotal=%d, want 5 (arming-thread only)", got)
	}
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
	if !tr.Covered(0) && !tr.Truncated() {
		t.Fatalf("expected offset 0 covered (or honestly truncated)")
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

// TestHwtraceTraceCallAuto exercises the auto-escalating CALL-OWNING cross-tier
// trace (asmtest_trace_call_auto): it OWNS the invocation and self-manages the tier
// lifecycle, so it runs standalone with NO HwTraceInit fixture — off x86-64 Linux it
// self-skips with EUNAVAIL. Mirrors test_hwtrace.py::
// test_trace_call_auto_owns_the_call_and_completes and the C reference test_call_auto.
func TestHwtraceTraceCallAuto(t *testing.T) {
	// The exec allocator only needs the lib loaded (not an armed tier); a failure here
	// means libasmtest_hwtrace is absent, so self-skip exactly as the other tests do.
	code, err := HwNativeCodeFromBytes(hwtraceRoutine)
	if err != nil {
		t.Skipf("hardware-trace tier unavailable: %v", err)
	}
	defer code.Free()

	// 42 <= 100 -> jle taken, dec skipped.
	res := TraceCallAuto(code, 20, 22)
	if res.RC != 0 && res.RC != HwEUnavail {
		t.Fatalf("TraceCallAuto rc: got %d, want 0 or EUNAVAIL (%d)", res.RC, HwEUnavail)
	}
	if res.RC == 0 {
		if res.Result != 42 {
			t.Fatalf("TraceCallAuto(20,22).Result: got %d, want 42", res.Result)
		}
		if res.Truncated {
			t.Fatalf("TraceCallAuto unexpectedly flagged truncated") // some tier captured the whole path
		}
		if !res.Trace.Covered(0) { // entry block covered
			t.Fatalf("TraceCallAuto: expected entry block (offset 0) covered")
		}
		if res.Used.Tier != TierHwtrace {
			t.Fatalf("TraceCallAuto used tier %d, want TierHwtrace (%d)", res.Used.Tier, TierHwtrace)
		}
		res.Trace.Free()
	}

	// A loop past the 16-taken-branch LBR window must STILL yield a complete trace
	// (escalating off the ceiling-bounded backend on an AMD host; the single-step floor
	// completes it directly elsewhere).
	loop, err := HwNativeCodeFromBytes(hwtraceLoop)
	if err != nil {
		t.Fatalf("HwNativeCodeFromBytes(loop): %v", err)
	}
	defer loop.Free()

	lres := TraceCallAuto(loop, 1, 25) // 25 back-edges > 16-deep window
	if lres.RC != 0 && lres.RC != HwEUnavail {
		t.Fatalf("TraceCallAuto(loop) rc: got %d, want 0 or EUNAVAIL (%d)", lres.RC, HwEUnavail)
	}
	if lres.RC == 0 {
		if lres.Result != 25 {
			t.Fatalf("TraceCallAuto(loop,1,25).Result: got %d, want 25", lres.Result)
		}
		if lres.Truncated {
			t.Fatalf("TraceCallAuto(loop) truncated: escalation should have completed the trace")
		}
		if !lres.Trace.Covered(0x7) { // loop-body block covered
			t.Fatalf("TraceCallAuto(loop): expected loop-body block (offset 0x7) covered")
		}
		lres.Trace.Free()
	}
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

	result, err := PtraceTraceCall(code.Ptr(), code.Len(), []int64{20, 22}, tr)
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

// TestPtraceTraceCallBlockstep exercises the BTF block-step tier: one #DB per TAKEN
// branch, intra-block instructions reconstructed with Capstone — the stream is
// byte-identical to the per-instruction TestPtraceTraceCall above. Self-skips where
// PTRACE_SINGLEBLOCK / Capstone are absent (e.g. AArch64).
func TestPtraceTraceCallBlockstep(t *testing.T) {
	skipIfNoPtrace(t)
	if !PtraceBlockstepAvailable() {
		t.Skip("BTF block-step unavailable (needs x86-64 PTRACE_SINGLEBLOCK + Capstone)")
	}
	code, err := HwNativeCodeFromBytes(hwtraceRoutine)
	if err != nil {
		t.Fatalf("HwNativeCodeFromBytes: %v", err)
	}
	defer code.Free()
	tr := NewHwTrace(64, 64) // blocks=64, instructions=64
	defer tr.Free()

	result, err := PtraceTraceCallBlockstep(code.Ptr(), code.Len(), []int64{20, 22}, tr)
	if err != nil {
		t.Fatalf("PtraceTraceCallBlockstep: %v", err)
	}
	if result != 42 {
		t.Fatalf("PtraceTraceCallBlockstep(20,22): got %d, want 42", result)
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

// ---- Call descent (asmtest_descent_t) ----
//
// Mirrors the Python suite's test_descent_edges_and_frames plus a resolver-upcall
// test. Named with the TestHwtrace prefix so the `make hwtrace-go-test` lane (which
// runs `go test -run TestHwtrace`) exercises them. They self-skip when the ptrace
// single-stepper is unavailable (off x86-64 Linux, or when libasmtest_hwtrace is
// absent).

// descentBlob is the call-descent fixture (x86-64): R@0 = mov rax,rdi; call S(+4);
// add rax,rsi; ret — with the in-blob leaf S@0xc = inc rax; ret. The traced region is
// R only (0xc bytes); S lives beyond it in the SAME allocation, so trace_call_ex must
// be told region=0xc or S mis-records as recursion. Args (20,22) -> 43.
var descentBlob = []byte{
	0x48, 0x89, 0xF8, 0xE8, 0x04, 0x00, 0x00, 0x00,
	0x48, 0x01, 0xF0, 0xC3, 0x48, 0xFF, 0xC0, 0xC3,
}

func descentEqualU64(a, b []uint64) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// TestHwtraceDescent mirrors the Python suite's test_descent_edges_and_frames: at
// level 1 R's call to the in-blob leaf S is recorded as one edge (S stepped over); at
// level 2, with S in the allow-set, S descends as nested frame 1.
func TestHwtraceDescent(t *testing.T) {
	skipIfNoPtrace(t)
	code, err := HwNativeCodeFromBytes(descentBlob)
	if err != nil {
		t.Fatalf("HwNativeCodeFromBytes: %v", err)
	}
	defer code.Free()
	sBase := uint64(code.Base()) + 0xc

	// Level 1: R's own body + one (call -> S) edge; S is stepped over.
	d1 := NewDescent(DescentRecordEdges)
	if d1 == nil {
		t.Fatal("NewDescent(DescentRecordEdges): got nil")
	}
	defer d1.Free()
	r, err := PtraceTraceCallEx(code.Ptr(), code.Len(), []int64{20, 22}, nil, d1, 0xc)
	if err != nil {
		t.Fatalf("PtraceTraceCallEx L1: %v", err)
	}
	if r != 43 {
		t.Fatalf("L1 result: got %d, want 43", r)
	}
	if d1.FramesLen() != 1 {
		t.Fatalf("L1 FramesLen: got %d, want 1", d1.FramesLen())
	}
	if got := d1.FrameInsns(0); !descentEqualU64(got, []uint64{0x0, 0x3, 0x8, 0xb}) {
		t.Fatalf("L1 FrameInsns(0): got %v, want [0 3 8 11]", got)
	}
	edges := d1.Edges()
	if len(edges) != 1 {
		t.Fatalf("L1 Edges: got %d, want 1", len(edges))
	}
	if edges[0].Site != 0x3 || edges[0].Target != sBase {
		t.Fatalf("L1 edge: got site=%#x target=%#x, want site=0x3 target=%#x",
			edges[0].Site, edges[0].Target, sBase)
	}
	if d1.Truncated() {
		t.Fatal("L1 Truncated: got true, want false")
	}

	// Level 2: S (in the allow-set) descends as frame 1.
	d2 := NewDescent(DescentDescendKnown)
	if d2 == nil {
		t.Fatal("NewDescent(DescentDescendKnown): got nil")
	}
	defer d2.Free()
	if rc := d2.AllowRegion(code.Base()+0xc, 4); rc != 0 {
		t.Fatalf("AllowRegion: got %d, want 0", rc)
	}
	r2, err := PtraceTraceCallEx(code.Ptr(), code.Len(), []int64{20, 22}, nil, d2, 0xc)
	if err != nil {
		t.Fatalf("PtraceTraceCallEx L2: %v", err)
	}
	if r2 != 43 {
		t.Fatalf("L2 result: got %d, want 43", r2)
	}
	if d2.FramesLen() != 2 {
		t.Fatalf("L2 FramesLen: got %d, want 2", d2.FramesLen())
	}
	if d2.FrameBase(1) != sBase {
		t.Fatalf("L2 FrameBase(1): got %#x, want %#x", d2.FrameBase(1), sBase)
	}
	if d2.FrameDepth(1) != 1 {
		t.Fatalf("L2 FrameDepth(1): got %d, want 1", d2.FrameDepth(1))
	}
	if got := d2.FrameInsns(1); !descentEqualU64(got, []uint64{0x0, 0x3}) {
		t.Fatalf("L2 FrameInsns(1): got %v, want [0 3]", got)
	}
	if len(d2.Edges()) != 0 {
		t.Fatalf("L2 Edges: got %d, want 0", len(d2.Edges()))
	}
}

// TestHwtraceDescentResolver proves the resolver UPCALL fires. Instead of a static
// allow-region, a Go closure (installed via SetResolver, kept alive GC-safe through
// the package registry + //export trampoline) decides to descend into the in-blob leaf
// S. The closure must be invoked during the out-of-process single-step (calls > 0 and
// the callee address it saw is S), and S must then appear as descended frame 1 — proof
// the C engine round-tripped through Go and back.
func TestHwtraceDescentResolver(t *testing.T) {
	skipIfNoPtrace(t)
	code, err := HwNativeCodeFromBytes(descentBlob)
	if err != nil {
		t.Fatalf("HwNativeCodeFromBytes: %v", err)
	}
	defer code.Free()

	target := uint64(code.Base()) + 0xc
	var calls int
	var sawCallee uint64
	d := NewDescent(DescentDescendKnown)
	if d == nil {
		t.Fatal("NewDescent(DescentDescendKnown): got nil")
	}
	defer d.Free()
	d.SetResolver(func(callee uint64) (bool, uint64, uint64) {
		calls++
		sawCallee = callee
		if callee == target {
			return true, target, 4
		}
		return false, 0, 0
	})

	r, err := PtraceTraceCallEx(code.Ptr(), code.Len(), []int64{20, 22}, nil, d, 0xc)
	if err != nil {
		t.Fatalf("PtraceTraceCallEx: %v", err)
	}
	if r != 43 {
		t.Fatalf("result: got %d, want 43", r)
	}
	if calls == 0 {
		t.Fatal("resolver upcall never fired")
	}
	if sawCallee != target {
		t.Fatalf("resolver callee: got %#x, want %#x", sawCallee, target)
	}
	if d.FramesLen() != 2 {
		t.Fatalf("FramesLen: got %d, want 2 (resolver-driven descent)", d.FramesLen())
	}
	if d.FrameBase(1) != target {
		t.Fatalf("FrameBase(1): got %#x, want %#x", d.FrameBase(1), target)
	}
}

// ---- Time-aware code-image recorder (asmtest_codeimage.h) ----
//
// Mirrors the code-image tests in the Python suite. They self-skip when the userspace
// recorder cannot detect page changes on this host (no PAGEMAP_SCAN / soft-dirty), or
// when libasmtest_hwtrace is absent.

// codeimageRoutine is mov rax,rdi; add rax,rsi; ret — the smallest two-arg adder, used
// to assert a byte-for-byte Track -> BytesAt round-trip on the recording process.
var codeimageRoutine = []byte{0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0xC3}

// TestHwtraceCodeImage tracks a freshly-materialized native region in THIS process
// (pid 0) and round-trips its bytes back through the timeline.
func TestHwtraceCodeImage(t *testing.T) {
	if !CodeImageAvailable() {
		t.Skipf("code-image recorder unavailable: %s", CodeImageSkipReason())
	}

	code, err := HwNativeCodeFromBytes(codeimageRoutine)
	if err != nil {
		t.Fatalf("HwNativeCodeFromBytes: %v", err)
	}
	defer code.Free()

	img := NewCodeImage(0) // 0 => this process
	if img == nil {
		t.Fatalf("NewCodeImage(0): got nil")
	}
	defer img.Close()

	if err := img.Track(code.Base(), len(codeimageRoutine)); err != nil {
		t.Fatalf("Track: %v", err)
	}

	// Tracking snapshots version 0, so the logical clock has advanced past 0.
	if now := img.Now(); now < 1 {
		t.Fatalf("Now after Track: got %d, want >= 1", now)
	}

	// A refresh with nothing changed is cheap and never negative.
	if n, err := img.Refresh(); err != nil || n < 0 {
		t.Fatalf("Refresh: got (%d, %v), want (>= 0, nil)", n, err)
	}

	// BytesAt(base, 0) returns the bytes live at base as of the latest version — the
	// ones we just tracked, byte-for-byte.
	got := img.BytesAt(code.Base(), 0)
	if got == nil {
		t.Fatalf("BytesAt(base, 0): got nil, want the tracked bytes")
	}
	if len(got) < len(codeimageRoutine) || !bytes.Equal(got[:len(codeimageRoutine)], codeimageRoutine) {
		t.Fatalf("BytesAt round-trip: got %x, want a prefix of %x", got, codeimageRoutine)
	}

	// An address that was never tracked has no version (nil round-trip).
	if b := img.BytesAt(0x1, 0); b != nil {
		t.Fatalf("BytesAt(untracked addr): got %x, want nil", b)
	}
}

// TestHwtraceCodeImageBpf probes the optional eBPF emission detector's FFI round-trip:
// when it is available, WatchBpf must load and attach cleanly (ASMTEST_CI_OK).
func TestHwtraceCodeImageBpf(t *testing.T) {
	if !CodeImageBpfAvailable() {
		t.Skipf("code-image eBPF detector unavailable: %s", CodeImageBpfSkipReason())
	}

	img := NewCodeImage(0)
	if img == nil {
		t.Fatalf("NewCodeImage(0): got nil")
	}
	defer img.Close()

	if rc := img.WatchBpf(); rc != 0 {
		t.Fatalf("WatchBpf: got %d, want 0 (ASMTEST_CI_OK)", rc)
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

// TestHwtraceFlagday covers the 2026-07 API flag day: the F27 struct-size
// negotiated init (a drifted mirror would EINVAL), the F29 status surface
// (EPERM vs EUNAVAIL, locked to available()/skip_reason()), and the F22/F26/F37
// mechanism discriminator.
func TestHwtraceFlagday(t *testing.T) {
	if !HwTraceAvailable(SingleStep) {
		t.Skipf("single-step backend unavailable: %s", HwTraceSkipReason(SingleStep))
	}

	// F27: the wrapper self-describes via struct_size; init must succeed.
	if err := HwTraceInit(SingleStep); err != nil {
		t.Fatalf("size-negotiated init failed: %v", err)
	}
	HwTraceShutdown()

	// F29: status invariants across all four backends.
	paranoid := HwTracePerfEventParanoid()
	for _, b := range []int{IntelPT, CoreSight, AmdLBR, SingleStep} {
		st, err := HwTraceStatus(b)
		if err != nil {
			t.Fatalf("HwTraceStatus(%d): %v", b, err)
		}
		if st.Available != HwTraceAvailable(b) {
			t.Fatalf("status(%d).Available=%v != available()=%v", b, st.Available, HwTraceAvailable(b))
		}
		if (st.Code == 0) != st.Available {
			t.Fatalf("status(%d): Code=%d vs Available=%v", b, st.Code, st.Available)
		}
		if st.Code != 0 && st.Code != HwEUnavail && st.Code != HwEperm {
			t.Fatalf("status(%d).Code=%d not in {OK, EUNAVAIL, EPERM}", b, st.Code)
		}
		if st.Reason != HwTraceSkipReason(b) {
			t.Fatalf("status(%d).Reason %q != skip_reason %q", b, st.Reason, HwTraceSkipReason(b))
		}
		if st.PerfEventParanoid != paranoid {
			t.Fatalf("status(%d).PerfEventParanoid=%d != reader %d", b, st.PerfEventParanoid, paranoid)
		}
	}
	// LIVE permission-vs-hardware lane (self-skips where not applicable).
	amd, _ := HwTraceStatus(AmdLBR)
	if amd.Stage == HwStageProbe && paranoid > 2 && os.Geteuid() != 0 {
		if amd.Code != HwEperm {
			t.Fatalf("paranoid-blocked AMD probe: Code=%d, want EPERM (%d)", amd.Code, HwEperm)
		}
	} else {
		t.Logf("# SKIP flagday live-EPERM lane (stage=%d paranoid=%d)", amd.Stage, paranoid)
	}

	// F22/F26/F37: resolved rows + TraceCallAuto's Used carry the mechanism; no
	// exact producer is ever statistical.
	for _, c := range ResolveTiers(TraceBest) {
		if c.Mechanism == MechNone || c.Mechanism == MechStatistical || c.Fidelity == FidelityStatistical {
			t.Fatalf("exact cascade row has dishonest mechanism/fidelity: %+v", c)
		}
		if c.Tier == TierHwtrace && c.Backend == SingleStep && c.Mechanism != MechTfStep {
			t.Fatalf("single-step row mechanism: got %d, want MechTfStep", c.Mechanism)
		}
	}
	code, err := HwNativeCodeFromBytes(hwtraceRoutine)
	if err != nil {
		t.Skipf("hardware-trace tier unavailable: %v", err)
	}
	defer code.Free()
	res := TraceCallAuto(code, 20, 22)
	if res.RC == 0 {
		switch res.Used.Mechanism {
		case MechHwBranch, MechTfStep, MechMsrLbr, MechBlockStep, MechPerInsn:
		default:
			t.Fatalf("TraceCallAuto winning mechanism: got %d, want a concrete exact rung", res.Used.Mechanism)
		}
		if res.Used.Fidelity != FidelityNative {
			t.Fatalf("TraceCallAuto fidelity: got %d, want FidelityNative", res.Used.Fidelity)
		}
		res.Trace.Free()
	}
}
