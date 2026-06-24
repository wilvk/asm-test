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
        """Read an x86-64 guest register (rax, rbx, …) from the result."""
        base = load().offset("emu_result_t", "regs") + load().offset(
            "emu_x86_regs_t", name
        )
        return struct.unpack_from("<Q", self._raw, base)[0]


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

    def call(self, fn, args=(), code_len=64, max_insns=0):
        """Copy `code_len` bytes of `fn` and run with integer args in regs."""
        c = load()
        out = C.create_string_buffer(c.size("emu_result_t"))
        args = list(args)
        ran = c.lib.emu_call(
            self._h, C.c_void_p(_addr(fn)), C.c_size_t(code_len),
            C.cast(_longs(args, max(len(args), 1)), C.c_void_p),
            C.c_int(len(args)), C.c_uint64(max_insns), C.cast(out, C.c_void_p),
        )
        return EmuResult(out, bool(ran))

    def close(self):
        if self._h:
            load().lib.emu_close(self._h)
            self._h = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
