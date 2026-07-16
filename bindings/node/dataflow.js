// asmtest/dataflow (Node, koffi) — binding for the data-flow tier (Phase 6 + F7).
//
// Wraps libasmtest_dataflow (built with `make shared-dataflow`), mirroring the
// Python/C++ bindings: the pure GC-move canonicalizer, the tiered-re-JIT method
// resolver, the L0/L1/L2 ValueTrace pipeline, and — F7 — LIVE-ATTACH capture over a
// running pid (ValueTrace.attachPid / attachPidTid / attachJit), which fills that
// same ValueTrace so a live capture slices exactly like a hand-built one.
// available() === false when the lib is not built, so a general node binding run
// never fails on it; liveAttachAvailable() probes for the (Linux x86-64) attach tier.
'use strict';

const koffi = require('koffi');
const path = require('path');

// Struct layouts (koffi matches the C alignment/padding from the field types).
const GcMove = koffi.struct('asmtest_gcmove_t', {
  old_base: 'uint64',
  new_base: 'uint64',
  len: 'uint64',
  step: 'uint32',
});
const Method = koffi.struct('asmtest_method_t', {
  addr: 'uint64',
  size: 'uint64',
  name: 'str', // const char* (in-only string marshal)
  version: 'uint64',
});

// at_val_rec_t — one operand record; koffi matches the C layout from the field types.
const ValRec = koffi.struct('at_val_rec_t', {
  kind: 'int',
  reg: 'uint32',
  base: 'uint32',
  index: 'uint32',
  scale: 'int32',
  disp: 'int64',
  addr: 'uint64',
  size: 'uint16',
  is_write: 'bool',
  value_valid: 'bool',
  wide: 'bool',
  wide_off: 'uint32',
  value: 'uint64',
  step: 'uint32',
});

// at_loc_kind_t
const LOC_REG = 0;
const LOC_MEM_ABS = 1;
const LOC_MEM_OFF = 2;

function candidates() {
  const c = [];
  if (process.env.ASMTEST_DATAFLOW_LIB) c.push(process.env.ASMTEST_DATAFLOW_LIB);
  c.push(path.join(__dirname, '_libs', 'libasmtest_dataflow.so'));
  c.push(path.join(__dirname, '..', '..', 'build', 'libasmtest_dataflow.so'));
  c.push('libasmtest_dataflow.so');
  return c;
}

let _lib = null;
let _fn = null;
(function load() {
  for (const cand of candidates()) {
    try {
      _lib = koffi.load(cand);
      break;
    } catch (e) {
      /* try the next candidate */
    }
  }
  if (!_lib) return;
  _fn = {
    gcmoveCanon: _lib.func(
      'uint64 asmtest_gcmove_canon(asmtest_gcmove_t *moves, size_t nmoves, ' +
        'uint32 step, uint64 phys)'
    ),
    methodResolvePc: _lib.func(
      'int asmtest_method_resolve_pc(asmtest_method_t *methods, size_t nmethods, uint64 pc)'
    ),
    // L0 sink + L1 def-use + L2 slice (handles are opaque void*; the slice seed is
    // an at_val_rec_t passed BY VALUE, only its .step field is read).
    valtraceNew: _lib.func('void* asmtest_valtrace_new(size_t, size_t, size_t)'),
    valtraceFree: _lib.func('void asmtest_valtrace_free(void*)'),
    valtraceAppend: _lib.func(
      'void asmtest_valtrace_append(void*, uint64, at_val_rec_t *recs, size_t n)'
    ),
    defuseBuild: _lib.func('void* asmtest_defuse_build(void*)'),
    defuseFree: _lib.func('void asmtest_defuse_free(void*)'),
    sliceForward: _lib.func('void* asmtest_slice_forward(void*, at_val_rec_t)'),
    sliceBackward: _lib.func('void* asmtest_slice_backward(void*, at_val_rec_t)'),
    sliceFree: _lib.func('void asmtest_slice_free(void*)'),
    sliceContains: _lib.func('int asmtest_slice_contains(void*, uint32)'),
    valtraceSteps: _lib.func('size_t asmtest_valtrace_steps(void*)'),
    valtraceRecs: _lib.func('size_t asmtest_valtrace_recs(void*)'),
    // F7 — the LIVE-ATTACH producer entry points (src/dataflow_ptrace.c). The
    // producer ships no header (a value-trace PRODUCER is a tier, not part of the
    // shared sink API), so this prototype IS the contract — keep it in step with
    // that file. pid_t is int; `long` is 64-bit on the Linux x86-64 this tier runs
    // on. _Out_ params come back through a one-element array.
    attachPid: _lib.func(
      'int asmtest_dataflow_ptrace_attach_pid(int pid, uint64 base, size_t code_len, ' +
        'uint64 max_insns, _Out_ long *result, void *vt)'
    ),
    attachPidTid: _lib.func(
      'int asmtest_dataflow_ptrace_attach_pid_tid(int pid, int only_tid, uint64 base, ' +
        'size_t code_len, uint64 max_insns, _Out_ long *result, void *vt)'
    ),
    attachJit: _lib.func(
      'int asmtest_dataflow_ptrace_attach_jit(int pid, int only_tid, uint64 base, ' +
        'size_t code_len, void *img, uint64 when, uint64 max_insns, ' +
        '_Out_ long *result, _Out_ int *survived, void *vt)'
    ),
  };
})();

