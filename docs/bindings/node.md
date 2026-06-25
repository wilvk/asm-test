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
const { Regs, Emu, assertRet, assertABIPreserved, assertNoFault, assertEmuReg }
    = require('./asmtest');

// Native capture through the real ABI.
const r = new Regs();
r.capture6(fn, 40, 2);
assertRet(r, 42);
assertABIPreserved(r);

// Emulator: faults are data, never a crash.
const e = new Emu();
const res = e.call2(fn, 40, 2);
assertNoFault(res);
assertEmuReg(res, 'rax', 42);
```

## In-line assembler (optional)

Pass a routine as an **assembly string**. Present only in the Keystone-carrying
`libasmtest_emu_asm` (`make node-asm-test` points `ASMTEST_LIB` at it);
`Emu#asmAvailable()` is false against the plain lib.

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

## Run the tests

```sh
make node-test        # from the repo root (needs the shared libs + Node + koffi)
make docker-node      # or in an isolated container (koffi preinstalled)
make node-asm-test    # points ASMTEST_LIB at libasmtest_emu_asm
```

`make node-test` builds `libasmtest_emu` + the routine fixture lib, then runs
[`conformance.js`](https://github.com/wilvk/asm-test/blob/main/bindings/node/conformance.js)
— a thin consumer that `require`s `asmtest.js` and replays the corpus — with
`ASMTEST_LIB` / `ASMTEST_CORPUS_LIB` set.

## Maturity

A published npm package with prebuilt binaries (and `vitest`/`jest` integration of
the `assert*` helpers) is future work; the reusable module with Tier-2 assertions
ships today. See [Packaging the bindings](../packaging.md).
