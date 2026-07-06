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
  HwTrace, NativeCode, Ptrace, Descent, CodeImage, SINGLESTEP, AMD_LBR,
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
