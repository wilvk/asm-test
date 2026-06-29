"""Hardware-tier native runtime tracing for Python (single-step / Intel PT / AMD).

This is the language-wrapper surface for the optional hardware-trace tier (see
``include/asmtest_hwtrace.h`` and ``docs/native-tracing.md``). It records the same
``asmtest_trace_t`` offsets as the emulator and DynamoRIO tiers, but by observing
the **real CPU** — and unlike the DynamoRIO wrapper it needs no DynamoRIO install.

Four backends share one API, selected by enum:

* ``SINGLESTEP`` — EFLAGS.TF single-step (#DB -> SIGTRAP). Exact and complete on
  **any x86-64 Linux** (Intel, any-Zen AMD, VM, CI, container): no PMU, no
  perf_event, no privilege. This is the portable default and the one that runs
  everywhere, so it is what every binding's self-test exercises live.
* ``INTEL_PT`` / ``CORESIGHT`` / ``AMD_LBR`` — hardware branch-trace backends that
  self-skip off the specific bare-metal hardware they need.

``HwTrace.available(backend)`` reports whether the chosen backend can run so
callers self-skip cleanly. The module loads ``libasmtest_hwtrace`` (resolved from
``$ASMTEST_HWTRACE_LIB``, else the repo ``build/``); nothing here links a decoder.

Example::

    from asmtest.hwtrace import HwTrace, NativeCode, SINGLESTEP

    if HwTrace.available(SINGLESTEP):
        HwTrace.init(SINGLESTEP)
        code = NativeCode.from_bytes(b"\\x48\\x89\\xf8\\x48\\x01\\xf0\\xc3")  # mov rax,rdi; add rax,rsi; ret
        trace = HwTrace.new(blocks=64, instructions=64)
        trace.register("add", code)
        with trace.region("add"):
            result = code.call(20, 22)
        assert result == 42 and trace.covered(0)
        HwTrace.shutdown()
"""
import ctypes as C
import os
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[3]

ASMTEST_HW_OK = 0

# asmtest_trace_backend_t
INTEL_PT = 0
CORESIGHT = 1
AMD_LBR = 2
SINGLESTEP = 3


class _Options(C.Structure):
    """Mirrors asmtest_hwtrace_options_t."""

    _fields_ = [
        ("backend", C.c_int),
        ("aux_size", C.c_size_t),
        ("data_size", C.c_size_t),
        ("snapshot", C.c_int),
        ("object_hint", C.c_char_p),
    ]


def _lib_name():
    return "libasmtest_hwtrace.dylib" if sys.platform == "darwin" else "libasmtest_hwtrace.so"


def _load():
    cands = []
    env = os.environ.get("ASMTEST_HWTRACE_LIB")
    if env:
        cands.append(Path(env))
    cands += [_REPO_ROOT / "build" / _lib_name(), Path(_lib_name())]
    errors = []
    for c in cands:
        try:
            return C.CDLL(str(c))
        except OSError as e:  # noqa: PERF203
            errors.append(f"  {c}: {e}")
    raise OSError(
        "libasmtest_hwtrace not found; build it with `make shared-hwtrace` "
        "or set ASMTEST_HWTRACE_LIB.\n" + "\n".join(errors)
    )


def _declare(lib):
    v, sz, u64, cc, ci = (C.c_void_p, C.c_size_t, C.c_uint64, C.c_char_p, C.c_int)
    lib.asmtest_hwtrace_available.argtypes = [ci]
    lib.asmtest_hwtrace_available.restype = ci
    lib.asmtest_hwtrace_skip_reason.argtypes = [ci, cc, sz]
    lib.asmtest_hwtrace_init.argtypes = [C.POINTER(_Options)]
    lib.asmtest_hwtrace_init.restype = ci
    lib.asmtest_hwtrace_register_region.argtypes = [cc, v, sz, v]
    lib.asmtest_hwtrace_register_region.restype = ci
    lib.asmtest_hwtrace_begin.argtypes = [cc]
    lib.asmtest_hwtrace_end.argtypes = [cc]
    lib.asmtest_hwtrace_shutdown.restype = None
    lib.asmtest_hwtrace_exec_alloc.argtypes = [v, sz, C.POINTER(v), C.POINTER(sz)]
    lib.asmtest_hwtrace_exec_alloc.restype = ci
    lib.asmtest_hwtrace_exec_free.argtypes = [v, sz]
    # Trace handle + accessors (from the shared trace.o, also in this lib).
    lib.asmtest_trace_new.argtypes = [sz, sz]
    lib.asmtest_trace_new.restype = v
    lib.asmtest_trace_free.argtypes = [v]
    lib.asmtest_trace_covered.argtypes = [v, u64]
    lib.asmtest_trace_covered.restype = ci
    lib.asmtest_emu_trace_blocks_len.argtypes = [v]
    lib.asmtest_emu_trace_blocks_len.restype = u64
    lib.asmtest_emu_trace_insns_total.argtypes = [v]
    lib.asmtest_emu_trace_insns_total.restype = u64
    lib.asmtest_emu_trace_insns_len.argtypes = [v]
    lib.asmtest_emu_trace_insns_len.restype = u64
    lib.asmtest_emu_trace_truncated.argtypes = [v]
    lib.asmtest_emu_trace_truncated.restype = ci
    lib.asmtest_emu_trace_block_at.argtypes = [v, sz]
    lib.asmtest_emu_trace_block_at.restype = u64
    lib.asmtest_emu_trace_insn_at.argtypes = [v, sz]
    lib.asmtest_emu_trace_insn_at.restype = u64
    return lib


