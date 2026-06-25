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
  regsRet: L.func('unsigned long asmtest_regs_ret(void *)'),
  regsFret: L.func('double asmtest_regs_fret(void *)'),
  regsFlagSet: L.func('int asmtest_regs_flag_set(void *, const char *)'),
  checkAbi: L.func('int asmtest_check_abi(void *, void *, size_t)'),
  emuOpen: L.func('void *emu_open()'),
  emuClose: L.func('void emu_close(void *)'),
  emuResNew: L.func('void *asmtest_emu_result_new()'),
  emuResFree: L.func('void asmtest_emu_result_free(void *)'),
  emuCall2: L.func('int asmtest_emu_call2(void *, void *, long, long, void *)'),
  emuFaulted: L.func('int asmtest_emu_result_faulted(void *)'),
  emuReg: L.func('uint64_t asmtest_emu_x86_reg(void *, const char *)'),
};

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
  /** The integer return value (rax / x0). */
  ret() { return Number(fn.regsRet(this._h)); }
  /** The scalar FP return value (xmm0 / d0). */
  fret() { return fn.regsFret(this._h); }
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
  /** Read an x86-64 guest register by name (e.g. "rax"). */
  reg(name) { return Number(fn.emuReg(this._h, name)); }
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
  close() { if (this._h) { fn.emuClose(this._h); this._h = null; } }
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
function assertNoFault(res) {
  if (res.faulted()) throw new AsmtestError('unexpected fault');
}
function assertEmuReg(res, name, want) {
  const got = res.reg(name);
  if (got !== want) throw new AsmtestError(`emu ${name}: got ${got}, want ${want}`);
}

module.exports = {
  corpusRoutine,
  Regs, Emu, EmuResult, AsmtestError,
  assertRet, assertAbiPreserved, assertFlag, assertFp, assertNoFault, assertEmuReg,
};
