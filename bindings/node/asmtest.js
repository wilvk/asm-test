// asmtest.js — asm-test Node binding (Track N): the reusable library module.
//
// This is the module a Node project consumes; it keeps all koffi FFI inside, so
// calling code never declares a native entry point. It drives the opaque-handle
// FFI layer (src/ffi.c), so no C struct layout is mirrored: a Regs handle with
// capture6 / captureFp2 + accessors, Emu / EmuResult for the emulator (faults
// as data), and Tier-2 assert* helpers that throw AsmtestError on failure.
//
// The shared libraries are taken from the environment, matching how the
// framework's Makefile invokes the bindings:
//   ASMTEST_LIB         libasmtest_emu.{so,dylib}   (capture + emulator + accessors)
//   ASMTEST_CORPUS_LIB  libasmtest_corpus.{so,dylib} (the canonical fixtures)
'use strict';
const koffi = require('koffi');
const path = require('path');
const fs = require('fs');

// Resolve libasmtest_emu: an explicit ASMTEST_LIB wins (dev / custom build);
// otherwise fall back to the native payload bundled in the published package at
// native/<os>-<arch>/ next to this module (how the npm package ships it).
function resolveEmuLib() {
  if (process.env.ASMTEST_LIB) return process.env.ASMTEST_LIB;
  const os = process.platform === 'darwin' ? 'darwin' : 'linux';
  const arch = process.arch === 'arm64' ? 'arm64' : 'x86_64'; // node 'x64' -> x86_64
  const ext = process.platform === 'darwin' ? 'dylib' : 'so';
  const bundled = path.join(__dirname, 'native', `${os}-${arch}`, `libasmtest_emu.${ext}`);
  if (fs.existsSync(bundled)) return bundled;
  throw new Error(`set ASMTEST_LIB to libasmtest_emu.${ext} `
    + `(no bundled native/${os}-${arch}/ in this package)`);
}

const emuPath = resolveEmuLib();
const corpusPath = process.env.ASMTEST_CORPUS_LIB;

const L = koffi.load(emuPath);
const C = corpusPath ? koffi.load(corpusPath) : null;

