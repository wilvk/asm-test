// Node data-flow binding smoke (Phase 6): GC-move canonicalizer + method resolver,
// mirroring the Python/C++ suites' semantics. Self-skips when the lib is not built.
'use strict';

const df = require('./dataflow');

let n = 0;
let failed = false;
function check(cond, desc) {
  n++;
  console.log((cond ? 'ok ' : 'not ok ') + n + ' - ' + desc);
  if (!cond) failed = true;
}

if (!df.available()) {
  console.log('# SKIP dataflow node binding: libasmtest_dataflow not built (make shared-dataflow)');
  console.log('1..0 # skipped');
  process.exit(0);
}

// --- GC-move canonicalizer (forward-to-final; uint64 -> BigInt) --- //
check(df.gcmoveCanon([], 0, 0x1234) === 0x1234n, 'gcmove: empty move set is identity');
const mv = [[0x1000, 0x2000, 0x100, 5]];
check(df.gcmoveCanon(mv, 3, 0x1010) === 0x2010n, 'gcmove: pre-move addr forwards to final');
check(df.gcmoveCanon(mv, 3, 0x1000) === 0x2000n, 'gcmove: object base forwards');
check(df.gcmoveCanon(mv, 3, 0x10ff) === 0x20ffn, 'gcmove: last byte of half-open window forwards');
check(df.gcmoveCanon(mv, 3, 0x1100) === 0x1100n, 'gcmove: one past the window not forwarded');
check(df.gcmoveCanon(mv, 5, 0x1010) === 0x1010n, 'gcmove: at-move-step observation not forwarded');
check(df.gcmoveCanon(mv, 3, 0x3000) === 0x3000n, 'gcmove: out-of-range addr unchanged');
const mv2 = [[0x1000, 0x2000, 0x100, 3], [0x2000, 0x3000, 0x100, 6]];
check(df.gcmoveCanon(mv2, 1, 0x1010) === 0x3010n, 'gcmove: two compactions compose to final');

// --- method resolver (tiered re-JIT aware; int) --- //
const ms = [[0x1000, 0x40, 'Foo', 3], [0x2000, 0x20, 'Bar', 1], [0x3000, 0, 'Baz', 2]];
check(df.methodResolvePc(ms, 0x1000) === 0, 'method: Foo range start');
check(df.methodResolvePc(ms, 0x103f) === 0, 'method: Foo last byte (half-open)');
check(df.methodResolvePc(ms, 0x1040) === -1, 'method: one past Foo -> none');
check(df.methodResolvePc(ms, 0x2010) === 1, 'method: Bar range');
check(df.methodResolvePc(ms, 0x3000) === 2, 'method: Baz point match');
check(df.methodResolvePc(ms, 0x3001) === -1, 'method: Baz is point-only');
const rj = [[0x1000, 0x40, 'Foo', 1], [0x1000, 0x40, 'Foo', 5]];
check(df.methodResolvePc(rj, 0x1010) === 1, 'method: tiered re-JIT newest version wins');
check(df.methodResolvePc([], 0x1000) === -1, 'method: empty map -> -1');

console.log('1..' + n);
process.exit(failed ? 1 : 0);
