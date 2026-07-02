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
callers self-skip cleanly. ``HwTrace.auto(policy)`` / ``HwTrace.resolve(policy)``
pick the most faithful available backend for the host without hard-coding an enum
(the hardware-tier fallback cascade — see ``docs/native-tracing.md``). The module
loads ``libasmtest_hwtrace`` (resolved from ``$ASMTEST_HWTRACE_LIB``, else the repo
``build/``); nothing here links a decoder.

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
# Native lib bundled into the wheel by `make python-package` (mirrors asmtest._native).
_LIBS = Path(__file__).resolve().parent / "_libs"

ASMTEST_HW_OK = 0
ASMTEST_HW_EUNAVAIL = -3  # no hardware-trace backend available on this host

# asmtest_trace_backend_t
INTEL_PT = 0
CORESIGHT = 1
AMD_LBR = 2
SINGLESTEP = 3

# asmtest_hwtrace_policy_t — backend auto-selection policy
BEST = 0          # the most faithful available backend
CEILING_FREE = 1  # the same, but skipping the one fixed-window backend (AMD LBR);
                  # re-resolve under this after a trace comes back truncated

# asmtest_trace_auto.h — the CROSS-TIER orchestrator over all three trace tiers
# (hardware + DynamoRIO + emulator), not just the hardware backends above.
# asmtest_trace_tier_t
TIER_HWTRACE = 0    # HW branch trace / single-step (real CPU)
TIER_DYNAMORIO = 1  # in-process software DBI (real CPU)
TIER_EMULATOR = 2   # Unicorn virtual CPU (isolated guest)
# asmtest_trace_fidelity_t
FIDELITY_NATIVE = 0   # runs the real bytes on the real CPU in-process
FIDELITY_VIRTUAL = 1  # isolated guest on an emulated CPU
# cross-tier policy bitmask
TRACE_BEST = 0x0          # most-faithful available; emulator floor allowed
TRACE_CEILING_FREE = 0x1  # drop the fixed-window backend (AMD LBR)
TRACE_NATIVE_ONLY = 0x2   # forbid the native->emulator fidelity crossing

# asmtest_ptrace.h — out-of-process / foreign-process tracing status codes
ASMTEST_PTRACE_OK = 0
ASMTEST_PTRACE_ENOENT = -7  # region / symbol / method not found

# asmtest_codeimage.h — time-aware code-image recorder status codes
ASMTEST_CI_OK = 0
ASMTEST_CI_ENOENT = -7  # address never tracked / no version at/before `when`

# Code-emission event kinds (eBPF detector)
ASMTEST_CI_KIND_MPROTECT = 1
ASMTEST_CI_KIND_MMAP = 2
ASMTEST_CI_KIND_MEMFD = 3


class _Options(C.Structure):
    """Mirrors asmtest_hwtrace_options_t."""

    _fields_ = [
        ("backend", C.c_int),
        ("aux_size", C.c_size_t),
        ("data_size", C.c_size_t),
        ("snapshot", C.c_int),
        ("object_hint", C.c_char_p),
    ]


class _Choice(C.Structure):
    """Mirrors asmtest_trace_choice_t (three int-sized enum fields, no padding)."""

    _fields_ = [
        ("tier", C.c_int),
        ("backend", C.c_int),
        ("fidelity", C.c_int),
    ]


class TierChoice(C.Structure):
    """A resolved cross-tier trace option: which ``tier`` to use, which hardware
    ``backend`` within it (meaningful only when ``tier == TIER_HWTRACE``), and the
    ``fidelity`` class (``FIDELITY_NATIVE`` vs ``FIDELITY_VIRTUAL``)."""

    _fields_ = _Choice._fields_

    def __repr__(self):
        return (f"TierChoice(tier={self.tier}, backend={self.backend}, "
                f"fidelity={self.fidelity})")