// All native entry points, declared once and kept private to this module.
const fn = {
  corpusRoutine: C && C.func('void *asmtest_corpus_routine(const char *)'),
  regsNew: L.func('void *asmtest_regs_new()'),
  regsFree: L.func('void asmtest_regs_free(void *)'),
  capture6: L.func('void asmtest_capture6(void *, void *, long, long, long, long, long, long)'),
  captureFp2: L.func('void asmtest_capture_fp2(void *, void *, double, double)'),
  captureVecF32: L.func('void asmtest_capture_vec_f32(void *, void *, float *, int)'),
  regsRet: L.func('unsigned long asmtest_regs_ret(void *)'),
  regsFret: L.func('double asmtest_regs_fret(void *)'),
  regsVecF32: L.func('float asmtest_regs_vec_f32(void *, int, int)'),
  regsFlagSet: L.func('int asmtest_regs_flag_set(void *, const char *)'),
  checkAbi: L.func('int asmtest_check_abi(void *, void *, size_t)'),
  emuOpen: L.func('void *emu_open()'),
  emuClose: L.func('void emu_close(void *)'),
  emuResNew: L.func('void *asmtest_emu_result_new()'),
  emuResFree: L.func('void asmtest_emu_result_free(void *)'),
  emuCall2: L.func('int asmtest_emu_call2(void *, void *, long, long, void *)'),
  // The in-line assembler (Keystone) is carried by libasmtest_emu. koffi throws
  // here if the symbol is absent, so degrade to null against an older/leaner lib.
  // The widened run shim takes six scalar args + syntax + an instruction cap;
  // asmBytes is multi-arch text->bytes; asmLastError carries the diagnostic.
  emuCallAsm6: (() => {
    try { return L.func('int asmtest_emu_call_asm6(void *, const char *, int, long, long, long, long, long, long, int, uint64_t, void *)'); }
    catch { return null; }
  })(),
  asmBytes: (() => {
    try { return L.func('int asmtest_asm_bytes(int, int, const char *, uint64_t, _Out_ uint8_t *, int)'); }
    catch { return null; }
  })(),
  asmLastError: (() => {
    try { return L.func('const char *asmtest_asm_last_error()'); }
    catch { return null; }
  })(),
  // The disassembler (Capstone) is carried by libasmtest_emu (resolved
  // defensively, in case of an older/leaner lib). emuDisas decodes one
  // instruction at `off` into a text buffer; the probe guards the call.
  emuDisas: (() => {
    try { return L.func('size_t emu_disas(int, const uint8_t *, size_t, uint64_t, uint64_t, _Out_ char *, size_t)'); }
    catch { return null; }
  })(),
  emuDisasAvailable: (() => {
    try { return L.func('bool emu_disas_available()'); }
    catch { return null; }
  })(),
  emuFaulted: L.func('int asmtest_emu_result_faulted(void *)'),
  emuFaultAddr: L.func('uint64_t asmtest_emu_result_fault_addr(void *)'),
  emuFaultKind: L.func('int asmtest_emu_result_fault_kind(void *)'),
  emuReg: L.func('uint64_t asmtest_emu_x86_reg(void *, const char *)'),
  emuXmmF64: L.func('double asmtest_emu_x86_xmm_f64(void *, int, int)'),
  emuXmmF32: L.func('float asmtest_emu_x86_xmm_f32(void *, int, int)'),
  // Extended x86 emulator calls (array form: explicit code + length). All pointer
  // args go in as Buffers (void *), so raw machine-code bytes run directly.
  emuCall: L.func('int emu_call(void *, void *, size_t, void *, int, uint64_t, void *)'),
  emuCallFp: L.func('int emu_call_fp(void *, void *, size_t, void *, int, void *, int, uint64_t, void *)'),
  emuCallVec: L.func('int emu_call_vec(void *, void *, size_t, void *, int, void *, int, uint64_t, void *)'),
  emuCallWin64: L.func('int emu_call_win64(void *, void *, size_t, void *, int, uint64_t, void *)'),
  emuCallTraced: L.func('int emu_call_traced(void *, void *, size_t, void *, int, uint64_t, void *, void *)'),
  // Opaque trace handle.
  traceNew: L.func('void *asmtest_emu_trace_new(size_t, size_t)'),
  traceFree: L.func('void asmtest_emu_trace_free(void *)'),
  traceCovered: L.func('int asmtest_emu_trace_covered(void *, uint64_t)'),
  // Cross-arch guests (raw bytes, any host) + per-arch result accessors.
  arm64Open: L.func('void *emu_arm64_open()'),
  arm64Close: L.func('void emu_arm64_close(void *)'),
  arm64Call: L.func('int emu_arm64_call(void *, void *, size_t, void *, int, uint64_t, void *)'),
  arm64CallTraced: L.func('int emu_arm64_call_traced(void *, void *, size_t, void *, int, uint64_t, void *, void *)'),
  arm64ResNew: L.func('void *asmtest_emu_arm64_result_new()'),
  arm64ResFree: L.func('void asmtest_emu_arm64_result_free(void *)'),
  arm64Reg: L.func('uint64_t asmtest_emu_arm64_reg(void *, const char *)'),
  riscvOpen: L.func('void *emu_riscv_open()'),
  riscvClose: L.func('void emu_riscv_close(void *)'),
  riscvCall: L.func('int emu_riscv_call(void *, void *, size_t, void *, int, uint64_t, void *)'),
  riscvResNew: L.func('void *asmtest_emu_riscv_result_new()'),
  riscvResFree: L.func('void asmtest_emu_riscv_result_free(void *)'),
  riscvReg: L.func('uint64_t asmtest_emu_riscv_reg(void *, const char *)'),
  armOpen: L.func('void *emu_arm_open()'),
  armClose: L.func('void emu_arm_close(void *)'),
  armCall: L.func('int emu_arm_call(void *, void *, size_t, void *, int, uint64_t, void *)'),
  armResNew: L.func('void *asmtest_emu_arm_result_new()'),
  armResFree: L.func('void asmtest_emu_arm_result_free(void *)'),
  armReg: L.func('uint64_t asmtest_emu_arm_reg(void *, const char *)'),
  // Mid-execution guards (Track F)
  emuMap: L.func('int emu_map(void *, uint64_t, size_t)'),
  watchWrites: L.func('void emu_watch_writes(void *, uint64_t, size_t, int, void *)'),
  watchClear: L.func('void emu_watch_clear(void *)'),
  guardReg: L.func('int emu_guard_reg(void *, const char *, uint64_t, void *)'),
  guardRegClear: L.func('void emu_guard_reg_clear(void *)'),
  watchNew: L.func('void *asmtest_emu_watch_new()'),
  watchFree: L.func('void asmtest_emu_watch_free(void *)'),
  watchViolated: L.func('int asmtest_emu_watch_violated(void *)'),
  watchAddr: L.func('uint64_t asmtest_emu_watch_addr(void *)'),
  watchRip: L.func('uint64_t asmtest_emu_watch_rip_off(void *)'),
  rguardNew: L.func('void *asmtest_emu_reg_guard_new()'),
  rguardFree: L.func('void asmtest_emu_reg_guard_free(void *)'),
  rguardViolated: L.func('int asmtest_emu_reg_guard_violated(void *)'),
  rguardGot: L.func('uint64_t asmtest_emu_reg_guard_got(void *)'),
  // Coverage-guided fuzzing + mutation testing (Track E)
  fuzzCover1: L.func('int emu_fuzz_cover1(void *, void *, size_t, long, long, uint64_t, uint64_t, void *, void *)'),
  mutationTest1: L.func('size_t emu_mutation_test1(void *, void *, size_t, void *, size_t, uint64_t, uint64_t, void *)'),
  fuzzNew: L.func('void *asmtest_emu_fuzz_stat_new()'),
  fuzzFree: L.func('void asmtest_emu_fuzz_stat_free(void *)'),
  fuzzBlocks: L.func('uint64_t asmtest_emu_fuzz_blocks_reached(void *)'),
  fuzzCorpus: L.func('uint64_t asmtest_emu_fuzz_corpus_len(void *)'),
  mutNew: L.func('void *asmtest_emu_mutation_stat_new()'),
  mutFree: L.func('void asmtest_emu_mutation_stat_free(void *)'),
  mutKilled: L.func('uint64_t asmtest_emu_mutation_killed(void *)'),
  mutSurvived: L.func('uint64_t asmtest_emu_mutation_survived(void *)'),
  // AVX2 256-bit capture (Track D)
  captureVec256: L.func('void asm_call_capture_vec256(void *, void *, void *, void *)'),
  cpuAvx2: L.func('int asmtest_cpu_has_avx2()'),
  // AVX-512 512-bit capture (Track D)
  captureVec512: L.func('void asm_call_capture_vec512(void *, void *, void *, void *)'),
  cpuAvx512f: L.func('int asmtest_cpu_has_avx512f()'),
};

