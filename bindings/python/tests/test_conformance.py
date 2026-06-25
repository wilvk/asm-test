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


_SYNTAX = {"intel": asmtest.Syntax.INTEL, "att": asmtest.Syntax.ATT}
_ARCH = {
    "x86_64": asmtest.Arch.X86_64, "arm64": asmtest.Arch.ARM64,
    "riscv": asmtest.Arch.RISCV64, "arm": asmtest.Arch.ARM32,
}


@pytest.mark.skipif(not _CASES, reason="no corpus (run `make python-test`)")
@pytest.mark.parametrize("case", _CASES, ids=[c["name"] for c in _CASES])
def test_corpus_case(case, routine):
    expect = case["expect"]
    tier = case["tier"]

    # The raw-bytes / assembler tiers carry their own code or source text rather
    # than a named corpus routine.
    if tier == "emu_bytes":
        _run_emu_bytes(case, expect)
        return
    if tier == "emu_trace":
        _run_emu_trace(case, expect)
        return
    if tier == "asm":
        _run_asm(case, expect)
        return

    fn = routine(case["routine"])

    if tier == "capture":
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


def _check_emu_expect(res, expect, is_guest):
    """Shared expectation checks over an x86 EmuResult or a cross-arch GuestResult."""
    assert res.faulted is expect.get("faulted", False)
    for name, val in expect.get("reg", {}).items():
        assert res.reg(name) == val, f"{name} != {val}"
    for idx, val in expect.get("xmm_f64", {}).items():
        assert res.xmm_f64(int(idx), 0) == val
    if "vret_f32" in expect:
        lanes = [res.xmm_f32(0, i) for i in range(4)]
        assert lanes == [float(x) for x in expect["vret_f32"]]


def _run_emu_bytes(case, expect):
    """A raw-machine-code routine run under the emulator — the x86-64 guest (the
    new wide-int / FP / vector / Win64 calls) or a cross-arch guest (arm64/riscv/
    arm), each driven from the case's `code` bytes."""
    code = bytes(case["code"])
    guest = case["guest"]
    call = case.get("call", "int")
    args = case.get("args", [])

    if guest in ("x86_64", "x86_64_win64"):
        with asmtest.Emulator() as e:
            if guest == "x86_64_win64":
                res = e.call_win64(code, args)
            elif call == "fp":
                res = e.call_fp(code, fargs=case.get("fargs", []))
            elif call == "vec":
                vargs = [asmtest.vec_f32(*lanes) for lanes in case["vargs"]]
                res = e.call_vec(code, vargs=vargs)
            else:
                res = e.call(code, args)
            _check_emu_expect(res, expect, is_guest=False)
        return

    # Cross-arch guest (raw bytes, runs on any host).
    with asmtest.GuestEmulator(guest) as e:
        if call == "fp":
            res = e.call_fp(code, fargs=case.get("fargs", []))
        elif call == "vec":
            vargs = [asmtest.vec_f32(*lanes) for lanes in case["vargs"]]
            res = e.call_vec(code, vargs=vargs)
        else:
            res = e.call(code, args)
        try:
            assert res.faulted is expect.get("faulted", False)
            for name, val in expect.get("reg", {}).items():
                assert res.reg(name) == val, f"{name} != {val}"
        finally:
            res.free()


def _run_emu_trace(case, expect):
    """A cross-arch routine run with execution-trace / coverage recording: the
    `covered` / `uncovered` block byte-offsets are verdicts, like a fault."""
    code = bytes(case["code"])
    with asmtest.GuestEmulator(case["guest"]) as e, asmtest.Trace() as tr:
        res = e.call_traced(code, case.get("args", []), trace=tr)
        try:
            assert res.faulted is expect.get("faulted", False)
            for name, val in expect.get("reg", {}).items():
                assert res.reg(name) == val
            for off in expect.get("covered", []):
                assert tr.covered(off), f"block {off} should be covered"
            for off in expect.get("uncovered", []):
                assert not tr.covered(off), f"block {off} should NOT be covered"
        finally:
            res.free()


def _run_asm(case, expect):
    """The in-line assembler tier (Keystone): run assembled text on the emulator,
    assert a bad string fails, or assemble-to-bytes for any arch. Self-skips when
    the loaded lib has no assembler."""
    if not asmtest.asm_available():
        pytest.skip("in-line assembler not in this build")
    call = case["call"]
    syntax = _SYNTAX[case.get("syntax", "intel")]

    if call == "run":
        with asmtest.Emulator() as e:
            res = e.call_asm(case["src"], case.get("args", []), syntax=syntax)
            assert res.faulted is expect.get("faulted", False)
            for name, val in expect.get("reg", {}).items():
                assert res.reg(name) == val
    elif call == "error":
        with asmtest.Emulator() as e:
            with pytest.raises(asmtest.AsmtestError):
                e.call_asm(case["src"], syntax=syntax)
    elif call == "assemble":
        got = asmtest.assemble(case["src"], _ARCH[case["arch"]], syntax)
        assert list(got) == list(expect["bytes"])
    else:
        pytest.fail(f"unknown asm call: {call}")
