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
const fs = require('fs');
const os = require('os');
const path = require('path');
const koffi = require('koffi');
const {
  HwTrace, NativeCode, AddrChannel, Ptrace, Descent, CodeImage, SINGLESTEP, AMD_LBR,
  BEST, CEILING_FREE, ASMTEST_HW_EUNAVAIL,
  TIER_HWTRACE, TIER_EMULATOR, FIDELITY_NATIVE, FIDELITY_VIRTUAL,
  TRACE_BEST, TRACE_CEILING_FREE, TRACE_NATIVE_ONLY,
  DESCENT_RECORD_EDGES, DESCENT_DESCEND_KNOWN,
} = require('./hwtrace');

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
const ROUTINE = Buffer.from([0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D,
  0x64, 0x00, 0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3]);

// mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret  (19 back-edges > LBR's 16)
const LOOP = Buffer.from([0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00,
  0x48, 0x01, 0xF8, 0x48, 0xFF, 0xCE, 0x75, 0xF8, 0xC3]);

// Whole-window OOP test leaves: two 7-byte native "methods" the driver frame calls into.
const M1 = Buffer.from([0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0xC3]); // rax = rdi + rsi
const M2 = Buffer.from([0x48, 0x89, 0xF8, 0x48, 0x29, 0xF0, 0xC3]); // rax = rdi - rsi

// Build the self-contained 35-byte driver blob (the window frame), mirroring the C oracle
// test_stealth_windowed: mov edi,7; mov esi,3; movabs rax,a1; call rax; movabs rax,a2; call
// rax; ret. a1/a2 are the runtime addresses of M1/M2 (BigInt). m2(7,3)=4 is the frame return.
function buildWindowDriver(a1, a2) {
  const drv = Buffer.from([
    0xBF, 7, 0, 0, 0, // mov edi, 7
    0xBE, 3, 0, 0, 0, // mov esi, 3
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, // movabs rax, a1  (imm at offset 12)
    0xFF, 0xD0, // call rax
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, // movabs rax, a2  (imm at offset 24)
    0xFF, 0xD0, // call rax
    0xC3, // ret
  ]);
  drv.writeBigUInt64LE(BigInt(a1), 12);
  drv.writeBigUInt64LE(BigInt(a2), 24);
  return drv;
}