// Pack helpers: marshal JS numbers into native arrays as Buffers (koffi passes a
// Buffer straight through as a `void *`), or null when empty.
function packLongs(args) {
  if (!args || !args.length) return null;
  const b = Buffer.alloc(8 * args.length);
  for (let i = 0; i < args.length; i++) b.writeBigInt64LE(BigInt(args[i]), i * 8);
  return b;
}
function packDoubles(f) {
  if (!f || !f.length) return null;
  const b = Buffer.alloc(8 * f.length);
  for (let i = 0; i < f.length; i++) b.writeDoubleLE(f[i], i * 8);
  return b;
}
function packVecs(vargs) {
  if (!vargs || !vargs.length) return null;
  const b = Buffer.alloc(16 * vargs.length);
  for (let i = 0; i < vargs.length; i++)
    for (let l = 0; l < 4; l++) b.writeFloatLE(vargs[i][l] || 0, i * 16 + l * 4);
  return b;
}

/** Invalid-access kind reported by EmuResult.faultKind() (mirrors emu_fault_kind_t). */
const FaultKind = { None: 0, Read: 1, Write: 2, Fetch: 3 };

/** Resolve a canonical corpus routine (e.g. "add_signed") to its address. The
 * name table (asmtest_corpus_routine) covers the catalogued routines; when it
 * misses (null), fall back to the corpus lib's exported symbol of that name —
 * e.g. vec_add8d, which lives in the fixture lib but isn't in the table. */
