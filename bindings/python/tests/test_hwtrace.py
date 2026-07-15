"""Live test for the single-step hardware-trace backend via the Python wrapper.

Unlike the DynamoRIO wrapper (which needs a DynamoRIO install) and the PT/AMD
backends (which need specific bare-metal hardware), the SINGLESTEP backend runs on
ANY x86-64 Linux — so this asserts a real, live trace here and in CI/containers,
self-skipping only off x86-64 Linux or without Capstone.
"""
import os
import struct

import pytest

from asmtest.hwtrace import (
    HwTrace, NativeCode, Ptrace, CodeImage, SINGLESTEP, AMD_LBR,
    INTEL_PT, CORESIGHT,
    BEST, CEILING_FREE, ASMTEST_HW_EUNAVAIL, ASMTEST_HW_OK, ASMTEST_HW_EPERM,
    TIER_HWTRACE, TIER_EMULATOR, FIDELITY_NATIVE, FIDELITY_VIRTUAL,
    FIDELITY_STATISTICAL,
    MECH_NONE, MECH_HW_BRANCH, MECH_TF_STEP, MECH_MSR_LBR, MECH_BLOCKSTEP,
    MECH_PER_INSN, MECH_STATISTICAL,
    STAGE_OK, STAGE_PROBE,
    TRACE_BEST, TRACE_CEILING_FREE, TRACE_NATIVE_ONLY,
    ASMTEST_CI_KIND_MPROTECT,
)

# mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret   (two blocks)
ROUTINE = bytes(
    [0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00,
     0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3]
)


@pytest.fixture
def hwtrace():
    if not HwTrace.available(SINGLESTEP):
        pytest.skip(f"single-step backend unavailable: {HwTrace.skip_reason(SINGLESTEP)}")
    HwTrace.init(SINGLESTEP)
    yield
    HwTrace.shutdown()


def test_singlestep_live_trace(hwtrace):
    code = NativeCode.from_bytes(ROUTINE)
    trace = HwTrace.new(blocks=64, instructions=64)
    trace.register("add2", code)

    with trace.region("add2"):
        result = code.call(20, 22)  # 42 <= 100 -> jle taken, dec skipped

    assert result == 42
    # Byte-for-byte the Unicorn/DynamoRIO/PT/AMD result for this fixture.
    assert trace.insn_offsets() == [0x0, 0x3, 0x6, 0xC, 0x11]
    assert trace.insns_total() == 5
    assert trace.covered(0) and trace.covered(0x11)
    assert trace.blocks_len() == 2
    assert not trace.truncated()

    trace.free()
    code.free()


def test_call_scoped_traces_a_native_call(hwtrace):
    # call_scoped: arm + call + disarm entirely in native code — registry-free, returns
    # the call result and the executed body disassembly in one step.
    code = NativeCode.from_bytes(ROUTINE)
    res = HwTrace.call_scoped(code, 20, 22)  # 42 <= 100 -> jle taken, dec skipped
    assert res.result == 42
    assert res.rc == 0
    assert not res.truncated
    if res.path:  # non-empty when Capstone is present
        assert "ret" in res.path.lower()
        assert res.path.count("\n") == 5  # 5 rendered instruction lines (taken path)
    # Registry-free: many calls must NOT exhaust the fixed 32-slot region table.
    for i in range(40):
        assert HwTrace.call_scoped(code, i, 1).result == i + 1
    code.free()


# double add(double a, double b) { return a + b; }  ->  addsd xmm0, xmm1; ret
FADD2 = bytes([0xF2, 0x0F, 0x58, 0xC1, 0xC3])


def test_call_scoped_fp_traces_a_double_call(hwtrace):
    # call_scoped_fp: the FP (double…)->double sibling of call_scoped — arm + call + disarm
    # in native code, args in xmm0..xmm7, return in xmm0. Traces a double->double leaf the
    # integer call_scoped ABI cannot express.
    code = NativeCode.from_bytes(FADD2)
    res = HwTrace.call_scoped_fp(code, 20.0, 22.0)  # addsd xmm0,xmm1 -> 42.0
    assert res.result == 42.0
    assert isinstance(res.result, float)
    assert res.rc == 0
    assert not res.truncated
    if res.path:  # non-empty when Capstone is present
        assert "addsd" in res.path.lower()
        assert res.path.count("\n") == 2  # addsd + ret
    # The named form registers under a call-site-constant name, so a loop at one site
    # reuses one MAX_REGIONS slot — no fixed-table exhaustion.
    for i in range(40):
        assert HwTrace.call_scoped_fp(code, float(i), 1.0).result == float(i) + 1.0
    code.free()