class _JitEntry(C.Structure):
    """Mirrors asmtest_jitdump_entry_t (four u64 fields)."""

    _fields_ = [
        ("code_addr", C.c_uint64),
        ("code_size", C.c_uint64),
        ("timestamp", C.c_uint64),
        ("code_index", C.c_uint64),
    ]


class JitMethod:
    """A JIT method resolved from a jitdump: load address, size, the JIT's
    timestamp/index, and (optionally) the recorded native code bytes."""

    def __init__(self, code_addr, code_size, timestamp, code_index, code=b""):
        self.code_addr = code_addr
        self.code_size = code_size
        self.timestamp = timestamp
        self.code_index = code_index
        self.code = code

    def __repr__(self):
        return (f"JitMethod(code_addr={self.code_addr:#x}, "
                f"code_size={self.code_size}, timestamp={self.timestamp}, "
                f"code_index={self.code_index}, code={len(self.code)} bytes)")


class _CodeimageEvent(C.Structure):
    """Mirrors asmtest_codeimage_event_t / bpf/codeimage_event.h (40 bytes)."""

    _fields_ = [
        ("addr", C.c_uint64),
        ("len", C.c_uint64),
        ("timestamp", C.c_uint64),
        ("pid", C.c_uint32),
        ("tid", C.c_uint32),
        ("kind", C.c_uint32),
        ("fd", C.c_int32),
    ]


class CodeEmission:
    """A code-emission event from the eBPF detector: a published executable region
    (addr/len), its kind (ASMTEST_CI_KIND_*), the publishing pid/tid, a kernel
    timestamp, and the memfd fd (or -1)."""

    def __init__(self, addr, length, timestamp, pid, tid, kind, fd):
        self.addr = addr
        self.len = length
        self.timestamp = timestamp
        self.pid = pid
        self.tid = tid
        self.kind = kind
        self.fd = fd

    def __repr__(self):
        return (f"CodeEmission(addr={self.addr:#x}, len={self.len}, kind={self.kind}, "
                f"pid={self.pid}, tid={self.tid}, fd={self.fd})")


def _lib_name():
    return "libasmtest_hwtrace.dylib" if sys.platform == "darwin" else "libasmtest_hwtrace.so"


_resolved_path = None


def _load():
    global _resolved_path
    cands = []
    env = os.environ.get("ASMTEST_HWTRACE_LIB")
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
        "libasmtest_hwtrace not found; build it with `make shared-hwtrace` "
        "or set ASMTEST_HWTRACE_LIB.\n" + "\n".join(errors)
    )


