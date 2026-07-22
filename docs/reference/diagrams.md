# Diagrams

This page collects every architecture and flow diagram used across the docs in one
place. Each diagram links back to the page it illustrates, and each of those pages
links here.

## Framework build and run pipeline

How the assembly routines, C tests, build, and runner fit together — from
[asm-test](../index.md).

```mermaid
flowchart TB
    subgraph Author["You write"]
        ASM["Assembly routines<br/>examples/foo.s · foo.asm"]
        CT["C test cases<br/>TEST(...) + ASSERT_*"]
    end
    subgraph Build["Build (Makefile)"]
        AS["Assemble .s via cc (GAS)<br/>or nasm (Intel syntax)"]
        CC["Compile C tests +<br/>libasmtest runtime"]
        LD["Link one binary per suite<br/>build/test_foo"]
    end
    subgraph Run["Run the suite binary"]
        MAIN["Framework main()<br/>discover + filter + order tests"]
        ENG{"Execution engine"}
        NAT["Native tier — capture trampoline<br/>real CPU, real ABI"]
        EMU["Emulator tier (optional)<br/>Unicorn virtual CPU"]
        REP["Assert on return / registers /<br/>flags / memory / faults"]
        OUT["Colored TAP or JUnit XML<br/>+ nonzero exit on any failure"]
    end
    ASM --> AS
    CT --> CC
    AS --> LD
    CC --> LD
    LD --> MAIN --> ENG
    ENG -->|"ASM_CALLn"| NAT
    ENG -->|"emu_call"| EMU
    NAT --> REP
    EMU --> REP
    REP --> OUT
```

## Test lifecycle states

The states each test moves through in the runner — from [Writing tests](../getting-started/writing-tests.md).

```mermaid
stateDiagram-v2
    [*] --> Registered
    Registered --> Selected : main() filters / shuffles the registry
    Selected --> Setup : run next test (forked child by default)
    Setup --> Body : SETUP(suite)
    Body --> Passed : all assertions held
    Body --> Failed : ASSERT_* failed, siglongjmp
    Body --> Skipped : SKIP(reason), siglongjmp
    Body --> Crashed : SIGSEGV / SIGBUS or alarm() timeout
    Passed --> Teardown
    Failed --> Teardown
    Skipped --> Teardown
    Crashed --> Teardown : parent rebuilds verdict from wait() status
    Teardown --> Report : TEARDOWN(suite)
    Report --> Selected : more tests remain
    Report --> Summary : registry exhausted
    Summary --> [*] : exit nonzero if any test failed
```

## Runner fork-per-test lifecycle

How the parent runner and each forked child exchange a verdict — from
[The test runner](../guides/runner.md).

```mermaid
sequenceDiagram
    participant R as Runner (parent)
    participant C as Forked child
    participant T as Routine under test
    R->>C: fork() — one child per test
    Note over C: arm alarm(timeout) +<br/>SIGSEGV / SIGBUS handler
    C->>T: run test body (real ABI call)
    alt passes / fails / skips
        T-->>C: verdict via siglongjmp
        C->>R: write PASS/FAIL/SKIP + msg + location over pipe
    else infinite loop
        Note over C: alarm fires, SIGALRM
        C->>R: write "timed out"
    else hard crash (SIGABRT-class)
        Note over C: child dies before writing
        R->>R: synthesize result from wait() status
    end
    R->>R: reap_child(), record in registration order
    Note over R: -jN keeps N children in flight,<br/>report order stays deterministic
```

## Capture trampoline

How `ASM_CALLn` runs a routine through the real ABI and snapshots register state —
from [ABI capture & registers](../guides/abi-capture.md).

```mermaid
flowchart LR
    M["ASM_CALL2(r, fn, 2, 3)"] --> T
    subgraph T["capture trampoline — capture.s"]
        direction TB
        S1["Seed callee-saved regs<br/>with sentinels (0x1111…, 0x2222…)"]
        S2["Marshal args into ABI arg registers<br/>rdi,rsi,… / x0,x1,…"]
        S3["Real CALL into the routine"]
        S4["Snapshot all GP regs + RFLAGS<br/>(+ FP / vector file for _fp / _vec)"]
        S1 --> S2 --> S3 --> S4
    end
    T --> R["regs_t r<br/>ret · flags · callee-saved · fret · vec[]"]
    R --> A1["ASSERT_EQ(r.ret, 5)"]
    R --> A2["ASSERT_ABI_PRESERVED(r)"]
    R --> A3["ASSERT_FLAG_CLEAR(r, CF)"]
```

## Register snapshot layouts across ABIs

The `regs_t` shape under each calling convention — from
[ABI capture & registers](../guides/abi-capture.md).