_lib = None


def _get():
    global _lib
    if _lib is None:
        _lib = _declare(_load())
    return _lib


class NativeCode:
    """Host-native machine code in real executable (W^X) memory."""

    def __init__(self, base, length):
        self._base = base
        self._len = length
        self._freed = False

    @classmethod
    def from_bytes(cls, code: bytes) -> "NativeCode":
        lib = _get()
        base = C.c_void_p()
        length = C.c_size_t()
        buf = (C.c_ubyte * len(code)).from_buffer_copy(code)
        rc = lib.asmtest_hwtrace_exec_alloc(C.cast(buf, C.c_void_p), len(code),
                                            C.byref(base), C.byref(length))
        if rc != ASMTEST_HW_OK:
            raise RuntimeError(f"asmtest_hwtrace_exec_alloc failed: {rc}")
        return cls(base.value, length.value)

    @property
    def base(self) -> int:
        return self._base

    @property
    def length(self) -> int:
        return self._len

    def call(self, *args, restype=C.c_long, argtypes=None) -> int:
        """Invoke the code through a function pointer. By default each argument is
        passed as a C long and the result read as a long (the SysV integer ABI)."""
        if argtypes is None:
            argtypes = [C.c_long] * len(args)
        proto = C.CFUNCTYPE(restype, *argtypes)
        fn = proto(self._base)
        return fn(*args)

    def free(self):
        if not self._freed:
            _get().asmtest_hwtrace_exec_free(self._base, self._len)
            self._freed = True


class _Region:
    def __init__(self, lib, name):
        self._lib = lib
        self._name = name.encode()

    def __enter__(self):
        self._lib.asmtest_hwtrace_begin(self._name)
        return self

    def __exit__(self, *exc):
        self._lib.asmtest_hwtrace_end(self._name)
        return False


class HwTrace:
    """A coverage recorder for a registered native region, via the hardware tier."""

    def __init__(self, handle):
        self._lib = _get()
        self._handle = handle

    # ---- process-wide lifecycle ----
    @staticmethod
    def available(backend=SINGLESTEP) -> bool:
        """True if the chosen backend can run on this host (self-skip otherwise)."""
        return bool(_get().asmtest_hwtrace_available(backend))

    @staticmethod
    def skip_reason(backend=SINGLESTEP) -> str:
        """Human-readable reason available() is false (or 'available')."""
        buf = C.create_string_buffer(160)
        _get().asmtest_hwtrace_skip_reason(backend, buf, len(buf))
        return buf.value.decode()

    @classmethod
    def init(cls, backend=SINGLESTEP):
        """Select a backend and initialize the tier. SINGLESTEP is the portable
        default that runs on any x86-64 Linux."""
        lib = _get()
        opts = _Options()
        opts.backend = backend
        rc = lib.asmtest_hwtrace_init(C.byref(opts))
        if rc != ASMTEST_HW_OK:
            raise RuntimeError(f"asmtest_hwtrace_init failed: {rc}")

    @staticmethod
    def shutdown():
        _get().asmtest_hwtrace_shutdown()

    # ---- per-trace ----
    @classmethod
    def new(cls, blocks=64, instructions=0) -> "HwTrace":
        handle = _get().asmtest_trace_new(instructions, blocks)
        if not handle:
            raise RuntimeError("asmtest_trace_new returned NULL")
        return cls(handle)

    def register(self, name: str, code: NativeCode):
        rc = self._lib.asmtest_hwtrace_register_region(
            name.encode(), code.base, code.length, self._handle)
        if rc != ASMTEST_HW_OK:
            raise RuntimeError(f"register_region failed: {rc}")

    def region(self, name: str) -> _Region:
        """Context manager bracketing begin(name)/end(name) around a traced call."""
        return _Region(self._lib, name)

    def covered(self, off: int) -> bool:
        return bool(self._lib.asmtest_trace_covered(self._handle, off))

    def blocks_len(self) -> int:
        return int(self._lib.asmtest_emu_trace_blocks_len(self._handle))

    def insns_total(self) -> int:
        return int(self._lib.asmtest_emu_trace_insns_total(self._handle))

    def insns_len(self) -> int:
        return int(self._lib.asmtest_emu_trace_insns_len(self._handle))

    def truncated(self) -> bool:
        return bool(self._lib.asmtest_emu_trace_truncated(self._handle))

    def block_at(self, i: int) -> int:
        return int(self._lib.asmtest_emu_trace_block_at(self._handle, i))

    def insn_at(self, i: int) -> int:
        return int(self._lib.asmtest_emu_trace_insn_at(self._handle, i))

    def block_offsets(self) -> list:
        return [self.block_at(i) for i in range(self.blocks_len())]

    def insn_offsets(self) -> list:
        return [self.insn_at(i) for i in range(self.insns_len())]

    def free(self):
        if self._handle:
            self._lib.asmtest_trace_free(self._handle)
            self._handle = None
