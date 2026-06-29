// hwtrace.js — hardware-tier native runtime tracing for Node, observing the real CPU.
//
// This is the Node counterpart to asmtest/hwtrace.py: the language-wrapper
// surface for the optional hardware-trace tier (see include/asmtest_hwtrace.h
// and docs/native-tracing.md). Like the DynamoRIO wrapper (drtrace.js) it records
// native execution as `asmtest_trace_t` offsets and reuses the begin/end region
// markers, but by observing the **real CPU** — and unlike the DynamoRIO wrapper it
// needs no DynamoRIO install.
//
// Four backends share one API, selected by enum:
//
//   * SINGLESTEP — EFLAGS.TF single-step (#DB -> SIGTRAP). Exact and complete on
//     ANY x86-64 Linux (Intel, any-Zen AMD, VM, CI, container): no PMU, no
//     perf_event, no privilege. This is the portable default and the one that runs
//     everywhere, so it is what this binding's self-test exercises live.
//   * INTEL_PT / CORESIGHT / AMD_LBR — hardware branch-trace backends that
//     self-skip off the specific bare-metal hardware they need.
//
// HwTrace.available(backend) reports whether the chosen backend can run so callers
// self-skip cleanly. The module loads libasmtest_hwtrace (resolved from
// $ASMTEST_HWTRACE_LIB, else the repo build/); nothing here links a decoder.
//
// Example:
//   const { HwTrace, NativeCode, SINGLESTEP } = require('./hwtrace');
//   if (HwTrace.available(SINGLESTEP)) {
//     HwTrace.init(SINGLESTEP);
//     const code = NativeCode.fromBytes(Buffer.from([0x48,0x89,0xF8,0x48,0x01,0xF0,0xC3]));
//     const tr = HwTrace.create({ blocks: 64, instructions: 64 });
//     tr.register('add', code);
//     let r; tr.region('add', () => { r = code.call(20, 22); });
//     // r === 42 && tr.covered(0)
//     HwTrace.shutdown();
//   }
'use strict';
const koffi = require('koffi');
const path = require('path');

const ASMTEST_HW_OK = 0;

const ASMTEST_HW_EUNAVAIL = -3; // no hardware-trace backend available on this host

// asmtest_trace_backend_t
const INTEL_PT = 0;
const CORESIGHT = 1;
const AMD_LBR = 2;
const SINGLESTEP = 3;

// asmtest_hwtrace_policy_t — backend auto-selection policy
const BEST = 0;         // the most faithful available backend
const CEILING_FREE = 1; // the same, but skipping the one fixed-window backend (AMD
                        // LBR); re-resolve under this after a trace comes back truncated

// asmtest_trace_auto.h — the CROSS-TIER orchestrator over all three trace tiers
// (hardware + DynamoRIO + emulator), not just the hardware backends above.
// asmtest_trace_tier_t
const TIER_HWTRACE = 0;   // HW branch trace / single-step (real CPU)
const TIER_DYNAMORIO = 1; // in-process software DBI (real CPU)
const TIER_EMULATOR = 2;  // Unicorn virtual CPU (isolated guest)
// asmtest_trace_fidelity_t
const FIDELITY_NATIVE = 0;  // runs the real bytes on the real CPU in-process
const FIDELITY_VIRTUAL = 1; // isolated guest on an emulated CPU
// cross-tier policy bitmask
const TRACE_BEST = 0x0;         // most-faithful available; emulator floor allowed
const TRACE_CEILING_FREE = 0x1; // drop the fixed-window backend (AMD LBR)
const TRACE_NATIVE_ONLY = 0x2;  // forbid the native->emulator fidelity crossing

// Resolve libasmtest_hwtrace: an explicit ASMTEST_HWTRACE_LIB wins (dev / custom
// build); otherwise fall back to the build/ tree next to the repo root. The tier
// may be absent, so the load is wrapped in try/catch and available() self-skips
// cleanly when it can't resolve.
function resolveHwtraceLib() {
  if (process.env.ASMTEST_HWTRACE_LIB) return process.env.ASMTEST_HWTRACE_LIB;
  const ext = process.platform === 'darwin' ? 'dylib' : 'so';
  // bindings/node/ -> repo root is two levels up.
  return path.join(__dirname, '..', '..', 'build', `libasmtest_hwtrace.${ext}`);
}