def _declare(lib):
    v, sz, u64, cc, ci = (C.c_void_p, C.c_size_t, C.c_uint64, C.c_char_p, C.c_int)
    lib.asmtest_hwtrace_available.argtypes = [ci]
    lib.asmtest_hwtrace_available.restype = ci
    lib.asmtest_hwtrace_skip_reason.argtypes = [ci, cc, sz]
    lib.asmtest_hwtrace_resolve.argtypes = [ci, C.POINTER(ci), sz]
    lib.asmtest_hwtrace_resolve.restype = sz
    lib.asmtest_hwtrace_auto.argtypes = [ci]
    lib.asmtest_hwtrace_auto.restype = ci
    # Cross-tier orchestrator (asmtest_trace_auto.h).
    lib.asmtest_trace_resolve.argtypes = [C.c_uint, C.POINTER(_Choice), sz]
    lib.asmtest_trace_resolve.restype = sz
    lib.asmtest_trace_auto.argtypes = [C.c_uint, C.POINTER(_Choice)]
    lib.asmtest_trace_auto.restype = ci
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
    # asmtest_ptrace.h — out-of-process / foreign-process tracing toolkit.
    pl = C.POINTER(C.c_long)
    lib.asmtest_ptrace_available.restype = ci
    lib.asmtest_ptrace_skip_reason.argtypes = [cc, sz]
    lib.asmtest_ptrace_trace_call.argtypes = [v, sz, pl, ci, pl, v]
    lib.asmtest_ptrace_trace_call.restype = ci
    lib.asmtest_ptrace_trace_attached.argtypes = [ci, v, sz, pl, v]
    lib.asmtest_ptrace_trace_attached.restype = ci
    lib.asmtest_ptrace_run_to.argtypes = [ci, v]
    lib.asmtest_ptrace_run_to.restype = ci
    lib.asmtest_proc_region_by_addr.argtypes = [ci, v, C.POINTER(v), C.POINTER(sz)]
    lib.asmtest_proc_region_by_addr.restype = ci
    lib.asmtest_proc_perfmap_symbol.argtypes = [ci, cc, C.POINTER(v), C.POINTER(sz)]
    lib.asmtest_proc_perfmap_symbol.restype = ci
    lib.asmtest_jitdump_find.argtypes = [
        cc, ci, cc, C.POINTER(_JitEntry), C.POINTER(C.c_ubyte), sz, C.POINTER(sz)]
    lib.asmtest_jitdump_find.restype = ci
    lib.asmtest_ptrace_trace_attached_versioned.argtypes = [ci, v, sz, v, u64, pl, v]
    lib.asmtest_ptrace_trace_attached_versioned.restype = ci

    # asmtest_codeimage.h — time-aware code-image recorder + optional eBPF detector.
    lib.asmtest_codeimage_available.restype = ci
    lib.asmtest_codeimage_skip_reason.argtypes = [cc, sz]
    lib.asmtest_codeimage_new.argtypes = [ci]
    lib.asmtest_codeimage_new.restype = v
    lib.asmtest_codeimage_free.argtypes = [v]
    lib.asmtest_codeimage_track.argtypes = [v, v, sz]
    lib.asmtest_codeimage_track.restype = ci
    lib.asmtest_codeimage_refresh.argtypes = [v]
    lib.asmtest_codeimage_refresh.restype = ci
    lib.asmtest_codeimage_now.argtypes = [v]
    lib.asmtest_codeimage_now.restype = u64
    lib.asmtest_codeimage_bytes_at.argtypes = [
        v, v, u64, C.POINTER(C.POINTER(C.c_ubyte)), C.POINTER(sz)]
    lib.asmtest_codeimage_bytes_at.restype = ci
    lib.asmtest_codeimage_bpf_available.restype = ci
    lib.asmtest_codeimage_bpf_skip_reason.argtypes = [cc, sz]
    lib.asmtest_codeimage_watch_bpf.argtypes = [v]
    lib.asmtest_codeimage_watch_bpf.restype = ci
    lib.asmtest_codeimage_poll_bpf.argtypes = [v, ci]
    lib.asmtest_codeimage_poll_bpf.restype = ci
    lib.asmtest_codeimage_next.argtypes = [v, C.POINTER(_CodeimageEvent)]
    lib.asmtest_codeimage_next.restype = ci
    return lib


_lib = None


def _get():
    global _lib
    if _lib is None:
        _lib = _declare(_load())
    return _lib


def _try_get():
    """Like _get() but folds a missing/unloadable library into None so the
    documented available()/skip_reason() gates can self-skip cleanly (matching
    the Node/Ruby/Lua bindings) instead of raising OSError."""
    try:
        return _get()
    except OSError:
        return None


