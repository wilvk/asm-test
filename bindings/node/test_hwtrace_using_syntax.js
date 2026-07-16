// test_hwtrace_using_syntax.js — the REAL `using` half of the §D1 scope test.
//
// SPLIT OUT ON PURPOSE, and the only reason this file exists: `using` is SYNTAX, so a
// source file containing it fails to PARSE wholesale on a Node without Explicit Resource
// Management — the failure lands at load, before any version guard inside the file could
// run. Requiring this module IS therefore the feature gate: test_hwtrace_using.js only
// reaches it after probing that the parser accepts a `using` declaration (Node 24+, or
// Node 22 with --harmony-explicit-resource-management), and self-skips below that. Nothing
// here may be inlined back into a file that must run on this binding's Node 18 floor.
//
// Everything the driver can assert WITHOUT the syntax (that the disposer ends + renders,
// its idempotence, the auto-name, §0.4 slot reuse) lives there and runs everywhere. What
// only this file can prove is the language integration: that block exit — and an
// exception unwinding the block — really do invoke the disposer.
'use strict';
const { NativeCode, AsmTrace } = require('./hwtrace');

/** Drive the real `using` construct over a native leaf. `ok(cond, msg)` is the caller's
 *  TAP assert; `ROUTINE` the shared two-block fixture (kept in the driver so the byte
 *  fixture has one home). The tier is already up (the driver init'd it). */
function run({ ok, ROUTINE }) {
  // --- the construct: `using t = new AsmTrace(code)` disposes at BLOCK EXIT ---
  {
    const code = NativeCode.fromBytes(ROUTINE);
    let r;
    let scope; // the same object, kept to inspect AFTER the block has exited
    {
      using t = new AsmTrace(code, { emit: false });
      scope = t;
      r = code.call(20, 22); // 42 <= 100 -> jle taken, dec skipped
      ok(t.path === '', 'using: nothing rendered INSIDE the block (dispose has not run yet)');
    }
    // Block exited -> the language called t[Symbol.dispose]() -> end + render.
    ok(Number(r) === 42, 'using: add2(20,22) == 42');
    ok(scope.armed, 'using: armed on an available backend');
    ok(!scope.truncated, 'using: not truncated');
    ok(scope.path.length > 0, 'using: render-on-close produced text AT BLOCK EXIT');
    ok((scope.path.match(/\n/g) || []).length === 5, 'using: 5 rendered instruction lines');
    ok(scope.path.includes('ret'), 'using: rendered listing includes the ret');
    ok(scope.name.startsWith('test_hwtrace_using_syntax.js:'),
      `using: auto-name is basename:line (${scope.name})`);
    code.free();
  }

  // --- dispose still runs when the block unwinds on a throw (the reason the sugar is
  //     worth having over a bare begin/end pair: no finally to forget) ---
  {
    const code = NativeCode.fromBytes(ROUTINE);
    let scope;
    let caught = '';
    try {
      using t = new AsmTrace(code, { emit: false });
      scope = t;
      code.call(20, 22);
      throw new Error('boom'); // unwinds the using block: dispose runs, THEN the catch
    } catch (e) {
      caught = e.message;
    }
    ok(caught === 'boom', 'using(throw): the block error propagates to the caller');
    ok(scope.path.length > 0, 'using(throw): disposed + rendered anyway while unwinding');
    ok(!scope.truncated, 'using(throw): not truncated');
    code.free();
  }
}

module.exports = { run };