// koffi struct layout mirroring the C API (the one place we mirror a C struct;
// the rest of the surface is opaque handles, like asmtest.js).
const Options = koffi.struct('asmtest_hwtrace_options_t', {
  backend: 'int',
  aux_size: 'size_t',
  data_size: 'size_t',
  snapshot: 'int',
  object_hint: 'str',
});

// The generated code is invoked through this function-pointer prototype: each
// argument is passed as a C long and the result read as a long (the SysV integer
// ABI), matching the Python wrapper's default call().
const Fn2 = koffi.proto('long asmtest_native_fn(long, long)');

// Load the lib and declare every native entry point, all kept private here. On
// failure (no hwtrace build present) we keep `_lib` null so available() returns
// false instead of throwing — the tier self-skips.
let _lib = null;
let _fn = null;
let _loadError = null;

(function load() {
  let lib;
  try {
    lib = koffi.load(resolveHwtraceLib());
  } catch (e) {
    _loadError = e;
    return;
  }
  _lib = lib;
  _fn = {
    available: lib.func('int asmtest_hwtrace_available(int)'),
    skipReason: lib.func('void asmtest_hwtrace_skip_reason(int, _Out_ char*, size_t)'),
    resolve: lib.func('size_t asmtest_hwtrace_resolve(int, _Out_ int*, size_t)'),
    auto: lib.func('int asmtest_hwtrace_auto(int)'),
    // Cross-tier orchestrator (asmtest_trace_auto.h). A choice is exactly three
    // consecutive int32s (tier, backend, fidelity), so — like resolve above — we
    // marshal a raw int array and read triples rather than declaring a struct.
    traceResolve: lib.func('size_t asmtest_trace_resolve(uint, _Out_ int*, size_t)'),
    traceAuto: lib.func('int asmtest_trace_auto(uint, _Out_ int*)'),
    init: lib.func('int asmtest_hwtrace_init(const asmtest_hwtrace_options_t*)'),
    registerRegion: lib.func('int asmtest_hwtrace_register_region(const char*, void*, size_t, void*)'),
    begin: lib.func('void asmtest_hwtrace_begin(const char*)'),
    end: lib.func('void asmtest_hwtrace_end(const char*)'),
    shutdown: lib.func('void asmtest_hwtrace_shutdown()'),
    execAlloc: lib.func('int asmtest_hwtrace_exec_alloc(const void*, size_t, _Out_ void**, _Out_ size_t*)'),
    execFree: lib.func('void asmtest_hwtrace_exec_free(void*, size_t)'),
    // Trace handle + accessors (from the shared trace.o, also in this lib). NOTE
    // the argument order: asmtest_trace_new takes insns_cap FIRST, blocks_cap SECOND.
    traceNew: lib.func('void* asmtest_trace_new(size_t, size_t)'),
    traceFree: lib.func('void asmtest_trace_free(void*)'),
    traceCovered: lib.func('int asmtest_trace_covered(void*, uint64_t)'),
    blocksLen: lib.func('uint64_t asmtest_emu_trace_blocks_len(void*)'),
    insnsTotal: lib.func('uint64_t asmtest_emu_trace_insns_total(void*)'),
    insnsLen: lib.func('uint64_t asmtest_emu_trace_insns_len(void*)'),
    truncated: lib.func('int asmtest_emu_trace_truncated(void*)'),
    blockAt: lib.func('uint64_t asmtest_emu_trace_block_at(void*, size_t)'),
    insnAt: lib.func('uint64_t asmtest_emu_trace_insn_at(void*, size_t)'),
  };
})();

/** Host-native machine code in real executable (W^X) memory. */
class NativeCode {
  constructor(base, length) {
    this._base = base; // a koffi external pointer to the executable mapping
    this._len = length;
    this._freed = false;
  }

