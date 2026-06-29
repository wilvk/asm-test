// test_hwtrace.js — standalone test for the single-step hardware-trace wrapper
// (hwtrace.js). Mirrors bindings/python/tests/test_hwtrace.py.
//
// Unlike the DynamoRIO wrapper (which needs a DynamoRIO install) and the PT/AMD
// backends (which need specific bare-metal hardware), the SINGLESTEP backend runs
// on ANY x86-64 Linux — so this asserts a real, live trace here and in CI /
// containers, self-skipping only off x86-64 Linux or without Capstone. On a
// skip it prints "# SKIP ..." and exits 0; on a failure it exits nonzero.
//
// Run (from the repo root, with the lib env set):
//   cd bindings/node && \
//     ASMTEST_HWTRACE_LIB=$PWD/../../build/libasmtest_hwtrace.so \
//     NODE_PATH=$(npm root -g) node test_hwtrace.js
'use strict';
const assert = require('assert');
const {
  HwTrace, NativeCode, SINGLESTEP, AMD_LBR,
  BEST, CEILING_FREE, ASMTEST_HW_EUNAVAIL,
  TIER_HWTRACE, TIER_EMULATOR, FIDELITY_NATIVE, FIDELITY_VIRTUAL,
  TRACE_BEST, TRACE_CEILING_FREE, TRACE_NATIVE_ONLY,
} = require('./hwtrace');

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
const ROUTINE = Buffer.from([0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D,
  0x64, 0x00, 0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3]);

// mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret  (19 back-edges > LBR's 16)
const LOOP = Buffer.from([0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00,
  0x48, 0x01, 0xF8, 0x48, 0xFF, 0xCE, 0x75, 0xF8, 0xC3]);

let _n = 0;
let _failed = false;

function ok(cond, msg) {
  _n += 1;
  if (cond) {
    console.log(`ok ${_n} - ${msg}`);
  } else {
    _failed = true;
    console.log(`not ok ${_n} - ${msg}`);
  }
}

