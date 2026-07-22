"""CPython managed-host gate (native-trace Phase 0b).

Measures whether the in-process DynamoRIO model survives a real CPython process:
thread-takeover scope (dr_app_running_under_dynamorio), signal chaining (a signal
still reaches the interpreter's handler while DR is started), tracing from the live
interpreter, and single-threaded start/stop bracketing. DR is initialized ONCE for
the module (re-attach per test is itself fragile). Self-skips unless the tier is
built and DynamoRIO is resolvable.

MEASURED RESULT on the dev host (DynamoRIO 11.x, Linux x86-64): the GIL-serialized
single-threaded cases — takeover, signal chaining, tracing, and repeated start/stop
bracketing — are stable. NOT stable: repeated bracketed re-takeover while another
thread is concurrently busy, and creating/taking-over threads repeatedly under DR;
those can crash inside DR's all-thread takeover. This is exactly the fragility the
plan flags as the critical unknown, and the reason JIT/GC-heavy managed runtimes
(with concurrent background threads) should route to the hardware-trace backend
rather than in-process DynamoRIO. CPython's GIL-serialized usage is the supported
managed case; this gate asserts that case works and documents the boundary.
"""
import os
import signal
import sys
import time

import pytest

drtrace = pytest.importorskip("asmtest.drtrace")
NativeTrace = drtrace.NativeTrace
NativeCode = drtrace.NativeCode

ROUTINE = bytes([0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0xC3])  # mov rax,rdi; add rax,rsi; ret


@pytest.fixture(scope="module")
def gate():
    try:
        if not NativeTrace.available():
            pytest.skip("DynamoRIO native-trace tier unavailable (self-skip)")
    except OSError as e:
        pytest.skip(f"libasmtest_drapp not built: {e}")
    if not os.environ.get("ASMTEST_DRCLIENT"):
        pytest.skip("ASMTEST_DRCLIENT not set")
    try:
        NativeTrace.initialize()  # init + start ONCE on the main thread
    except RuntimeError as e:
        pytest.skip(f"dr_init/start failed: {e}")
    yield
    NativeTrace.shutdown()


def test_takeover_scope(gate):
    # The init/calling thread must be under DR control after start().
    assert NativeTrace.under_dynamorio()


@pytest.mark.skipif(
    sys.platform == "darwin",
    reason="DR signal chaining HANGS on the macOS fork build: SIGUSR1 raised "
    "while attached never reaches CPython's handler and the process wedges "
    "(DR_SIGNAL_DELIVER path; measured 2026-07-22, recorded in "
    "macos-drtrace-plan.md — a fork-side DR fix, not a wrapper issue)",
)
def test_signal_chaining(gate):
    # A signal raised while DR is started must still reach CPython's own handler
    # (the client returns DR_SIGNAL_DELIVER). If DR swallowed it, `got` stays empty.
    got = []
    prev = signal.signal(signal.SIGUSR1, lambda s, f: got.append(s))
    try:
        os.kill(os.getpid(), signal.SIGUSR1)
        for _ in range(100):
            if got:
                break
            time.sleep(0.005)
        assert got == [signal.SIGUSR1]
    finally:
        signal.signal(signal.SIGUSR1, prev)


def test_trace_under_managed_host(gate):
    code = NativeCode.from_bytes(ROUTINE)
    tr = NativeTrace.new(blocks=16, instructions=0)
    tr.register("g", code)
    with tr.region("g"):
        r = code.call(20, 22)
    assert r == 42 and tr.covered(0)
    tr.unregister("g"); code.free(); tr.free()


def test_start_stop_bracketing_single_threaded(gate):
    # Bracket model: stop -> native, start -> re-takeover. Single-threaded /
    # GIL-serialized (the supported CPython case) is stable across many brackets.
    code = NativeCode.from_bytes(ROUTINE)
    tr = NativeTrace.new(blocks=16, instructions=0)
    tr.register("b", code)
    for _ in range(5):
        NativeTrace.stop()
        assert not NativeTrace.under_dynamorio()
        NativeTrace.start()
        with tr.region("b"):
            assert code.call(1, 2) == 3
    assert tr.covered(0)
    tr.unregister("b"); code.free(); tr.free()