  /** Map executable memory and copy `buf` (Buffer/Uint8Array/Array) of host-native
   *  machine code into it; offset 0 is the entry point. */
  static fromBytes(buf) {
    const bytes = Buffer.isBuffer(buf) ? buf : Buffer.from(buf);
    const baseOut = [null];
    const lenOut = [0];
    const rc = _fn.execAlloc(bytes, bytes.length, baseOut, lenOut);
    if (rc !== ASMTEST_HW_OK) throw new Error(`asmtest_hwtrace_exec_alloc failed: ${rc}`);
    return new NativeCode(baseOut[0], Number(lenOut[0]));
  }

  /** The executable mapping holding the bytes (a koffi external pointer). */
  get base() { return this._base; }

  /** The number of code bytes. */
  get length() { return this._len; }

  /** Invoke the code through a function pointer with up to two integer args,
   *  passed as C longs and the result read back as a long (the SysV integer ABI).
   *  koffi turns the external `base` pointer into a callable by decoding it as the
   *  Fn2 prototype. Returns a JS number (or BigInt for out-of-safe-range results). */
  call(a = 0, b = 0) {
    const callable = koffi.decode(this._base, Fn2);
    return callable(a, b);
  }

  /** Unmap the executable memory. Unregister any region keyed to it FIRST. */
  free() {
    if (!this._freed) {
      _fn.execFree(this._base, this._len);
      this._freed = true;
    }
  }
}

/** A coverage recorder for a registered native region, via the hardware tier. */
class HwTrace {
  constructor(handle) { this._handle = handle; }

  // ---- process-wide lifecycle ----

  /** True if the chosen backend can run on this host (self-skip otherwise). False
   *  on load failure OR when the C side reports the backend is unavailable. */
  static available(backend = SINGLESTEP) {
    if (!_lib) return false;
    return _fn.available(backend) !== 0;
  }

  /** Human-readable reason available() is false (or 'available'). */
  static skipReason(backend = SINGLESTEP) {
    if (!_lib) return _loadError ? `load failed: ${_loadError.message}` : 'load failed';
    const buf = Buffer.alloc(160);
    _fn.skipReason(backend, buf, buf.length);
    const nul = buf.indexOf(0);
    return buf.toString('utf8', 0, nul < 0 ? buf.length : nul);
  }

  /** This host's hardware-trace fallback cascade: the available backends, most-
   *  faithful first (INTEL_PT > AMD_LBR > SINGLESTEP > CORESIGHT), honoring `policy`.
   *  Empty only off x86-64 Linux (single-step is the floor there). CEILING_FREE
   *  drops the depth-bounded backend (AMD LBR). Returns an array of backend ints. */
  static resolve(policy = BEST) {
    const out = Buffer.alloc(4 * 4); // up to 4 int32 backend enums
    const n = Number(_fn.resolve(policy, out, 4));
    const cascade = new Array(n);
    for (let i = 0; i < n; i++) cascade[i] = out.readInt32LE(i * 4);
    return cascade;
  }

  /** The single most-preferred available backend under `policy` (a backend enum
   *  >= 0, ready to init()), or ASMTEST_HW_EUNAVAIL (< 0) when no hardware-trace
   *  backend is available on this host. */
  static auto(policy = BEST) {
    return _fn.auto(policy);
  }

  /** This host's full CROSS-TIER cascade (asmtest_trace_resolve), most-faithful
   *  first: Intel PT -> AMD LBR -> DynamoRIO -> single-step -> CoreSight ->
   *  emulator, each included only if its tier is available. Returns an array of
   *  { tier, backend, fidelity } objects. TRACE_NATIVE_ONLY drops the emulator
   *  floor (no native->emulator fidelity crossing); TRACE_CEILING_FREE drops AMD
   *  LBR. A choice is three int32s, so we read consecutive triples. */
  static resolveTiers(policy = TRACE_BEST) {
    const cap = 8; // up to 8 cross-tier choices (Intel PT..emulator)
    const out = Buffer.alloc(cap * 3 * 4); // each choice = 3 int32s
    const n = Number(_fn.traceResolve(policy, out, cap));
    const cascade = new Array(n);
    for (let i = 0; i < n; i++) {
      cascade[i] = {
        tier: out.readInt32LE(i * 12),
        backend: out.readInt32LE(i * 12 + 4),
        fidelity: out.readInt32LE(i * 12 + 8),
      };
    }
    return cascade;
  }

