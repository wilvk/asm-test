"""Live test for the single-step hardware-trace backend via the Python wrapper.

Unlike the DynamoRIO wrapper (which needs a DynamoRIO install) and the PT/AMD
backends (which need specific bare-metal hardware), the SINGLESTEP backend runs on
ANY x86-64 Linux — so this asserts a real, live trace here and in CI/containers,
self-skipping only off x86-64 Linux or without Capstone.
"""
import pytest

from asmtest.hwtrace import HwTrace, NativeCode, SINGLESTEP

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
