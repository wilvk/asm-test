// drtrace.js — in-process native runtime tracing for Node, backed by DynamoRIO.
//
// This is the Node counterpart to asmtest/drtrace.py: the language-wrapper
// surface for the optional DynamoRIO native-trace tier (see
// include/asmtest_drtrace.h and docs/native-tracing.md). Where the emulator tier
// (asmtest.js's Trace) traces isolated guest bytes, NativeTrace traces
// host-native code as it runs **inside this Node process**: initialize DynamoRIO
// once at startup, materialize host-native machine code, mark a region, call into
// it, and read back basic-block coverage / the instruction stream.
//
// It loads libasmtest_drapp and drives the C API; libdynamorio is dlopen()ed
// lazily by the C side after the client is configured, so nothing here links
// DynamoRIO. Advanced, Linux-x86-64-only, and opt-in: NativeTrace.available()
// reports whether the tier can run so callers self-skip cleanly.
//
// The shared library is taken from the environment, matching how the framework's
// Makefile invokes the bindings:
//   ASMTEST_DRAPP_LIB   libasmtest_drapp.{so,dylib}  (the app-facing DR API)
//   ASMTEST_DRCLIENT    libasmtest_drclient.so       (read by the C side at init)
//
// Example:
//   const { NativeTrace, NativeCode } = require('./drtrace');
//   NativeTrace.initialize({ client: './build/libasmtest_drclient.so',
//                            dynamorioHome: '/opt/DynamoRIO' });
//   const code = NativeCode.fromBytes(Buffer.from([0x48,0x89,0xF8,0x48,0x01,0xF0,0xC3]));
//   const tr = NativeTrace.create(64);
//   tr.register('add', code);
//   let r; tr.region('add', () => { r = code.call(20, 22); });
//   // r === 42 && tr.covered(0)
//   NativeTrace.shutdown();
'use strict';
const koffi = require('koffi');
const path = require('path');

const ASMTEST_DR_OK = 0;

// Resolve libasmtest_drapp: an explicit ASMTEST_DRAPP_LIB wins (dev / custom
// build); otherwise fall back to the build/ tree next to the repo root. The tier
// requires DynamoRIO and may be absent, so the load is wrapped in try/catch and
// available() self-skips cleanly when it can't resolve.
function resolveDrappLib() {
  if (process.env.ASMTEST_DRAPP_LIB) return process.env.ASMTEST_DRAPP_LIB;
  const ext = process.platform === 'darwin' ? 'dylib' : 'so';
  // bindings/node/ -> repo root is two levels up.
  return path.join(__dirname, '..', '..', 'build', `libasmtest_drapp.${ext}`);
}

// koffi struct layouts mirroring the C API (the one place we mirror C structs;
// the rest of the surface is opaque handles, like asmtest.js).
const Options = koffi.struct('asmtest_drtrace_options_t', {
  dynamorio_home: 'str',
  client_path: 'str',
  client_options: 'str',
  mode: 'int',
});
const ExecCode = koffi.struct('asmtest_exec_code_t', { base: 'void*', len: 'size_t' });

// The generated code is invoked through this function-pointer prototype: each
// argument is passed as a C long and the result read as a long (the SysV integer
// ABI), matching the Python wrapper's default call().
const Fn2 = koffi.proto('long asmtest_native_fn(long, long)');

// Load the lib and declare every native entry point, all kept private here. On
// failure (no DynamoRIO build present) we keep `_lib` null so available() returns
// false instead of throwing — the tier self-skips.
let _lib = null;
let _fn = null;
let _loadError = null;

(function load() {
  let lib;
  try {
    lib = koffi.load(resolveDrappLib());
  } catch (e) {
    _loadError = e;
    return;
  }
  _lib = lib;
  _fn = {
    available: lib.func('int asmtest_dr_available()'),
    init: lib.func('int asmtest_dr_init(const asmtest_drtrace_options_t*)'),
    start: lib.func('int asmtest_dr_start()'),
    stop: lib.func('int asmtest_dr_stop()'),
    shutdown: lib.func('void asmtest_dr_shutdown()'),
    registerRegion: lib.func('int asmtest_dr_register_region(const char*, void*, size_t, void*)'),
    unregisterRegion: lib.func('int asmtest_dr_unregister_region(const char*)'),
    traceBegin: lib.func('void asmtest_trace_begin(const char*)'),
    traceEnd: lib.func('void asmtest_trace_end(const char*)'),
    markerError: lib.func('int asmtest_dr_marker_error()'),
    execAlloc: lib.func('int asmtest_exec_alloc(const uint8_t*, size_t, _Out_ asmtest_exec_code_t*)'),
    execFree: lib.func('void asmtest_exec_free(asmtest_exec_code_t*)'),
    // Trace handle + accessors (from the shared trace.o). NOTE the argument
    // order: asmtest_trace_new takes insns_cap FIRST, blocks_cap SECOND.
    traceNew: lib.func('void* asmtest_trace_new(size_t, size_t)'),
    traceFree: lib.func('void asmtest_trace_free(void*)'),
    traceCovered: lib.func('int asmtest_trace_covered(void*, uint64_t)'),
    blocksLen: lib.func('uint64_t asmtest_emu_trace_blocks_len(void*)'),
    insnsTotal: lib.func('uint64_t asmtest_emu_trace_insns_total(void*)'),
  };
})();

