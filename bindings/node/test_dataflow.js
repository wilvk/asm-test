// Node data-flow binding smoke: GC-move canonicalizer + method resolver + the
// L0/L1/L2 pipeline, mirroring the Python/C++ suites' semantics — and (F7) a REAL
// live attach to a victim process by pid. Self-skips when the lib is not built;
// the live section does NOT self-skip on a linux/x64 host (see its comment).
'use strict';

const df = require('./dataflow');
const koffi = require('koffi');

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

// --- L0->L1->L2 pipeline (ValueTrace: build -> def-use -> slice) --- //
function setEq(a, b) {
  if (a.size !== b.size) return false;
  for (const x of b) if (!a.has(x)) return false;
  return true;
}
const REG = df.LOC_REG;
const MEM = df.LOC_MEM_ABS;
{
  const vt = new df.ValueTrace();
  vt.step(0x00, [], [[REG, 10]]); // def r10
  vt.step(0x03, [[REG, 10]], [[REG, 11]]); // r11 <- r10
  vt.step(0x06, [[REG, 11]], [[REG, 12]]); // r12 <- r11
  check(setEq(vt.forwardSlice(0), new Set([0, 1, 2])), 'pipeline: reg move chain forward slice');
  check(setEq(vt.backwardSlice(2), new Set([0, 1, 2])), 'pipeline: reg move chain backward slice');
  check(setEq(vt.forwardSlice(2), new Set([2])), 'pipeline: nothing downstream of the tail');
  vt.free();
}
{
  const vt = new df.ValueTrace();
  vt.step(0x00, [[REG, 8]], [[MEM, 0x7fff0000]]);
  vt.step(0x04, [[MEM, 0x7fff0000]], [[REG, 9]]);
  check(setEq(vt.forwardSlice(0), new Set([0, 1])), 'pipeline: load-after-store edge through memory');
  vt.free();
}
{
  const vt = new df.ValueTrace(); // independent chains must not cross-link
  vt.step(0x00, [], [[REG, 1]]);
  vt.step(0x02, [[REG, 1]], [[REG, 2]]);
  vt.step(0x04, [], [[REG, 3]]);
  vt.step(0x06, [[REG, 3]], [[REG, 4]]);
  check(setEq(vt.forwardSlice(0), new Set([0, 1])), 'pipeline: no spurious cross-link');
  vt.free();
}

// ---------------------------------------------------------------------------
// T4 — the code-image recorder (asmtest_codeimage.h): track a buffer in THIS
// process and read back the exact bytes it snapshotted. Runs wherever
// soft-dirty page tracking is available -- no ptrace, so it runs even where
// the live-attach section below self-skips.
// ---------------------------------------------------------------------------
if (!df.CodeImage.available()) {
  console.log(`# SKIP codeimage: ${df.CodeImage.skipReason()}`);
} else {
  const cbuf = Buffer.alloc(16); // off-heap ArrayBuffer backing: address-stable
  for (let i = 0; i < 16; i++) cbuf[i] = 0xA0 + i;
  const addr = koffi.address(cbuf);
  const img = new df.CodeImage(0);
  const trc = img.track(addr, 16);
  check(trc === df.ASMTEST_CI_OK, 'codeimage: track() snapshots v0');
  const t0 = img.now();
  check(t0 > 0, 'codeimage: now() advanced past 0 after track');
  const got = img.bytesAt(addr, t0);
  check(got !== null && got.equals(cbuf), 'codeimage: bytes_at() returns the exact tracked bytes');
  img.free();
}

// ---------------------------------------------------------------------------
// F7 — live-attach data flow: capture over a REAL attached pid.
//
// Every assertion below is POSITIVE and keyed to something only a working capture
// can produce (the region's return value, the exact step count, the def-use shape).
// None of it hides behind "if we captured anything" — an EMPTY capture IS the
// failure signature, so a guard like that skips exactly when it should shout.
// ---------------------------------------------------------------------------
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

// A real blocking sleep: this suite is a straight-line script, so it cannot await.
// Atomics.wait on a never-notified SharedArrayBuffer is the standard synchronous
// sleep and needs no child process.
function sleepMs(ms) {
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
}

// The tier is Linux x86-64 only (src/dataflow_ptrace.c's own #if). On such a host
// the live tests MUST run: an unavailable tier there means the lib was linked
// without Capstone — a build defect that has to be RED, not a skip.
const liveExpected = process.platform === 'linux' && process.arch === 'x64';
const victimExe = process.env.ASMTEST_DATAFLOW_VICTIM;

