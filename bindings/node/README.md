# asm-test — Node.js binding

Run, **capture**, **emulate**, and **assemble** assembly routines through the
[asm-test](https://github.com/wilvk/asm-test) framework from Node.js, via
[`koffi`](https://koffi.dev) (prebuilt FFI, no node-gyp build).

It calls the opaque-handle FFI layer (`src/ffi.c`), so no C struct layout is
mirrored in JS: `asmtest_corpus_routine` for routine addresses, `asmtest_capture6`
/ `_fp2` + `asmtest_regs_*` accessors for capture, and `asmtest_emu_call2` +
accessors for the emulator (faults as data).

## Run

```sh
make node-test        # from the repo root (needs the shared libs + Node + koffi)
make docker-node      # or in an isolated container (koffi preinstalled)
```

The reusable module is [`asmtest.js`](asmtest.js) — it keeps all koffi FFI
inside and exposes the `Regs` / `Emu` / `EmuResult` classes plus the Tier-2
`assert*` helpers. [`conformance.js`](conformance.js) is a thin consumer that
`require`s it and replays the corpus. `make node-test` builds `libasmtest_emu` +
the routine fixture lib, then runs `conformance.js` with `ASMTEST_LIB` /
`ASMTEST_CORPUS_LIB` set. Install koffi locally with `npm install` (see
[package.json](package.json)).

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

## Deferred

A published npm package with prebuilt binaries (and `vitest`/`jest` integration
of the `assert*` helpers) is future work; the reusable module with Tier-2
assertions ships today.