  /** The single most-preferred available cross-tier choice under `policy` as a
   *  { tier, backend, fidelity } object, or null on EUNAVAIL (the cascade is empty
   *  only off a native host under TRACE_NATIVE_ONLY). */
  static autoTier(policy = TRACE_BEST) {
    const out = Buffer.alloc(3 * 4); // one choice = 3 int32s
    const rc = _fn.traceAuto(policy, out);
    if (rc !== ASMTEST_HW_OK) return null;
    return {
      tier: out.readInt32LE(0),
      backend: out.readInt32LE(4),
      fidelity: out.readInt32LE(8),
    };
  }

  /** Select a backend and initialize the tier. SINGLESTEP is the portable default
   *  that runs on any x86-64 Linux. Throws on a nonzero rc. */
  static init(backend = SINGLESTEP) {
    const opts = { backend, aux_size: 0, data_size: 0, snapshot: 0, object_hint: null };
    const rc = _fn.init(opts);
    if (rc !== ASMTEST_HW_OK) throw new Error(`asmtest_hwtrace_init failed: ${rc}`);
  }

  static shutdown() { _fn.shutdown(); }

  // ---- per-trace ----

  /** Allocate a trace handle. Block recording when `blocks` > 0, instruction
   *  recording when `instructions` > 0 (asmtest_trace_new takes insns first). */
  static create({ blocks = 64, instructions = 0 } = {}) {
    const h = _fn.traceNew(instructions, blocks);
    if (!h) throw new Error('asmtest_trace_new failed');
    return new HwTrace(h);
  }

  /** Register a non-overlapping native code range under `name`, recording
   *  coverage into this trace. */
  register(name, code) {
    const rc = _fn.registerRegion(name, code.base, code.length, this._handle);
    if (rc !== ASMTEST_HW_OK) throw new Error(`register_region(${name}) failed: ${rc}`);
    return this;
  }

  /** Open recording for `name`, run `fn`, then close recording — single-step arms
   *  the trap flag across the bracket, so keep begin..call..end tight and run end
   *  in a finally even if `fn` throws. */
  region(name, fn) {
    _fn.begin(name);
    try {
      return fn();
    } finally {
      _fn.end(name);
    }
  }

  /** True if the basic block at byte-offset `off` was entered. */
  covered(off) { return _fn.traceCovered(this._handle, off) !== 0; }

  /** Number of distinct basic blocks recorded. */
  blocksLen() { return Number(_fn.blocksLen(this._handle)); }

  /** Total instructions executed (may exceed the stored insnsLen if capped). */
  insnsTotal() { return Number(_fn.insnsTotal(this._handle)); }

  /** Number of instruction offsets actually stored. */
  insnsLen() { return Number(_fn.insnsLen(this._handle)); }

  /** True if the instruction stream was truncated (more executed than stored). */
  truncated() { return _fn.truncated(this._handle) !== 0; }

  /** The distinct basic-block start offsets recorded, in first-seen order. */
  blockOffsets() {
    const n = Number(_fn.blocksLen(this._handle));
    const out = new Array(n);
    for (let i = 0; i < n; i++) out[i] = Number(_fn.blockAt(this._handle, i));
    return out;
  }

  /** The ordered instruction-offset stream actually stored (insnsLen entries, in
   *  execution order — not the possibly-larger insnsTotal). */
  insnOffsets() {
    const n = Number(_fn.insnsLen(this._handle));
    const out = new Array(n);
    for (let i = 0; i < n; i++) out[i] = Number(_fn.insnAt(this._handle, i));
    return out;
  }

  free() {
    if (this._handle) {
      _fn.traceFree(this._handle);
      this._handle = null;
    }
  }
}

module.exports = {
  HwTrace, NativeCode,
  ASMTEST_HW_OK, ASMTEST_HW_EUNAVAIL, INTEL_PT, CORESIGHT, AMD_LBR, SINGLESTEP,
  BEST, CEILING_FREE,
  TIER_HWTRACE, TIER_DYNAMORIO, TIER_EMULATOR,
  FIDELITY_NATIVE, FIDELITY_VIRTUAL,
  TRACE_BEST, TRACE_CEILING_FREE, TRACE_NATIVE_ONLY,
};