```mermaid
flowchart LR
    subgraph SV["regs_t — x86-64 System V"]
        SVf["ret = rax (return value)<br/>rdx = second return register<br/>rbx, rbp, r12–r15 = callee-saved<br/>flags = RFLAGS (CF/PF/ZF/SF/OF)<br/>fret = xmm0 (FP return)<br/>vec[16] = xmm0–xmm15"]
    end
    subgraph W64["regs_t — x86-64 Win64"]
        W64f["ret, rdx = return registers<br/>rbx, rbp, r12–r15 = callee-saved<br/>rdi, rsi = callee-saved (Win64 only)<br/>flags = RFLAGS<br/>fret = xmm0<br/>vec[16] = xmm6–15 also callee-saved"]
    end
    subgraph A64["regs_t — AArch64 AAPCS64"]
        A64f["ret = x0 (return value)<br/>x19–x28, x29 = callee-saved<br/>flags = NZCV<br/>fret = d0 (FP return)<br/>vec[32] = v0–v31"]
    end
```

## Assertion families

The six families of `ASSERT_*` macros — from [Assertions](../guides/assertions.md).

```mermaid
flowchart LR
    A["ASSERT_* families"] --> V["Value"]
    A --> MEM["Memory / string"]
    A --> REG["Register / flags / ABI"]
    A --> FPV["Floating-point / SIMD"]
    A --> PROP["Differential / property"]
    A --> EMU["Emulator"]
    V --> V1["ASSERT_TRUE / FALSE"]
    V --> V2["ASSERT_EQ/NE/LT/LE/GT/GE (signed)"]
    V --> V3["ASSERT_UEQ/UNE/ULT/ULE/UGT/UGE (unsigned hex)"]
    MEM --> M1["ASSERT_STREQ"]
    MEM --> M2["ASSERT_MEM_EQ (hexdump diff)"]
    REG --> R1["ASSERT_ABI_PRESERVED"]
    REG --> R2["ASSERT_FLAG_SET / CLEAR"]
    REG --> R3["ASSERT_REG_EQ"]
    FPV --> F1["ASSERT_FP_EQ / NEAR (ULP-aware)"]
    FPV --> F2["ASSERT_VEC_EQ + lane asserts<br/>ASSERT_DEQ/DNEAR/FEQ/FNEAR"]
    PROP --> P1["ASSERT_MATCHES_REF1 / 2 / 3"]
    EMU --> E1["ASSERT_NO_FAULT / FAULT / FAULT_AT"]
    EMU --> E2["ASSERT_EMU_REG_EQ / FP_EQ / VEC_EQ"]
    EMU --> E3["ASSERT_BLOCK_COVERED /<br/>ASSERT_BLOCKS_AT_LEAST"]
```

## Property and differential testing loop

The generate → call → compare-against-reference loop — from
[Property / differential testing](../guides/property-testing.md).

```mermaid
flowchart TB
    SEED["seed — fixed by default,<br/>or ASMTEST_SEED"] --> RNG["splitmix64 RNG"]
    RNG --> GEN["gen(rng, args, cap)<br/>build one input tuple"]
    GEN --> CALL["call routine via real ABI<br/>(asm_call_capture_args)"]
    GEN --> REF["C reference model: ref(…)"]
    CALL --> CMP{"results equal?"}
    REF --> CMP
    CMP -->|"equal (silent)"| NEXT{"tried n inputs?"}
    NEXT -->|"no"| GEN
    NEXT -->|"yes"| PASS["test passes"]
    CMP -->|"mismatch"| FAIL["report input + both results + seed,<br/>then fail the test"]
```

## Emulator guests

The five emulator guests and the shared `*_open` → `*_call` → result shape — from
[Emulator tier](../guides/emulator.md).

```mermaid
flowchart TB
    G1["x86-64 System V"]
    G2["x86-64 Win64 ABI"]
    G3["AArch64 + NEON"]
    G4["RISC-V RV64"]
    G5["ARM32 A32 + NEON"]
    G1 & G2 & G3 & G4 & G5 --> OPEN["*_open(): map internal code + stack regions"]
    OPEN --> MAP["*_map / *_write:<br/>preload arbitrary guest memory"]
    MAP --> LOAD["Load routine bytes<br/>x86-64: copy from the built fn<br/>others: raw machine code"]
    LOAD --> ARGS["Marshal args into guest ABI registers<br/>scalar / _fp / _vec variants"]
    ARGS --> RUN["Run to the routine's ret<br/>OR stop after max_insns (mid-routine)"]
    RUN --> RES["emu_result_t:<br/>full register file + rip/flags + xmm/vec,<br/>faulted · fault_addr · fault_kind"]
    RUN -.->|"while running"| HOOKS["Hooks:<br/>invalid memory access becomes a fault,<br/>CODE/BLOCK becomes trace + coverage"]
    HOOKS -.-> RES
```

## Emulator trace and coverage flow

How `emu_call_traced` accumulates coverage and feeds the reporting/lcov helpers —
from [Emulator tier](../guides/emulator.md).