def test_window_region_free_whole_window(hwtrace):
    # window: §Z1 region-free whole-window scope — the callback form of dotnet's empty-ctor
    # `using (new AsmTrace())`. Arm a REGION-FREE single-step capture on THIS thread (no
    # NativeCode, no [base,len)), run fn, disarm, render. HONEST-BUT-NOISY: it records
    # EVERYTHING between begin and end, so the leaf's ABSOLUTE addresses are a SUBSET of the
    # listing. Self-skips (armed False) on a non-single-step backend; fn still runs.
    code = NativeCode.from_bytes(ROUTINE)
    box = {}
    w = HwTrace.window(lambda: box.__setitem__("r", code.call(20, 22)))
    assert box["r"] == 42  # fn ALWAYS runs, armed or self-skipped
    assert w.armed  # the `hwtrace` fixture inited single-step, so the window arms here
    assert not w.truncated  # the generous 1M-insn cap is not overflowed by the tiny leaf
    if w.path:  # non-empty when Capstone is present
        assert "ret" in w.path.lower()
    code.free()


def test_trace_call_auto_owns_the_call_and_completes():
    # trace_call_auto OWNS the invocation: run under the fastest exact tier and
    # auto-escalate to a ceiling-free tier if the trace truncates. It self-manages the
    # tier lifecycle (no `hwtrace` init fixture), so it runs standalone; off x86-64
    # Linux it self-skips with EUNAVAIL. Mirrors the C reference test_call_auto.
    code = NativeCode.from_bytes(ROUTINE)
    res = HwTrace.trace_call_auto(code, 20, 22)  # 42 <= 100 -> jle taken, dec skipped
    assert res.rc in (0, ASMTEST_HW_EUNAVAIL)
    if res.rc == 0:
        assert res.result == 42
        assert not res.truncated  # some tier captured the whole path
        assert res.trace.covered(0)  # entry block covered
        assert res.used is not None and res.used.tier == TIER_HWTRACE
        res.trace.free()
    code.free()

    # A loop past the 16-taken-branch LBR window must STILL yield a complete trace
    # (escalating off the ceiling-bounded backend on an AMD host; the single-step floor
    # completes it directly elsewhere). mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret.
    loop = bytes([0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00,
                  0x48, 0x01, 0xF8, 0x48, 0xFF, 0xCE, 0x75, 0xF8, 0xC3])
    lcode = NativeCode.from_bytes(loop)
    lres = HwTrace.trace_call_auto(lcode, 1, 25)  # 25 back-edges > 16-deep window
    assert lres.rc in (0, ASMTEST_HW_EUNAVAIL)
    if lres.rc == 0:
        assert lres.result == 25
        assert not lres.truncated  # escalated to a ceiling-free tier
        assert lres.trace.covered(0x7)  # loop-body block covered
        lres.trace.free()
    lcode.free()


def test_scoped_trace_matches_callback(hwtrace):
    # The scope object is a faithful wrapper of the callback region() form: it
    # produces the same offsets, renders the assembly on close, and auto-names from
    # the call site.
    code = NativeCode.from_bytes(ROUTINE)

    # Callback form: 5 in-region instructions for add2(20,22).
    cb = HwTrace.new(blocks=64, instructions=64)
    cb.register("cb_add2", code)
    with cb.region("cb_add2"):
        r_cb = code.call(20, 22)
    assert r_cb == 42
    assert cb.insns_total() == 5
    cb.free()

    # Scope form over the same routine: rendered non-empty, one line per insn.
    with HwTrace.scope(code, name="sc_add2", emit=False) as t:
        r_sc = code.call(20, 22)
    assert r_sc == 42
    assert t.armed
    assert not t.truncated
    assert t.path and t.path.count("\n") == 5  # 5 rendered instruction lines
    assert "ret" in t.path.lower()

    code.free()


