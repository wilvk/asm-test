// conformance.js — asm-test Node binding (Track N): the conformance runner.
//
// A thin consumer of the reusable library module (./asmtest): it replays the
// cross-language conformance corpus through the Regs / Emu / assert* API and
// never touches koffi itself. Exits nonzero on any mismatch.
//
//   ASMTEST_LIB         libasmtest_emu.{so,dylib}
//   ASMTEST_CORPUS_LIB  libasmtest_corpus.{so,dylib}
'use strict';
const asmtest = require('./asmtest');
const {
  corpusRoutine: routine, Regs, Emu, AsmtestError,
  assemble, Arch, Syntax,
  assertRet, assertAbiPreserved, assertFp, assertVecF32,
} = asmtest;

let fails = 0;
let total = 0;
function check(name, ok) {
  total++;
  console.log(`${ok ? 'ok' : 'not ok'} - ${name}`);
  if (!ok) fails++;
}

function withRegs(fn) {
  const r = new Regs();
  try { fn(r); } finally { r.free(); }
}

// --- Tier 1: corpus replay (capture trampoline) ----------------------------
withRegs((r) => {
  r.capture6(routine('add_signed'), 40, 2);
  check('add_signed.basic', r.ret() === 42 && r.abiPreserved());
});
withRegs((r) => {
  r.capture6(routine('sum_via_rbx'), 20, 22);
  check('sum_via_rbx.abi_preserved', r.ret() === 42 && r.abiPreserved());
});
withRegs((r) => {
  r.capture6(routine('clobbers_rbx'), 1, 2);
  check('clobbers_rbx.abi_violation_detected', !r.abiPreserved());
});
withRegs((r) => {
  r.capture6(routine('set_carry'));
  check('set_carry.cf_set', r.flagSet('CF'));
});
withRegs((r) => {
  r.capture6(routine('clear_carry'));
  check('clear_carry.cf_clear', !r.flagSet('CF'));
});
withRegs((r) => {
  r.captureFp2(routine('fp_add'), 1.5, 2.25);
  check('fp_add.basic', r.fret() === 3.75);
});
withRegs((r) => {
  r.captureVecF32(routine('vec_add4f'), [[1, 2, 3, 4], [10, 20, 30, 40]]);
  const v = r.vecF32(0);
  check('vec_add4f.basic', v[0] === 11 && v[1] === 22 && v[2] === 33 && v[3] === 44);
});

// --- Tier 1: corpus replay (emulator, x86-64 guest) ------------------------
{
  const e = new Emu();
  const res = e.call2(routine('add_signed'), 40, 2);
  check('emu.add_signed', !res.faulted() && res.reg('rax') === 42);
  res.free();

  // in-line assembly (Keystone) replays add_signed, only if the lib has it
  if (e.asmAvailable()) {
    const ares = e.callAsm('mov rax, rdi; add rax, rsi; ret', [40, 2]);
    check('asm.add_signed', !ares.faulted() && ares.reg('rax') === 42);
    ares.free();

    // Widened shim: AT&T syntax + a third arg (rdi+rsi+rdx).
    const att = e.callAsm('mov %rdi, %rax; add %rsi, %rax; add %rdx, %rax; ret',
      [10, 20, 12], { syntax: Syntax.ATT });
    check('asm.att_3arg', !att.faulted() && att.reg('rax') === 42);
    att.free();

    // Failure path: a bad string throws with the Keystone diagnostic.
    let threw = false;
    try { e.callAsm('mov rax, nonsense_token').free(); }
    catch (err) { threw = err instanceof AsmtestError && err.message.length > 'in-line assembly failed: '.length; }
    check('asm.bad_source_throws', threw);

    // Multi-arch assemble-to-bytes: AArch64 `ret` is C0 03 5F D6.
    const a64 = assemble('ret', Arch.ARM64);
    check('asm.arm64_bytes', a64.length === 4 && a64[0] === 0xC0 && a64[3] === 0xD6);
  }
  e.close();
}

// --- Tier 2: idiomatic assertions pass on good input -----------------------
let t2pass = true;
try {
  withRegs((r) => {
    r.capture6(routine('add_signed'), 40, 2);
    assertRet(r, 42);
    assertAbiPreserved(r);
  });
  withRegs((r) => {
    r.captureFp2(routine('fp_add'), 1.5, 2.25);
    assertFp(r, 3.75);
  });
  withRegs((r) => {
    r.captureVecF32(routine('vec_add4f'), [[1, 2, 3, 4], [10, 20, 30, 40]]);
    assertVecF32(r, 0, [11, 22, 33, 44]);
  });
} catch (_e) {
  t2pass = false;
}
check('tier2.assertions_pass', t2pass);

// --- Tier 2: the assertions actually fail when they should -----------------
let t2teeth = false;
try {
  withRegs((r) => {
    r.capture6(routine('add_signed'), 40, 2);
    assertRet(r, 99); // wrong on purpose
  });
} catch (e) {
  t2teeth = e instanceof AsmtestError;
}
check('tier2.assertions_have_teeth', t2teeth);

console.log(`# ${total - fails} passed, ${fails} failed, ${total} total`);
process.exit(fails === 0 ? 0 : 1);
