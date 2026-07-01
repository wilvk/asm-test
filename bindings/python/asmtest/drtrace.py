"""In-process native runtime tracing for Python, backed by DynamoRIO.

This is the language-wrapper surface for the optional DynamoRIO native-trace tier
(see ``include/asmtest_drtrace.h`` and ``docs/native-tracing.md``). Where the
emulator tier (``asmtest.Trace``) traces isolated guest bytes, ``NativeTrace``
traces host-native code as it runs **inside this Python process**: initialize
DynamoRIO once at startup, materialize host-native machine code, mark a region,
call into it, and read back basic-block coverage / the instruction stream.

The module is distinct from the private loader ``asmtest._native`` (hence the name
``drtrace``, not ``native``). It loads ``libasmtest_drapp`` and drives the C API:
``libdynamorio`` is dlopen()ed lazily by the C side after the client is
configured, so nothing here links DynamoRIO.

Advanced, Linux-x86-64-only, and opt-in. ``NativeTrace.available()`` reports
whether the tier can run so callers self-skip cleanly.

Example::

    from asmtest.drtrace import NativeTrace, NativeCode

    NativeTrace.initialize(client="./build/libasmtest_drclient.so",
                           dynamorio_home="/opt/DynamoRIO")
    code = NativeCode.from_bytes(b"\\x48\\x89\\xf8\\x48\\x01\\xf0\\xc3")  # mov rax,rdi; add rax,rsi; ret
    trace = NativeTrace.new(blocks=64, instructions=0)
    trace.register("add", code)
    with trace.region("add"):
        result = code.call(20, 22)
    assert result == 42 and trace.covered(0)
    NativeTrace.shutdown()
"""
import ctypes as C
import os
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[3]
# Native libs bundled into the wheel by `make python-package` (mirrors asmtest._native):
# libasmtest_drapp + its DR client + the vendored libdynamorio all land here.
_LIBS = Path(__file__).resolve().parent / "_libs"

ASMTEST_DR_OK = 0


class _Options(C.Structure):
    """Mirrors asmtest_drtrace_options_t."""

    _fields_ = [
        ("dynamorio_home", C.c_char_p),
        ("client_path", C.c_char_p),
        ("client_options", C.c_char_p),
        ("mode", C.c_int),
    ]


class _ExecCode(C.Structure):
    """Mirrors asmtest_exec_code_t."""

    _fields_ = [("base", C.c_void_p), ("len", C.c_size_t)]


def _lib_name():
    return "libasmtest_drapp.dylib" if sys.platform == "darwin" else "libasmtest_drapp.so"


_resolved_path = None


def _load():
    global _resolved_path
    cands = []
    env = os.environ.get("ASMTEST_DRAPP_LIB")
    if env:
        cands.append(Path(env))
    # Bundled (published wheel) is tried BEFORE the dev build/ tree, so an installed
    # package never prefers a leaked checkout; system search is the last resort.
    cands += [_LIBS / _lib_name(), _REPO_ROOT / "build" / _lib_name(), Path(_lib_name())]
    errors = []
    for c in cands:
        try:
            lib = C.CDLL(str(c))
            _resolved_path = str(c)
            return lib
        except OSError as e:  # noqa: PERF203
            errors.append(f"  {c}: {e}")
    raise OSError(
        "libasmtest_drapp not found; build it with `make shared-drtrace "
        "DYNAMORIO_HOME=...` or set ASMTEST_DRAPP_LIB.\n" + "\n".join(errors)
    )


def _client_name():
    return "libasmtest_drclient.dylib" if sys.platform == "darwin" else "libasmtest_drclient.so"


def _default_client():
    """Bundled DR client shipped alongside libasmtest_drapp, if present (else None
    → the C side falls back to $ASMTEST_DRCLIENT / the repo build tree)."""
    env = os.environ.get("ASMTEST_DRCLIENT")
    if env:
        return env
    for c in (_LIBS / _client_name(), _REPO_ROOT / "build" / _client_name()):
        if c.is_file():
            return str(c)
    return None