// A live victim: spawn it, learn its region base. `a`/`b` are OURS, so the expected
// result is a property of THIS run — a wrapper that hardcodes an answer cannot
// satisfy two victims with different args.
function spawnVictim(tag, a, b) {
  const counterPath = path.join(os.tmpdir(), `asmtest-df-node-${tag}.counter`);
  const outPath = path.join(os.tmpdir(), `asmtest-df-node-${tag}.hs`);
  // Redirect the victim's stdout to a FILE and poll it for the handshake line
  // ("base=0x<hex> len=<n>"), rather than reading a pipe: node's synchronous
  // stream reads are not portable here (proc.stdout.fd is undefined on node 18,
  // and this suite has no event loop to await on).
  const fd = fs.openSync(outPath, 'w');
  const proc = spawn(victimExe, [counterPath, String(a), String(b)], {
    stdio: ['ignore', fd, 'inherit'],
  });
  fs.closeSync(fd);
  let line = '';
  for (let i = 0; i < 500 && !line; i++) {
    const s = fs.readFileSync(outPath, 'latin1');
    const nl = s.indexOf('\n');
    if (nl >= 0) line = s.slice(0, nl);
    else sleepMs(10);
  }
  const m = /^base=0x([0-9a-f]+) len=(\d+) pid=(\d+)$/.exec(line);
  if (!m) throw new Error(`victim handshake failed: ${JSON.stringify(line)}`);
  return {
    proc,
    // The victim's OWN pid (see bindings/dataflow_victim.c) — right in every
    // binding, including those whose spawn goes through a shell.
    pid: Number(m[3]),
    base: BigInt('0x' + m[1]),
    len: Number(m[2]),
    counter: () => fs.readFileSync(counterPath).readBigUInt64LE(0),
    kill: () => proc.kill('SIGKILL'),
  };
}

function setEqA(s, arr) {
  return s.size === arr.length && arr.every((x) => s.has(x));
}

// ETRACE is NOT a skip. ptrace is a capability the lane can be GIVEN
// (--cap-add=SYS_PTRACE / seccomp=unconfined), and the victim opts in via
// PR_SET_PTRACER_ANY, so a refusal means the lane is misconfigured — be loud.
function checkRc(rc, what) {
  if (rc === df.PTRACE_ETRACE) {
    console.log(`# ${what}: ptrace refused (ETRACE) — the lane needs ` +
      '--cap-add=SYS_PTRACE; this is NOT a valid skip');
  }
  check(rc === df.PTRACE_OK, what);
}