def test_scoped_trace_auto_name(hwtrace):
    # The generated region name reflects the enclosing file + line (CPython frame
    # walk); degrades to a synthetic label off CPython.
    code = NativeCode.from_bytes(ROUTINE)
    with HwTrace.scope(code, emit=False) as t:
        code.call(20, 22)
    assert t.name.startswith("test_hwtrace.py:") or t.name.startswith("asmscope#")
    code.free()


def test_singlestep_loop_no_depth_ceiling(hwtrace):
    # mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret  (19 back-edges > LBR's 16)
    loop = bytes([0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00,
                  0x48, 0x01, 0xF8, 0x48, 0xFF, 0xCE, 0x75, 0xF8, 0xC3])
    code = NativeCode.from_bytes(loop)
    trace = HwTrace.new(blocks=64, instructions=256)
    trace.register("loop", code)

    with trace.region("loop"):
        result = code.call(1, 20)

    assert result == 20
    assert trace.insns_total() == 62  # 1 + 20*3 + 1, all captured
    assert trace.covered(0) and trace.covered(0x7) and trace.covered(0xf)
    # 3 blocks {0, 0x7, 0xf}: ends-at-branch normalization makes the ret after the
    # not-taken jnz its own block (matches the C reference test_singlestep_loop).
    assert trace.blocks_len() == 3
    assert not trace.truncated()

    trace.free()
    code.free()


def test_auto_resolve_selection_invariants():
    """The orchestrator's selection invariants hold on every host (even where all
    backends self-skip and the cascade is empty)."""
    best = HwTrace.resolve(BEST)
    cf = HwTrace.resolve(CEILING_FREE)

    # Every resolved backend is actually available, ordered by descending fidelity
    # (ascending enum), with no duplicates.
    assert all(HwTrace.available(b) for b in best)
    assert best == sorted(set(best))

    # CEILING_FREE drops the one fixed-window backend (AMD LBR) and is otherwise a
    # subset of BEST.
    assert AMD_LBR not in cf
    assert set(cf).issubset(set(best))

    # auto(policy) is the head of resolve(policy), or EUNAVAIL when empty.
    ab = HwTrace.auto(BEST)
    assert ab == (best[0] if best else ASMTEST_HW_EUNAVAIL)


def test_cross_tier_resolve_invariants():
    """The cross-tier orchestrator (resolve over hwtrace + DynamoRIO + emulator)
    holds its structural invariants on every host."""
    best = HwTrace.resolve_tiers(TRACE_BEST)
    nat = HwTrace.resolve_tiers(TRACE_NATIVE_ONLY)
    cf = HwTrace.resolve_tiers(TRACE_CEILING_FREE)

    # Every HW choice satisfies the hardware-tier probe; NATIVE choices precede the
    # single VIRTUAL emulator floor, which is the last entry under BEST.
    for c in best:
        if c.tier == TIER_HWTRACE:
            assert HwTrace.available(c.backend)
        assert c.fidelity == (FIDELITY_VIRTUAL if c.tier == TIER_EMULATOR
                              else FIDELITY_NATIVE)
    assert best and best[-1].tier == TIER_EMULATOR
    assert sum(1 for c in best if c.tier == TIER_EMULATOR) == 1

    # NATIVE_ONLY forbids the native->emulator crossing: it is BEST minus the floor.
    assert all(c.tier != TIER_EMULATOR for c in nat)
    assert len(nat) == len(best) - 1

    # CEILING_FREE drops AMD LBR.
    assert all(not (c.tier == TIER_HWTRACE and c.backend == AMD_LBR) for c in cf)

    # auto(policy) is the head of resolve(policy).
    one = HwTrace.auto_tier(TRACE_BEST)
    assert one is not None
    assert (one.tier, one.backend) == (best[0].tier, best[0].backend)


def test_cross_tier_native_only_resolves_on_linux_x86_64():
    """On any x86-64 Linux host the single-step backend is a native floor, so even
    NATIVE_ONLY resolves (the cascade never collapses to nothing here)."""
    if not HwTrace.available(SINGLESTEP):
        pytest.skip(f"single-step backend unavailable: {HwTrace.skip_reason(SINGLESTEP)}")
    nat = HwTrace.resolve_tiers(TRACE_NATIVE_ONLY)
    pick = HwTrace.auto_tier(TRACE_NATIVE_ONLY)
    assert nat and pick is not None and pick.fidelity == FIDELITY_NATIVE
    assert any(c.tier == TIER_HWTRACE and c.backend == SINGLESTEP for c in nat)


