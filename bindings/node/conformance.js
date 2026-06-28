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
  corpusRoutine: routine, Regs, Emu, Trace, Guest, AsmtestError, FaultKind,
  assemble, disas, disasAvailable, Arch, Syntax,
  assertRet, assertAbiPreserved, assertFp, assertVecF32, assertFault,
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

  // read_fault dereferences an unmapped address: the fault is data — where
  // (faultAddr) and why (faultKind) — not a crash.
  const fres = e.call2(routine('read_fault'), 0x00DEAD00, 0);
  check('emu.read_fault',
    fres.faulted() && fres.faultAddr() === 0x00DEAD00 && fres.faultKind() === FaultKind.Read);
  fres.free();

  // int_to_double lands (double)42 in xmm0 (the XMM file, beyond the GP regs);
  // a clean run also keeps rflags live (x86 holds bit 1 set).
  const xres = e.call2(routine('int_to_double'), 42, 0);
  check('emu.int_to_double',
    !xres.faulted() && xres.xmmF64(0, 0) === 42 && (xres.reg('rflags') & 0x2) !== 0);
  xres.free();

  // --- cross-arch emulator guests (raw bytes, any host) ---
  for (const [arch, code, regname] of [
    ['arm64', [0x00, 0x00, 0x01, 0x8B, 0xC0, 0x03, 0x5F, 0xD6], 'x0'],
    ['riscv', [0x33, 0x05, 0xB5, 0x00, 0x67, 0x80, 0x00, 0x00], 'a0'],
    ['arm', [0x01, 0x00, 0x80, 0xE0, 0x1E, 0xFF, 0x2F, 0xE1], 'r0'],
  ]) {
    const g = new Guest(arch);
    const gres = g.call(Buffer.from(code), [40, 2]);
    check(`emu_${arch}.add`, !gres.faulted() && gres.reg(regname) === 42);
    gres.free();
    g.close();
  }

  // --- extended x86-64 emulator calls (raw bytes) ---
  const wide = e.callBytes(Buffer.from([0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x01, 0xD0, 0xC3]), [10, 20, 12]);
  check('emu.wide_int', !wide.faulted() && wide.reg('rax') === 42);
  wide.free();
  const fpr = e.callFp(Buffer.from([0xF2, 0x0F, 0x58, 0xC1, 0xC3]), { fargs: [1.5, 2.25] });
  check('emu.fp_add', !fpr.faulted() && fpr.xmmF64(0, 0) === 3.75);
  fpr.free();
  const vecr = e.callVec(Buffer.from([0x0F, 0x58, 0xC1, 0xC3]), { vargs: [[1, 2, 3, 4], [10, 20, 30, 40]] });
  check('emu.vec_add4f', !vecr.faulted() && vecr.xmmF32(0, 0) === 11 && vecr.xmmF32(0, 3) === 44);
  vecr.free();
  const winr = e.callWin64(Buffer.from([0x48, 0x89, 0xC8, 0x48, 0x01, 0xD0, 0xC3]), [40, 2]);
  check('emu.win64_add', !winr.faulted() && winr.reg('rax') === 42);
  winr.free();

  // --- execution trace / coverage (cross-arch arm64) ---
  {
    const g = new Guest('arm64');
    const tr = new Trace();
    const sel = Buffer.from([
      0x60, 0x00, 0x00, 0xB4, 0x60, 0x0C, 0x80, 0xD2, 0xC0, 0x03, 0x5F, 0xD6,
      0x40, 0x05, 0x80, 0xD2, 0xC0, 0x03, 0x5F, 0xD6,
    ]);
    const tres = g.callTraced(sel, [0], tr);
    check('emu_arm64.trace_sel',
      !tres.faulted() && tres.reg('x0') === 42 && tr.covered(0) && tr.covered(12) && !tr.covered(4));
    tres.free();
    tr.free();
    g.close();
  }

  // in-line assembly (Keystone) replays add_signed; carried by libasmtest_emu,
  // the probe is a defensive guard against an older/leaner lib
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

// --- Tier 1: disassembly (Capstone) decodes known bytes to text ------------
// libasmtest_emu carries Capstone, so this runs by default; the probe stays a
// defensive guard -- only an older/leaner lib reports disasAvailable() false.
if (disasAvailable()) {
  const code = Buffer.from([0x48, 0x31, 0xC0, 0xC3]); // xor rax, rax ; ret
  check('disas.xor_rax', disas(code, 0) === 'xor rax, rax');
  check('disas.ret', disas(code, 3) === 'ret');
  check('disas.nop', disas(Buffer.from([0x90])) === 'nop');
} else {
  console.log('ok - disas.xor_rax # SKIP no disassembler (older/leaner lib)');
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
  {
    const e = new Emu();
    const fres = e.call2(routine('read_fault'), 0x00DEAD00, 0);
    assertFault(fres);
    fres.free();
    e.close();
  }
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

// Track F: mid-execution guards (byte-literal routines).
{
  const e = new Emu();
  const twoWrites = Buffer.from([0x48, 0x89, 0x07, 0x48, 0x89, 0x87, 0x00, 0x08, 0x00, 0x00, 0xC3]);
  e.map(0x400000, 0x1000);
  const w = e.watchWrites(0x400000, 8, 'only');
  e.callBytes(twoWrites, [0x400000]);
  e.watchClear();
  check('guard.watch_escape', w.violated && w.addr === 0x400800 && w.ripOff === 3);
  w.free();
  const clobber = Buffer.from([0x48, 0xC7, 0xC3, 0x99, 0x00, 0x00, 0x00, 0xEB, 0x00, 0xC3]);
  const g = e.guardReg('rbx', 0);
  e.callBytes(clobber, []);
  e.guardRegClear();
  check('guard.reg_invariant', g.violated && g.got === 0x99);
  g.free();
  e.close();
}

// Track E: coverage-guided fuzzing + mutation testing over classify3.
{
  const e = new Emu();
  const classify3 = Buffer.from([0x31, 0xC0, 0x48, 0x85, 0xFF, 0x78, 0x0B, 0x48, 0x85, 0xFF, 0x74, 0x05,
    0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3]);
  const fixed = e.fuzzCover(classify3, 5, 5, 1);
  const guided = e.fuzzCover(classify3, -50, 50, 2000);
  check('fuzz.coverage_beats_fixed', guided.blocks > fixed.blocks);
  const weak = e.mutationTest(classify3, [5]);
  const strong = e.mutationTest(classify3, [-7, 0, 9]);
  check('mutation.strong_kills_more', weak.survived > 0 && strong.survived < weak.survived);
  e.close();
}

// Track D: AVX2 256-bit capture (self-skips off-AVX2).
if (asmtest.cpuHasAvx2()) {
  const out = asmtest.captureVec256(routine('vec_add4d'), [[1, 2, 3, 4], [10, 20, 30, 40]]);
  check('vec256.add4d', out[0] === 11 && out[1] === 22 && out[2] === 33 && out[3] === 44);
} else {
  console.log('ok - vec256.add4d # SKIP no AVX2');
}

console.log(`# ${total - fails} passed, ${fails} failed, ${total} total`);
process.exit(fails === 0 ? 0 : 1);
