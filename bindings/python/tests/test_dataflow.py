"""Data-flow tier Python binding: the pure analysis pipeline + F7 live attach.

Mirrors the C suite test_dataflow_gcmove's forward-to-final semantics through the
ctypes wrapper, exercises the L0/L1/L2 pipeline, and — F7 — captures data flow off
a REAL live victim process by pid (see the live-attach section at the bottom).
Runs under pytest, or standalone (`python3 test_dataflow.py`) as a TAP-ish reporter
so it validates on a host without pytest installed. The live tests need the lane's
env (`make dataflow-python-test`), which builds the lib and the victim.
"""

import os
import platform
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from asmtest import dataflow  # noqa: E402


def _lib_available():
    try:
        dataflow._load()
        return True
    except OSError:
        return False


# Under pytest, skip cleanly when libasmtest_dataflow is not built (e.g. the general
# `bindings (python)` job, which does not build shared-dataflow) so this never reddens
# an unrelated run. `make dataflow-python-test` builds the lib + runs it for real (the
# standalone __main__ path below). pytest itself is optional (the host may lack it).
try:
    import pytest  # noqa: E402

    pytestmark = pytest.mark.skipif(
        not _lib_available(),
        reason="libasmtest_dataflow not built; run `make dataflow-python-test`",
    )
except ImportError:
    pytest = None


def test_empty_move_set_is_identity():
    assert dataflow.gcmove_canon([], 0, 0x1234) == 0x1234


def test_forward_pre_move_address_to_final():
    # Object A: [0x1000, 0x1100) relocated to 0x2000 by a compaction at step 5.
    moves = [(0x1000, 0x2000, 0x100, 5)]
    # An address inside A, observed BEFORE the move (step 3), forwards to the new
    # base + the same intra-object offset — the affine remap that preserves
    # (object, field) identity.
    assert dataflow.gcmove_canon(moves, 3, 0x1010) == 0x2010
    # The object's base itself forwards to the new base.
    assert dataflow.gcmove_canon(moves, 3, 0x1000) == 0x2000
    # The last byte of the half-open window forwards; one past the end does NOT.
    assert dataflow.gcmove_canon(moves, 3, 0x10FF) == 0x20FF
    assert dataflow.gcmove_canon(moves, 3, 0x1100) == 0x1100


def test_post_move_observation_not_forwarded():
    # An address observed at/after the move's step is already in the new space (a
    # different object may now occupy the vacated old slot) — never forwarded.
    moves = [(0x1000, 0x2000, 0x100, 5)]
    assert dataflow.gcmove_canon(moves, 5, 0x1010) == 0x1010
    assert dataflow.gcmove_canon(moves, 7, 0x1010) == 0x1010


def test_address_outside_every_range_unchanged():
    moves = [(0x1000, 0x2000, 0x100, 5)]
    assert dataflow.gcmove_canon(moves, 3, 0x3000) == 0x3000


def test_two_compactions_compose_to_final():
    # A moves 0x1000->0x2000 at step 3, then 0x2000->0x3000 at step 6. An address
    # observed at step 1 forwards through BOTH to its final resting place.
    moves = [(0x1000, 0x2000, 0x100, 3), (0x2000, 0x3000, 0x100, 6)]
    assert dataflow.gcmove_canon(moves, 1, 0x1010) == 0x3010
    # Observed between the two moves (step 4 >= 3, < 6): only the second applies.
    assert dataflow.gcmove_canon(moves, 4, 0x2010) == 0x3010


def test_method_resolve_point_and_range():
    # Foo [0x1000,+0x40) v3; Bar [0x2000,+0x20) v1; Baz point-match at 0x3000 v2.
    methods = [
        (0x1000, 0x40, "Foo", 3),
        (0x2000, 0x20, "Bar", 1),
        (0x3000, 0, "Baz", 2),
    ]
    assert dataflow.method_resolve_pc(methods, 0x1000) == 0  # Foo start
    assert dataflow.method_resolve_pc(methods, 0x103F) == 0  # Foo last byte (half-open)
    assert dataflow.method_resolve_pc(methods, 0x1040) == -1  # one past Foo -> none
    assert dataflow.method_resolve_pc(methods, 0x2010) == 1  # Bar
    assert dataflow.method_resolve_pc(methods, 0x3000) == 2  # Baz point match
    assert dataflow.method_resolve_pc(methods, 0x3001) == -1  # Baz is point-only
    assert dataflow.method_resolve_pc(methods, 0x9999) == -1  # nothing