def test_options_struct_size_guard():
    """F27 flag day: the mirrored options struct leads with ``struct_size`` and
    the library honors it — a size too small to reach ``backend`` is EINVAL,
    a future (larger) declared size still inits, and the normal init() path
    (which self-describes) works."""
    import ctypes as C

    from asmtest import hwtrace as hw

    if not HwTrace.available(SINGLESTEP):
        pytest.skip(f"single-step backend unavailable: {HwTrace.skip_reason(SINGLESTEP)}")
    lib = hw._get()

    # A struct_size that cannot even carry `backend` is rejected — proving the
    # leading negotiator is at offset 0 and actually honored.
    tiny = hw._Options()
    tiny.struct_size = C.sizeof(C.c_size_t)
    tiny.backend = SINGLESTEP
    assert lib.asmtest_hwtrace_init(C.byref(tiny)) == -1  # ASMTEST_HW_EINVAL

    # A pretend-future caller declaring a LARGER size inits fine (the library
    # clamps its copy to its own sizeof and ignores the unknown tail).
    fut = hw._Options()
    fut.struct_size = C.sizeof(hw._Options) + 64
    fut.backend = SINGLESTEP
    assert lib.asmtest_hwtrace_init(C.byref(fut)) == ASMTEST_HW_OK
    HwTrace.shutdown()

    # And the ordinary wrapper path self-describes and still traces.
    HwTrace.init(SINGLESTEP)
    HwTrace.shutdown()


def test_status_surface_invariants():
    """F29: status() is available()/skip_reason() made machine-readable — one
    classifier, so they can never drift — and distinguishes EPERM (substrate
    present, permission denied) from EUNAVAIL (hardware absent)."""
    paranoid = HwTrace.perf_event_paranoid()
    for b in (INTEL_PT, CORESIGHT, AMD_LBR, SINGLESTEP):
        st = HwTrace.status(b)
        assert st.available == HwTrace.available(b)
        assert (st.code == ASMTEST_HW_OK) == st.available
        assert st.code in (ASMTEST_HW_OK, ASMTEST_HW_EUNAVAIL, ASMTEST_HW_EPERM)
        assert st.reason == HwTrace.skip_reason(b)
        assert st.perf_event_paranoid == paranoid
    if HwTrace.available(SINGLESTEP):
        st = HwTrace.status(SINGLESTEP)
        assert st.code == ASMTEST_HW_OK and st.stage == STAGE_OK

    # The LIVE permission-vs-hardware distinction (self-skipping lane): an AMD
    # probe that REACHED the perf open while paranoid blocks unprivileged perf
    # must classify as EPERM, never as missing silicon.
    st = HwTrace.status(AMD_LBR)
    if st.stage == STAGE_PROBE and paranoid > 2 and os.geteuid() != 0:
        assert st.code == ASMTEST_HW_EPERM
        assert not st.available and not HwTrace.available(AMD_LBR)


def test_mechanism_discriminator():
    """F22/F26/F37: resolved rows and call_auto's ``used`` carry the concrete
    capture mechanism; no exact producer ever reports STATISTICAL."""
    for c in HwTrace.resolve_tiers(TRACE_BEST):
        assert c.mechanism != MECH_NONE
        assert c.mechanism != MECH_STATISTICAL
        assert c.fidelity != FIDELITY_STATISTICAL
        if c.tier == TIER_HWTRACE and c.backend == SINGLESTEP:
            assert c.mechanism == MECH_TF_STEP

    if not HwTrace.available(SINGLESTEP):
        pytest.skip(f"single-step backend unavailable: {HwTrace.skip_reason(SINGLESTEP)}")
    code = NativeCode.from_bytes(ROUTINE)
    r = HwTrace.trace_call_auto(code, 20, 22)
    assert r.rc == ASMTEST_HW_OK and r.result == 42
    assert r.used.mechanism in (MECH_HW_BRANCH, MECH_TF_STEP, MECH_MSR_LBR,
                                MECH_BLOCKSTEP, MECH_PER_INSN)
    assert r.used.mechanism != MECH_STATISTICAL
    assert r.used.fidelity == FIDELITY_NATIVE
    r.trace.free()
    code.free()