function corpusRoutine(name) {
  if (!fn.corpusRoutine) {
    throw new Error('set ASMTEST_CORPUS_LIB to use corpusRoutine()');
  }
  const p = fn.corpusRoutine(name);
  if (p == null && C) {
    try { return C.symbol(name, 'void'); } catch (_e) { /* not exported either */ }
  }
  return p;
}

/** A captured register/flags snapshot. Call free() when done. */
class Regs {
  constructor() { this._h = fn.regsNew(); }
  /** Call fn through the real ABI with up to six integer args. */
  capture6(routine, a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0) {
    fn.capture6(this._h, routine, a0, a1, a2, a3, a4, a5);
  }
  /** Call fn with two double args, capturing the FP return. */
  captureFp2(routine, f0, f1) { fn.captureFp2(this._h, routine, f0, f1); }
  /**
   * Call fn with up to eight 128-bit vector args, capturing the vector register
   * file. `vectors` is an array of four-float32-lane arrays; the vector return
   * is read back with vecF32(0).
   */
  captureVecF32(routine, vectors) {
    const lanes = [];
    for (const v of vectors) for (let i = 0; i < 4; i++) lanes.push(v[i] || 0);
    fn.captureVecF32(this._h, routine, Float32Array.from(lanes), vectors.length);
  }
  /** The integer return value (rax / x0). */
  ret() { return Number(fn.regsRet(this._h)); }
  /** The scalar FP return value (xmm0 / d0). */
  fret() { return fn.regsFret(this._h); }
  /** The four float32 lanes of vector register `index` (0 = the vector return). */
  vecF32(index = 0) { return [0, 1, 2, 3].map((lane) => fn.regsVecF32(this._h, index, lane)); }
  /** Whether a named condition flag (e.g. "CF") is set. */
  flagSet(name) { return fn.regsFlagSet(this._h, name) === 1; }
  /** Whether the callee-saved registers were restored (verdict shim). */
  abiPreserved() { return fn.checkAbi(this._h, null, 0) === 0; }
  free() { if (this._h) { fn.regsFree(this._h); this._h = null; } }
}

/** An emulator run's outcome — faults surfaced as data, not a crash. */
class EmuResult {
  constructor() { this._h = fn.emuResNew(); }
  faulted() { return fn.emuFaulted(this._h) !== 0; }
  /** Faulting guest address; only meaningful when faulted(). */
  faultAddr() { return Number(fn.emuFaultAddr(this._h)); }
  /** Why the access was invalid (a FaultKind value); only meaningful when faulted(). */
  faultKind() { return fn.emuFaultKind(this._h); }
  /** Read an x86-64 guest register by name — GP plus "rip" / "rflags". */
  reg(name) { return Number(fn.emuReg(this._h, name)); }
  /** Lane (0..1) of guest XMM register `index` as a double (scalar return = xmmF64(0, 0)). */
  xmmF64(index = 0, lane = 0) { return fn.emuXmmF64(this._h, index, lane); }
  /** Lane (0..3) of guest XMM register `index` as a float32. */
  xmmF32(index = 0, lane = 0) { return fn.emuXmmF32(this._h, index, lane); }
  free() { if (this._h) { fn.emuResFree(this._h); this._h = null; } }
}

