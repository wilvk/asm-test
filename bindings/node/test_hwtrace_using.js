// test_hwtrace_using.js — standalone test for the §D1 `using` / `Symbol.dispose` scope
// construct (hwtrace.js `AsmTrace`), the sugar over the same register-then-begin/end pair
// as the `region(name, fn)` / `scope(code, fn, opts)` callback forms.
//
// WHY IT IS ITS OWN LANE, IN TWO FILES: `using` is SYNTAX. A file containing it fails to
// PARSE wholesale on a Node without Explicit Resource Management, so the syntax cannot sit
// in any file that must also run on this binding's Node 18 floor — no in-file version
// guard can save it, because the SyntaxError precedes execution. So:
//
//   * THIS file parses on any Node and asserts the DISPOSAL SEMANTICS directly, by
//     calling the disposer exactly as the language would — `t[kDispose]()` — and checking
//     the captured trace. `kDispose` is the guarded key the binding publishes, so this
//     half is real (not `typeof`-deep) even where `using` cannot run; and
//   * test_hwtrace_using_syntax.js holds the real `using t = new AsmTrace(code)` and is
//     required ONLY where the parser accepts it (Node 24+, or Node 22 with
//     --harmony-explicit-resource-management). Below that it self-skips with a reason.
//
// Conventions match test_hwtrace.js: "# SKIP ..." + exit 0 where the tier is unavailable
// (the single-step backend runs on any x86-64 Linux, so it normally does not), TAP-ish
// "ok N - msg" lines, and a nonzero exit on failure.
//
// Run (from the repo root, with the lib env set):
//   cd bindings/node && \
//     ASMTEST_HWTRACE_LIB=$PWD/../../build/libasmtest_hwtrace.so \
//     NODE_PATH=$(npm root -g) node test_hwtrace_using.js
'use strict';
const { HwTrace, NativeCode, AsmTrace, kDispose, SINGLESTEP } = require('./hwtrace');

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks) — the
// same leaf test_hwtrace.js traces, so the rendered shape is comparable across the lane.
const ROUTINE = Buffer.from([0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D,
  0x64, 0x00, 0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3]);

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

// Does this runtime's PARSER accept a `using` declaration? Compile-only probe: the body
// is never run, and a function body is a legal position for `using` (the top level of a
// Script is not), so a SyntaxError here means only one thing — no Explicit Resource
// Management. Feature-detected rather than version-sniffed because the syntax also rides
// a V8 flag on Node 22, which a `process.versions.node >= 24` test would call a skip.
function usingSyntaxSupported() {
  try {
    new Function('using _x = { [Symbol.dispose]() {} };');
    return true;
  } catch (e) {
    if (e instanceof SyntaxError) return false;
    throw e; // anything else (e.g. code generation disabled) is not ours to swallow
  }
}

