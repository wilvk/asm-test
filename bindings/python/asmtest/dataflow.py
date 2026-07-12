"""asmtest.dataflow — Python binding for the data-flow analysis tier.

Phase 6, increment 1. Wraps ``libasmtest_dataflow`` (built with
``make shared-dataflow``) — the pure L0/L1/L2 analysis pipeline: the value-trace
sink, the def-use graph, the slicer, method identity, GC-move canonicalization,
and runtime-helper summaries. This first increment exposes the pure,
allocation-free **GC-move canonicalizer** (:func:`gcmove_canon`); the remaining
surface (value-trace build, def-use, slicing) is wrapped in later increments.

The value-trace PRODUCERS (emulator / ptrace / DynamoRIO) are separate native
tiers and are not part of this analysis library.
"""

import ctypes as _C
import os as _os
from pathlib import Path as _Path

_HERE = _Path(__file__).resolve().parent
_REPO = _HERE.parent.parent.parent  # bindings/python/asmtest -> repo root
_LIBS = _HERE / "_libs"

_lib = None


def _lib_name():
    # Linux only for now (the tier's producers are Linux/x86-64); a darwin dylib
    # name is a later increment, matching the other bindings' platform maps.
    return "libasmtest_dataflow.so"


def _load():
    """Locate + dlopen libasmtest_dataflow. Bundled (_libs) is tried before the dev
    build/ tree so an installed package never prefers a leaked checkout."""
    global _lib
    if _lib is not None:
        return _lib
    cands = []
    env = _os.environ.get("ASMTEST_DATAFLOW_LIB")
    if env:
        cands.append(_Path(env))
    cands += [_LIBS / _lib_name(), _REPO / "build" / _lib_name(), _Path(_lib_name())]
    errors = []
    for c in cands:
        try:
            lib = _C.CDLL(str(c))
        except OSError as e:  # noqa: PERF203
            errors.append(f"  {c}: {e}")
            continue
        lib.asmtest_gcmove_canon.restype = _C.c_uint64
        lib.asmtest_gcmove_canon.argtypes = [
            _C.c_void_p, _C.c_size_t, _C.c_uint32, _C.c_uint64
        ]
        lib.asmtest_method_resolve_pc.restype = _C.c_int
        lib.asmtest_method_resolve_pc.argtypes = [
            _C.c_void_p, _C.c_size_t, _C.c_uint64
        ]
        # L0 sink + L1 def-use + L2 slice (the analysis pipeline). Handles are
        # opaque here (passed as void*); records + the slice seed marshal via _ValRec.
        lib.asmtest_valtrace_new.restype = _C.c_void_p
        lib.asmtest_valtrace_new.argtypes = [_C.c_size_t, _C.c_size_t, _C.c_size_t]
        lib.asmtest_valtrace_free.argtypes = [_C.c_void_p]
        lib.asmtest_valtrace_append.argtypes = [
            _C.c_void_p, _C.c_uint64, _C.POINTER(_ValRec), _C.c_size_t
        ]
        lib.asmtest_defuse_build.restype = _C.c_void_p
        lib.asmtest_defuse_build.argtypes = [_C.c_void_p]
        lib.asmtest_defuse_free.argtypes = [_C.c_void_p]
        lib.asmtest_slice_forward.restype = _C.c_void_p
        lib.asmtest_slice_forward.argtypes = [_C.c_void_p, _ValRec]
        lib.asmtest_slice_backward.restype = _C.c_void_p
        lib.asmtest_slice_backward.argtypes = [_C.c_void_p, _ValRec]
        lib.asmtest_slice_free.argtypes = [_C.c_void_p]
        lib.asmtest_slice_contains.restype = _C.c_int
        lib.asmtest_slice_contains.argtypes = [_C.c_void_p, _C.c_uint32]
        _lib = lib
        return _lib
    raise OSError(
        "libasmtest_dataflow not found; build it with `make shared-dataflow` "
        "or set ASMTEST_DATAFLOW_LIB.\n" + "\n".join(errors)
    )


class _GcMove(_C.Structure):
    # Mirrors asmtest_gcmove_t (include/asmtest_valtrace.h): three u64 + a u32 step;
    # natural alignment pads it to 32 bytes exactly as the C struct.
    _fields_ = [
        ("old_base", _C.c_uint64),
        ("new_base", _C.c_uint64),
        ("len", _C.c_uint64),
        ("step", _C.c_uint32),
    ]


def gcmove_canon(moves, step, phys):
    """Map heap address ``phys`` observed at value-trace ``step`` to its canonical
    (final-resting) address after every GC compaction in ``moves`` that relocates it.

    ``moves`` is a sequence of ``(old_base, new_base, len, step)`` tuples — the shape
    of an EventPipe ``GCBulkMovedObjectRanges`` batch — and MUST be sorted ascending
    by step. A move whose boundary step is strictly greater than ``step`` forwards an
    address in its ``[old_base, old_base+len)`` window to ``new_base + offset``;
    everything else (post-move observations, out-of-range addresses, an empty move
    set) is returned unchanged. Pure and deterministic.
    """
    lib = _load()
    n = len(moves)
    arr = (_GcMove * n)() if n else None
    for i, m in enumerate(moves):
        old_base, new_base, length, mstep = m
        arr[i] = _GcMove(old_base, new_base, length, mstep)
    return int(lib.asmtest_gcmove_canon(arr, n, int(step), int(phys)))


