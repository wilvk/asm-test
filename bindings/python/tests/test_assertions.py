"""Tier-2 assertion helpers: the pass paths succeed and the failure paths bite."""
import pytest

import asmtest
from asmtest.assertions import (
    assert_abi_clobbered,
    assert_abi_preserved,
    assert_flag,
    assert_fp,
    assert_no_fault,
    assert_reg,
    assert_ret,
    assert_vec_f32,
)


def test_pass_paths(routine):
    r = asmtest.capture(routine("add_signed"), 40, 2)
    assert_ret(r, 42)
    assert_abi_preserved(r)

    r = asmtest.capture(routine("set_carry"))
    assert_flag(r, "CF", set=True)
    r = asmtest.capture(routine("clear_carry"))
    assert_flag(r, "CF", set=False)

    r = asmtest.capture_fp(routine("fp_add"), fargs=[1.5, 2.25])
    assert_fp(r, 3.75)

    r = asmtest.capture_vec(
        routine("vec_add4f"),
        vargs=[asmtest.vec_f32(1, 2, 3, 4), asmtest.vec_f32(10, 20, 30, 40)],
    )
    assert_vec_f32(r, [11, 22, 33, 44])

    r = asmtest.capture(routine("clobbers_rbx"), 1, 2)
    assert_abi_clobbered(r)

    with asmtest.Emulator() as e:
        res = e.call(routine("add_signed"), [40, 2])
    assert_no_fault(res)
    assert_reg(res, "rax", 42)


def test_failure_paths_have_teeth(routine):
    r = asmtest.capture(routine("add_signed"), 40, 2)
    with pytest.raises(AssertionError):
        assert_ret(r, 99)
    with pytest.raises(AssertionError):
        assert_fp(r, 1.0)  # not an FP routine

    clob = asmtest.capture(routine("clobbers_rbx"), 1, 2)
    with pytest.raises(AssertionError):
        assert_abi_preserved(clob)
    with pytest.raises(AssertionError):
        assert_abi_clobbered(r)  # add_signed preserves the ABI