/** An open emulator (x86-64 Unicorn guest). Call close() when done. */
class Emu {
  constructor() { this._h = fn.emuOpen(); }
  /** Run fn in the emulator with two integer args; returns an EmuResult. */
  call2(routine, a0, a1) {
    const res = new EmuResult();
    fn.emuCall2(this._h, routine, a0, a1, res._h);
    return res;
  }
  /** Run raw x86-64 machine-code bytes (a Buffer/array) with up to six int args. */
  callBytes(code, args = []) {
    const res = new EmuResult();
    const buf = Buffer.from(code);
    fn.emuCall(this._h, buf, buf.length, packLongs(args), args.length, 0, res._h);
    return res;
  }
  /** Run raw bytes marshalling doubles into the FP arg registers (scalar return =
   *  res.xmmF64(0, 0)). */
  callFp(code, { iargs = [], fargs = [] } = {}) {
    const res = new EmuResult();
    const buf = Buffer.from(code);
    fn.emuCallFp(this._h, buf, buf.length, packLongs(iargs), iargs.length,
      packDoubles(fargs), fargs.length, 0, res._h);
    return res;
  }
  /** Run raw bytes marshalling 128-bit vectors (arrays of four float32 lanes). */
  callVec(code, { iargs = [], vargs = [] } = {}) {
    const res = new EmuResult();
    const buf = Buffer.from(code);
    fn.emuCallVec(this._h, buf, buf.length, packLongs(iargs), iargs.length,
      packVecs(vargs), vargs.length, 0, res._h);
    return res;
  }
  /** Run raw bytes under the Microsoft x64 (Win64) convention. */
  callWin64(code, args = []) {
    const res = new EmuResult();
    const buf = Buffer.from(code);
    fn.emuCallWin64(this._h, buf, buf.length, packLongs(args), args.length, 0, res._h);
    return res;
  }
  /** Like callBytes, but record an execution trace / coverage into `trace`. */
  callTraced(code, args, trace) {
    const res = new EmuResult();
    const buf = Buffer.from(code);
    fn.emuCallTraced(this._h, buf, buf.length, packLongs(args), args.length, 0, res._h, trace._h);
    return res;
  }
  /** Whether the loaded native lib carries the in-line assembler (Keystone). */
  asmAvailable() { return fn.emuCallAsm6 !== null; }
  /**
   * Assemble x86-64 `src` in `opts.syntax` (0=Intel, 1=AT&T, 2=NASM, 3=MASM,
   * 4=GAS; see `Syntax`) via Keystone and
   * run it with the integer `args` (up to six), stopping after `opts.maxInsns`
   * instructions (0 = run to `ret`). Returns the EmuResult; throws AsmtestError
   * carrying the Keystone diagnostic if the string fails to assemble.
   * libasmtest_emu carries the assembler, so this works by default; guard with
   * asmAvailable() for an older/leaner lib.
   */
  callAsm(src, args = [], { syntax = 0, maxInsns = 0 } = {}) {
    if (!fn.emuCallAsm6) throw new AsmtestError('in-line assembler not in this build');
    const a = [0, 0, 0, 0, 0, 0];
    const nargs = Math.min(args.length, 6);
    for (let i = 0; i < nargs; i++) a[i] = args[i];
    const res = new EmuResult();
    const ok = fn.emuCallAsm6(this._h, src, syntax,
      a[0], a[1], a[2], a[3], a[4], a[5], nargs, maxInsns, res._h) !== 0;
    if (!ok) { res.free(); throw new AsmtestError('in-line assembly failed: ' + asmError()); }
    return res;
  }
  // --- Mid-execution guards (Track F) ---
  /** Map a guest RW region [addr, addr+size) the routine can use. */
  map(addr, size) { return fn.emuMap(this._h, addr, size) !== 0; }
  /** Arm a memory-write watchpoint over [addr,addr+size): mode 'only' (flag a
   * write that escapes it) or 'never' (one that touches it). Returns a Watch. */
  watchWrites(addr, size, mode = 'only') {
    const w = new Watch();
    fn.watchWrites(this._h, addr, size, mode === 'never' ? 0 : 1, w._h);
    return w;
  }
  watchClear() { fn.watchClear(this._h); }
  /** Arm a register invariant: GP `name` must hold `want` at every block entry.
   * Returns a RegGuard; throws for an unknown register name. */
  guardReg(name, want) {
    const g = new RegGuard();
    if (fn.guardReg(this._h, name, want, g._h) === 0) {
      g.free(); throw new AsmtestError('unknown register: ' + name);
    }
    return g;
  }
  guardRegClear() { fn.guardRegClear(this._h); }
  // --- Coverage-guided fuzzing + mutation testing (Track E) ---
  /** Coverage-guided input search over one-int-arg `code`; {blocks, corpus}. */
  fuzzCover(code, lo, hi, iters, seed = 0xC0FFEE) {
    const buf = Buffer.from(code);
    const uni = fn.traceNew(0, 256), st = fn.fuzzNew();
    fn.fuzzCover1(this._h, buf, buf.length, lo, hi, iters, seed, uni, st);
    const out = { blocks: Number(fn.fuzzBlocks(st)), corpus: Number(fn.fuzzCorpus(st)) };
    fn.fuzzFree(st); fn.traceFree(uni);
    return out;
  }
  /** Bit-flip mutation testing of `code` against `inputs`; {killed, survived}. */
  mutationTest(code, inputs, maxMutants = 0, seed = 0xABCD) {
    const buf = Buffer.from(code);
    const st = fn.mutNew();
    fn.mutationTest1(this._h, buf, buf.length, packLongs(inputs), inputs.length, maxMutants, seed, st);
    const out = { killed: Number(fn.mutKilled(st)), survived: Number(fn.mutSurvived(st)) };
    fn.mutFree(st);
    return out;
  }
  close() { if (this._h) { fn.emuClose(this._h); this._h = null; } }
}