def test_auto_resolve_traces_live():
    """On any x86-64 Linux host the cascade is non-empty (single-step floor), so
    auto() resolves a usable backend; trace the shared fixture through it."""
    if not HwTrace.available(SINGLESTEP):
        pytest.skip(f"single-step backend unavailable: {HwTrace.skip_reason(SINGLESTEP)}")

    best = HwTrace.resolve(BEST)
    ab = HwTrace.auto(BEST)
    assert best and ab >= 0  # single-step keeps the cascade non-empty here

    HwTrace.init(ab)
    try:
        code = NativeCode.from_bytes(ROUTINE)
        trace = HwTrace.new(blocks=64, instructions=64)
        trace.register("auto", code)
        with trace.region("auto"):
            result = code.call(20, 22)
        assert result == 42
        # AMD LBR honestly truncates this tiny single-shot fixture on a Zen 3+ host;
        # single-step covers block 0. The honest invariant is "covered OR truncated".
        assert trace.covered(0) or trace.truncated()
        if ab == SINGLESTEP:  # the pick off PT/AMD hosts: byte-exact parity
            assert trace.insn_offsets() == [0x0, 0x3, 0x6, 0xC, 0x11]
        trace.free()
        code.free()
    finally:
        HwTrace.shutdown()


# ---- Out-of-process / foreign-process toolkit (asmtest.hwtrace.Ptrace) ----

def _skip_if_no_ptrace():
    if not Ptrace.available():
        pytest.skip(f"ptrace backend unavailable: {Ptrace.skip_reason()}")


def test_ptrace_trace_call():
    """Fork a tracee, single-step it out of process, get the same offsets."""
    _skip_if_no_ptrace()
    code = NativeCode.from_bytes(ROUTINE)
    trace = HwTrace.new(blocks=64, instructions=64)
    result = Ptrace.trace_call(code, [20, 22], trace)
    assert result == 42
    assert trace.insn_offsets() == [0x0, 0x3, 0x6, 0xC, 0x11]
    assert not trace.truncated()
    trace.free()
    code.free()


def test_ptrace_trace_call_blockstep():
    """BTF block-step tier: one #DB per TAKEN branch, intra-block instructions
    reconstructed with Capstone — the stream is byte-identical to the
    per-instruction trace_call. Self-skips where PTRACE_SINGLEBLOCK / Capstone
    are absent (e.g. AArch64)."""
    _skip_if_no_ptrace()
    if not Ptrace.blockstep_available():
        pytest.skip("BTF block-step unavailable (needs x86-64 PTRACE_SINGLEBLOCK + Capstone)")
    code = NativeCode.from_bytes(ROUTINE)
    trace = HwTrace.new(blocks=64, instructions=64)
    result = Ptrace.trace_call_blockstep(code, [20, 22], trace)
    assert result == 42
    assert trace.insn_offsets() == [0x0, 0x3, 0x6, 0xC, 0x11]
    assert not trace.truncated()
    trace.free()
    code.free()


def test_descent_edges_and_frames():
    """Call descent (asmtest.hwtrace.Descent): a region that calls an in-blob leaf S
    records the call as an edge at level 1 and descends S as a nested frame at level 2."""
    _skip_if_no_ptrace()
    from asmtest.hwtrace import Descent, DESCENT_RECORD_EDGES, DESCENT_DESCEND_KNOWN
    # R@0: mov rax,rdi; call S(+4); add rax,rsi; ret   S@0xc: inc rax; ret
    blob = bytes([0x48, 0x89, 0xf8, 0xe8, 0x04, 0x00, 0x00, 0x00,
                  0x48, 0x01, 0xf0, 0xc3, 0x48, 0xff, 0xc0, 0xc3])
    code = NativeCode.from_bytes(blob)
    try:
        # Level 1: R's own body + one (call -> S) edge; S is stepped over. The traced
        # region is R only (0xc); S lives beyond it in the same allocation.
        with Descent(DESCENT_RECORD_EDGES) as d:
            r = Ptrace.trace_call_ex(code, [20, 22], None, d, region=0xc)
            assert r == 43
            assert d.frames_len() == 1
            assert d.frame_insns(0) == [0x0, 0x3, 0x8, 0xb]
            edges = d.edges()
            assert len(edges) == 1
            assert edges[0][0] == 0x3 and edges[0][1] == code.base + 0xc
        # Level 2: S (in the allow-set) descends as frame 1.
        with Descent(DESCENT_DESCEND_KNOWN) as d:
            d.allow_region(code.base + 0xc, 4)
            r = Ptrace.trace_call_ex(code, [20, 22], None, d, region=0xc)
            assert r == 43
            assert d.frames_len() == 2
            assert d.frame_base(1) == code.base + 0xc
            assert d.frame_depth(1) == 1
            assert d.frame_insns(1) == [0x0, 0x3]
            assert d.edges() == []
    finally:
        code.free()


