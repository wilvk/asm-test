"""Live test for the single-step hardware-trace backend via the Python wrapper.

Unlike the DynamoRIO wrapper (which needs a DynamoRIO install) and the PT/AMD
backends (which need specific bare-metal hardware), the SINGLESTEP backend runs on
ANY x86-64 Linux — so this asserts a real, live trace here and in CI/containers,
self-skipping only off x86-64 Linux or without Capstone.
"""
import pytest

from asmtest.hwtrace import (
    HwTrace, NativeCode, SINGLESTEP, AMD_LBR,
    BEST, CEILING_FREE, ASMTEST_HW_EUNAVAIL,
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