/** A memory-write watchpoint result (Track F). */
class Watch {
  constructor() { this._h = fn.watchNew(); }
  get violated() { return fn.watchViolated(this._h) !== 0; }
  get addr() { return Number(fn.watchAddr(this._h)); }
  get ripOff() { return Number(fn.watchRip(this._h)); }
  free() { if (this._h) { fn.watchFree(this._h); this._h = null; } }
}

/** A register-invariant guard result (Track F). */
class RegGuard {
  constructor() { this._h = fn.rguardNew(); }
  get violated() { return fn.rguardViolated(this._h) !== 0; }
  get got() { return Number(fn.rguardGot(this._h)); }
  free() { if (this._h) { fn.rguardFree(this._h); this._h = null; } }
}

/** True if the host CPU + OS support AVX2 (gate captureVec256). */
function cpuHasAvx2() { return fn.cpuAvx2() !== 0; }

/** AVX2 256-bit capture (Track D): `vargs` = array of [4 doubles]; returns the
 * 4 f64 lanes of ymm0 (the vector return). x86-64 + AVX2 only. */
function captureVec256(fnAddr, vargs) {
  const out = Buffer.alloc(16 * 32);
  const va = Buffer.alloc(8 * 32);
  vargs.slice(0, 8).forEach((v, i) => {
    for (let l = 0; l < 4; l++) va.writeDoubleLE(v[l] || 0, i * 32 + l * 8);
  });
  const ia = Buffer.alloc(6 * 8);
  fn.captureVec256(out, fnAddr, ia, va);
  return [0, 8, 16, 24].map((o) => out.readDoubleLE(o));
}

/** True if the host CPU + OS support AVX-512F (gate captureVec512). */
function cpuHasAvx512f() { return fn.cpuAvx512f() !== 0; }