// True when libasmtest_dataflow loaded (i.e. `make shared-dataflow` has run).
function available() {
  return _fn !== null;
}

// --- F7: live-attach data-flow capture ------------------------------------
// The scoped ptrace producer's return codes (src/dataflow_ptrace.c), re-declared
// for the same reason the prototypes above are.
const PTRACE_OK = 0; // a complete scoped trace
const PTRACE_FAULT = 1; // the routine faulted; a partial trace is filled
const PTRACE_EINVAL = -1; // bad arguments
const PTRACE_ENOSYS = -3; // off Linux x86-64 / no Capstone: the tier is absent
const PTRACE_ETRACE = -4; // ptrace/wait failure (seccomp/yama)

// True iff this build's live-attach tier is real (Linux x86-64 + Capstone) rather
// than the off-platform ENOSYS stub. PROBED, not guessed: an argument-rejecting call
// returns EINVAL from the real producer and ENOSYS from the stub, which is the only
// way to tell them apart — the symbol resolves either way. Attaches to nothing.
function liveAttachAvailable() {
  if (!_fn) return false;
  const v = _fn.valtraceNew(1, 1, 0);
  try {
    const out = [0];
    return _fn.attachPid(0, 0n, 0, 0n, out, v) !== PTRACE_ENOSYS;
  } finally {
    _fn.valtraceFree(v);
  }
}

// Map heap address `phys` observed at value-trace `step` to its canonical
// (final-resting) address across the compactions in `moves`. Each move is a
// [old_base, new_base, len, step] tuple; `moves` must be sorted ascending by step.
// Returns a BigInt. See asmtest_gcmove_canon.
function gcmoveCanon(moves, step, phys) {
  if (!_fn) throw new Error('libasmtest_dataflow not loaded (make shared-dataflow)');
  const arr = moves.map((m) => ({
    old_base: BigInt(m[0]),
    new_base: BigInt(m[1]),
    len: BigInt(m[2]),
    step: m[3] >>> 0,
  }));
  // koffi returns a uint64 as a Number when it fits in 53 bits, else a BigInt;
  // normalize to BigInt so the API is consistent (exact for both forms).
  return BigInt(_fn.gcmoveCanon(arr, arr.length, step >>> 0, BigInt(phys)));
}

// Resolve `pc` to the owning method-map record index, or -1. Each method is an
// [addr, size, name, version] tuple (size 0 = point match on addr). Newest version
// wins on a tiered re-JIT collision. See asmtest_method_resolve_pc.
function methodResolvePc(methods, pc) {
  if (!_fn) throw new Error('libasmtest_dataflow not loaded (make shared-dataflow)');
  const arr = methods.map((m) => ({
    addr: BigInt(m[0]),
    size: BigInt(m[1]),
    name: String(m[2]),
    version: BigInt(m[3]),
  }));
  return _fn.methodResolvePc(arr, arr.length, BigInt(pc));
}

// Build a fully-populated at_val_rec_t (koffi wants every struct field; 64-bit
// fields are BigInt). `loc` is [kind, key] (key = reg id for LOC_REG, else addr).
function mkRec(loc, isWrite) {
  const [kind, key] = loc;
  const r = {
    kind,
    reg: 0,
    base: 0,
    index: 0,
    scale: 0,
    disp: 0n,
    addr: 0n,
    size: 0,
    is_write: isWrite,
    value_valid: false,
    wide: false,
    wide_off: 0,
    value: 0n,
    step: 0,
  };
  if (kind === LOC_REG) r.reg = key >>> 0;
  else r.addr = BigInt(key);
  return r;
}

function emptyRec(step) {
  const r = mkRec([LOC_REG, 0], false);
  r.step = step >>> 0;
  return r;
}

