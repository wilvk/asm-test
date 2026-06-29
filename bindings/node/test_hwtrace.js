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
const { HwTrace, NativeCode, SINGLESTEP } = require('./hwtrace');

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
  if (!HwTrace.available(SINGLESTEP)) {
    console.log(`# SKIP single-step backend unavailable: ${HwTrace.skipReason(SINGLESTEP)}`);
    process.exit(0);
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

  if (_failed) {
    console.log(`# FAILED ${_n} tests`);
    process.exit(1);
  }
  console.log(`1..${_n}`);
}

main();