def test_ptrace_run_to_rejects_null():
    """run_to wraps the foreign-process software-breakpoint primitive (run an attached
    target to a resolved method, then trace it). A live foreign attach is covered by the
    C suite — driving PTRACE_ATTACH from this harness is impractical, same as
    trace_attached — so here exercise the FFI round-trip safely: a NULL target address is
    rejected (EINVAL) before any ptrace call."""
    _skip_if_no_ptrace()
    assert Ptrace.run_to(os.getpid(), 0) != 0  # ASMTEST_PTRACE_OK == 0


def test_proc_region_by_addr():
    """Discover an executable region's extent from /proc/<pid>/maps by an interior
    address (this process)."""
    _skip_if_no_ptrace()
    code = NativeCode.from_bytes(ROUTINE)
    region = Ptrace.region_by_addr(os.getpid(), code.base + 4)
    assert region is not None
    base, length = region
    assert base == code.base and length >= len(ROUTINE)
    assert Ptrace.region_by_addr(os.getpid(), 0x1) is None  # nothing maps addr 1
    code.free()


def test_proc_perfmap_symbol():
    """Parse a JIT perf-map (/tmp/perf-<pid>.map) and resolve a method by name."""
    _skip_if_no_ptrace()
    pid = os.getpid()
    path = f"/tmp/perf-{pid}.map"
    with open(path, "w") as f:
        f.write("400000 1a void demo(long, long)\n500000 8 other\n")
    try:
        assert Ptrace.perfmap_symbol(pid, "void demo(long, long)") == (0x400000, 0x1A)
        assert Ptrace.perfmap_symbol(pid, "missing") is None
    finally:
        os.remove(path)


def test_jitdump_find(tmp_path):
    """Read a binary jitdump and resolve a method to (addr,size,index) + bytes."""
    _skip_if_no_ptrace()
    path = str(tmp_path / "jit.dump")
    name = b"void demo(long, long)"
    with open(path, "wb") as f:
        # header: magic, version, total_size=40, elf_mach, pad1, pid, timestamp, flags
        f.write(struct.pack("<IIIIIIQQ", 0x4A695444, 1, 40, 62, 0, 0, 0, 0))
        total = 16 + 40 + (len(name) + 1) + len(ROUTINE)
        f.write(struct.pack("<IIQ", 0, total, 5))  # JIT_CODE_LOAD: id, total, ts
        # body: pid, tid, vma, code_addr, code_size, code_index
        f.write(struct.pack("<IIQQQQ", 0, 0, 0x2000, 0x2000, len(ROUTINE), 9))
        f.write(name + b"\x00")
        f.write(ROUTINE)
    m = Ptrace.jitdump_find(path, "void demo(long, long)", want_bytes=64)
    assert m is not None
    assert (m.code_addr, m.code_size, m.code_index, m.timestamp) == (
        0x2000, len(ROUTINE), 9, 5)
    assert m.code == ROUTINE
    assert Ptrace.jitdump_find(path, "missing") is None


# ---- Time-aware code-image recorder (asmtest.hwtrace.CodeImage) ----

# Two 7-byte routines sharing an address; they differ at one byte (add vs sub).
_BLOB_A = b"\x48\x89\xf8\x48\x01\xf0\xc3"
_BLOB_B = b"\x48\x89\xf8\x48\x29\xf0\xc3"