/** AVX-512 512-bit capture (Track D): `vargs` = array of [8 doubles]; returns the
 * 8 f64 lanes of zmm0 (the vector return). x86-64 + AVX-512F only. */
function captureVec512(fnAddr, vargs) {
  const out = Buffer.alloc(64 * 32);
  const va = Buffer.alloc(64 * 8);
  vargs.slice(0, 8).forEach((v, i) => {
    for (let l = 0; l < 8; l++) va.writeDoubleLE(v[l] || 0, i * 64 + l * 8);
  });
  const ia = Buffer.alloc(6 * 8);
  fn.captureVec512(out, fnAddr, ia, va);
  return [0, 8, 16, 24, 32, 40, 48, 56].map((o) => out.readDoubleLE(o));
}

/** The Keystone diagnostic from the most recent assemble (thread-local; '' on success). */
function asmError() { return fn.asmLastError ? fn.asmLastError() : ''; }

/**
 * Assemble `src` for `arch` (0=x86-64,1=arm64,2=riscv64,3=arm32) / `syntax`
 * (0=Intel,1=AT&T) at load address `addr`, returning the machine-code bytes as
 * a Buffer. Multi-arch (unlike Emu#callAsm, which runs on the x86-64 guest).
 * Throws AsmtestError with the Keystone diagnostic on failure.
 */
function assemble(src, arch = 0, syntax = 0, addr = 0x00100000) {
  if (!fn.asmBytes) throw new AsmtestError('in-line assembler not in this build');
  let buf = Buffer.alloc(256);
  let n = fn.asmBytes(arch, syntax, src, addr, buf, buf.length);
  if (n === 0) throw new AsmtestError('assemble failed: ' + asmError());
  if (n > buf.length) { buf = Buffer.alloc(n); fn.asmBytes(arch, syntax, src, addr, buf, n); }
  return buf.subarray(0, n);
}

/** Whether the loaded native lib carries the disassembler (Capstone). True for
 * libasmtest_emu (the superset); only an older/leaner lib returns false. */
function disasAvailable() { return fn.emuDisas !== null && fn.emuDisasAvailable(); }

/**
 * Disassemble the one instruction at byte `off` of `code` (a Buffer / byte
 * array) for `arch` (0=x86-64,1=arm64,2=riscv64,3=arm32; mirrors emu_arch_t).
 * `base` is the address the bytes run at (EMU_CODE_BASE) so PC-relative operands
 * resolve. Returns 'mnemonic operands', or '' with no disassembler / decode miss.
 */
function disas(code, off = 0, arch = 0, base = 0x00100000) {
  if (!disasAvailable()) return '';
  const buf = Buffer.alloc(160);
  const n = fn.emuDisas(arch, Buffer.from(code), code.length, base, off, buf, buf.length);
  if (n === 0) return '';
  const z = buf.indexOf(0);
  return buf.toString('utf8', 0, z < 0 ? buf.length : z);
}

/** An opaque execution-trace / basic-block coverage recorder. */
class Trace {
  constructor(insnsCap = 4096, blocksCap = 4096) { this._h = fn.traceNew(insnsCap, blocksCap); }
  /** True if the basic block at byte-offset `off` was entered. */
  covered(off) { return fn.traceCovered(this._h, off) !== 0; }
  free() { if (this._h) { fn.traceFree(this._h); this._h = null; } }
}

/** A cross-arch run's outcome; registers are read by name. Call free() when done. */
class GuestResult {
  constructor(arch) { this.arch = arch; this._h = fn[`${arch}ResNew`](); }
  faulted() { return fn.emuFaulted(this._h) !== 0; }
  /** Guest register by name (e.g. "x0"/"sp", "a0"/"x10", "r0"). */
  reg(name) { return Number(fn[`${this.arch}Reg`](this._h, name)); }
  free() { if (this._h) { fn[`${this.arch}ResFree`](this._h); this._h = null; } }
}

