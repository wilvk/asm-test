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

// asmtest_ptrace.h — out-of-process / foreign-process tracing status codes
const ASMTEST_PTRACE_OK = 0;
const ASMTEST_PTRACE_ENOENT = -7; // region / symbol / method not found

// asmtest_codeimage.h — time-aware code-image recorder status codes + event kinds
const ASMTEST_CI_OK = 0;
const ASMTEST_CI_ENOENT = -7; // address never tracked / no version at-or-before when
// asmtest_codeimage_event_t kind field
const ASMTEST_CI_KIND_MPROTECT = 1;
const ASMTEST_CI_KIND_MMAP = 2;
const ASMTEST_CI_KIND_MEMFD = 3;

// koffi truncates a JS Number passed to a `const void*` parameter via Int32Value
// (32-bit), so a real 64-bit address (JIT code at ~0x7f..) would be silently
// mangled. Route any numeric address through BigInt, whose lossless Int64Value
// path preserves the full 64 bits. External pointers, BigInt, and null/undefined
// pass through unchanged.
function _addr(v) {
  return typeof v === 'number' ? BigInt(v) : v;
}

// The native-payload slot the published npm package ships (mirrors asmtest.js's
// core loader): native/<os>-<arch>/lib<name>.<ext> next to this module.
function bundledSlot(name) {
  const os = process.platform === 'darwin' ? 'darwin' : 'linux';
  const arch = process.arch === 'arm64' ? 'arm64' : 'x86_64'; // node 'x64' -> x86_64
  const ext = process.platform === 'darwin' ? 'dylib' : 'so';
  return path.join(__dirname, 'native', `${os}-${arch}`, `${name}.${ext}`);
}

// Ordered candidate paths for libasmtest_hwtrace: an explicit ASMTEST_HWTRACE_LIB
// wins (dev / custom build); then the native payload bundled in the published
// package at native/<os>-<arch>/ next to this module; then the dev build/ tree
// next to the repo root; then a bare name for the system loader. The bundled slot
// is tried BEFORE the dev build/ tree so an installed package never prefers a
// leaked checkout. The tier may be absent, so the load is wrapped in try/catch and
// available() self-skips cleanly when it can't resolve.
function hwtraceCandidates() {
  const ext = process.platform === 'darwin' ? 'dylib' : 'so';
  const cands = [];
  if (process.env.ASMTEST_HWTRACE_LIB) cands.push(process.env.ASMTEST_HWTRACE_LIB);
  cands.push(bundledSlot('libasmtest_hwtrace'));
  // bindings/node/ -> repo root is two levels up.
  cands.push(path.join(__dirname, '..', '..', 'build', `libasmtest_hwtrace.${ext}`));
  cands.push(`libasmtest_hwtrace.${ext}`);
  return cands;
}

// Absolute path of the libasmtest_hwtrace this process resolved (null until a
// successful load), captured for libraryPath().
let _resolvedPath = null;

// koffi struct layout mirroring the C API (the one place we mirror a C struct;
// the rest of the surface is opaque handles, like asmtest.js).
const Options = koffi.struct('asmtest_hwtrace_options_t', {
  backend: 'int',
  aux_size: 'size_t',
  data_size: 'size_t',
  snapshot: 'int',
  object_hint: 'str',
});