def test_codeimage_temporal():
    """The same-address-different-bytes proof: track a region, rewrite it in place, and
    confirm bytes_at(t0) still returns the OLD bytes where a late snapshot would see B."""
    if not CodeImage.available():
        pytest.skip(f"codeimage unavailable: {CodeImage.skip_reason()}")
    import ctypes
    import mmap as _mmap

    mm = _mmap.mmap(-1, _mmap.PAGESIZE, prot=_mmap.PROT_READ | _mmap.PROT_WRITE)
    mm[:len(_BLOB_A)] = _BLOB_A
    addr = ctypes.addressof(ctypes.c_char.from_buffer(mm))

    img = CodeImage(0)
    try:
        assert img.track(addr, len(_BLOB_A)) == 0
        t0 = img.now()
        assert t0 >= 1
        mm[:len(_BLOB_B)] = _BLOB_B          # re-JIT in place
        assert img.refresh() >= 1
        assert img.bytes_at(addr, t0) == _BLOB_A   # the temporal fix
        assert img.bytes_at(addr, 0) == _BLOB_B    # latest
        assert img.bytes_at(addr + len(_BLOB_A), 0) is None  # untracked
    finally:
        img.free()
        del addr
        mm.close()


def test_codeimage_bpf_probe():
    """The eBPF emission detector round-trips through FFI; it self-skips without
    libbpf/CAP_BPF (the live path is exercised by the C suite / docker-hwtrace-codeimage)."""
    if not CodeImage.bpf_available():
        pytest.skip(f"codeimage bpf unavailable: {CodeImage.bpf_skip_reason()}")
    img = CodeImage(0)
    try:
        assert img.watch_bpf() == 0
        assert ASMTEST_CI_KIND_MPROTECT == 1
    finally:
        img.free()


# ---- §3.1(c) whole-window noise attribution (region_name + symbolize_bucket) ----

def test_symbolize_bucket_and_region_name():
    """The address->name REVERSE resolver (region_name) + IP bucketer (symbolize_buckets).
    Feed a synthetic IP list spanning three regions — this process's own loaded native lib
    (a mapped file), an anonymous mmap ([anon]), and a synthetic perf-map JIT symbol — and
    assert the bucket counts + resolved labels. Host-testable: no live PT capture, no TF
    arming. Mirrors the C reference test_symbolize_bucket."""
    import ctypes as _ct
    import mmap as _mmap

    from asmtest import hwtrace as hw

    if not os.path.exists("/proc/self/maps"):
        pytest.skip("region_name / symbolize_bucket read /proc/<pid>/maps (Linux only)")

    lib = hw._get()
    pid = os.getpid()
    mappath = f"/tmp/perf-{pid}.map"
    with open(mappath, "w") as f:
        f.write("40000000 1000 MyJitMethod\n")  # synthetic JIT symbol range
    mm = _mmap.mmap(-1, _mmap.PAGESIZE, prot=_mmap.PROT_READ | _mmap.PROT_WRITE)
    try:
        # A real mapped-FILE address: any function inside the loaded native lib resolves to
        # its .so pathname — the mapped-file counterpart of the mmap'd anon region.
        ip_self = _ct.cast(lib.asmtest_hwtrace_available, _ct.c_void_p).value
        ip_mmap = _ct.addressof(_ct.c_char.from_buffer(mm)) + 16  # a private mmap region
        ip_jit = 0x40000500                                       # MyJitMethod
        ips = [ip_self, ip_self, ip_self, ip_mmap, ip_jit, ip_jit]

        buckets = HwTrace.symbolize_buckets(ips, pid=pid, cap=8)
        labels = {b.label: b.count for b in buckets}
        # Three distinct regions (lib text, mmap, JIT), every IP attributed.
        assert len(buckets) == 3
        assert sum(b.count for b in buckets) == 6
        # The perf-map JIT symbol wins over any mapping and is counted x2.
        assert labels.get("MyJitMethod") == 2
        # The self-code IPs bucket together (x3); with the JIT (2) and mmap (1) buckets that
        # accounts for all six, so none fall through to [unknown].
        assert "[unknown]" not in labels
        assert any(c == 3 for c in labels.values())
        # The mmap region is attributed to a real named mapping, not [unknown] (its exact
        # label — "[anon]" vs a "/dev/zero" backing — is glibc/kernel dependent).
        assert HwTrace.region_name(ip_mmap, pid=0) is not None

        # region_name keeps the maps pathname + extent for the self-code address (the
        # reverse of Ptrace.region_by_addr, which discards the pathname).
        got = HwTrace.region_name(ip_self, pid=0)
        assert got is not None
        name, start, end = got
        assert name and start <= ip_self < end
        # A miss returns None (nothing maps address 1).
        assert HwTrace.region_name(0x1, pid=0) is None
    finally:
        mm.close()
        os.remove(mappath)
