# asm-test — Go binding

Run, **capture**, and **emulate** assembly routines through the
[asm-test](https://github.com/wilvk/asm-test) framework from **Go**, via `cgo`.

`cgo` declares the opaque-handle FFI helpers (`src/ffi.c` + `emu.c`) and links the
prebuilt shared libraries, so no C struct layout is mirrored in Go:
`asmtest_corpus_routine` for addresses, `asmtest_capture6` / `_fp2` +
`asmtest_regs_*` for capture (with `asmtest_check_abi` as the ABI-preservation
verdict), and `asmtest_emu_call2` + accessors for the emulator (faults as data).
The result handles are all C-allocated, so they are exempt from cgo's
pointer-passing rules.

Tier-2 idiomatic assertions (`AssertRet`, `AssertABIPreserved`, `AssertFlag`,
`AssertFP`, `AssertNoFault`, `AssertEmuReg`) take a small `TB` interface that
`*testing.T` satisfies, so they fail a test with a legible message — and are
themselves testable (the suite proves each one bites on bad input).

## Run

```sh
make go-test          # from the repo root (needs the shared libs + the Go toolchain)
make docker-go        # or in an isolated container
```

`make go-test` builds `libasmtest_emu` + the routine fixture lib, then runs
[`conformance_test.go`](conformance_test.go) (the corpus replayed via `go test`)
with `CGO_LDFLAGS` and `LD_LIBRARY_PATH` / `DYLD_LIBRARY_PATH` pointing at the
build directory. The emulator case uses the x86-64 guest, so run it on an x86-64
target (e.g. `make docker-go DOCKER_PLATFORM=linux/amd64`).

## Deferred

A published Go module (with the native libs bundled per platform) is future work;
this is the Tier-1 + Tier-2 binding that proves the `cgo` path.
