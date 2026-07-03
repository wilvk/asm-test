# Node.js binding

The [Node.js binding](https://github.com/wilvk/asm-test/tree/main/bindings/node)
drives asm-test via [`koffi`](https://koffi.dev) (prebuilt FFI, no node-gyp
build). The reusable module is
[`asmtest.js`](https://github.com/wilvk/asm-test/blob/main/bindings/node/asmtest.js)
— it keeps all koffi FFI inside and exposes the `Regs` / `Emu` / `EmuResult`
classes plus the Tier-2 `assert*` helpers.

It calls the opaque-handle FFI layer, so no C struct layout is mirrored in JS:
`asmtest_corpus_routine` for routine addresses, `asmtest_capture6` / `_fp2` +
`asmtest_regs_*` accessors for capture, and `asmtest_emu_call2` + accessors for
the emulator (faults as data: `faulted()`, plus `faultAddr()` / `faultKind()` —
against the `FaultKind` export — for where and why one hit). See
[Language bindings](../bindings.md) for the shared architecture.

## Setup

From the repository root, build the native library, then install koffi:

```sh
make shared-emu     # libasmtest_emu.{so,dylib} — capture trampoline + emulator + FFI accessors
cd bindings/node && npm install   # installs koffi (see package.json)
```

Point the module at the built libs via `ASMTEST_LIB` / `ASMTEST_CORPUS_LIB`.

## Usage

```js
const { Regs, Emu, assertRet, assertAbiPreserved, assertNoFault, assertEmuReg }
    = require('./asmtest');

// Native capture through the real ABI.
const r = new Regs();
r.capture6(fn, 40, 2);
assertRet(r, 42);
assertAbiPreserved(r);

// Emulator: faults are data, never a crash.
const e = new Emu();
const res = e.call2(fn, 40, 2);
assertNoFault(res);
assertEmuReg(res, 'rax', 42);
```

## In-line assembler (optional)

Pass a routine as an **assembly string**. The Keystone assembler is built into
`libasmtest_emu`, so this runs by default under `make node-test`;
`Emu#asmAvailable()` remains a defensive probe, false only against an
older/leaner lib.

```js
const { Emu, assemble, Arch, Syntax, AsmtestError } = require('./asmtest');

const e = new Emu();
if (e.asmAvailable()) {
  // Intel, up to six args; throws AsmtestError (with the Keystone diagnostic)
  // if the string fails to assemble.
  const res = e.callAsm('mov rax, rdi; add rax, rsi; ret', [40, 2]);  // res.reg('rax') === 42
  // AT&T syntax + an instruction cap:
  e.callAsm(src, [10, 20, 12], { syntax: Syntax.ATT, maxInsns: 0 });
  // Multi-arch text -> bytes (x86-64/arm64/riscv64/arm32):
  const a64 = assemble('ret', Arch.ARM64);
}
```

## Function reference

Every export of `asmtest.js`, with an example and its options. Handles (`Regs`,
`EmuResult`, `Trace`, `Guest`, `GuestResult`) are explicitly released with
`free()`; an `Emu`/`Guest` with `close()`. A routine reference is the pointer from
`corpusRoutine(name)` (or your own koffi-resolved address); the `callBytes`-family
takes a `Buffer`/array of machine code.

### Resolving routines

```js
const asm = require('./asmtest');
const fn = asm.corpusRoutine('add_signed');   // built-in fixture (needs ASMTEST_CORPUS_LIB)
new asm.Emu().asmAvailable();                  // is the in-line assembler in this build?
```

### Capture tier — `Regs`

```js
const r = new asm.Regs();
r.capture6(fn, 40, 2);                         // up to 6 integer args (default 0)
r.captureFp2(fn, 1.5, 2.25);                   // two doubles; FP return in r.fret()
r.captureVecF32(fn, [[1, 2, 3, 4]]);           // up to 8 vectors (four float32 lanes each)
r.ret();                  // integer return (rax) as a Number
r.fret();                 // scalar double return (xmm0)
r.vecF32(0);              // [l0,l1,l2,l3] lanes of vector register 0
r.flagSet('CF');          // condition flag by name (CF/PF/ZF/SF/OF)
r.abiPreserved();         // every callee-saved register restored
r.free();
```

### Emulator tier — `Emu` / `EmuResult`

```js
const e = new asm.Emu();                       // x86-64 Unicorn guest
let res = e.call2(fn, 40, 2);                  // routine address + two int args
res = e.callBytes(code, [40, 2]);              // raw bytes (Buffer/array), up to 6 int args
res = e.callFp(code, { iargs: [1], fargs: [1.5] });   // doubles -> xmm0..7
res = e.callVec(code, { iargs: [], vargs: [[1,2,3,4]] }); // 128-bit vecs -> xmm0..7
res = e.callWin64(code, [1, 2, 3, 4]);         // Microsoft x64 (rcx, rdx, r8, r9)

res.faulted();            // invalid access? (data, not a crash)
res.faultAddr();          // where (valid when faulted)
res.faultKind();          // a FaultKind value (None/Read/Write/Fetch)
res.reg('rax');           // any GP register, plus 'rip' / 'rflags'
res.xmmF64(0, 0);         // xmm lane as double (scalar FP return)
res.xmmF32(0, 0);         // xmm lane as float32 (vector return)
e.close();
```

* `call2` is the only path taking a routine **address** (two int args). The
  `callBytes`/`callFp`/`callVec`/`callWin64`/`callTraced` family takes raw code.
* `callFp` / `callVec` take an options object (`{ iargs, fargs }` /
  `{ iargs, vargs }`).

### Execution trace / coverage — `Trace`

```js
const t = new asm.Trace(4096, 4096);           // insns / blocks buffer caps
const res = e.callTraced(code, [1, 2], t);     // record while running
t.covered(0x0);           // was the basic block at this byte-offset entered?
t.free();
```

### Native tracing — `NativeTrace` (optional, DynamoRIO)

A separate, opt-in tier from the emulator's `Trace`: instead of tracing isolated
guest bytes under Unicorn, [`drtrace.js`](https://github.com/wilvk/asm-test/blob/main/bindings/node/drtrace.js)
brings DynamoRIO up **in-process** and records basic-block / instruction
coverage of host-native code as it runs inside this Node process. The surface is
`NativeTrace` / `NativeCode` (plus the `symbolDemo` fixture). It self-skips when
the tier isn't built or DynamoRIO can't be resolved, so guard every use with
`NativeTrace.available()`.

```js
const { NativeTrace, NativeCode, symbolDemo } = require('./drtrace');

if (!NativeTrace.available()) return;          // tier not built / no DynamoRIO
NativeTrace.initialize();                       // bring DynamoRIO up + take over

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two blocks)
const code = NativeCode.fromBytes(Buffer.from([
  0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00, 0x00, 0x00,
  0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3]));

const tr = NativeTrace.create(64, 64);          // blocks=64, instructions=64
tr.register('add2', code);
tr.region('add2', () => code.call(1, 2));       // record only inside the region
tr.covered(0);                                  // entry block entered?
tr.blockOffsets();                              // distinct block starts, first-seen order
tr.insnOffsets();                               // jle-taken path -> [0, 3, 6, 0xc, 0x11]
code.free();
tr.free();

// Symbol mode: trace a named exported function — always-on, no region/markers.
const sym = NativeTrace.create(64);
sym.registerSymbol('asmtest_symbol_demo', 256);
symbolDemo(3, 4) === 10;                         // 3*2+4, recorded as it runs
sym.covered(0);                                 // symbol entry block entered?
sym.free();

NativeTrace.shutdown();
```

* Under Node — a managed/JIT runtime — the in-process tier **self-skips at run
  time**: in-process DynamoRIO can't take over V8's background threads, so
  `available()` may return true yet tracing yields no coverage. Intel PT is the
  recommended backend on such hosts (see the central doc).
* Linux x86-64 only; full reference in [Native runtime tracing](../tracing/native-tracing.md).

### Hardware / single-step tracing — `HwTrace` (optional)

A sibling native tier ([`hwtrace.js`](https://github.com/wilvk/asm-test/blob/main/bindings/node/hwtrace.js))
records the **same** `asmtest_trace_t` coverage from the real CPU, but needs no
separate engine install: it defaults to the **single-step** backend (the CPU's
`EFLAGS.TF` trap flag), so `HwTrace.available(SINGLESTEP)` is true and it **traces
live on any x86-64 Linux** — CI and plain containers included — where the in-process
DynamoRIO tier self-skips under V8. Intel PT and AMD LBR are picked automatically on
the bare-metal hardware that has them.

```js
const { HwTrace, NativeCode, SINGLESTEP, BEST } = require('./hwtrace');

if (!HwTrace.available(SINGLESTEP)) return;     // self-skip off x86-64 Linux
HwTrace.init(SINGLESTEP);

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two blocks)
const code = NativeCode.fromBytes(Buffer.from([
  0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00, 0x00, 0x00,
  0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3]));

const tr = HwTrace.create({ blocks: 64, instructions: 64 });
tr.register('add2', code);
let r;
tr.region('add2', () => { r = code.call(20, 22); });   // 42; jle taken, dec skipped
tr.insnOffsets();                               // [0, 3, 6, 0xc, 0x11] — == Unicorn/PT
tr.covered(0);                                  // entry block entered?
code.free();
tr.free();

HwTrace.shutdown();
```

`HwTrace.resolve(BEST)` / `HwTrace.auto(BEST)` pick the host's most-faithful
available backend (Intel PT → AMD LBR → single-step), and `HwTrace.resolveTiers` /
`autoTier` extend the cascade across the DynamoRIO and emulator tiers. An
out-of-process `Ptrace` surface traces a method in a **separate** process
(fork-and-step, foreign-process attach + run-to-method, and `/proc`-map / jitdump
resolution) — the managed-runtime path. Full reference in
[Native runtime tracing](../tracing/native-tracing.md).

### Cross-arch guests — `Guest` / `GuestResult`

```js
const g = new asm.Guest('arm64');              // 'arm64' | 'riscv' | 'arm'
const gr = g.call(code, [40, 2]);              // raw bytes, ints in the guest ABI regs
gr.reg('x0');             // by name: x0/sp, a0/x10, r0…
gr.faulted();             // faults are data here too
g.callTraced(code, [1], t);                    // arm64 only (throws otherwise)
gr.free(); g.close();
```

### In-line assembler (optional)

```js
e.asmAvailable();                              // assembler compiled in?
let res = e.callAsm('mov rax, rdi; ret', [42]);                 // assemble x86-64 + run
res = e.callAsm(src, [10, 20], { syntax: asm.Syntax.ATT, maxInsns: 8 });
const bytes = asm.assemble('ret', asm.Arch.ARM64);             // text -> bytes, any arch
asm.asmError();           // last Keystone diagnostic ('' on success)
```

* `Emu#callAsm(src, args=[], { syntax=0, maxInsns=0 })` — assemble x86-64 `src` and
  run it (≤6 int args). Throws `AsmtestError` (Keystone diagnostic) on a bad string
  or a Keystone-free build.
* `assemble(src, arch=0, syntax=0, addr=0x00100000)` — assemble-only, returning a
  `Buffer`. `arch` is `Arch.{X86_64,ARM64,RISCV64,ARM32}`; `syntax` is
  `Syntax.{INTEL,ATT,NASM,MASM,GAS}`.

### Tier-2 assertions (throw `AsmtestError`)

```js
asm.assertRet(r, 42);                  // r.ret() === 42
asm.assertAbiPreserved(r);             // callee-saved restored
asm.assertFlag(r, 'CF', true);         // flag set/clear
asm.assertFp(r, 3.75);                 // r.fret() === 3.75
asm.assertVecF32(r, 0, [1, 2, 3, 4]);  // vector lanes of register 0
asm.assertNoFault(res);                // emulator run clean
asm.assertFault(res);                  // emulator run faulted
asm.assertEmuReg(res, 'rax', 42);      // x86 guest register
asm.assertGuestReg(gr, 'x0', 42);      // cross-arch guest register
asm.assertCovered(t, 0x0);             // basic block entered
```

## Run the tests

```sh
make node-test        # from the repo root (needs the shared libs + Node + koffi)
make docker-node      # or in an isolated container (koffi preinstalled)
```

`make node-test` builds `libasmtest_emu` + the routine fixture lib, then runs
[`conformance.js`](https://github.com/wilvk/asm-test/blob/main/bindings/node/conformance.js)
— a thin consumer that `require`s `asmtest.js` and replays the corpus — with
`ASMTEST_LIB` / `ASMTEST_CORPUS_LIB` set.

## Maturity

A published npm package with prebuilt binaries (and `vitest`/`jest` integration of
the `assert*` helpers) is future work; the reusable module with Tier-2 assertions
ships today. See [Packaging the bindings](../packaging.md).
