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
