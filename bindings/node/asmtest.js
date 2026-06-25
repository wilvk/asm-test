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

const emuPath = process.env.ASMTEST_LIB;
const corpusPath = process.env.ASMTEST_CORPUS_LIB;
if (!emuPath) {
  throw new Error('set ASMTEST_LIB to libasmtest_emu.{so,dylib}');
}

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
  // Optional: the in-line assembler (Keystone) is only in the emu+asm lib. koffi
  // throws here if the symbol is absent, so degrade to null against plain emu.
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
  emuFaulted: L.func('int asmtest_emu_result_faulted(void *)'),
  emuFaultAddr: L.func('uint64_t asmtest_emu_result_fault_addr(void *)'),
  emuFaultKind: L.func('int asmtest_emu_result_fault_kind(void *)'),
  emuReg: L.func('uint64_t asmtest_emu_x86_reg(void *, const char *)'),
  emuXmmF64: L.func('double asmtest_emu_x86_xmm_f64(void *, int, int)'),
  emuXmmF32: L.func('float asmtest_emu_x86_xmm_f32(void *, int, int)'),
};

/** Invalid-access kind reported by EmuResult.faultKind() (mirrors emu_fault_kind_t). */
const FaultKind = { None: 0, Read: 1, Write: 2, Fetch: 3 };

/** Resolve a canonical corpus routine (e.g. "add_signed") to its address. */
function corpusRoutine(name) {
  if (!fn.corpusRoutine) {
    throw new Error('set ASMTEST_CORPUS_LIB to use corpusRoutine()');
  }
  return fn.corpusRoutine(name);
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
  /** Whether the loaded native lib carries the in-line assembler (Keystone). */
  asmAvailable() { return fn.emuCallAsm6 !== null; }
  /**
   * Assemble x86-64 `src` in `opts.syntax` (0=Intel, 1=AT&T) via Keystone and
   * run it with the integer `args` (up to six), stopping after `opts.maxInsns`
   * instructions (0 = run to `ret`). Returns the EmuResult; throws AsmtestError
   * carrying the Keystone diagnostic if the string fails to assemble. Only when
   * asmAvailable() — needs the emu+asm native lib.
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
  close() { if (this._h) { fn.emuClose(this._h); this._h = null; } }
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

// Architecture / syntax codes for assemble() (mirror asm_arch_t / asm_syntax_t).
const Arch = { X86_64: 0, ARM64: 1, RISCV64: 2, ARM32: 3 };
const Syntax = { INTEL: 0, ATT: 1 };

module.exports = {
  corpusRoutine,
  Regs, Emu, EmuResult, AsmtestError, FaultKind,
  assemble, asmError, Arch, Syntax,
  assertRet, assertAbiPreserved, assertFlag, assertFp, assertVecF32,
  assertNoFault, assertFault, assertEmuReg,
};