def library_path():
    """Absolute path of the libasmtest_drapp this process resolved (loading it if
    needed). Lets a clean-room test assert the bundled tier — not a leaked build/
    tree — satisfied the load."""
    _get()
    return _resolved_path


def _declare(lib):
    v, sz, u64, cc, ci = (C.c_void_p, C.c_size_t, C.c_uint64, C.c_char_p, C.c_int)
    lib.asmtest_dr_available.restype = ci
    lib.asmtest_dr_init.argtypes = [C.POINTER(_Options)]
    lib.asmtest_dr_init.restype = ci
    for fn in ("asmtest_dr_start", "asmtest_dr_stop"):
        getattr(lib, fn).restype = ci
    lib.asmtest_dr_shutdown.restype = None
    lib.asmtest_dr_register_region.argtypes = [cc, v, sz, v]
    lib.asmtest_dr_register_region.restype = ci
    lib.asmtest_dr_unregister_region.argtypes = [cc]
    lib.asmtest_dr_unregister_region.restype = ci
    lib.asmtest_trace_begin.argtypes = [cc]
    lib.asmtest_trace_end.argtypes = [cc]
    lib.asmtest_dr_marker_error.restype = ci
    lib.asmtest_dr_under_dynamorio.restype = ci
    lib.asmtest_dr_register_symbol.argtypes = [cc, sz, v]
    lib.asmtest_dr_register_symbol.restype = ci
    lib.asmtest_symbol_demo.argtypes = [C.c_long, C.c_long]
    lib.asmtest_symbol_demo.restype = C.c_long
    lib.asmtest_exec_alloc.argtypes = [C.c_char_p, sz, C.POINTER(_ExecCode)]
    lib.asmtest_exec_alloc.restype = ci
    lib.asmtest_exec_free.argtypes = [C.POINTER(_ExecCode)]
    # Trace handle + accessors (from the shared trace.o).
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

    def __init__(self, exec_code):
        self._code = exec_code  # _ExecCode
        self._freed = False

    @classmethod
    def from_bytes(cls, code: bytes) -> "NativeCode":
        lib = _get()
        ec = _ExecCode()
        rc = lib.asmtest_exec_alloc(code, len(code), C.byref(ec))
        if rc != ASMTEST_DR_OK:
            raise RuntimeError(f"asmtest_exec_alloc failed: {rc}")
        return cls(ec)

    @property
    def base(self) -> int:
        return self._code.base

    @property
    def length(self) -> int:
        return self._code.len

    def call(self, *args, restype=C.c_long, argtypes=None) -> int:
        """Invoke the code through a function pointer. By default each argument is
        passed as a C long and the result read as a long (the SysV integer ABI)."""
        if argtypes is None:
            argtypes = [C.c_long] * len(args)
        proto = C.CFUNCTYPE(restype, *argtypes)
        fn = proto(self._code.base)
        return fn(*args)

    def free(self):
        if not self._freed:
            _get().asmtest_exec_free(C.byref(self._code))
            self._freed = True


class _Region:
    def __init__(self, lib, name):
        self._lib = lib
        self._name = name.encode()

    def __enter__(self):
        self._lib.asmtest_trace_begin(self._name)
        return self

    def __exit__(self, *exc):
        self._lib.asmtest_trace_end(self._name)
        return False


