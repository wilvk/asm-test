"""Tests for the in-process DynamoRIO native-trace wrapper (asmtest.drtrace).

Self-skips unless the tier is built AND DynamoRIO is resolvable — i.e. unless
ASMTEST_DRAPP_LIB / ASMTEST_DRCLIENT (and ASMTEST_DR_LIB or DYNAMORIO_HOME) point
at a built libasmtest_drapp + client on a DynamoRIO-capable Linux x86-64 host.
The `make docker-drtrace` lane sets these up in a container; on a dev box build
with `make shared-drtrace drtrace-client DYNAMORIO_HOME=...` and export the env.
"""
import os

import pytest

drtrace = pytest.importorskip("asmtest.drtrace")
NativeTrace = drtrace.NativeTrace
NativeCode = drtrace.NativeCode

# mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
ROUTINE = bytes([0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00,
                 0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3])


def _skip_if_unavailable():
    try:
        if not NativeTrace.available():
            pytest.skip("DynamoRIO native-trace tier unavailable (self-skip)")
    except OSError as e:
        pytest.skip(f"libasmtest_drapp not built: {e}")
    if not os.environ.get("ASMTEST_DRCLIENT"):
        pytest.skip("ASMTEST_DRCLIENT not set (build the DR client)")


@pytest.fixture(scope="module")
def started():
    _skip_if_unavailable()
    try:
        NativeTrace.initialize()
    except RuntimeError as e:
        pytest.skip(f"dr_init/start failed: {e}")
    yield
    NativeTrace.shutdown()


def test_block_coverage_and_accumulation(started):
    code = NativeCode.from_bytes(ROUTINE)
    tr = NativeTrace.new(blocks=64, instructions=0)
    tr.register("add2", code)

    with tr.region("add2"):
        r = code.call(20, 22)
    assert r == 42
    assert tr.covered(0)  # entry block

    before = tr.blocks_len
    with tr.region("add2"):
        r2 = code.call(60, 60)  # 120 > 100 -> dec -> 119, takes the other block
    assert r2 == 119
    assert tr.blocks_len >= before
    assert NativeTrace.marker_error() == 0

    tr.unregister("add2")
    code.free()
    tr.free()


def test_instruction_mode(started):
    code = NativeCode.from_bytes(ROUTINE)
    tr = NativeTrace.new(blocks=64, instructions=64)
    tr.register("add2i", code)
    with tr.region("add2i"):
        r = code.call(1, 2)
    assert r == 3
    assert tr.insns_total >= 4  # ordered instruction stream recorded
    tr.unregister("add2i")
    code.free()
    tr.free()