def library_path():
    """Absolute path of the libasmtest_hwtrace this process resolved (loading it if
    needed). Lets a clean-room test assert the bundled tier — not a leaked build/
    tree — satisfied the load."""
    _get()
    return _resolved_path


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
        lib = _try_get()
        if lib is None:
            return False
        return bool(lib.asmtest_hwtrace_available(backend))

    @staticmethod
    def skip_reason(backend=SINGLESTEP) -> str:
        """Human-readable reason available() is false (or 'available')."""
        lib = _try_get()
        if lib is None:
            return "libasmtest_hwtrace not found"
        buf = C.create_string_buffer(160)
        lib.asmtest_hwtrace_skip_reason(backend, buf, len(buf))
        return buf.value.decode()

    @staticmethod
    def resolve(policy=BEST) -> list:
        """This host's hardware-trace fallback cascade: the available backends,
        most-faithful first (INTEL_PT > AMD_LBR > SINGLESTEP > CORESIGHT), honoring
        ``policy``. Empty only off x86-64 Linux (single-step is the floor there).
        ``CEILING_FREE`` drops the depth-bounded backend (AMD LBR)."""
        out = (C.c_int * 4)()
        n = _get().asmtest_hwtrace_resolve(policy, out, len(out))
        return [out[i] for i in range(n)]

    @staticmethod
    def auto(policy=BEST) -> int:
        """The single most-preferred available backend under ``policy`` (a backend
        enum >= 0, ready to ``init``), or ``ASMTEST_HW_EUNAVAIL`` (< 0) when no
        hardware-trace backend is available on this host."""
        return int(_get().asmtest_hwtrace_auto(policy))

    @staticmethod
    def resolve_tiers(policy=TRACE_BEST) -> list:
        """The host's full CROSS-TIER cascade (``asmtest_trace_resolve``), most-
        faithful first: Intel PT -> AMD LBR -> DynamoRIO -> single-step -> CoreSight
        -> emulator, each included only if its tier is available. Returns a list of
        :class:`TierChoice`. ``TRACE_NATIVE_ONLY`` drops the emulator floor (no
        native->emulator fidelity crossing); ``TRACE_CEILING_FREE`` drops AMD LBR."""
        out = (_Choice * 8)()
        n = _get().asmtest_trace_resolve(policy, out, len(out))
        return [TierChoice(out[i].tier, out[i].backend, out[i].fidelity)
                for i in range(n)]

    @staticmethod
    def auto_tier(policy=TRACE_BEST):
        """The single most-preferred available cross-tier choice under ``policy`` as
        a :class:`TierChoice`, or ``None`` when the cascade is empty (only off a
        native host under ``TRACE_NATIVE_ONLY``)."""
        out = _Choice()
        rc = _get().asmtest_trace_auto(policy, C.byref(out))
        if rc != ASMTEST_HW_OK:
            return None
        return TierChoice(out.tier, out.backend, out.fidelity)

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