/** Host-native machine code in real executable (W^X) memory. */
class NativeCode {
  constructor(execCode) {
    this._code = execCode; // a decoded asmtest_exec_code_t object {base, len}
    this._freed = false;
  }

  /** Map executable memory and copy `buf` (Buffer/Uint8Array) of host-native
   *  machine code into it; offset 0 is the entry point. */
  static fromBytes(buf) {
    const bytes = Buffer.isBuffer(buf) ? buf : Buffer.from(buf);
    const out = {};
    const rc = _fn.execAlloc(bytes, bytes.length, out);
    if (rc !== ASMTEST_DR_OK) throw new Error(`asmtest_exec_alloc failed: ${rc}`);
    return new NativeCode(out);
  }

  /** The executable mapping holding the bytes (a koffi external pointer). */
  get base() { return this._code.base; }

  /** The number of code bytes. */
  get length() { return Number(this._code.len); }

  /** Invoke the code through a function pointer with two integer args, passed as
   *  C longs and the result read back as a long (the SysV integer ABI). koffi
   *  turns the external `base` pointer into a callable by decoding it as the Fn2
   *  prototype. Returns a JS number (or BigInt for out-of-safe-range results). */
  call(a = 0, b = 0) {
    const callable = koffi.decode(this._code.base, Fn2);
    return callable(a, b);
  }

  /** Unmap the executable memory. Unregister any region keyed to it FIRST. */
  free() {
    if (!this._freed) {
      _fn.execFree(this._code);
      this._freed = true;
    }
  }
}

/** An app-owned coverage recorder for a registered native region. */
class NativeTrace {
  constructor(handle) { this._handle = handle; }

  // ---- process-wide lifecycle ----

  /** True if the tier can run (built + libdynamorio resolvable). False on load
   *  failure OR when the C side reports the tier is unavailable. */
  static available() {
    if (!_lib) return false;
    return _fn.available() !== 0;
  }

  /**
   * Bring DynamoRIO up in-process and take over: fill the options struct, run
   * asmtest_dr_init then asmtest_dr_start, throwing on a nonzero rc. `client` is
   * the path to libasmtest_drclient.so (empty/undefined -> null, so the C side
   * falls back to $ASMTEST_DRCLIENT); `dynamorioHome` lets the C side find
   * libdynamorio (else $ASMTEST_DR_LIB / rpath).
   */
  static initialize({ client, dynamorioHome, clientOptions, mode = 0 } = {}) {
    const opts = {
      client_path: client || null,
      dynamorio_home: dynamorioHome || null,
      client_options: clientOptions || null,
      mode,
    };
    let rc = _fn.init(opts);
    if (rc !== ASMTEST_DR_OK) throw new Error(`asmtest_dr_init failed: ${rc}`);
    rc = _fn.start();
    if (rc !== ASMTEST_DR_OK) throw new Error(`asmtest_dr_start failed: ${rc}`);
  }

  static shutdown() { _fn.shutdown(); }

  /** Count of illegal marker operations observed since init (0 == balanced). */
  static markerError() { return _fn.markerError(); }

  // ---- per-trace ----

  /** Allocate a trace handle. Block recording when `blocks` > 0, instruction
   *  recording when `instructions` > 0 (asmtest_trace_new takes insns first). */
  static create(blocks = 64, instructions = 0) {
    const h = _fn.traceNew(instructions, blocks);
    if (!h) throw new Error('asmtest_trace_new failed');
    return new NativeTrace(h);
  }

  /** Register a non-overlapping native code range under `name`, recording
   *  coverage into this trace. */
  register(name, code) {
    const rc = _fn.registerRegion(name, code.base, code.length, this._handle);
    if (rc !== ASMTEST_DR_OK) throw new Error(`register_region(${name}) failed: ${rc}`);
    return this;
  }

  unregister(name) { _fn.unregisterRegion(name); }

  /** Open recording for `name`, run `fn`, then close recording — markers must be
   *  balanced, so end runs in a finally even if `fn` throws. */
  region(name, fn) {
    _fn.traceBegin(name);
    try {
      return fn();
    } finally {
      _fn.traceEnd(name);
    }
  }

  /** True if the basic block at byte-offset `off` was entered. */
  covered(off) { return _fn.traceCovered(this._handle, off) !== 0; }

  /** Number of distinct basic blocks recorded. */
  blocksLen() { return Number(_fn.blocksLen(this._handle)); }

  /** Total instructions in the ordered instruction stream (instruction mode). */
  insnsTotal() { return Number(_fn.insnsTotal(this._handle)); }

  free() {
    if (this._handle) {
      _fn.traceFree(this._handle);
      this._handle = null;
    }
  }
}

module.exports = { NativeTrace, NativeCode, ASMTEST_DR_OK };