```mermaid
flowchart TB
    IN["inputs: -5, 0, +7"] --> CALL["emu_call_traced(…, tr)"]
    CALL --> TR["emu_trace_t tr (APPENDS)<br/>insns[] ordered trace<br/>blocks[] distinct block offsets"]
    TR -->|"re-run unions coverage"| CALL
    TR --> Q{"every branch hit?"}
    Q --> A1["ASSERT_BLOCK_COVERED /<br/>ASSERT_BLOCKS_AT_LEAST"]
    Q --> A2["emu_trace_report /<br/>emu_coverage_uncovered"]
    Q --> A3["emu_trace_lcov<br/>(offset-level .info)"]
    MAP["line map: (offset, line) rows<br/>from objdump / DWARF, out-of-band"] --> SRC
    TR --> SRC["emu_trace_source_report /<br/>emu_trace_lcov_source"]
    SRC --> OUT["source-line coverage + lcov"]
```

## Trace and coverage backends

Every trace backend — emulator, native DBI, and hardware — fills the **same**
`asmtest_trace_t` sink, so a test switches backends without changing how it reads
coverage — from [Native runtime tracing](../guides/tracing/native-tracing.md),
[Execution traces](../guides/tracing/traces.md), and
[Hardware tracing](../guides/tracing/hardware-tracing.md).

```mermaid
flowchart TB
    RT["Routine under test<br/>(offsets from entry, 0 = first byte)"]
    subgraph Emu["Emulator tier — any host"]
        UC["Unicorn virtual CPU<br/>CODE/BLOCK hooks"]
    end
    subgraph DBI["Native software DBI"]
        DR["DynamoRIO client<br/>bb event, in-process<br/>Linux x86-64"]
    end
    subgraph HW["Native hardware / single-step — asmtest_hwtrace.h"]
        PT["Intel PT → libipt<br/>bare-metal Intel x86-64"]
        AMD["AMD LBR → built-in<br/>bare-metal Zen 4+"]
        CS["ARM CoreSight → OpenCSD<br/>AArch64 boards (scaffold)"]
        SS["Single-step EFLAGS.TF → #DB<br/>any x86-64 Linux (exact)"]
    end
    RT --> UC & DR & PT & AMD & CS & SS
    UC & DR & PT & AMD & CS & SS -->|"trace_append_insn /<br/>trace_append_block (dedup)"| SINK
    SINK["asmtest_trace_t (shared sink)<br/>insns[] ordered · blocks[] distinct<br/>insns_total · blocks_total · truncated"]
    SINK --> COV["Coverage helpers<br/>ASSERT_BLOCK_COVERED · _report · _lcov"]
    SINK --> ANN["Capstone annotation layer<br/>offsets → instruction text"]
    SINK -.->|"ptrace call descent:<br/>flat trace stays frame 0"| DESC["asmtest_descent_t<br/>edges + nested callee frames<br/>(separate opaque handle)"]
```

## Portability across targets

How one source set reaches every target natively and via the emulator guests — from
[Portability](portability.md).

```mermaid
flowchart TB
    SRC["One source set<br/>foo.s (GAS) + foo.asm (NASM)<br/>ASM_FUNC abstracts ELF vs Mach-O"]
    SRC --> NATIVE["Native build — host architecture"]
    SRC --> EMUG["Emulator guests — any host"]
    NATIVE --> X86["x86-64"]
    NATIVE --> ARM["AArch64"]
    X86 --> X86OS["Linux · macOS"]
    ARM --> ARMOS["Linux · macOS (Apple Silicon)"]
    X86OS --> BK["Backend: GAS (default) or NASM"]
    ARMOS --> BKA["Backend: GAS only (NASM is x86-only)"]
    EMUG --> EG["x86-64 SysV · x86-64 Win64 ·<br/>AArch64 · RISC-V RV64 · ARM32"]
```

## Language bindings architecture

The C core, the flat binding ABI, and the per-language modules that reproduce a
shared conformance corpus — from [Language bindings](../bindings/index.md).

```mermaid
flowchart TB
    subgraph Native["C core (this repo)"]
        H["Headers: asmtest.h / asmtest_emu.h<br/>regs_t + emu result structs<br/>_Static_assert layout guards"]
        LIB["libasmtest_emu (shared)<br/>binding ABI: capture entry points,<br/>verdict shims, opaque-handle accessors"]
        GEN["gen-manifest.c"]
        JSON["asmtest_abi.json<br/>struct sizes + field offsets"]
        H --> LIB
        H --> GEN --> JSON
    end
    subgraph Modules["Per-language modules — FFI kept inside"]
        PY["Python (ctypes)"]
        OTHERS["Go cgo · Rust · C++ · Zig · Node koffi ·<br/>Ruby Fiddle · Lua ffi · Java FFM · .NET P/Invoke"]
    end
    JSON -->|"struct layout"| PY
    LIB --> PY
    LIB -->|"opaque-handle accessors"| OTHERS
    PY --> CORP
    OTHERS --> CORP
    CORP["Conformance corpus<br/>canonical routines + expected captures<br/>SINGLE SOURCE OF TRUTH"] --> V{"every binding reproduces<br/>the same results?"}
```