class _Method(_C.Structure):
    # Mirrors asmtest_method_t: [addr, addr+size) bounds the body (size 0 = point
    # match on addr), a borrowed name, and the version / code_index re-JIT counter.
    _fields_ = [
        ("addr", _C.c_uint64),
        ("size", _C.c_uint64),
        ("name", _C.c_char_p),
        ("version", _C.c_uint64),
    ]


def method_resolve_pc(methods, pc):
    """Resolve ``pc`` to the index of the method-map record that owns it, or -1.

    ``methods`` is a sequence of ``(addr, size, name, version)`` tuples (``size == 0``
    means a point match on ``addr`` only; ``name`` is a str or bytes). When several
    records cover ``pc`` after an in-place tiered re-JIT at a reused address, the one
    with the GREATEST version wins (ties resolve to the last such record — the newest
    load). Pure; the map need not be sorted.
    """
    lib = _load()
    n = len(methods)
    keepalive = []  # hold the encoded name bytes alive across the call
    arr = (_Method * n)() if n else None
    for i, m in enumerate(methods):
        addr, size, name, version = m
        nb = name.encode() if isinstance(name, str) else (name or b"")
        keepalive.append(nb)
        arr[i] = _Method(addr, size, nb, version)
    return int(lib.asmtest_method_resolve_pc(arr, n, int(pc)))


# at_loc_kind_t (include/asmtest_valtrace.h): the location space of an operand.
LOC_REG = 0  # a register (key = Capstone reg id)
LOC_MEM_ABS = 1  # memory at an absolute effective address (key = addr)
LOC_MEM_OFF = 2  # memory at a routine offset


class _ValRec(_C.Structure):
    # Mirrors at_val_rec_t exactly (natural alignment matches the C layout; the
    # round-trip def-use/slice test validates the marshalling end to end).
    _fields_ = [
        ("kind", _C.c_int),  # at_loc_kind_t
        ("reg", _C.c_uint32),
        ("base", _C.c_uint32),
        ("index", _C.c_uint32),
        ("scale", _C.c_int32),
        ("disp", _C.c_int64),
        ("addr", _C.c_uint64),
        ("size", _C.c_uint16),
        ("is_write", _C.c_bool),
        ("value_valid", _C.c_bool),
        ("wide", _C.c_bool),
        ("wide_off", _C.c_uint32),
        ("value", _C.c_uint64),
        ("step", _C.c_uint32),
    ]


def _mk_rec(loc, is_write):
    # loc is (kind, key): key is a reg id for LOC_REG, else an absolute address.
    kind, key = loc
    r = _ValRec()
    r.kind = int(kind)
    if kind == LOC_REG:
        r.reg = int(key)
    else:
        r.addr = int(key)
    r.is_write = bool(is_write)
    return r


class ValueTrace:
    """A hand-built L0 value trace fed to the L1 def-use builder + L2 slicer.

    Each :meth:`step` records one executed instruction as its read + write operand
    *locations* — the last-writer def-use is built from those (values are optional and
    not needed for slicing). A location is a ``(kind, key)`` pair: ``(LOC_REG, reg_id)``
    or ``(LOC_MEM_ABS, address)``. :meth:`forward_slice` / :meth:`backward_slice` return
    the set of step indices reached. Normally a producer (emulator / ptrace / DR) fills
    the trace; this hand-built path exercises the analysis directly.
    """

    def __init__(self, steps_cap=256, recs_cap=2048):
        self._lib = _load()
        self._v = self._lib.asmtest_valtrace_new(steps_cap, recs_cap, 0)
        if not self._v:
            raise MemoryError("asmtest_valtrace_new failed")
        self._g = None
        self._n_steps = 0  # tracked Python-side (avoids marshalling the sink struct)

    def step(self, off, reads=(), writes=()):
        """Append one executed instruction at offset `off` reading `reads` and writing
        `writes` (each a sequence of (kind, key) locations). Read-set before write-set."""
        recs = [_mk_rec(loc, False) for loc in reads] + [
            _mk_rec(loc, True) for loc in writes
        ]
        n = len(recs)
        arr = (_ValRec * n)(*recs) if n else None
        self._lib.asmtest_valtrace_append(self._v, int(off), arr, n)
        self._n_steps += 1
        self._g = None  # invalidate a stale def-use graph
        return self

    def _defuse(self):
        if self._g is None:
            self._g = self._lib.asmtest_defuse_build(self._v)
            if not self._g:
                raise MemoryError("asmtest_defuse_build failed")
        return self._g

    def _slice(self, origin, forward):
        g = self._defuse()
        seed = _ValRec()
        seed.step = int(origin)
        fn = self._lib.asmtest_slice_forward if forward else self._lib.asmtest_slice_backward
        s = fn(g, seed)
        try:
            out = set()
            # Probe membership for every recorded step (slice steps are a subset).
            for i in range(self._n_steps):
                if self._lib.asmtest_slice_contains(s, i):
                    out.add(i)
            return out
        finally:
            self._lib.asmtest_slice_free(s)

    def forward_slice(self, origin):
        """Steps influenced by the value defined at step `origin` (origin included)."""
        return self._slice(origin, True)

    def backward_slice(self, sink):
        """Steps that produced the value used at step `sink` (sink included)."""
        return self._slice(sink, False)

    def free(self):
        if self._g:
            self._lib.asmtest_defuse_free(self._g)
            self._g = None
        if self._v:
            self._lib.asmtest_valtrace_free(self._v)
            self._v = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.free()
        return False
