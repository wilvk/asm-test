"""asmtest.dataflow — Python binding for the data-flow tier.

Wraps ``libasmtest_dataflow`` (built with ``make shared-dataflow``):

* the pure L0/L1/L2 analysis pipeline — the value-trace sink, the def-use graph,
  the slicer, method identity (:func:`method_resolve_pc`), GC-move
  canonicalization (:func:`gcmove_canon`), and runtime-helper summaries;
* **F7** — the scoped ptrace **live-attach** producer: :meth:`ValueTrace.attach_pid`,
  :meth:`ValueTrace.attach_pid_tid` and :meth:`ValueTrace.attach_jit` capture a real
  per-step value trace off a LIVE process by pid, out of band, then detach and leave
  it running.

Both halves meet at one opaque handle. :class:`ValueTrace` owns an
``asmtest_valtrace_t*``; a live capture fills that same sink a hand-built trace
does, so :meth:`~ValueTrace.forward_slice` / :meth:`~ValueTrace.backward_slice`
work identically over either — the tier's whole design point.

Live attach is Linux x86-64 only and needs ptrace permission over the target;
:func:`live_attach_available` probes for the tier and the ``PTRACE_*`` codes report
the rest. The Unicorn emulator producer remains a separate tier, not in this lib.
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
        lib.asmtest_valtrace_steps.restype = _C.c_size_t
        lib.asmtest_valtrace_steps.argtypes = [_C.c_void_p]
        lib.asmtest_valtrace_recs.restype = _C.c_size_t
        lib.asmtest_valtrace_recs.argtypes = [_C.c_void_p]
        # F7 — the LIVE-ATTACH producer entry points (src/dataflow_ptrace.c).
        # Declared unconditionally: this lib is the one `make shared-dataflow`
        # builds, and it always contains them (off Linux x86-64 / without Capstone
        # they are ENOSYS stubs, which is a RUNTIME answer live_attach_available()
        # reports honestly — not a missing symbol). A getattr-guarded registration
        # here would turn a mislinked lib into a silent skip.
        lib.asmtest_dataflow_ptrace_attach_pid.restype = _C.c_int
        lib.asmtest_dataflow_ptrace_attach_pid.argtypes = [
            _C.c_int, _C.c_uint64, _C.c_size_t, _C.c_uint64,
            _C.POINTER(_C.c_long), _C.c_void_p,
        ]
        lib.asmtest_dataflow_ptrace_attach_pid_tid.restype = _C.c_int
        lib.asmtest_dataflow_ptrace_attach_pid_tid.argtypes = [
            _C.c_int, _C.c_int, _C.c_uint64, _C.c_size_t, _C.c_uint64,
            _C.POINTER(_C.c_long), _C.c_void_p,
        ]
        lib.asmtest_dataflow_ptrace_attach_jit.restype = _C.c_int
        lib.asmtest_dataflow_ptrace_attach_jit.argtypes = [
            _C.c_int, _C.c_int, _C.c_uint64, _C.c_size_t, _C.c_void_p,
            _C.c_uint64, _C.c_uint64, _C.POINTER(_C.c_long),
            _C.POINTER(_C.c_int), _C.c_void_p,
        ]
        lib.asmtest_dataflow_ptrace_attach_pid_versioned.restype = _C.c_int
        lib.asmtest_dataflow_ptrace_attach_pid_versioned.argtypes = [
            _C.c_int, _C.c_uint64, _C.c_size_t, _C.c_uint64, _C.c_void_p,
            _C.c_uint64, _C.POINTER(_C.c_long), _C.c_void_p,
        ]
        # T4 — the versioned-decode code-image recorder (asmtest_codeimage.h). Its
        # object (pic/codeimage.o) is already linked into libasmtest_dataflow (like
        # the F7 entry points above), so these symbols are always present too.
        lib.asmtest_codeimage_available.restype = _C.c_int
        lib.asmtest_codeimage_available.argtypes = []
        lib.asmtest_codeimage_skip_reason.argtypes = [_C.c_char_p, _C.c_size_t]
        lib.asmtest_codeimage_new.restype = _C.c_void_p
        lib.asmtest_codeimage_new.argtypes = [_C.c_int]
        lib.asmtest_codeimage_free.argtypes = [_C.c_void_p]
        lib.asmtest_codeimage_track.restype = _C.c_int
        lib.asmtest_codeimage_track.argtypes = [_C.c_void_p, _C.c_void_p, _C.c_size_t]
        lib.asmtest_codeimage_now.restype = _C.c_uint64
        lib.asmtest_codeimage_now.argtypes = [_C.c_void_p]
        lib.asmtest_codeimage_bytes_at.restype = _C.c_int
        lib.asmtest_codeimage_bytes_at.argtypes = [
            _C.c_void_p, _C.c_void_p, _C.c_uint64,
            _C.POINTER(_C.POINTER(_C.c_ubyte)), _C.POINTER(_C.c_size_t),
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


# --- F7: live-attach data-flow capture ------------------------------------
# The scoped ptrace producer's return codes (src/dataflow_ptrace.c). The producer
# ships NO header on purpose (a value-trace PRODUCER is a tier, not part of the
# shared sink API), so — exactly as its own C suite does — the binding re-declares
# them. Keep in step with that file.
PTRACE_OK = 0  # a complete scoped trace
PTRACE_FAULT = 1  # the routine faulted; a partial trace is filled
PTRACE_EINVAL = -1  # bad arguments
PTRACE_ENOSYS = -3  # off Linux x86-64 / no Capstone: the tier is absent
PTRACE_ETRACE = -4  # ptrace/wait failure (seccomp/yama): the caller self-skips


def live_attach_available():
    """True iff this build's live-attach tier is real (Linux x86-64 + Capstone).

    Probed, not guessed: an argument-rejecting call (pid 0) returns ``EINVAL`` from
    the real producer but ``ENOSYS`` from the off-platform stub, which is the only
    honest way to tell them apart — the symbol resolves either way. Side-effect
    free; it attaches to nothing.
    """
    lib = _load()
    v = lib.asmtest_valtrace_new(1, 1, 0)
    try:
        out = _C.c_long(0)
        rc = lib.asmtest_dataflow_ptrace_attach_pid(0, 0, 0, 0, _C.byref(out), v)
    finally:
        lib.asmtest_valtrace_free(v)
    return rc != PTRACE_ENOSYS


# T4 — the versioned-decode code-image recorder's status codes, re-declared for
# the same reason the PTRACE_* ones are.
CI_OK = 0  # ok
CI_ENOENT = -7  # address never tracked / no version at-or-before `when`


class CodeImage:
    """Time-aware code-image recorder (asmtest_codeimage.h): a userspace
    PERF_RECORD_TEXT_POKE. Records a timestamped timeline of a process's code
    regions so :meth:`bytes_at` returns the bytes that were live at trace-position
    `when` — the right answer for a JIT whose code is patched/freed/reused, where a
    single late snapshot returns the wrong bytes. ``pid == 0`` records this process.
    Mirrors the CodeImage class already shipped for the hwtrace binding
    (hwtrace.py), against this lib's own handle."""

    def __init__(self, pid: int = 0):
        self._lib = _load()
        self._handle = self._lib.asmtest_codeimage_new(pid)
        if not self._handle:
            raise RuntimeError("asmtest_codeimage_new failed")

    @staticmethod
    def available() -> bool:
        lib = _load()
        return bool(lib.asmtest_codeimage_available())

    @staticmethod
    def skip_reason() -> str:
        lib = _load()
        buf = _C.create_string_buffer(200)
        lib.asmtest_codeimage_skip_reason(buf, len(buf))
        return buf.value.decode()

    def track(self, base: int, length: int) -> int:
        """Begin tracking [base, base+length): snapshot version 0 and arm change
        detection. Returns CI_OK (0) or a negative status."""
        return int(self._lib.asmtest_codeimage_track(self._handle, _C.c_void_p(base), length))

    def now(self) -> int:
        """The current capture sequence (a monotonic logical timestamp)."""
        return int(self._lib.asmtest_codeimage_now(self._handle))

    def bytes_at(self, addr: int, when: int = 0):
        """The bytes live at `addr` as of sequence `when` (0 = latest), or None if
        the address was never tracked / had no version at-or-before `when`."""
        out = _C.POINTER(_C.c_ubyte)()
        outlen = _C.c_size_t()
        rc = self._lib.asmtest_codeimage_bytes_at(
            self._handle, _C.c_void_p(addr), when, _C.byref(out), _C.byref(outlen))
        if rc != CI_OK:
            return None
        return bytes(out[i] for i in range(outlen.value))

    def free(self):
        if self._handle:
            self._lib.asmtest_codeimage_free(self._handle)
            self._handle = None


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

    # --- F7: live-attach capture (fills this same trace) -------------------
    # The producer fills the very asmtest_valtrace_t this handle owns, so a live
    # capture flows into the SAME def-use / slice methods a hand-built trace does
    # — which is the whole point of the tier sharing one L0 sink. Each of these
    # RESYNCS _n_steps from the native trace afterwards (the producer appends
    # behind our back) and drops any stale def-use graph.

    def _post_attach(self):
        self._n_steps = self._lib.asmtest_valtrace_steps(self._v)
        if self._g:
            self._lib.asmtest_defuse_free(self._g)
        self._g = None

    def attach_pid(self, pid, base, code_len, max_insns=0):
        """Attach to LIVE pid, single-step the region [base, base+code_len), detach.

        Steps the thread-group LEADER (a routine that only ever runs on a worker
        thread needs :meth:`attach_pid_tid`). Returns ``(rc, result)``: an
        ``PTRACE_*`` code and the region's return value. On ``PTRACE_OK`` this
        trace holds the captured steps. The target is NOT killed — it survives the
        detach and keeps running.
        """
        out = _C.c_long(0)
        rc = self._lib.asmtest_dataflow_ptrace_attach_pid(
            int(pid), int(base), int(code_len), int(max_insns), _C.byref(out), self._v
        )
        self._post_attach()
        return int(rc), int(out.value)

    def attach_pid_tid(self, pid, only_tid, base, code_len, max_insns=0):
        """:meth:`attach_pid`, but step whichever THREAD first enters the region.

        SEIZEs every thread of `pid`, plants one shared region-entry breakpoint and
        steps the thread that arrives — identified by its own tid, never assumed to
        be the leader — while the siblings run free. ``only_tid=0`` means any
        thread; a nonzero value pins exactly one. This is the entry managed methods
        need: they run on workers. Returns ``(rc, result)``.
        """
        out = _C.c_long(0)
        rc = self._lib.asmtest_dataflow_ptrace_attach_pid_tid(
            int(pid), int(only_tid), int(base), int(code_len), int(max_insns),
            _C.byref(out), self._v,
        )
        self._post_attach()
        return int(rc), int(out.value)

    def attach_jit(self, pid, only_tid, base, code_len, max_insns=0, when=0, img=None):
        """The JIT-aware live attach: worker-targeting + an explicit survival report.

        Returns ``(rc, result, survived)`` — ``survived`` is 1 when the detach left
        the target alive and running. This is the entry point F4's live GC-move
        canonicalization lane drives.

        ``img`` (a :class:`CodeImage`, or None) is the versioned-decode code-image:
        pass one to decode each step from the bytes live *at* ``when`` instead of a
        live snapshot of the target; None (the default) keeps today's behaviour.
        """
        out = _C.c_long(0)
        survived = _C.c_int(0)
        img_h = img._handle if img is not None else None
        rc = self._lib.asmtest_dataflow_ptrace_attach_jit(
            int(pid), int(only_tid), int(base), int(code_len), img_h, int(when),
            int(max_insns), _C.byref(out), _C.byref(survived), self._v,
        )
        self._post_attach()
        return int(rc), int(out.value), int(survived.value)

    def attach_pid_versioned(self, pid, base, code_len, img, when, max_insns=0):
        """The versioned-decode live attach: steps the thread-group LEADER (like
        :meth:`attach_pid`) but decodes each step's operands from ``img``'s bytes
        as of sequence ``when`` instead of a live snapshot — the right answer when
        a live JIT patches/frees/reuses the region mid-capture. ``img`` (a
        :class:`CodeImage`) may be None (degrades to exactly :meth:`attach_pid`).
        Returns ``(rc, result)``.
        """
        out = _C.c_long(0)
        img_h = img._handle if img is not None else None
        rc = self._lib.asmtest_dataflow_ptrace_attach_pid_versioned(
            int(pid), int(base), int(code_len), int(max_insns), img_h, int(when),
            _C.byref(out), self._v,
        )
        self._post_attach()
        return int(rc), int(out.value)

    @property
    def steps(self):
        """Steps stored in the trace (the live producer's, or the hand-built ones)."""
        return int(self._lib.asmtest_valtrace_steps(self._v))

    @property
    def recs(self):
        """Operand records stored in the trace."""
        return int(self._lib.asmtest_valtrace_recs(self._v))

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