def test_method_resolve_tiered_rejit_newest_version_wins():
    # Two records cover 0x1000 after an in-place re-JIT (v1 then v5); the greatest
    # version — the newest load — wins.
    methods = [(0x1000, 0x40, "Foo", 1), (0x1000, 0x40, "Foo", 5)]
    assert dataflow.method_resolve_pc(methods, 0x1010) == 1


def test_method_resolve_empty_map():
    assert dataflow.method_resolve_pc([], 0x1000) == -1


def test_pipeline_register_move_chain():
    # A register move chain: r10 -> r11 -> r12. forward(0) reaches every step;
    # backward(2) reaches every step. This round-trip also validates the
    # at_val_rec_t ctypes layout end to end (a wrong layout mis-splits read/write
    # sets or the slice seed, breaking these sets).
    REG = dataflow.LOC_REG
    with dataflow.ValueTrace() as vt:
        vt.step(0x00, reads=[], writes=[(REG, 10)])            # def r10
        vt.step(0x03, reads=[(REG, 10)], writes=[(REG, 11)])   # r11 <- r10
        vt.step(0x06, reads=[(REG, 11)], writes=[(REG, 12)])   # r12 <- r11
        assert vt.forward_slice(0) == {0, 1, 2}
        assert vt.backward_slice(2) == {0, 1, 2}
        assert vt.backward_slice(1) == {0, 1}   # stops before the consumer
        assert vt.forward_slice(2) == {2}        # nothing downstream


def test_pipeline_load_after_store_via_memory():
    # A store then a load at the SAME absolute address forms a def-use edge through
    # memory (not registers): step0 writes M[A]; step1 reads M[A] -> writes r9.
    REG, MEM = dataflow.LOC_REG, dataflow.LOC_MEM_ABS
    A = 0x7FFF0000
    with dataflow.ValueTrace() as vt:
        vt.step(0x00, reads=[(REG, 8)], writes=[(MEM, A)])     # M[A] <- r8
        vt.step(0x04, reads=[(MEM, A)], writes=[(REG, 9)])     # r9 <- M[A]
        assert vt.forward_slice(0) == {0, 1}   # the store's def reaches the load
        assert vt.backward_slice(1) == {0, 1}


def test_pipeline_no_spurious_edge():
    # Two independent chains must not cross-link: r1->r2 and r3->r4 are disjoint.
    REG = dataflow.LOC_REG
    with dataflow.ValueTrace() as vt:
        vt.step(0x00, writes=[(REG, 1)])
        vt.step(0x02, reads=[(REG, 1)], writes=[(REG, 2)])
        vt.step(0x04, writes=[(REG, 3)])
        vt.step(0x06, reads=[(REG, 3)], writes=[(REG, 4)])
        assert vt.forward_slice(0) == {0, 1}   # r1 chain only
        assert vt.forward_slice(2) == {2, 3}   # r3 chain only, no cross-link


# ---------------------------------------------------------------------------
# F7 — live-attach data flow: capture over an ATTACHED PID.
#
# These attach to a REAL, independently-running victim process (bindings/
# dataflow_victim.c), not to a mock and not to a child we trace-forked. What makes
# them non-vacuous: every assertion below is POSITIVE and keyed to a value only a
# working capture can produce — the region's return value, the exact step count,
# the def-use shape. There is deliberately no `assert not truncated`-style check,
# the shape that let the Python hwtrace lane pass while blind: a failed capture
# leaves an EMPTY trace, so a test whose real assertions sit behind "if we got
# anything" skips itself exactly when it should shout.
# ---------------------------------------------------------------------------

# The tier is Linux x86-64 only (src/dataflow_ptrace.c's own #if). On such a host
# the live tests MUST run: an unavailable tier there means the lib was linked
# without Capstone — a build defect that has to be RED, not a skip. Anywhere else
# the ISA genuinely lacks the tier and skipping is the honest answer.
_LIVE_EXPECTED = platform.system() == "Linux" and platform.machine() == "x86_64"
_VICTIM = os.environ.get("ASMTEST_DATAFLOW_VICTIM")


