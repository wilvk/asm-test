"""In-line assembler (Keystone) tests for the Python binding.

These exercise the assembler surface — :meth:`Emulator.call_asm` and
:func:`assemble` — which Keystone provides. It ships in libasmtest_emu (the
superset), so these run by default (``make python-asm-test`` is an alias of
``make python-test``); they self-skip only against an older/leaner lib without
Keystone.
"""
import pytest

import asmtest

pytestmark = pytest.mark.skipif(
    not asmtest.asm_available(),
    reason="in-line assembler not in this build (ASMTEST_LIB points at an older/leaner lib)",
)


def test_call_asm_intel():
    with asmtest.Emulator() as e:
        res = e.call_asm("mov rax, rdi; add rax, rsi; ret", [40, 2])
        assert not res.faulted
        assert res.reg("rax") == 42


def test_call_asm_att_three_args():
    # Widened shim: AT&T syntax + a third arg (rdi + rsi + rdx).
    with asmtest.Emulator() as e:
        res = e.call_asm(
            "mov %rdi, %rax; add %rsi, %rax; add %rdx, %rax; ret",
            [10, 20, 12],
            syntax=asmtest.Syntax.ATT,
        )
        assert res.reg("rax") == 42


def test_call_asm_max_insns_caps_execution():
    # Stop after the first `mov rax, rdi`, before the add — rax is the first arg.
    with asmtest.Emulator() as e:
        res = e.call_asm("mov rax, rdi; add rax, rsi; ret", [40, 2], max_insns=1)
        assert res.reg("rax") == 40


def test_call_asm_bad_source_raises():
    with asmtest.Emulator() as e:
        with pytest.raises(asmtest.AsmtestError) as ei:
            e.call_asm("mov rax, nonsense_token")
    # The Keystone diagnostic is carried, not a bare "failed".
    assert len(str(ei.value)) > len("in-line assembly failed: ")


def test_reg_all_documented_names_resolve():
    # Every documented GP register name plus rip/rflags must resolve, not raise
    # KeyError — regression for the manifest publishing only rax/rip/rflags/xmm.
    names = [
        "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip", "rflags",
    ]
    with asmtest.Emulator() as e:
        res = e.call_asm("mov rax, rdi; add rax, rsi; ret", [40, 2])
        # All 18 names must resolve to a value, not raise KeyError.
        vals = {n: res.reg(n) for n in names}
        assert vals["rax"] == 42
        # Distinct offsets map to distinct storage (not all reading the rax slot):
        # rip and rsp are always meaningful and non-zero after a run.
        assert vals["rip"] != 0
        assert vals["rsp"] != 0


def test_assemble_multi_arch():
    # AArch64 `ret` is C0 03 5F D6 — a guest the x86 emulator can't run, but the
    # assemble-only shim still produces its bytes.
    assert asmtest.assemble("ret", asmtest.Arch.ARM64) == bytes([0xC0, 0x03, 0x5F, 0xD6])


def test_assemble_bad_source_raises():
    with pytest.raises(asmtest.AsmtestError):
        asmtest.assemble("frobnicate rax")
