"""asm-test — Python binding (Track P).

Run, capture, and emulate assembly routines through the asm-test framework's two
engines from Python, and let pytest validate the results. Layout comes from the
``asmtest_abi.json`` manifest, so the binding is correct for whatever
architecture the shared library was built for.

    import asmtest
    r = asmtest.capture(add_signed_addr, 40, 2)
    assert r.ret == 42 and r.abi_preserved

    with asmtest.Emulator() as e:
        res = e.call(routine_addr, [40, 2])
        assert not res.faulted and res.reg("rax") == 42
"""
from . import assertions
from .core import (
    Arch,
    AsmtestError,
    Context,
    Emulator,
    EmuResult,
    Regs,
    Syntax,
    asm_available,
    asm_error,
    assemble,
    capture,
    capture_fp,
    capture_vec,
    load,
    vec_f32,
    vec_f64,
)

__all__ = [
    "Arch",
    "AsmtestError",
    "Context",
    "Emulator",
    "EmuResult",
    "Regs",
    "Syntax",
    "asm_available",
    "asm_error",
    "assemble",
    "assertions",
    "capture",
    "capture_fp",
    "capture_vec",
    "load",
    "vec_f32",
    "vec_f64",
]

__version__ = "1.0.0"