// koffi struct layout mirroring asmtest_codeimage_event_t (40 bytes): three
// uint64s then three uint32s and an int32, all naturally packed.
const CodeImageEvent = koffi.struct('asmtest_codeimage_event_t', {
  addr: 'uint64',
  len: 'uint64',
  timestamp: 'uint64',
  pid: 'uint32',
  tid: 'uint32',
  kind: 'uint32',
  fd: 'int32',
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
  let lib = null;
  for (const cand of hwtraceCandidates()) {
    try {
      lib = koffi.load(cand);
      _resolvedPath = path.isAbsolute(cand) ? cand : path.resolve(cand);
      break;
    } catch (e) {
      _loadError = e;
    }
  }
  if (!lib) return;
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
    // asmtest_ptrace.h — out-of-process / foreign-process tracing toolkit. `pid` is
    // a C int; the (long*) args/result are marshalled as raw little-endian buffers
    // (readBigInt64LE/readInt32LE), exactly like resolve()/autoTier() above. The
    // jitdump entry is four consecutive uint64s read at offsets 0/8/16/24.
    ptraceAvailable: lib.func('int asmtest_ptrace_available()'),
    ptraceSkipReason: lib.func('void asmtest_ptrace_skip_reason(_Out_ char*, size_t)'),
    ptraceTraceCall: lib.func('int asmtest_ptrace_trace_call(const void*, size_t, const long*, int, _Out_ long*, void*)'),
    ptraceTraceAttached: lib.func('int asmtest_ptrace_trace_attached(int, const void*, size_t, _Out_ long*, void*)'),
    // Version-aware attach: traces a foreign region but resolves the bytes from a
    // code-image timeline (`img`) as of capture sequence `when`, not a late snapshot.
    ptraceTraceAttachedVersioned: lib.func('int asmtest_ptrace_trace_attached_versioned(int, const void*, size_t, void*, uint64_t, _Out_ long*, void*)'),
    ptraceRunTo: lib.func('int asmtest_ptrace_run_to(int, const void*)'),
    procRegionByAddr: lib.func('int asmtest_proc_region_by_addr(int, const void*, _Out_ void**, _Out_ size_t*)'),
    procPerfmapSymbol: lib.func('int asmtest_proc_perfmap_symbol(int, const char*, _Out_ void**, _Out_ size_t*)'),
    jitdumpFind: lib.func('int asmtest_jitdump_find(const char*, int, const char*, _Out_ uint8_t*, _Out_ uint8_t*, size_t, _Out_ size_t*)'),
    // asmtest_codeimage.h — time-aware code-image recorder (a userspace
    // PERF_RECORD_TEXT_POKE). The img handle is an opaque void*. bytes_at borrows
    // bytes owned by the timeline: it writes a const uint8_t* through `out` (read
    // as a pointer-to-pointer) and the available length through `out_len`. The
    // event drain pops one asmtest_codeimage_event_t (40 bytes) per next().
    codeimageAvailable: lib.func('int asmtest_codeimage_available()'),
    codeimageSkipReason: lib.func('void asmtest_codeimage_skip_reason(_Out_ char*, size_t)'),
    codeimageNew: lib.func('void* asmtest_codeimage_new(int)'),
    codeimageFree: lib.func('void asmtest_codeimage_free(void*)'),
    codeimageTrack: lib.func('int asmtest_codeimage_track(void*, const void*, size_t)'),
    codeimageRefresh: lib.func('int asmtest_codeimage_refresh(void*)'),
    codeimageNow: lib.func('uint64_t asmtest_codeimage_now(void*)'),
    codeimageBytesAt: lib.func('int asmtest_codeimage_bytes_at(void*, const void*, uint64_t, _Out_ void**, _Out_ size_t*)'),
    codeimageBpfAvailable: lib.func('int asmtest_codeimage_bpf_available()'),
    codeimageBpfSkipReason: lib.func('void asmtest_codeimage_bpf_skip_reason(_Out_ char*, size_t)'),
    codeimageWatchBpf: lib.func('int asmtest_codeimage_watch_bpf(void*)'),
    codeimagePollBpf: lib.func('int asmtest_codeimage_poll_bpf(void*, int)'),
    codeimageNext: lib.func('int asmtest_codeimage_next(void*, _Out_ asmtest_codeimage_event_t*)'),
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

  /** Absolute path of the libasmtest_hwtrace this process resolved, or null if the
   *  load failed. Lets a clean-room test assert the bundled tier — not a leaked
   *  build/ tree — satisfied the load. */
  static libraryPath() { return _resolvedPath; }

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

/** Out-of-process / foreign-process tracing (asmtest_ptrace.h): single-step a
 *  forked or externally-attached target out of band, and resolve the code region
 *  to trace from the OS — /proc/<pid>/maps, a JIT perf-map, or a binary jitdump.
 *  The managed-runtime path (JVM/.NET/Node on AMD, where Intel PT is unavailable
 *  and in-process DynamoRIO cannot seize the runtime's threads). Linux x86-64.
 *  Reuses the binding's NativeCode (executable bytes) and HwTrace (trace handle)
 *  objects; static methods are camelCase like the rest of the surface. */
class Ptrace {
  /** True if the out-of-process single-step tracer can run on this host. */
  static available() {
    if (!_lib) return false;
    return _fn.ptraceAvailable() !== 0;
  }

  /** Human-readable reason available() is false (or 'available'). */
  static skipReason() {
    if (!_lib) return _loadError ? `load failed: ${_loadError.message}` : 'load failed';
    const buf = Buffer.alloc(160);
    _fn.ptraceSkipReason(buf, buf.length);
    const nul = buf.indexOf(0);
    return buf.toString('utf8', 0, nul < 0 ? buf.length : nul);
  }

  /** Fork a tracee that calls `code` (a NativeCode) with up to six integer `args`,
   *  single-step it out of process, and fill `trace` (a HwTrace). Returns the
   *  routine's return value (the child's RAX at the ret) as a JS number. */
  static traceCall(code, codeLen, args, trace) {
    const n = args.length;
    // long*: pack each arg as a 64-bit little-endian signed integer.
    const argBuf = Buffer.alloc(8 * Math.max(n, 1));
    for (let i = 0; i < n; i++) argBuf.writeBigInt64LE(BigInt(args[i]), i * 8);
    const resultBuf = Buffer.alloc(8);
    const rc = _fn.ptraceTraceCall(code.base, codeLen, argBuf, n, resultBuf, trace._handle);
    if (rc !== ASMTEST_PTRACE_OK) throw new Error(`asmtest_ptrace_trace_call failed: ${rc}`);
    return Number(resultBuf.readBigInt64LE(0));
  }

  /** Trace a region in a SEPARATE, already-ptrace-stopped process (the caller owns
   *  PTRACE_ATTACH/DETACH). Reads the target's bytes via process_vm_readv. Returns
   *  the target's RAX at the ret as a JS number. */
  static traceAttached(pid, base, len, trace) {
    const resultBuf = Buffer.alloc(8);
    const rc = _fn.ptraceTraceAttached(pid, _addr(base), len, resultBuf, trace._handle);
    if (rc !== ASMTEST_PTRACE_OK) throw new Error(`asmtest_ptrace_trace_attached failed: ${rc}`);
    return Number(resultBuf.readBigInt64LE(0));
  }

  /** Like traceAttached, but reconstruct the region's bytes from a CodeImage
   *  timeline (`img`) as of capture sequence `when` (0 => latest) instead of a
   *  late process_vm_readv snapshot — the W2 fix for a JIT method whose address
   *  was patched or reused mid-trace. Returns the target's RAX at the ret. */
  static traceAttachedVersioned(pid, base, len, img, when, trace) {
    const resultBuf = Buffer.alloc(8);
    const rc = _fn.ptraceTraceAttachedVersioned(
      pid, _addr(base), len, img ? img._handle : null, BigInt(when), resultBuf, trace._handle);
    if (rc !== ASMTEST_PTRACE_OK) {
      throw new Error(`asmtest_ptrace_trace_attached_versioned failed: ${rc}`);
    }
    return Number(resultBuf.readBigInt64LE(0));
  }

  /** Run an already-attached, ptrace-stopped target forward until it reaches `addr`
   *  (a software breakpoint that fires when the program itself next calls in),
   *  leaving it stopped there ready for traceAttached — the step that makes a resolved
   *  JIT method traceable when you don't control call timing. Returns the status code
   *  (ASMTEST_PTRACE_OK, or ASMTEST_PTRACE_ENOENT if the target exited first). The
   *  caller owns PTRACE_ATTACH/DETACH. */
  static runTo(pid, addr) {
    return _fn.ptraceRunTo(pid, _addr(addr));
  }

  /** The executable mapping in /proc/<pid>/maps containing `addr`, as
   *  { base, len }, or null if no executable mapping contains it. */
  static procRegionByAddr(pid, addr) {
    const baseOut = [null];
    const lenOut = [0];
    const rc = _fn.procRegionByAddr(pid, _addr(addr), baseOut, lenOut);
    if (rc !== ASMTEST_PTRACE_OK) return null;
    return { base: baseOut[0], len: Number(lenOut[0]) };
  }

  /** A JIT method by `name` in /tmp/perf-<pid>.map, as { base, len }, or null. */
  static procPerfmapSymbol(pid, name) {
    const baseOut = [null];
    const lenOut = [0];
    const rc = _fn.procPerfmapSymbol(pid, name, baseOut, lenOut);
    if (rc !== ASMTEST_PTRACE_OK) return null;
    return { base: baseOut[0], len: Number(lenOut[0]) };
  }

  /** A JIT method by `name` from a jitdump (`path`, or /tmp/jit-<pid>.dump when
   *  path is null) as { codeAddr, codeSize, timestamp, codeIndex, code }, carrying
   *  up to `wantBytes` of recorded code (a Buffer), or null. The latest re-JIT body
   *  (highest timestamp) wins. The entry is four consecutive uint64s (offsets
   *  0/8/16/24). codeAddr/codeSize are returned as BigInt so a real 64-bit JIT
   *  address survives intact and feeds runTo/traceAttached/track without truncation;
   *  timestamp/codeIndex stay JS numbers (logical counters). */
  static jitdumpFind(path, name, pid = 0, wantBytes = 0) {
    const entry = Buffer.alloc(32); // asmtest_jitdump_entry_t: 4 x uint64
    const codeBuf = wantBytes ? Buffer.alloc(wantBytes) : null;
    const lenOut = [0]; // size_t* out — koffi writes the byte count back here
    const rc = _fn.jitdumpFind(path, pid, name, entry, codeBuf, wantBytes,
      wantBytes ? lenOut : null);
    if (rc !== ASMTEST_PTRACE_OK) return null;
    const n = wantBytes ? Number(lenOut[0]) : 0;
    return {
      codeAddr: entry.readBigUInt64LE(0),
      codeSize: entry.readBigUInt64LE(8),
      timestamp: Number(entry.readBigUInt64LE(16)),
      codeIndex: Number(entry.readBigUInt64LE(24)),
      code: wantBytes ? Buffer.from(codeBuf.subarray(0, n)) : Buffer.alloc(0),
    };
  }
}

/** A time-aware code-image recorder (asmtest_codeimage.h): a userspace
 *  PERF_RECORD_TEXT_POKE. track() snapshots a region's bytes (version 0) and arms
 *  write-protect; refresh() re-snapshots only changed pages as new versions stamped
 *  with a monotonic logical timestamp; bytesAt(addr, when) answers "what bytes were
 *  live at addr as of sequence `when`" — the query a branch-trace decoder needs to
 *  reconstruct a JIT method whose address was patched, freed, or reused mid-trace.
 *  Records THIS process (pid 0) or a foreign one. Linux. Reuses koffi external
 *  pointers; static probes are camelCase like the rest of the surface. */
class CodeImage {
  constructor(pid = 0) {
    this._handle = _fn.codeimageNew(pid);
    if (!this._handle) throw new Error('asmtest_codeimage_new failed');
  }

  /** True if the userspace page-change recorder can run on this host (PAGEMAP_SCAN
   *  or the soft-dirty fallback). False on load failure. Self-skip otherwise. */
  static available() {
    if (!_lib) return false;
    return _fn.codeimageAvailable() !== 0;
  }

  /** Human-readable reason available() is false (or 'available'). */
  static skipReason() {
    if (!_lib) return _loadError ? `load failed: ${_loadError.message}` : 'load failed';
    const buf = Buffer.alloc(160);
    _fn.codeimageSkipReason(buf, buf.length);
    const nul = buf.indexOf(0);
    return buf.toString('utf8', 0, nul < 0 ? buf.length : nul);
  }

  /** True if the optional eBPF emission detector (Phase C) can load and attach on
   *  this host (libbpf, BTF, privilege). False on load failure. */
  static bpfAvailable() {
    if (!_lib) return false;
    return _fn.codeimageBpfAvailable() !== 0;
  }

  /** Human-readable reason bpfAvailable() is false (or 'available'). */
  static bpfSkipReason() {
    if (!_lib) return _loadError ? `load failed: ${_loadError.message}` : 'load failed';
    const buf = Buffer.alloc(160);
    _fn.codeimageBpfSkipReason(buf, buf.length);
    const nul = buf.indexOf(0);
    return buf.toString('utf8', 0, nul < 0 ? buf.length : nul);
  }

  /** Begin tracking [base, base+len): snapshot version 0 and arm write-protect on
   *  its pages. `base` may be a NativeCode's external pointer or a numeric/BigInt
   *  address. Returns the status code (ASMTEST_CI_OK on success). */
  track(base, len) {
    return _fn.codeimageTrack(this._handle, _addr(base), len);
  }

  /** Scan tracked ranges for changed pages and re-snapshot each as a new version.
   *  Returns the number of new versions recorded (>= 0), or a negative status. */
  refresh() {
    return _fn.codeimageRefresh(this._handle);
  }

  /** The current capture sequence — a monotonic logical timestamp. 0 before
   *  anything is tracked, advancing by one per version recorded. */
  now() {
    return Number(_fn.codeimageNow(this._handle));
  }

  /** The bytes live at `addr` as of capture sequence `when` (0 => latest) as a
   *  Buffer (a copy of the borrowed timeline bytes), or null on ASMTEST_CI_ENOENT /
   *  any non-OK status. `addr` may be a NativeCode external pointer or a numeric
   *  address. The C side writes a const uint8_t* through `out` and the available
   *  length through `out_len`; koffi decodes the borrowed pointer into a Buffer. */
  bytesAt(addr, when = 0) {
    const outPtr = [null];
    const outLen = [0];
    const rc = _fn.codeimageBytesAt(this._handle, _addr(addr), BigInt(when), outPtr, outLen);
    if (rc !== ASMTEST_CI_OK) return null;
    const n = Number(outLen[0]);
    if (!outPtr[0] || n === 0) return Buffer.alloc(0);
    // outPtr[0] is a borrowed pointer owned by the timeline (valid until free()).
    // Decode n bytes out of it and copy into an owned Buffer.
    const view = koffi.decode(outPtr[0], 'uint8_t', n);
    return Buffer.from(view);
  }

  /** Load and attach the eBPF emission detector, filtered to this image's pid.
   *  Subsequent pollBpf() drains events. Returns the status code (0 on success). */
  watchBpf() {
    return _fn.codeimageWatchBpf(this._handle);
  }

  /** Drain ready emission events from the BPF ring buffer into the internal queue.
   *  timeout_ms == 0 is a non-blocking drain. Returns the number queued (>= 0) or a
   *  negative status. */
  pollBpf(timeoutMs = 0) {
    return _fn.codeimagePollBpf(this._handle, timeoutMs);
  }

  /** Pop one queued emission event as { addr, len, timestamp, pid, tid, kind, fd },
   *  or null when the queue is empty (or on a negative status). */
  nextEvent() {
    const ev = {};
    const rc = _fn.codeimageNext(this._handle, ev);
    if (rc !== 1) return null;
    return {
      addr: Number(ev.addr),
      len: Number(ev.len),
      timestamp: Number(ev.timestamp),
      pid: ev.pid,
      tid: ev.tid,
      kind: ev.kind,
      fd: ev.fd,
    };
  }

  /** Free the timeline and all recorded versions; detaches any eBPF watch. */
  free() {
    if (this._handle) {
      _fn.codeimageFree(this._handle);
      this._handle = null;
    }
  }
}

module.exports = {
  HwTrace, NativeCode, Ptrace, CodeImage,
  ASMTEST_HW_OK, ASMTEST_HW_EUNAVAIL, INTEL_PT, CORESIGHT, AMD_LBR, SINGLESTEP,
  BEST, CEILING_FREE,
  TIER_HWTRACE, TIER_DYNAMORIO, TIER_EMULATOR,
  FIDELITY_NATIVE, FIDELITY_VIRTUAL,
  TRACE_BEST, TRACE_CEILING_FREE, TRACE_NATIVE_ONLY,
  ASMTEST_PTRACE_OK, ASMTEST_PTRACE_ENOENT,
  ASMTEST_CI_OK, ASMTEST_CI_ENOENT,
  ASMTEST_CI_KIND_MPROTECT, ASMTEST_CI_KIND_MMAP, ASMTEST_CI_KIND_MEMFD,
};