class _Victim:
    """A live victim process to attach to: spawn it, learn its region base.

    The victim publishes `base=0x<hex> len=<n>` on stdout and then loops calling
    that region with (a, b) forever, bumping an 8-byte counter file. `a` and `b`
    are OURS, so the expected result is a property of this run — a wrapper that
    hardcodes an answer cannot satisfy two victims with different args.
    """

    def __init__(self, tmpdir, a, b):
        self.a, self.b = a, b
        self.counter_path = os.path.join(tmpdir, f"victim-{a}-{b}.counter")
        self.proc = subprocess.Popen(
            [_VICTIM, self.counter_path, str(a), str(b)],
            stdout=subprocess.PIPE, text=True,
        )
        line = self.proc.stdout.readline()  # blocks until the victim is looping
        if not line.startswith("base=0x"):
            self.close()
            raise AssertionError(f"victim handshake failed: {line!r}")
        base_s, len_s = line.split()
        self.base = int(base_s[len("base="):], 16)
        self.len = int(len_s[len("len="):])
        self.pid = self.proc.pid

    def counter(self):
        with open(self.counter_path, "rb") as f:
            return struct.unpack("<Q", f.read(8))[0]

    def close(self):
        self.proc.kill()
        self.proc.wait()
        if self.proc.stdout:
            self.proc.stdout.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


def _tmpdir():
    import tempfile

    return tempfile.mkdtemp(prefix="asmtest-df-")


def _check_rc(rc, what):
    # ETRACE is NOT a skip here. ptrace is a capability the lane can be GIVEN
    # (--cap-add=SYS_PTRACE / seccomp=unconfined), and the victim opts in via
    # PR_SET_PTRACER_ANY, so a refusal means the lane is misconfigured — which
    # must be loud. Only genuinely absent hardware/ISA earns a skip.
    if rc == dataflow.PTRACE_ETRACE:
        raise AssertionError(
            f"{what}: ptrace refused (ETRACE). The lane needs ptrace permission — "
            "run it with --cap-add=SYS_PTRACE (and seccomp=unconfined if the "
            "default profile blocks ptrace). Not a valid skip."
        )
    assert rc == dataflow.PTRACE_OK, f"{what}: rc={rc}"


def test_live_attach_tier_is_real():
    if not _LIVE_EXPECTED:
        return  # genuinely off-tier ISA
    # Probe, not a symbol-resolves check: EINVAL (real) vs ENOSYS (stub).
    assert dataflow.live_attach_available(), (
        "live-attach tier reports ENOSYS on linux/x86_64 — libasmtest_dataflow was "
        "built without Capstone or without src/dataflow_ptrace.c"
    )


def test_attach_pid_captures_live_region():
    if not _LIVE_EXPECTED or not _VICTIM:
        return  # pytest without the lane's env; _main() bails instead of skipping
    tmp = _tmpdir()
    with _Victim(tmp, 7, 5) as vic, dataflow.ValueTrace(64, 512) as vt:
        rc, result = vt.attach_pid(vic.pid, vic.base, vic.len)
        _check_rc(rc, "attach_pid")
        # The region really executed IN the victim: rax = rdi + rsi.
        assert result == 12, f"attach_pid: result={result}, want 12"
        # Exactly the six in-region instructions of df_chain — not "some".
        assert vt.steps == 6, f"attach_pid: steps={vt.steps}, want 6"
        assert vt.recs > 0, "attach_pid: no operand records captured"

        # SURVIVAL: we attached to a process we did not own; it must outlive the
        # detach. The counter is written by the victim, so movement proves it runs.
        c0 = vic.counter()
        time.sleep(0.05)
        assert vic.counter() > c0, "attach_pid: victim did not survive the detach"

        # The def-use built over the LIVE trace has the real shape: the store at
        # step 1 feeds the load at step 2 (a MEMORY edge through [rsp-8]), which
        # chains to the lea and the final mov.
        assert vt.backward_slice(4) == {0, 1, 2, 3, 4}, (
            f"attach_pid: backward_slice(4)={vt.backward_slice(4)}"
        )
        fwd = vt.forward_slice(0)
        assert 4 in fwd, "attach_pid: forward_slice(0) misses the final mov"
        # Negative control on the SHAPE: the `ret` (step 5) consumes none of the
        # chain, so a slice containing it would mean the graph links everything.
        assert 5 not in fwd, "attach_pid: forward_slice(0) wrongly reached the ret"