class Ptrace:
    """Out-of-process / foreign-process tracing (asmtest_ptrace.h): single-step a
    forked or externally-attached target out of band, and resolve the code region to
    trace from the OS — /proc/<pid>/maps, a JIT perf-map, or a binary jitdump. The
    managed-runtime path (JVM/.NET/Node on AMD, where Intel PT is unavailable and
    in-process DynamoRIO cannot seize the runtime's threads). Linux x86-64."""

    @staticmethod
    def available() -> bool:
        lib = _try_get()
        if lib is None:
            return False
        return bool(lib.asmtest_ptrace_available())

    @staticmethod
    def skip_reason() -> str:
        lib = _try_get()
        if lib is None:
            return "libasmtest_hwtrace not found"
        buf = C.create_string_buffer(160)
        lib.asmtest_ptrace_skip_reason(buf, len(buf))
        return buf.value.decode()

    @staticmethod
    def trace_call(code: NativeCode, args, trace: "HwTrace") -> int:
        """Fork a tracee that calls `code` (a NativeCode) with up to six integer
        `args`, single-step it out of process, and fill `trace`; returns the routine's
        return value (the child's RAX at the ret)."""
        n = len(args)
        arr = (C.c_long * max(n, 1))(*args)
        result = C.c_long()
        rc = _get().asmtest_ptrace_trace_call(
            code.base, code.length, arr, n, C.byref(result), trace._handle)
        if rc != ASMTEST_PTRACE_OK:
            raise RuntimeError(f"asmtest_ptrace_trace_call failed: {rc}")
        return result.value

    @staticmethod
    def trace_attached(pid: int, base: int, length: int, trace: "HwTrace") -> int:
        """Trace a region in a SEPARATE, already-ptrace-stopped process (the caller
        owns PTRACE_ATTACH/DETACH). Reads the target's bytes via process_vm_readv."""
        result = C.c_long()
        rc = _get().asmtest_ptrace_trace_attached(
            pid, C.c_void_p(base), length, C.byref(result), trace._handle)
        if rc != ASMTEST_PTRACE_OK:
            raise RuntimeError(f"asmtest_ptrace_trace_attached failed: {rc}")
        return result.value

    @staticmethod
    def trace_attached_versioned(pid: int, base: int, length: int,
                                 image: "CodeImage", when: int, trace: "HwTrace") -> int:
        """Like trace_attached, but decode the region against TIME-CORRECT bytes from a
        :class:`CodeImage` at logical timestamp `when` (0 = latest) instead of a single
        live snapshot — the right bytes when the address was re-JITted/reused mid-run."""
        result = C.c_long()
        rc = _get().asmtest_ptrace_trace_attached_versioned(
            pid, C.c_void_p(base), length, image._handle, when, C.byref(result),
            trace._handle)
        if rc != ASMTEST_PTRACE_OK:
            raise RuntimeError(f"asmtest_ptrace_trace_attached_versioned failed: {rc}")
        return result.value

    @staticmethod
    def run_to(pid: int, addr: int) -> int:
        """Run an already-attached, ptrace-stopped target forward until it reaches
        `addr` (a software breakpoint that fires when the program itself next calls in),
        leaving it stopped there ready for trace_attached — the step that turns a
        resolved JIT method into a traceable one when you don't control call timing.
        Returns ASMTEST_PTRACE_OK, or ASMTEST_PTRACE_ENOENT if the target exited first.
        The caller owns PTRACE_ATTACH/DETACH."""
        return _get().asmtest_ptrace_run_to(pid, C.c_void_p(addr))

    @staticmethod
    def region_by_addr(pid: int, addr: int):
        """The executable mapping in /proc/<pid>/maps containing `addr`, as
        (base, len), or None if no executable mapping contains it."""
        base = C.c_void_p()
        length = C.c_size_t()
        rc = _get().asmtest_proc_region_by_addr(
            pid, C.c_void_p(addr), C.byref(base), C.byref(length))
        return (base.value, length.value) if rc == ASMTEST_PTRACE_OK else None

    @staticmethod
    def perfmap_symbol(pid: int, name: str):
        """A JIT method by `name` in /tmp/perf-<pid>.map, as (base, len), or None."""
        base = C.c_void_p()
        length = C.c_size_t()
        rc = _get().asmtest_proc_perfmap_symbol(
            pid, name.encode(), C.byref(base), C.byref(length))
        return (base.value, length.value) if rc == ASMTEST_PTRACE_OK else None

    @staticmethod
    def jitdump_find(path, name: str, pid: int = 0, want_bytes: int = 0):
        """A JIT method by `name` from a jitdump (`path`, or /tmp/jit-<pid>.dump when
        path is None) as a JitMethod carrying up to `want_bytes` of recorded code, or
        None. The latest re-JIT body (highest timestamp) wins."""
        e = _JitEntry()
        buf = (C.c_ubyte * want_bytes)() if want_bytes else None
        blen = C.c_size_t()
        p = path.encode() if path is not None else None
        rc = _get().asmtest_jitdump_find(
            p, pid, name.encode(), C.byref(e), buf, want_bytes,
            C.byref(blen) if want_bytes else None)
        if rc != ASMTEST_PTRACE_OK:
            return None
        code = bytes(buf[:blen.value]) if want_bytes else b""
        return JitMethod(e.code_addr, e.code_size, e.timestamp, e.code_index, code)


