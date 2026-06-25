"""High-level, Pythonic surface over the asm-test binding ABI.

Two ways to run an assembly routine, mirroring the C framework's two tiers:

* :func:`capture` / :func:`capture_fp` / :func:`capture_vec` run the routine
  through the **real ABI** on real hardware (the capture trampoline) and return
  a :class:`Regs` snapshot — return value, flags, callee-saved state, FP/vector
  lanes.
* :class:`Emulator` runs the routine inside the **Unicorn emulator**, returning
  an :class:`EmuResult` whose faults are *data*, not a crash.

All struct fields are read at offsets taken from the layout manifest, so the
binding is correct for whatever architecture the shared library was built for.
"""
import ctypes as C
import struct

from . import _native


class Context:
    """The loaded library + manifest. Created once, lazily (see :func:`load`)."""

    def __init__(self):
        self.lib, self.lib_path = _native.find_library()
        m = _native.find_manifest()
        _native.declare(self.lib)
        self.version = m["version"]
        self.arch = m["host_arch"]
        self.flags = dict(m["flags"])
        self.sentinels = {k: int(x, 16) for k, x in m["sentinels"].items()}
        self._structs = {s["name"]: s for s in m["structs"]}
        self.has_emu = _native.has_emu(self.lib)
        self.has_asm = _native.has_asm(self.lib)

    def size(self, struct_name):
        return self._structs[struct_name]["size"]

    def offset(self, struct_name, field_name):
        for f in self._structs[struct_name]["fields"]:
            if f["name"] == field_name:
                return f["offset"]
        raise KeyError(f"{struct_name}.{field_name} not in manifest")


_ctx = None


def load():
    """Return the process-wide :class:`Context`, initializing it on first use."""
    global _ctx
    if _ctx is None:
        _ctx = Context()
    return _ctx


def _addr(fn):
    """Coerce a routine reference (int address or ctypes function/pointer)."""
    if isinstance(fn, int):
        return fn
    return C.cast(fn, C.c_void_p).value


def _longs(values, n):
    arr = (C.c_long * n)()
    for i, val in enumerate(values[:n]):
        arr[i] = int(val)
    return arr


# ------------------------------------------------------------------ #
# Capture tier                                                        #
# ------------------------------------------------------------------ #
class Regs:
    """A captured register/flag snapshot, read via manifest offsets."""

    def __init__(self, buf):
        self._buf = buf  # keep the live buffer alive for the verdict shim
        self._raw = bytes(buf)

    def _u64(self, off):
        return struct.unpack_from("<Q", self._raw, off)[0]

    @property
    def ret(self):
        return self._u64(load().offset("regs_t", "ret"))

    @property
    def flags(self):
        return self._u64(load().offset("regs_t", "flags"))

    @property
    def fret(self):
        return struct.unpack_from("<d", self._raw, load().offset("regs_t", "fret"))[0]

    def vec_bytes(self, index=0):
        base = load().offset("regs_t", "vec") + index * 16
        return self._raw[base : base + 16]

    def vec_f32(self, index=0):
        return list(struct.unpack("<4f", self.vec_bytes(index)))

    def vec_f64(self, index=0):
        return list(struct.unpack("<2d", self.vec_bytes(index)))

    @property
    def abi_preserved(self):
        """All callee-saved registers restored — via the native verdict shim."""
        return load().lib.asmtest_check_abi(C.cast(self._buf, C.c_void_p), None, 0) == 0

    def flag_set(self, name):
        """True if condition flag `name` (CF/ZF/…, per the manifest) is set."""
        return bool(self.flags & load().flags[name])


def capture(fn, *args):
    """Call `fn` through the integer ABI (up to 6 args) and capture state."""
    c = load()
    buf = C.create_string_buffer(c.size("regs_t"))
    c.lib.asm_call_capture(
        C.cast(buf, C.c_void_p), C.c_void_p(_addr(fn)),
        C.cast(_longs(list(args), 6), C.c_void_p),
    )
    return Regs(buf)