// Classify a captured absolute-address trace by range containment (no Capstone needed):
// which of the driver frame / leaf m1 / leaf m2 it hit, and the index each was first seen.
function windowContainment(insns, dvBase, dvLen, a1, a2) {
  let hitDrv = false, hitM1 = false, hitM2 = false, firstM1 = -1, firstM2 = -1;
  insns.forEach((at, i) => {
    if (at >= dvBase && at < dvBase + BigInt(dvLen)) hitDrv = true;
    if (at >= a1 && at < a1 + BigInt(M1.length)) { hitM1 = true; if (firstM1 < 0) firstM1 = i; }
    if (at >= a2 && at < a2 + BigInt(M2.length)) { hitM2 = true; if (firstM2 < 0) firstM2 = i; }
  });
  return { hitDrv, hitM1, hitM2, firstM1, firstM2 };
}

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

  // --- traceCallAuto: auto-escalating CALL-OWNING cross-tier trace. It SELF-MANAGES the
  //     tier lifecycle (init -> begin -> invoke -> end -> shutdown) internally, so it runs
  //     STANDALONE — outside the HwTrace.init/shutdown bracket below, with NO pre-arm (a
  //     pre-arm would double-init and tear the tier down). Off x86-64 Linux it self-skips
  //     with EUNAVAIL. Mirrors test_hwtrace.py::test_trace_call_auto_owns_the_call_and_completes. ---
  {
    const code = NativeCode.fromBytes(ROUTINE);
    const res = HwTrace.traceCallAuto(code, 20, 22); // 42 <= 100 -> jle taken, dec skipped
    ok(res.rc === 0 || res.rc === ASMTEST_HW_EUNAVAIL, 'traceCallAuto: rc in {OK, EUNAVAIL}');
    if (res.rc === 0) {
      ok(res.result === 42, 'traceCallAuto: add2(20,22).result == 42');
      ok(!res.truncated, 'traceCallAuto: not truncated (some tier captured the whole path)');
      ok(res.trace.covered(0), 'traceCallAuto: entry block 0 covered');
      ok(res.used !== null && res.used.tier === TIER_HWTRACE,
        'traceCallAuto: used.tier == TIER_HWTRACE');
      res.trace.free();
    }
    code.free();

    // A loop past the 16-taken-branch LBR window must STILL yield a complete trace (escalating
    // off the ceiling-bounded backend on an AMD host; the single-step floor completes it directly
    // elsewhere). mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret.
    const lcode = NativeCode.fromBytes(LOOP);
    const lres = HwTrace.traceCallAuto(lcode, 1, 25); // 25 back-edges > 16-deep window
    ok(lres.rc === 0 || lres.rc === ASMTEST_HW_EUNAVAIL, 'traceCallAuto(loop): rc in {OK, EUNAVAIL}');
    if (lres.rc === 0) {
      ok(lres.result === 25, 'traceCallAuto: loop(1,25).result == 25');
      ok(!lres.truncated, 'traceCallAuto: loop not truncated (escalated to a ceiling-free tier)');
      ok(lres.trace.covered(0x7), 'traceCallAuto: loop-body block 0x7 covered');
      lres.trace.free();
    }
    lcode.free();
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

    // --- scope: callback form with auto-name + render-on-close ---
    {
      const code = NativeCode.fromBytes(ROUTINE);
      const tr = HwTrace.create({ blocks: 64, instructions: 256 });
      let r;
      const res = tr.scope(code, () => { r = code.call(20, 22); }, { emit: false });
      ok(Number(r) === 42, 'scope: add2(20,22) == 42');
      ok(res.armed, 'scope: armed on an available backend');
      ok(!res.truncated, 'scope: not truncated');
      ok(res.path && res.path.length > 0, 'scope: render-on-close produced text');
      ok((res.path.match(/\n/g) || []).length === 5, 'scope: 5 rendered instruction lines');
      ok(res.path.includes('ret'), 'scope: rendered listing includes the ret');
      ok(res.name.startsWith('test_hwtrace.js:'), `scope: auto-name is basename:line (${res.name})`);
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
      // Three blocks {0, 0x7, 0xf}: entry, the jnz target (loop body), and the
      // loop-exit ret at 0xf — the fall-through immediately after the NOT-taken
      // jnz is its own block leader in the single-step partition (matching PT/DR/
      // Unicorn; see src/ss_backend.c ss_normalize).
      assert.deepStrictEqual(tr.blockOffsets(), [0x0, 0x7, 0xF]);
      ok(tr.blocksLen() === 3, 'blocksLen() == 3 (entry, loop body, exit ret)');
      ok(!tr.truncated(), '!truncated()');

      code.free();
      tr.free();
    }

    // --- callScoped: arm + call + disarm entirely in native code — registry-free,
    //     returning the call result and the executed body disassembly in one step.
    //     Mirrors test_hwtrace.py::test_call_scoped_traces_a_native_call. ---
    {
      const code = NativeCode.fromBytes(ROUTINE);
      const res = HwTrace.callScoped(code, 20, 22); // 42 <= 100 -> jle taken, dec skipped
      ok(res.result === 42, 'callScoped: add2(20,22).result == 42');
      ok(!res.truncated, 'callScoped: not truncated');
      if (res.path) { // non-empty when Capstone is present
        ok(res.path.includes('ret'), 'callScoped: rendered listing includes the ret');
        ok((res.path.match(/\n/g) || []).length === 5,
          'callScoped: 5 rendered instruction lines (taken path)');
      }
      // Registry-free: 40 calls must NOT exhaust the fixed MAX_REGIONS region table.
      let regFree = true;
      for (let i = 0; i < 40; i++) {
        if (HwTrace.callScoped(code, i, 1).result !== i + 1) regFree = false;
      }
      ok(regFree, 'callScoped: 40 registry-free calls each return i+1');
      code.free();
    }

    // --- window: region-free WHOLE-WINDOW capture (§Z1 — the empty-ctor
    //     `using (new AsmTrace())`). HONEST-BUT-NOISY: captures the FFI dispatch too,
    //     so the routine's absolute addresses are a SUBSET. Mirrors the C whole-window
    //     test (examples/test_hwtrace.c). ---
    {
      const code = NativeCode.fromBytes(ROUTINE);
      let r;
      // A generous cap so the noisy managed window (V8 + FFI dispatch) fits un-truncated.
      const res = HwTrace.window(() => { r = code.call(20, 22); }, 1 << 20); // 42 -> jle taken
      console.log(`# window: armed=${res.armed} truncated=${res.truncated} insns=${res.insns.length}`);
      ok(r === 42, 'window: the traced call still returns 42 (execution intact under TF)');
      if (res.armed) {
        ok(res.insns.length >= 5, 'window: captured instructions (routine + harness noise)');
        ok(res.path.length > 0, 'window: render_window produced disassembly text');
        // insns[] hold ABSOLUTE addresses. When the capture did NOT overflow, the
        // routine's own addresses [base+0,+3,+6,+0xc,+0x11] must all be present (a
        // SUBSET amid the noise). If it truncated, the routine may be past the prefix —
        // an honest best-effort outcome, so we only assert the subset on a clean capture.
        if (!res.truncated) {
          const base = BigInt(koffi.address(code.base));
          const want = [0, 3, 6, 0xc, 0x11].map((o) => base + BigInt(o));
          const got = new Set(res.insns.map((x) => x.toString()));
          ok(want.every((w) => got.has(w.toString())),
            "window: the routine's absolute addresses are all captured (subset)");
        } else {
          console.log('# note: window truncated (managed capture overflowed the buffer) '
            + '— skipping the exact-subset assert (honest best-effort)');
        }
      } else {
        console.log('# note: window self-skipped (begin_window unavailable)');
      }
      code.free();
    }

    // --- attributeWindow: whole-window capture + attribute the absolute addresses to caller-
    //     named regions. Two IDENTICAL-byte leaves A,B in distinct mappings — the named-region
    //     path (exact address range) splits them into SEPARATE buckets, which symbol/disasm-based
    //     attribution cannot. Mirrors the C oracle test_wholewindow_buckets. ---
    {
      const A = NativeCode.fromBytes(ROUTINE);
      const B = NativeCode.fromBytes(ROUTINE); // identical bytes, distinct mapping
      const aBase = BigInt(koffi.address(A.base));
      const bBase = BigInt(koffi.address(B.base));
      let ra, rb;
      const res = HwTrace.attributeWindow(
        () => { ra = A.call(20, 22); rb = B.call(30, 12); },
        [{ name: 'leafA', base: aBase, len: A.length }, { name: 'leafB', base: bBase, len: B.length }]);
      console.log(`# attributeWindow: armed=${res.armed} buckets=${res.buckets.length}`);
      ok(ra === 42 && rb === 42, 'attributeWindow: both traced leaves still return their results');
      if (!res.armed) {
        console.log('# SKIP attributeWindow: single-step tier unavailable (begin_window)');
      } else {
        const byName = new Map(res.buckets.map((b) => [b.label, b]));
        const la = byName.get('leafA');
        const lb = byName.get('leafB');
        ok(la && lb, 'attributeWindow: identical-byte leaves split into SEPARATE named buckets leafA/leafB');
        if (la && lb) {
          ok(la.count === 5n && lb.count === 5n,
            'attributeWindow: each named leaf bucket counts its 5 executed instructions');
        }
      }
      A.free();
      B.free();
    }
  } finally {
    HwTrace.shutdown();
  }

  // --- stitchHandles: the §D0.4 async-hop merge. HOST-INDEPENDENT (pure merge — no single-step,
  //     no Capstone, no PT): script two "hops" OUT of seq order and prove they merge back BY seq.
  //     Mirrors the C oracle test_stitch_slices. ---
  {
    const trA = HwTrace.create({ blocks: 16, instructions: 16 });
    const trB = HwTrace.create({ blocks: 16, instructions: 16 });
    trA.appendInsn(0).appendInsn(3).appendInsn(6); // hop A: seq 0
    trB.appendInsn(0).appendInsn(4).appendInsn(8); // hop B: seq 1
    // Pass the hops OUT of seq order (B then A); stitch must re-order by seq.
    const st = HwTrace.stitchHandles([trB, trA],
      { scopeIds: [7, 7], seqs: [1, 0], tids: [222, 111], versions: [9, 5] });
    assert.deepStrictEqual(st.insns, [0, 3, 6, 0, 4, 8]);
    ok(true, 'stitchHandles: merges hops BY seq (A[0,3,6] before B[0,4,8]) despite input order');
    ok(st.bounds.length === 2, 'stitchHandles: one slice bound per hop');
    ok(st.bounds[0].seq === 0 && st.bounds[0].insnOff === 0 && st.bounds[0].tid === 111
      && st.bounds[0].version === 5n, 'stitchHandles: bound[0] is hop A (seq 0, off 0, tid 111, v5)');
    ok(st.bounds[1].seq === 1 && st.bounds[1].insnOff === 3 && st.bounds[1].tid === 222
      && st.bounds[1].version === 9n, 'stitchHandles: bound[1] is hop B (seq 1, off 3, tid 222, v9)');
    trA.free();
    trB.free(); // free the hops only AFTER reading the merged result (shallow-copy lifetime)
  }

  // --- symbolizeBuckets + regionName: whole-window noise attribution. HOST-INDEPENDENT-ish
  //     (Linux /proc + a synthetic /tmp perf-map; no single-step/Capstone/PT/privilege). Mirrors
  //     the C oracle test_symbolize_bucket. ---
  if (process.platform !== 'linux') {
    console.log('# SKIP symbolize_bucket/region_name: Linux /proc + perf-map only');
  } else {
    const code = NativeCode.fromBytes(ROUTINE);
    const base = BigInt(koffi.address(code.base));
    const perfMap = `/tmp/perf-${process.pid}.map`;
    fs.writeFileSync(perfMap, '40000000 1000 MyJitMethod\n');
    try {
      // ips: base x3 (the self mapping), a JIT IP x2 (resolves to MyJitMethod via the perf-map),
      // and address 1 (unmapped -> [unknown]).
      const jit = 0x40000500n;
      const buckets = HwTrace.symbolizeBuckets([base, base, base, jit, jit, 1n]);
      const total = buckets.reduce((s, b) => s + Number(b.count), 0);
      ok(total === 6, 'symbolizeBuckets: every IP is bucketed (total count == 6)');
      const jitB = buckets.find((b) => b.label.includes('MyJitMethod'));
      ok(jitB && jitB.count === 2n, 'symbolizeBuckets: the 2 JIT IPs bucket under MyJitMethod (perf-map)');
      ok(buckets.some((b) => b.label.includes('unknown')), 'symbolizeBuckets: the unmapped IP buckets under [unknown]');
      ok(HwTrace.symbolizeBuckets([]).length === 0, 'symbolizeBuckets: empty input -> empty');
      // regionName reverse-resolves the self mapping's extent.
      const rn = HwTrace.regionName(base);
      ok(rn !== null && rn.start <= base && base < rn.end && rn.name.length > 0,
        'regionName: resolves the containing mapping name + extent');
    } finally {
      try { fs.unlinkSync(perfMap); } catch (_e) { /* ignore */ }
      code.free();
    }
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

  // --- Out-of-process / foreign-process toolkit (Ptrace). Mirrors the four
  //     tests after the "foreign-process toolkit" banner in test_hwtrace.py.
  //     Each self-skips when the ptrace backend is unavailable. ---
  if (!Ptrace.available()) {
    console.log(`# SKIP ptrace backend unavailable: ${Ptrace.skipReason()}`);
  } else {
    // Fork a tracee, single-step it out of process, get the same offsets.
    {
      const code = NativeCode.fromBytes(ROUTINE);
      const tr = HwTrace.create({ blocks: 64, instructions: 64 });
      const r = Ptrace.traceCall(code, code.length, [20, 22], tr);
      ok(Number(r) === 42, 'ptrace traceCall returns 42');
      assert.deepStrictEqual(tr.insnOffsets(), [0x0, 0x3, 0x6, 0xC, 0x11]);
      ok(true, 'ptrace traceCall insnOffsets() == [0, 3, 6, 12, 17]');
      ok(!tr.truncated(), 'ptrace traceCall !truncated()');
      code.free();
      tr.free();
    }

    // Exact-result guarantee: a routine returning a full 64-bit value ABOVE 2^53 must come back
    // as an exact BigInt, not a Number rounded through the double mantissa (the _safeInt path).
    {
      const BIG = 0x0102030405060708n; // 72623859790382856 > Number.MAX_SAFE_INTEGER
      // movabs rax, 0x0102030405060708 ; ret
      const bigLeaf = NativeCode.fromBytes(
        Buffer.from([0x48, 0xB8, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0xC3]));
      const tr = HwTrace.create({ blocks: 8, instructions: 8 });
      const r = Ptrace.traceCall(bigLeaf, bigLeaf.length, [], tr);
      ok(typeof r === 'bigint' && r === BIG,
        'traceCall: a 64-bit result > 2^53 returns an EXACT BigInt (not a rounded Number)');
      bigLeaf.free();
      tr.free();
    }

    // BTF block-step tier: one #DB per TAKEN branch, intra-block instructions
    // reconstructed with Capstone — the stream is byte-identical to the
    // per-instruction path above. Self-skips where PTRACE_SINGLEBLOCK / Capstone
    // are absent (e.g. AArch64).
    if (!Ptrace.blockstepAvailable()) {
      console.log('# SKIP BTF block-step unavailable (needs x86-64 PTRACE_SINGLEBLOCK + Capstone)');
    } else {
      const code = NativeCode.fromBytes(ROUTINE);
      const tr = HwTrace.create({ blocks: 64, instructions: 64 });
      const r = Ptrace.traceCallBlockstep(code, code.length, [20, 22], tr);
      ok(Number(r) === 42, 'ptrace traceCallBlockstep returns 42');
      assert.deepStrictEqual(tr.insnOffsets(), [0x0, 0x3, 0x6, 0xC, 0x11]);
      ok(true, 'ptrace traceCallBlockstep insn stream identical to single-step');
      ok(!tr.truncated(), 'ptrace traceCallBlockstep !truncated()');
      code.free();
      tr.free();
    }

    // §D3 stealth stepper (HwTrace.stealthTrace): a reverse-attached helper child
    // single-steps the region while THIS (V8) thread runs the leaf — so NO EFLAGS.TF is
    // armed on the caller. This is the CRASH-PROOF managed-capture route (a ptrace-stop
    // is not gated by the tracee's signal mask), the Node analog of dotnet's
    // AsmTrace.Method(..., outOfProcess: true). Needs no HwTrace.init. Self-skips when the
    // reverse-attach is refused (Yama ptrace_scope); otherwise reconstructs the identical
    // [0,3,6,c,11] ground-truth stream as the fork/single-step paths above.
    {
      const code = NativeCode.fromBytes(ROUTINE);
      const res = HwTrace.stealthTrace(code, 20, 22); // 42 <= 100 -> jle taken, dec skipped
      console.log(`# stealth: armed=${res.armed} truncated=${res.truncated} offsets=${res.offsets.length}`);
      if (!res.armed) {
        console.log('# SKIP ptrace stealth: reverse-attach not permitted (Yama ptrace_scope)');
      } else {
        // HARD guarantee: the out-of-band stepper reads the true return from the caller's RAX,
        // EXACT even when the instruction stream is best-effort — and TF is never armed on the
        // V8 thread (the crash-proof property). result + armed are the invariants.
        ok(res.result === 42, 'stealthTrace: add2(20,22).result == 42 (out of band, from caller RAX)');
        // Stream: EXACT when the reverse-attach single-step ran to completion. Over a LIVE
        // runtime it may truncate (the runtime's async signals interrupt the per-insn step) —
        // honest best-effort, mirroring dotnet's outOfProcess AsmTrace.Method and window(). Assert
        // the exact [0,3,6,c,11] stream only when it did NOT truncate.
        if (!res.truncated) {
          assert.deepStrictEqual(res.offsets, [0x0, 0x3, 0x6, 0xC, 0x11]);
          ok(res.blocks === 2,
            'stealthTrace: exact offsets [0,3,6,12,17] over two blocks (complete out-of-band capture)');
        } else {
          ok(true,
            'stealthTrace: stream truncated over the live runtime (honest best-effort; result still exact)');
        }
      }
      code.free();
    }

    // §D3 WHOLE-WINDOW fork-internal capture (Ptrace.windowCall): fork a child that runs the
    // driver frame; record the driver AND both channel-published leaves as ABSOLUTE addresses,
    // in call order. Fork-internal (steps a child, not this thread), so it asserts
    // unconditionally on any ptrace lane. Mirrors the C oracle test_ptrace_window_call.
    {
      const m1 = NativeCode.fromBytes(M1);
      const m2 = NativeCode.fromBytes(M2);
      const a1 = BigInt(koffi.address(m1.base)), a2 = BigInt(koffi.address(m2.base));
      const drv = NativeCode.fromBytes(buildWindowDriver(a1, a2));
      const chan = AddrChannel.newLocal();
      chan.publish(m1).publish(m2); // pre-publish the leaves the frame calls into
      const res = Ptrace.windowCall(drv, [7, 3], chan);
      ok(res.result === 4, 'windowCall: driver frame returns m2(7,3) == 4');
      const c = windowContainment(res.insns, BigInt(koffi.address(drv.base)), drv.length, a1, a2);
      ok(c.hitDrv && c.hitM1 && c.hitM2,
        'windowCall: records the driver frame AND both channel-published leaves');
      ok(c.firstM1 >= 0 && c.firstM2 > c.firstM1, 'windowCall: follows the calls in order (m1 before m2)');
      ok(!res.truncated, 'windowCall: capture complete');
      chan.free(); drv.free(); m1.free(); m2.free();
    }

    // §D3 CRASH-PROOF whole-window OOP capture (HwTrace.stealthWindow): a reverse-attached
    // helper steps the window body out of band while THIS thread runs it — the OOP analog of
    // the in-process window() footgun, mirroring dotnet's AsmTrace.Window. Self-skips on a
    // refused reverse-attach; else records driver + both leaves in order (best-effort stream
    // over a live runtime, exact result). Mirrors the C oracle test_stealth_windowed.
    {
      const m1 = NativeCode.fromBytes(M1);
      const m2 = NativeCode.fromBytes(M2);
      const a1 = BigInt(koffi.address(m1.base)), a2 = BigInt(koffi.address(m2.base));
      const drv = NativeCode.fromBytes(buildWindowDriver(a1, a2));
      const chan = AddrChannel.newShared(); // reverse-attach: the forked stepper drains it live
      chan.publish(m1).publish(m2);
      const res = HwTrace.stealthWindow(drv, chan);
      console.log(`# stealthWindow: armed=${res.armed} truncated=${res.truncated} insns=${res.insns.length}`);
      if (!res.armed) {
        console.log('# SKIP stealth windowed: reverse-attach not permitted (Yama ptrace_scope)');
      } else {
        ok(res.result === 4, 'stealthWindow: frame returns m2(7,3) == 4 (out of band)');
        if (!res.truncated) {
          const c = windowContainment(res.insns, BigInt(koffi.address(drv.base)), drv.length, a1, a2);
          ok(c.hitDrv && c.hitM1 && c.hitM2,
            'stealthWindow: records the driver frame AND both pre-published leaves');
          ok(c.firstM1 >= 0 && c.firstM2 > c.firstM1,
            'stealthWindow: follows the calls in order (m1 before m2)');
        } else {
          ok(true, 'stealthWindow: stream truncated over the live runtime (honest best-effort; result exact)');
        }
      }
      chan.free(); drv.free(); m1.free(); m2.free();
    }

    // run_to drives an attached target to a resolved method (software breakpoint). A
    // live foreign attach is covered by the C suite; exercise the FFI round-trip safely
    // — a NULL target address is rejected (EINVAL, non-zero) before any ptrace call.
    ok(Ptrace.runTo(process.pid, 0) !== 0,
      'ptrace runTo(NULL addr) rejected (EINVAL) via the FFI round-trip');

    // Discover an executable region's extent from /proc/<pid>/maps by an interior
    // address (this process). traceAttached needs no live test — it is declared in
    // hwtrace.js (the parity gate references the symbol); only assert it exists.
    {
      ok(typeof Ptrace.traceAttached === 'function', 'ptrace traceAttached is declared');
      const code = NativeCode.fromBytes(ROUTINE);
      // koffi external pointers don't compare by value, so work in numeric
      // addresses: koffi.address() resolves a pointer to its BigInt address, and a
      // numeric/BigInt address marshals straight into a void* parameter.
      const baseAddr = koffi.address(code.base);
      const region = Ptrace.procRegionByAddr(process.pid, baseAddr + 4n); // interior addr
      ok(region !== null, 'procRegionByAddr finds the mapping containing base+4');
      ok(koffi.address(region.base) === baseAddr, 'procRegionByAddr base == code base');
      ok(region.len >= ROUTINE.length, 'procRegionByAddr length >= 18');
      ok(Ptrace.procRegionByAddr(process.pid, 1) === null,
        'procRegionByAddr(addr=1) is null (nothing maps it)');
      code.free();
    }

    // Parse a JIT perf-map (/tmp/perf-<pid>.map) and resolve a method by name.
    {
      const pid = process.pid;
      const mapPath = `/tmp/perf-${pid}.map`;
      fs.writeFileSync(mapPath, '400000 1a void demo(long, long)\n500000 8 other\n');
      try {
        const m = Ptrace.procPerfmapSymbol(pid, 'void demo(long, long)');
        ok(m !== null && koffi.address(m.base) === 0x400000n && m.len === 0x1A,
          'procPerfmapSymbol resolves demo to (0x400000, 0x1a)');
        ok(Ptrace.procPerfmapSymbol(pid, 'missing') === null,
          'procPerfmapSymbol(missing) is null');
      } finally {
        fs.unlinkSync(mapPath);
      }
    }

    // Read a binary jitdump and resolve a method to (addr,size,index) + bytes.
    {
      const dumpPath = path.join(os.tmpdir(), `asmtest-jit-${process.pid}.dump`);
      const name = Buffer.from('void demo(long, long)', 'utf8');
      // header: magic, version, total_size=40, elf_mach, pad1, pid, timestamp, flags
      const header = Buffer.alloc(40);
      header.writeUInt32LE(0x4A695444, 0); // magic 'JiTD' (little-endian read order)
      header.writeUInt32LE(1, 4);          // version
      header.writeUInt32LE(40, 8);         // total_size (header)
      header.writeUInt32LE(62, 12);        // elf_mach (EM_X86_64)
      header.writeUInt32LE(0, 16);         // pad1
      header.writeUInt32LE(0, 20);         // pid
      header.writeBigUInt64LE(0n, 24);     // timestamp
      header.writeBigUInt64LE(0n, 32);     // flags
      // JIT_CODE_LOAD record header: id=0, total_size, timestamp=5
      const total = 16 + 40 + (name.length + 1) + ROUTINE.length;
      const recHdr = Buffer.alloc(16);
      recHdr.writeUInt32LE(0, 0);          // id (JIT_CODE_LOAD)
      recHdr.writeUInt32LE(total, 4);      // total_size
      recHdr.writeBigUInt64LE(5n, 8);      // timestamp
      // body: pid, tid, vma, code_addr, code_size, code_index
      const body = Buffer.alloc(8 + 8 * 4);
      body.writeUInt32LE(0, 0);            // pid
      body.writeUInt32LE(0, 4);            // tid
      body.writeBigUInt64LE(0x2000n, 8);   // vma
      body.writeBigUInt64LE(0x2000n, 16);  // code_addr
      body.writeBigUInt64LE(BigInt(ROUTINE.length), 24); // code_size
      body.writeBigUInt64LE(9n, 32);       // code_index
      const nameNul = Buffer.concat([name, Buffer.from([0])]);
      fs.writeFileSync(dumpPath,
        Buffer.concat([header, recHdr, body, nameNul, ROUTINE]));
      try {
        const m = Ptrace.jitdumpFind(dumpPath, 'void demo(long, long)', 0, 64);
        ok(m !== null, 'jitdumpFind resolves the method');
        // codeAddr/codeSize are BigInt (lossless 64-bit); codeIndex/timestamp stay Number.
        ok(m.codeAddr === 0x2000n && m.codeSize === BigInt(ROUTINE.length)
          && m.codeIndex === 9 && m.timestamp === 5,
          'jitdumpFind (codeAddr,codeSize,codeIndex,timestamp) == (0x2000,18,9,5)');
        assert.deepStrictEqual(m.code, ROUTINE);
        ok(true, 'jitdumpFind code bytes == ROUTINE');
        ok(Ptrace.jitdumpFind(dumpPath, 'missing') === null,
          'jitdumpFind(missing) is null');
      } finally {
        fs.unlinkSync(dumpPath);
      }
    }

    // --- Call descent (Descent + Ptrace.traceCallEx): edges (L1) and descended
    //     nested frames (L2), plus a GC-safe resolver upcall. Mirrors
    //     test_hwtrace.py::test_descent_edges_and_frames. Fixture: R@0 mov rax,rdi;
    //     call S(+4); add rax,rsi; ret.  S@0xc inc rax; ret. region=0xc keeps the
    //     in-blob sibling S OUTSIDE the traced region (passing the whole allocation
    //     would fold S into R and mis-record the call as recursion). ---
    {
      const FIX = Buffer.from([0x48, 0x89, 0xF8, 0xE8, 0x04, 0x00, 0x00, 0x00,
        0x48, 0x01, 0xF0, 0xC3, 0x48, 0xFF, 0xC0, 0xC3]);

      // L1 RECORD_EDGES, region=0xc: frame 0 only, one edge (site 3 -> base+0xc).
      {
        const code = NativeCode.fromBytes(FIX);
        const baseAddr = koffi.address(code.base); // BigInt absolute base
        const d = new Descent(DESCENT_RECORD_EDGES);
        const tr = HwTrace.create({ blocks: 64, instructions: 64 });
        const r = Ptrace.traceCallEx(code, [20, 22], tr, d, 0xc);
        ok(Number(r) === 43, 'descent L1 traceCallEx(region=0xc) returns 43');
        ok(d.framesLen() === 1, 'descent L1 framesLen() == 1 (edge only, no descent)');
        assert.deepStrictEqual(d.frameInsns(0), [0n, 3n, 8n, 0xBn]);
        ok(true, 'descent L1 frameInsns(0) == [0, 3, 8, 0xb] (BigInt)');
        const edges = d.edges();
        ok(edges.length === 1 && edges[0].site === 3 && edges[0].depth === 0,
          'descent L1 one edge: site 3, depth 0');
        ok(typeof edges[0].target === 'bigint' && edges[0].target === baseAddr + 0xCn,
          'descent L1 edge target == code_base+0xc (BigInt absolute address)');
        ok(!d.truncated(), 'descent L1 !truncated()');
        d.free(); tr.free(); code.free();
      }

      // L2 DESCEND_KNOWN + allow_region(base+0xc, 4): the leaf S is descended as
      // frame 1 (depth 1) and the edge is consumed (edges() empty).
      {
        const code = NativeCode.fromBytes(FIX);
        const baseAddr = koffi.address(code.base);
        const d = new Descent(DESCENT_DESCEND_KNOWN);
        d.allowRegion(baseAddr + 0xCn, 4);
        const tr = HwTrace.create({ blocks: 64, instructions: 64 });
        const r = Ptrace.traceCallEx(code, [20, 22], tr, d, 0xc);
        ok(Number(r) === 43, 'descent L2 traceCallEx(region=0xc) returns 43');
        ok(d.framesLen() === 2, 'descent L2 framesLen() == 2 (leaf S descended)');
        assert.deepStrictEqual(d.frameInsns(0), [0n, 3n, 8n, 0xBn]);
        ok(true, 'descent L2 frameInsns(0) == [0, 3, 8, 0xb]');
        ok(typeof d.frameBase(1) === 'bigint' && d.frameBase(1) === baseAddr + 0xCn,
          'descent L2 frameBase(1) == code_base+0xc (BigInt)');
        ok(d.frameDepth(1) === 1, 'descent L2 frameDepth(1) == 1');
        assert.deepStrictEqual(d.frameInsns(1), [0n, 3n]);
        ok(true, 'descent L2 frameInsns(1) == [0, 3]');
        ok(d.edges().length === 0, 'descent L2 edges() == [] (edge consumed by descent)');
        d.free(); tr.free(); code.free();
      }

      // Resolver upcall (resolver-capable binding): at L2 with an EMPTY allow-set a
      // koffi.register'd resolver decides. It MUST fire (proving the retained
      // trampoline survives the single-step / is not GC'd) and, by writing the
      // callee region back through the out-params, drive the same leaf descent.
      {
        const code = NativeCode.fromBytes(FIX);
        const baseAddr = koffi.address(code.base);
        const d = new Descent(DESCENT_DESCEND_KNOWN);
        let firedWith = null;
        d.setResolver((callee) => {
          firedWith = callee;
          return callee === baseAddr + 0xCn ? { base: baseAddr + 0xCn, len: 4 } : null;
        });
        const tr = HwTrace.create({ blocks: 64, instructions: 64 });
        const r = Ptrace.traceCallEx(code, [20, 22], tr, d, 0xc);
        ok(Number(r) === 43, 'descent resolver traceCallEx returns 43');
        ok(firedWith === baseAddr + 0xCn,
          'descent resolver upcall FIRED with callee == code_base+0xc (BigInt)');
        ok(d.framesLen() === 2 && d.frameBase(1) === baseAddr + 0xCn,
          'descent resolver drove the leaf descent (frame 1 == code_base+0xc)');
        d.free(); tr.free(); code.free();
      }

      // >2^53 round-trip: a uint64 accessor must survive a value past the JS safe-
      // integer boundary as a lossless BigInt — a real >2^53 ASLR address would
      // round if read as Number. The descent address getters read through this same
      // koffi uint64 path; prove it with a routine that returns 2^53+1 in RAX.
      {
        const BIG = 0x0020000000000001n; // 2^53 + 1
        const ret = NativeCode.fromBytes(Buffer.from(
          [0x48, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0xC3])); // mov rax,BIG; ret
        const got = koffi.decode(ret.base, koffi.proto('uint64_t u64probe()'))();
        ok(typeof got === 'bigint' && got === BIG,
          'uint64 address path round-trips a >2^53 value losslessly as BigInt');
        ret.free();
      }
    }
  }

  // --- Time-aware code-image recorder (CodeImage, asmtest_codeimage.h): a userspace
  //     PERF_RECORD_TEXT_POKE. Self-skips when the userspace page-change recorder
  //     can't run on this host (no PAGEMAP_SCAN / soft-dirty). ---
  if (!CodeImage.available()) {
    console.log(`# SKIP codeimage recorder unavailable: ${CodeImage.skipReason()}`);
  } else {
    // Track a NativeCode region in THIS process (pid 0) and round-trip its bytes
    // back out of the version-0 snapshot. The 7-byte routine is mov rax,rdi;
    // add rax,rsi; ret.
    const SNIPPET = Buffer.from([0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0xC3]);
    {
      const code = NativeCode.fromBytes(SNIPPET);
      // koffi external pointers don't compare by value; work in numeric addresses.
      const baseAddr = koffi.address(code.base);
      const img = new CodeImage(0); // 0 => this process
      try {
        ok(img.track(baseAddr, SNIPPET.length) === 0, 'codeimage track(base, 7) == OK');
        ok(img.now() >= 1, 'codeimage now() >= 1 after track (version 0 recorded)');
        ok(img.refresh() >= 0, 'codeimage refresh() >= 0 (no error)');

        const bytes = img.bytesAt(baseAddr, 0); // when 0 => latest
        ok(bytes !== null && bytes.length >= SNIPPET.length,
          'codeimage bytesAt(base, 0) returns at least 7 bytes');
        let same = bytes !== null;
        for (let i = 0; same && i < SNIPPET.length; i++) {
          if (bytes[i] !== SNIPPET[i]) same = false;
        }
        ok(same, 'codeimage bytesAt(base, 0) round-trips the 7 snippet bytes');
      } finally {
        img.free();
        code.free();
      }
    }

    // render_versioned: version-AWARE disassembly. Track a WRITABLE region as 'add', then
    // rewrite it to 'sub' and refresh (a 2nd version). Rendering a trace of the same ABSOLUTE
    // address at the OLD version shows 'add', at the NEW version shows 'sub' — proving the render
    // decodes the timeline SNAPSHOT, not live memory (which would show only 'sub'). Mirrors the C
    // oracle test_render_versioned. Self-skips without a 2nd version or without Capstone.
    {
      const CIA = Buffer.from([0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0xC3]); // mov rax,rdi; add rax,rsi; ret
      const region = Buffer.alloc(4096); // writable + address-stable (off-heap ArrayBuffer backing)
      CIA.copy(region, 0);
      const addr = koffi.address(region); // BigInt data address
      const img = new CodeImage(0);
      const tr = HwTrace.create({ blocks: 4, instructions: 4 });
      try {
        img.track(addr, CIA.length);
        const t0 = img.now();
        region.writeUInt8(0x29, 4); // add (0x01) -> sub (0x29) in place
        img.refresh();
        const t1 = img.now();
        tr.appendInsn(addr + 3n); // ABSOLUTE address of the add/sub instruction (offset 3)
        const b0 = img.renderVersioned(t0, tr);
        const b1 = img.renderVersioned(t1, tr);
        if (t1 <= t0) {
          console.log('# SKIP render_versioned: recorder saw no page change (no 2nd version)');
        } else if (!b0 || !b1) {
          console.log('# SKIP render_versioned: Capstone decoder absent (render returned empty)');
        } else {
          ok(b0.includes('add'), 'render_versioned at t0 shows add (version A bytes)');
          ok(b1.includes('sub'), 'render_versioned at t1 shows sub (version B bytes)');
          ok(b0 !== b1, 'render_versioned is version-aware (t0 text != t1 text)');
        }
      } finally {
        tr.free();
        img.free();
      }
    }

    // eBPF emission detector (Phase C) probe: skip without libbpf / CAP_BPF, else
    // the program loads and attaches (watchBpf() == OK).
    if (!CodeImage.bpfAvailable()) {
      console.log(`# SKIP codeimage eBPF detector unavailable: ${CodeImage.bpfSkipReason()}`);
    } else {
      const img = new CodeImage(0);
      try {
        ok(img.watchBpf() === 0, 'codeimage watchBpf() == OK (eBPF program loaded + attached)');
      } finally {
        img.free();
      }
    }
  }

  if (_failed) {
    console.log(`# FAILED ${_n} tests`);
    process.exit(1);
  }
  console.log(`1..${_n}`);
}

main();