if (!liveExpected) {
  console.log('# SKIP live-attach: not linux/x64 (the tier is Linux x86-64 only)');
} else if (!victimExe) {
  // The lane always exports this; missing means a misconfigured lane, and silently
  // skipping every live test is the hole this suite must not have.
  console.log('Bail out! ASMTEST_DATAFLOW_VICTIM unset; run `make dataflow-node-test`');
  process.exit(1);
} else {
  // Probed, not a symbol-resolves check: EINVAL (real) vs ENOSYS (stub).
  check(df.liveAttachAvailable(), 'live: tier is real on linux/x64 (EINVAL, not ENOSYS)');

  {
    const vic = spawnVictim('1', 7, 5);
    const vt = new df.ValueTrace(64, 512);
    const r = vt.attachPid(vic.pid, vic.base, vic.len);
    checkRc(r.rc, 'live: attach_pid a FOREIGN running pid + stepped the region');
    // The region really executed IN the victim: rax = rdi + rsi.
    check(r.result === 12, `live: attach_pid region returned 12 (got ${r.result})`);
    // Exactly df_chain's six in-region instructions — not "some".
    check(vt.steps === 6, `live: six in-region steps captured (got ${vt.steps})`);
    check(vt.recs > 0, 'live: operand records captured');

    // SURVIVAL: we attached to a process we do not own; it must outlive the detach.
    const c0 = vic.counter();
    sleepMs(50);
    check(vic.counter() > c0, 'live: victim SURVIVED the detach (counter advanced)');

    // The def-use over the LIVE trace has the real shape: the store at step 1 feeds
    // the load at step 2 (a MEMORY edge through [rsp-8]), which chains onward.
    check(setEqA(vt.backwardSlice(4), [0, 1, 2, 3, 4]),
      'live: backward slice(step4) = {0,1,2,3,4} over the live capture');
    const fwd = vt.forwardSlice(0);
    check(fwd.has(4), 'live: forward slice(step0) reaches the final mov');
    // Negative control on the SHAPE: the `ret` consumes none of the chain, so
    // reaching it would mean the graph links everything to everything.
    check(!fwd.has(5), 'live: forward slice(step0) excludes the ret');
    vt.free();
    vic.kill();
  }
  {
    // THE anti-hardcode control: a second victim, different args, same wrapper.
    const vic = spawnVictim('2', 17, 25);
    const vt = new df.ValueTrace(64, 512);
    const r = vt.attachPid(vic.pid, vic.base, vic.len);
    checkRc(r.rc, 'live: attach_pid the second victim');
    check(r.result === 42, `live: result TRACKS the victim's args (17+25=42, got ${r.result})`);
    check(vt.steps === 6, 'live: six steps on the second victim too');
    vt.free();
    vic.kill();
  }
  {
    const vic = spawnVictim('3', 9, 4);
    const vt = new df.ValueTrace(64, 512);
    // onlyTid 0: step whichever thread enters the region (here, the only one).
    const r = vt.attachPidTid(vic.pid, 0, vic.base, vic.len);
    checkRc(r.rc, 'live: attach_pid_tid stepped the entering thread');
    check(r.result === 13, `live: attach_pid_tid region returned 13 (got ${r.result})`);
    check(vt.steps === 6, 'live: attach_pid_tid captured six steps');
    vt.free();
    vic.kill();
  }
  {
    const vic = spawnVictim('4', 20, 3);
    const vt = new df.ValueTrace(64, 512);
    const r = vt.attachJit(vic.pid, 0, vic.base, vic.len);
    checkRc(r.rc, 'live: attach_jit stepped the region');
    check(r.result === 23, `live: attach_jit region returned 23 (got ${r.result})`);
    check(vt.steps === 6, 'live: attach_jit captured six steps');
    // The producer's OWN survival report — the house rule that a foreign target is
    // never killed, asserted from its side.
    check(r.survived === 1, 'live: attach_jit reported the target as survived');
    const c0 = vic.counter();
    sleepMs(50);
    check(vic.counter() > c0, 'live: attach_jit victim kept running after detach');
    vt.free();
    vic.kill();
  }
  if (df.CodeImage.available()) {
    // T4 — a real code-image threaded through attachPidVersioned: build the
    // recorder over the victim's OWN published region, then decode the capture
    // against it. A non-null img must not break the capture or land in the
    // wrong argument slot (a dropped/misplaced pointer would corrupt base/pid
    // and the result assert below would catch it).
    const vic = spawnVictim('5', 11, 6);
    const img = new df.CodeImage(vic.pid);
    const trc = img.track(vic.base, vic.len);
    check(trc === df.ASMTEST_CI_OK, "codeimage: track() over the victim's published region");
    const vt = new df.ValueTrace(64, 512);
    const when0 = img.now();
    const r = vt.attachPidVersioned(vic.pid, vic.base, vic.len, img, when0);
    checkRc(r.rc, 'live: attach_pid_versioned with a real img');
    check(r.result === 17,
      `live: attach_pid_versioned result TRACKS the victim's args (11+6=17, got ${r.result})`);
    check(vt.steps === 6, 'live: attach_pid_versioned captured six steps with a real img');
    img.free();
    vt.free();
    vic.kill();
  } else {
    console.log(`# SKIP codeimage live: ${df.CodeImage.skipReason()}`);
  }
  {
    // Negative control: the wrapper must surface the producer's rejections rather
    // than manufacture success.
    const vt = new df.ValueTrace(8, 8);
    check(vt.attachPid(12345, 0x1000, 0).rc === df.PTRACE_EINVAL,
      'live: zero-length region is rejected (EINVAL)');
    check(vt.attachPid(0, 0x1000, 21).rc === df.PTRACE_EINVAL,
      'live: pid 0 is rejected (EINVAL)');
    check(vt.attachPid(0x7ffffff0, 0x1000, 21).rc !== df.PTRACE_OK,
      'live: attaching to a nonexistent pid never returns OK');
    vt.free();
  }
}

console.log('1..' + n);
process.exit(failed ? 1 : 0);