def capture_fp(fn, iargs=(), fargs=()):
    """Call `fn` marshalling up to 8 doubles into the FP arg registers."""
    c = load()
    buf = C.create_string_buffer(c.size("regs_t"))
    fa = (C.c_double * 8)()
    for i, val in enumerate(list(fargs)[:8]):
        fa[i] = float(val)
    c.lib.asm_call_capture_fp(
        C.cast(buf, C.c_void_p), C.c_void_p(_addr(fn)),
        C.cast(_longs(list(iargs), 6), C.c_void_p), C.cast(fa, C.c_void_p),
    )
    return Regs(buf)


def capture_vec(fn, iargs=(), vargs=()):
    """Call `fn` marshalling up to 8 128-bit vectors (16-byte `bytes` each)."""
    c = load()
    buf = C.create_string_buffer(c.size("regs_t"))
    vb = (C.c_ubyte * (8 * 16))()
    for i, vec in enumerate(list(vargs)[:8]):
        raw = bytes(vec)
        if len(raw) != 16:
            raise ValueError("each vector arg must be exactly 16 bytes")
        vb[i * 16 : i * 16 + 16] = raw
    c.lib.asm_call_capture_vec(
        C.cast(buf, C.c_void_p), C.c_void_p(_addr(fn)),
        C.cast(_longs(list(iargs), 6), C.c_void_p), C.cast(vb, C.c_void_p),
    )
    return Regs(buf)


def vec_f32(a, b, c, d):
    """Pack four float32 lanes into a 16-byte vector arg."""
    return struct.pack("<4f", a, b, c, d)


def vec_f64(a, b):
    """Pack two float64 lanes into a 16-byte vector arg."""
    return struct.pack("<2d", a, b)


# ------------------------------------------------------------------ #
# Emulator tier                                                       #
# ------------------------------------------------------------------ #
class EmuResult:
    """An emulator run's outcome; faults are data, not a process crash."""

    def __init__(self, buf, ran):
        self._raw = bytes(buf)
        self.ran = ran

    @property
    def ok(self):
        return bool(self._raw[load().offset("emu_result_t", "ok")])

    @property
    def faulted(self):
        return bool(self._raw[load().offset("emu_result_t", "faulted")])

    @property
    def fault_addr(self):
        off = load().offset("emu_result_t", "fault_addr")
        return struct.unpack_from("<Q", self._raw, off)[0]

    @property
    def fault_kind(self):
        off = load().offset("emu_result_t", "fault_kind")
        return struct.unpack_from("<i", self._raw, off)[0]

    def reg(self, name):
        """Read an x86-64 guest register from the result — the GP file plus
        ``rip`` / ``rflags`` (e.g. ``rax``, ``rip``, ``rflags``)."""
        base = load().offset("emu_result_t", "regs") + load().offset(
            "emu_x86_regs_t", name
        )
        return struct.unpack_from("<Q", self._raw, base)[0]

    def xmm_f64(self, index=0, lane=0):
        """Lane (0..1) of guest XMM register ``index`` as a double — the FP/vector
        side of the file (a scalar double return is ``xmm_f64(0, 0)``)."""
        base = load().offset("emu_result_t", "regs") + load().offset(
            "emu_x86_regs_t", "xmm"
        )
        return struct.unpack_from("<d", self._raw, base + index * 16 + lane * 8)[0]

    def xmm_f32(self, index=0, lane=0):
        """Lane (0..3) of guest XMM register ``index`` as a float32."""
        base = load().offset("emu_result_t", "regs") + load().offset(
            "emu_x86_regs_t", "xmm"
        )
        return struct.unpack_from("<f", self._raw, base + index * 16 + lane * 4)[0]


