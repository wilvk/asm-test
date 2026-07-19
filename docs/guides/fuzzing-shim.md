# External-engine fuzzing shim (libFuzzer / AFL++)

The [emulator tier](emulator.md) already records basic-block coverage and drives
its own in-tree [coverage-guided loop and mutation
tester](emulator.md#coverage-guided-fuzzing--mutation-testing) (`emu_fuzz_cover1`
/ `emu_mutation_test1`). This guide is for when you want to drive a guest routine
with an **external industrial fuzzer** instead — libFuzzer or AFL++ — so you get
its mature corpus management, dictionaries, and crash minimization, while the
feedback signal is still the emulator's block coverage.

## When to use which

| | In-tree loop (`emu_fuzz_cover1`) | External shim (this guide) |
|---|---|---|
| Dependency | none (just libunicorn) | clang **or** afl++ |
| Corpus / dictionaries / minimization | basic | the engine's full machinery |
| Setup | one function call | build a harness, run the engine |
| Best for | a quick coverage sweep inside a test | a sustained campaign against one routine |

If the in-tree loop already answers your question, keep it — it adds no
dependency. Reach for the shim when you want a real fuzzing campaign.

## The mechanism: coverage without instrumenting the guest

The crux, and the whole reason this is a distinct technique: **the guest is raw
machine code executed under Unicorn.** `clang -fsanitize=fuzzer` and
`afl-clang-fast` never see the guest bytes, so their automatic instrumentation
covers only the harness loop — not the routine under test.

So the harness registers an **external** coverage map and writes the emulator's
executed block offsets into it by hand, exactly as `afl-qemu-trace` /
Unicorn-mode / FRIDA-mode do for binary-only targets (and as Jazzer feeds a
non-C runtime's coverage). Every harness bumps that map through one tested seam:

```c
/* asmtest_emu.h — run one input, get the DISTINCT executed block offsets */
size_t emu_cover_hits(emu_t *e, const void *code, size_t code_len,
                      const long *args, int nargs, uint64_t max_insns,
                      uint64_t *block_offs, size_t cap);
```

Every recorded offset is `< code_len` (offsets are measured from routine entry),
so the harness sizes its counter array to `code_len` and indexes it directly by
offset — no hash needed. The guest is x86-64, like the rest of the fuzz tier.

## libFuzzer

`examples/fuzz_libfuzzer.c` embeds the guest and registers an external 8-bit
counter array via SanitizerCoverage, which libFuzzer then consumes as if it were
compiler-generated:

```c
extern void __sanitizer_cov_8bit_counters_init(uint8_t *start, uint8_t *stop);
extern void __sanitizer_cov_pcs_init(const uintptr_t *beg, const uintptr_t *end);
/* ... register g_counters (one slot per block offset) in LLVMFuzzerInitialize,
   then in LLVMFuzzerTestOneInput: run the guest through emu_cover_hits and
   g_counters[offset]++ for each executed block. */
```

`clang -fsanitize=fuzzer` links libFuzzer's own `main()` and its
SanitizerCoverage runtime. Two details matter in practice:

- The harness links the emulator objects **minus** any object that defines
  `main` (the framework's `asmtest.o` carries the test-runner `main`, which
  would clash with libFuzzer's); the build uses a `-DASMTEST_NO_MAIN` object
  instead.
- clang's libFuzzer cross-checks that every registered 8-bit-counter region has
  a matching **PC table** of equal length. Registering counters alone aborts at
  startup with *"The size of coverage PC tables does not match"*, so the harness
  also registers a synthetic table via `__sanitizer_cov_pcs_init`.

Build and run:

```sh
make fuzz-libfuzzer
./build/fuzz_libfuzzer -runs=50000 -max_len=8      # cov: climbs as paths are found
```

## AFL++

AFL++ writes coverage through a shared-memory bitmap (`__afl_area_ptr`), not
SanitizerCoverage — this AFL++ ships **no** `__sanitizer_cov_*` runtime — so the
guest coverage is written into that bitmap by hand. There are two harnesses.

**Native persistent-mode forkserver** (`examples/fuzz_afl.c`) uses AFL's own
macros — `__AFL_INIT()` starts the forkserver after the expensive `emu_open()`,
`__AFL_LOOP(N)` reuses the process across testcases — and bumps the map for each
executed guest block:

```sh
make fuzz-afl
mkdir -p seeds && printf '\x05\x00\x00\x00' > seeds/a
afl-fuzz -i seeds -o out -- ./build/fuzz_afl
```

**aflpp_driver reuse** (`fuzz-afl-driver`) drives the *same* libFuzzer harness
under AFL++: `afl-clang-fast -fsanitize=fuzzer` links `libAFLDriver.a` (its own
`main` + forkserver loop) in place of libFuzzer, and a `-DFUZZ_AFL_DRIVER` build
mode swaps the harness's coverage sink from SanitizerCoverage counters to AFL's
map.

```sh
make fuzz-afl-driver
afl-fuzz -i seeds -o out -- ./build/fuzz_afl_driver
```

```{note}
The map write lives in a **plain-compiled** helper
(`examples/fuzz_afl_map.c`), not in the `afl-clang-fast`-compiled harness: the
AFL instrumentation pass rewrites an in-harness `__afl_area_ptr` reference to an
undefined module-local `__afl_area_ptr.2`, so a direct map write from
instrumented code fails to link. Keep the write in an uninstrumented TU.
```

## The Docker lane

You do not need clang or afl++ on your host — `make docker-fuzz` builds an image
carrying both (`Dockerfile.fuzz`, on the bindings base) and runs
`make fuzz-shim-test`, which builds every harness and **fails unless each engine
finds a planted crash** (a routine that faults only on the negative path). It is
a real test, never a self-skip.

```sh
make docker-fuzz
```

## Scope

- **Node vs edge coverage.** The shim exports **node** (per-block) coverage,
  matching the libFuzzer 8-bit-counter model. AFL's native **edge** coverage
  (`map[cur ^ prev]`) needs the *ordered* block sequence, which the deduped
  block set does not carry; edge coverage is out of scope.
- **x86-64 guest only**, like the in-tree loop.
- libFuzzer ships with LLVM (Apache-2.0 with the LLVM exception) and AFL++ is
  Apache-2.0; both are build/test tooling here and are never bundled into a
  user-facing package.
