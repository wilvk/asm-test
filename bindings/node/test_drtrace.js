// test_drtrace.js — standalone test for the in-process DynamoRIO native-trace
// wrapper (drtrace.js). Mirrors bindings/python/tests/test_drtrace.py.
//
// Self-skips unless the tier is built AND DynamoRIO is resolvable — i.e. unless
// ASMTEST_DRAPP_LIB / ASMTEST_DRCLIENT (and ASMTEST_DR_LIB or DYNAMORIO_HOME)
// point at a built libasmtest_drapp + client on a DynamoRIO-capable Linux
// x86-64 host. On any other host it prints "SKIP: ..." and exits 0.
//
// Run (from the repo root, with the lib env set):
//   cd bindings/node && ASMTEST_DRAPP_LIB=$PWD/../../build/libasmtest_drapp.so \
//     node test_drtrace.js
'use strict';
const assert = require('assert');
const { NativeTrace, NativeCode } = require('./drtrace');

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
const ROUTINE = Buffer.from([0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D,
  0x64, 0x00, 0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3]);

function main() {
  if (!NativeTrace.available()) {
    console.log('SKIP: DynamoRIO native-trace tier unavailable (self-skip)');
    process.exit(0);
  }
  if (!process.env.ASMTEST_DRCLIENT) {
    console.log('SKIP: ASMTEST_DRCLIENT not set (build the DR client)');
    process.exit(0);
  }

  try {
    NativeTrace.initialize();
  } catch (e) {
    console.log(`SKIP: dr_init/start failed: ${e.message}`);
    process.exit(0);
  }

  try {
    // --- block coverage and accumulation ---
    {
      const code = NativeCode.fromBytes(ROUTINE);
      const tr = NativeTrace.create(64); // blocks=64, instructions=0
      tr.register('add2', code);

      let r;
      tr.region('add2', () => { r = code.call(20, 22); });
      assert.strictEqual(Number(r), 42);
      assert.ok(tr.covered(0), 'entry block should be covered');

      const before = tr.blocksLen();
      let r2;
      // 120 > 100 -> dec -> 119, takes the other block.
      tr.region('add2', () => { r2 = code.call(60, 60); });
      assert.strictEqual(Number(r2), 119);
      assert.ok(tr.blocksLen() >= before, 'block count should not decrease');
      assert.strictEqual(NativeTrace.markerError(), 0);

      tr.unregister('add2');
      code.free();
      tr.free();
    }

    // --- instruction mode ---
    {
      const code = NativeCode.fromBytes(ROUTINE);
      const tr = NativeTrace.create(64, 64); // blocks=64, instructions=64
      tr.register('add2i', code);

      let r;
      tr.region('add2i', () => { r = code.call(1, 2); });
      assert.strictEqual(Number(r), 3);
      assert.ok(tr.insnsTotal() >= 4, 'ordered instruction stream recorded');

      tr.unregister('add2i');
      code.free();
      tr.free();
    }
  } finally {
    NativeTrace.shutdown();
  }

  console.log('PASS');
}

main();