class Emulator:
    """A Unicorn x86-64 guest. Use as a context manager: ``with Emulator() as e``."""

    def __init__(self):
        c = load()
        if not c.has_emu:
            raise RuntimeError(
                "emulator tier unavailable: load libasmtest_emu "
                "(build it with `make shared-emu`)"
            )
        self._h = c.lib.emu_open()
        if not self._h:
            raise RuntimeError("emu_open failed")

    @staticmethod
    def _code(fn, code_len):
        """Resolve a routine reference to (keepalive, addr, length). `fn` may be a
        ctypes function / int address (a 64-byte code window is read) or a `bytes`
        object of raw machine code (run directly — the cross-arch guests and the
        raw-bytes corpus cases use this)."""
        if isinstance(fn, (bytes, bytearray)):
            b = bytes(fn)
            buf = C.create_string_buffer(b)  # len(b)+1 bytes, NUL-terminated
            return buf, C.cast(buf, C.c_void_p).value, len(b)
        return None, _addr(fn), code_len

    def call(self, fn, args=(), code_len=64, max_insns=0):
        """Copy `code_len` bytes of `fn` and run with integer args in regs."""
        c = load()
        keep, addr, clen = self._code(fn, code_len)
        out = C.create_string_buffer(c.size("emu_result_t"))
        args = list(args)
        ran = c.lib.emu_call(
            self._h, C.c_void_p(addr), C.c_size_t(clen),
            C.cast(_longs(args, max(len(args), 1)), C.c_void_p),
            C.c_int(len(args)), C.c_uint64(max_insns), C.cast(out, C.c_void_p),
        )
        return EmuResult(out, bool(ran))

    def call_fp(self, fn, iargs=(), fargs=(), code_len=64, max_insns=0):
        """Run `fn` marshalling up to 8 doubles into the FP arg registers
        (xmm0..7); the scalar double return is ``res.xmm_f64(0, 0)``."""
        c = load()
        keep, addr, clen = self._code(fn, code_len)
        out = C.create_string_buffer(c.size("emu_result_t"))
        iargs, fargs = list(iargs), list(fargs)
        fa = (C.c_double * 8)()
        for i, val in enumerate(fargs[:8]):
            fa[i] = float(val)
        ran = c.lib.emu_call_fp(
            self._h, C.c_void_p(addr), C.c_size_t(clen),
            C.cast(_longs(iargs, max(len(iargs), 1)), C.c_void_p), C.c_int(len(iargs)),
            C.cast(fa, C.c_void_p), C.c_int(len(fargs)),
            C.c_uint64(max_insns), C.cast(out, C.c_void_p),
        )
        return EmuResult(out, bool(ran))

    def call_vec(self, fn, iargs=(), vargs=(), code_len=64, max_insns=0):
        """Run `fn` marshalling up to 8 128-bit vectors (16-byte `bytes` each)
        into xmm0..7; a vector return is ``res.xmm_f32(0, lane)``."""
        c = load()
        keep, addr, clen = self._code(fn, code_len)
        out = C.create_string_buffer(c.size("emu_result_t"))
        iargs, vargs = list(iargs), list(vargs)
        vb = (C.c_ubyte * (8 * 16))()
        for i, vec in enumerate(vargs[:8]):
            raw = bytes(vec)
            if len(raw) != 16:
                raise ValueError("each vector arg must be exactly 16 bytes")
            vb[i * 16 : i * 16 + 16] = raw
        ran = c.lib.emu_call_vec(
            self._h, C.c_void_p(addr), C.c_size_t(clen),
            C.cast(_longs(iargs, max(len(iargs), 1)), C.c_void_p), C.c_int(len(iargs)),
            C.cast(vb, C.c_void_p), C.c_int(len(vargs)),
            C.c_uint64(max_insns), C.cast(out, C.c_void_p),
        )
        return EmuResult(out, bool(ran))

    def call_win64(self, fn, args=(), code_len=64, max_insns=0):
        """Run `fn` under the Microsoft x64 (Win64) convention — integer args in
        rcx, rdx, r8, r9 — so a Win64 routine can be tested on a System V host."""
        c = load()
        keep, addr, clen = self._code(fn, code_len)
        out = C.create_string_buffer(c.size("emu_result_t"))
        args = list(args)
        ran = c.lib.emu_call_win64(
            self._h, C.c_void_p(addr), C.c_size_t(clen),
            C.cast(_longs(args, max(len(args), 1)), C.c_void_p),
            C.c_int(len(args)), C.c_uint64(max_insns), C.cast(out, C.c_void_p),
        )
        return EmuResult(out, bool(ran))

    def call_traced(self, fn, args=(), trace=None, code_len=64, max_insns=0):
        """Like :meth:`call`, but record an execution trace / basic-block coverage
        into `trace` (a :class:`Trace`)."""
        c = load()
        keep, addr, clen = self._code(fn, code_len)
        out = C.create_string_buffer(c.size("emu_result_t"))
        args = list(args)
        th = trace.handle if trace is not None else None
        ran = c.lib.emu_call_traced(
            self._h, C.c_void_p(addr), C.c_size_t(clen),
            C.cast(_longs(args, max(len(args), 1)), C.c_void_p),
            C.c_int(len(args)), C.c_uint64(max_insns), C.cast(out, C.c_void_p), th,
        )
        return EmuResult(out, bool(ran))

    def call_asm(self, src, args=(), syntax=0, max_insns=0):
        """Assemble x86-64 `src` in `syntax` (0=Intel, 1=AT&T, 2=NASM, 3=MASM,
        4=GAS; see :class:`Syntax`) via Keystone and
        run it with the integer `args` (up to six), stopping after `max_insns`
        instructions (0 = run to ``ret``). Returns an :class:`EmuResult`; raises
        :class:`AsmtestError` carrying the Keystone diagnostic if it fails to
        assemble. Only when the loaded lib has the assembler (libasmtest_emu_asm).
        """
        c = load()
        if not c.has_asm:
            raise AsmtestError("in-line assembler not in this build")
        out = C.create_string_buffer(c.size("emu_result_t"))
        a = [0, 0, 0, 0, 0, 0]
        args = list(args)
        n = min(len(args), 6)
        for i in range(n):
            a[i] = int(args[i])
        ok = c.lib.asmtest_emu_call_asm6(
            self._h, src.encode(), int(syntax),
            a[0], a[1], a[2], a[3], a[4], a[5], n, int(max_insns),
            C.cast(out, C.c_void_p),
        )
        if not ok:
            raise AsmtestError("in-line assembly failed: " + asm_error())
        return EmuResult(out, True)

    def close(self):
        if self._h:
            load().lib.emu_close(self._h)
            self._h = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


