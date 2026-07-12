// asmtest/dataflow (Node, koffi) — binding for the data-flow analysis tier (Phase 6).
//
// Wraps libasmtest_dataflow (built with `make shared-dataflow`): this increment mirrors
// the Python/C++ bindings — the pure GC-move canonicalizer and the tiered-re-JIT method
// resolver. self-skips (available() === false) when the lib is not built, so a general
// node binding run never fails on it.
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
  };
})();

// True when libasmtest_dataflow loaded (i.e. `make shared-dataflow` has run).
function available() {
  return _fn !== null;
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
  gcmoveCanon,
  methodResolvePc,
  ValueTrace,
  LOC_REG,
  LOC_MEM_ABS,
  LOC_MEM_OFF,
};
