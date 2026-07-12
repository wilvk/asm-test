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

module.exports = { available, gcmoveCanon, methodResolvePc };