class CodeImage:
    """Time-aware code-image recorder (asmtest_codeimage.h): a userspace
    PERF_RECORD_TEXT_POKE. Records a timestamped timeline of a process's code regions so
    :meth:`bytes_at` returns the bytes that were live at trace-position `when` — the right
    answer for a JIT whose code is patched/freed/reused, where a single late snapshot
    returns the wrong bytes. ``pid == 0`` records this process. The optional eBPF emission
    detector (:meth:`watch_bpf`/:meth:`poll_bpf`/:meth:`next_event`) self-skips without
    libbpf/CAP_BPF. Linux."""

    def __init__(self, pid: int = 0):
        self._handle = _get().asmtest_codeimage_new(pid)
        if not self._handle:
            raise RuntimeError("asmtest_codeimage_new failed")

    @staticmethod
    def available() -> bool:
        """True if soft-dirty page tracking works on this host (else self-skip)."""
        lib = _try_get()
        if lib is None:
            return False
        return bool(lib.asmtest_codeimage_available())

    @staticmethod
    def skip_reason() -> str:
        lib = _try_get()
        if lib is None:
            return "libasmtest_hwtrace not found"
        buf = C.create_string_buffer(200)
        lib.asmtest_codeimage_skip_reason(buf, len(buf))
        return buf.value.decode()

    def track(self, base: int, length: int) -> int:
        """Begin tracking [base, base+length): snapshot version 0 and arm change
        detection. Returns ASMTEST_CI_OK (0) or a negative status."""
        return int(_get().asmtest_codeimage_track(self._handle, C.c_void_p(base), length))

    def refresh(self) -> int:
        """Re-snapshot any changed tracked pages as new versions; returns the number of
        new versions recorded (>= 0) or a negative status."""
        return int(_get().asmtest_codeimage_refresh(self._handle))

    def now(self) -> int:
        """The current capture sequence (a monotonic logical timestamp)."""
        return int(_get().asmtest_codeimage_now(self._handle))

    def bytes_at(self, addr: int, when: int = 0):
        """The bytes live at `addr` as of sequence `when` (0 = latest), or None if the
        address was never tracked / had no version at-or-before `when`."""
        out = C.POINTER(C.c_ubyte)()
        outlen = C.c_size_t()
        rc = _get().asmtest_codeimage_bytes_at(
            self._handle, C.c_void_p(addr), when, C.byref(out), C.byref(outlen))
        if rc != ASMTEST_CI_OK:
            return None
        return bytes(out[i] for i in range(outlen.value))

    # ---- optional eBPF emission detector ----
    @staticmethod
    def bpf_available() -> bool:
        lib = _try_get()
        if lib is None:
            return False
        return bool(lib.asmtest_codeimage_bpf_available())

    @staticmethod
    def bpf_skip_reason() -> str:
        lib = _try_get()
        if lib is None:
            return "libasmtest_hwtrace not found"
        buf = C.create_string_buffer(200)
        lib.asmtest_codeimage_bpf_skip_reason(buf, len(buf))
        return buf.value.decode()

    def watch_bpf(self) -> int:
        """Load + attach the CO-RE emission detector, filtered to this image's pid.
        Returns ASMTEST_CI_OK (0), or a negative status (self-skips without libbpf)."""
        return int(_get().asmtest_codeimage_watch_bpf(self._handle))

    def poll_bpf(self, timeout_ms: int) -> int:
        """Drain ready emission events (timeout_ms == 0 is non-blocking); returns the
        number queued (>= 0) or a negative status."""
        return int(_get().asmtest_codeimage_poll_bpf(self._handle, timeout_ms))

    def next_event(self):
        """Pop one queued :class:`CodeEmission`, or None if the queue is empty."""
        ev = _CodeimageEvent()
        rc = _get().asmtest_codeimage_next(self._handle, C.byref(ev))
        if rc != 1:
            return None
        return CodeEmission(ev.addr, ev.len, ev.timestamp, ev.pid, ev.tid, ev.kind, ev.fd)

    def free(self):
        if self._handle:
            _get().asmtest_codeimage_free(self._handle)
            self._handle = None
