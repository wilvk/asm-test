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
    BEST, CEILING_FREE, ASMTEST_HW_EUNAVAIL,
    TIER_HWTRACE, TIER_EMULATOR, FIDELITY_NATIVE, FIDELITY_VIRTUAL,
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
        assert trace.covered(0)
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