# ------------------------------------------------------------------ #
# Execution trace / coverage                                         #
# ------------------------------------------------------------------ #
class Trace:
    """An opaque execution-trace / basic-block coverage recorder. Pass it to
    :meth:`Emulator.call_traced` (or :meth:`GuestEmulator.call_traced`) to record,
    then ask which block byte-offsets were entered. Use as a context manager."""

    def __init__(self, insns_cap=4096, blocks_cap=4096):
        self._h = load().lib.asmtest_emu_trace_new(insns_cap, blocks_cap)
        if not self._h:
            raise RuntimeError("asmtest_emu_trace_new failed")

    @property
    def handle(self):
        return self._h

    def covered(self, off):
        """True if the basic block at byte-offset `off` (from the routine entry)
        was entered."""
        return bool(load().lib.asmtest_emu_trace_covered(self._h, C.c_uint64(off)))

    @property
    def insns_total(self):
        return load().lib.asmtest_emu_trace_insns_total(self._h)

    @property
    def blocks_len(self):
        return load().lib.asmtest_emu_trace_blocks_len(self._h)

    def free(self):
        if self._h:
            load().lib.asmtest_emu_trace_free(self._h)
            self._h = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.free()


# ------------------------------------------------------------------ #
# Cross-arch emulator guests (AArch64 / RISC-V / ARM32)              #
# ------------------------------------------------------------------ #
# These guests run *raw machine-code bytes* (there is no host routine to copy),
# so they emulate regardless of the host architecture. Results are read by
# register name through the opaque per-arch accessors.
_GUEST_VEC_F64 = {
    "arm64": "asmtest_emu_arm64_vec_f64",
    "arm": "asmtest_emu_arm_q_f64",
    "riscv": "asmtest_emu_riscv_f_f64",
}