// A hand-built L0 value trace fed to the L1 def-use builder + L2 slicer — the analog
// of the Python/C++ ValueTrace. step() records read/write operand locations; each
// location is [LOC_REG, regId] or [LOC_MEM_ABS, addr]. forwardSlice/backwardSlice
// return a Set of reached step indices. Normally a producer fills the trace.
class ValueTrace {
  constructor(stepsCap = 256, recsCap = 2048) {
    if (!_fn) throw new Error('libasmtest_dataflow not loaded (make shared-dataflow)');
    this._v = _fn.valtraceNew(stepsCap, recsCap, 0);
    this._g = null;
    this._n = 0;
  }

  step(off, reads = [], writes = []) {
    const recs = [];
    for (const loc of reads) recs.push(mkRec(loc, false));
    for (const loc of writes) recs.push(mkRec(loc, true));
    _fn.valtraceAppend(this._v, BigInt(off), recs, recs.length);
    this._n++;
    if (this._g) {
      _fn.defuseFree(this._g);
      this._g = null;
    }
    return this;
  }

  _slice(origin, forward) {
    if (!this._g) this._g = _fn.defuseBuild(this._v);
    const seed = emptyRec(origin);
    const s = forward ? _fn.sliceForward(this._g, seed) : _fn.sliceBackward(this._g, seed);
    const out = new Set();
    for (let i = 0; i < this._n; i++) if (_fn.sliceContains(s, i)) out.add(i);
    _fn.sliceFree(s);
    return out;
  }

  forwardSlice(origin) {
    return this._slice(origin, true);
  }

  backwardSlice(sink) {
    return this._slice(sink, false);
  }

  // --- F7: live-attach capture (fills THIS trace) --------------------------
  // The producer fills the very asmtest_valtrace_t this handle owns, so a live
  // capture flows into the same forwardSlice/backwardSlice a hand-built trace does
  // — the point of every tier sharing one L0 sink. Each resyncs the step count from
  // the native trace (the producer appends behind our back) and drops a stale graph.

  _postAttach() {
    this._n = Number(_fn.valtraceSteps(this._v));
    if (this._g) {
      _fn.defuseFree(this._g);
      this._g = null;
    }
  }

  // Attach to LIVE `pid`, single-step [base, base+codeLen), then DETACH leaving the
  // target running. Steps the thread-group LEADER (a routine that only ever runs on
  // a worker thread needs attachPidTid). Returns { rc, result }.
  attachPid(pid, base, codeLen, maxInsns = 0) {
    const out = [0];
    const rc = _fn.attachPid(pid, BigInt(base), codeLen, BigInt(maxInsns), out, this._v);
    this._postAttach();
    return { rc, result: Number(out[0]) };
  }

  // As attachPid, but SEIZE every thread and step whichever one first ENTERS the
  // region — identified by its own tid, never assumed to be the leader — while the
  // siblings run free. onlyTid 0 = any thread; nonzero pins exactly one. This is the
  // entry managed methods need: they run on workers. Returns { rc, result }.
  attachPidTid(pid, onlyTid, base, codeLen, maxInsns = 0) {
    const out = [0];
    const rc = _fn.attachPidTid(
      pid, onlyTid, BigInt(base), codeLen, BigInt(maxInsns), out, this._v
    );
    this._postAttach();
    return { rc, result: Number(out[0]) };
  }

  // The JIT-aware live attach: worker-targeting plus an explicit survival report.
  // Returns { rc, result, survived } — survived === 1 means the detach left the
  // target alive. This is the entry F4's live GC-move canonicalization lane drives.
  // The versioned-decode code-image (img/when) is passed NULL: operands decode from
  // a live snapshot. `when` is accepted so the order matches the C entry point.
  attachJit(pid, onlyTid, base, codeLen, maxInsns = 0, when = 0) {
    const out = [0];
    const survived = [0];
    const rc = _fn.attachJit(
      pid, onlyTid, BigInt(base), codeLen, null, BigInt(when), BigInt(maxInsns),
      out, survived, this._v
    );
    this._postAttach();
    return { rc, result: Number(out[0]), survived: survived[0] };
  }

  // Steps / records stored in the trace (a live producer's, or hand-built ones).
  get steps() {
    return Number(_fn.valtraceSteps(this._v));
  }

  get recs() {
    return Number(_fn.valtraceRecs(this._v));
  }

  free() {
    if (this._g) {
      _fn.defuseFree(this._g);
      this._g = null;
    }
    if (this._v) {
      _fn.valtraceFree(this._v);
      this._v = null;
    }
  }
}

module.exports = {
  available,
  liveAttachAvailable,
  gcmoveCanon,
  methodResolvePc,
  ValueTrace,
  LOC_REG,
  LOC_MEM_ABS,
  LOC_MEM_OFF,
  PTRACE_OK,
  PTRACE_FAULT,
  PTRACE_EINVAL,
  PTRACE_ENOSYS,
  PTRACE_ETRACE,
};
