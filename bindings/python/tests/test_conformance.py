"""Replay the cross-language conformance corpus (Track 0.4) from Python.

This is the multi-language analog of the C reference runner: the binding loads
the SAME ``corpus.json`` the C reference emits, calls each canonical routine
through the binding-ABI entry points, and must reproduce the same expected
results. Passing here proves the Python binding wired the ABI up correctly.

Driven by ``make python-test``, which sets:
  ASMTEST_CORPUS_JSON  the corpus emitted by `make conformance`
  ASMTEST_CORPUS_LIB   a shared lib exporting the canonical routines as symbols
"""
import json
import os
from pathlib import Path

import pytest

import asmtest

_CORPUS_JSON = os.environ.get("ASMTEST_CORPUS_JSON")


def _load_cases():
    if not _CORPUS_JSON or not Path(_CORPUS_JSON).is_file():
        return []
    return json.loads(Path(_CORPUS_JSON).read_text())["corpus"]["cases"]


_CASES = _load_cases()


@pytest.mark.skipif(not _CASES, reason="no corpus (run `make python-test`)")
@pytest.mark.parametrize("case", _CASES, ids=[c["name"] for c in _CASES])
def test_corpus_case(case, routine):
    fn = routine(case["routine"])
    expect = case["expect"]

    if case["tier"] == "capture":
        call = case["call"]
        if call == "int":
            r = asmtest.capture(fn, *case.get("args", []))
            if "ret" in expect:
                assert r.ret == expect["ret"]
            if "abi_preserved" in expect:
                assert r.abi_preserved is expect["abi_preserved"]
            for fl in expect.get("flags_set", []):
                assert r.flag_set(fl), f"{fl} should be set"
            for fl in expect.get("flags_clear", []):
                assert not r.flag_set(fl), f"{fl} should be clear"
        elif call == "fp":
            r = asmtest.capture_fp(fn, fargs=case.get("fargs", []))
            assert r.fret == expect["fret"]
        elif call == "vec":
            vargs = [asmtest.vec_f32(*lanes) for lanes in case["vargs"]]
            r = asmtest.capture_vec(fn, vargs=vargs)
            assert r.vec_f32(0) == [float(x) for x in expect["vret_f32"]]
        else:
            pytest.fail(f"unknown capture call type: {call}")

    elif case["tier"] == "emu":
        with asmtest.Emulator() as e:
            res = e.call(fn, case.get("args", []))
        want_fault = expect.get("faulted", False)
        # A clean run completes (ran); a fault stops it short — but as data.
        assert res.ran is (not want_fault)
        assert res.faulted is want_fault
        if "fault_addr" in expect:
            assert res.fault_addr == expect["fault_addr"]
        if "fault_kind" in expect:
            assert res.fault_kind == expect["fault_kind"]
        for reg_name, reg_val in expect.get("reg", {}).items():
            assert res.reg(reg_name) == reg_val
        # Lane 0 of the named XMM registers (the FP/vector file).
        for idx, val in expect.get("xmm_f64", {}).items():
            assert res.xmm_f64(int(idx), 0) == val
        # The widened GP read reaches rip/rflags on every run: x86 always keeps
        # rflags bit 1 set, so a nonzero mask proves the field resolved.
        assert res.reg("rflags") & 0x2

    else:
        pytest.fail(f"unknown tier: {case['tier']}")
