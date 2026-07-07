"""Wide-arity, mixed integer+FP, and struct-return capture (repo-review N4).

These paths reach past the single-register-file helpers: `capture_args` spills
integer args 7+ onto the stack, `capture_fp` fills the integer AND FP argument
registers in one call, and `capture_sret` routes a memory-class struct through
the hidden result pointer. Fixtures live in the corpus lib
(examples/{args,fp,structs}.s).
"""
import struct

import asmtest


def test_wide_arity_sum8(routine):
    """sum8 takes 8 integer args — 2 of them on the stack on x86-64."""
    r = asmtest.capture_args(routine("sum8"), 1, 2, 3, 4, 5, 6, 7, 8)
    assert r.ret == 36
    assert r.abi_preserved


def test_wide_arity_sum10_positions(routine):
    """Powers of two make a wrong slot/order obvious in the sum."""
    r = asmtest.capture_args(routine("sum10"),
                             1, 2, 4, 8, 16, 32, 64, 128, 256, 512)
    assert r.ret == 1023


def test_mixed_int_fp(routine):
    """mix_scale(n, x) = (double)n * x reads BOTH argument register files."""
    r = asmtest.capture_fp(routine("mix_scale"), iargs=[3], fargs=[2.5])
    assert r.fret == 7.5


def test_struct_return_sret(routine):
    """make_big returns a 24-byte struct{long a,b,c} via the hidden pointer."""
    r, raw = asmtest.capture_sret(routine("make_big"), 24, 7, 8, 9)
    assert struct.unpack("<3q", raw) == (7, 8, 9)
    # make_big also returns the result pointer (rax / x0) — nonzero on success.
    assert r.ret != 0
