"""Data-flow tier Python binding (Phase 6, increment 1): the GC-move canonicalizer.

Mirrors the C suite test_dataflow_gcmove's forward-to-final semantics through the
ctypes wrapper. Runs under pytest, or standalone (`python3 test_dataflow.py`) as a
TAP-ish reporter so it validates on a host without pytest installed.
"""

import os
import sys

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


def _main():
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
