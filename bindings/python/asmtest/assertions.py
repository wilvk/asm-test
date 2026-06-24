"""Tier-2 idiomatic assertions for the Python binding.

Thin helpers over the Regs / EmuResult result objects that raise AssertionError
with a clear message, so a pytest suite reads naturally and failures are legible:

    from asmtest.assertions import assert_ret, assert_abi_preserved
    r = asmtest.capture(fn, 40, 2)
    assert_ret(r, 42)
    assert_abi_preserved(r)

Tier 1 (plain `assert r.ret == 42`) still works; this is the optional idiomatic
layer on top.
"""
from .core import EmuResult, Regs


def assert_ret(regs: Regs, expected: int) -> None:
    """The integer return value equals `expected`."""
    if regs.ret != expected:
        raise AssertionError(f"return value: got {regs.ret}, want {expected}")


def assert_abi_preserved(regs: Regs) -> None:
    """Every callee-saved register was restored."""
    if not regs.abi_preserved:
        raise AssertionError(
            "ABI not preserved: a callee-saved register was not restored"
        )


def assert_abi_clobbered(regs: Regs) -> None:
    """A callee-saved register was *not* restored (the negative case)."""
    if regs.abi_preserved:
        raise AssertionError(
            "expected an ABI violation, but all callee-saved registers were restored"
        )


def assert_flag(regs: Regs, name: str, set: bool = True) -> None:
    """Condition flag `name` (CF/ZF/…) is set (or clear when set=False)."""
    actual = regs.flag_set(name)
    if actual != set:
        raise AssertionError(
            f"flag {name}: got {'set' if actual else 'clear'}, "
            f"want {'set' if set else 'clear'}"
        )


def assert_fp(regs: Regs, expected: float) -> None:
    """The scalar double return equals `expected` exactly."""
    if regs.fret != expected:
        raise AssertionError(f"FP return: got {regs.fret!r}, want {expected!r}")


def assert_vec_f32(regs: Regs, expected, index: int = 0) -> None:
    """The four float32 lanes of vector register `index` equal `expected`."""
    got = regs.vec_f32(index)
    exp = [float(x) for x in expected]
    if got != exp:
        raise AssertionError(f"vector lanes [{index}]: got {got}, want {exp}")


def assert_no_fault(res: EmuResult) -> None:
    """The emulator run completed without an invalid memory access."""
    if res.faulted:
        raise AssertionError(
            f"unexpected fault at 0x{res.fault_addr:x} (kind {res.fault_kind})"
        )


def assert_fault(res: EmuResult) -> None:
    """The emulator run hit an invalid memory access."""
    if not res.faulted:
        raise AssertionError("expected a fault, but the run completed cleanly")


def assert_reg(res: EmuResult, name: str, expected: int) -> None:
    """An x86-64 guest register equals `expected` after the run."""
    got = res.reg(name)
    if got != expected:
        raise AssertionError(f"register {name}: got {got}, want {expected}")