/** A cross-arch Unicorn guest ('arm64'/'riscv'/'arm') running raw machine-code
 *  bytes — emulated regardless of host arch. Call close() when done. */
class Guest {
  constructor(arch) { this.arch = arch; this._h = fn[`${arch}Open`](); }
  /** Run raw bytes with integer args in the guest ABI registers. */
  call(code, args = []) {
    const res = new GuestResult(this.arch);
    const buf = Buffer.from(code);
    fn[`${this.arch}Call`](this._h, buf, buf.length, packLongs(args), args.length, 0, res._h);
    return res;
  }
  /** Like call, but record an execution trace / coverage into `trace` (arm64). */
  callTraced(code, args, trace) {
    if (this.arch !== 'arm64') throw new AsmtestError('traced guest run only wired for arm64');
    const res = new GuestResult(this.arch);
    const buf = Buffer.from(code);
    fn.arm64CallTraced(this._h, buf, buf.length, packLongs(args), args.length, 0, res._h, trace._h);
    return res;
  }
  close() { if (this._h) { fn[`${this.arch}Close`](this._h); this._h = null; } }
}

/** Thrown by the assert* helpers on a failed check. */
class AsmtestError extends Error {}

// Tier-2 idiomatic assertions: throw with a clear message on failure.
function assertRet(r, want) {
  const got = r.ret();
  if (got !== want) throw new AsmtestError(`ret: got ${got}, want ${want}`);
}
function assertAbiPreserved(r) {
  if (!r.abiPreserved()) throw new AsmtestError('ABI not preserved');
}
function assertFlag(r, name, set = true) {
  if (r.flagSet(name) !== set) throw new AsmtestError(`flag ${name}: want ${set}`);
}
function assertFp(r, want) {
  const got = r.fret();
  if (got !== want) throw new AsmtestError(`fp: got ${got}, want ${want}`);
}
function assertVecF32(r, index, want) {
  const got = r.vecF32(index);
  for (let i = 0; i < want.length; i++) {
    if (got[i] !== want[i]) throw new AsmtestError(`vec[${index}] lane ${i}: got ${got[i]}, want ${want[i]}`);
  }
}
function assertNoFault(res) {
  if (res.faulted()) throw new AsmtestError('unexpected fault');
}
function assertFault(res) {
  if (!res.faulted()) throw new AsmtestError('expected a fault, but the run completed cleanly');
}
function assertEmuReg(res, name, want) {
  const got = res.reg(name);
  if (got !== want) throw new AsmtestError(`emu ${name}: got ${got}, want ${want}`);
}
function assertGuestReg(res, name, want) {
  const got = res.reg(name);
  if (got !== want) throw new AsmtestError(`guest ${name}: got ${got}, want ${want}`);
}
function assertCovered(trace, off) {
  if (!trace.covered(off)) throw new AsmtestError(`block ${off}: expected covered`);
}

// Architecture / syntax codes for assemble() (mirror asm_arch_t / asm_syntax_t).
const Arch = { X86_64: 0, ARM64: 1, RISCV64: 2, ARM32: 3 };
const Syntax = { INTEL: 0, ATT: 1, NASM: 2, MASM: 3, GAS: 4 };

// Absolute path of the native library actually loaded — for clean-room install
// tests to assert it came from the bundled package payload, not a leaked build/
// tree, a Homebrew dylib, or an ASMTEST_LIB override (macos-clean-test, Track A).
function libraryPath() { return fs.realpathSync(emuPath); }

module.exports = {
  corpusRoutine,
  Regs, Emu, EmuResult, Trace, Guest, GuestResult, AsmtestError, FaultKind,
  Watch, RegGuard, cpuHasAvx2, captureVec256, cpuHasAvx512f, captureVec512,
  assemble, asmError, disas, disasAvailable, Arch, Syntax, libraryPath,
  assertRet, assertAbiPreserved, assertFlag, assertFp, assertVecF32,
  assertNoFault, assertFault, assertEmuReg, assertGuestReg, assertCovered,
};