class NativeTrace:
    """An app-owned coverage recorder for a registered native region."""

    _started = False

    def __init__(self, handle):
        self._lib = _get()
        self._handle = handle

    # ---- process-wide lifecycle ----
    @staticmethod
    def available() -> bool:
        """True if the tier can run (built + libdynamorio resolvable)."""
        return bool(_get().asmtest_dr_available())

    @classmethod
    def initialize(cls, client=None, dynamorio_home=None, client_options=None,
                   mode=0, start=True):
        """Bring DynamoRIO up in-process. ``client`` is the path to
        libasmtest_drclient.so (else $ASMTEST_DRCLIENT); ``dynamorio_home`` lets
        the C side find libdynamorio (else $ASMTEST_DR_LIB / rpath). With
        ``start=False`` DR is configured but not taken over yet — use start()/stop()
        to bracket only the calls into traced code (the managed-runtime model)."""
        lib = _get()
        if client is None:
            client = _default_client()  # prefer the bundled DR client in a wheel
        opts = _Options()
        opts.client_path = (client or "").encode() or None
        opts.dynamorio_home = (dynamorio_home or "").encode() or None
        opts.client_options = (client_options or "").encode() or None
        opts.mode = mode
        rc = lib.asmtest_dr_init(C.byref(opts))
        if rc != ASMTEST_DR_OK:
            raise RuntimeError(f"asmtest_dr_init failed: {rc}")
        if start:
            cls.start()

    @classmethod
    def start(cls):
        """Take DynamoRIO over (dr_app_start). Must run on the init thread."""
        rc = _get().asmtest_dr_start()
        if rc != ASMTEST_DR_OK:
            raise RuntimeError(f"asmtest_dr_start failed: {rc}")
        cls._started = True

    @classmethod
    def stop(cls):
        """Return to native execution (dr_app_stop) without tearing DR down."""
        rc = _get().asmtest_dr_stop()
        if rc != ASMTEST_DR_OK:
            raise RuntimeError(f"asmtest_dr_stop failed: {rc}")
        cls._started = False

    @classmethod
    def under_dynamorio(cls) -> bool:
        """True if the calling thread is currently executing under DynamoRIO."""
        return bool(_get().asmtest_dr_under_dynamorio())

    @classmethod
    def shutdown(cls):
        _get().asmtest_dr_shutdown()
        cls._started = False

    @classmethod
    def marker_error(cls) -> int:
        return int(_get().asmtest_dr_marker_error())

    @staticmethod
    def symbol_demo(a, b) -> int:
        """Call the exported asmtest_symbol_demo fixture (a*2+b); the symbol-mode
        test traces it by name."""
        return int(_get().asmtest_symbol_demo(a, b))

    # ---- per-trace ----
    @classmethod
    def new(cls, blocks=64, instructions=0) -> "NativeTrace":
        h = _get().asmtest_trace_new(instructions, blocks)
        if not h:
            raise MemoryError("asmtest_trace_new failed")
        return cls(h)

    def register(self, name: str, code: NativeCode):
        rc = self._lib.asmtest_dr_register_region(
            name.encode(), code.base, code.length, self._handle)
        if rc != ASMTEST_DR_OK:
            raise RuntimeError(f"register_region({name!r}) failed: {rc}")
        return self

    def register_symbol(self, symbol: str, max_len: int = 256):
        """Symbol mode: trace a named exported function by name, with no
        begin/end markers. Recording is always-on over the resolved range
        [entry, entry+max_len) once DR is started — just call the function."""
        rc = self._lib.asmtest_dr_register_symbol(
            symbol.encode(), max_len, self._handle)
        if rc != ASMTEST_DR_OK:
            raise RuntimeError(f"register_symbol({symbol!r}) failed: {rc}")
        return self

    def unregister(self, name: str):
        self._lib.asmtest_dr_unregister_region(name.encode())

    def region(self, name: str) -> _Region:
        return _Region(self._lib, name)

    def covered(self, off: int) -> bool:
        return bool(self._lib.asmtest_trace_covered(self._handle, off))

    @property
    def blocks_len(self) -> int:
        return int(self._lib.asmtest_emu_trace_blocks_len(self._handle))

    @property
    def insns_total(self) -> int:
        return int(self._lib.asmtest_emu_trace_insns_total(self._handle))

    def block_offsets(self) -> list:
        """The distinct basic-block start offsets recorded, in first-seen order."""
        n = self.blocks_len
        at = self._lib.asmtest_emu_trace_block_at
        return [int(at(self._handle, i)) for i in range(n)]

    def insn_offsets(self) -> list:
        """The ordered instruction-offset stream actually stored — each executed
        instruction's offset in execution order, up to the trace's insns capacity
        (insns_len, not the possibly-larger insns_total)."""
        n = int(self._lib.asmtest_emu_trace_insns_len(self._handle))
        at = self._lib.asmtest_emu_trace_insn_at
        return [int(at(self._handle, i)) for i in range(n)]

    def free(self):
        if self._handle:
            self._lib.asmtest_trace_free(self._handle)
            self._handle = None
