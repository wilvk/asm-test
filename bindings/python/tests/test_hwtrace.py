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
    HwTrace, NativeCode, Ptrace, SINGLESTEP, AMD_LBR,
    BEST, CEILING_FREE, ASMTEST_HW_EUNAVAIL,
    TIER_HWTRACE, TIER_EMULATOR, FIDELITY_NATIVE, FIDELITY_VIRTUAL,
    TRACE_BEST, TRACE_CEILING_FREE, TRACE_NATIVE_ONLY,
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
    assert trace.covered(0) and trace.covered(0x7)
    assert trace.blocks_len() == 2
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
