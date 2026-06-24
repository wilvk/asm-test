// conformance.js — asm-test Node binding (Track N), via koffi FFI.
//
// Replays the conformance corpus through the opaque-handle FFI layer (no struct
// layout needed): asmtest_corpus_routine for addresses, asmtest_capture6 / _fp2
// + accessors for capture, and asmtest_emu_call2 + accessors for the emulator.
// Exits nonzero on any mismatch.
//
//   ASMTEST_LIB         libasmtest_emu.{so,dylib}
//   ASMTEST_CORPUS_LIB  libasmtest_corpus.{so,dylib}
'use strict';
const koffi = require('koffi');

const emuPath = process.env.ASMTEST_LIB;
const corpusPath = process.env.ASMTEST_CORPUS_LIB;
if (!emuPath || !corpusPath) {
  console.error('set ASMTEST_LIB and ASMTEST_CORPUS_LIB');
  process.exit(2);
}

const L = koffi.load(emuPath);
const C = koffi.load(corpusPath);

const corpusRoutine = C.func('void *asmtest_corpus_routine(const char *)');
const regsNew = L.func('void *asmtest_regs_new()');
const regsFree = L.func('void asmtest_regs_free(void *)');
const capture6 = L.func('void asmtest_capture6(void *, void *, long, long, long, long, long, long)');
const captureFp2 = L.func('void asmtest_capture_fp2(void *, void *, double, double)');
const regsRet = L.func('unsigned long asmtest_regs_ret(void *)');
const regsFret = L.func('double asmtest_regs_fret(void *)');
const regsFlagSet = L.func('int asmtest_regs_flag_set(void *, const char *)');
const checkAbi = L.func('int asmtest_check_abi(void *, void *, size_t)');
const emuOpen = L.func('void *emu_open()');
const emuClose = L.func('void emu_close(void *)');
const emuResNew = L.func('void *asmtest_emu_result_new()');
const emuResFree = L.func('void asmtest_emu_result_free(void *)');
const emuCall2 = L.func('int asmtest_emu_call2(void *, void *, long, long, void *)');
const emuFaulted = L.func('int asmtest_emu_result_faulted(void *)');
const emuReg = L.func('uint64_t asmtest_emu_x86_reg(void *, const char *)');

const routine = (name) => corpusRoutine(name);

// Tier-2 idiomatic assertions: throw with a clear message on failure.
class AsmtestError extends Error {}
function assertRet(r, e) {
  const got = Number(regsRet(r));
  if (got !== e) throw new AsmtestError(`ret: got ${got}, want ${e}`);
}
function assertAbiPreserved(r) {
  if (checkAbi(r, null, 0) !== 0) throw new AsmtestError('ABI not preserved');
}
function assertFp(r, e) {
  const got = regsFret(r);
  if (got !== e) throw new AsmtestError(`fp: got ${got}, want ${e}`);
}

let fails = 0;
let total = 0;
function check(name, ok) {
  total++;
  if (ok) {
    console.log(`ok - ${name}`);
  } else {
    fails++;
    console.log(`not ok - ${name}`);
  }
}

function withRegs(fn) {
  const r = regsNew();
  try {
    fn(r);
  } finally {
    regsFree(r);
  }
}

withRegs((r) => {
  capture6(r, routine('add_signed'), 40, 2, 0, 0, 0, 0);
  check('add_signed.basic', Number(regsRet(r)) === 42 && checkAbi(r, null, 0) === 0);
});

withRegs((r) => {
  capture6(r, routine('sum_via_rbx'), 20, 22, 0, 0, 0, 0);
  check('sum_via_rbx.abi_preserved', Number(regsRet(r)) === 42 && checkAbi(r, null, 0) === 0);
});

withRegs((r) => {
  capture6(r, routine('clobbers_rbx'), 1, 2, 0, 0, 0, 0);
  check('clobbers_rbx.abi_violation_detected', checkAbi(r, null, 0) !== 0);
});

withRegs((r) => {
  capture6(r, routine('set_carry'), 0, 0, 0, 0, 0, 0);
  check('set_carry.cf_set', regsFlagSet(r, 'CF') === 1);
});

withRegs((r) => {
  capture6(r, routine('clear_carry'), 0, 0, 0, 0, 0, 0);
  check('clear_carry.cf_clear', regsFlagSet(r, 'CF') === 0);
});

withRegs((r) => {
  captureFp2(r, routine('fp_add'), 1.5, 2.25);
  check('fp_add.basic', regsFret(r) === 3.75);
});

{
  const e = emuOpen();
  const res = emuResNew();
  emuCall2(e, routine('add_signed'), 40, 2, res);
  check('emu.add_signed', emuFaulted(res) === 0 && Number(emuReg(res, 'rax')) === 42);
  emuResFree(res);
  emuClose(e);
}

// Tier-2 idiomatic assertions: pass paths succeed, failure paths bite.
let t2pass = true;
try {
  withRegs((r) => {
    capture6(r, routine('add_signed'), 40, 2, 0, 0, 0, 0);
    assertRet(r, 42);
    assertAbiPreserved(r);
  });
  withRegs((r) => {
    captureFp2(r, routine('fp_add'), 1.5, 2.25);
    assertFp(r, 3.75);
  });
} catch (_e) {
  t2pass = false;
}
check('tier2.assertions_pass', t2pass);

let t2teeth = false;
try {
  withRegs((r) => {
    capture6(r, routine('add_signed'), 40, 2, 0, 0, 0, 0);
    assertRet(r, 99); // wrong on purpose
  });
} catch (e) {
  t2teeth = e instanceof AsmtestError;
}
check('tier2.assertions_have_teeth', t2teeth);

console.log(`# ${total - fails} passed, ${fails} failed, ${total} total`);
process.exit(fails === 0 ? 0 : 1);
