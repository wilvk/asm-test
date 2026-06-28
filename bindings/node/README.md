# asm-test — Node.js binding

Run, **capture**, **emulate**, and **assemble** assembly routines through the
[asm-test](https://github.com/wilvk/asm-test) framework from Node.js, via
[`koffi`](https://koffi.dev) (prebuilt FFI, no node-gyp build).

It calls the opaque-handle FFI layer (`src/ffi.c`), so no C struct layout is
mirrored in JS: `asmtest_corpus_routine` for routine addresses, `asmtest_capture6`
/ `_fp2` + `asmtest_regs_*` accessors for capture, and `asmtest_emu_call2` +
accessors for the emulator (faults as data: `faulted()`, plus `faultAddr()` /
`faultKind()` — against the `FaultKind` export — for where and why one hit).

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

## Native tracing (DynamoRIO, optional)

[`drtrace.js`](drtrace.js) is the Node counterpart to the Python
`asmtest.drtrace`: in-process native runtime tracing backed by DynamoRIO. Where
the emulator tier traces isolated guest bytes, `NativeTrace` traces host-native
code as it runs inside this Node process — materialize machine code with
`NativeCode.fromBytes`, mark a region, call into it, and read back basic-block
coverage / the instruction stream. It loads `libasmtest_drapp` (via the existing
`koffi` dependency) from `ASMTEST_DRAPP_LIB` (else `../../build/`), and invokes
the generated code through a koffi function-pointer prototype.

The tier is Linux-x86-64-only and requires a DynamoRIO build, so
`NativeTrace.available()` self-skips cleanly when the lib can't load or
`libdynamorio` is absent. [`test_drtrace.js`](test_drtrace.js) is a standalone
runner (`node test_drtrace.js`) that prints `SKIP: ...` and exits 0 unless the
tier is built and DynamoRIO is resolvable (`ASMTEST_DRAPP_LIB` +
`ASMTEST_DRCLIENT`).

## Deferred

A published npm package with prebuilt binaries (and `vitest`/`jest` integration
of the `assert*` helpers) is future work; the reusable module with Tier-2
assertions ships today.