def test_attach_pid_result_tracks_the_victims_args():
    # THE anti-hardcode control. A second victim, different args, same wrapper: a
    # stubbed/faked capture cannot return 12 here and 42 there.
    if not _LIVE_EXPECTED or not _VICTIM:
        return  # pytest without the lane's env; _main() bails instead of skipping
    tmp = _tmpdir()
    with _Victim(tmp, 17, 25) as vic, dataflow.ValueTrace(64, 512) as vt:
        rc, result = vt.attach_pid(vic.pid, vic.base, vic.len)
        _check_rc(rc, "attach_pid(17,25)")
        assert result == 42, f"attach_pid: result={result}, want 42"
        assert vt.steps == 6


def test_attach_pid_tid_any_thread():
    if not _LIVE_EXPECTED or not _VICTIM:
        return  # pytest without the lane's env; _main() bails instead of skipping
    tmp = _tmpdir()
    with _Victim(tmp, 9, 4) as vic, dataflow.ValueTrace(64, 512) as vt:
        # only_tid=0: step whichever thread enters the region (here, the only one).
        rc, result = vt.attach_pid_tid(vic.pid, 0, vic.base, vic.len)
        _check_rc(rc, "attach_pid_tid")
        assert result == 13, f"attach_pid_tid: result={result}, want 13"
        assert vt.steps == 6


def test_attach_jit_reports_survival():
    if not _LIVE_EXPECTED or not _VICTIM:
        return  # pytest without the lane's env; _main() bails instead of skipping
    tmp = _tmpdir()
    with _Victim(tmp, 20, 3) as vic, dataflow.ValueTrace(64, 512) as vt:
        rc, result, survived = vt.attach_jit(vic.pid, 0, vic.base, vic.len)
        _check_rc(rc, "attach_jit")
        assert result == 23, f"attach_jit: result={result}, want 23"
        assert vt.steps == 6
        # The entry point's own survival report — the house rule that a foreign
        # target is never killed, asserted from the producer's side.
        assert survived == 1, "attach_jit: did not report the target as survived"
        c0 = vic.counter()
        time.sleep(0.05)
        assert vic.counter() > c0, "attach_jit: victim did not survive the detach"


def test_attach_rejects_bad_arguments():
    # Negative control: the wrapper must surface the producer's rejections rather
    # than manufacture success. A zero-length region and a nonexistent pid are the
    # two the C entry point separates (EINVAL vs a seize failure).
    if not _LIVE_EXPECTED:
        return
    with dataflow.ValueTrace(8, 8) as vt:
        rc, _ = vt.attach_pid(12345, 0x1000, 0)  # code_len = 0
        assert rc == dataflow.PTRACE_EINVAL, f"zero-length region: rc={rc}"
        rc, _ = vt.attach_pid(0, 0x1000, 21)  # pid <= 0
        assert rc == dataflow.PTRACE_EINVAL, f"pid 0: rc={rc}"
    with dataflow.ValueTrace(8, 8) as vt:
        # A pid that does not exist cannot be seized: never OK.
        rc, _ = vt.attach_pid(0x7FFFFFF0, 0x1000, 21)
        assert rc != dataflow.PTRACE_OK, "attaching to a nonexistent pid returned OK"


def _main():
    if _LIVE_EXPECTED and not _VICTIM:
        # The make lane always exports this. Missing == misconfigured lane, and a
        # silent skip of every live test is exactly the hole this suite must not have.
        print("Bail out! ASMTEST_DATAFLOW_VICTIM unset; run `make dataflow-python-test`")
        return 1
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    n = 0
    failed = False
    for t in tests:
        n += 1
        try:
            t()
            print(f"ok {n} - {t.__name__}")
        except Exception as e:  # noqa: BLE001
            print(f"not ok {n} - {t.__name__}: {e}")
            failed = True
    print(f"1..{n}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(_main())
