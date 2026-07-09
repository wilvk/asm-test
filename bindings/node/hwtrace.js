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

// asmtest_descent_level_t — call-descent policy (see the Descent class). Decides
// what the ptrace stepper does at each call-out it would otherwise step over.
const DESCENT_OFF = 0;           // step over, record nothing (today's behaviour)
const DESCENT_RECORD_EDGES = 1;  // record (call-site -> callee) edges, still step over
const DESCENT_DESCEND_KNOWN = 2; // step INTO resolvable calls (allow-set / resolver)
const DESCENT_DESCEND_ALL = 3;   // step INTO everything (denylist + budget + watchdog gated)

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

// Read a captured 64-bit return value exactly: a JS number when it fits the safe-integer
// range, else the BigInt (a full 64-bit hash/id/pointer in RAX above 2^53 would silently
// round through Number()). Preserves the "exact result" guarantee of the OOP capture forms.
function _safeInt(bi) {
  return (bi >= -(2n ** 53n) && bi <= 2n ** 53n) ? Number(bi) : bi;
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

// koffi struct layout mirroring asmtest_hwtrace_scope_t (8 bytes): an opaque
// per-scope capture handle — a TLS range-stack index tagged with a generation
// counter. asmtest_hwtrace_call_scoped_ex fills one through an _Out_ pointer;
// asmtest_hwtrace_render_scope takes it BY VALUE (a two-uint32 8-byte struct is a
// single INTEGER-class eightbyte on SysV x86-64, passed in one register, which koffi
// marshals straight from the koffi.struct — no manual packing needed).
const HwScope = koffi.struct('asmtest_hwtrace_scope_t', {
  idx: 'uint32',
  gen: 'uint32',
});

// The generated code is invoked through this function-pointer prototype: each
// argument is passed as a C long and the result read as a long (the SysV integer
// ABI), matching the Python wrapper's default call().
const Fn2 = koffi.proto('long asmtest_native_fn(long, long)');

// Call-descent upcall prototypes (asmtest_ptrace.h). The tracer invokes these
// mid-single-step to decide whether to descend into a call-out. A JS closure is
// bound to one via koffi.register (a PERSISTENT trampoline, unlike a transient
// callback) and retained on the Descent object (Descent._cb) so the GC cannot
// collect it while a descended trace is running. The callee address arrives as a
// uint64_t; the resolver writes the callee region back through two uint64_t*
// out-params (koffi.encode) and returns 1 to descend / 0 to step over.
const ResolverFn = koffi.proto(
  'int asmtest_descent_resolver_fn(uint64_t callee, void *user, uint64_t *base_out, uint64_t *len_out)');
const DenylistFn = koffi.proto(
  'int asmtest_descent_denylist_fn(uint64_t callee, void *user)');

// §D3 stealth stepper upcall (asmtest_hwtrace_stealth_trace): the caller-supplied
// `run_region(arg)` the helper child drives to invoke the leaf while it single-steps
// it out of band. Non-capturing at the C level (a plain void(void*)); the JS closure
// carries the code + args and is bound via koffi.register like the descent upcalls.
const RunRegionFn = koffi.proto('void asmtest_run_region(void *arg)');

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
    // Scoped-tracing shared core (§0/§1): error-returning begin + render-on-close.
    tryBegin: lib.func('int asmtest_hwtrace_try_begin(const char*)'),
    render: lib.func('int asmtest_hwtrace_render(const char*, _Out_ char*, size_t)'),
    // Registry-free lazy-arm call + handle-keyed render (call_scoped_ex path). The
    // region is given DIRECTLY as [base,len) with a caller-owned trace, so NO named
    // region is registered and NO MAX_REGIONS slot is consumed — safe in a tight loop.
    // args is an in-array of C longs (Buffer, like the ptrace path); result_out and the
    // 8-byte scope handle are _Out_. render_scope takes that scope handle BY VALUE.
    callScopedEx: lib.func('int asmtest_hwtrace_call_scoped_ex(void*, size_t, void*, void*, const long*, int, _Out_ long*, _Out_ asmtest_hwtrace_scope_t*)'),
    renderScope: lib.func('int asmtest_hwtrace_render_scope(asmtest_hwtrace_scope_t, _Out_ char*, size_t)'),
    // §Z0/§Z1 region-free whole-window scope (the empty-ctor `using (new AsmTrace())`).
    // begin_window arms with NO registered region; insns[] then hold ABSOLUTE addresses.
    // end_window/render_window take the 8-byte scope handle BY VALUE (render_scope-style).
    beginWindow: lib.func('int asmtest_hwtrace_begin_window(void*, _Out_ asmtest_hwtrace_scope_t*)'),
    endWindow: lib.func('int asmtest_hwtrace_end_window(asmtest_hwtrace_scope_t, void*)'),
    renderWindow: lib.func('int asmtest_hwtrace_render_window(asmtest_hwtrace_scope_t, _Out_ char*, size_t)'),
    // §D3 concealed out-of-process ptrace-stealth stepper. A helper CHILD reverse-attaches
    // to THIS process and single-steps the region [base,len) while `run_region(arg)` runs it
    // — so NO EFLAGS.TF is armed on the calling (V8) thread. The crash-proof capture path on a
    // no-PT host (Zen 2, Docker-on-Mac). Fills the caller-owned trace* (region-RELATIVE
    // offsets) + result_out (the region's return, read from the caller's RAX at the ret).
    stealthTrace: lib.func('int asmtest_hwtrace_stealth_trace(const void*, size_t, void*, _Out_ long*, asmtest_run_region*, void*)'),
    // §D3 WHOLE-WINDOW OOP capture. The addr_channel (asmtest_addr_channel.h) is an opaque
    // handle — the caller pre-publishes the code regions the window frame calls INTO (the
    // leaves/JIT methods beyond the frame itself), so the out-of-process stepper — which cannot
    // see a runtime's own JIT events — records them and steps over everything else. new() is a
    // process-local ring (for the fork-internal window_call); new_shared() is MAP_SHARED (for
    // the reverse-attach stepper, whose forked child drains it live).
    addrChannelNew: lib.func('void* asmtest_addr_channel_new()'),
    addrChannelNewShared: lib.func('void* asmtest_addr_channel_new_shared()'),
    addrChannelPublishRec: lib.func('void asmtest_addr_channel_publish_rec(void*, uint64_t, uint64_t, uint64_t)'),
    addrChannelFree: lib.func('void asmtest_addr_channel_free(void*)'),
    addrChannelFreeShared: lib.func('void asmtest_addr_channel_free_shared(void*)'),
    // window_call FORKS a child that runs code(args...) as the window frame; stealth_windowed
    // REVERSE-ATTACHES and steps the calling thread's window body (run_region invokes the frame).
    // Both record the frame [base,len) + every channel-published region as ABSOLUTE addresses.
    windowCall: lib.func('int asmtest_ptrace_trace_window_call(const void*, size_t, const long*, int, void*, _Out_ long*, void*)'),
    stealthWindowed: lib.func('int asmtest_hwtrace_stealth_trace_windowed(const void*, size_t, void*, void*, _Out_ long*, asmtest_run_region*, void*)'),
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
    ptraceBlockstepAvailable: lib.func('int asmtest_ptrace_blockstep_available()'),
    ptraceTraceCallBlockstep: lib.func('int asmtest_ptrace_trace_call_blockstep(const void*, size_t, const long*, int, _Out_ long*, void*)'),
    ptraceTraceAttachedBlockstep: lib.func('int asmtest_ptrace_trace_attached_blockstep(int, const void*, size_t, _Out_ long*, void*)'),
    ptraceTraceAttached: lib.func('int asmtest_ptrace_trace_attached(int, const void*, size_t, _Out_ long*, void*)'),
    // Version-aware attach: traces a foreign region but resolves the bytes from a
    // code-image timeline (`img`) as of capture sequence `when`, not a late snapshot.
    ptraceTraceAttachedVersioned: lib.func('int asmtest_ptrace_trace_attached_versioned(int, const void*, size_t, void*, uint64_t, _Out_ long*, void*)'),
    ptraceRunTo: lib.func('int asmtest_ptrace_run_to(int, const void*)'),
    procRegionByAddr: lib.func('int asmtest_proc_region_by_addr(int, const void*, _Out_ void**, _Out_ size_t*)'),
    procPerfmapSymbol: lib.func('int asmtest_proc_perfmap_symbol(int, const char*, _Out_ void**, _Out_ size_t*)'),
    jitdumpFind: lib.func('int asmtest_jitdump_find(const char*, int, const char*, _Out_ uint8_t*, _Out_ uint8_t*, size_t, _Out_ size_t*)'),
    // asmtest_ptrace.h — call descent (asmtest_descent_t): edges + nested frames.
    // The address-returning accessors (edge_target, frame_base, frame_len,
    // frame_insn_at, frame_block_at) are declared uint64_t and surfaced as JS
    // BigInt by the Descent wrapper — a >2^53 ASLR address rounds as a Number.
    // set_resolver/set_denylist take a koffi-registered upcall pointer (see the
    // ResolverFn/DenylistFn protos and Descent.setResolver/setDenylist).
    descentNew: lib.func('void* asmtest_descent_new(int)'),
    descentFree: lib.func('void asmtest_descent_free(void*)'),
    descentSetMaxDepth: lib.func('void asmtest_descent_set_max_depth(void*, uint32_t)'),
    descentSetInsnBudget: lib.func('void asmtest_descent_set_insn_budget(void*, uint64_t)'),
    descentSetWatchdogMs: lib.func('void asmtest_descent_set_watchdog_ms(void*, uint32_t)'),
    descentUseDefaultDenylist: lib.func('void asmtest_descent_use_default_denylist(void*)'),
    descentAllowRegion: lib.func('int asmtest_descent_allow_region(void*, const void*, size_t)'),
    descentDenyRegion: lib.func('int asmtest_descent_deny_region(void*, const void*, size_t)'),
    descentSetResolver: lib.func('void asmtest_descent_set_resolver(void*, asmtest_descent_resolver_fn*, void*)'),
    descentSetDenylist: lib.func('void asmtest_descent_set_denylist(void*, asmtest_descent_denylist_fn*, void*)'),
    descentEdgesLen: lib.func('size_t asmtest_descent_edges_len(void*)'),
    descentEdgeSite: lib.func('uint64_t asmtest_descent_edge_site(void*, size_t)'),
    descentEdgeTarget: lib.func('uint64_t asmtest_descent_edge_target(void*, size_t)'),
    descentEdgeDepth: lib.func('uint32_t asmtest_descent_edge_depth(void*, size_t)'),
    descentFramesLen: lib.func('size_t asmtest_descent_frames_len(void*)'),
    descentFrameBase: lib.func('uint64_t asmtest_descent_frame_base(void*, size_t)'),
    descentFrameLen: lib.func('uint64_t asmtest_descent_frame_len(void*, size_t)'),
    descentFrameDepth: lib.func('uint32_t asmtest_descent_frame_depth(void*, size_t)'),
    descentFrameParent: lib.func('int32_t asmtest_descent_frame_parent(void*, size_t)'),
    descentFrameInsnCount: lib.func('size_t asmtest_descent_frame_insn_count(void*, size_t)'),
    descentFrameInsnAt: lib.func('uint64_t asmtest_descent_frame_insn_at(void*, size_t, size_t)'),
    descentFrameBlockCount: lib.func('size_t asmtest_descent_frame_block_count(void*, size_t)'),
    descentFrameBlockAt: lib.func('uint64_t asmtest_descent_frame_block_at(void*, size_t, size_t)'),
    descentTruncated: lib.func('int asmtest_descent_truncated(void*)'),
    descentDepthCapped: lib.func('int asmtest_descent_depth_capped(void*)'),
    // Descending variants of the three trace entry points — each threads a descent
    // handle through the stepper (descent == NULL reproduces the non-_ex forms).
    ptraceTraceCallEx: lib.func('int asmtest_ptrace_trace_call_ex(const void*, size_t, const long*, int, _Out_ long*, void*, void*)'),
    ptraceTraceAttachedEx: lib.func('int asmtest_ptrace_trace_attached_ex(int, const void*, size_t, _Out_ long*, void*, void*)'),
    ptraceTraceAttachedVersionedEx: lib.func('int asmtest_ptrace_trace_attached_versioned_ex(int, const void*, size_t, void*, uint64_t, _Out_ long*, void*, void*)'),
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

/** The §D3 cross-process JIT-address channel (asmtest_addr_channel.h). A whole-window
 *  out-of-process capture (`Ptrace.windowCall` / `HwTrace.stealthWindow`) records the window
 *  FRAME plus every region you PRE-PUBLISH here — the leaves/methods the frame calls into,
 *  which the out-of-process stepper cannot otherwise discover (it does not see the runtime's
 *  own JIT events). Everything else the frame steps through (runtime glue) is elided.
 *  `newLocal()` is a process-local ring for the fork-internal `windowCall`; `newShared()` is a
 *  MAP_SHARED ring for the reverse-attach `stealthWindow` (the forked stepper drains it live).
 *  Mirrors dotnet's `HwTrace.AddrChannel`. */
class AddrChannel {
  constructor(handle, shared) { this._handle = handle; this._shared = shared; }

  /** A process-local channel (for the fork-internal `Ptrace.windowCall`). */
  static newLocal() {
    const h = _fn.addrChannelNew();
    if (!h) throw new Error('asmtest_addr_channel_new failed');
    return new AddrChannel(h, false);
  }

  /** A MAP_SHARED channel (for the reverse-attach `HwTrace.stealthWindow`). */
  static newShared() {
    const h = _fn.addrChannelNewShared();
    if (!h) throw new Error('asmtest_addr_channel_new_shared failed');
    return new AddrChannel(h, true);
  }

  /** Publish one code region the window calls into: either a `NativeCode` (its whole
   *  allocation) or an explicit `(base, len)`, with an optional code-image `version`
   *  (0 = untracked). Returns `this` for chaining. */
  publish(codeOrBase, len, version = 0) {
    let base = codeOrBase, l = len;
    if (codeOrBase && typeof codeOrBase === 'object' && codeOrBase._base !== undefined) {
      base = koffi.address(codeOrBase.base); // a NativeCode
      l = codeOrBase.length;
    }
    _fn.addrChannelPublishRec(this._handle, BigInt(base), BigInt(l), BigInt(version));
    return this;
  }

  /** Free the channel (routes to the process-local vs shared native free). */
  free() {
    if (this._handle) {
      if (this._shared) _fn.addrChannelFreeShared(this._handle);
      else _fn.addrChannelFree(this._handle);
      this._handle = null;
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
   *  drops the ceiling-bounded backend (AMD LBR). Returns an array of backend ints. */
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

  /** Trace ONE native call the managed-safe way: arm the single-step window, call
   *  `code(...args)` through the SysV integer ABI, and disarm — all in native code
   *  (asmtest_hwtrace_call_scoped_ex) — so nothing the binding runs between arm and
   *  disarm is stepped, a tighter window than scope()'s (where code.call's FFI dispatch
   *  runs out-of-region but stepped). REGISTRY-FREE: it takes the region as [base,len)
   *  with its own caller-owned trace, consuming no MAX_REGIONS slot, so it is safe in a
   *  tight loop. For a native leaf `fn == base`. Args pass as C longs (0-6). Requires an
   *  initialized single-step tier (HwTrace.init). Returns `{ result, path, truncated }`:
   *  `result` the call's return (a JS number, BigInt out of safe range), `path` the
   *  executed body's disassembly ('' when the decoder is absent), `truncated` the
   *  honesty bit. On a non-single-step backend (rc != OK) returns result null. */
  static callScoped(code, ...args) {
    // Own trace handle (Python parity: instructions=256, blocks=64); freed in finally.
    const handle = _fn.traceNew(256, 64);
    if (!handle) throw new Error('asmtest_trace_new failed');
    try {
      const n = args.length;
      // long*: pack each arg as a 64-bit little-endian signed integer (ptrace path).
      const argBuf = Buffer.alloc(8 * Math.max(n, 1));
      for (let i = 0; i < n; i++) argBuf.writeBigInt64LE(BigInt(args[i]), i * 8);
      const resultBuf = Buffer.alloc(8);
      const scope = {}; // _Out_ asmtest_hwtrace_scope_t — koffi fills idx/gen
      const rc = _fn.callScopedEx(code.base, code.length, handle, code.base,
        argBuf, n, resultBuf, scope);
      if (rc !== ASMTEST_HW_OK) return { result: null, path: '', truncated: false };
      // Render the body from the just-captured (thread-local) scope handle, then read
      // the honesty bit off the trace before freeing it.
      const path = _renderScope(scope);
      const truncated = _fn.truncated(handle) !== 0;
      return { result: _safeInt(resultBuf.readBigInt64LE(0)), path, truncated };
    } finally {
      _fn.traceFree(handle);
    }
  }

  /** Trace an ARBITRARY block of native work the region-free (empty-ctor) way — the
   *  §Z1 whole-window scope, the callback form of dotnet's `using (new AsmTrace())`.
   *  Arms a REGION-FREE single-step capture on THIS thread (no NativeCode, no
   *  [base,len)), runs `fn`, disarms, and renders. Unlike `callScoped`'s clean 5-insn
   *  body this is HONEST-BUT-NOISY: it records EVERYTHING between begin and end (the
   *  FFI dispatch + harness too), so the traced routine's own ABSOLUTE addresses appear
   *  as a SUBSET of `insns` (surfaced as BigInt). Keep the window TIGHT — single-step
   *  steps every instruction. Returns `{ path, truncated, insns, armed }`; on a
   *  non-single-step backend (rc != OK) it self-skips: `fn` still runs, `armed` is
   *  false, `insns` is empty. Requires the tier to be up (`HwTrace.init`).
   *
   *  SAFETY: this arms EFLAGS.TF single-step on the CALLING (V8) thread, so `fn` must be a
   *  TIGHT native leaf (an FFI call), not an arbitrary managed block. A body that blocks
   *  SIGTRAP or spawns threads (`pthread_create` — glibc masks SIGTRAP around clone) turns
   *  the #DB into a fatal blocked signal and KILLS the process. To trace a whole block of
   *  arbitrary managed code, use the crash-proof out-of-process path (see
   *  docs/internal/plans/managed-wholewindow-oop-plan.md), never this in-process form.
   *
   *  `insnsCap` sizes the capture buffer. Single-stepping a managed runtime records a
   *  LOT — a single V8-dispatched call runs ~100k instructions — so the default is
   *  generous (~1M insns); if the window overflows it, `truncated` is set (an honest
   *  best-effort outcome) and the stored `insns` are a labelled PREFIX. */
  static window(fn, insnsCap = 1 << 20) {
    // whole-window is insns-only: insns_cap FIRST, blocks_cap=0 SECOND.
    const handle = _fn.traceNew(insnsCap, 0);
    if (!handle) throw new Error('asmtest_trace_new failed');
    try {
      const scope = {}; // _Out_ asmtest_hwtrace_scope_t — koffi fills idx/gen
      const rc = _fn.beginWindow(handle, scope);
      if (rc !== ASMTEST_HW_OK) { // EUNAVAIL/ESTATE/EFULL/EINVAL -> clean self-skip
        fn();
        return { path: '', truncated: false, insns: [], armed: false };
      }
      try {
        fn(); // keep TIGHT: EFLAGS.TF single-step is armed across this
      } finally {
        _fn.endWindow(scope, handle); // scope BY VALUE, then trace*
      }
      const path = _renderWindow(scope);
      const truncated = _fn.truncated(handle) !== 0;
      const n = Number(_fn.insnsLen(handle));
      const insns = new Array(n);
      for (let i = 0; i < n; i++) insns[i] = BigInt(_fn.insnAt(handle, i)); // ABSOLUTE addrs
      return { path, truncated, insns, armed: true };
    } finally {
      _fn.traceFree(handle);
    }
  }

  /** Trace ONE native leaf the CRASH-PROOF out-of-process way: a helper child
   *  reverse-attaches to this process and single-steps the region [base,len) while we
   *  run `code.call(a, b)` — so NO EFLAGS.TF is ever armed on the calling (V8) thread
   *  (asmtest_hwtrace_stealth_trace). This is the safe counterpart to `callScoped`/`window`
   *  for a host with no PT/LBR (Zen 2, Docker-on-Mac): the in-process single-step tier is
   *  forbidden against a managed runtime (a body that blocks SIGTRAP or spawns a thread
   *  KILLS the process), whereas a ptrace-stop is not gated by the tracee's signal mask, so
   *  the body survives. Mirrors dotnet's `AsmTrace.Method(..., outOfProcess: true)`.
   *
   *  The `result` is EXACT (the helper reads the caller's RAX at the ret), but the instruction
   *  STREAM is best-effort over a live runtime: single-stepping the runtime's own thread can be
   *  interrupted by its async signals, so `truncated` may be set with a partial `offsets` — the
   *  same honest-degradation posture as `window()` and dotnet's out-of-process `AsmTrace.Method`.
   *
   *  Args pass as C longs, up to two (the `NativeCode.call` ceiling). Returns
   *  `{ result, offsets, blocks, truncated, armed }`: `result` the leaf's return (from the
   *  helper's RAX read, a JS number / BigInt out of safe range), `offsets` the executed
   *  body's region-RELATIVE instruction offsets, `blocks` the basic-block count, `truncated`
   *  the honesty bit. On a refused reverse-attach (Yama `ptrace_scope`, no ptrace, or
   *  off-x86-64-Linux) it self-skips: `armed` is false, `offsets` empty — but the call STILL
   *  RUNS (never a silent miss), exactly like dotnet's stealth path. Needs no `HwTrace.init`
   *  (the stealth stepper is ptrace-based, independent of the single-step tier). */
  static stealthTrace(code, a = 0, b = 0) {
    const handle = _fn.traceNew(256, 64);
    if (!handle) throw new Error('asmtest_trace_new failed');
    // Persistent trampoline (koffi.register, not a transient callback) so the helper can
    // call back into JS mid-step; the closure invokes the leaf so control enters [base,len).
    let ran = false;
    const thunk = () => { ran = true; try { code.call(a, b); } catch (_e) { /* result_out is authoritative */ } };
    const cb = koffi.register(thunk, koffi.pointer(RunRegionFn));
    try {
      const resultBuf = Buffer.alloc(8);
      const rc = _fn.stealthTrace(code.base, code.length, handle, resultBuf, cb, null);
      if (rc !== ASMTEST_HW_OK) { // EUNAVAIL/EINVAL/ENOSYS -> clean self-skip
        if (!ran) { try { code.call(a, b); } catch (_e) { /* never lose the call itself */ } }
        return { result: null, offsets: [], blocks: 0, truncated: false, armed: false };
      }
      const n = Number(_fn.insnsLen(handle));
      const offsets = new Array(n);
      for (let i = 0; i < n; i++) offsets[i] = Number(_fn.insnAt(handle, i)); // region-RELATIVE
      return {
        result: _safeInt(resultBuf.readBigInt64LE(0)),
        offsets,
        blocks: Number(_fn.blocksLen(handle)),
        truncated: _fn.truncated(handle) !== 0,
        armed: true,
      };
    } finally {
      koffi.unregister(cb);
      _fn.traceFree(handle);
    }
  }

  /** Trace an ARBITRARY WHOLE WINDOW of native work the CRASH-PROOF out-of-process way — the
   *  reverse-attach analog of `window()`, mirroring dotnet's `AsmTrace.Window`. A helper child
   *  reverse-attaches and single-steps the window body out of band while THIS thread runs it,
   *  so NO EFLAGS.TF is armed on the V8 thread (unlike in-process `window()`, which steps the
   *  calling thread and is fatal for arbitrary managed code that blocks SIGTRAP / spawns a
   *  thread). `driver` is a NativeCode — the WINDOW FRAME whose RETURN delimits the window;
   *  `channel` is an AddrChannel (use `newShared()`) pre-published with the leaves the frame
   *  calls into. Records the frame [base,len) PLUS every published region as ABSOLUTE addresses;
   *  runtime/glue between them is stepped over, not recorded. (asmtest_hwtrace_stealth_trace_windowed.)
   *  Returns `{ result, insns, truncated, armed }`: `result` the frame's return (from the
   *  helper's RAX read — a JS number, BigInt out of safe range), `insns` the captured ABSOLUTE
   *  addresses (BigInt), `truncated` the honesty bit. Self-skips (armed:false, empty insns)
   *  where the reverse-attach is refused
   *  (Yama ptrace_scope) — but the body STILL RUNS (never a silent miss). Needs no HwTrace.init. */
  static stealthWindow(driver, channel) {
    const handle = _fn.traceNew(1 << 16, 4096); // whole-window: generous insns cap
    if (!handle) throw new Error('asmtest_trace_new failed');
    // Persistent trampoline: run_region invokes the driver (the window frame) so the helper's
    // single-step of [win_base,win_len) begins at the frame entry.
    let ran = false;
    const thunk = () => { ran = true; try { driver.call(0, 0); } catch (_e) { /* result_out is authoritative */ } };
    const cb = koffi.register(thunk, koffi.pointer(RunRegionFn));
    try {
      const resultBuf = Buffer.alloc(8);
      const rc = _fn.stealthWindowed(driver.base, driver.length, channel._handle, handle, resultBuf, cb, null);
      if (rc !== ASMTEST_HW_OK) { // EUNAVAIL/EINVAL/ENOSYS -> clean self-skip
        if (!ran) { try { driver.call(0, 0); } catch (_e) { /* never lose the call itself */ } }
        return { result: null, insns: [], truncated: false, armed: false };
      }
      const n = Number(_fn.insnsLen(handle));
      const insns = new Array(n);
      for (let i = 0; i < n; i++) insns[i] = BigInt(_fn.insnAt(handle, i)); // ABSOLUTE addrs
      return {
        result: _safeInt(resultBuf.readBigInt64LE(0)),
        insns,
        truncated: _fn.truncated(handle) !== 0,
        armed: true,
      };
    } finally {
      koffi.unregister(cb);
      _fn.traceFree(handle);
    }
  }

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

  /** A block scope over the register-then-begin/end pair with the shared-core
   *  render-on-close — the callback form of the *import + scope* surface (the
   *  version-independent fallback; the `using` + Symbol.dispose sugar needs Node 24+).
   *  `code` is the traced region; the name auto-generates as `basename:line` from the
   *  call site (override with `opts.name`). Renders the executed assembly on close and
   *  (when `opts.emit !== false`) writes it. The C core flags the trace `truncated` on
   *  a cross-thread close (§0.2/§1). Genuine cross-thread work (Worker / libuv pool) is
   *  a disclosed gap — untraced, flagged via the tid assert, not stitched. Returns
   *  `{ name, path, armed, truncated }`. `fn` runs even if it throws (end is finally'd). */
  scope(code, fn, opts = {}) {
    const emit = opts.emit !== false;
    const name = opts.name || _scopeName(new Error().stack);
    this.register(name, code); // register-then-begin (Core §0.4 idempotent-by-name)
    const armed = _fn.tryBegin(name) === ASMTEST_HW_OK; // nonzero -> clean self-skip
    try {
      fn();
    } finally {
      _fn.end(name);
    }
    const path = _renderRegion(name);
    if (emit && path) process.stdout.write(path);
    return { name, path, armed, truncated: this.truncated() };
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
   *  routine's return value (the child's RAX at the ret) as a JS number (BigInt
   *  out of safe range). */
  static traceCall(code, codeLen, args, trace) {
    const n = args.length;
    // long*: pack each arg as a 64-bit little-endian signed integer.
    const argBuf = Buffer.alloc(8 * Math.max(n, 1));
    for (let i = 0; i < n; i++) argBuf.writeBigInt64LE(BigInt(args[i]), i * 8);
    const resultBuf = Buffer.alloc(8);
    const rc = _fn.ptraceTraceCall(code.base, codeLen, argBuf, n, resultBuf, trace._handle);
    if (rc !== ASMTEST_PTRACE_OK) throw new Error(`asmtest_ptrace_trace_call failed: ${rc}`);
    return _safeInt(resultBuf.readBigInt64LE(0));
  }

  /** True if the BTF block-step variant (PTRACE_SINGLEBLOCK — one #DB per TAKEN
   *  branch instead of one per instruction) can run here: x86-64 Linux with a
   *  functional PTRACE_SINGLEBLOCK and Capstone for the intra-block reconstruction.
   *  Hang-proof, cached one-shot probe; callers self-skip cleanly on false. */
  static blockstepAvailable() {
    if (!_lib) return false;
    return _fn.ptraceBlockstepAvailable() !== 0;
  }

  /** Block-step variant of traceCall: drives PTRACE_SINGLEBLOCK (DEBUGCTL.BTF),
   *  stopping once per TAKEN branch and reconstructing the intra-block instructions
   *  with Capstone — the same insns/blocks stream as traceCall at a fraction of the
   *  stops. Probe first with blockstepAvailable(). Complete at moderate overhead,
   *  NOT cheap: each block still costs a full ptrace round-trip. */
  static traceCallBlockstep(code, codeLen, args, trace) {
    const n = args.length;
    const argBuf = Buffer.alloc(8 * Math.max(n, 1));
    for (let i = 0; i < n; i++) argBuf.writeBigInt64LE(BigInt(args[i]), i * 8);
    const resultBuf = Buffer.alloc(8);
    const rc = _fn.ptraceTraceCallBlockstep(code.base, codeLen, argBuf, n, resultBuf, trace._handle);
    if (rc !== ASMTEST_PTRACE_OK) throw new Error(`asmtest_ptrace_trace_call_blockstep failed: ${rc}`);
    return _safeInt(resultBuf.readBigInt64LE(0));
  }

  /** WHOLE-WINDOW capture that OWNS its tracee: fork a child that runs `driver` (a NativeCode —
   *  the WINDOW FRAME) with up to six integer `args`, single-step it out of process across the
   *  whole window, and record the ABSOLUTE address of every instruction in the frame [base,len)
   *  OR any region pre-published on `channel` (an AddrChannel via `newLocal()` — the leaves the
   *  frame calls into; runtime/glue between them is stepped over, not recorded). The window ends
   *  when the frame returns. (asmtest_ptrace_trace_window_call.) Fork-internal — needs no
   *  reverse-attach permission, runs on any ptrace-capable x86-64 Linux. Returns
   *  `{ result, insns, truncated }`: `result` the frame's return (BigInt out of safe range),
   *  `insns` the captured ABSOLUTE addresses (BigInt), `truncated` the honesty bit. Throws on a
   *  non-OK rc; gate with `Ptrace.available()` (the deterministic side-effecting `driver` runs
   *  in the forked child, so it must be re-runnable). */
  static windowCall(driver, args, channel) {
    const n = args.length;
    const argBuf = Buffer.alloc(8 * Math.max(n, 1));
    for (let i = 0; i < n; i++) argBuf.writeBigInt64LE(BigInt(args[i]), i * 8);
    const resultBuf = Buffer.alloc(8);
    const handle = _fn.traceNew(1 << 16, 4096);
    if (!handle) throw new Error('asmtest_trace_new failed');
    try {
      const rc = _fn.windowCall(driver.base, driver.length, argBuf, n, channel._handle, resultBuf, handle);
      if (rc !== ASMTEST_PTRACE_OK) throw new Error(`asmtest_ptrace_trace_window_call failed: ${rc}`);
      const nn = Number(_fn.insnsLen(handle));
      const insns = new Array(nn);
      for (let i = 0; i < nn; i++) insns[i] = BigInt(_fn.insnAt(handle, i)); // ABSOLUTE addrs
      return { result: _safeInt(resultBuf.readBigInt64LE(0)), insns, truncated: _fn.truncated(handle) !== 0 };
    } finally {
      _fn.traceFree(handle);
    }
  }

  /** Like traceCall, but thread a Descent handle through the loop so call-outs are
   *  recorded as edges and (at level >= 2) descended as nested frames. `trace`
   *  (the flat frame-0 view) may be null to record only into `descent`, and vice
   *  versa. CRITICAL: `region` is the traced region's byte length — pass the caller
   *  region's extent, NOT the whole allocation, when the call target is an in-blob
   *  sibling that must stay OUTSIDE the region (else it mis-records as recursion).
   *  Defaults to code.length only when omitted. Returns the child's RAX at the ret. */
  static traceCallEx(code, args, trace, descent, region) {
    const n = args.length;
    const argBuf = Buffer.alloc(8 * Math.max(n, 1));
    for (let i = 0; i < n; i++) argBuf.writeBigInt64LE(BigInt(args[i]), i * 8);
    const resultBuf = Buffer.alloc(8);
    const len = region === undefined || region === null ? code.length : region;
    const th = trace ? trace._handle : null;
    const dh = descent ? descent._handle : null;
    const rc = _fn.ptraceTraceCallEx(code.base, len, argBuf, n, resultBuf, th, dh);
    if (rc !== ASMTEST_PTRACE_OK) throw new Error(`asmtest_ptrace_trace_call_ex failed: ${rc}`);
    return _safeInt(resultBuf.readBigInt64LE(0));
  }

  /** Descending variant of traceAttached for an externally-attached process: threads
   *  a Descent handle through the attach-trace loop. `trace`/`descent` may each be
   *  null. Returns the target's RAX at the ret. */
  static traceAttachedEx(pid, base, len, trace, descent) {
    const resultBuf = Buffer.alloc(8);
    const th = trace ? trace._handle : null;
    const dh = descent ? descent._handle : null;
    const rc = _fn.ptraceTraceAttachedEx(pid, _addr(base), len, resultBuf, th, dh);
    if (rc !== ASMTEST_PTRACE_OK) throw new Error(`asmtest_ptrace_trace_attached_ex failed: ${rc}`);
    return _safeInt(resultBuf.readBigInt64LE(0));
  }

  /** Descending variant of traceAttachedVersioned: decodes the region against the
   *  time-correct bytes from a CodeImage timeline (`img`) as of sequence `when`,
   *  while threading a Descent handle. `trace`/`descent` may each be null. */
  static traceAttachedVersionedEx(pid, base, len, img, when, trace, descent) {
    const resultBuf = Buffer.alloc(8);
    const th = trace ? trace._handle : null;
    const dh = descent ? descent._handle : null;
    const rc = _fn.ptraceTraceAttachedVersionedEx(
      pid, _addr(base), len, img ? img._handle : null, BigInt(when), resultBuf, th, dh);
    if (rc !== ASMTEST_PTRACE_OK) {
      throw new Error(`asmtest_ptrace_trace_attached_versioned_ex failed: ${rc}`);
    }
    return _safeInt(resultBuf.readBigInt64LE(0));
  }

  /** Trace a region in a SEPARATE, already-ptrace-stopped process (the caller owns
   *  PTRACE_ATTACH/DETACH). Reads the target's bytes via process_vm_readv. Returns
   *  the target's RAX at the ret as a JS number (BigInt out of safe range). */
  static traceAttached(pid, base, len, trace) {
    const resultBuf = Buffer.alloc(8);
    const rc = _fn.ptraceTraceAttached(pid, _addr(base), len, resultBuf, trace._handle);
    if (rc !== ASMTEST_PTRACE_OK) throw new Error(`asmtest_ptrace_trace_attached failed: ${rc}`);
    return _safeInt(resultBuf.readBigInt64LE(0));
  }

  /** Block-step variant of traceAttached: one #DB per TAKEN branch (intra-block
   *  instructions reconstructed with Capstone), same contract otherwise — the
   *  rootless managed-runtime completeness fallback at a fraction of the stops.
   *  Probe first with blockstepAvailable(). */
  static traceAttachedBlockstep(pid, base, len, trace) {
    const resultBuf = Buffer.alloc(8);
    const rc = _fn.ptraceTraceAttachedBlockstep(pid, _addr(base), len, resultBuf, trace._handle);
    if (rc !== ASMTEST_PTRACE_OK) throw new Error(`asmtest_ptrace_trace_attached_blockstep failed: ${rc}`);
    return _safeInt(resultBuf.readBigInt64LE(0));
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
    return _safeInt(resultBuf.readBigInt64LE(0));
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

/** Call descent (asmtest_descent_t): configure how the ptrace stepper handles the
 *  call-outs it would otherwise step over, and read back the recorded edges +
 *  nested frames. Four levels (see the DESCENT_* constants): OFF, RECORD_EDGES,
 *  DESCEND_KNOWN, DESCEND_ALL. Pass to Ptrace.traceCallEx and friends. Frame 0 is
 *  the root region (a superset of the flat trace); descended callees are frames
 *  1..N. HAZARD: the address getters (edgeTarget, frameBase, frameLen, frameInsns,
 *  frameBlocks) return JS BigInt — a >2^53 ASLR address rounds if read as Number. */
class Descent {
  constructor(level = DESCENT_OFF) {
    this._handle = _fn.descentNew(level);
    if (!this._handle) throw new Error('asmtest_descent_new failed');
    // Registered upcall trampolines (koffi.register handles). Held for the handle's
    // lifetime so the GC cannot collect/move a resolver/denylist closure mid-single-
    // step; released in free() via koffi.unregister.
    this._cb = [];
  }

  /** Idempotent free: NULL the handle (the C side is NULL-safe, so a double free is
   *  a no-op) and unregister any retained upcall trampolines. */
  free() {
    if (this._handle) {
      _fn.descentFree(this._handle);
      this._handle = null;
    }
    for (const cb of this._cb) {
      try { koffi.unregister(cb); } catch (_e) { /* already gone */ }
    }
    this._cb = [];
  }

  // ---- configuration (in) ----

  /** Ceiling on nested descent depth (frame 0 is depth 0). 0 restores the default. */
  setMaxDepth(d) { _fn.descentSetMaxDepth(this._handle, d); return this; }

  /** Total single-step instruction budget across all descended frames; 0 = default. */
  setInsnBudget(b) { _fn.descentSetInsnBudget(this._handle, BigInt(b)); return this; }

  /** Real-time watchdog (ms) for a descended run (L3 blocked-syscall escape); 0 = default. */
  setWatchdogMs(ms) { _fn.descentSetWatchdogMs(this._handle, ms); return this; }

  /** Arm the built-in L3 default denylist (PLT resolver / vdso / GC-JIT modules;
   *  plus blocking-libc entry points on the fork path). */
  useDefaultDenylist() { _fn.descentUseDefaultDenylist(this._handle); return this; }

  /** Add [base, base+len) to the level-2 allow-set (descend into calls landing inside).
   *  `base` may be a NativeCode external pointer or a numeric/BigInt address. Returns
   *  0 on success, negative on OOM. */
  allowRegion(base, len) { return _fn.descentAllowRegion(this._handle, _addr(base), len); }

  /** Add [base, base+len) to the level-3 deny-set (never descend into it). 0 / negative. */
  denyRegion(base, len) { return _fn.descentDenyRegion(this._handle, _addr(base), len); }

  /** Install a level-2/3 resolver. `fn(calleeAddr)` receives the callee as a BigInt and
   *  returns `{ base, len }` (truthy len) to descend into that region, or a falsy value
   *  to step over. The trampoline is koffi-registered and retained on this Descent so a
   *  GC cannot collect it mid-single-step. */
  setResolver(fn, user = 0) {
    const thunk = (callee, _user, baseOut, lenOut) => {
      const r = fn(BigInt(callee));
      if (r && r.len) {
        koffi.encode(baseOut, 'uint64_t', BigInt(r.base));
        koffi.encode(lenOut, 'uint64_t', BigInt(r.len));
        return 1;
      }
      return 0;
    };
    const cb = koffi.register(thunk, koffi.pointer(ResolverFn));
    this._cb.push(cb);
    _fn.descentSetResolver(this._handle, cb, _addr(user));
    return this;
  }

  /** Install a level-3 denylist. `fn(calleeAddr)` receives the callee as a BigInt and
   *  returns truthy to REFUSE descent (step over it). Retained like setResolver. */
  setDenylist(fn, user = 0) {
    const thunk = (callee, _user) => (fn(BigInt(callee)) ? 1 : 0);
    const cb = koffi.register(thunk, koffi.pointer(DenylistFn));
    this._cb.push(cb);
    _fn.descentSetDenylist(this._handle, cb, _addr(user));
    return this;
  }

  // ---- results (out) ----

  /** Every stepped-over call (level >= 1) as { site, target, depth }: `site` is a
   *  call-site byte offset (Number), `target` the ABSOLUTE callee address (BigInt),
   *  `depth` the caller's frame depth (Number). */
  edges() {
    const n = Number(_fn.descentEdgesLen(this._handle));
    const out = new Array(n);
    for (let i = 0; i < n; i++) {
      out[i] = {
        site: Number(_fn.descentEdgeSite(this._handle, i)),
        target: BigInt(_fn.descentEdgeTarget(this._handle, i)),
        depth: Number(_fn.descentEdgeDepth(this._handle, i)),
      };
    }
    return out;
  }

  /** Number of recorded frames (>= 1 once a trace ran: frame 0 is the root region). */
  framesLen() { return Number(_fn.descentFramesLen(this._handle)); }

  /** ABSOLUTE base address of frame `f` (BigInt). */
  frameBase(f) { return BigInt(_fn.descentFrameBase(this._handle, f)); }

  /** Byte length of frame `f` (BigInt). */
  frameLen(f) { return BigInt(_fn.descentFrameLen(this._handle, f)); }

  /** Descent depth of frame `f` (0 = frame 0). */
  frameDepth(f) { return Number(_fn.descentFrameDepth(this._handle, f)); }

  /** Parent frame index of `f`, or -1 for the root. */
  frameParent(f) { return Number(_fn.descentFrameParent(this._handle, f)); }

  /** The instruction byte-offsets recorded in frame `f`, in execution order, as
   *  BigInts (uint64 accessor — never rounded to Number). */
  frameInsns(f) {
    const n = Number(_fn.descentFrameInsnCount(this._handle, f));
    const out = new Array(n);
    for (let i = 0; i < n; i++) out[i] = BigInt(_fn.descentFrameInsnAt(this._handle, f, i));
    return out;
  }

  /** The distinct basic-block byte-offsets recorded in frame `f`, as BigInts. */
  frameBlocks(f) {
    const n = Number(_fn.descentFrameBlockCount(this._handle, f));
    const out = new Array(n);
    for (let i = 0; i < n; i++) out[i] = BigInt(_fn.descentFrameBlockAt(this._handle, f, i));
    return out;
  }

  /** True if a pool overflowed / a byte failed to decode (the record is incomplete). */
  truncated() { return _fn.descentTruncated(this._handle) !== 0; }

  /** True if descent stopped at a policy limit (max_depth / budget / recursion cap). */
  depthCapped() { return _fn.descentDepthCapped(this._handle) !== 0; }
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

// basename:line call-site name from a captured stack (frame [2] is the caller of
// scope()); basename dodges the 64-char C name ceiling and full-path aliasing under
// Core §0.4's by-name registry.
function _scopeName(stack) {
  const lines = (stack || '').split('\n');
  const frame = lines[2] || '';
  const m = frame.match(/([^\s/\\(]+):(\d+):\d+\)?\s*$/);
  if (m) { const n = `${m[1]}:${m[2]}`; return n.length > 63 ? n.slice(-63) : n; }
  return 'asmscope';
}

// Render the named region's recorded instructions to assembly text; '' if the
// decoder is unavailable. (snprintf semantics: returns the would-be length.)
function _renderRegion(name) {
  const buf = Buffer.alloc(16384);
  const n = _fn.render(name, buf, buf.length);
  if (n <= 0) return '';
  return buf.toString('latin1', 0, Math.min(n, buf.length - 1));
}

// Render a call_scoped_ex capture (keyed by its BY-VALUE scope handle) to assembly
// text; '' if the decoder is unavailable. Same snprintf semantics as _renderRegion.
function _renderScope(scope) {
  const buf = Buffer.alloc(16384);
  const n = _fn.renderScope(scope, buf, buf.length);
  if (n <= 0) return '';
  return buf.toString('latin1', 0, Math.min(n, buf.length - 1));
}

// Render a begin_window whole-window capture (BY-VALUE scope handle) to assembly text
// by decoding the recorded ABSOLUTE addresses from live self-memory; '' if the decoder
// is unavailable. Same snprintf semantics as _renderScope.
function _renderWindow(scope) {
  const buf = Buffer.alloc(16384);
  const n = _fn.renderWindow(scope, buf, buf.length);
  if (n <= 0) return '';
  return buf.toString('latin1', 0, Math.min(n, buf.length - 1));
}

module.exports = {
  HwTrace, NativeCode, AddrChannel, Ptrace, Descent, CodeImage,
  ASMTEST_HW_OK, ASMTEST_HW_EUNAVAIL, INTEL_PT, CORESIGHT, AMD_LBR, SINGLESTEP,
  BEST, CEILING_FREE,
  TIER_HWTRACE, TIER_DYNAMORIO, TIER_EMULATOR,
  FIDELITY_NATIVE, FIDELITY_VIRTUAL,
  TRACE_BEST, TRACE_CEILING_FREE, TRACE_NATIVE_ONLY,
  ASMTEST_PTRACE_OK, ASMTEST_PTRACE_ENOENT,
  DESCENT_OFF, DESCENT_RECORD_EDGES, DESCENT_DESCEND_KNOWN, DESCENT_DESCEND_ALL,
  ASMTEST_CI_OK, ASMTEST_CI_ENOENT,
  ASMTEST_CI_KIND_MPROTECT, ASMTEST_CI_KIND_MMAP, ASMTEST_CI_KIND_MEMFD,
};
