"""Mid-execution guard tests (Track F): memory-write watchpoints and register
invariants, driven through the emulator binding with hand-assembled x86-64 byte
literals (so they run on any host)."""
import pytest

import asmtest

# mov [rdi],rax ; mov [rdi+0x800],rax ; ret  — writes inside then far outside.
TWO_WRITES = bytes([0x48, 0x89, 0x07, 0x48, 0x89, 0x87, 0x00, 0x08, 0x00, 0x00, 0xC3])
WATCH_BASE = 0x400000
# mov rbx,0x99 ; jmp +0 ; ret — the jump target (the ret's block) sees rbx=0x99.
CLOBBER_RBX = bytes([0x48, 0xC7, 0xC3, 0x99, 0x00, 0x00, 0x00, 0xEB, 0x00, 0xC3])


def test_watchpoint_only_flags_escaping_write():
    with asmtest.Emulator() as e:
        assert e.map(WATCH_BASE, 0x1000)  # both writes land in mapped memory
        w = e.watch_writes(WATCH_BASE, 8, asmtest.EMU_WATCH_ONLY)
        e.call(TWO_WRITES, args=[WATCH_BASE])
        e.watch_clear()
        assert w.violated  # the 2nd store escaped [base, base+8)
        assert w.addr == WATCH_BASE + 0x800
        assert w.rip_off == 3  # offset of the escaping store
        w.free()


def test_watchpoint_respected_in_bounds():
    with asmtest.Emulator() as e:
        assert e.map(WATCH_BASE, 0x1000)
        w = e.watch_writes(WATCH_BASE, 0x1000, asmtest.EMU_WATCH_ONLY)
        e.call(TWO_WRITES, args=[WATCH_BASE])  # whole region allowed
        e.watch_clear()
        assert not w.violated
        w.free()


def test_reg_invariant_catches_clobber():
    with asmtest.Emulator() as e:
        g = e.guard_reg("rbx", 0)  # rbx must stay 0 at every block entry
        e.call(CLOBBER_RBX)
        e.guard_reg_clear()
        assert g.violated  # broken at the ret's block (rbx == 0x99 there)
        assert g.got == 0x99
        g.free()


def test_guard_reg_rejects_unknown_name():
    with asmtest.Emulator() as e:
        with pytest.raises(ValueError):
            e.guard_reg("nope", 0)