function main() {
  // --- auto-select orchestrator: selection invariants (hold on every host, even
  //     where all backends self-skip and the cascade is empty) ---
  {
    const best = HwTrace.resolve(BEST);
    const cf = HwTrace.resolve(CEILING_FREE);

    // Every resolved backend is actually available, ordered by descending fidelity
    // (ascending enum), with no duplicates.
    ok(best.every((b) => HwTrace.available(b)), 'resolve(BEST) returns only available backends');
    let ordered = true;
    for (let i = 1; i < best.length; i++) if (best[i] <= best[i - 1]) ordered = false;
    ok(ordered, 'resolve(BEST) is ordered by descending fidelity, no dups');

    // CEILING_FREE drops the one fixed-window backend (AMD LBR) and is otherwise a
    // subset of BEST.
    ok(!cf.includes(AMD_LBR), 'resolve(CEILING_FREE) never selects AMD LBR');
    ok(cf.every((b) => best.includes(b)), 'resolve(CEILING_FREE) is a subset of resolve(BEST)');

    // auto(policy) is the head of resolve(policy), or EUNAVAIL when empty.
    const ab = HwTrace.auto(BEST);
    ok(ab === (best.length ? best[0] : ASMTEST_HW_EUNAVAIL), 'auto(BEST) is the head of resolve(BEST)');
  }

  // --- cross-tier orchestrator: structural invariants (hold on every host) ---
  {
    const best = HwTrace.resolveTiers(TRACE_BEST);
    const nat = HwTrace.resolveTiers(TRACE_NATIVE_ONLY);
    const cf = HwTrace.resolveTiers(TRACE_CEILING_FREE);

    // Every HW choice satisfies the hardware-tier probe; each choice's fidelity
    // matches its tier (emulator is VIRTUAL, every other tier is NATIVE).
    let hwAvail = true;
    let fidOK = true;
    for (const c of best) {
      if (c.tier === TIER_HWTRACE && !HwTrace.available(c.backend)) hwAvail = false;
      const want = c.tier === TIER_EMULATOR ? FIDELITY_VIRTUAL : FIDELITY_NATIVE;
      if (c.fidelity !== want) fidOK = false;
    }
    ok(hwAvail, 'resolveTiers(BEST): every HW-tier choice is available');
    ok(fidOK, 'resolveTiers(BEST): fidelity matches tier (emulator VIRTUAL, else NATIVE)');

    // NATIVE choices precede the single VIRTUAL emulator floor, which is the last
    // entry under BEST.
    ok(best.length > 0 && best[best.length - 1].tier === TIER_EMULATOR,
      'resolveTiers(BEST): emulator floor is the last entry');
    ok(best.filter((c) => c.tier === TIER_EMULATOR).length === 1,
      'resolveTiers(BEST): exactly one emulator entry');

    // NATIVE_ONLY forbids the native->emulator crossing: it is BEST minus the floor.
    ok(nat.every((c) => c.tier !== TIER_EMULATOR), 'resolveTiers(NATIVE_ONLY) drops the emulator floor');
    ok(nat.length === best.length - 1, 'resolveTiers(NATIVE_ONLY) is BEST minus the floor');

    // CEILING_FREE drops AMD LBR.
    ok(cf.every((c) => !(c.tier === TIER_HWTRACE && c.backend === AMD_LBR)),
      'resolveTiers(CEILING_FREE) never selects AMD LBR');

    // autoTier(policy) is the head of resolveTiers(policy).
    const one = HwTrace.autoTier(TRACE_BEST);
    ok(one !== null && one.tier === best[0].tier && one.backend === best[0].backend,
      'autoTier(BEST) is the head of resolveTiers(BEST)');
  }

  if (!HwTrace.available(SINGLESTEP)) {
    console.log(`# SKIP single-step backend unavailable: ${HwTrace.skipReason(SINGLESTEP)}`);
    process.exit(0);
  }

  // --- cross-tier native-only resolves on x86-64 Linux: single-step is a native
  //     floor, so even NATIVE_ONLY never collapses to nothing here. ---
  {
    const nat = HwTrace.resolveTiers(TRACE_NATIVE_ONLY);
    const pick = HwTrace.autoTier(TRACE_NATIVE_ONLY);
    ok(nat.length > 0 && pick !== null && pick.fidelity === FIDELITY_NATIVE,
      'resolveTiers(NATIVE_ONLY) resolves a native choice on x86-64 Linux');
    ok(nat.some((c) => c.tier === TIER_HWTRACE && c.backend === SINGLESTEP),
      'resolveTiers(NATIVE_ONLY) includes the single-step native floor');
  }

  HwTrace.init(SINGLESTEP);
  try {
    // --- straight-line + branch fixture: a*1 + b, two blocks ---
    {
      const code = NativeCode.fromBytes(ROUTINE);
      const tr = HwTrace.create({ blocks: 64, instructions: 64 });
      tr.register('add2', code);

      let r;
      // Keep begin..call..end tight: single-step arms the trap flag across it.
      tr.region('add2', () => { r = code.call(20, 22); });

      ok(Number(r) === 42, 'routine returns 42 (42 <= 100 -> jle taken, dec skipped)');
      assert.deepStrictEqual(tr.insnOffsets(), [0x0, 0x3, 0x6, 0xC, 0x11]);
      ok(true, 'insnOffsets() == [0, 3, 6, 12, 17]');
      ok(tr.insnsTotal() === 5, 'insnsTotal() == 5');
      ok(tr.covered(0) && tr.covered(0x11), 'covered(0) && covered(17)');
      ok(tr.blocksLen() === 2, 'blocksLen() == 2');
      ok(!tr.truncated(), '!truncated()');

      code.free();
      tr.free();
    }

    // --- loop fixture: 20 iterations, no depth ceiling ---
    {
      const code = NativeCode.fromBytes(LOOP);
      const tr = HwTrace.create({ blocks: 64, instructions: 256 });
      tr.register('loop', code);

      let r;
      tr.region('loop', () => { r = code.call(1, 20); });

      ok(Number(r) === 20, 'loop returns 20 (sum of 1, 20 times)');
      ok(tr.insnsTotal() === 62, 'insnsTotal() == 62 (1 + 20*3 + 1, all captured)');
      ok(tr.covered(0) && tr.covered(0x7), 'covered(0) && covered(7)');
      ok(tr.blocksLen() === 2, 'blocksLen() == 2');
      ok(!tr.truncated(), '!truncated()');

      code.free();
      tr.free();
    }
  } finally {
    HwTrace.shutdown();
  }

  // --- auto-select orchestrator: live trace through whatever auto picked. On any
  //     x86-64 Linux host the cascade is non-empty (single-step floor), so auto()
  //     resolves a usable backend. Own init/shutdown (single global lifecycle). ---
  {
    const best = HwTrace.resolve(BEST);
    const pick = HwTrace.auto(BEST);
    ok(best.length > 0 && pick >= 0, 'auto resolves a backend (single-step floor)');

    HwTrace.init(pick);
    try {
      const code = NativeCode.fromBytes(ROUTINE);
      const tr = HwTrace.create({ blocks: 64, instructions: 64 });
      tr.register('auto', code);

      let r;
      tr.region('auto', () => { r = code.call(20, 22); });

      ok(Number(r) === 42, 'auto-selected backend traces a live call (returns 42)');
      ok(tr.covered(0), 'auto-selected backend covers block offset 0');
      if (pick === SINGLESTEP) { // the pick off PT/AMD hosts: byte-exact parity
        assert.deepStrictEqual(tr.insnOffsets(), [0x0, 0x3, 0x6, 0xC, 0x11]);
        ok(true, 'auto pick (single-step) yields offsets [0, 3, 6, 12, 17]');
      }

      code.free();
      tr.free();
    } finally {
      HwTrace.shutdown();
    }
  }

  if (_failed) {
    console.log(`# FAILED ${_n} tests`);
    process.exit(1);
  }
  console.log(`1..${_n}`);
}

main();
