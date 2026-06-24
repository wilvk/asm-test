# asm-test — Node.js binding

Run, **capture**, and **emulate** assembly routines through the
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

`make node-test` builds `libasmtest_emu` + the routine fixture lib, then runs
[`conformance.js`](conformance.js) (the corpus replayed in Node) with
`ASMTEST_LIB` / `ASMTEST_CORPUS_LIB` set. Install koffi locally with
`npm install` (see [package.json](package.json)).

## Deferred

An npm package with prebuilt binaries and a `vitest`/`jest` Tier-2 assertion
layer are future work; this is the Tier-1 binding that proves the koffi path.