class GuestResult:
    """A cross-arch guest run's outcome. Faults are data; registers are read by
    name (``"x0"``/``"sp"`` for arm64, ``"a0"``/``"x10"`` for riscv, ``"r0"`` for
    arm)."""

    def __init__(self, handle, arch):
        self._h = handle
        self.arch = arch

    @property
    def faulted(self):
        return bool(load().lib.asmtest_emu_result_faulted(self._h))

    @property
    def fault_addr(self):
        return load().lib.asmtest_emu_result_fault_addr(self._h)

    @property
    def fault_kind(self):
        return load().lib.asmtest_emu_result_fault_kind(self._h)

    def reg(self, name):
        fn = getattr(load().lib, f"asmtest_emu_{self.arch}_reg")
        return fn(self._h, name.encode())

    def vec_f64(self, index=0, lane=0):
        fn = getattr(load().lib, _GUEST_VEC_F64[self.arch])
        return fn(self._h, C.c_int(index), C.c_int(lane))

    def free(self):
        if self._h:
            getattr(load().lib, f"asmtest_emu_{self.arch}_result_free")(self._h)
            self._h = None


class GuestEmulator:
    """A cross-arch Unicorn guest — ``"arm64"``, ``"riscv"``, or ``"arm"`` — that
    runs raw machine-code bytes on any host. Use as a context manager."""

    _ARCHS = ("arm64", "riscv", "arm")

    def __init__(self, arch):
        if arch not in self._ARCHS:
            raise ValueError(f"unknown guest arch: {arch!r} (use {self._ARCHS})")
        c = load()
        if not c.has_emu:
            raise RuntimeError(
                "emulator tier unavailable: load libasmtest_emu "
                "(build it with `make shared-emu`)"
            )
        self.arch = arch
        self._open = getattr(c.lib, f"emu_{arch}_open")
        self._h = self._open()
        if not self._h:
            raise RuntimeError(f"emu_{arch}_open failed")

    def _new_result(self):
        return getattr(load().lib, f"asmtest_emu_{self.arch}_result_new")()

    def call(self, code, args=(), max_insns=0):
        """Run raw machine-code `code` (bytes) with integer args in the ABI arg
        registers (x0..x5 / a0..a7 / r0..r3). Returns a :class:`GuestResult`."""
        c = load()
        b = bytes(code)
        buf = C.create_string_buffer(b)
        res = self._new_result()
        args = list(args)
        getattr(c.lib, f"emu_{self.arch}_call")(
            self._h, C.cast(buf, C.c_void_p), C.c_size_t(len(b)),
            C.cast(_longs(args, max(len(args), 1)), C.c_void_p),
            C.c_int(len(args)), C.c_uint64(max_insns), res,
        )
        return GuestResult(res, self.arch)

    def call_fp(self, code, iargs=(), fargs=(), max_insns=0):
        """Run `code` marshalling doubles into the guest FP arg registers; the
        scalar double return is ``res.vec_f64(0, 0)``."""
        c = load()
        b = bytes(code)
        buf = C.create_string_buffer(b)
        res = self._new_result()
        iargs, fargs = list(iargs), list(fargs)
        fa = (C.c_double * 8)()
        for i, val in enumerate(fargs[:8]):
            fa[i] = float(val)
        getattr(c.lib, f"emu_{self.arch}_call_fp")(
            self._h, C.cast(buf, C.c_void_p), C.c_size_t(len(b)),
            C.cast(_longs(iargs, max(len(iargs), 1)), C.c_void_p), C.c_int(len(iargs)),
            C.cast(fa, C.c_void_p), C.c_int(len(fargs)), C.c_uint64(max_insns), res,
        )
        return GuestResult(res, self.arch)

    def call_vec(self, code, iargs=(), vargs=(), max_insns=0):
        """Run `code` marshalling 128-bit vectors into the guest vector arg
        registers (arm64 / arm only; RISC-V has no Unicorn vector file)."""
        if self.arch == "riscv":
            raise RuntimeError("the RISC-V guest has no vector register file")
        c = load()
        b = bytes(code)
        buf = C.create_string_buffer(b)
        res = self._new_result()
        iargs, vargs = list(iargs), list(vargs)
        vb = (C.c_ubyte * (8 * 16))()
        for i, vec in enumerate(vargs[:8]):
            raw = bytes(vec)
            if len(raw) != 16:
                raise ValueError("each vector arg must be exactly 16 bytes")
            vb[i * 16 : i * 16 + 16] = raw
        getattr(c.lib, f"emu_{self.arch}_call_vec")(
            self._h, C.cast(buf, C.c_void_p), C.c_size_t(len(b)),
            C.cast(_longs(iargs, max(len(iargs), 1)), C.c_void_p), C.c_int(len(iargs)),
            C.cast(vb, C.c_void_p), C.c_int(len(vargs)), C.c_uint64(max_insns), res,
        )
        return GuestResult(res, self.arch)

    def call_traced(self, code, args=(), trace=None, max_insns=0):
        """Like :meth:`call`, but record an execution trace / coverage into
        `trace` (a :class:`Trace`)."""
        c = load()
        b = bytes(code)
        buf = C.create_string_buffer(b)
        res = self._new_result()
        args = list(args)
        th = trace.handle if trace is not None else None
        getattr(c.lib, f"emu_{self.arch}_call_traced")(
            self._h, C.cast(buf, C.c_void_p), C.c_size_t(len(b)),
            C.cast(_longs(args, max(len(args), 1)), C.c_void_p),
            C.c_int(len(args)), C.c_uint64(max_insns), res, th,
        )
        return GuestResult(res, self.arch)

    def close(self):
        if self._h:
            getattr(load().lib, f"emu_{self.arch}_close")(self._h)
            self._h = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