function main() {
  // --- the guard itself: kDispose must be a usable key on EVERY supported Node ---
  {
    ok(typeof kDispose === 'symbol', 'kDispose: the guarded key resolves to a symbol');
    // Where the well-known symbol exists (native from Node 18.18 / 20.4) the guard must
    // resolve to it — `using` looks up Symbol.dispose and nothing else, so a fallback
    // winning here would mean the syntax silently never finds the disposer.
    if (typeof Symbol.dispose === 'symbol') {
      ok(kDispose === Symbol.dispose,
        'kDispose: is Symbol.dispose where native (the key `using` looks up)');
    } else {
      ok(kDispose === Symbol.for('nodejs.dispose'),
        `kDispose: falls back to Symbol.for('nodejs.dispose') on node ${process.version}`);
    }
    ok(typeof AsmTrace.prototype[kDispose] === 'function',
      'AsmTrace: the disposer is keyed off the guarded kDispose');
  }

  if (!HwTrace.available(SINGLESTEP)) {
    console.log(`# SKIP single-step backend unavailable: ${HwTrace.skipReason(SINGLESTEP)}`);
    console.log(`1..${_n}`);
    process.exit(0);
  }
  HwTrace.init(SINGLESTEP);

  // --- disposal semantics, driven the way `using` drives them (works on any Node): the
  //     trace must END and RENDER at dispose, not at construction ---
  {
    const code = NativeCode.fromBytes(ROUTINE);
    const t = new AsmTrace(code, { emit: false });
    ok(t.path === '', 'AsmTrace: nothing rendered before dispose');
    const r = code.call(20, 22); // 42 <= 100 -> jle taken, dec skipped
    t[kDispose]();               // exactly what `using` invokes at block exit

    ok(Number(r) === 42, 'AsmTrace: add2(20,22) == 42');
    ok(t.armed, 'AsmTrace: armed on an available backend');
    ok(!t.truncated, 'AsmTrace: not truncated');
    ok(t.path.length > 0, 'AsmTrace: render-on-close produced text AT dispose');
    ok((t.path.match(/\n/g) || []).length === 5, 'AsmTrace: 5 rendered instruction lines');
    ok(t.path.includes('ret'), 'AsmTrace: rendered listing includes the ret');
    ok(t.name.startsWith('test_hwtrace_using.js:'),
      `AsmTrace: auto-name is basename:line (${t.name})`);
    code.free();
  }

  // --- dispose is idempotent: an explicit close inside a `using` block must not
  //     double-end when the language disposes again at block exit ---
  {
    const code = NativeCode.fromBytes(ROUTINE);
    const t = new AsmTrace(code, { emit: false, name: 'idem' });
    code.call(20, 22);
    t[kDispose]();
    const first = t.path;
    t[kDispose]();     // the second disposal the language would run after a manual close
    t.close();         // and the manual alias, for good measure
    ok(t.path === first && first.length > 0, 'AsmTrace: dispose is idempotent (no double-end)');
    ok(t.name === 'idem', 'AsmTrace: opts.name overrides the call-site auto-name');
    code.free();
  }

  // --- a scope in a LOOP: every iteration re-registers under one call-site-constant
  //     auto-name. Core §0.4 refreshes that slot in place — the case the C core calls out
  //     for `using`/RAII scopes, which would otherwise exhaust MAX_REGIONS ---
  {
    const code = NativeCode.fromBytes(ROUTINE);
    let rendered = 0;
    let name = '';
    for (let i = 0; i < 4; i++) {
      const t = new AsmTrace(code, { emit: false });
      code.call(20, 22);
      t[kDispose]();
      if (t.path.length > 0 && t.armed && !t.truncated) rendered += 1;
      name = t.name;
    }
    ok(rendered === 4, 'AsmTrace: 4 loop iterations at one call site each captured (§0.4 slot reuse)');
    ok(name.startsWith('test_hwtrace_using.js:'), 'AsmTrace: the looped scope keeps its call-site name');
    code.free();
  }

  // --- two SIMULTANEOUSLY-LIVE scopes at ONE name. The call-site auto-name is
  //     CONSTANT, so recursion (or any reentrant traced helper) lands two live scopes on
  //     the ONE registry slot §0.4 refreshes in place. The scopes RECORD correctly
  //     regardless (each range-stack frame snapshots its own trace at begin), but the
  //     name-keyed render/end resolve to whichever registered LAST — so without a
  //     reclaim the outer renders the inner's trace, which the inner's close has already
  //     freed: a use-after-free (SIGSEGV, or a silently wrong listing when the freed read
  //     lands on mapped memory). ---
  {
    const code = NativeCode.fromBytes(ROUTINE);
    const outer = new AsmTrace(code, { name: 'shared', emit: false });
    const inner = new AsmTrace(code, { name: 'shared', emit: false });
    code.call(20, 22);
    inner.close();
    outer.close();
    const innerLines = (inner.path.match(/\n/g) || []).length;
    const outerLines = (outer.path.match(/\n/g) || []).length;
    ok(innerLines === 5, `AsmTrace: inner scope at a shared name renders its own slice (${innerLines})`);
    ok(outerLines === 5,
      `AsmTrace: outer scope at the SAME name renders ITS OWN slice, not the freed inner's (${outerLines})`);
    code.free();
  }

  // --- the same hazard in its natural shape: recursion through ONE auto-named call
  //     site, where nothing but the language put two live scopes on one name ---
  {
    const code = NativeCode.fromBytes(ROUTINE);
    const lines = [];
    function traced(depth) {
      const t = new AsmTrace(code, { emit: false }); // ONE call-site-constant auto-name
      code.call(20, 22);
      if (depth > 0) traced(depth - 1);
      t.close();
      lines.push((t.path.match(/\n/g) || []).length);
    }
    traced(1);
    // lines[0] is the INNER (it closes first): its own 5-insn call. lines[1] is the
    // OUTER: its own call PLUS the nested one, which ran while it was still lexically
    // open and inside its range — 10. (Pre-fix this read freed memory and rendered 512.)
    ok(lines[0] === 5, `AsmTrace: recursive inner scope renders its own 5 lines (${lines[0]})`);
    ok(lines[1] === 10,
      `AsmTrace: recursive outer scope renders its own + the nested call, not freed garbage (${lines[1]})`);
    code.free();
  }

  // --- an UNARMED scope must not end(). `end()` is name-keyed but its single-step path
  //     closes the calling thread's TOP range-stack frame without checking it is the
  //     caller's, so a self-skipped scope (try_begin -> EFULL) that ends anyway pops a
  //     LIVE SIBLING's frame and silently disarms it mid-capture. ---
  {
    const code = NativeCode.fromBytes(ROUTINE);
    const live = [];
    let unarmed = null;
    // Fill this thread's range stack until a scope self-skips (EFULL). Probed rather
    // than hardcoded to SS_MAX_FRAMES so the test survives a change to that depth.
    for (let i = 0; i < 16 && unarmed === null; i++) {
      const t = new AsmTrace(code, { name: `depth${i}`, emit: false });
      if (t.armed) live.push(t); else unarmed = t;
    }
    if (unarmed === null) {
      console.log('# SKIP unarmed-close: could not fill the range stack to force a self-skip');
      for (let i = live.length - 1; i >= 0; i--) live[i].close();
    } else {
      const victim = live[live.length - 1]; // the TOP frame — what a stray end() pops
      unarmed.close();     // must not touch the range stack: it never armed
      code.call(20, 22);   // victim is still open, so this must land in ITS trace
      for (let i = live.length - 1; i >= 0; i--) live[i].close();
      ok(victim.path.length > 0,
        'AsmTrace: an unarmed scope closing does not disarm a live sibling (end is armed-guarded)');
      ok(live[0].path.length > 0, 'AsmTrace: the outermost live scope still captured');
    }
    code.free();
  }

  // --- the real `using` syntax (Node 24+) — self-skips on an older parser ---
  if (usingSyntaxSupported()) {
    require('./test_hwtrace_using_syntax.js').run({ ok, ROUTINE });
  } else {
    console.log(`# SKIP using-syntax scope: node ${process.version} does not parse \`using\` `
      + '(needs Node 24+, or Node 22 with --harmony-explicit-resource-management). The '
      + 'AsmTrace disposal asserted above is what `using` invokes at block exit; '
      + 'region()/scope() stay the version-independent callback fallback.');
  }

  HwTrace.shutdown();

  if (_failed) {
    console.log(`# FAILED ${_n} tests`);
    process.exit(1);
  }
  console.log(`1..${_n}`);
}

main();