# ------------------------------------------------------------------ #
# In-line assembler (Keystone) — optional                            #
# ------------------------------------------------------------------ #
class AsmtestError(RuntimeError):
    """Raised when an in-line assembly string fails to assemble."""


# Architecture / syntax codes for :func:`assemble` (mirror asm_arch_t / asm_syntax_t).
class Arch:
    X86_64, ARM64, RISCV64, ARM32 = 0, 1, 2, 3


class Syntax:
    INTEL, ATT, NASM, MASM, GAS = 0, 1, 2, 3, 4


def asm_available():
    """Whether the loaded native lib carries the in-line assembler (Keystone)."""
    return load().has_asm


def asm_error():
    """The Keystone diagnostic from the most recent assemble (thread-local; "" on success)."""
    c = load()
    if not c.has_asm:
        return ""
    p = c.lib.asmtest_asm_last_error()
    return p.decode() if p else ""


def assemble(src, arch=Arch.X86_64, syntax=Syntax.INTEL, addr=0x00100000):
    """Assemble `src` for `arch`/`syntax` at load address `addr`, returning the
    machine-code bytes. Multi-arch (unlike :meth:`Emulator.call_asm`, which runs
    on the x86-64 guest). Raises :class:`AsmtestError` on a Keystone error.
    """
    c = load()
    if not c.has_asm:
        raise AsmtestError("in-line assembler not in this build")
    cap = 256
    buf = C.create_string_buffer(cap)
    n = c.lib.asmtest_asm_bytes(
        int(arch), int(syntax), src.encode(), int(addr), C.cast(buf, C.c_void_p), cap
    )
    if n == 0:
        raise AsmtestError("assemble failed: " + asm_error())
    if n > cap:
        buf = C.create_string_buffer(n)
        n = c.lib.asmtest_asm_bytes(
            int(arch), int(syntax), src.encode(), int(addr), C.cast(buf, C.c_void_p), n
        )
    return buf.raw[:n]
